#pragma once
#include <sys/eventfd.h>
#include "LibUtils.hpp"
#include "Function.hpp"
#include <sys/inotify.h>
#include <sys/resource.h>
#include <poll.h>
#include <sched.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>

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

    // 优雅退出：析构置位 + 写 eventfd 唤醒阻塞的 poll/select，cv 打断定时等待
    std::atomic<bool> shouldExit_{false};
    int stopEventFd_ = -1;
    std::mutex stopMtx_;
    std::condition_variable stopCv_;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    std::string lastTopApp;          // 上一次前台应用包名（用于画像切换去重）
    long long   lastGameCpusetFixMs = 0;  // 游戏档 cpuset 矫正节流: 上次矫正时刻(ms), 防 ping-pong
    std::string heldPkg;             // 正在保活的游戏/保活画像包名(进程存活则维持), 空=未保活

    // App 无障碍服务(AppSwitchService)推送的前台包名 + 接收时刻; 新鲜则优先于 dumpsys, 省 popen。
    // 服务未启用/被杀 → 推送变陈旧 → 自动回退 dumpsys, 不影响"不开 App 也能用"。
    std::mutex  pushMtx_;
    std::string pushedTopApp_;
    std::atomic<long long> lastPushMs_{0};
    static constexpr long long kPushTtlMs = 15000;  // 推送有效期(ms): App 端每 ~10s 心跳重推
    static constexpr const char* topAppPath = "/sdcard/Android/CTS/topapp.txt";

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 缓存上一次写入的 (min,max,gov)，相同则跳过 sysfs 写，避免抖动场景重复刷盘
    string_t lastMinFreq[Config::kClusterCount];
    string_t lastMaxFreq[Config::kClusterCount];
    string_t lastGovernor[Config::kClusterCount];
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        pinSelfToLittleCores();   // 必须在创建工作线程前设主线程亲和性，新线程继承之
        Init();
        stopEventFd_ = eventfd(0, EFD_CLOEXEC);
        // 单 inotify 线程监控三个配置文件，替代原 config/json/perapp 三线程
        threads.emplace_back(thread(&Schedule::fileWatchTask, this));

        if (Config::AppProfile::enable) {
            threads.emplace_back(thread(&Schedule::appProfileTask, this));
        }
        // 开机后多次重应用, 覆盖 ROM 晚启动的开机提频(否则要手动切模式才正常)
        threads.emplace_back(thread(&Schedule::bootResettleTask, this));
        // 实时监测 CPU 状态, 被 ROM/perf 改写则写回配置
        threads.emplace_back(thread(&Schedule::cpuEnforceTask, this));
    }

    // 同 Function::stopGuards 风格：置位 → 唤醒(cv + eventfd) → join，成员销毁前线程已全部退出
    ~Schedule() {
        {
            std::lock_guard<std::mutex> lk(stopMtx_);
            shouldExit_.store(true);
        }
        stopCv_.notify_all();
        if (stopEventFd_ >= 0) {
            eventfd_write(stopEventFd_, 1);
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        if (stopEventFd_ >= 0) {
            close(stopEventFd_);
            stopEventFd_ = -1;
        }
    }

    // 可被析构即时打断的睡眠；返回 true 表示收到停止请求
    bool waitStop(int ms) {
        std::unique_lock<std::mutex> lk(stopMtx_);
        return stopCv_.wait_for(lk, std::chrono::milliseconds(ms),
                                [this] { return shouldExit_.load(); });
    }

    // 开机重应用：ROM 的 perf HAL / 开机提频晚于本守护启动, 会把我们应用的频率/调速器顶高,
    // 而去重缓存挡住自愈 → 卡高频, 要手动切模式才好。开机后分几次强制重应用(applyAllConfig
    // 内含 invalidateFreqCache), 等 ROM 稳定。可被析构打断。
    void bootResettleTask() {
        const int delaysMs[] = { 8000, 20000, 45000 };
        for (int d : delaysMs) {
            if (waitStop(d)) return;
            logger.Debug("开机重应用: 覆盖 ROM 开机提频, 强制重应用当前配置");
            applyAllConfig();
        }
    }

    // 必须 size 感知：clear() 只置 _size=0 不动缓冲区，仅比 c_str() 会把"已清空"误判为
    // 等于旧值，导致 invalidateFreqCache()/漂移纠正后同值重写被去重跳过（"降不回/不生效"根因）。
    static inline bool str_eq(const string_t& a, const string_t& b) {
        return a.size() == b.size() && strcmp(a.c_str(), b.c_str()) == 0;
    }

    // 传引用避免每次调用 3 个 string_t 堆拷贝
    void FreqWriter(const int Policy, const string_t& MinFreq, const string_t& MaxFreq, const string_t& Governor) {
        std::lock_guard<std::mutex> lock(mtx);
        int cluster = -1;
        for (int i = 0; i < kClusterCount; i++) {
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

    // 非游戏档 governor 兜底：画像留空 → 基础模式 → SoC 默认(高通 conservative / 其他 sugov_ext)。
    // 防外部(如 Scene)把 config 里的 governor 清空后, 该簇被 daemon 放手、随 ROM 漂移到 walt。
    // 游戏档不走此兜底：留空表示交给 ROM(风驰/hmbird)。
    string_t resolveGovernor(int clusterIdx, const string_t& want) {
        if (!want.empty()) return want;
        if (!Performances::CpuGovernor[clusterIdx].empty())
            return Performances::CpuGovernor[clusterIdx];
        return function.checkQcom() ? "conservative" : "sugov_ext";
    }

    // 返回 core 所属的 policy（起始核 <= core 的最大 CpuPolicy），找不到返回 -1
    int policyForCore(int core) {
        int best = -1;
        for (int i = 0; i < kClusterCount; i++) {
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

    // 优先级：AppProfile > Performances；空字段回退到 Performances；GPU/核心仅在画像中定义时才覆盖
    void applyAppProfile(int modelIdx) {
        if (modelIdx < 0 || modelIdx >= Config::AppProfile::modelCount) {
            // 无匹配画像，回到基础 mode
            applyBaseMode();
            return;
        }

        const auto& model = Config::AppProfile::Models[modelIdx];

        // 先开核心再写 governor/频率，否则离线核收不到 governor 写入
        {
            bool hasCustomOnline = false;
            for (int i = 0; i < kCoreCount; i++) {
                if (model.Online[i] != -1) { hasCustomOnline = true; break; }
            }
            if (hasCustomOnline) {
                char path[256];
                bool broughtOnline = false;
                for (int i = 0; i < kCoreCount; i++) {
                    if (model.Online[i] == -1) continue;
                    FastSnprintf(path, sizeof(path), onlinePath, i);
                    utils.WriteInt(path, model.Online[i]);
                    if (model.Online[i] == 1) broughtOnline = true;
                    logger.Debug("画像核心: %d %s", i, model.Online[i] ? "开启" : "关闭");
                }
                // 等新上线的簇 affected_cpus 稳定后再写频率/调速器，并重落 cpuset
                if (broughtOnline) {
                    for (int i = 0; i < kCoreCount; i++)
                        if (model.Online[i] == 1) waitClusterReady(policyForCore(i));
                    function.cpusetFunction();
                }
            } else {
                // 画像未定义核心开关 → 跟随基础模式，防止省电档的离线核未被拉回
                restoreBaseCoresIfNeeded();
            }
        }

        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;

            // 回退：AppProfile -> Performances
            const string_t& minF = !model.MinFreq[i].empty() ? model.MinFreq[i]
                                                              : Performances::MinFreq[i];
            const string_t& maxF = !model.MaxFreq[i].empty() ? model.MaxFreq[i]
                                                              : Performances::MaxFreq[i];
            // governor: 游戏档留空=交给 ROM(风驰); 非游戏档留空则兜底基础模式/默认,
            // 避免被外部(如 Scene)清空后该簇随 ROM 漂移到 walt(用户反馈"一个co一个walt")
            const string_t gov = model.isGame ? model.Governor[i]
                                              : resolveGovernor(i, model.Governor[i]);
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
            // 守护快照同步为画像目标: 否则 gpuFreqGuard 仍按基础值判"漂移", ~1s 内把
            // 画像 GPU 频率改回基础值(画像 GPU 设置形同虚设); 同步后守护反而保护画像值。
            GpuFreq::min_mhz.store(Fastatoi(gpuMin.c_str()));
            GpuFreq::max_mhz.store(Fastatoi(gpuMax.c_str()));
        } else {
            // 画像未定义 GPU → 跟随基础档, 防止上一画像的 GPU 频率/快照残留
            applyBaseGpu();
        }

        char spPath[256];
        for (int i = 0; i < kClusterCount; i++) {
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
                // 阻塞写：governor 类节点用 O_NONBLOCK 会 EAGAIN 静默失败(同 scaling_governor)
                utils.FileWriteBlocking(spPath, model.SchedParamValue[i][j]);
                logger.Debug("画像调速器参数: c%d/%s = %s",
                             i, model.SchedParamName[i][j].c_str(),
                             model.SchedParamValue[i][j].c_str());
            }
        }

        logger.Info("场景画像已应用: %s (isGame=%d)",
                    model.modelName.c_str(),
                    model.isGame ? 1 : 0);
    }

    void applyBaseMode() {
        // 先上线核心再写 governor/频率，否则离线核收不到 governor 写入
        restoreBaseCoresIfNeeded();

        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            // 基础模式(powersave/balance/...)均非游戏: governor 留空也兜底, 不交给 ROM
            const string_t gov = resolveGovernor(i, Performances::CpuGovernor[i]);
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                    Performances::MaxFreq[i], gov);
        }
        // applyBaseMode 必须重写基础 SchedParam，否则 governor tunables 保留游戏画像值导致不降频
        SchedParam();
        applyBaseGpu();
        function.FeasFunc(false);
    }

    // 退出画像时主动还原基础 GPU 频率，否则要等 gpuFreqGuard 最长 15s 才生效
    void applyBaseGpu() {
        if (!Config::GpuFreq::enable) return;
        // 画像可能改写过守护快照, 回基础档必须还原(基础未配 GPU 时发布 0/0 让守护停手)
        JsonConfig::publishGpuSnapshot();
        if (Config::GpuFreq::min_freq.empty() && Config::GpuFreq::max_freq.empty()) return;
        function.gpuFreqControlCustom(Config::GpuFreq::min_freq, Config::GpuFreq::max_freq);
    }

    void Reset() {
        invalidateFreqCache();
        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    void invalidateFreqCache() {
        for (int i = 0; i < kClusterCount; i++) {
            lastMinFreq[i].clear();
            lastMaxFreq[i].clear();
            lastGovernor[i].clear();
        }
    }

    void applyCurrentMode() {
        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }
        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (conf.mode == "fast" && OfficialMode::enable) {
            Reset();
            return;
        }
        applyWithProfile();
    }

    // 统一频率应用入口：有匹配画像则用画像，否则用基础 mode
    void applyWithProfile() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        int matchIdx = Config::AppProfile::currentMatch.load();
        if (Config::AppProfile::enable && matchIdx >= 0) {
            applyAppProfile(matchIdx);
        } else {
            applyBaseMode(); // 内部已先 restoreBaseCoresIfNeeded, 不再重复
        }
    }

    void restoreBaseCoresIfNeeded() {
        bool anyDefined = false;
        bool anyOnline  = false;
        for (int i = 0; i < kCoreCount; i++) {
            if (Performances::Online[i] != -1) anyDefined = true;
            if (Performances::Online[i] == 1)  anyOnline  = true;
        }
        if (anyDefined) {
            online();
            // 等新上线的簇稳定后再写 governor，hotplug 异步，内核不自动还原 cpuset
            for (int i = 0; i < kCoreCount; i++)
                if (Performances::Online[i] == 1) waitClusterReady(policyForCore(i));
            if (anyOnline) function.cpusetFunction();
        }
    }

    void online() {
        char path[256];
        for (int i = 0; i < kCoreCount; i++) {
            if (Performances::Online[i] == -1) continue; // -1 = 跳过，不误关核心
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    void SchedParam() {
        char path[256];
        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            for (int j = 0; j < kMaxSchedParams; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(path, sizeof(path), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                // 阻塞写：governor 类节点用 O_NONBLOCK 会 EAGAIN 静默失败(同 scaling_governor)
                utils.FileWriteBlocking(path, conf.schedParam[i].Value[j]);
                logger.Debug("CPU簇: %d 调速器参数: %s 值: %s", Policy::CpuPolicy[i], conf.schedParam[i].Name[j].c_str(), conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    void applyAllConfig() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        // 配置变更(改频率/换模式/恢复默认)必清去重缓存: 否则若 sysfs 已被外部(如提频)抬高、
        // 而缓存仍是旧低值, 恢复同值会被判"未变"跳过 → 高频留住降不回 ("恢复配置也不生效")。
        invalidateFreqCache();
        applyCurrentMode();
        SchedParam();
        online();
        function.gpuFreqControl();
        if (!Config::AppProfile::enable) return;
        // 保活中且游戏进程仍在 → 重应用游戏画像(用新配置), 不因重载切到当前前台:
        // 防 Scene/编辑配置触发的重载在开着小窗时把游戏档丢掉。否则清保活按当前前台重评。
        if (!heldPkg.empty() && utils.isPackageRunning(heldPkg.c_str())) {
            int m = Config::AppProfile::findMatchingModel(heldPkg.c_str());
            if (m >= 0) {
                Config::AppProfile::currentMatch.store(m);
                applyWithProfile();
                return;
            }
        }
        heldPkg.clear();
        forceReevalForeground();
    }

    // perapp 改变后包名不变但画像可能变，必须强制重算（绕过 pkg==lastTopApp 去重）
    void onPerappChanged() {
        // 持 applyMtx 改写全局画像表，与 applyAppProfile/applyBaseMode 的并发读互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        conf.loadPerApp();
        logger.Info("perapp_powermode.txt 已重载 (%d 条映射), 重算当前前台画像",
                    Config::AppProfile::perAppCount);
        invalidateFreqCache(); // sysfs 可能被外部改过, 同值切档不能被去重跳过
        heldPkg.clear();       // 保活包的映射可能已变, 重评后按需重建
        forceReevalForeground();
    }

    void forceReevalForeground() {
        // lastTopApp 与前台线程共享, 必须持 applyMtx(可重入, 调用方已持锁也安全)
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
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

    void onModeChanged(size_t& lastHash) {
        size_t h = fileContentHash(configPath);
        if (h != 0 && h == lastHash) {
            logger.Debug("mode.txt 内容未变,跳过重载");
            return;
        }
        // readConfig 改写 Performances/AppProfile 等全局 string，必须与画像应用互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (conf.readConfig()) {
            applyAllConfig();
            lastHash = h;
        } else {
            logger.Warn("配置重载失败，跳过应用");
        }
    }

    void onJsonChanged(size_t& lastHash) {
        size_t h = fileContentHash(jsonPath);
        if (h != 0 && h == lastHash) {
            logger.Debug("config.json 内容未变,跳过重载");
            return;
        }
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (conf.readConfig()) {
            function.ReloadFunC();
            applyAllConfig();
            lastHash = h;
        } else {
            logger.Warn("JSON 配置重载失败，跳过应用");
        }
    }

    // 单 inotify 实例 + 阻塞 read 监控目录下三个配置文件（合并原三线程，消除 5s select 定时唤醒）
    // /sdcard/ 是 FUSE，inotify 不能监控文件，只能监控目录再过滤文件名
    void fileWatchTask() {
        if (waitStop(2000)) return;
        const char* watchDir = "/sdcard/Android/CTS";
        size_t lastModeHash = 0, lastJsonHash = 0;
        while (!shouldExit_.load()) {
            int fd = inotify_init1(IN_CLOEXEC);
            if (fd < 0) {
                logger.Error("inotify_init 失败: %s", strerror(errno));
                if (waitStop(5000)) break;
                continue;
            }
            int wd = inotify_add_watch(fd, watchDir, IN_CLOSE_WRITE | IN_MOVED_TO);
            if (wd < 0) {
                logger.Error("inotify_add_watch 失败 %s: %s", watchDir, strerror(errno));
                close(fd);
                if (waitStop(5000)) break;
                continue;
            }

            bool alive = true;
            alignas(struct inotify_event) char buf[4096];
            while (alive && !shouldExit_.load()) {
                // poll 同时等 inotify 与停止 eventfd，析构写 eventfd 即可打断阻塞
                struct pollfd pfds[2] = {
                    { fd,           POLLIN, 0 },
                    { stopEventFd_, POLLIN, 0 },
                };
                nfds_t nfds = (stopEventFd_ >= 0) ? 2 : 1;
                int pr = poll(pfds, nfds, (stopEventFd_ >= 0) ? -1 : 1000);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    alive = false;
                    break;
                }
                if (pr == 0) continue;  // 无 eventfd 时的兜底超时，回头查停止标志
                if (nfds == 2 && (pfds[1].revents & POLLIN)) break;  // 停止请求
                if (!(pfds[0].revents & POLLIN)) {
                    if (pfds[0].revents) alive = false;  // 错误/挂起，重建 inotify
                    continue;
                }
                ssize_t len = read(fd, buf, sizeof(buf));
                if (len <= 0) {
                    if (len < 0 && errno == EINTR) continue;
                    alive = false;
                    break;
                }
                for (char* ptr = buf; ptr < buf + len; ) {
                    auto* ev = reinterpret_cast<struct inotify_event*>(ptr);
                    ptr += sizeof(struct inotify_event) + ev->len;
                    if (ev->mask & (IN_IGNORED | IN_UNMOUNT)) { alive = false; continue; } // watch 失效，重建
                    if (ev->len == 0) continue;
                    if      (strcmp(ev->name, "mode.txt") == 0)             onModeChanged(lastModeHash);
                    else if (strcmp(ev->name, "config.json") == 0)          onJsonChanged(lastJsonHash);
                    else if (strcmp(ev->name, "perapp_powermode.txt") == 0) onPerappChanged();
                    else if (strcmp(ev->name, "topapp.txt") == 0)           onTopAppChanged();
                }
            }
            inotify_rm_watch(fd, wd);
            close(fd);
        }
    }

    // 阻塞等待 top-app cpuset 变动事件（前台切换会改写该 cgroup），超时返回 0，inotify 失败返回 -1
    int waitTopAppEvent(int inotifyFd, int wd, int maxWaitMs) {
        (void)wd;
        if (inotifyFd < 0) return -1;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(inotifyFd, &rfds);
        int maxFd = inotifyFd;
        // 一并监听停止 eventfd，析构即时打断等待（不消费，外层查 shouldExit_ 退出）
        if (stopEventFd_ >= 0) {
            FD_SET(stopEventFd_, &rfds);
            if (stopEventFd_ > maxFd) maxFd = stopEventFd_;
        }
        struct timeval tv;
        tv.tv_sec  = maxWaitMs / 1000;
        tv.tv_usec = (maxWaitMs % 1000) * 1000;
        int ret = select(maxFd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) return (errno == EINTR) ? 0 : -1;
        if (ret == 0) return 0;
        if (stopEventFd_ >= 0 && FD_ISSET(stopEventFd_, &rfds)) return 0;
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
        // 后台优先级：自身工作均为微秒级 sysfs 写，nice 10 不影响生效时机但不与前台争时间片
        setpriority(PRIO_PROCESS, 0, 10);
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
    // 实时回读各簇 governor/min/max, 与上次写入值(配置目标)不符即清缓存强制重应用,
    // 对抗 ROM/perf HAL 运行时改写。返回是否检测到漂移并已写回。
    bool enforceState() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        char path[256], cur[64];
        bool drift = false;
        for (int i = 0; i < kClusterCount && !drift; i++) {
            int p = Config::Policy::CpuPolicy[i];
            if (p == -1) continue;
            if (!lastGovernor[i].empty()) {
                FastSnprintf(path, sizeof(path), GovernorPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastGovernor[i].c_str())) drift = true;
            }
            if (!drift && !lastMinFreq[i].empty()) {
                FastSnprintf(path, sizeof(path), MinFreqPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastMinFreq[i].c_str())) drift = true;
            }
            if (!drift && !lastMaxFreq[i].empty()) {
                FastSnprintf(path, sizeof(path), MaxFreqPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastMaxFreq[i].c_str())) drift = true;
            }
        }
        if (drift) {
            logger.Debug("CPU 状态被外部改写 → 强制写回配置");
            invalidateFreqCache();
            applyWithProfile();
        }
        return drift;
    }

    // 实时监测线程: 周期回读纠正; 稳定久了退避省电, 一旦被改写立即回 2s 紧盯
    void cpuEnforceTask() {
        if (waitStop(6000)) return;   // 等启动稳定
        int interval = 2000, stable = 0;
        while (!shouldExit_.load()) {
            if (enforceState()) { interval = 2000; stable = 0; }
            else if (++stable >= 5) interval = 5000;
            if (waitStop(interval)) break;
        }
    }

    void correctGameTopAppCpus() {
        // 读 Models/Cpuset 全局 string，与配置重载互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
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
        long long t = nowMs();
        if (t - lastGameCpusetFixMs < kFixMinMs) return;
        lastGameCpusetFixMs = t;

        // cpuset 子集 ⊆ 父集：先放开 root，再写 top-app/foreground
        utils.FileWrite("/dev/cpuset/cpus", desired);
        utils.FileWrite("/dev/cpuset/top-app/cpus", desired);
        utils.FileWrite("/dev/cpuset/foreground/cpus", desired);
        logger.Debug("游戏档: top-app cpuset 被收窄(%s) → 矫正回 %s", now, desired);
    }

    // 取前台包名(黑名单感知): 多窗口/小窗下 dumpsys 会列出多个 type=standard visible=true,
    // 顶层可能是小窗工具(如 MT管理器)。这里在可见标准 Task 里跳过黑名单, 取最顶的"非黑名单"应用,
    // 对齐 Scene 的忽略名单行为, 从源头修正"小窗抢占检测"。全为黑名单/快照无效时回退三级链。
    std::string getForegroundPkg(bool fresh) {
        // ① App 无障碍服务推送(新鲜)优先: 普通切换直接用, 省去 dumpsys popen
        {
            std::lock_guard<std::mutex> lk(pushMtx_);
            if (!pushedTopApp_.empty() && nowMs() - lastPushMs_.load() < kPushTtlMs) {
                if (!Config::AppProfile::isBlacklisted(pushedTopApp_.c_str()))
                    return pushedTopApp_;
                // 推送的是黑名单(小窗工具/launcher) → 落 dumpsys 从可见栈里捞主应用
            }
        }
        // ② dumpsys 黑名单感知
        Utils::ForegroundSnapshot s = utils.getForegroundCached(fresh ? 0 : 2500);
        if (s.valid) {
            for (const auto& p : s.standardVisible)
                if (!Config::AppProfile::isBlacklisted(p.c_str())) return p;
            if (!s.topPkg.empty()) return s.topPkg;   // 全是黑名单 → 退回最顶层
        }
        return fresh ? utils.getTopApp() : utils.getTopAppCached(2500);
    }

    // App 推送 topapp.txt 变更: 记录包名+时刻, 立即重评前台(走 getForegroundPkg 会用到推送值)
    void onTopAppChanged() {
        char buf[256];
        if (!readNodeTrim(topAppPath, buf, sizeof(buf)) || !buf[0]) return;
        {
            std::lock_guard<std::mutex> lk(pushMtx_);
            pushedTopApp_ = buf;
        }
        lastPushMs_.store(nowMs());
        evalForegroundOnce(true);
    }

    // fresh=true: 强制取最新前台（事件驱动路径用，避免 TTL 缓存返回旧包名误判未切换）
    // fresh=false: 允许用缓存（空闲复查/低频路径，省电）
    void evalForegroundOnce(bool fresh) {
        // 串行化"取前台 + 改 lastTopApp/currentMatch + 应用"，防两线程并发写 sysfs
        std::lock_guard<std::recursive_mutex> lk(applyMtx);

        // 保活：检测到游戏/keep_alive 包后, 只要其进程存活就维持画像, 无视前台变化
        // (小窗/分屏/输入法/通知栏/回桌面/切别的App 都不掉), 直到游戏进程退出才解除。
        if (!heldPkg.empty()) {
            if (utils.isPackageRunning(heldPkg.c_str())) {
                if (!fresh) return;   // 空闲复查: 进程在就维持, 不取前台
                // 前台事件: 仅当切到"另一个"保活类 App 才移交, 否则维持(不打日志)
                std::string cur = getForegroundPkg(true);
                if (cur.empty() || cur == "null" || cur == heldPkg) return;
                int m = Config::AppProfile::isBlacklisted(cur.c_str())
                          ? -1 : Config::AppProfile::findMatchingModel(cur.c_str());
                if (m < 0 || !(Config::AppProfile::Models[m].isGame ||
                               Config::AppProfile::Models[m].keepAlive)) return;
                heldPkg.clear();      // 移交给新保活 App(走下方重建)
                lastTopApp.clear();
            } else {
                logger.Info("保活: %s 已退出 → 解除", heldPkg.c_str());  // 退出打印一次
                heldPkg.clear();
                Config::AppProfile::currentMatch.store(-1);
                lastTopApp.clear();
                applyWithProfile();   // 立即落基础, 防去重跳过致高频残留
            }
        }

        std::string pkg = getForegroundPkg(fresh);
        if (pkg.empty() || pkg == "null") return;

        if (pkg == lastTopApp) return;
        lastTopApp = pkg;

        // 黑名单前台(launcher/systemui/输入法等)或无匹配画像 → matched=-1
        int matched = Config::AppProfile::isBlacklisted(pkg.c_str())
                        ? -1 : Config::AppProfile::findMatchingModel(pkg.c_str());

        // 建立保活须在去重之外: 重载清掉 heldPkg 后 match 可能不变(被去重跳过), 仍要恢复持有。
        // 仅真实匹配到游戏/保活画像时建立(省电兜底不建立保活)。
        if (matched >= 0) {
            const auto& mdl = Config::AppProfile::Models[matched];
            if (mdl.isGame || mdl.keepAlive) heldPkg = pkg;
        }

        // 无匹配/黑名单 → 应用省电模型(找不到则回落基础档)
        int newMatch = matched;
        bool toPowersave = false;
        if (newMatch < 0) {
            int ps = Config::AppProfile::findModelByName("powersave");
            if (ps >= 0) { newMatch = ps; toPowersave = true; }
        }

        // 去重：与当前画像相同则跳过(进入游戏/省电的日志即在此打印一次)
        int oldMatch = Config::AppProfile::currentMatch.load();
        if (newMatch != oldMatch) {
            Config::AppProfile::currentMatch.store(newMatch);
            if (matched >= 0) {
                const auto& mdl = Config::AppProfile::Models[matched];
                logger.Info("前台: %s → 档[%s]%s", pkg.c_str(),
                            mdl.modelName.c_str(), mdl.isGame ? " (游戏, 保活至退出)" : "");
            } else {
                logger.Info("前台: %s → %s", pkg.c_str(),
                            toPowersave ? "省电(无匹配/黑名单)" : "基础档(跟随全局)");
            }
            applyWithProfile();
        }
    }

    void appProfileTask() {
        if (waitStop(5000)) return; // 等待系统启动完成

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
        logger.Debug(fd >= 0
            ? "场景画像监控线程已启动 (事件驱动: 监听 top-app 切换, 支持通配符匹配)"
            : "场景画像监控线程已启动 (inotify 不可用, 回退轮询; 支持通配符匹配)");

        evalForegroundOnce(true);

        while (!shouldExit_.load()) {
            int ev = waitTopAppEvent(fd, wd, kPollIdleMs);
            if (shouldExit_.load()) break;
            if (ev < 0) {
                if (waitStop(kPollIdleMs)) break;
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
            if (shouldExit_.load()) break;
            evalForegroundOnce(true);
            correctGameTopAppCpus();
        }

        if (fd >= 0) {
            if (wd >= 0) inotify_rm_watch(fd, wd);
            close(fd);
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
            applyCurrentMode();
            online();
            SchedParam();
            function.gpuFreqControl();
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }
    }
};
