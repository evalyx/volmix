#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>
#include <mutex>
#include <fstream>
#include <iomanip>
#include <sys/stat.h>
#include <poll.h>
#include <cstdlib>

const char* SERIAL_PORT = "/dev/ttyUSB0";
const int BAUD_RATE = B115200;
const int THRESHOLD = 8;

struct FaderConfig {
    std::string lastKnownId;  // The ID currently being used for wpctl
    std::string resolvedName; // The persistent name (e.g., "Spotify")
};

// Global State
std::mutex dataMutex;
std::map<int, std::multimap<int, FaderConfig>> layeredMapping;
std::vector<int> currentPercents(9, 0); 
int activeLayer = 0;
bool isSerialAlive = false;

// --- PIPEWIRE DYNAMIC RESOLVER ---

// Searches wpctl status for a name associated with an ID (or vice versa)
std::string getPwInfo(std::string searchFor, bool findName) {
    FILE* pipe = popen("wpctl status", "r");
    if (!pipe) return "";

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        std::string line(buffer);
        size_t dotPos = line.find('.');
        // Look for lines starting with "  ID. Name"
        if (dotPos != std::string::npos && dotPos < 10) {
            std::string id = line.substr(0, dotPos);
            id.erase(0, id.find_first_not_of(" \t"));
            id.erase(id.find_last_not_of(" \t") + 1);

            if (findName && id == searchFor) {
                size_t start = dotPos + 1;
                size_t end = line.find('['); // End at volume info
                if (end == std::string::npos) end = line.length();
                std::string name = line.substr(start, end - start);
                name.erase(0, name.find_first_not_of(" \t"));
                name.erase(name.find_last_not_of(" \t") + 1);
                pclose(pipe);
                return name;
            } 
            else if (!findName && line.find(searchFor) != std::string::npos) {
                pclose(pipe);
                return id;
            }
        }
    }
    pclose(pipe);
    return "";
}

// Re-scans all apps to see if their IDs have changed
void refreshDynamicIds() {
    std::lock_guard<std::mutex> lock(dataMutex);
    for (auto& [layer, faders] : layeredMapping) {
        for (auto& [faderIdx, cfg] : faders) {
            if (!cfg.resolvedName.empty()) {
                std::string newId = getPwInfo(cfg.resolvedName, false);
                if (!newId.empty() && newId != cfg.lastKnownId) {
                    std::cout << "[DEBUG] App '" << cfg.resolvedName << "' moved: " 
                              << cfg.lastKnownId << " -> " << newId << std::endl;
                    cfg.lastKnownId = newId;
                }
            }
        }
    }
}

// --- UTILS ---

std::string getFullConfigPath() {
    const char* home = std::getenv("HOME");
    if (!home) return "./volmix.conf";
    std::string dir = std::string(home) + "/.config/volmix";
    system(("mkdir -p " + dir).c_str());
    return dir + "/volmix.conf";
}

void loadConfig(const std::string& path) {
    std::lock_guard<std::mutex> lock(dataMutex);
    layeredMapping.clear();
    std::ifstream f_in(path);
    if (!f_in.is_open()) return;

    int cl, cf; std::string cid, calias;
    while (f_in >> cl >> cf >> cid >> calias) {
        std::string appName = "";
        // If it's a number, try to "bind" it to the current app name
        if (cid != "@DEFAULT_AUDIO_SINK@" && std::all_of(cid.begin(), cid.end(), ::isdigit)) {
            appName = getPwInfo(cid, true);
        }
        layeredMapping[cl].insert({cf, {cid, appName}});
    }
    std::cout << "[INFO] Config loaded. " << layeredMapping.size() << " layers active." << std::endl;
}

void runWpctl(std::string targetId, int percent) {
    if (targetId.empty() || targetId == "---") return;
    double vol = percent / 100.0;
    std::stringstream cmd;
    cmd << "wpctl set-volume " << targetId << " " << std::fixed << std::setprecision(2) << vol << " && ";
    cmd << "wpctl set-mute " << targetId << " " << (percent == 0 ? "1" : "0");
    system((cmd.str() + " > /dev/null 2>&1 &").c_str());
}

// --- SERIAL THREAD ---

void serialThread() {
    while (true) {
        int fd = open(SERIAL_PORT, O_RDONLY | O_NOCTTY);
        if (fd < 0) { isSerialAlive = false; std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }

        struct termios tty;
        tcgetattr(fd, &tty);
        cfsetispeed(&tty, BAUD_RATE); cfsetospeed(&tty, BAUD_RATE);
        tty.c_cflag |= (CLOCAL | CREAD | CS8);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tcsetattr(fd, TCSANOW, &tty);

        isSerialAlive = true;
        std::vector<int> lastVals(10, -1);
        std::string buffer; char c;

        while (read(fd, &c, 1) > 0) {
            if (c == '\n') {
                if (buffer.find("DATA") != std::string::npos) {
                    std::vector<int> vals;
                    std::stringstream ss(buffer); std::string item;
                    while (std::getline(ss, item, ',')) {
                        if (item == "DATA") continue;
                        try { vals.push_back(std::stoi(item)); } catch (...) {}
                    }
                    if (vals.size() >= 9) {
                        activeLayer = vals[0];
                        // Physical Fader 1 (Master)
                        if (std::abs(vals[1] - lastVals[1]) > THRESHOLD) {
                            int pct = std::clamp((vals[1] * 100) / 1014, 0, 100);
                            currentPercents[1] = pct;
                            runWpctl("@DEFAULT_AUDIO_SINK@", pct);
                            lastVals[1] = vals[1];
                        }
                        // Physical Faders 2-8 (Mapped Apps)
                        for (int i = 1; i <= 7; i++) {
                            if (std::abs(vals[i+1] - lastVals[i+1]) > THRESHOLD) {
                                std::lock_guard<std::mutex> lock(dataMutex);
                                int pct = std::clamp((vals[i+1] * 100) / 1014, 0, 100);
                                currentPercents[i+1] = pct;
                                auto range = layeredMapping[activeLayer].equal_range(i);
                                for (auto it = range.first; it != range.second; ++it) {
                                    runWpctl(it->second.lastKnownId, pct);
                                }
                                lastVals[i+1] = vals[i+1];
                            }
                        }
                    }
                }
                buffer.clear();
            } else if (c != '\r') buffer += c;
        }
        close(fd);
    }
}

// --- MAIN LOOP ---

int main() {
    std::string configPath = getFullConfigPath();
    loadConfig(configPath);

    std::thread sThread(serialThread);
    sThread.detach();

    auto lastRefresh = std::chrono::steady_clock::now();
    auto lastEnforce = std::chrono::steady_clock::now();
    time_t lastMTime = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();

        // 1. Config Change Detection
        struct stat st;
        if (stat(configPath.c_str(), &st) == 0) {
            if (st.st_mtime > lastMTime) {
                loadConfig(configPath);
                lastMTime = st.st_mtime;
            }
        }

        // 2. Re-scan PipeWire IDs every 3 seconds (The Fix)
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRefresh).count() >= 3) {
            refreshDynamicIds();
            lastRefresh = now;
        }

        // 3. Volume Enforcement (Every 2 seconds)
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastEnforce).count() >= 2) {
            std::lock_guard<std::mutex> lock(dataMutex);
            runWpctl("@DEFAULT_AUDIO_SINK@", currentPercents[1]);
            for (auto const& [faderIdx, cfg] : layeredMapping[activeLayer]) {
                runWpctl(cfg.lastKnownId, currentPercents[faderIdx+1]);
            }
            lastEnforce = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return 0;
}