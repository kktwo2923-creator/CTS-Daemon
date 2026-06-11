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
    std::string lastReassertCpus;    // 游戏小窗护核: 上次实测的 top-app cpus(仅状态变化才记日志, 防刷屏)
    std::atomic<bool> keeperShouldExit{false};  // cpuset 守护循环退出标志
    std::atomic<int>  keptGameModel{-1};        // 游戏会话保活: 当前锁定的游戏模型索引(-1=未保活)
    std::string       lastGovWritten[4];        // 游戏护航: 每簇本会话已决定的 governor(空=未决定/簇还离线)
    // [Fix 卡死旧调速器] 非游戏护航: 每簇已确认"写不进"的配置 governor(空=未失败)。
    //   命中则本轮不再重试失败写、改以 SoC 默认调速器为守护目标 → 防每秒重试+Warn 刷屏。
    //   仅 cpusetKeeperTask 单线程读写(enforceBaseGovernors 唯一调用方), 无需加锁。
    std::string       baseGovFailed[4];
    int               baseGovFailedMatch = -2;  // 记录失败时的画像索引: 画像切换即清空重试(自愈)

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
            // [Fix 小窗护核] cpuset 守护循环: 游戏档期间把 top-app 压在全核, 压制 ROM 反复收窄。
            threads.emplace_back(thread(&Schedule::cpusetKeeperTask, this));
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
            // [交ROM] governor 不再回退基础档: 画像该簇留空 = 交 ROM 风驰
            //   (空值 FreqWriter 跳过, 不写 scaling_governor)。频率仍回退基础档。
            //   否则游戏档(governor 留空)会被按上基础档的调速器(如省电的 conservative)。
            const string_t& gov = model.Governor[i];
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        // GPU 频率（仅在画像中定义时覆盖）
        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
        }

        // [风驰/conservative] 画像调速器内置初始化: 即使画像未定义 SchedParam,
        //   只要该簇 governor 是 scx/hmbird(游戏风驰)或 conservative, 也要补写其专属可调参数
        //   (风驰 target_loads / conservative up_threshold 等), 否则风驰停在内核默认 target_loads。
        //   按"画像自己的 governor"判, 不回退基础档:
        //     - 留空(交 ROM 风驰) → 检测实际 scaling_governor, 是风驰就初始化(只初始化不接管);
        //     - scx/hmbird → 写 target_loads;  conservative → 写 conservative 参数。
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            const string_t& gov = model.Governor[i];
            if (gov.empty())
                initFengchiByDetect(Policy::CpuPolicy[i]);
            else if (isFengchiGov(gov))
                applyFengchiTunables(Policy::CpuPolicy[i], gov);
            else if (strcmp(gov.c_str(), "conservative") == 0)
                applyConservativeTunables(Policy::CpuPolicy[i]);
        }

        // 调速器自定义参数 SchedParam（仅在画像中定义时覆盖）
        char spPath[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (model.SchedParamCount[i] == 0) continue;
            // 二级回退：AppProfile -> Performances
            const string_t& gov = !model.Governor[i].empty() ? model.Governor[i]
                                                              : Performances::CpuGovernor[i];
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

    // [conservative] 省电档 conservative(co)调速器的内置初始化(偏省电)。
    //   取值综合 Color调度1385 极致版 与 ColorOS 各版本省电档, 往省电方向收一档:
    //     up_threshold=95    各版本最高(ColorOS 4.1/4.2): 最不愿升频 → 最省电
    //     down_threshold=80  极致版 B + ColorOS 3.5Pro:    负载一降就快速回落低频 → 省电
    //     freq_step=1        所有版本一致:                  台阶最细, 不过冲浪费电、也不突兀
    //     sampling_rate=12500 极致版 A:                     采样周期适中
    //   仅在某簇 governor 配成 conservative 时调用；conservative 目录的可调节点
    //   与 walt(adaptive_*/hispeed_load/up_rate_limit_us 等)完全不同，因此不能走
    //   通用 ParamSched 路径。写失败(该 SoC 无 conservative 调速器,已回退 walt/sugov_ext)
    //   由 FileWrite 静默忽略。
    void applyConservativeTunables(const int Policy) {
        static constexpr struct { const char* name; const char* value; } kConservative[] = {
            { "up_threshold",   "95"    },
            { "down_threshold", "80"    },
            { "freq_step",      "1"     },
            { "sampling_rate",  "12500" },
        };
        char path[256];
        for (const auto& p : kConservative) {
            FastSnprintf(path, sizeof(path), SchedParamPath, Policy, "conservative", p.name);
            utils.FileWrite(path, p.value);
            logger.Debug("CPU簇: %d conservative 参数: %s 值: %s", Policy, p.name, p.value);
        }
    }

    // [风驰] scx/hmbird(风驰)调速器是否为目标调速器。
    static bool isFengchiGov(const string_t& g) {
        return strcmp(g.c_str(), "scx") == 0 || strcmp(g.c_str(), "hmbird") == 0;
    }

    // [风驰] 游戏风驰(scx/hmbird)调速器的内置初始化。
    //   风驰只暴露一个可调节点 target_loads(CTS极致版 scx / ColorOS hmbird 一致),
    //   walt 形 ParamSched(adaptive_*/hispeed_load 等)在风驰目录下不存在, 不能走通用路径。
    //   target_loads=90(偏稳, 参考 CTS极致版 balance scx): 数值越低风驰越早拉高频→越流畅越费电。
    //   只初始化参数、不接管 governor: 仅在某簇 governor 已是 scx/hmbird 时写入;
    //   留空交 ROM 风驰的簇(gov 为空)不写。写失败(该 SoC 无此调速器,已回退)由 FileWrite 静默忽略。
    void applyFengchiTunables(const int Policy, const string_t& gov) {
        char path[256];
        FastSnprintf(path, sizeof(path), SchedParamPath, Policy, gov.c_str(), "target_loads");
        utils.FileWrite(path, "90");
        logger.Debug("CPU簇: %d 风驰(%s) target_loads: 90", Policy, gov.c_str());
    }

    // [风驰检测] 某簇 config governor 留空(交 ROM 风驰)时, 读实际 scaling_governor:
    //   若 ROM/系统当前跑的就是 scx/hmbird(风驰) → 写 target_loads 初始化。
    //   "只初始化、不接管": 全程不写 scaling_governor, 只在检测到风驰时补其专属参数。
    //   非风驰(walt/sugov_ext 等) → 什么都不做。
    void initFengchiByDetect(const int Policy) {
        char gp[256];
        FastSnprintf(gp, sizeof(gp), GovernorPath, Policy);
        char cur[64] = { 0 };
        readTrimmedNode(gp, cur, sizeof(cur));
        if (strcmp(cur, "scx") == 0 || strcmp(cur, "hmbird") == 0)
            applyFengchiTunables(Policy, string_t(cur));
    }

    void SchedParam() {
        char path[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            // [conservative] conservative 簇走内置初始化, 不写 walt 形 ParamSched
            //   (那些节点在 conservative 目录下不存在, 会整片写失败刷日志)。
            if (strcmp(Performances::CpuGovernor[i].c_str(), "conservative") == 0) {
                applyConservativeTunables(Policy::CpuPolicy[i]);
                continue;
            }
            // [风驰] scx/hmbird 簇走内置初始化(target_loads), 同样不写 walt 形 ParamSched。
            if (isFengchiGov(Performances::CpuGovernor[i])) {
                applyFengchiTunables(Policy::CpuPolicy[i], Performances::CpuGovernor[i]);
                continue;
            }
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

    // 读 sysfs/cpuset 节点首段并去掉尾部空白/换行。失败返回 false 且 out 置空串。
    bool readTrimmedNode(const char* p, char* out, size_t cap) {
        out[0] = '\0';
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        ssize_t n = read(fd, out, cap - 1);
        close(fd);
        if (n <= 0) { out[0] = '\0'; return false; }
        out[n] = '\0';
        for (ssize_t i = n - 1; i >= 0 &&
             (out[i] == '\n' || out[i] == '\r' || out[i] == ' ' || out[i] == '\t'); --i)
            out[i] = '\0';
        return true;
    }

    // top-app"应有的全核范围": 优先用配置 Cpuset::top_app(用户显式设过), 否则用当前全部
    //   在线 CPU(/sys/devices/system/cpu/online), 这样即便没启用 cpuset 配置也能护住全核。
    bool computeFullCpus(char* out, size_t cap) {
        if (Config::Cpuset::enable && !Config::Cpuset::top_app.empty()) {
            snprintf(out, cap, "%s", Config::Cpuset::top_app.c_str());
            return out[0] != '\0';
        }
        return readTrimmedNode("/sys/devices/system/cpu/online", out, cap);
    }

    // 把 top-app cpuset 压回全核范围。被收窄才写(幂等), 返回是否执行了写入。
    //   [关键] cpuset 约束: 子集必须 ⊆ 父集。实测此 ROM 把 top-app/foreground 连同父级
    //   (root /dev/cpuset)一起收到 0-5 → 直接写 top-app=0-7 会被内核拒绝/截断(看似写了实则
    //   没生效)。所以先把父(root)放开到全核, 再写 top-app/foreground。background/system-background
    //   不动(它们本就该限制在小核)。无 dumpsys/无日志, 供高频守护循环调用。
    bool enforceTopAppFullCpus() {
        char desired[64] = { 0 };
        if (!computeFullCpus(desired, sizeof(desired)) || !desired[0]) return false;
        char cur[64] = { 0 };
        readTrimmedNode("/dev/cpuset/top-app/cpus", cur, sizeof(cur));
        if (strcmp(cur, desired) == 0) return false;   // 未被收窄 → 不写
        utils.FileWrite("/dev/cpuset/cpus", desired);          // 先放开父集(内核禁改 root 时写失败无副作用)
        utils.FileWrite("/dev/cpuset/top-app/cpus", desired);
        utils.FileWrite("/dev/cpuset/foreground/cpus", desired);
        return true;
    }

    // 游戏被小窗盖住时把 top-app cpuset 拉回全核 + 回读校验 + 记日志(仅状态变化时, 防刷屏)。
    //   守卫路径(低频, 开小窗那一刻)用它; 高频守护循环用上面的 enforceTopAppFullCpus(静默)。
    void reassertGameTopAppCpus() {
        char desired[64] = { 0 };
        if (!computeFullCpus(desired, sizeof(desired)) || !desired[0]) return;
        char cur[64] = { 0 };
        readTrimmedNode("/dev/cpuset/top-app/cpus", cur, sizeof(cur));
        if (strcmp(cur, desired) == 0) { lastReassertCpus.assign(cur); return; }

        utils.FileWrite("/dev/cpuset/cpus", desired);
        utils.FileWrite("/dev/cpuset/top-app/cpus", desired);
        utils.FileWrite("/dev/cpuset/foreground/cpus", desired);

        // 读回实测值: 写入是否真生效只能看这里(不能只信"写了")。仅在状态变化时记日志, 防刷屏。
        char after[64] = { 0 };
        readTrimmedNode("/dev/cpuset/top-app/cpus", after, sizeof(after));
        if (lastReassertCpus != after) {
            if (strcmp(after, desired) == 0) {
                logger.Info("游戏小窗: top-app cpuset 被收窄(%s) → 已拉回全核 %s", cur, after);
            } else {
                char root[64] = { 0 };
                readTrimmedNode("/dev/cpuset/cpus", root, sizeof(root));
                logger.Warn("游戏小窗: 拉回全核未生效 目标=%s 实测 top-app=%s root=%s "
                            "(父集受限或被 ROM 即时改回)",
                            desired, after[0] ? after : "?", root[0] ? root : "?");
            }
            lastReassertCpus.assign(after);
        }
    }

    // [Best practice §5/#4] 把本守护进程自身的线程钉在低位核, 避免调度器守护自己抢占前台
    //   游戏最吃紧的超大核(参考 uperf: 控制器线程绑小核)。骁龙 8 Elite 是 2+6 无小核, 故选
    //   性能簇低位核 0-3, 避开 6-7 两颗 Oryon 超大核; 兼容传统 big.LITTLE(0-3 即小核)。
    //   在创建任何工作线程之前对主线程设亲和性 → 后续 new thread 继承之, 全进程生效。
    //   守护线程本就大多阻塞/低频, 钉低位核不影响其及时性(reassert/keeper 容忍 ms~200ms 抖动);
    //   且不降优先级, 以保证与 ORMS 抢写 cpuset 时仍能及时压回。失败仅记日志, 不影响功能。
    void pinSelfToLittleCores() {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c <= 3; c++) CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0)
            logger.Info("守护进程已绑定低位核 0-3(避开超大核 6-7), 不抢占前台游戏");
        else
            logger.Debug("绑定低位核失败 errno=%d(忽略, 不影响功能)", errno);
    }

    // 当前是游戏档才把 top-app 压回全核。供"即时压制"(inotify 事件)与守护循环共用。
    //   纯 cpuset 文件读写、幂等(未被收窄不写), 极轻量。
    void maybeEnforceGameCpus() {
        int cur = Config::AppProfile::currentMatch.load();
        if (keepAliveEligible(cur))
            enforceTopAppFullCpus();
    }

    // [Fix 超大核拿不到 scx] 游戏护航: 确保游戏模型每个"已上线"的簇的 cpufreq governor == 配置值。
    //   根因: 超大核(policy6)受 core_ctl 控制, 刚进游戏负载未起时还没上线 → applyAppProfile 里
    //   FreqWriter 那一刻 affected_cpus 为空被"无在线核"静默跳过 → 超大核停在内核默认(walt),
    //   之后没人补写。本函数每秒巡检: 簇已上线但 governor 不对就补写; 读实际 sysfs 比对, 不依赖
    //   FreqWriter 的去重缓存(那是盲区)。写失败(该簇不支持此 governor, 如 scx 只在小核簇暴露)
    //   → 回退 SoC 默认并告警一次(lastGovWritten 记最终值, 防每秒刷屏/重试)。
    void enforceGameGovernors(int idx) {
        const auto& m = Config::AppProfile::Models[idx];
        for (int i = 0; i <= 3; i++) {
            int policy = Config::Policy::CpuPolicy[i];
            if (policy < 0) continue;

            char aff[256];
            FastSnprintf(aff, sizeof(aff),
                         "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", policy);
            if (!utils.FileStartsWithDigit(aff)) continue;   // 簇仍离线 → 等它上线再决定

            // [交ROM] 游戏模型该簇 governor 留空 → 不接管(不补写 governor, 不回退基础档),
            //   只检测实际 scaling_governor: 是风驰(scx/hmbird)就补 target_loads(检测到风驰就初始化)。
            if (m.Governor[i].empty()) { initFengchiByDetect(policy); continue; }
            const string_t& gov = m.Governor[i];

            char gp[256];
            FastSnprintf(gp, sizeof(gp), GovernorPath, policy);
            char cur[64] = { 0 };
            readTrimmedNode(gp, cur, sizeof(cur));

            if (!lastGovWritten[i].empty()) {
                // 本会话已决定该簇 governor。若决定值就是配置目标且被 ORMS 改走 → 写回(防抢)。
                if (lastGovWritten[i] == gov.c_str() && strcmp(cur, gov.c_str()) != 0) {
                    utils.FileWriteBlocking(gp, gov);
                    // [风驰] 切换 governor 后内核会重置其可调节点 → 补写 target_loads。
                    if (isFengchiGov(gov)) applyFengchiTunables(policy, gov);
                }
                continue;
            }
            // 首次决定(该簇刚上线)
            if (strcmp(cur, gov.c_str()) == 0) {
                lastGovWritten[i] = gov.c_str();
                // [风驰] 超大核刚上线: governor 已对, 但 SchedParam 早先(核离线时)被跳过 →
                //   target_loads 仍是内核默认, 这里补写一次。
                if (isFengchiGov(gov)) applyFengchiTunables(policy, gov);
                continue;
            }
            if (utils.FileWriteBlocking(gp, gov)) {
                logger.Info("游戏护航: policy%d 调速器补写 %s (原 %s)", policy, gov.c_str(), cur);
                lastGovWritten[i] = gov.c_str();
                // [风驰] 补写 governor 后初始化其 target_loads。
                if (isFengchiGov(gov)) applyFengchiTunables(policy, gov);
            } else {
                string_t fb = function.checkQcom() ? "walt" : "sugov_ext";
                if (fb != gov) utils.FileWriteBlocking(gp, fb);
                logger.Warn("游戏护航: policy%d 不支持 %s, 已回退 %s(该簇请在 config 改用受支持的调速器)",
                            policy, gov.c_str(), fb.c_str());
                lastGovWritten[i] = fb.c_str();
            }
        }
    }

    // [Fix 超大核掉walt/不同步] 非游戏档的 governor 护航 — 与游戏档 enforceGameGovernors 对称。
    //   根因: 切 App 时若某簇(常见超大核)正被 core_ctl 离线/晚于 waitClusterReady 上线,
    //   FreqWriter 那一刻 affected_cpus 为空被跳过 → 该簇停在内核默认 walt, 其它簇已是 config
    //   的 conservative → 表现为"超大核 walt、小核 co 不同步"。本函数巡检每个"已上线"簇的实际
    //   governor, 与当前生效配置(画像优先, 否则基础 mode)不符就补写并初始化其可调参数。
    //   幂等: 稳定时只读不写; 空 governor(游戏档交 ROM)只检测风驰、不接管。
    void enforceBaseGovernors() {
        int match = Config::AppProfile::currentMatch.load();
        bool hasProfile = (match >= 0 && match < Config::AppProfile::modelCount);
        // [Fix 卡死旧调速器] 画像切换 → 清空"写失败"记录, 给新画像的 governor 重试机会
        //   (也覆盖 ROM 后来才暴露 hmbird 的场景: 下次切档自愈)。
        if (match != baseGovFailedMatch) {
            for (int i = 0; i <= 3; i++) baseGovFailed[i].clear();
            baseGovFailedMatch = match;
        }
        for (int i = 0; i <= 3; i++) {
            int policy = Config::Policy::CpuPolicy[i];
            if (policy < 0) continue;
            // 当前生效 governor: 有画像匹配用画像自身的, 否则用基础 mode 的(均不回退)
            const string_t& gov = hasProfile ? Config::AppProfile::Models[match].Governor[i]
                                             : Config::Performances::CpuGovernor[i];
            char aff[256];
            FastSnprintf(aff, sizeof(aff),
                         "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", policy);
            if (!utils.FileStartsWithDigit(aff)) continue;   // 簇离线 → 跳过, 上线后下一轮再补
            if (gov.empty()) { initFengchiByDetect(policy); continue; }   // 交 ROM: 只检测风驰

            char gp[256];
            FastSnprintf(gp, sizeof(gp), GovernorPath, policy);
            char cur[64] = { 0 };
            readTrimmedNode(gp, cur, sizeof(cur));
            if (strcmp(cur, gov.c_str()) == 0) {             // 已同步 → 不写(幂等)
                baseGovFailed[i].clear();                    // 配置 governor 实际可用 → 解除回退
                continue;
            }
            // [Fix 卡死旧调速器] 该簇配置 governor 本会话已确认写不进 → 不再每秒重试失败写,
            //   改以 SoC 默认调速器为守护目标(被 ROM 改走才补写, 幂等), 防 Warn 刷屏。
            if (!baseGovFailed[i].empty() && baseGovFailed[i] == gov.c_str()) {
                string_t fb = function.checkQcom() ? "walt" : "sugov_ext";
                if (strcmp(cur, fb.c_str()) != 0) utils.FileWriteBlocking(gp, fb);
                continue;
            }
            if (utils.FileWriteBlocking(gp, gov)) {
                logger.Info("governor 护航: policy%d %s→%s(晚上线/被改, 已补同步)", policy, cur, gov.c_str());
                if (isFengchiGov(gov)) applyFengchiTunables(policy, gov);
                else if (strcmp(gov.c_str(), "conservative") == 0) applyConservativeTunables(policy);
                baseGovFailed[i].clear();
            } else {
                // [Fix 卡死旧调速器] 与 enforceGameGovernors 对称: 写失败(该 SoC/当前状态不支持
                //   此 governor, 如 ROM 未暴露 hmbird)→ 回退 SoC 默认并 Warn 一次。否则该簇
                //   永远停在上一画像遗留的旧 governor(如小窗误切后的 conservative)→ 混合滞留态。
                string_t fb = function.checkQcom() ? "walt" : "sugov_ext";
                if (fb != gov) utils.FileWriteBlocking(gp, fb);
                logger.Warn("governor 护航: policy%d 不支持 %s, 已回退 %s(该簇请在 config 改用受支持的调速器)",
                            policy, gov.c_str(), fb.c_str());
                baseGovFailed[i] = gov.c_str();              // 记失败值: 本画像期内不再重试/刷屏
            }
        }
    }

    // 指定(游戏)模型的包名是否仍"在前台/可见"。判据: 前台快照里任一可见 Task 的包名
    //   findMatchingModel 仍命中该模型 → 还在游戏(支持通配符包名)。dumpsys 失败(快照无效)→
    //   保守返回 true(不因一次取值失败就误判游戏退出、放弃保活)。
    // [keep_alive] 该画像是否享有"前台保活"资格: 游戏档(isGame)或显式 keep_alive=true
    //   的画像(如风驰 fast)。保活/小窗保护统一用它判定, 不再只认 isGame。
    static bool keepAliveEligible(int idx) {
        if (idx < 0 || idx >= Config::AppProfile::modelCount) return false;
        const auto& m = Config::AppProfile::Models[idx];
        return m.isGame || m.keepAlive;
    }

    bool isGameModelForeground(int idx) {
        auto snap = utils.getForegroundCached(800);
        if (!snap.valid) return true;
        for (const auto& v : snap.visible)
            if (Config::AppProfile::findMatchingModel(v.c_str()) == idx)
                return true;
        return false;
    }

    // [游戏会话保活守护] 首次进入游戏档(game_heavy 等 isGame 模型)即启用; 之后只要该游戏模型的
    //   包名仍能在前台被检测到(top-app / 可见), 就把整套游戏画像(CPU 频率 / GPU 频率 / cpuset 0-7 /
    //   核心在线)持续锁定, 无视小窗 / 通知栏 / 焦点切换 / ROM(ORMS)抢写。直到该游戏真的离开前台
    //   (不可见)才退出保活、交回正常前台评估。
    //   节流: top-app cpuset 每 200ms 即时压(便宜, 应对 ORMS 高频抢 cpuset); 在场检测 + 整套重写
    //   每 ~1s 一次(一次 dumpsys + applyWithProfile, 靠各层 dedup 幂等, 频率多为 no-op, 不刷屏不顿挫)。
    void cpusetKeeperTask() {
        sleep(3);   // 等首次配置应用完成
        int tick = 0;
        while (!keeperShouldExit.load()) {
            int kept = keptGameModel.load();

            if (kept < 0) {
                // 未保活: 跟随 currentMatch 进入任一保活资格画像(游戏档或 keep_alive)→ 启用守护
                int cur = Config::AppProfile::currentMatch.load();
                if (keepAliveEligible(cur)) {
                    keptGameModel.store(cur);
                    tick = 0;
                    logger.Info("游戏会话保活已启用: %s",
                                Config::AppProfile::Models[cur].modelName.c_str());
                } else {
                    // [Fix 超大核掉walt] 非游戏档也护航 governor: 巡检已上线簇, 把晚上线/被改
                    //   而没同步到 config 的 governor(超大核常见)补回。便宜(只读, 不符才写),
                    //   复用本分支原有的 1s 唤醒, 几乎零额外开销。
                    {
                        std::lock_guard<std::recursive_mutex> lk(applyMtx);
                        enforceBaseGovernors();
                    }
                    utils.sleep_ms(1000);
                }
                continue;
            }

            // 保活中: 每 200ms 即时压 top-app cpuset(应对 ORMS 高频抢写)
            enforceTopAppFullCpus();

            // 每 ~1s: 检测游戏是否仍在前台 + 重写整套游戏画像
            if ((tick++ % 5) == 0) {
                if (isGameModelForeground(kept)) {
                    std::lock_guard<std::recursive_mutex> lk(applyMtx);
                    if (Config::AppProfile::currentMatch.load() != kept)
                        Config::AppProfile::currentMatch.store(kept);   // 抵消误切, 锁回游戏档
                    applyWithProfile(scene_.current());                  // 重写整套(频率/GPU/核心/cpuset, 幂等)
                    enforceGameGovernors(kept);                          // 护航: 超大核上线后补写 scx 等
                } else {
                    // 游戏确实离开前台(不可见) → 结束保活, 交回正常前台评估
                    logger.Info("游戏会话保活退出: %s 已不在前台",
                                Config::AppProfile::Models[kept].modelName.c_str());
                    {
                        std::lock_guard<std::recursive_mutex> lk(applyMtx);
                        keptGameModel.store(-1);
                        for (int i = 0; i <= 3; i++) lastGovWritten[i].clear();   // 下次进游戏重新决定
                        lastTopApp.clear();      // 让下次评估必走完整流程, 正确切到新前台
                    }
                    evalForegroundOnce(true);
                    continue;                    // 不 sleep, 立即重判
                }
            }
            utils.sleep_ms(200);
        }
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
            if (keepAliveEligible(curMatch)) {
                int nm = Config::AppProfile::findMatchingModel(pkg.c_str());
                // 新获焦若也是保活资格画像(真切到另一游戏/风驰 App)→ 允许切换; 否则(小窗/弹窗/
                //   桌面等)视为覆盖物, 维持当前画像不掉档。
                bool newIsGame = keepAliveEligible(nm);
                // [Fix 小窗误切] 主信号改为"当前游戏模型(按其包名规则)是否仍有可见 Task":
                //   开小窗时游戏仍在后台渲染 → 其 Task 仍 visible=true → 命中。比只探单个
                //   lastTopApp 更稳: 不依赖 lastTopApp 是否恰为游戏/是否被清空, 且支持通配/多包名。
                //   与游戏保活循环(isGameModelForeground)同一判据, 从源头消除"切到小窗 App 又被
                //   保活拉回游戏"的~1s 掉档(正是"开小窗识别成小窗 App"的现象)。复用同一份前台
                //   快照(getForegroundCached), 不增加 dumpsys 开销。
                bool gameStillForeground =
                        isGameModelForeground(curMatch)
                        || (!lastTopApp.empty() &&
                            (utils.isPackageInTopApp(lastTopApp.c_str())
                             || utils.isPackageVisible(lastTopApp.c_str())));
                if (!newIsGame && gameStillForeground) {
                    // [Fix 小窗收窄cpuset] 仅保持画像还不够: 部分 ROM(ColorOS/OplusOS)在小窗/浮窗
                    //   (freeform)获焦瞬间会"只改 cpuset 不下线核心"地把 top-app cpuset 收窄,
                    //   把超大核 6-7 踢出 → top-app 卡 0-5。原先这里只 return 不重写 cpuset,
                    //   导致 ROM 的收窄固着(实测: 游戏仍在 top-app、cpu6/7 online, 却卡 0-5)。
                    //   这里把 top-app cpuset 重新拉回应有的全核范围, 抵消 ROM 收窄。ROM 的收窄
                    //   写入本身会触发 top-app inotify → 本路径被唤醒重判 → 事件驱动地纠正,
                    //   无需轮询; 且幂等(实值==目标不写)避免与 ROM 来回 ping-pong 写风暴。
                    reassertGameTopAppCpus();
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
                logger.Info("前台应用切换: %s -> 匹配场景 '%s' (isGame=%d)",
                            pkg.c_str(),
                            Config::AppProfile::Models[newMatch].modelName.c_str(),
                            Config::AppProfile::Models[newMatch].isGame ? 1 : 0);
            } else {
                logger.Info("前台应用切换: %s -> 无匹配场景画像", pkg.c_str());
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

            // [Route B 即时压制] 收到 top-app cpuset 变动事件先无防抖地把 top-app 压回全核
            //   (游戏档时;便宜、幂等)。ROM 的 ORMS 抢写 top-app/cpus 本身就会触发本事件 →
            //   抵消其收窄的延迟从"尾沿防抖最长 1.2s"压到一次 inotify 往返(ms 级)。重操作
            //   (取包名+画像)仍走下面的尾沿防抖。
            //   注: 不在下面的防抖 while 里再调本函数 —— enforce 会读 top-app/cpus, 在 IN_ALL_EVENTS
            //   监视下自己的读会生成 IN_ACCESS 事件, 在循环内调用会自我诱发、把防抖顶到 kMaxDeferMs。
            //   持续抢写由 200ms 守护循环(cpusetKeeperTask)兜底, 此处只需即时压一次。
            maybeEnforceGameCpus();

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
