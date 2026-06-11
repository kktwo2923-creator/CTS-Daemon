#pragma once
// 重构：
//   1) Way_Balance 扁平格式 + Switch 合并到 models (基础模型有 mode 字段 + Scenes 子节点)
//   2) [Fix] 容忍配置文件把 bool/int 写成字符串（"true"/"75" 这种）
//   3) [Fix] readBool/readInt 统一封装，避免 try/catch 噪音
//   4) [Fix] 把模型在 models[] 中的 baseIdx 直接保存，避免重复 array()[idx] 查找
//   5) [Fix] 解析 Scenes 提取为独立函数，减少嵌套层数

#include "Json/json.h"
#include "Config.hpp"
#include "Logger.hpp"
#include <stdexcept>
#include <cstring>

using namespace Config;
using namespace qlib;

class JsonConfig {
private:
    static constexpr const char* configPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* modePath   = "/sdcard/Android/CTS/mode.txt";

    Logger logger;
    json_view_t json;

    char buff[256];

public:
    SchedParam  schedParam[4];
    std::string mode;
    // [Fix] baseModelIdx 缓存：避免后续 Scenes 解析时再次遍历 models 找 baseIdx
    int         baseModelIdx = -1;

    // ============================================================
    //  公共：mode.txt
    // ============================================================
    void LoadConfig() {
        ifstream file(modePath);
        if (!file.is_open()) {
            // mode.txt 不存在时默认 balance，避免首次部署报错
            fprintf(stderr, "无法打开配置文件: %s，使用默认 balance\n", modePath);
            mode = "balance";
            return;
        }
        std::string temp;
        getline(file, temp);
        size_t end = temp.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) temp.erase(end + 1);
        else                          temp.clear();
        mode = std::move(temp);
    }

    bool switchConfig() const {
        return mode == "powersave" || mode == "balance" ||
               mode == "performance" || mode == "fast";
    }

    // ============================================================
    //  helpers — 容忍多种字段类型
    //
    //  许多用户/编辑器会把所有值写成字符串（"true" / "75"）。
    //  qlib::json 是严格类型库，get<bool>() 在 string 值上抛异常 → 设置被静默忽略。
    //  这些 helper 同时接受真实的 bool/int 和字符串形态。
    // ============================================================
    static string_t intToStr(int v) {
        char tmp[16];
        FastSnprintf(tmp, sizeof(tmp), "%d", v);
        return string_t(tmp);
    }

    // 解析"true"/"false"/"1"/"0"/"on"/"off"/"yes"/"no"
    static bool parseBoolString(const char* s, bool& out) {
        if (!s || !*s) return false;
        if (!strcmp(s, "true") || !strcmp(s, "1") ||
            !strcmp(s, "on")   || !strcmp(s, "yes")) {
            out = true; return true;
        }
        if (!strcmp(s, "false") || !strcmp(s, "0") ||
            !strcmp(s, "off")   || !strcmp(s, "no")) {
            out = false; return true;
        }
        return false;
    }

    // readBool: 优先 native bool；不行就尝试 string
    template <class Node>
    bool readBool(Node& node, const char* key, bool& out) {
        try {
            out = node[key].template get<bool>();
            return true;
        } catch (...) {}
        try {
            string_t s = node[key].template get<string_t>();
            return parseBoolString(s.c_str(), out);
        } catch (...) {}
        return false;
    }

    // readInt: 优先 native int；不行就 atoi(string)
    template <class Node>
    bool readInt(Node& node, const char* key, int& out) {
        try {
            out = node[key].template get<int>();
            return true;
        } catch (...) {}
        try {
            string_t s = node[key].template get<string_t>();
            if (s.empty()) return false;
            int v = Fastatoi(s.c_str());
            // Fastatoi 不支持负号，对于 "-1" 这种特例补一下
            if (s.c_str()[0] == '-') v = -Fastatoi(s.c_str() + 1);
            out = v;
            return true;
        } catch (...) {}
        return false;
    }

    // readStr: native string
    template <class Node>
    bool readStr(Node& node, const char* key, string_t& out) {
        try {
            out = node[key].template get<string_t>();
            return true;
        } catch (...) {}
        return false;
    }

    // readFreq: 支持 int / string；0 或 -1 视为"未设置"
    template <class Node>
    bool readFreq(Node& node, const char* key, string_t& out) {
        int v;
        if (readInt(node, key, v)) {
            if (v <= 0) return false;
            out = intToStr(v);
            return true;
        }
        // 已经是 string 形态的 readInt 会被 readInt 中的 string 分支处理
        // 这里不再尝试 readStr，因为非数字字符串当频率没意义
        return false;
    }

    // ============================================================
    //  解析单个 model 节点 -> AppProfileModel
    // ============================================================
    template <class Node>
    void parseModelNode(Node& node, AppProfileModel& out) {
        readStr (node, "model_name", out.modelName);
        readStr (node, "mode",       out.modeName);
        readBool(node, "is_game",    out.isGame);
        readBool(node, "keep_alive", out.keepAlive);   // [keep_alive] 非游戏画像也可享保活

        // packages
        try {
            auto& arr = node["packages"];
            int n = 0;
            for (int p = 0; p < 32; p++) {
                try {
                    string_t pkg = arr.array()[p].template get<string_t>();
                    if (pkg.empty()) break;
                    out.packages[n++] = pkg;
                } catch (...) { break; }
            }
            out.packageCount = n;
        } catch (...) {}

        // 频率 freq_min_c0 / freq_max_c0 ...
        for (int i = 0; i <= 3; i++) {
            FastSnprintf(buff, sizeof(buff), "freq_min_c%d", i);
            readFreq(node, buff, out.MinFreq[i]);
            FastSnprintf(buff, sizeof(buff), "freq_max_c%d", i);
            readFreq(node, buff, out.MaxFreq[i]);
        }

        // gov_c0 / gov_c1 / ... 嵌套 governor + params
        parseGovernorNodes(node, out);

        // GPU 频率：gpu_min_mhz / gpu_max_mhz（数字或字符串）
        readFreq(node, "gpu_min_mhz", out.GpuMinFreq);
        readFreq(node, "gpu_max_mhz", out.GpuMaxFreq);
        // 兼容旧的嵌套写法
        try {
            auto& gpu = node["GpuFreq"];
            if (out.GpuMinFreq.empty()) readFreq(gpu, "min_freq", out.GpuMinFreq);
            if (out.GpuMaxFreq.empty()) readFreq(gpu, "max_freq", out.GpuMaxFreq);
        } catch (...) {}

        // 核心开关 CoreOnline:{Core0:1,...}
        try {
            auto& coreNode = node["CoreOnline"];
            for (int i = 0; i <= 7; i++) {
                FastSnprintf(buff, sizeof(buff), "Core%d", i);
                int v;
                if (readInt(coreNode, buff, v)) {
                    out.Online[i] = (v == 1) ? 1 : (v == 0 ? 0 : -1);
                }
            }
        } catch (...) {}
    }

    // 子函数：解析 gov_c0..gov_c3 的 governor + params
    template <class Node>
    void parseGovernorNodes(Node& node, AppProfileModel& out) {
        // 此表是按内核常见调速器参数白名单（qlib::json 不支持迭代 object 的 key）
        static const char* commonParams[] = {
            "hispeed_freq", "hispeed_load", "hispeed_cond_freq",
            "rtg_boost_freq", "pl", "zone_max_util_pct",
            "above_hispeed_delay", "go_hispeed_load", "target_loads",
            "min_sample_time", "timer_rate", "boost",
            "boostpulse_duration", "io_is_busy", "down_rate_limit_us",
            "up_rate_limit_us"
        };
        constexpr int N = sizeof(commonParams) / sizeof(commonParams[0]);

        for (int i = 0; i <= 3; i++) {
            FastSnprintf(buff, sizeof(buff), "gov_c%d", i);
            try {
                auto& gov = node[buff];
                readStr(gov, "governor", out.Governor[i]);
                try {
                    auto& params = gov["params"];
                    int spCount = 0;
                    for (int k = 0; k < N && spCount < 16; k++) {
                        string_t v;
                        if (readStr(params, commonParams[k], v) && !v.empty()) {
                            out.SchedParamName [i][spCount] = commonParams[k];
                            out.SchedParamValue[i][spCount] = v;
                            spCount++;
                        }
                    }
                    out.SchedParamCount[i] = spCount;
                } catch (...) {}
            } catch (...) {}
        }

        // [兼容] 同时支持 "governor": {"c0":"hmbird","c1":...} 这种嵌套格式。
        // 仅当上面的 gov_cN 没读到时,从 governor.cN 补读,避免配置写法不一致导致调速器静默失效。
        try {
            auto& govObj = node["governor"];
            for (int i = 0; i <= 3; i++) {
                if (!out.Governor[i].empty()) continue;
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                string_t v;
                if (readStr(govObj, buff, v) && !v.empty())
                    out.Governor[i] = v;
            }
        } catch (...) {}
    }

    // ============================================================
    //  解析 Scenes 子节点（属于基础模型）
    //    向后兼容两种写法：
    //      "Standby": { "freq_max_c0": 600000 }           — 扁平（推荐）
    //      "Standby": { "MaxFreq": {"c0":"600000"} }       — 旧嵌套
    // ============================================================
    void resetSceneFreq() {
        for (int s = 0; s < SceneFreq::SCENE_COUNT; s++) {
            for (int i = 0; i <= 3; i++) {
                SceneFreq::MinFreq [s][i] = "";
                SceneFreq::MaxFreq [s][i] = "";
                SceneFreq::Governor[s][i] = "";
            }
        }
    }

    template <class Node>
    void parseScenesNode(Node& sceneRoot) {
        static const char* sceneNames[] = {
            "None", "Touch", "AmSwitch", "HeavyLoad", "Standby"
        };
        int loaded = 0;
        for (int s = 0; s < SceneFreq::SCENE_COUNT; s++) {
            try {
                auto& oneScene = sceneRoot[sceneNames[s]];
                bool any = false;
                for (int i = 0; i <= 3; i++) {
                    // 扁平写法 freq_min_c0
                    FastSnprintf(buff, sizeof(buff), "freq_min_c%d", i);
                    if (readFreq(oneScene, buff, SceneFreq::MinFreq[s][i])) any = true;
                    FastSnprintf(buff, sizeof(buff), "freq_max_c%d", i);
                    if (readFreq(oneScene, buff, SceneFreq::MaxFreq[s][i])) any = true;

                    // 兼容旧嵌套写法
                    FastSnprintf(buff, sizeof(buff), "c%d", i);
                    string_t v;
                    try {
                        if (readStr(oneScene["MinFreq"], buff, v) && !v.empty()) {
                            SceneFreq::MinFreq[s][i] = v; any = true;
                        }
                    } catch (...) {}
                    try {
                        if (readStr(oneScene["MaxFreq"], buff, v) && !v.empty()) {
                            SceneFreq::MaxFreq[s][i] = v; any = true;
                        }
                    } catch (...) {}
                    try {
                        if (readStr(oneScene["governor"], buff, v) && !v.empty()) {
                            SceneFreq::Governor[s][i] = v; any = true;
                        }
                    } catch (...) {}
                }
                if (any) loaded++;
            } catch (...) {}
        }
        if (loaded > 0) logger.Info("场景频率已加载 %d 个", loaded);
    }

    // ============================================================
    //  清零所有运行时状态
    // ============================================================
    void resetState() {
        for (int i = 0; i <= 3; i++) {
            for (int j = 0; j < 24; j++) {
                schedParam[i].Name [j] = "";
                schedParam[i].Value[j] = "";
            }
            Performances::MinFreq[i]     = "";
            Performances::MaxFreq[i]     = "";
            Performances::CpuGovernor[i] = "";
            LaunchBoost::BoostFreq[i]    = "";
        }
        for (int i = 0; i <= 7; i++) Performances::Online[i] = -1;
        GpuFreq::min_freq = "";
        GpuFreq::max_freq = "";
        Meta::name = ""; Meta::author = ""; Meta::loglevel = "";

        AppProfile::modelCount      = 0;
        AppProfile::blacklistCount  = 0;
        AppProfile::currentMatch.store(-1);
        baseModelIdx = -1;
        for (int i = 0; i < AppProfile::MAX_BLACKLIST; i++)
            AppProfile::packageBlacklist[i] = "";
        for (int i = 0; i < AppProfile::MAX_MODELS; i++) {
            auto& m = AppProfile::Models[i];
            m.modelName = ""; m.modeName = ""; m.isGame = false; m.keepAlive = false; m.packageCount = 0;
            for (int j = 0; j < 32; j++) m.packages[j] = "";
            for (int j = 0; j <= 3; j++) {
                m.MinFreq[j] = ""; m.MaxFreq[j] = ""; m.Governor[j] = "";
                m.SchedParamCount[j] = 0;
                for (int k = 0; k < 16; k++) {
                    m.SchedParamName [j][k] = "";
                    m.SchedParamValue[j][k] = "";
                }
            }
            m.GpuMinFreq = ""; m.GpuMaxFreq = "";
            for (int j = 0; j <= 7; j++) m.Online[j] = -1;
        }
        resetSceneFreq();
    }

    // ============================================================
    //  主入口
    // ============================================================
    bool readConfig() {
        resetState();

        ifstream ifs(configPath, std::ios::binary);
        if (!ifs) { logger.Error("无法打开配置文件: %s", configPath); return false; }
        std::string text((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
        int rc = json::parse(&json, text.data(), text.data() + text.size());
        if (rc != 0) { logger.Error("解析 config.json 失败 错误码: %d", rc); return false; }

        // ---- meta ----
        // [Fix] meta 是第一个解析的节点，loglevel 必须在所有后续 logger.Info 之前生效
        // 否则用户即使配 loglevel=ERROR，readConfig() 里几十条 INFO 也会全部刷出来
        try {
            auto& m = json["meta"];
            readStr (m, "name",     Meta::name);
            readInt (m, "version",  Meta::version);
            readStr (m, "author",   Meta::author);
            readStr (m, "loglevel", Meta::loglevel);
        } catch (...) { logger.Warn("meta 节点缺失"); }
        // 立即生效 loglevel，避免后续 readConfig 内部的 Info 日志破坏顺序
        if (!Meta::loglevel.empty()) logger.setLogLevel(Meta::loglevel);

        // ---- Policy ----
        try {
            auto& p = json["Policy"];
            for (int i = 0; i <= 3; i++) {
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                int v;
                if (readInt(p, buff, v)) {
                    Policy::CpuPolicy[i] = (v == 255) ? -1 : v;
                }
            }
        } catch (...) { logger.Warn("Policy 节点异常"); }

        // ---- Function ----
        parseFunctionSection();

        LoadConfig();
        // [Fix] mode.txt 是外部文件，可能被写入画像模型名(game_heavy/video 等无 mode 字段的
        //       AppProfile 模型)或其它非法值。早期实现遇到非法值直接 return false 中止整次
        //       readConfig，导致守护拒绝应用配置、表现为"情景模式无效/配置重载失败"反复刷屏，
        //       看似崩溃循环。守护进程不应因一次外部脏写而失效——改为告警并回退到安全基础档
        //       balance，保证始终能加载并应用一份有效配置。
        //       (按 App 自动切换的画像由 AppProfile 按包名匹配处理，本就不经过 mode.txt。)
        if (mode.empty()) {
            logger.Warn("情景模式为空，回退到 balance");
            mode = "balance";
        }
        if (!switchConfig()) {
            logger.Warn("情景模式无效: '%s'，回退到 balance", mode.c_str());
            mode = "balance";
        }
        logger.Info("当前情景模式: %s", mode.c_str());

        // ---- package_blacklist + models ----
        parseBlacklist();
        parseModels();

        // ---- 在 models 中找 mode 匹配的基础模型 → 填充 Performances 和 Scenes ----
        applyBaseModel();

        // ---- 加载 perapp_powermode.txt 覆盖层(优先于 config 的 packages) ----
        loadPerApp();

        return true;
    }

    // 加载 /sdcard/Android/CTS/perapp_powermode.txt
    //   每行 "包名 画像名"; # 注释; * 开头为全局默认行。填充 AppProfile::perApp*。
    void loadPerApp() {
        using namespace Config;
        AppProfile::perAppCount = 0;
        AppProfile::perAppGlobal = "";
        static const char* paths[] = {
            "/sdcard/Android/CTS/perapp_powermode.txt",
            "/storage/emulated/0/Android/CTS/perapp_powermode.txt",
        };
        FILE* fp = nullptr;
        for (const char* p : paths) { fp = fopen(p, "r"); if (fp) break; }
        if (!fp) { logger.Info("perapp_powermode.txt 不存在,跳过(仅用 config 画像)"); return; }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            // 去首尾空白
            char* s = line;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = strlen(s);
            while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
                s[--len] = '\0';
            if (len == 0 || s[0] == '#') continue;
            if (s[0] == '*') {                       // 全局默认行: * <mode>
                char* sp = s + 1;
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp) AppProfile::perAppGlobal = sp;
                continue;
            }
            // 拆 "包名 画像名"
            char* sep = s;
            while (*sep && *sep != ' ' && *sep != '\t') sep++;
            if (!*sep) continue;                     // 没有第二列,跳过
            *sep = '\0';
            char* mode = sep + 1;
            while (*mode == ' ' || *mode == '\t') mode++;
            if (!*mode) continue;
            if (AppProfile::perAppCount < Config::AppProfile::MAX_PERAPP) {
                AppProfile::perAppPkg  [AppProfile::perAppCount] = s;
                AppProfile::perAppModel[AppProfile::perAppCount] = mode;
                AppProfile::perAppCount++;
            }
        }
        fclose(fp);
        // [优化] inotify 每次触发都会重载,只在条数变化(新增/删减)时打印,避免刷屏
        static int lastPerAppCount = -1;
        if (AppProfile::perAppCount != lastPerAppCount) {
            logger.Info("perapp_powermode.txt 已加载: %d 条 App 画像覆盖(优先于 config)",
                        AppProfile::perAppCount);
            lastPerAppCount = AppProfile::perAppCount;
        }
    }

private:
    // ---- Function 子树 ----
    void parseFunctionSection() {
        try {
            auto& F = json["Function"];

            // Cpuset
            try {
                auto& c = F["Cpuset"];
                readBool(c, "enable",            Cpuset::enable);
                readStr (c, "top_app",           Cpuset::top_app);
                readStr (c, "foreground",        Cpuset::foreground);
                readStr (c, "background",        Cpuset::background);
                readStr (c, "system_background", Cpuset::system_background);
                readStr (c, "restricted",        Cpuset::restricted);
            } catch (...) { logger.Debug("无 Cpuset 节点"); }

            // LaunchBoost
            try {
                auto& l = F["LaunchBoost"];
                readBool(l, "enable",              LaunchBoost::enable);
                readInt (l, "boost_rate_limit_ms", LaunchBoost::boost_rate_limit_ms);
                try {
                    auto& bf = l["BoostFreq"];
                    for (int i = 0; i <= 3; i++) {
                        FastSnprintf(buff, sizeof(buff), "c%d", i);
                        readStr(bf, buff, LaunchBoost::BoostFreq[i]);
                    }
                } catch (...) {}
            } catch (...) {}

            // OfficialMode
            try {
                auto& o = F["OfficialMode"];
                readBool(o, "enable", OfficialMode::enable);
            } catch (...) {}

            // Scheduler
            try {
                auto& s = F["Scheduler"];
                readBool(s, "enable",                       Scheduler::enable);
                readBool(s, "sched_energy_aware",           Scheduler::Sched_energy_aware);
                readBool(s, "sched_schedstats",             Scheduler::Sched_schedstats);
                readStr (s, "sched_latency_ns",             Scheduler::Sched_latency_ns);
                readStr (s, "sched_migration_cost_ns",      Scheduler::Sched_migration_cost_ns);
                readStr (s, "sched_min_granularity_ns",     Scheduler::Sched_min_granularity_ns);
                readStr (s, "sched_wakeup_granularity_ns",  Scheduler::Sched_wakeup_granularity_ns);
                readStr (s, "sched_nr_migrate",             Scheduler::Sched_nr_migrate);
                readStr (s, "sched_util_clamp_min",         Scheduler::Sched_util_clamp_min);
                readStr (s, "sched_util_clamp_max",         Scheduler::Sched_util_clamp_max);
            } catch (...) {}

            // GpuFreq.enable
            try {
                auto& g = F["GpuFreq"];
                readBool(g, "enable", GpuFreq::enable);
            } catch (...) {}

            // SceneDetect
            try {
                auto& sc = F["SceneDetect"];
                readBool(sc, "enable",                  SceneCfg::enable);
                readInt (sc, "heavy_load_thd",          SceneCfg::heavy_load_thd);
                readInt (sc, "idle_load_thd",           SceneCfg::idle_load_thd);
                readInt (sc, "heavy_confirm_count",     SceneCfg::heavy_confirm_count);
                readInt (sc, "heavy_max_duration_ms",   SceneCfg::heavy_max_duration_ms);
                readInt (sc, "request_burst_slack_ms",  SceneCfg::request_burst_slack_ms);
                // 突发负载（类卡顿）检测参数
                readInt (sc, "burst_delta_thd",         SceneCfg::burst_delta_thd);
                readInt (sc, "burst_min_load",          SceneCfg::burst_min_load);
                readInt (sc, "am_switch_duration_ms",   SceneCfg::am_switch_duration_ms);
                readInt (sc, "touch_duration_ms",       SceneCfg::touch_duration_ms);
                readInt (sc, "load_sample_interval_ms", SceneCfg::load_sample_interval_ms);
                readInt (sc, "screen_poll_interval_ms", SceneCfg::screen_poll_interval_ms);
                readBool(sc, "touch_enable",            SceneCfg::touch_enable);
                readInt (sc, "input_dev_max",           SceneCfg::input_dev_max);
            } catch (...) {}

            // AppProfile
            try {
                auto& ap = F["AppProfile"];
                readBool(ap, "enable", AppProfile::enable);
            } catch (...) {}

            // NetlinkScreen
            try {
                auto& nl = F["NetlinkScreen"];
                readBool(nl, "enable",           NetlinkCfg::enable);
                readInt (nl, "fallback_poll_ms", NetlinkCfg::fallback_poll_ms);
            } catch (...) {}
        } catch (const qlib::exception& e) {
            logger.Warn("Function 节点异常: %s", e.what());
        }
    }

    void parseBlacklist() {
        try {
            auto& bl = json["package_blacklist"];
            int n = 0;
            for (int i = 0; i < AppProfile::MAX_BLACKLIST; i++) {
                try {
                    string_t pkg = bl.array()[i].get<string_t>();
                    if (pkg.empty()) break;
                    AppProfile::packageBlacklist[n++] = pkg;
                } catch (...) { break; }
            }
            AppProfile::blacklistCount = n;
            // [优化] 只在首次加载或条数变化时打印,避免每次重载 config 刷屏
            static int lastBlacklistCount = -1;
            if (n > 0 && n != lastBlacklistCount) {
                logger.Info("包名黑名单已加载: %d 条", n);
                lastBlacklistCount = n;
            }
        } catch (...) { logger.Debug("无 package_blacklist"); }
    }

    void parseModels() {
        // 明细日志只在进程首次加载时打印一次(确认模型加载成功);
        // 之后每次切档重载不再刷屏(降级为 Debug)。
        static bool firstLoad = true;
        try {
            auto& arr = json["models"];
            int n = 0;
            for (int m = 0; m < AppProfile::MAX_MODELS; m++) {
                try {
                    auto& node = arr.array()[m];
                    string_t mName;
                    readStr(node, "model_name", mName);
                    if (mName.empty()) break;
                    parseModelNode(node, AppProfile::Models[n]);
                    if (firstLoad)
                        logger.Info("模型已加载: %s (mode=%s, 包名=%d, isGame=%d, c0=[%s,%s])",
                                AppProfile::Models[n].modelName.c_str(),
                                AppProfile::Models[n].modeName.c_str(),
                                AppProfile::Models[n].packageCount,
                                AppProfile::Models[n].isGame ? 1 : 0,
                                AppProfile::Models[n].MinFreq[0].c_str(),
                                AppProfile::Models[n].MaxFreq[0].c_str());
                    n++;
                } catch (...) { break; }
            }
            AppProfile::modelCount = n;
            if (firstLoad) {
                logger.Info("共加载 %d 个模型", n);
                firstLoad = false;
            }
        } catch (...) { logger.Debug("无 models 节点"); }
    }

    // 从 models 中找 mode 匹配的基础模型，填充 Performances + GpuFreq + schedParam[] + Scenes
    void applyBaseModel() {
        baseModelIdx = -1;
        for (int i = 0; i < AppProfile::modelCount; i++) {
            // [Fix] qlib::string 的 operator== 在两个独立堆分配的 string_t 之间比较时
            //            有 UB（friend operator==(a,b) 调用 equal(a.begin(), b.end(), b.begin())
            //            导致 distance 跨指针越界）。这就是日志显示"找不到 mode='powersave'"
            //            但明明 powersave 已加载的根本原因。改用 strcmp 走 c_str() 路径。
            const char* mn = AppProfile::Models[i].modeName.c_str();
            if (mn && strcmp(mn, mode.c_str()) == 0) {
                baseModelIdx = i;
                break;
            }
        }

        if (baseModelIdx < 0) {
            logger.Error("models 中找不到 mode='%s' 的基础模型 — 无法应用配置", mode.c_str());
            return;
        }

        const auto& m = AppProfile::Models[baseModelIdx];
        for (int i = 0; i <= 3; i++) {
            Performances::MinFreq[i]     = m.MinFreq[i];
            Performances::MaxFreq[i]     = m.MaxFreq[i];
            Performances::CpuGovernor[i] = m.Governor[i];
            if (!Performances::MinFreq[i].empty() ||
                !Performances::MaxFreq[i].empty() ||
                !Performances::CpuGovernor[i].empty()) {
                logger.Info("CPU簇 %d: min=%s max=%s gov=%s",
                            Policy::CpuPolicy[i],
                            Performances::MinFreq[i].c_str(),
                            Performances::MaxFreq[i].c_str(),
                            Performances::CpuGovernor[i].c_str());
            }
        }
        for (int i = 0; i <= 7; i++)
            Performances::Online[i] = m.Online[i];

        // SchedParam 拷贝到全局 schedParam[]
        for (int i = 0; i <= 3; i++) {
            int cnt = m.SchedParamCount[i];
            for (int j = 0; j < cnt && j < 23; j++) {
                // schedParam[].Name/Value 是 1-indexed（历史原因）
                schedParam[i].Name [j+1] = m.SchedParamName [i][j];
                schedParam[i].Value[j+1] = m.SchedParamValue[i][j];
            }
        }
        GpuFreq::min_freq = m.GpuMinFreq;
        GpuFreq::max_freq = m.GpuMaxFreq;
        logger.Info("基础模型: %s (mode=%s)", m.modelName.c_str(), mode.c_str());

        // ---- Scenes（场景频率覆盖）从基础模型的 Scenes 字段读取 ----
        try {
            auto& sceneRoot = json["models"].array()[baseModelIdx]["Scenes"];
            parseScenesNode(sceneRoot);
        } catch (...) { logger.Debug("基础模型无 Scenes 节点"); }
    }
};
