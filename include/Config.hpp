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

    namespace Meta {
        string_t name;
        int      version = -1;
        string_t author;
        string_t loglevel;
    }

    namespace Policy {
        int CpuPolicy[4] = { -1, -1, -1, -1 };
    }

    namespace Cpuset {
        bool     enable = false;
        string_t top_app;
        string_t foreground;
        string_t background;
        string_t system_background;
        string_t restricted;
    }

    namespace LaunchBoost {
        bool     enable = false;
        int      boost_rate_limit_ms = 35;
        string_t BoostFreq[4];
    }

    namespace OfficialMode {
        bool enable = false;
    }

    namespace Scheduler {
        bool     enable = false;
        bool     Sched_energy_aware = false;
        bool     Sched_schedstats   = false;
        string_t Sched_latency_ns;
        string_t Sched_migration_cost_ns;
        string_t Sched_min_granularity_ns;
        string_t Sched_wakeup_granularity_ns;
        string_t Sched_nr_migrate;
        string_t Sched_util_clamp_min;
        string_t Sched_util_clamp_max;
    }

    namespace GpuFreq {
        bool     enable   = true;
        string_t min_freq;
        string_t max_freq;
        // 原子快照：配置加载完成时发布，gpuFreqGuard 每秒读它而非上面的 string（消除跨线程竞争）
        std::atomic<int> min_mhz{0};
        std::atomic<int> max_mhz{0};
        std::atomic<int> enabled{1};
    }

    namespace Performances {
        int      Online[8]    = { -1, -1, -1, -1, -1, -1, -1, -1 };  // 默认 -1 跳过，不误关核心
        string_t MinFreq[4];
        string_t MaxFreq[4];
        string_t CpuGovernor[4];
    }

    // 场景化频率：按场景覆盖 Performances，缺省字段继承基础值
    namespace SceneFreq {
        constexpr int SCENE_COUNT = 5;       // None / Touch / AmSwitch / HeavyLoad / Standby
        string_t MinFreq [SCENE_COUNT][4];
        string_t MaxFreq [SCENE_COUNT][4];
        string_t Governor[SCENE_COUNT][4];
        bool     enable = true;
    }

    namespace SceneCfg {
        bool enable = true;

        int heavy_load_thd        = 70;
        int idle_load_thd         = 30;
        int heavy_confirm_count   = 2;
        int heavy_max_duration_ms = 3000;
        int request_burst_slack_ms= 2000;

        // 采样间负载跳变 > burst_delta_thd 且 curLoad >= burst_min_load → 类卡顿事件，触发 boost
        int burst_delta_thd       = 30;
        int burst_min_load        = 50;

        int am_switch_duration_ms = 1500;
        int touch_duration_ms     = 1000;

        int load_sample_interval_ms = 2000;
        int screen_poll_interval_ms = 3000;   // fallback 轮询间隔

        // touch 事件极高频，默认关闭，避免频繁频率重写费电
        bool touch_enable = false;

        int input_dev_max = 16;
    }

    // 包名匹配支持 * 任意序列 / ? 单字符；优先级：AppProfile > SceneFreq > Performances
    struct AppProfileModel {
        string_t modelName;
        string_t modeName;
        bool     isGame = false;
        // keepAlive=true 使非游戏画像也能锁定前台不掉档（小窗/弹窗/焦点切换均保持）
        bool     keepAlive = false;
        string_t packages[32];
        int      packageCount = 0;

        string_t MinFreq[4];
        string_t MaxFreq[4];
        string_t Governor[4];
        string_t GpuMinFreq;
        string_t GpuMaxFreq;
        int      Online[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

        string_t SchedParamName [4][16];
        string_t SchedParamValue[4][16];
        int      SchedParamCount[4] = { 0, 0, 0, 0 };
    };

    namespace AppProfile {
        static constexpr int MAX_MODELS    = 32;
        static constexpr int MAX_BLACKLIST = 64;
        bool     enable = false;

        string_t        packageBlacklist[MAX_BLACKLIST];
        int             blacklistCount = 0;

        AppProfileModel Models[MAX_MODELS];
        int             modelCount   = 0;

        // perapp_powermode.txt 覆盖层：优先级高于 config.json packages
        static constexpr int MAX_PERAPP = 256;
        string_t perAppPkg[MAX_PERAPP];
        string_t perAppModel[MAX_PERAPP];
        int      perAppCount = 0;
        string_t perAppGlobal;              // * 行全局默认，可空

        // 多线程读、单线程写，必须 atomic
        std::atomic<int> currentMatch{-1};

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

    namespace NetlinkCfg {
        bool enable           = true;
        int  fallback_poll_ms = 3000;
    }

    struct SchedParam {
        string_t Name [24];
        string_t Value[24];
    };
};
