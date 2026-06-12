#pragma once

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
    int         baseModelIdx = -1;

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

    // qlib::json 严格类型，用户常把 bool/int 写成字符串，这些 helper 同时接受两种形态。
    static string_t intToStr(int v) {
        char tmp[16];
        FastSnprintf(tmp, sizeof(tmp), "%d", v);
        return string_t(tmp);
    }

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
            // Fastatoi 不支持负号，"-1" 特例补丁
            if (s.c_str()[0] == '-') v = -Fastatoi(s.c_str() + 1);
            out = v;
            return true;
        } catch (...) {}
        return false;
    }

    template <class Node>
    bool readStr(Node& node, const char* key, string_t& out) {
        try {
            out = node[key].template get<string_t>();
            return true;
        } catch (...) {}
        return false;
    }

    // 读频率，0/-1 视为未设置
    template <class Node>
    bool readFreq(Node& node, const char* key, string_t& out) {
        int v;
        if (readInt(node, key, v)) {
            if (v <= 0) return false;
            out = intToStr(v);
            return true;
        }
        return false;
    }

    template <class Node>
    void parseModelNode(Node& node, AppProfileModel& out) {
        readStr (node, "model_name", out.modelName);
        readStr (node, "mode",       out.modeName);
        readBool(node, "is_game",    out.isGame);
        readBool(node, "keep_alive", out.keepAlive);

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

        // 频率 freq_min_c0 / freq_max_c0 / ...
        for (int i = 0; i <= 3; i++) {
            FastSnprintf(buff, sizeof(buff), "freq_min_c%d", i);
            readFreq(node, buff, out.MinFreq[i]);
            FastSnprintf(buff, sizeof(buff), "freq_max_c%d", i);
            readFreq(node, buff, out.MaxFreq[i]);
        }

        parseGovernorNodes(node, out);

        readFreq(node, "gpu_min_mhz", out.GpuMinFreq);
        readFreq(node, "gpu_max_mhz", out.GpuMaxFreq);
        // 兼容旧嵌套写法 GpuFreq:{min_freq,max_freq}
        try {
            auto& gpu = node["GpuFreq"];
            if (out.GpuMinFreq.empty()) readFreq(gpu, "min_freq", out.GpuMinFreq);
            if (out.GpuMaxFreq.empty()) readFreq(gpu, "max_freq", out.GpuMaxFreq);
        } catch (...) {}

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

    template <class Node>
    void parseGovernorNodes(Node& node, AppProfileModel& out) {
        // qlib::json 不支持迭代 object key，故用白名单枚举调速器参数
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

        // 兼容 "governor":{"c0":"hmbird",...} 嵌套格式，仅当 gov_cN 未读到时补读
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
                    FastSnprintf(buff, sizeof(buff), "freq_min_c%d", i);
                    if (readFreq(oneScene, buff, SceneFreq::MinFreq[s][i])) any = true;
                    FastSnprintf(buff, sizeof(buff), "freq_max_c%d", i);
                    if (readFreq(oneScene, buff, SceneFreq::MaxFreq[s][i])) any = true;

                    // 兼容旧嵌套写法 MinFreq/MaxFreq/governor:{cN:...}
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
        // 热重载时需清簇映射，防止删掉的 cN 键残留旧值
        for (int i = 0; i <= 3; i++) Policy::CpuPolicy[i] = -1;
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

    bool readConfig() {
        resetState();

        ifstream ifs(configPath, std::ios::binary);
        if (!ifs) { logger.Error("无法打开配置文件: %s", configPath); return false; }
        std::string text((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
        int rc = json::parse(&json, text.data(), text.data() + text.size());
        if (rc != 0) { logger.Error("解析 config.json 失败 错误码: %d", rc); return false; }

        // ---- meta ----（loglevel 须在其余节点解析前生效）
        try {
            auto& m = json["meta"];
            readStr (m, "name",     Meta::name);
            readInt (m, "version",  Meta::version);
            readStr (m, "author",   Meta::author);
            readStr (m, "loglevel", Meta::loglevel);
        } catch (...) { logger.Warn("meta 节点缺失"); }
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
        // mode.txt 可能被外部写入非法值，回退 balance 避免守护进程整体失效
        if (mode.empty()) {
            logger.Warn("情景模式为空，回退到 balance");
            mode = "balance";
        }
        if (!switchConfig()) {
            logger.Warn("情景模式无效: '%s'，回退到 balance", mode.c_str());
            mode = "balance";
        }
        logger.Info("当前情景模式: %s", mode.c_str());

        parseBlacklist();
        parseModels();
        applyBaseModel();
        loadPerApp();

        return true;
    }

    // 每行 "包名 画像名"，# 注释，* 开头为全局默认行
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
            char* s = line;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = strlen(s);
            while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
                s[--len] = '\0';
            if (len == 0 || s[0] == '#') continue;
            if (s[0] == '*') {
                char* sp = s + 1;
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp) AppProfile::perAppGlobal = sp;
                continue;
            }
            char* sep = s;
            while (*sep && *sep != ' ' && *sep != '\t') sep++;
            if (!*sep) continue;
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
        // 仅在条数变化时打印，避免 inotify 重载刷屏
        static int lastPerAppCount = -1;
        if (AppProfile::perAppCount != lastPerAppCount) {
            logger.Info("perapp_powermode.txt 已加载: %d 条 App 画像覆盖(优先于 config)",
                        AppProfile::perAppCount);
            lastPerAppCount = AppProfile::perAppCount;
        }
    }

private:
    void parseFunctionSection() {
        try {
            auto& F = json["Function"];

            try {
                auto& c = F["Cpuset"];
                readBool(c, "enable",            Cpuset::enable);
                readStr (c, "top_app",           Cpuset::top_app);
                readStr (c, "foreground",        Cpuset::foreground);
                readStr (c, "background",        Cpuset::background);
                readStr (c, "system_background", Cpuset::system_background);
                readStr (c, "restricted",        Cpuset::restricted);
            } catch (...) { logger.Debug("无 Cpuset 节点"); }

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

            try {
                auto& o = F["OfficialMode"];
                readBool(o, "enable", OfficialMode::enable);
            } catch (...) {}

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

            try {
                auto& g = F["GpuFreq"];
                readBool(g, "enable", GpuFreq::enable);
            } catch (...) {}

            try {
                auto& sc = F["SceneDetect"];
                readBool(sc, "enable",                  SceneCfg::enable);
                readInt (sc, "heavy_load_thd",          SceneCfg::heavy_load_thd);
                readInt (sc, "idle_load_thd",           SceneCfg::idle_load_thd);
                readInt (sc, "heavy_confirm_count",     SceneCfg::heavy_confirm_count);
                readInt (sc, "heavy_max_duration_ms",   SceneCfg::heavy_max_duration_ms);
                readInt (sc, "request_burst_slack_ms",  SceneCfg::request_burst_slack_ms);
                readInt (sc, "burst_delta_thd",         SceneCfg::burst_delta_thd);
                readInt (sc, "burst_min_load",          SceneCfg::burst_min_load);
                readInt (sc, "am_switch_duration_ms",   SceneCfg::am_switch_duration_ms);
                readInt (sc, "touch_duration_ms",       SceneCfg::touch_duration_ms);
                readInt (sc, "load_sample_interval_ms", SceneCfg::load_sample_interval_ms);
                readInt (sc, "screen_poll_interval_ms", SceneCfg::screen_poll_interval_ms);
                readBool(sc, "touch_enable",            SceneCfg::touch_enable);
                readInt (sc, "input_dev_max",           SceneCfg::input_dev_max);
            } catch (...) {}

            try {
                auto& ap = F["AppProfile"];
                readBool(ap, "enable", AppProfile::enable);
            } catch (...) {}

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
            // 仅在条数变化时打印，避免重载刷屏
            static int lastBlacklistCount = -1;
            if (n > 0 && n != lastBlacklistCount) {
                logger.Info("包名黑名单已加载: %d 条", n);
                lastBlacklistCount = n;
            }
        } catch (...) { logger.Debug("无 package_blacklist"); }
    }

    void parseModels() {
        // 首次加载打印详细日志，后续重载降为 Debug 避免刷屏
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

    void applyBaseModel() {
        baseModelIdx = -1;
        for (int i = 0; i < AppProfile::modelCount; i++) {
            // qlib::string operator== 跨堆分配时有 UB，改用 strcmp 走 c_str()
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

        for (int i = 0; i <= 3; i++) {
            int cnt = m.SchedParamCount[i];
            for (int j = 0; j < cnt && j < 23; j++) {
                // schedParam[].Name/Value 1-indexed（历史原因）
                schedParam[i].Name [j+1] = m.SchedParamName [i][j];
                schedParam[i].Value[j+1] = m.SchedParamValue[i][j];
            }
        }
        GpuFreq::min_freq = m.GpuMinFreq;
        GpuFreq::max_freq = m.GpuMaxFreq;
        logger.Info("基础模型: %s (mode=%s)", m.modelName.c_str(), mode.c_str());

        try {
            auto& sceneRoot = json["models"].array()[baseModelIdx]["Scenes"];
            parseScenesNode(sceneRoot);
        } catch (...) { logger.Debug("基础模型无 Scenes 节点"); }
    }
};
