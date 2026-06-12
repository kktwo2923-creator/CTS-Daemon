#pragma once

// StatusReporter — 每秒采样功耗/CPU频率/GPU/电量写 status.json，维护 daemon.alive 心跳
// 功耗：自动判单/双芯电压量级（<6V 则双芯×2）；policy 动态枚举；全程 sysfs 读取无 popen

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

class StatusReporter {
private:
    static constexpr const char* CTS_DIR    = "/sdcard/Android/CTS";
    static constexpr const char* STATUS     = "/sdcard/Android/CTS/status.json";
    static constexpr const char* ALIVE      = "/sdcard/Android/CTS/daemon.alive";
    static constexpr const char* CPUFREQ    = "/sys/devices/system/cpu/cpufreq";
    static constexpr const char* BATT       = "/sys/class/power_supply/battery";

    std::atomic<bool> running_{true};
    std::thread th_;
    int cellCount_;             // 1 单芯 / 2 双芯串联
    std::vector<int> policies_; // 动态发现的 policy 列表

    static long readLong(const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return LONG_MIN;
        char buf[32] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return LONG_MIN;
        return atol(buf);
    }

    static std::string readStr(const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return "";
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return "";
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        return s;
    }

    static std::vector<int> discoverPolicies() {
        std::vector<int> v;
        DIR* d = opendir(CPUFREQ);
        if (!d) return v;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strncmp(e->d_name, "policy", 6) == 0) {
                int p = atoi(e->d_name + 6);
                v.push_back(p);
            }
        }
        closedir(d);
        std::sort(v.begin(), v.end());
        return v;
    }

    double computeWatt() const {
        long i = readLong(std::string(BATT) + "/current_now");
        long v = readLong(std::string(BATT) + "/voltage_now");
        if (i == LONG_MIN || v == LONG_MIN) return 0.0;
        long absI = labs(i), absV = labs(v);
        double currentMa = (absI > 100000) ? absI / 1000.0 : (double)absI;  // µA→mA
        double voltageMv = (absV > 100000) ? absV / 1000.0 : (double)absV;  // µV→mV
        double mult = (cellCount_ >= 2 && voltageMv < 6000.0) ? 2.0 : 1.0;
        return currentMa * voltageMv / 1000000.0 * mult;
    }

    bool isCharging() const {
        std::string s = readStr(std::string(BATT) + "/status");
        return s == "Charging" || s == "Full";
    }

    int gpuMhz() const {
        static const char* paths[] = {
            "/sys/class/kgsl/kgsl-3d0/gpuclk",
            "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
            "/sys/kernel/gpu/gpu_clock",
        };
        for (auto p : paths) {
            long raw = readLong(p);
            if (raw == LONG_MIN) continue;
            if (raw > 10000000) return (int)(raw / 1000000);
            if (raw > 10000)     return (int)(raw / 1000);
            return (int)raw;
        }
        return -1;
    }

    std::string buildJson() const {
        double watt = computeWatt();
        bool chg = isCharging();
        int batt = (int)readLong(std::string(BATT) + "/capacity");
        if (batt < 0) batt = -1;
        int gpu = gpuMhz();

        std::string j = "{";
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "\"ts\":%ld,", (long)time(nullptr));
        j += tmp;
        snprintf(tmp, sizeof(tmp), "\"watt\":%.3f,", watt);
        j += tmp;
        j += std::string("\"charging\":") + (chg ? "true" : "false") + ",";
        snprintf(tmp, sizeof(tmp), "\"batt_pct\":%d,", batt);
        j += tmp;
        snprintf(tmp, sizeof(tmp), "\"gpu_mhz\":%d,", gpu);
        j += tmp;

        j += "\"clusters\":[";
        for (size_t k = 0; k < policies_.size(); ++k) {
            int p = policies_[k];
            long curK = readLong(std::string(CPUFREQ) + "/policy" + std::to_string(p) + "/scaling_cur_freq");
            int curMhz = (curK > 0) ? (int)(curK / 1000) : -1;
            std::string gov = readStr(std::string(CPUFREQ) + "/policy" + std::to_string(p) + "/scaling_governor");
            if (k) j += ",";
            snprintf(tmp, sizeof(tmp), "{\"policy\":%d,\"cur_mhz\":%d,\"gov\":\"", p, curMhz);
            j += tmp;
            j += gov;
            j += "\"}";
        }
        j += "]}";
        return j;
    }

    static void atomicWrite(const char* path, const std::string& content) {
        // 写临时文件再 rename，防止 App 读到半截 JSON
        std::string tmp = std::string(path) + ".tmp";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        ssize_t n = write(fd, content.data(), content.size());
        (void)n;
        close(fd);
        rename(tmp.c_str(), path);
    }

    void loop() {
        mkdir(CTS_DIR, 0755);
        policies_ = discoverPolicies();
        while (running_.load(std::memory_order_relaxed)) {
            char hb[32];
            snprintf(hb, sizeof(hb), "%ld\n", (long)time(nullptr));
            atomicWrite(ALIVE, hb);
            atomicWrite(STATUS, buildJson());
            for (int s = 0; s < 10 && running_.load(std::memory_order_relaxed); ++s)
                usleep(100 * 1000);
        }
    }

public:
    explicit StatusReporter(int cellCount = 2) : cellCount_(cellCount) {}

    void start() {
        th_ = std::thread(&StatusReporter::loop, this);
    }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (th_.joinable()) th_.join();
        unlink(ALIVE);
    }

    ~StatusReporter() { stop(); }
};
