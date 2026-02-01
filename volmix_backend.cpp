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
#include <regex>

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

    char buffer[1024];
    std::string lastFoundId = "";
    std::string lastFoundName = "";
    
    // Regex to find: "  123. Name of Application   [vol: 0.50]"
    // This ensures we only pick objects that actually have volume controls.
    std::regex lineRegex(R"(^\s*(\d+)\.\s+(.*?)\s*(?:\[.*\])?)");

    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        std::string line(buffer);
        std::smatch match;
        
        if (std::regex_search(line, match, lineRegex)) {
            std::string id = match[1];
            std::string name = match[2];

            if (findName) {
                if (id == searchFor) {
                    pclose(pipe);
                    return name;
                }
            } else {
                std::string sLower = searchFor;
                std::string nLower = name;
                std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::tolower);
                std::transform(nLower.begin(), nLower.end(), nLower.begin(), ::tolower);
                
                if (nLower.find(sLower) != std::string::npos) {
                    // Update our candidate, but keep looking for better/later matches (streams)
                    lastFoundId = id;
                }
            }
        }
    }
    pclose(pipe);
    return findName ? "" : lastFoundId;
}

void refreshDynamicIds() {
    std::lock_guard<std::mutex> lock(dataMutex);
    for (auto& [layer, faders] : layeredMapping) {
        for (auto& [faderIdx, cfg] : faders) {
            if (!cfg.resolvedName.empty()) {
                std::string newId = getPwInfo(cfg.resolvedName, false);
                // Only update if we found a valid ID and it's not a "ghost" ID like 1 or 0
                if (!newId.empty() && newId.length() > 1 && newId != cfg.lastKnownId) {
                    std::cout << "[DEBUG] Resolved '" << cfg.resolvedName << "' -> ID: " << newId << std::endl;
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
        // Check if cid is numeric and not the default sink string
        bool isNumeric = !cid.empty() && std::all_of(cid.begin(), cid.end(), ::isdigit);
        
        if (cid != "@DEFAULT_AUDIO_SINK@" && isNumeric) {
            std::string foundName = getPwInfo(cid, true);
            if (!foundName.empty()) {
                resolvedName = foundName;
            }
        }
        layeredMapping[cl].insert({cf, {cid, resolvedName}});
    }
    std::cout << "[INFO] Config loaded from " << path << std::endl;
}

void runWpctl(std::string targetId, int percent) {
    if (targetId.empty() || targetId == "---" || targetId == "0") return;
    
    // Validate target (Must be @DEFAULT or a numeric ID > 10)
    if (targetId != "@DEFAULT_AUDIO_SINK@") {
        if (!std::all_of(targetId.begin(), targetId.end(), ::isdigit) || targetId.length() < 2) return;
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
                        // Fader 1
                        if (std::abs(vals[1] - lastVals[1]) > THRESHOLD) {
                            int pct = std::clamp((vals[1] * 100) / 1014, 0, 100);
                            currentPercents[1] = pct;
                            runWpctl("@DEFAULT_AUDIO_SINK@", pct);
                            lastVals[1] = vals[1];
                        }
                        // Faders 2-8
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

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastEnforce).count() >= 4) {
            std::lock_guard<std::mutex> lock(dataMutex);
            runWpctl("@DEFAULT_AUDIO_SINK@", currentPercents[1]);
            for (auto const& [faderIdx, cfg] : layeredMapping[activeLayer]) {
                runWpctl(cfg.lastKnownId, currentPercents[faderIdx+1]);
            }
            lastEnforce = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}