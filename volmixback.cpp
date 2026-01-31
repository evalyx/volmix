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

// Use a global string for the path determined at runtime
std::string GLOBAL_CONFIG_PATH = "";

struct FaderConfig {
    std::string id;
    std::string alias;
};

std::mutex uiMutex;
// Changed to multimap to allow multiple targets per fader
std::map<int, std::multimap<int, FaderConfig>> layeredMapping;
std::vector<int> currentPercents(9, 0);
std::vector<int> rawDebugVals(9, 0);
int activeLayer = 0;
bool isSerialAlive = false;

// --- UTILS ---

// Resolves /home/$USER/.config/volmix/volmix.conf
std::string getConfigPath() {
    const char* home = std::getenv("HOME");
    std::string path;
    if (home) {
        path = std::string(home) + "/.config/volmix";
    } else {
        path = "./config"; // Fallback
    }
    // Create directory if missing
    system(("mkdir -p " + path).c_str());
    return path + "/volmix.conf";
}

time_t getFileMTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_mtime;
}

void loadConfig() {
    std::lock_guard<std::mutex> lock(uiMutex);
    layeredMapping.clear();
    std::ifstream f_in(GLOBAL_CONFIG_PATH);
    int cl, cf; std::string cid, calias;
    while (f_in >> cl >> cf >> cid >> calias) {
        layeredMapping[cl].insert({cf, {cid, calias}});
    }
}

void saveConfig() {
    std::ofstream f_out(GLOBAL_CONFIG_PATH);
    for (auto const& [lay, faders] : layeredMapping) {
        for (auto const& [f, cfg] : faders) {
            f_out << lay << " " << f << " " << cfg.id << " " << cfg.alias << "\n";
        }
    }
}

// --- UI HELPERS ---
std::string getBar(int percent, std::string label) {
    int width = 8;
    int filled = (percent * width) / 100;
    std::string bar = label + " [";
    for (int i = 0; i < width; ++i) bar += (i < filled) ? "#" : " ";
    return (percent == 0) ? label + " [ MUTE ]" : bar + "]";
}

void refreshUI() {
    if (!isatty(STDIN_FILENO)) return;
    std::lock_guard<std::mutex> lock(uiMutex);
    std::cout << "\033[s\033[1;1H\033[1;37;44m";
    std::cout << " L" << activeLayer << " | " << (isSerialAlive ? "LIVE" : "DEAD") << " | ";
    std::cout << getBar(currentPercents[1], "MST") << " " << currentPercents[1] << "% | ";

    for(int i = 1; i <= 7; i++) {
        std::string label = "F" + std::to_string(i);
        if (layeredMapping[activeLayer].count(i)) {
            label = layeredMapping[activeLayer].find(i)->second.alias;
            if (layeredMapping[activeLayer].count(i) > 1) label += "+";
        }
        std::cout << getBar(currentPercents[i+1], label) << " ";
    }
    std::cout << "\033[0m\033[K\033[u" << std::flush;
}

// --- CORE LOGIC ---
void applyVolume(std::string targetId, int rawValue, int faderIdx) {
    int percent = std::clamp((rawValue * 100) / 1014, 0, 100);
    currentPercents[faderIdx] = percent;
    if (targetId.empty() || targetId == "---") return;

    double vol = percent / 100.0;
    std::stringstream cmd;
    cmd << "wpctl set-volume " << targetId << " " << std::fixed << std::setprecision(2) << vol << " && ";
    cmd << "wpctl set-mute " << targetId << " " << (percent == 0 ? "1" : "0");
    system((cmd.str() + " > /dev/null 2>&1 &").c_str());
}

void serialThread() {
    while (true) {
        // 1. Attempt to open the port
        int fd = open(SERIAL_PORT, O_RDONLY | O_NOCTTY);

        if (fd < 0) {
            isSerialAlive = false;
            refreshUI(); // Update UI to show "DEAD"
            // Wait before trying again to avoid CPU pegging
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // 2. Configure the port
        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) {
            close(fd);
            continue;
        }

        cfsetispeed(&tty, BAUD_RATE);
        cfsetospeed(&tty, BAUD_RATE);
        tty.c_cflag |= (CLOCAL | CREAD | CS8);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tcsetattr(fd, TCSANOW, &tty);

        isSerialAlive = true;
        refreshUI();

        std::vector<int> lastVals(10, -1);
        std::string buffer;
        char c;

        // 3. Read loop
        // read() will return <= 0 if the device is unplugged
        while (read(fd, &c, 1) > 0) {
            if (c == '\n') {
                if (buffer.find("DATA") != std::string::npos) {
                    std::vector<int> vals;
                    std::stringstream ss(buffer);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        if (item == "DATA") continue;
                        try { vals.push_back(std::stoi(item)); } catch (...) {}
                    }

                    if (vals.size() >= 9) {
                        activeLayer = vals[0];
                        for(int i = 1; i <= 8; i++) rawDebugVals[i] = vals[i];

                        // Master Fader
                        if (std::abs(vals[1] - lastVals[1]) > THRESHOLD) {
                            applyVolume("@DEFAULT_AUDIO_SINK@", vals[1], 1);
                            lastVals[1] = vals[1];
                        }

                        // Other Faders
                        for (int i = 1; i <= 7; i++) {
                            if (std::abs(vals[i+1] - lastVals[i+1]) > THRESHOLD) {
                                std::lock_guard<std::mutex> lock(uiMutex);
                                auto range = layeredMapping[activeLayer].equal_range(i);
                                for (auto it = range.first; it != range.second; ++it) {
                                    applyVolume(it->second.id, vals[i+1], i+1);
                                }
                                lastVals[i+1] = vals[i+1];
                            }
                        }
                        refreshUI();
                    }
                }
                buffer.clear();
            } else if (c != '\r') {
                buffer += c;
            }
        }

        // 4. Cleanup on disconnect
        isSerialAlive = false;
        close(fd);
        refreshUI();
        // Short pause before attempting to reconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main() {
    GLOBAL_CONFIG_PATH = getConfigPath();
    loadConfig();
    time_t lastMTime = getFileMTime(GLOBAL_CONFIG_PATH);

    std::thread sThread(serialThread);
    sThread.detach();

    struct pollfd fds[1];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    while (true) {
        time_t currentMTime = getFileMTime(GLOBAL_CONFIG_PATH);
        if (currentMTime > lastMTime) {
            loadConfig();
            lastMTime = currentMTime;
        }

        int ret = poll(fds, 1, 100);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            std::string input;
            if (!std::getline(std::cin, input) || input == "exit") break;

            if (input == "ls") {
                std::cout << "Config Path: " << GLOBAL_CONFIG_PATH << "\n";
                // Add your full interface display logic here if needed
            }
            else if (input.substr(0, 7) == "unbind ") {
                int ul, uf; char ud; std::stringstream ss(input.substr(7));
                if (ss >> ul >> ud >> uf) {
                    { std::lock_guard<std::mutex> lock(uiMutex); layeredMapping[ul].erase(uf); }
                    saveConfig();
                    lastMTime = getFileMTime(GLOBAL_CONFIG_PATH);
                }
            } else {
                int l, f; std::string id, name = "";
                std::string processed = input;
                std::replace(processed.begin(), processed.end(), '-', ' ');
                std::stringstream pss(processed);
                if (pss >> l >> f >> id) {
                    if (!(pss >> name)) name = "F" + std::to_string(f);
                    { std::lock_guard<std::mutex> lock(uiMutex);
                        layeredMapping[l].insert({f, {id, name}}); }
                        saveConfig();
                        lastMTime = getFileMTime(GLOBAL_CONFIG_PATH);
                }
            }
            std::cout << "Command: " << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return 0;
}
