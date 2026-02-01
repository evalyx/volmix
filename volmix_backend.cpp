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
    std::string lastKnownId;
    std::string resolvedName;
};

std::mutex dataMutex;
std::map<int, std::multimap<int, FaderConfig>> layeredMapping;
std::vector<int> currentPercents(9, 0); 
int activeLayer = 0;
bool isSerialAlive = false;

// --- PIPEWIRE DYNAMIC RESOLVER ---

std::string getPwInfo(std::string searchFor, bool findName) {
    FILE* pipe = popen("wpctl status", "r");
    if (!pipe) return "";

    char buffer[512];
    std::string result = "";

    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        std::string line(buffer);
        size_t dotPos = line.find('.');
        
        // IDs are always followed by a dot. We check a wider range to accommodate tree symbols.
        if (dotPos != std::string::npos && dotPos < 20) {
            std::string idPartRaw = line.substr(0, dotPos);
            std::string idPart = "";

            // STICKY FIX: Only keep numeric characters for the ID
            for (char c : idPartRaw) {
                if (std::isdigit(c)) idPart += c;
            }

            if (idPart.empty()) continue;

            size_t start = dotPos + 1;
            size_t end = line.find('[');
            if (end == std::string::npos) end = line.length();
            
            std::string namePart = line.substr(start, end - start);
            // Clean up name: remove tree symbols and whitespace
            namePart.erase(0, namePart.find_first_not_of(" \t│├─└")); 
            namePart.erase(namePart.find_last_not_of(" \t") + 1);

            if (findName) {
                if (idPart == searchFor) {
                    result = namePart;
                    break;
                }
            } else {
                std::string searchLower = searchFor;
                std::string nameLower = namePart;
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                
                if (nameLower.find(searchLower) != std::string::npos) {
                    result = idPart;
                    break;
                }
            }
        }
    }
    pclose(pipe);
    return result;
}

void refreshDynamicIds() {
    std::lock_guard<std::mutex> lock(dataMutex);
    for (auto& [layer, faders] : layeredMapping) {
        for (auto& [faderIdx, cfg] : faders) {
            if (!cfg.resolvedName.empty()) {
                std::string newId = getPwInfo(cfg.resolvedName, false);
                if (!newId.empty() && newId != cfg.lastKnownId) {
                    std::cout << "[DEBUG] Resolved '" << cfg.resolvedName << "' to ID: " << newId << std::endl;
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
        std::string resolvedName = calias;
        if (cid != "@DEFAULT_AUDIO_SINK@" && std::all_of(cid.begin(), cid.end(), ::isdigit)) {
            std::string foundName = getPwInfo(cid, true);
            if (!foundName.empty()) resolvedName = foundName;
        }
        layeredMapping[cl].insert({cf, {cid, resolvedName}});
    }
    std::cout << "[INFO] Config loaded. Layers active: " << layeredMapping.size() << std::endl;
}

void runWpctl(std::string targetId, int percent) {
    if (targetId.empty() || targetId == "---") return;
    
    // Safety check: ensure targetId is purely numeric or the @DEFAULT string
    if (targetId != "@DEFAULT_AUDIO_SINK@") {
        for (char c : targetId) {
            if (!std::isdigit(c)) return; 
        }
    }

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
        if (fd < 0) { 
            isSerialAlive = false; 
            std::this_thread::sleep_for(std::chrono::seconds(2)); 
            continue; 
        }

        struct termios tty;
        tcgetattr(fd, &tty);
        cfsetispeed(&tty, BAUD_RATE); 
        cfsetospeed(&tty, BAUD_RATE);
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
                        if (std::abs(vals[1] - lastVals[1]) > THRESHOLD) {
                            int pct = std::clamp((vals[1] * 100) / 1014, 0, 100);
                            currentPercents[1] = pct;
                            runWpctl("@DEFAULT_AUDIO_SINK@", pct);
                            lastVals[1] = vals[1];
                        }
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

        struct stat st;
        if (stat(configPath.c_str(), &st) == 0) {
            if (st.st_mtime > lastMTime) {
                loadConfig(configPath);
                lastMTime = st.st_mtime;
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRefresh).count() >= 3) {
            refreshDynamicIds();
            lastRefresh = now;
        }

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