#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include "SceneDetector.hpp"
#include <sys/inotify.h>
#include <sched.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <mutex>

class Schedule {
private:
    static constexpr const char* configPath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* jsonPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* cpusetEventPath = "/dev/cpuset/top-app";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";

    std::vector<thread> threads;
    std::mutex mtx;
    // 串行化画像应用，防并发交错产生混合态；可重入避免同线程嵌套死锁。
    std::recursive_mutex applyMtx;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;
    SceneDetector scene_;

    bool cpuBoost = false;
    std::string lastTopApp;          // 上一次前台应用包名（用于画像切换去重）
    long long   lastGameCpusetFixMs = 0;  // 游戏档 cpuset 矫正节流: 上次矫正时刻(ms), 防 ping-pong

    // 缓存上一次写入的 (min,max,gov)，相同则跳过 sysfs 写，避免抖动场景重复刷盘
    string_t lastMinFreq[4];
    string_t lastMaxFreq[4];
    string_t lastGovernor[4];
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        pinSelfToLittleCores();   // 必须在创建工作线程前设主线程亲和性，新线程继承之
        Init();
        threads.emplace_back(thread(&Schedule::configTriggerTask, this));
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        threads.emplace_back(thread(&Schedule::perappTriggerTask, this));
        threads.emplace_back(thread(&Schedule::cpuSetTriggerTask, this));

        if (Config::AppProfile::enable) {
            threads.emplace_back(thread(&Schedule::appProfileTask, this));
        }
    }

    // qlib::string 跨堆分配比较是 UB，用 strcmp 走 c_str()
    static inline bool str_eq(const string_t& a, const string_t& b) {
        return strcmp(a.c_str(), b.c_str()) == 0;
    }

    void FreqWriter(const int Policy, const string_t MinFreq, const string_t MaxFreq, const string_t Governor) {
        std::lock_guard<std::mutex> lock(mtx);
        int cluster = -1;
        for (int i = 0; i <= 3; i++) {
            if (Config::Policy::CpuPolicy[i] == Policy) { cluster = i; break; }
        }
        if (cluster >= 0 &&
            str_eq(lastMinFreq[cluster],  MinFreq) &&
            str_eq(lastMaxFreq[cluster],  MaxFreq) &&
            str_eq(lastGovernor[cluster], Governor)) {
            return;
        }

        char path[256];
        // affected_cpus 为空说明整簇已 offline，写 governor 会报错，整簇跳过。
        char affPath[256];
        FastSnprintf(affPath, sizeof(affPath),
                     "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", Policy);
        if (!utils.FileStartsWithDigit(affPath)) {
            logger.Debug("CPU簇: %d 无在线核,跳过 governor/频率", Policy);
            // 离线跳过时清缓存，确保核心重新上线后必写（否则被去重永久跳过）
            if (cluster >= 0) {
                lastMinFreq[cluster].clear();
                lastMaxFreq[cluster].clear();
                lastGovernor[cluster].clear();
            }
            return;
        }

        // 先写 governor 再写 min/max：切调速器时内核可能重置频率。
        bool govOk = true;
        if (!Governor.empty()) {
            FastSnprintf(path, sizeof(path), GovernorPath, Policy);
            bool ok = utils.FileWriteBlocking(path, Governor);
            if (!ok) {
                // 配置的调速器不被当前 SoC 支持时，回退到默认（高通 walt / 其他 sugov_ext）
                string_t fb = function.checkQcom() ? "walt" : "sugov_ext";
                if (fb != Governor) {
                    if (utils.FileWriteBlocking(path, fb)) {
                        logger.Warn("CPU簇: %d 调速器 %s 不可用, 已回退 %s",
                                    Policy, Governor.c_str(), fb.c_str());
                        ok = true;
                    }
                }
            }
            if (ok) logger.Info("CPU簇: %d 调速器: %s", Policy, Governor.c_str());
            else    logger.Warn("CPU簇: %d 调速器写入失败: %s (节点不可写或调速器不存在)", Policy, Governor.c_str());
            govOk = ok;
        }

        // 写失败时不更新去重缓存，否则同值重试会被永久跳过
        bool freqOk = true;
        if (!MinFreq.empty()) {
            FastSnprintf(path, sizeof(path), MinFreqPath, Policy);
            if (!utils.FileWriteBlocking(path, MinFreq)) freqOk = false;
            logger.Debug("CPU簇: %d 最小频率: %s", Policy, MinFreq.c_str());
        }

        if (!MaxFreq.empty()) {
            FastSnprintf(path, sizeof(path), MaxFreqPath, Policy);
            if (!utils.FileWriteBlocking(path, MaxFreq)) freqOk = false;
            logger.Debug("CPU簇: %d 最大频率: %s", Policy, MaxFreq.c_str());
        }

        if (cluster >= 0 && govOk && freqOk) {
            lastMinFreq[cluster]  = MinFreq;
            lastMaxFreq[cluster]  = MaxFreq;
            lastGovernor[cluster] = Governor;
        }
    }

    void Boost() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                    LaunchBoost::BoostFreq[i], Performances::CpuGovernor[i]);
            utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
        }
    }

    // 返回 core 所属的 policy（起始核 <= core 的最大 CpuPolicy），找不到返回 -1
    int policyForCore(int core) {
        int best = -1;
        for (int i = 0; i <= 3; i++) {
            int p = Policy::CpuPolicy[i];
            if (p != -1 && p <= core && p > best) best = p;
        }
        return best;
    }

    // CPU 上线是异步 hotplug，写 online=1 后 affected_cpus 可能未更新，轮询等待再写 governor
    void waitClusterReady(int policy, int timeoutMs = 80) {
        if (policy < 0) return;
        char p[256];
        FastSnprintf(p, sizeof(p),
                     "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", policy);
        int waited = 0;
        while (waited < timeoutMs) {
            if (utils.FileStartsWithDigit(p)) return;
            utils.sleep_ms(8);
            waited += 8;
        }
    }

    // scene 参数保留签名兼容性，CPU 永远走基础 mode（场景识别已禁用）
    void applyScene(SceneDetector::Scene scene) {
        (void)scene;
        Release();
    }

    // 优先级：AppProfile > Performances；空字段回退到 Performances；GPU/核心仅在画像中定义时才覆盖
    void applyAppProfile(int modelIdx, SceneDetector::Scene scene) {
        if (modelIdx < 0 || modelIdx >= Config::AppProfile::modelCount) {
            // 无匹配画像，使用场景频率
            applyScene(scene);
            return;
        }

        const auto& model = Config::AppProfile::Models[modelIdx];

        // 先开核心再写 governor/频率，否则离线核收不到 governor 写入
        {
            bool hasCustomOnline = false;
            for (int i = 0; i <= 7; i++) {
                if (model.Online[i] != -1) { hasCustomOnline = true; break; }
            }
            if (hasCustomOnline) {
                char path[256];
                bool broughtOnline = false;
                for (int i = 0; i <= 7; i++) {
                    if (model.Online[i] == -1) continue;
                    FastSnprintf(path, sizeof(path), onlinePath, i);
                    utils.WriteInt(path, model.Online[i]);
                    if (model.Online[i] == 1) broughtOnline = true;
                    logger.Debug("画像核心: %d %s", i, model.Online[i] ? "开启" : "关闭");
                }
                // 等新上线的簇 affected_cpus 稳定后再写频率/调速器，并重落 cpuset
                if (broughtOnline) {
                    for (int i = 0; i <= 7; i++)
                        if (model.Online[i] == 1) waitClusterReady(policyForCore(i));
                    function.cpusetFunction();
                }
            } else {
                // 画像未定义核心开关 → 跟随基础模式，防止省电档的离线核未被拉回
                restoreBaseCoresIfNeeded();
            }
        }

        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;

            // 回退：AppProfile -> Performances
            const string_t& minF = !model.MinFreq[i].empty() ? model.MinFreq[i]
                                                              : Performances::MinFreq[i];
            const string_t& maxF = !model.MaxFreq[i].empty() ? model.MaxFreq[i]
                                                              : Performances::MaxFreq[i];
            // governor 留空 = 跟随系统不写，频率仍回退基础模式
            const string_t& gov  = model.Governor[i];
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
        }

        char spPath[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (model.SchedParamCount[i] == 0) continue;
            // governor 留空则无目标路径，跳过 SchedParam
            const string_t& gov = model.Governor[i];
            if (gov.empty()) continue;
            for (int j = 0; j < model.SchedParamCount[i]; j++) {
                if (model.SchedParamName[i][j].empty()) continue;
                FastSnprintf(spPath, sizeof(spPath), SchedParamPath,
                             Policy::CpuPolicy[i], gov.c_str(),
                             model.SchedParamName[i][j].c_str());
                utils.FileWrite(spPath, model.SchedParamValue[i][j].c_str());
                logger.Debug("画像调速器参数: c%d/%s = %s",
                             i, model.SchedParamName[i][j].c_str(),
                             model.SchedParamValue[i][j].c_str());
            }
        }

        logger.Info("场景画像已应用: %s (isGame=%d)",
                    model.modelName.c_str(),
                    model.isGame ? 1 : 0);
    }

    void Release() {
        // 先上线核心再写 governor/频率，否则离线核收不到 governor 写入
        restoreBaseCoresIfNeeded();

        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], 
                    Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        // Release 必须重写基础 SchedParam，否则 governor tunables 保留游戏画像值导致不降频
        SchedParam();
        applyBaseGpu();
        function.FeasFunc(false);
    }

    // 退出画像时主动还原基础 GPU 频率，否则要等 gpuFreqGuard 最长 15s 才生效
    void applyBaseGpu() {
        if (!Config::GpuFreq::enable) return;
        if (Config::GpuFreq::min_freq.empty() && Config::GpuFreq::max_freq.empty()) return;
        function.gpuFreqControlCustom(Config::GpuFreq::min_freq, Config::GpuFreq::max_freq);
    }

    void Reset() {
        invalidateFreqCache();
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    void invalidateFreqCache() {
        for (int i = 0; i <= 3; i++) {
            lastMinFreq[i].clear();
            lastMaxFreq[i].clear();
            lastGovernor[i].clear();
        }
    }

    void release() {
        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }
        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (cpuBoost) {
            cpuBoost = false;
            if (Config::SceneCfg::enable) {
                if (scene_.triggerAmSwitch()) {
                    applyWithProfile(scene_.current());
                    return;
                }
            } else {
                Boost();
                return;
            }
        }
        if (conf.mode == "fast" && OfficialMode::enable) {
            Reset();
            return;
        }
        applyWithProfile(scene_.current());
    }

    // 统一频率应用入口：有匹配画像则用画像，否则用基础 mode
    void applyWithProfile(SceneDetector::Scene scene) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        int matchIdx = Config::AppProfile::currentMatch.load();
        if (Config::AppProfile::enable && matchIdx >= 0) {
            applyAppProfile(matchIdx, scene);
        } else {
            Release();
            restoreBaseCoresIfNeeded();
        }
    }

    void restoreBaseCoresIfNeeded() {
        bool anyDefined = false;
        bool anyOnline  = false;
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] != -1) anyDefined = true;
            if (Performances::Online[i] == 1)  anyOnline  = true;
        }
        if (anyDefined) {
            online();
            // 等新上线的簇稳定后再写 governor，hotplug 异步，内核不自动还原 cpuset
            for (int i = 0; i <= 7; i++)
                if (Performances::Online[i] == 1) waitClusterReady(policyForCore(i));
            if (anyOnline) function.cpusetFunction();
        }
    }

    void online() {
        char path[256];
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] == -1) continue; // -1 = 跳过，不误关核心
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    void SchedParam() {
        char path[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            for (int j = 1; j <= 16; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(path, sizeof(path), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                utils.FileWrite(path, conf.schedParam[i].Value[j].c_str());
                logger.Debug("CPU簇: %d 调速器参数: %s 值: %s", Policy::CpuPolicy[i], conf.schedParam[i].Name[j].c_str(), conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    void applyAllConfig() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        release();
        SchedParam();
        online();
        function.gpuFreqControl();
    }

    // 监控 perapp_powermode.txt 变化，重载后立即对当前前台重算画像
    void perappTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "perapp_powermode.txt";
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            conf.loadPerApp();
            // perapp 改变后包名不变但画像可能变，必须强制重算（绕过 pkg==lastTopApp 去重）
            logger.Info("perapp_powermode.txt 已重载 (%d 条映射), 重算当前前台画像",
                        Config::AppProfile::perAppCount);
            forceReevalForeground();
        }
    }

    void forceReevalForeground() {
        lastTopApp.clear();
        evalForegroundOnce(true);
    }

    // 返回文件内容哈希，用于去重：外部轮询可能反复回写相同内容触发 IN_CLOSE_WRITE，内容未变则跳过重载
    size_t fileContentHash(const char* path) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        std::string data;
        char tmp[4096];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0) data.append(tmp, (size_t)n);
        close(fd);
        if (data.empty()) return 0;
        return std::hash<std::string>{}(data);
    }

    void configTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "mode.txt";
        const char* modeFilePath = "/sdcard/Android/CTS/mode.txt";
        size_t lastHash = 0;
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            size_t h = fileContentHash(modeFilePath);
            if (h != 0 && h == lastHash) {
                logger.Debug("mode.txt 内容未变,跳过重载");
                continue;
            }
            if (conf.readConfig()) {
                applyAllConfig();
                lastHash = h;
            } else {
                logger.Warn("配置重载失败，跳过应用");
            }
        }
    }

    void jsonTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "config.json";
        const char* jsonFilePath = "/sdcard/Android/CTS/config.json";
        size_t lastHash = 0;
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            size_t h = fileContentHash(jsonFilePath);
            if (h != 0 && h == lastHash) {
                logger.Debug("config.json 内容未变,跳过重载");
                continue;
            }
            if (conf.readConfig()) {
                function.ReloadFunC();
                applyAllConfig();
                lastHash = h;
            } else {
                logger.Warn("JSON 配置重载失败，跳过应用");
            }
        }
    }

    // /sdcard/ 是 FUSE，inotify 不能监控文件，只能监控目录再过滤文件名
    bool watchFileInDir(const char* dirPath, const char* targetFile, uint32_t mask) {
        int fd = inotify_init();
        if (fd < 0) {
            logger.Error("inotify_init 失败: %s", strerror(errno));
            return false;
        }
        int wd = inotify_add_watch(fd, dirPath, mask | IN_MOVED_TO);
        if (wd < 0) {
            logger.Error("inotify_add_watch 失败 %s: %s", dirPath, strerror(errno));
            close(fd);
            return false;
        }

        constexpr int buflen = sizeof(struct inotify_event) + NAME_MAX + 1;
        char buf[buflen];
        fd_set readfds;
        bool triggered = false;
        while (!triggered) {
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            struct timeval tv = {5, 0}; // 5s timeout to re-create watch
            int ret = select(fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue; // timeout, re-create watch
            int len = read(fd, buf, buflen);
            if (len < 0) break;
            for (char* ptr = buf; ptr < buf + len; ) {
                struct inotify_event* ev = reinterpret_cast<struct inotify_event*>(ptr);
                if (ev->len > 0 && strcmp(ev->name, targetFile) == 0) {
                    triggered = true; break;
                }
                ptr += sizeof(struct inotify_event) + ev->len;
            }
        }
        inotify_rm_watch(fd, wd);
        close(fd);
        return triggered;
    }

    void cpuSetTriggerTask() {
        return;
    }

    // 阻塞等待 top-app cpuset 变动事件（前台切换会改写该 cgroup），超时返回 0，inotify 失败返回 -1
    int waitTopAppEvent(int inotifyFd, int wd, int maxWaitMs) {
        (void)wd;
        if (inotifyFd < 0) return -1;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(inotifyFd, &rfds);
        struct timeval tv;
        tv.tv_sec  = maxWaitMs / 1000;
        tv.tv_usec = (maxWaitMs % 1000) * 1000;
        int ret = select(inotifyFd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) return (errno == EINTR) ? 0 : -1;
        if (ret == 0) return 0;
        char buf[4096];
        ssize_t n = read(inotifyFd, buf, sizeof(buf));
        (void)n;
        return 1;
    }


    // 守护进程绑定低位核 0-3，避免抢占游戏超大核；主线程设亲和性后新线程继承（参考 uperf 最佳实践）
    void pinSelfToLittleCores() {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c <= 3; c++) CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0)
            logger.Info("守护进程已绑定低位核 0-3(避开超大核 6-7), 不抢占前台游戏");
        else
            logger.Debug("绑定低位核失败 errno=%d(忽略, 不影响功能)", errno);
    }



    bool readNodeTrim(const char* p, char* out, size_t cap) {
        out[0] = '\0';
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        ssize_t n = read(fd, out, cap - 1);
        close(fd);
        if (n <= 0) { out[0] = '\0'; return false; }
        out[n] = '\0';
        for (ssize_t i = n - 1; i >= 0 &&
             (out[i]=='\n'||out[i]=='\r'||out[i]==' '||out[i]=='\t'); --i) out[i] = '\0';
        return true;
    }

    // 游戏档下 ORMS 常把 top-app cpuset 收窄（踢掉 6-7），事件驱动+幂等+节流矫正回全核
    void correctGameTopAppCpus() {
        int cur = Config::AppProfile::currentMatch.load();
        if (cur < 0 || cur >= Config::AppProfile::modelCount
            || !Config::AppProfile::Models[cur].isGame) return;

        // 目标全核范围: 优先用配置的 top_app(游戏档应为 0-7), 否则用当前全部在线 CPU。
        char desired[64] = { 0 };
        if (Config::Cpuset::enable && !Config::Cpuset::top_app.empty())
            snprintf(desired, sizeof(desired), "%s", Config::Cpuset::top_app.c_str());
        else if (!readNodeTrim("/sys/devices/system/cpu/online", desired, sizeof(desired)))
            return;
        if (!desired[0]) return;

        char now[64] = { 0 };
        readNodeTrim("/dev/cpuset/top-app/cpus", now, sizeof(now));
        if (strcmp(now, desired) == 0) return;   // 已是全核 → 幂等不写

        constexpr long long kFixMinMs = 1000;    // 节流：最多每秒矫正一次，防 ping-pong
        long long t = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (t - lastGameCpusetFixMs < kFixMinMs) return;
        lastGameCpusetFixMs = t;

        // cpuset 子集 ⊆ 父集：先放开 root，再写 top-app/foreground
        utils.FileWrite("/dev/cpuset/cpus", desired);
        utils.FileWrite("/dev/cpuset/top-app/cpus", desired);
        utils.FileWrite("/dev/cpuset/foreground/cpus", desired);
        logger.Info("游戏档: top-app cpuset 被收窄(%s) → 矫正回 %s", now, desired);
    }

    // fresh=true: 强制取最新前台（事件驱动路径用，避免 TTL 缓存返回旧包名误判未切换）
    // fresh=false: 允许用缓存（空闲复查/低频路径，省电）
    void evalForegroundOnce(bool fresh) {
        // 串行化"取前台 + 改 lastTopApp/currentMatch + 应用"，防两线程并发写 sysfs
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        std::string pkg = fresh ? utils.getTopApp() : utils.getTopAppCached(2500);
        if (pkg.empty() || pkg == "null") return;

        // 黑名单前台（launcher/systemui/输入法等）只是临时覆盖，忽略以防误判离开游戏
        if (Config::AppProfile::isBlacklisted(pkg.c_str())) return;

        if (pkg == lastTopApp) return;

        // 游戏画像下新获焦是非游戏 App 但游戏窗口仍可见（小窗覆盖）→ 保持游戏画像不切
        // 主信号用 Task visible 而非窗口模式（ColorOS 小窗报 fullscreen，窗口模式判不出）
        {
            int curMatch = Config::AppProfile::currentMatch.load();
            if (curMatch >= 0 && curMatch < Config::AppProfile::modelCount
                && Config::AppProfile::Models[curMatch].isGame && !lastTopApp.empty()) {
                int nm = Config::AppProfile::findMatchingModel(pkg.c_str());
                bool newIsGame = (nm >= 0 && Config::AppProfile::Models[nm].isGame);
                if (!newIsGame &&
                    (utils.isPackageInTopApp(lastTopApp.c_str()) || utils.isPackageVisible(lastTopApp.c_str()))) {
                    logger.Debug("小窗覆盖游戏(前台=%s, 游戏仍可见) → 保持游戏画像", pkg.c_str());
                    return;
                }
            }
        }

        lastTopApp = pkg;
        int newMatch = Config::AppProfile::findMatchingModel(pkg.c_str());

        // 去重：新匹配画像与当前相同则跳过，不重复刷 sysfs
        int oldMatch = Config::AppProfile::currentMatch.load();
        if (newMatch != oldMatch) {
            Config::AppProfile::currentMatch.store(newMatch);
            if (newMatch >= 0) {
                const auto& mdl = Config::AppProfile::Models[newMatch];
                logger.Info("前台: %s → 档[%s]%s", pkg.c_str(),
                            mdl.modelName.c_str(), mdl.isGame ? " (游戏)" : "");
            } else {
                logger.Info("前台: %s → 默认档(无匹配画像)", pkg.c_str());
            }
            applyWithProfile(scene_.current());
        }
    }

    void appProfileTask() {
        sleep(5); // 等待系统启动完成

        // 事件驱动+尾沿防抖：监听 top-app cpuset 变动替代轮询，重操作等前台"安静"kQuietMs 后一次写入
        // 快速连切时持续顺延，不写入风暴；inotify 不可用时退化为轮询
        constexpr int kPollIdleMs  = 4000;  // 回退轮询 / inotify 空闲复查间隔
        constexpr int kQuietMs     = 300;   // 安静窗口：无新切换事件持续此时长视为已落定
        constexpr int kMaxDeferMs  = 1200;  // 硬上限：持续事件也保证最终生效，不无限顺延

        int fd = inotify_init1(IN_CLOEXEC);
        int wd = -1;
        if (fd >= 0) {
            // 排除 IN_ACCESS：读 top-app/cpus 会触发 IN_ACCESS 造成自诱发 busy-loop
            wd = inotify_add_watch(fd, cpusetEventPath,
                                   IN_MODIFY | IN_ATTRIB | IN_MOVED_TO | IN_CREATE);
            if (wd < 0) { close(fd); fd = -1; }
        }
        logger.Info(fd >= 0
            ? "场景画像监控线程已启动 (事件驱动: 监听 top-app 切换, 支持通配符匹配)"
            : "场景画像监控线程已启动 (inotify 不可用, 回退轮询; 支持通配符匹配)");

        evalForegroundOnce(true);

        while (true) {
            if (scene_.current() == SceneDetector::Scene::Standby) {
                utils.sleep_ms(8000);   // 熄屏省电，降低复查频率
                continue;
            }

            int ev = waitTopAppEvent(fd, wd, kPollIdleMs);
            if (ev < 0) {
                utils.sleep_ms(kPollIdleMs);
                evalForegroundOnce(true);
                correctGameTopAppCpus();
                continue;
            }
            if (ev == 0) {
                evalForegroundOnce(false);
                correctGameTopAppCpus();
                continue;
            }

            // 尾沿防抖：等前台安静再做重操作，避免连切时频率写风暴
            int deferred = 0;
            while (deferred < kMaxDeferMs) {
                int more = waitTopAppEvent(fd, wd, kQuietMs);
                if (more <= 0) break;
                deferred += kQuietMs;
            }
            evalForegroundOnce(true);
            correctGameTopAppCpus();
        }
    }

    void Init() {
        const int myPid = getpid();
        const char* checkNames[] = {"CoreTurboScheduler", "CpuTurboScheduler", "MW_CpuSpeedController"};

        // Step 1: Graceful termination (SIGTERM) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGTERM);
                        logger.Debug("发送 SIGTERM 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow graceful shutdown

        // Step 2: Force kill (SIGKILL) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGKILL);
                        logger.Debug("发送 SIGKILL 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow force kill to take effect

        // Step 3: Final check -- if any other instance still running, exit
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        logger.Error("CTS调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出", name, pid, myPid);
                        printf("\n!!! \n!!! CTS调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出 \n!!!\n\n", name, pid, myPid);
                        exit(-1);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }

        logger.clear_log();
        bool configOk = conf.readConfig();
        logger.Info("名称: %s",       Meta::name.c_str());
        logger.Info("版本: %d",       Meta::version);
        logger.Info("作者: %s",       Meta::author.c_str());
        logger.Info("日志等级: %s",   Meta::loglevel.c_str());
        function.AllFunC();
        if (configOk) {
            release();
            online();
            SchedParam();
            function.gpuFreqControl();
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }
    }
};
