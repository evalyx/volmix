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

std::string GLOBAL_CONFIG_PATH = "";

struct FaderConfig {
    std::string id;
    std::string alias;
};

std::mutex uiMutex;
std::map<int, std::multimap<int, FaderConfig>> layeredMapping;
std::vector<int> currentPercents(9, 0);
int activeLayer = 0;
bool isSerialAlive = false;

// --- UTILS ---

std::string getConfigPath() {
    const char* home = std::getenv("HOME");
    std::string path = (home) ? std::string(home) + "/.config/volmix" : "./config";
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
    std::cout << "[INFO] Configuration loaded from " << GLOBAL_CONFIG_PATH << std::endl;
}

void saveConfig() {
    std::ofstream f_out(GLOBAL_CONFIG_PATH);
    for (auto const& [lay, faders] : layeredMapping) {
        for (auto const& [f, cfg] : faders) {
            f_out << lay << " " << f << " " << cfg.id << " " << cfg.alias << "\n";
        }
    }
}

void refreshUI() {
    // Only print visual bars if we are actually in a terminal
    if (!isatty(STDIN_FILENO)) return;
    
    std::lock_guard<std::mutex> lock(uiMutex);
    std::cout << "\033[s\033[1;1H\033[1;37;44m";
    std::cout << " L" << activeLayer << " | " << (isSerialAlive ? "LIVE" : "DEAD") << " | MST " << currentPercents[1] << "% ";
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
        int fd = open(SERIAL_PORT, O_RDONLY | O_NOCTTY);
        if (fd < 0) {
            isSerialAlive = false;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) { close(fd); continue; }
        cfsetispeed(&tty, BAUD_RATE); cfsetospeed(&tty, BAUD_RATE);
        tty.c_cflag |= (CLOCAL | CREAD | CS8);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tcsetattr(fd, TCSANOW, &tty);

        isSerialAlive = true;
        std::vector<int> lastVals(10, -1);
        std::string buffer;
        char c;

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
                        if (std::abs(vals[1] - lastVals[1]) > THRESHOLD) {
                            applyVolume("@DEFAULT_AUDIO_SINK@", vals[1], 1);
                            lastVals[1] = vals[1];
                        }
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
            } else if (c != '\r') buffer += c;
        }
        isSerialAlive = false;
        close(fd);
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

    std::cout << "[READY] VolMix Backend started. Using: " << GLOBAL_CONFIG_PATH << std::endl;

    while (true) {
        // Auto-reload if file changes
        time_t currentMTime = getFileMTime(GLOBAL_CONFIG_PATH);
        if (currentMTime > lastMTime) {
            loadConfig();
            lastMTime = currentMTime;
        }

        // Handle Terminal input safely (won't crash in systemd)
        int ret = poll(fds, 1, 100);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            std::string input;
            if (std::getline(std::cin, input)) {
                if (input == "exit") break;
                if (input == "ls") {
                    std::cout << "Config Path: " << GLOBAL_CONFIG_PATH << std::endl;
                }
                // (Additional CLI commands could go here)
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
