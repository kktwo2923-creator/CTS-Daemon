// By Ktwo
#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <atomic>
#include <unordered_map>
#include "Json/string.hpp"

using string_t = qlib::string_t;
using bool_t   = qlib::bool_t;

using std::ifstream;
using std::unordered_map;

namespace Config {

    // 拓扑/容量常量：CPU 簇数、核数、单簇调速器参数容量（与白名单 commonParams 条数一致）
    inline constexpr int kClusterCount   = 4;
    inline constexpr int kCoreCount      = 8;
    inline constexpr int kMaxSchedParams = 16;

    namespace Meta {
        inline string_t name;
        inline int      version = -1;
        inline string_t author;
        inline string_t loglevel;
    }

    namespace Policy {
        inline int CpuPolicy[kClusterCount] = { -1, -1, -1, -1 };
    }

    namespace Cpuset {
        inline bool     enable = false;
        inline string_t top_app;
        inline string_t foreground;
        inline string_t background;
        inline string_t system_background;
        inline string_t restricted;
    }

    namespace LaunchBoost {
        inline bool     enable = false;
        inline int      boost_rate_limit_ms = 35;
        inline string_t BoostFreq[kClusterCount];
    }

    namespace OfficialMode {
        inline bool enable = false;
    }

    namespace Scheduler {
        inline bool     enable = false;
        inline bool     Sched_energy_aware = false;
        inline bool     Sched_schedstats   = false;
        inline string_t Sched_latency_ns;
        inline string_t Sched_migration_cost_ns;
        inline string_t Sched_min_granularity_ns;
        inline string_t Sched_wakeup_granularity_ns;
        inline string_t Sched_nr_migrate;
        inline string_t Sched_util_clamp_min;
        inline string_t Sched_util_clamp_max;
    }

    namespace GpuFreq {
        inline bool     enable   = true;
        inline string_t min_freq;
        inline string_t max_freq;
        // 原子快照：配置加载完成时发布，gpuFreqGuard 每秒读它而非上面的 string（消除跨线程竞争）
        inline std::atomic<int> min_mhz{0};
        inline std::atomic<int> max_mhz{0};
        inline std::atomic<int> enabled{1};
    }

    namespace Performances {
        inline int      Online[kCoreCount]    = { -1, -1, -1, -1, -1, -1, -1, -1 };  // 默认 -1 跳过，不误关核心
        inline string_t MinFreq[kClusterCount];
        inline string_t MaxFreq[kClusterCount];
        inline string_t CpuGovernor[kClusterCount];
    }

    // 包名匹配支持 * 任意序列 / ? 单字符；优先级：AppProfile > Performances
    struct AppProfileModel {
        string_t modelName;
        string_t modeName;
        bool     isGame = false;
        // keepAlive=true 使非游戏画像也能锁定前台不掉档（小窗/弹窗/焦点切换均保持）
        bool     keepAlive = false;
        string_t packages[32];
        int      packageCount = 0;

        string_t MinFreq[kClusterCount];
        string_t MaxFreq[kClusterCount];
        string_t Governor[kClusterCount];
        string_t GpuMinFreq;
        string_t GpuMaxFreq;
        int      Online[kCoreCount] = { -1, -1, -1, -1, -1, -1, -1, -1 };

        string_t SchedParamName [kClusterCount][kMaxSchedParams];
        string_t SchedParamValue[kClusterCount][kMaxSchedParams];
        int      SchedParamCount[kClusterCount] = { 0, 0, 0, 0 };
    };

    namespace AppProfile {
        static constexpr int MAX_MODELS    = 32;
        static constexpr int MAX_BLACKLIST = 64;
        inline bool     enable = false;

        inline string_t        packageBlacklist[MAX_BLACKLIST];
        inline int             blacklistCount = 0;

        inline AppProfileModel Models[MAX_MODELS];
        inline int             modelCount   = 0;

        // perapp_powermode.txt 覆盖层：优先级高于 config.json packages
        static constexpr int MAX_PERAPP = 256;
        inline string_t perAppPkg[MAX_PERAPP];
        inline string_t perAppModel[MAX_PERAPP];
        inline int      perAppCount = 0;
        inline string_t perAppGlobal;              // * 行全局默认，可空

        // 多线程读、单线程写，必须 atomic
        inline std::atomic<int> currentMatch{-1};

        // 迭代式通配符匹配，避免递归爆栈
        inline bool matchPackage(const char* pattern, const char* pkg) {
            const char* star = nullptr;   // 上次 '*' 的位置
            const char* mark = nullptr;   // pkg 在那次 '*' 后的起点
            while (*pkg) {
                if (*pattern == '?') {
                    pattern++; pkg++;
                } else if (*pattern == '*') {
                    star = pattern++;
                    mark = pkg;
                    if (!*pattern) return true; // 末尾 '*'
                } else if (*pattern == *pkg) {
                    pattern++; pkg++;
                } else if (star) {
                    pattern = star + 1;
                    pkg     = ++mark;
                } else {
                    return false;
                }
            }
            while (*pattern == '*') pattern++;
            return *pattern == '\0';
        }

        inline bool isBlacklisted(const char* pkg) {
            for (int i = 0; i < blacklistCount; i++) {
                if (matchPackage(packageBlacklist[i].c_str(), pkg)) return true;
            }
            return false;
        }

        inline int findModelByName(const char* name) {
            for (int i = 0; i < modelCount; i++)
                if (Models[i].modelName == name) return i;
            return -1;
        }

        inline int findMatchingModel(const char* pkg) {
            if (isBlacklisted(pkg)) return -1;
            // perapp 优先：config.json 与 perapp 重复时以 perapp 为准
            for (int i = 0; i < perAppCount; i++) {
                if (matchPackage(perAppPkg[i].c_str(), pkg)) {
                    int idx = findModelByName(perAppModel[i].c_str());
                    if (idx >= 0) return idx;
                }
            }
            for (int i = 0; i < modelCount; i++) {
                if (Models[i].packageCount == 0) continue;
                for (int j = 0; j < Models[i].packageCount; j++) {
                    if (matchPackage(Models[i].packages[j].c_str(), pkg)) return i;
                }
            }
            // 无匹配时回退到 perapp 全局默认行（* <模式>），防止停在上个 App 的档位
            if (!perAppGlobal.empty()) {
                int idx = findModelByName(perAppGlobal.c_str());
                if (idx >= 0) return idx;
            }
            return -1;
        }
    }

    // 基础模式的调速器参数表，0-indexed，容量与 AppProfileModel::SchedParam* 一致
    struct SchedParam {
        string_t Name [kMaxSchedParams];
        string_t Value[kMaxSchedParams];
    };
};
