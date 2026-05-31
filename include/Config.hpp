// By Ktwo  --  v4.3 重构：对齐 Way_Balance 格式
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

    // ---------- meta ----------
    namespace Meta {
        string_t name;
        int      version = -1;
        string_t author;
        string_t loglevel;
    }

    // ---------- Policy ----------
    namespace Policy {
        int CpuPolicy[4] = { -1, -1, -1, -1 };
    }

    // ---------- Cpuset（核心分配）----------
    namespace Cpuset {
        bool     enable = false;
        string_t top_app;
        string_t foreground;
        string_t background;
        string_t system_background;
        string_t restricted;
    }

    // ---------- LaunchBoost（启动加速，默认关，省电考虑）----------
    namespace LaunchBoost {
        bool     enable = false;
        int      boost_rate_limit_ms = 35;
        string_t BoostFreq[4];
    }

    // ---------- OfficialMode（fast 模式恢复默认）----------
    namespace OfficialMode {
        bool enable = false;
    }

    // ---------- Scheduler（CFS 调度器参数）----------
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

    // ---------- GpuFreq（GPU 频率控制）----------
    namespace GpuFreq {
        bool     enable   = true;   // [Fix v4.3] 新增 enable 字段（旧版本编译报错的原因）
        string_t min_freq;           // MHz，字符串保留兼容
        string_t max_freq;           // MHz
    }

    // ---------- Performances（基础频率，由当前 mode 选择的 model 填充）----------
    namespace Performances {
        int      Online[8]    = { -1, -1, -1, -1, -1, -1, -1, -1 };  // [Fix v4.3] 默认 -1（跳过），不再误把核心关掉
        string_t MinFreq[4];
        string_t MaxFreq[4];
        string_t CpuGovernor[4];
    }

    // ============================================================
    //  场景化频率（SceneFreq）
    //  按场景覆盖 Performances，缺省字段继承 Performances 基础值
    //  v4.3 省电改动：默认仅启用 Standby 场景的低频覆盖
    // ============================================================
    namespace SceneFreq {
        constexpr int SCENE_COUNT = 5;       // None / Touch / AmSwitch / HeavyLoad / Standby
        string_t MinFreq [SCENE_COUNT][4];
        string_t MaxFreq [SCENE_COUNT][4];
        string_t Governor[SCENE_COUNT][4];
        bool     enable = true;
    }

    // ---------- SceneDetect 行为参数 ----------
    namespace SceneCfg {
        bool enable = true;

        // HeavyLoad 触发参数（参考 uperf 三阶段省电设计）
        int heavy_load_thd        = 70;
        int idle_load_thd         = 30;
        int heavy_confirm_count   = 2;
        int heavy_max_duration_ms = 3000;
        int request_burst_slack_ms= 2000;

        // [v4.5] 突发负载（类卡顿）检测 —— uperf/FAS 风格的 burst boost
        // 真正的 frame-level jank 需要 SurfaceFlinger/fpsgo 数据源，CTS 暂无此接入；
        // 退而求其次：两次采样间负载跳变超过 burst_delta_thd，且 curLoad >= burst_min_load
        // → 视为"类卡顿事件"，立即进入 HeavyLoad 触发 boost 频率（与 HeavyLoad 共享上限）
        int burst_delta_thd       = 30;   // delta-load 跳变阈值（%）
        int burst_min_load        = 50;   // 当前 load 最小阈值（%），排除低负载常规变化

        // 短时场景时长
        int am_switch_duration_ms = 1500;
        int touch_duration_ms     = 1000;

        // 采样间隔 -- v4.3 默认延长（省电）
        int load_sample_interval_ms = 2000;   // 1s -> 2s
        int screen_poll_interval_ms = 3000;   // 2s -> 3s（仅 fallback 时使用）

        // Touch 检测开关 — 默认关闭：触摸事件极高频，触发频率重写会显著费电
        // 已由 AmSwitch + HeavyLoad 覆盖响应度场景；如需可在 config.json 中显式开启
        bool touch_enable = false;

        // 输入设备最大编号
        int input_dev_max = 16;
    }

    // ============================================================
    //  AppProfile 应用画像（v4.3 重构：Way_Balance 扁平格式）
    //
    //  JSON 结构：
    //    "package_blacklist": [...],
    //    "models": [
    //      {
    //        "model_name": "game_heavy",
    //        "is_game": true,
    //        "packages": ["com.tencent.tmgp.sgame", "com.miHoYo.*"],
    //        "freq_min_c0": 800000, "freq_max_c0": 2800000,
    //        "freq_min_c1": 1000000,"freq_max_c1": 3000000,
    //        "gpu_min_mhz": 500,    "gpu_max_mhz": 1100,
    //        "gov_c0": { "governor": "walt", "params": {"hispeed_freq":"1324800"} }
    //      }
    //    ]
    //
    //  包名匹配：精确 / "*" 任意序列 / "?" 单字符
    //  优先级：AppProfile > SceneFreq > Performances
    // ============================================================
    struct AppProfileModel {
        string_t modelName;
        string_t modeName;      // 情景模式名：powersave/balance/performance/fast（用于匹配 mode.txt）
        bool     isGame = false;
        string_t packages[32];
        int      packageCount = 0;

        // 频率（任一为空回退）
        string_t MinFreq[4];
        string_t MaxFreq[4];
        string_t Governor[4];
        string_t GpuMinFreq;
        string_t GpuMaxFreq;
        int      Online[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

        // 调速器自定义参数（每个集群独立）
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

        // ===== perapp_powermode.txt 覆盖层(daemon 主导切档,app 只当编辑器) =====
        // 格式: 每行 "包名 画像名"。优先级高于 config.json 的 packages。
        // 仅存"包名->画像名"映射,画像的实际频率仍从下面 Models[] 里同名 model 读。
        static constexpr int MAX_PERAPP = 256;
        string_t perAppPkg[MAX_PERAPP];     // 包名(支持通配符)
        string_t perAppModel[MAX_PERAPP];   // 对应画像名(model_name)
        int      perAppCount = 0;
        string_t perAppGlobal;              // 全局默认模式(* 行),可空

        // [Fix v4.1] currentMatch 被多个线程读、单线程写 → 必须 atomic
        // 含义：>=0 表示当前匹配到了 packages 非空的"应用画像"模型；
        //       -1 表示没有匹配的应用画像（此时仅用基础模型 + SceneFreq）
        std::atomic<int> currentMatch{-1};

        // [Fix v4.3] 迭代式通配符匹配，避免递归爆栈
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

        // 按画像名(model_name)找 model 索引,找不到 -1
        inline int findModelByName(const char* name) {
            for (int i = 0; i < modelCount; i++)
                if (Models[i].modelName == name) return i;
            return -1;
        }

        inline int findMatchingModel(const char* pkg) {
            if (isBlacklisted(pkg)) return -1;
            // [perapp 优先] 先查 perapp_powermode.txt: 命中按其画像名定位 model。
            //   config.json 与 perapp 重复时以 perapp 为准(daemon 主导切档)。
            for (int i = 0; i < perAppCount; i++) {
                if (matchPackage(perAppPkg[i].c_str(), pkg)) {
                    int idx = findModelByName(perAppModel[i].c_str());
                    if (idx >= 0) return idx;
                    // perapp 指定画像在 config 不存在 → 跳过,继续其它规则
                }
            }
            // 回退: config.json 各 model 的 packages 匹配
            for (int i = 0; i < modelCount; i++) {
                if (Models[i].packageCount == 0) continue;
                for (int j = 0; j < Models[i].packageCount; j++) {
                    if (matchPackage(Models[i].packages[j].c_str(), pkg)) return i;
                }
            }
            return -1;
        }
    }

    // ---------- Netlink uevent 屏幕监听 ----------
    namespace NetlinkCfg {
        bool enable           = true;
        int  fallback_poll_ms = 3000;   // v4.3 默认 3s（省电）
    }

    // ---------- 全局 SchedParam（被基础 model 写入）----------
    struct SchedParam {
        string_t Name [24];
        string_t Value[24];
    };
};
