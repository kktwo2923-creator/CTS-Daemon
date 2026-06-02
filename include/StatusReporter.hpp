#pragma once

// ============================================================================
// StatusReporter —— daemon 侧实时状态采集(优化方案 §3)
//
// 职责:
//   1) 每秒采样整机功耗(W)、各 policy 当前频率(MHz)、GPU 频率、电量,
//      写出 /sdcard/Android/CTS/status.json,供 App 顶栏轻量读取显示。
//   2) 维护 /sdcard/Android/CTS/daemon.alive 心跳(秒级 epoch),
//      App 据此判断 daemon 是否存活(存活则 App 退为纯 GUI,不重复探测/切档)。
//
// 设计:
//   - 功耗三重处理:单位(µA/mA)、符号(取绝对值)、双电芯倍率。
//     双芯倍率默认按 voltage_now 量级自动判定:读到单芯量级(<6V)且双芯 → ×2;
//     已是整包电压(≥6V)→ ×1(不再翻倍)。是否双芯由 cellCount 决定(默认 2)。
//   - policy 不硬编码,从 /sys/.../cpufreq/ 动态枚举。
//   - 直接 open/read sysfs,不 popen,不依赖外部 shell。
//   - 单独一条采样线程,1s 周期;不动现有线程结构,改动最小。
// ============================================================================

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

    // ---- 基础读取 ----
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
        // trim 尾部空白
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        return s;
    }

    // 动态发现所有 policy 编号
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

    // 整机功耗(W),三重处理
    double computeWatt() const {
        long i = readLong(std::string(BATT) + "/current_now");
        long v = readLong(std::string(BATT) + "/voltage_now");
        if (i == LONG_MIN || v == LONG_MIN) return 0.0;
        long absI = labs(i), absV = labs(v);
        double currentMa = (absI > 100000) ? absI / 1000.0 : (double)absI;  // µA→mA 或本就 mA
        double voltageMv = (absV > 100000) ? absV / 1000.0 : (double)absV;  // µV→mV 或本就 mV
        double mult = (cellCount_ >= 2 && voltageMv < 6000.0) ? 2.0 : 1.0;  // 双芯倍率
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
            if (raw > 10000000) return (int)(raw / 1000000); // Hz→MHz
            if (raw > 10000)     return (int)(raw / 1000);    // kHz→MHz
            return (int)raw;                                   // 已是 MHz
        }
        return -1;
    }

    // 组 JSON(手写,避免引入依赖;字段与 App 的 LiveStatus.parse 对齐)
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
        // 写临时文件再 rename,避免 App 读到半截 JSON
        std::string tmp = std::string(path) + ".tmp";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        ssize_t n = write(fd, content.data(), content.size());
        (void)n;
        close(fd);
        rename(tmp.c_str(), path);
    }

    void loop() {
        // 确保目录存在
        mkdir(CTS_DIR, 0755);
        policies_ = discoverPolicies();
        while (running_.load(std::memory_order_relaxed)) {
            // 心跳
            char hb[32];
            snprintf(hb, sizeof(hb), "%ld\n", (long)time(nullptr));
            atomicWrite(ALIVE, hb);
            // 状态
            atomicWrite(STATUS, buildJson());
            // 1s 周期(分 10 段查 running_,便于快速退出)
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
        // 移除心跳文件:App 立即得知 daemon 已退出,无需等 15s 心跳过期才回退自读。
        unlink(ALIVE);
    }

    ~StatusReporter() { stop(); }
};
