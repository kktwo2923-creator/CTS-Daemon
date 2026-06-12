#pragma once

// StatusReporter — 每秒采样功耗/CPU频率/GPU/电量写 status.json，维护 daemon.alive 心跳
// 功耗：自动判单/双芯电压量级（<6V 则双芯×2）；policy 动态枚举；全程 sysfs 读取无 popen

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <condition_variable>
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
    std::mutex mtx_;
    std::condition_variable cv_;
    int cellCount_;             // 1 单芯 / 2 双芯串联
    std::vector<int> policies_; // 动态发现的 policy 列表
    std::vector<std::string> curFreqPaths_, govPaths_;  // 预生成,避免每秒重复拼串

    static long readLong(const char* path) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return LONG_MIN;
        char buf[32] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return LONG_MIN;
        return atol(buf);
    }

    static std::string readStr(const char* path) {
        int fd = open(path, O_RDONLY);
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
        long i = readLong("/sys/class/power_supply/battery/current_now");
        long v = readLong("/sys/class/power_supply/battery/voltage_now");
        if (i == LONG_MIN || v == LONG_MIN) return 0.0;
        long absI = labs(i), absV = labs(v);
        double currentMa = (absI > 100000) ? absI / 1000.0 : (double)absI;  // µA→mA
        double voltageMv = (absV > 100000) ? absV / 1000.0 : (double)absV;  // µV→mV
        double mult = (cellCount_ >= 2 && voltageMv < 6000.0) ? 2.0 : 1.0;
        return currentMa * voltageMv / 1000000.0 * mult;
    }

    bool isCharging() const {
        std::string s = readStr("/sys/class/power_supply/battery/status");
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

    // 栈缓冲拼 JSON，消除每秒 ~10 次堆分配
    int buildJson(char* out, size_t cap) const {
        double watt = computeWatt();
        bool chg = isCharging();
        int batt = (int)readLong("/sys/class/power_supply/battery/capacity");
        if (batt < 0) batt = -1;
        int gpu = gpuMhz();

        int len = snprintf(out, cap,
            "{\"ts\":%ld,\"watt\":%.3f,\"charging\":%s,\"batt_pct\":%d,\"gpu_mhz\":%d,\"clusters\":[",
            (long)time(nullptr), watt, chg ? "true" : "false", batt, gpu);

        for (size_t k = 0; k < policies_.size() && len < (int)cap - 1; ++k) {
            long curK = readLong(curFreqPaths_[k].c_str());
            int curMhz = (curK > 0) ? (int)(curK / 1000) : -1;
            std::string gov = readStr(govPaths_[k].c_str());
            len += snprintf(out + len, cap - len, "%s{\"policy\":%d,\"cur_mhz\":%d,\"gov\":\"%s\"}",
                            k ? "," : "", policies_[k], curMhz, gov.c_str());
        }
        if (len < (int)cap - 2) len += snprintf(out + len, cap - len, "]}");
        if (len > (int)cap - 1) len = (int)cap - 1;
        return len;
    }

    static void atomicWrite(const char* path, const char* data, size_t len) {
        // 写临时文件再 rename，防止 App 读到半截 JSON
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        ssize_t n = write(fd, data, len);
        (void)n;
        close(fd);
        rename(tmp, path);
    }

    void loop() {
        mkdir(CTS_DIR, 0755);
        policies_ = discoverPolicies();
        for (int p : policies_) {
            std::string base = std::string(CPUFREQ) + "/policy" + std::to_string(p);
            curFreqPaths_.push_back(base + "/scaling_cur_freq");
            govPaths_.push_back(base + "/scaling_governor");
        }
        char json[4096];
        std::unique_lock<std::mutex> lk(mtx_);
        while (running_.load(std::memory_order_relaxed)) {
            lk.unlock();
            char hb[32];
            int hlen = snprintf(hb, sizeof(hb), "%ld\n", (long)time(nullptr));
            atomicWrite(ALIVE, hb, hlen);
            int jlen = buildJson(json, sizeof(json));
            atomicWrite(STATUS, json, jlen);
            lk.lock();
            // 单次 1s 等待替代 10×100ms 唤醒；stop() 通过 cv 即刻唤醒
            cv_.wait_for(lk, std::chrono::seconds(1),
                         [this] { return !running_.load(std::memory_order_relaxed); });
        }
    }

public:
    explicit StatusReporter(int cellCount = 2) : cellCount_(cellCount) {}

    void start() {
        th_ = std::thread(&StatusReporter::loop, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            running_.store(false, std::memory_order_relaxed);
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        unlink(ALIVE);
    }

    ~StatusReporter() { stop(); }
};
