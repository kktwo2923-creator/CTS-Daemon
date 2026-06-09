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
    // [Fix 半写混合态] 串行化整段画像应用(c0/c1/核心/cpuset 一气呵成)。FreqWriter 内的 mtx
    //   只保护单次写, 保护不了多簇序列; 多线程(appProfileTask / configTriggerTask /
    //   perappTriggerTask)同时 apply 会在簇级别交错 → 出现 c0/c1 调速器各属不同画像的混合态。
    //   可重入: applyAllConfig→release→applyWithProfile 同线程嵌套不自锁。
    std::recursive_mutex applyMtx;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;
    SceneDetector scene_;

    bool cpuBoost = false;
    std::string lastTopApp;          // 上一次前台应用包名（用于画像切换去重）

    // [dedup] 缓存上一次写入的 (min,max,gov)，相同则跳过 sysfs 写
    // 避免 None→Touch→None / 重复 applyScene 等抖动场景重复刷盘
    string_t lastMinFreq[4];
    string_t lastMaxFreq[4];
    string_t lastGovernor[4];

    // [Fix] 原版 char temp[256] 是 class 成员，多线程并发写同一 buffer 会乱
    //            把它从成员里删掉，FreqWriter / applyAppProfile / online 改用栈上 buffer
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        pinSelfToLittleCores();   // 必须最先调: 在创建任何工作线程前设主线程亲和性, 新线程继承之
        Init();
        threads.emplace_back(thread(&Schedule::configTriggerTask, this));
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        threads.emplace_back(thread(&Schedule::perappTriggerTask, this));
        // [Fix] 去掉每秒一次的 configPollingTask —— 与 inotify 重复，纯费电
        threads.emplace_back(thread(&Schedule::cpuSetTriggerTask, this));

        // 移除场景识别线程组（用户反馈：场景频繁切换导致 CPU 不降频，方案 Z）
        //   - 不再有 Standby 熄屏压频（系统 Doze 自带）
        //   - 不再有 HeavyLoad / 突发负载 boost（交给 governor 的 up/down_rate_limit_us 处理）
        //   - 触摸/AmSwitch 等短时场景全部跳过
        //
        //   保留：AppProfile（按 App 切频率，更精准）+ GPU 守护
        //
        //   旧线程 sceneTickTask / screenStateTask / loadSamplingTask / touchDetectTask
        //   仍保留函数定义但不再启动，方便未来要回归 Standby/HeavyLoad 时切回去。

        // 应用画像线程（独立于 LaunchBoost，通过 dumpsys 获取前台包名）
        if (Config::AppProfile::enable) {
            threads.emplace_back(thread(&Schedule::appProfileTask, this));
        }
    }

    // [Fix] qlib::string operator==(self,self) 在两个独立堆分配的 string_t 之间比较时是 UB
    //            (distance(a.begin(), b.end()) 跨堆分配指针)。改用 strcmp 走 c_str()。
    static inline bool str_eq(const string_t& a, const string_t& b) {
        return strcmp(a.c_str(), b.c_str()) == 0;
    }

    void FreqWriter(const int Policy, const string_t MinFreq, const string_t MaxFreq, const string_t Governor) {
        // 找到 Policy 对应的簇索引（c0..c3）做缓存比较
        // 注：参数名 Policy 在此作用域内会遮蔽 Config::Policy 命名空间，必须完全限定。
        std::lock_guard<std::mutex> lock(mtx); 
        int cluster = -1;
        for (int i = 0; i <= 3; i++) {
            if (Config::Policy::CpuPolicy[i] == Policy) { cluster = i; break; }
        }
        if (cluster >= 0 &&
            str_eq(lastMinFreq[cluster],  MinFreq) &&
            str_eq(lastMaxFreq[cluster],  MaxFreq) &&
            str_eq(lastGovernor[cluster], Governor)) {
            // 频率/调速器与上一次完全相同 — 跳过 sysfs 写
            return;
        }

        char path[256];
        // [Fix] 关核跳过: 读 policyN/affected_cpus(只列在线核),为空说明整簇已 offline,
        //       此时写 governor 会报"调速器写入失败"刷屏(如 powersave 关大核)。整簇跳过。
        char affPath[256];
        FastSnprintf(affPath, sizeof(affPath),
                     "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", Policy);
        if (!utils.FileStartsWithDigit(affPath)) {
            logger.Debug("CPU簇: %d 无在线核,跳过 governor/频率", Policy);
            // [Fix] 整簇离线被跳过时清掉该簇去重缓存。否则核心稍后重新上线、要写相同
            //   governor(如 hmbird)时, 会被开头的去重判为"已是该值"而跳过 → 刚上线的核
            //   停在内核默认(walt)。清掉缓存保证下次必写。
            if (cluster >= 0) {
                lastMinFreq[cluster].clear();
                lastMaxFreq[cluster].clear();
                lastGovernor[cluster].clear();
            }
            return;
        }

        // [Fix] 顺序: 先写 governor 再写 min/max。切换调速器时内核可能重置频率,
        //       故频率必须在 governor 之后写。governor 用阻塞写(O_NONBLOCK 会 EAGAIN 静默失败)。
        //       值为空则跳过该项(如 fast 档 max 留空=不限上限交风驰)。
        if (!Governor.empty()) {
            FastSnprintf(path, sizeof(path), GovernorPath, Policy);
            bool ok = utils.FileWriteBlocking(path, Governor);
            if (!ok) {
                // [Fix] 配置里的调速器不被当前 SoC 支持(如 8E 没有 hmbird、
                //   天玑9400e 没有 walt)→ 写入失败, 整簇没人接管频率。回退到 SoC 默认调速器
                //   (高通 walt / 非高通 sugov_ext)再写一次, 保证频率有人管。
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
        }

        if (!MinFreq.empty()) {
            FastSnprintf(path, sizeof(path), MinFreqPath, Policy);
            utils.FileWrite(path, MinFreq);
            logger.Debug("CPU簇: %d 最小频率: %s", Policy, MinFreq.c_str());
        }

        if (!MaxFreq.empty()) {
            FastSnprintf(path, sizeof(path), MaxFreqPath, Policy);
            utils.FileWrite(path, MaxFreq);
            logger.Debug("CPU簇: %d 最大频率: %s", Policy, MaxFreq.c_str());
        } else {
            logger.Debug("CPU簇: %d 最大频率: 留空(交风驰接管)", Policy);
        }

        if (cluster >= 0) {
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

    // [Fix] 返回 core 所属的 policy(起始核 <= core 的最大 CpuPolicy); 找不到返回 -1。
    int policyForCore(int core) {
        int best = -1;
        for (int i = 0; i <= 3; i++) {
            int p = Policy::CpuPolicy[i];
            if (p != -1 && p <= core && p > best) best = p;
        }
        return best;
    }

    // [Fix] 等某个簇真正上线(affected_cpus 非空)。CPU 上线是异步 hotplug, 刚写完
    //   online=1 时 affected_cpus 可能还没更新, 若此刻就写 governor 会被 FreqWriter 判
    //   "整簇离线"跳过 → 超大核(6-7)拿不到 hmbird/风驰。这里轮询等待(上限 timeoutMs)。
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

    // ============================================================
    //  简化：去掉 Scenes 对 CPU 频率的覆盖
    //  - 不再有 Standby 熄屏压频（系统 Doze 已经管这块）
    //  - 不再有 HeavyLoad / 突发负载 boost（交给 governor 的 up/down_rate_limit_us 处理）
    //  - 整个函数等价于 Release —— CPU 写基础 mode 频率
    //  - GPU 由 applyBaseGpu 处理（确保游戏退出后 GPU 也回落）
    //
    //  保留这个函数签名只是为了让 applyWithProfile 的调用点不变；
    //  scene 参数现在只用来打日志，不影响实际写入。
    // ============================================================
    void applyScene(SceneDetector::Scene scene) {
        (void)scene;  // 不再按 scene 分支 — CPU 永远走基础 mode
        Release();
    }

    // ============================================================
    //  应用画像应用 — 场景分类画像
    //
    //  参考 Way_Balance 设计：
    //  - 按场景分类定义策略（如 game_heavy, daily_lite）
    //  - 支持通配符匹配包名（* 匹配任意序列，? 匹配单个字符）
    //  - 按 models 数组顺序匹配，第一个匹配的场景生效
    //
    //  优先级：AppProfile > SceneFreq > Performances
    //  - AppProfile 中非空字段覆盖场景频率
    //  - AppProfile 中空字段回退到 SceneFreq → Performances
    //  - GPU 频率和核心开关仅在画像中定义时才覆盖
    // ============================================================
    void applyAppProfile(int modelIdx, SceneDetector::Scene scene) {
        if (modelIdx < 0 || modelIdx >= Config::AppProfile::modelCount) {
            // 无匹配画像，使用场景频率
            applyScene(scene);
            return;
        }

        const auto& model = Config::AppProfile::Models[modelIdx];

        // [Fix] 先开核心,再写 governor/频率。否则从省电档(如0-5)切到画像档(0-7)时,
        //       governor 先写,CPU6/7 还离线收不到,之后才上线 → 个别核 governor 没改成。
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
                // [Fix] 等被点亮的簇真正上线再写频率/调速器, 确保 hmbird 落到 6-7 等刚上线的核
                if (broughtOnline) {
                    for (int i = 0; i <= 7; i++)
                        if (model.Online[i] == 1) waitClusterReady(policyForCore(i));
                    function.cpusetFunction();   // 核心上线后重落 cpuset, top-app 恢复到全核范围
                }
            } else {
                // [Fix cpuset卡0-5] 画像未定义核心开关 → 跟随基础模式(Performances::Online)。
                //   否则从省电档(6/7 离线)切到这种画像时完全不碰核心 → 停在 0-5, 6/7 拉不回来。
                restoreBaseCoresIfNeeded();
            }
        }

        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;

            // 二级回退（去掉中间 SceneFreq 层）：AppProfile -> Performances
            const string_t& minF = !model.MinFreq[i].empty() ? model.MinFreq[i]
                                                              : Performances::MinFreq[i];
            const string_t& maxF = !model.MaxFreq[i].empty() ? model.MaxFreq[i]
                                                              : Performances::MaxFreq[i];
            // [跟随系统] governor 留空 = 跟随系统(不写, 不回退基础模式); 频率仍二级回退基础。
            const string_t& gov  = model.Governor[i];
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        // GPU 频率（仅在画像中定义时覆盖）
        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
        }

        // 调速器自定义参数 SchedParam（仅在画像中定义时覆盖）
        char spPath[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (model.SchedParamCount[i] == 0) continue;
            // [跟随系统] governor 留空 → 无目标路径, 跳过 SchedParam(不回退基础)
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
        // [Fix] 必须先把核心上线,再写 governor/频率。
        //       否则省电档 CPU7 离线时切到游戏档,governor 先写(CPU7 还没上线收不到),
        //       之后才上线 CPU7 → CPU7 保持默认 governor,表现为"切档后个别核调速器没改成"。
        restoreBaseCoresIfNeeded();

        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], 
                    Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        // [Fix] Release 也要重新写基础 SchedParam（governor tunables）
        //            否则游戏退出后 governor 仍保留游戏画像里的
        //            up/down_rate_limit_us / hispeed_freq 等，导致 CPU 不降频。
        //            这是"CPU 怎么都不降频"的根本原因之一。
        SchedParam();
        // [Fix] Release 也要还原基础 GPU 频率
        applyBaseGpu();
        function.FeasFunc(false);
    }

    // [Fix] 把 GPU 频率写回基础 mode 设定
    //   AppProfile 用 gpuFreqControlCustom 写画像的 GPU 频率；
    //   退出画像匹配（回桌面）/进入 Scene 时必须主动把 GPU 还原，否则要等
    //   gpuFreqGuard 最长 15s 才生效，期间 GPU 卡在游戏高频上耗电。
    //
    //   只在 GpuFreq::min_freq / max_freq 至少一个非空时才动作，避免空字符串穿透。
    //   去抖由 gpuFreqGuard 内的 lastWrittenMin/Max 已做（base mode 没变就 skip）。
    void applyBaseGpu() {
        if (!Config::GpuFreq::enable) return;
        if (Config::GpuFreq::min_freq.empty() && Config::GpuFreq::max_freq.empty()) return;
        function.gpuFreqControlCustom(Config::GpuFreq::min_freq, Config::GpuFreq::max_freq);
    }

    void Reset() {
        // Reset 是"强制覆盖"路径，绕过去重缓存
        invalidateFreqCache();
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    // 清空 FreqWriter 去重缓存（强制下次写入）
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
            // 旧 LaunchBoost 路径 — 触发 AmSwitch 场景
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
        // 普通路径 — 按当前场景 + 应用画像写入
        applyWithProfile(scene_.current());
    }

    // 统一的频率应用入口：自动判断是否使用应用画像
    // 简化：scene 永远是 None（场景识别已禁用），二选一：画像 or 基础 mode
    void applyWithProfile(SceneDetector::Scene scene) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        int matchIdx = Config::AppProfile::currentMatch.load();
        if (Config::AppProfile::enable && matchIdx >= 0) {
            applyAppProfile(matchIdx, scene);
        } else {
            Release();   // Release 内部已调用 applyBaseGpu
            restoreBaseCoresIfNeeded();
        }
    }

    // 将核心开关恢复为基础模型的设置（Performances::Online）
    // 仅在至少有一个 Online[i] != -1 时才动作，避免无谓的 sysfs 写
    void restoreBaseCoresIfNeeded() {
        bool anyDefined = false;
        bool anyOnline  = false;
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] != -1) anyDefined = true;
            if (Performances::Online[i] == 1)  anyOnline  = true;
        }
        if (anyDefined) {
            online();
            // [Fix] 全局模式(如全局风驰)同样: 等刚点亮的簇真正上线再写 governor,
            //   否则超大核 6-7 来不及上线 → FreqWriter 跳过 → 保持 walt。
            for (int i = 0; i <= 7; i++)
                if (Performances::Online[i] == 1) waitClusterReady(policyForCore(i));
            // [Fix cpuset卡0-5] 核心上线后重落 cpuset, 让 top-app/foreground 恢复全核范围,
            //   不依赖内核 hotplug 自动还原 cpuset 的 effective_cpus。
            if (anyOnline) function.cpusetFunction();
        }
    }

    void online() {
        char path[256];
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] == -1) continue; // [Fix] -1 = 跳过，不再误关核心
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

    /**
     * 完整应用所有配置
     * release(频率) + SchedParam(调速器参数) + online(核心开关) + gpuFreqControl(GPU)
     */
    void applyAllConfig() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        release();
        SchedParam();
        online();
        function.gpuFreqControl();
    }

    // [Fix] 删除了 FileState / getFileState / fileStateChanged / reloadRuntimeConfig 等
    //            polling 时代的 dead code（已由 inotify 覆盖）

    /**
     * 监控目录 /sdcard/Android/CTS/ 下的 mode.txt 文件变化
     * /sdcard/ 是 FUSE 文件系统，inotify 监控文件可能失败，改为监控目录
     */
    /**
     * 监控 perapp_powermode.txt 变化(app 编辑后即时重载,daemon 主导切档)
     */
    void perappTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "perapp_powermode.txt";
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            // 只需重载 perapp 覆盖表,无需整份 readConfig(更轻)
            // 日志由 loadPerApp 内部按"条数变化"打印,避免每次写入都刷屏
            conf.loadPerApp();
            // [Fix] perapp 改了要对"当前前台"立刻生效。evalForegroundOnce 用 pkg==lastTopApp 去重:
            //   游戏包名没变、但它在 perapp 里映射的画像可能变了 → 必须强制重算, 否则在游戏里改
            //   perapp 既不生效也不打日志。
            logger.Info("perapp_powermode.txt 已重载 (%d 条映射), 重算当前前台画像",
                        Config::AppProfile::perAppCount);
            forceReevalForeground();
        }
    }

    // perapp 表改动后, 强制对当前前台重算并应用(绕过 pkg==lastTopApp 去重)。
    //   清空 lastTopApp 让本次评估必走完整流程; 是否真重写由 newMatch!=oldMatch 决定
    //   (映射没变则自然跳过, 不会无谓刷 sysfs)。
    void forceReevalForeground() {
        lastTopApp.clear();
        evalForegroundOnce(true);
    }

    // 读取文件原始字节并返回内容哈希(0=读失败/空)。用于"内容未变则跳过重载":
    //   外部(如 CTS App 的轮询)可能反复以相同内容回写 mode.txt/config.json,
    //   每次 IN_CLOSE_WRITE 都触发一次整份 readConfig + applyAllConfig(全量刷 sysfs),
    //   表现为"没切模式却每隔几秒重载一遍",造成周期性卡顿。内容去重可彻底消除这种空转。
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
            // [防空转] mode.txt 内容与上次相同(被原值回写)→ 跳过整份重载,避免周期性刷 sysfs
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

    /**
     * 监控目录 /sdcard/Android/CTS/ 下的 config.json 文件变化
     */
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
            // [防空转] config.json 内容与上次相同(被原值回写)→ 跳过整份重载
            size_t h = fileContentHash(jsonFilePath);
            if (h != 0 && h == lastHash) {
                logger.Debug("config.json 内容未变,跳过重载");
                continue;
            }
            if (conf.readConfig()) {
                // [Fix] readConfig 内部已 setLogLevel，无需重复
                function.ReloadFunC();   // [Fix] 用 ReloadFunC 而非 AllFunC（不重启守护线程）
                applyAllConfig();
                lastHash = h;
            } else {
                logger.Warn("JSON 配置重载失败，跳过应用");
            }
        }
    }

    /**
     * 通用目录监控函数 — 监控目录下的特定文件
     * FUSE 文件系统上 inotify 不能监控文件，只能监控目录
     */
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
            // Check events for target file
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

    // [Fix] 已退役: 前台切换时的频率写入收归 appProfileTask 唯一负责。
    //   原因(快速切 App 顿挫 + 默认模式被反复覆盖):
    //     此线程原本在每个 top-app 切换事件都盲调 release() → applyWithProfile(含 hotplug),
    //     既不检测包名、也不去重——无论新 App 是不是默认模式都覆盖一遍。它与同样监听切换的
    //     appProfileTask 是两条独立线程, 快速连切时两边各写一轮(含两次 hotplug), 且此处的
    //     currentMatch 尚未更新, 还会先错误地应用上一个 App 的画像 → 顿挫。
    //   现在: appProfileTask 先检测包名, 经 300ms 尾沿防抖后只在"匹配画像确实变化"时写一次;
    //     新包名匹配到当前默认模式(或与当前同画像)则直接跳过, 不再覆盖。游戏等非默认画像
    //     的高频在进入时由画像自身写入(300ms 内), 无需此处再做盲 boost。
    void cpuSetTriggerTask() {
        return;
    }

    // [cleanup] 已移除整组场景识别线程（sceneTickTask / screenStateTask /
    //   loadSamplingTask / readProcStat / touchDetectTask / openTouchDevices /
    //   netlinkScreenLoop）——这些函数在 后从未被启动，属编译进二进制的死代码。
    //   频率响应已完全交给 governor 的 up/down_rate_limit_us / hispeed_load。
    //   如需回归 Standby/HeavyLoad，可从 git 历史 恢复。

    // ============================================================
    //  应用画像监控线程 — 场景分类画像
    //
    //  参考 Way_Balance 设计：
    //  - 通过 dumpsys window 获取当前前台应用包名
    //  - 使用通配符匹配场景画像（* 匹配任意序列，? 匹配单个字符）
    //  - 按 models 数组顺序匹配，第一个匹配的场景生效
    //
    //  与 cpuSetTriggerTask（inotify top-app cgroup）互补：
    //    - cpuSetTriggerTask: 快速检测应用切换（触发 AmSwitch 场景）
    //    - appProfileTask: 获取包名并匹配场景画像（频率策略）
    // ============================================================
    // 阻塞等待 top-app cpuset 发生变动事件(前台 App 切换会改写该 cgroup)。
    //   maxWaitMs: 最长阻塞时长; 期间有事件立即返回 true, 超时返回 false。
    //   inotify 失败(返回 fd<0)时返回 -1, 调用方据此回退到轮询模式。
    //   事件驱动相比定时轮询: 切换响应即时, 且空闲时线程完全休眠不耗电。
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
        if (ret == 0) return 0;            // 超时, 无事件
        char buf[4096];
        ssize_t n = read(inotifyFd, buf, sizeof(buf));   // 清空事件队列
        (void)n;
        return 1;                          // 有事件
    }


    // [Best practice §5/#4] 把本守护进程自身的线程钉在低位核, 避免调度器守护自己抢占前台
    //   游戏最吃紧的超大核(参考 uperf: 控制器线程绑小核)。骁龙 8 Elite 是 2+6 无小核, 故选
    //   性能簇低位核 0-3, 避开 6-7 两颗 Oryon 超大核; 兼容传统 big.LITTLE(0-3 即小核)。
    //   在创建任何工作线程之前对主线程设亲和性 → 后续 new thread 继承之, 全进程生效。
    //   守护线程本就大多阻塞/低频(inotify 等待), 钉低位核不影响其及时性。失败仅记日志, 不影响功能。
    void pinSelfToLittleCores() {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c <= 3; c++) CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0)
            logger.Info("守护进程已绑定低位核 0-3(避开超大核 6-7), 不抢占前台游戏");
        else
            logger.Debug("绑定低位核失败 errno=%d(忽略, 不影响功能)", errno);
    }



    // 取前台并按需切换画像。
    //   fresh=true: 强制取最新前台(事件驱动路径必须用,否则 getTopAppCached 的 TTL
    //   缓存会返回切换前的旧包名 → 误判"未变化"跳过 → 表现为检测延迟很高)。
    //   fresh=false: 允许用缓存(空闲复查/熄屏等低频路径,省电)。
    void evalForegroundOnce(bool fresh) {
        // [并发安全] 串行化整段"取前台 + 改 lastTopApp/currentMatch + 应用"。否则
        //   appProfileTask 与 perappTriggerTask 两线程同时进来会并发改 lastTopApp(非原子)
        //   并交错写 sysfs。可重入锁: 内部 applyWithProfile 同线程重入不自锁。
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        std::string pkg = fresh ? utils.getTopApp() : utils.getTopAppCached(2500);
        if (pkg.empty() || pkg == "null") return;

        // [Fix 小窗/弹窗/通知栏] 黑名单前台(launcher/systemui/输入法/PopupWindow/通知栏/android 等)
        //   只是临时覆盖在当前应用之上, 并不代表真正切换了应用 → 直接忽略, 保持当前画像不变,
        //   且不更新 lastTopApp(这样小窗关掉/点回游戏时能正确识别为"还在原应用")。
        //   否则开小窗、拉通知栏、弹输入法都会被误判为"离开游戏回默认", 风驰画像被撤掉
        //   (用户需再点一下游戏才恢复)。这正是问题1/3的根因。
        if (Config::AppProfile::isBlacklisted(pkg.c_str())) return;

        if (pkg == lastTopApp) return;

        // [Fix 游戏上开App小窗] 当前已是游戏画像、新获焦却是非游戏 App, 但游戏窗口仍"可见"
        //   (说明全屏游戏还在后面渲染, 只是被小窗/浮窗盖住) → 保持游戏画像不切, 也不更新
        //   lastTopApp(关掉小窗回到游戏时能正确识别为"还在游戏")。否则微信等 App 小窗一获焦就被
        //   判成前台, 切到它的画像(social 等)→ 撤掉风驰/游戏档; 且 social 无核心开关会回退基础模式,
        //   若基础模式是省电/均衡(6-7核关)→ 核心被降到 0-5。这是"开小窗后掉档/卡0-5"的根因。
        //   [信号选择] 主信号用"游戏 Task 是否 visible=true"(ROM 无关): 本机 ColorOS 小窗报
        //     mode=fullscreen 不是 freeform, 按窗口模式判不出; 且开小窗会把游戏踢出 top-app,
        //     isPackageInTopApp 也漏判。先查 top-app(读文件, 便宜)短路, 不在再 dumpsys 查可见性。
        //     此分支只在"游戏档 + 新获焦非游戏"时触发(正是开小窗那一刻)才付一次 dumpsys, 不影响常态。
        {
            int curMatch = Config::AppProfile::currentMatch.load();
            if (curMatch >= 0 && curMatch < Config::AppProfile::modelCount
                && Config::AppProfile::Models[curMatch].isGame && !lastTopApp.empty()) {
                int nm = Config::AppProfile::findMatchingModel(pkg.c_str());
                bool newIsGame = (nm >= 0 && Config::AppProfile::Models[nm].isGame);
                if (!newIsGame &&
                    (utils.isPackageInTopApp(lastTopApp.c_str()) || utils.isPackageVisible(lastTopApp.c_str()))) {
                    // [不抢 cpuset] 只在画像层保持游戏档不被切走; cpuset / 调速器交给系统,
                    //   不再把 top-app 拉回全核 —— 那正是之前与 ROM 来回抢写、导致 cpuset 在
                    //   0-7/2-7 之间抖动的根因。小窗只是覆盖, 游戏画像(频率/核心)保持即可。
                    logger.Debug("小窗覆盖游戏(前台=%s, 游戏仍可见) → 保持游戏画像", pkg.c_str());
                    return;   // 游戏仍在前台, 小窗只是覆盖其上 → 保持游戏画像
                }
            }
        }

        lastTopApp = pkg;
        int newMatch = Config::AppProfile::findMatchingModel(pkg.c_str());

        // [Fix] currentMatch 是 atomic
        // 去重: 新包名匹配到的画像与当前相同(含两者都是默认模式 -1)→ 不重复覆盖频率, 直接跳过。
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
            // 应用新的频率策略(核心开关已在 applyAppProfile/restoreBaseCoresIfNeeded 内处理)
            applyWithProfile(scene_.current());
        }
    }

    void appProfileTask() {
        sleep(5); // 等待系统启动完成

        // 事件驱动 + 尾沿防抖: 阻塞监听 top-app cpuset 变动(前台切换), 替代固定轮询。
        //   收益: 切换响应快、空闲零唤醒(省电)。inotify 不可用时自动回退到轮询。
        //
        //   [关键] 检测(取包名)可以快; 但"重操作"(写 scaling_max/min、切核)绝不能每个事件都做。
        //   快速连切 App 时若每切一次都重写频率+切核, 会与 App 启动争抢 CPU → 顿挫。
        //   因此重操作走尾沿防抖: 收到切换事件后, 等前台"安静"kQuietMs(不再有新切换)才认为
        //   已落定, 此时才取最新前台并应用一次。
        //     - 单次切换:    约 kQuietMs 后生效(远快于旧 3s 轮询, 解决"检测慢")
        //     - 快速连切:    持续顺延, 只在最终落定的那个 App 上写一次频率(不风暴, 不卡)
        //   本线程是前台切换写频率的唯一入口(cpuSetTriggerTask 的盲写已退役): 先检测包名, 仅在
        //   匹配画像确实变化时才写; 新包名匹配到当前默认模式/同画像则跳过, 不重复覆盖。
        constexpr int kPollIdleMs  = 4000;  // 回退轮询 / inotify 空闲复查间隔
        constexpr int kQuietMs     = 300;   // 安静窗口: 无新切换事件持续这么久即视为已落定
        constexpr int kMaxDeferMs  = 1200;  // 硬上限: 持续不断的事件也保证最终生效, 不无限顺延

        int fd = inotify_init1(IN_CLOEXEC);
        int wd = -1;
        if (fd >= 0) {
            wd = inotify_add_watch(fd, cpusetEventPath, IN_ALL_EVENTS);
            if (wd < 0) { close(fd); fd = -1; }
        }
        logger.Info(fd >= 0
            ? "场景画像监控线程已启动 (事件驱动: 监听 top-app 切换, 支持通配符匹配)"
            : "场景画像监控线程已启动 (inotify 不可用, 回退轮询; 支持通配符匹配)");

        // 启动时先评估一次当前前台(强制取最新)
        evalForegroundOnce(true);

        while (true) {
            // 熄屏时降低活动频率(省电); 此时不阻塞监听, 定期轻量复查
            if (scene_.current() == SceneDetector::Scene::Standby) {
                utils.sleep_ms(8000);   // : 5s -> 8s
                continue;
            }

            int ev = waitTopAppEvent(fd, wd, kPollIdleMs);
            if (ev < 0) {
                // inotify 出错 → 退化为纯轮询, 避免线程空转占 CPU
                utils.sleep_ms(kPollIdleMs);
                evalForegroundOnce(true);
                continue;
            }
            if (ev == 0) {
                // 超时无事件: 轻量复查一次(可用缓存, 覆盖个别 ROM 不触发 cpuset 事件的情况)
                evalForegroundOnce(false);
                continue;
            }

            // top-app cpuset 变动 = 前台可能切了。不再做任何 cpuset 抢写(交给系统),
            //   只走下面的尾沿防抖去取包名、按需切画像。
            // 尾沿防抖: 等前台安静下来再做重操作(取包名/切画像), 避免连切时频率写风暴
            int deferred = 0;
            while (deferred < kMaxDeferMs) {
                int more = waitTopAppEvent(fd, wd, kQuietMs);
                if (more <= 0) break;       // 安静窗口内无新事件(或超时/出错) → 已落定
                deferred += kQuietMs;        // 又来切换事件 → 继续等它安静
            }
            // 前台已稳定 → 取最新前台并应用一次(强制取最新, 不用缓存)
            evalForegroundOnce(true);
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
        // [Fix] readConfig 内部已在解析 meta 后立即 setLogLevel，
        // 所以这里的「名称/版本/作者」banner 必须放在 readConfig 之后才能受 loglevel 控制；
        // 同时不需要再次 setLogLevel 了。
        bool configOk = conf.readConfig();
        logger.Info("名称: %s",       Meta::name.c_str());
        logger.Info("版本: %d",       Meta::version);
        logger.Info("作者: %s",       Meta::author.c_str());
        logger.Info("日志等级: %s",   Meta::loglevel.c_str());
        function.AllFunC();   // [Fix] AllFunC -> startGuards() 已经会启 GPU 守护线程
        if (configOk) {
            release();
            online();
            SchedParam();
            function.gpuFreqControl();
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }
        // [Fix] 删掉这里第二次启动 gpuFreqGuard 的代码（与 AllFunC 重复，会跑两条守护线程）
    }
};
