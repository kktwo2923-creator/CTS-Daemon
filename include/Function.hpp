#pragma once

#include <cstring>
#include <cerrno>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include "JsonConfig.hpp"
#include "Config.hpp"
#include "Utils.hpp"
#include "Logger.hpp"

using namespace Config;

class Function {
public:
    std::atomic<bool> gpuGuardShouldExit{false};
    std::atomic<int> latestGpuMaxMhz{-1};
    std::atomic<int> latestGpuMinMhz{-1};

private:
    static constexpr const char* qcomFeas = "/sys/module/perfmgr/parameters/perfmgr_enable";
    static constexpr const char* mtkFeas = "/sys/module/mtk_fpsgo/parameters/perfmgr_enable";
    static constexpr const char* ufsPath = "/sys/class/block/sda";
    static constexpr const char* cpusetPath = "/dev/cpuset/";
    static constexpr const char* cpuctlPath = "/dev/cpuctl/";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* easSchedPath = "/proc/sys/kernel/sched_energy_aware";
    Utils utils;
    Logger logger;

    std::thread gpuGuardThread;

public:
    Function() = default;

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    ~Function() {
        stopGuards();
    }

    // [v4.1] AllFunC 在 Init 时调用一次（启动 GPU 守护 + 写 cpuset 等）
    //        重载 config.json 时只需走 ReloadFunC（不再无谓写 cpuset，省 sysfs 抖动）
    void AllFunC() {
        cpusetFunction();
        gpuFreqControl();
        CfsSchedOpt();
        startGuards();
    }

    // 仅用于运行时重载，跳过 cpuset 重写（cpuset 通常不变，重写会造成 sysfs 抖动）
    // 但如果用户确实改了 cpuset 字段，把 enable 临时关掉再开就能强制重写
    void ReloadFunC() {
        cpusetFunction();   // 仍然写一次（实际很多 utils.FileWrite 会 chmod，开销低）
        gpuFreqControl();
        CfsSchedOpt();
        // 不再调用 startGuards()：守护线程已经在跑，避免漏判 joinable() 状态
    }

    void startGuards() {
        gpuGuardShouldExit.store(false);

        if (GpuFreq::enable && !gpuGuardThread.joinable()) {
            gpuGuardThread = std::thread([this]() {
                gpuFreqGuard();
            });
            logger.Info("GPU频率守护线程已启动");
        }
    }

    void stopGuards() {
        gpuGuardShouldExit.store(true);

        if (gpuGuardThread.joinable()) {
            gpuGuardThread.join();
        }
    }

    void cpusetFunction() {
        if (!Cpuset::enable) return;

        utils.FileWrite("/dev/cpuset/top-app/cpus", Cpuset::top_app);
        utils.FileWrite("/dev/cpuset/foreground/cpus", Cpuset::foreground);
        utils.FileWrite("/dev/cpuset/background/cpus", Cpuset::background);
        utils.FileWrite("/dev/cpuset/system-background/cpus", Cpuset::system_background);
        utils.FileWrite("/dev/cpuset/restricted/cpus", Cpuset::restricted);

        logger.Info("Cpuset OK");
    }

    bool writeSysfsLocked(const char* path, const char* value, int valLen) {
        if (!path || !value || valLen <= 0) return false;

        // [Fix v4.6] 与 utils.FileWrite 对齐：open 失败时 chmod 0666 后重试
        //            许多新 SoC（如 SM8850）的 GPU sysfs 默认 0444/0644，
        //            直接 O_WRONLY 会 EACCES → write 失败 → tryGpuPath 误判此路径不可用 → 链式跳过。
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            chmod(path, 0666);
            fd = open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0) {
                logger.Debug("GPU sysfs 打开失败（已尝试 chmod）: %s errno=%d", path, errno);
                return false;
            }
        }

        char buf[64];

        if (valLen + 1 >= static_cast<int>(sizeof(buf))) {
            valLen = sizeof(buf) - 2;
        }

        memcpy(buf, value, valLen);
        buf[valLen] = '\n';

        int totalLen = valLen + 1;

        ssize_t n = write(fd, buf, totalLen);
        close(fd);

        if (n != totalLen) {
            logger.Debug("GPU sysfs 写入字节数不匹配: %s wrote=%zd expect=%d errno=%d",
                         path, n, totalLen, errno);
            return false;
        }
        return true;
    }

    void setKgslGovernorPerformance() {
        const char* govPath = "/sys/class/kgsl/kgsl-3d0/devfreq/governor";

        if (access(govPath, F_OK) != 0) return;

        char curGov[64] = {0};

        int fd = open(govPath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, curGov, sizeof(curGov) - 1);
            close(fd);

            if (n > 0) {
                for (int i = 0; curGov[i]; i++) {
                    if (curGov[i] == '\n' || curGov[i] == '\r') {
                        curGov[i] = '\0';
                        break;
                    }
                }

                if (strstr(curGov, "performance")) {
                    return;
                }
            }
        }

        chmod(govPath, 0666);

        int fdw = open(govPath, O_WRONLY | O_CLOEXEC);
        if (fdw >= 0) {
            write(fdw, "performance\n", 12);
            close(fdw);
        }
    }

    int readKgslAvailableFreqs(int freqsOut[]) {
        // [Fix v4.6] 旧的 kgsl-3d0/gpu_available_frequencies 在新内核（SM8850 等）
        //            可能不存在，改用 devfreq 的 available_frequencies 兜底
        static const char* paths[] = {
            "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies",
            "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/available_frequencies",
            "/sys/class/devfreq/5000000.qcom,kgsl-3d0/available_frequencies",
            "/sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies",
        };

        int fd = -1;
        for (auto path : paths) {
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) break;
        }
        if (fd < 0) return 0;

        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n <= 0) return 0;

        buf[n] = '\0';

        int count = 0;

        for (char* p = buf; *p && count < 64;) {
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;

            if (*p == '\0') break;

            freqsOut[count++] = atoi(p);

            while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        }

        return count;
    }

    int snapGpuFreq(int targetHz, const int availFreqs[], int count, bool isMax) {
        if (count <= 0) return targetHz;

        int best = -1;
        int bestDiff = 0x7FFFFFFF;

        for (int i = 0; i < count; i++) {
            int f = availFreqs[i];
            if (f <= 0) continue;

            if (isMax) {
                if (f <= targetHz) {
                    int diff = targetHz - f;
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        best = f;
                    }
                }
            } else {
                if (f >= targetHz) {
                    int diff = f - targetHz;
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        best = f;
                    }
                }
            }
        }

        if (best < 0) {
            bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < count; i++) {
                if (availFreqs[i] <= 0) continue;

                int diff = availFreqs[i] > targetHz
                               ? availFreqs[i] - targetHz
                               : targetHz - availFreqs[i];

                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = availFreqs[i];
                }
            }
        }

        return best > 0 ? best : targetHz;
    }

    bool tryGpuPath(const char* path, int mhzValue, bool isHz, bool needsGov, const char* label) {
        if (!path || !label) return false;
        if (mhzValue <= 0) return true;
        if (access(path, F_OK) != 0) return false;

        if (needsGov) {
            setKgslGovernorPerformance();
        }

        long long valueToWriteLL = isHz
                                       ? static_cast<long long>(mhzValue) * 1000000LL
                                       : static_cast<long long>(mhzValue);

        if (valueToWriteLL <= 0 || valueToWriteLL > 2147483647LL) {
            logger.Warn("GPU频率数值异常: path=%s value=%lld", path, valueToWriteLL);
            return false;
        }

        int valueToWrite = static_cast<int>(valueToWriteLL);

        bool isMax = false;
        if (label[0] == 'm' && label[1] == 'a') {
            isMax = true;
        }

        if (isHz && strstr(path, "kgsl")) {
            int availFreqs[64];
            int availCount = readKgslAvailableFreqs(availFreqs);

            if (availCount > 0) {
                int snapped = snapGpuFreq(valueToWrite, availFreqs, availCount, isMax);

                if (snapped != valueToWrite) {
                    logger.Debug("GPU频率校准: %d -> %d, path=%s", valueToWrite, snapped, path);
                    valueToWrite = snapped;
                }
            }
        }

        char valStr[32];
        int len = FastSnprintf(valStr, sizeof(valStr), "%d", valueToWrite);

        if (len <= 0) return false;

        bool ok = writeSysfsLocked(path, valStr, len);

        if (ok) {
            logger.Debug("GPU频率写入成功: %s = %s", path, valStr);
        } else {
            logger.Debug("GPU频率写入失败: %s = %s", path, valStr);
        }

        return ok;
    }

    // [天玑] 动态探测联发科 Mali devfreq 目录。地址前缀不固定(13000000/13040000 等),
    //        遍历 /sys/class/devfreq/ 找名字含 ".mali" 的节点,拼出 min_freq/max_freq 路径。
    //        命中后把完整路径写入 outPath。which: "max_freq" 或 "min_freq"。
    bool findMaliDevfreqPath(const char* which, char* outPath, size_t outLen) {
        DIR* dir = opendir("/sys/class/devfreq/");
        if (!dir) return false;
        struct dirent* entry;
        bool found = false;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            // 匹配 ".mali" 结尾的 devfreq 节点(如 13000000.mali)
            if (strstr(entry->d_name, ".mali") == nullptr) continue;
            FastSnprintf(outPath, outLen, "/sys/class/devfreq/%s/%s", entry->d_name, which);
            if (access(outPath, F_OK) == 0) { found = true; break; }
        }
        closedir(dir);
        return found;
    }

    bool tryGpuMaxPaths(int mhzMax) {
        if (mhzMax <= 0) return true;

        // [天玑] 联发科 Mali GPU: 标准 devfreq, 单位 Hz, 与高通逻辑一致。
        //        动态探测地址前缀,再固定地址兜底。
        {
            char maliPath[128];
            if (findMaliDevfreqPath("max_freq", maliPath, sizeof(maliPath)) &&
                tryGpuPath(maliPath, mhzMax, true, false, "max"))
                return true;
        }
        if (tryGpuPath("/sys/class/devfreq/13000000.mali/max_freq", mhzMax, true, false, "max"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/13040000.mali/max_freq", mhzMax, true, false, "max"))
            return true;

        // [Fix v4.6] 增加 SM8850/Adreno 8x 时代的标准 devfreq 路径
        //            新内核 devfreq 子系统挂载在 /sys/class/devfreq/<addr>.qcom,kgsl-3d0/
        //            旧的 /sys/class/kgsl/kgsl-3d0/devfreq/ 是软链，可能不存在
        if (tryGpuPath("/sys/class/devfreq/3d00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/5000000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        // [旧高通] 骁龙855/SM8150=2c00000, 骁龙800/801=fdb00000
        if (tryGpuPath("/sys/class/devfreq/2c00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/fdb00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;

        if (tryGpuPath("/sys/kernel/gpu/gpu_max_clock", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", mhzMax, true, true, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/max_gpuclk", mhzMax, true, false, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/max_clock_mhz", mhzMax, false, false, "max"))
            return true;

        return false;
    }

    bool tryGpuMinPaths(int mhzMin) {
        if (mhzMin <= 0) return true;

        // [天玑] 联发科 Mali GPU min_freq, 单位 Hz
        {
            char maliPath[128];
            if (findMaliDevfreqPath("min_freq", maliPath, sizeof(maliPath)) &&
                tryGpuPath(maliPath, mhzMin, true, false, "min"))
                return true;
        }
        if (tryGpuPath("/sys/class/devfreq/13000000.mali/min_freq", mhzMin, true, false, "min"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/13040000.mali/min_freq", mhzMin, true, false, "min"))
            return true;

        // [Fix v4.6] 同上，加 SM8850 标准 devfreq 路径
        if (tryGpuPath("/sys/class/devfreq/3d00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/5000000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        // [旧高通] 骁龙855/SM8150=2c00000, 骁龙800/801=fdb00000
        if (tryGpuPath("/sys/class/devfreq/2c00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        if (tryGpuPath("/sys/class/devfreq/fdb00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;

        if (tryGpuPath("/sys/kernel/gpu/gpu_min_clock", mhzMin, false, false, "min"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/devfreq/min_freq", mhzMin, true, true, "min"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/min_clock_mhz", mhzMin, false, false, "min"))
            return true;

        return false;
    }

    bool tryGpuPwrlevel(int mhzMax, int mhzMin) {
        const char* availPath  = "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies";
        const char* numPwrPath = "/sys/class/kgsl/kgsl-3d0/num_pwrlevels";
        const char* maxPwrPath = "/sys/class/kgsl/kgsl-3d0/max_pwrlevel";
        const char* minPwrPath = "/sys/class/kgsl/kgsl-3d0/min_pwrlevel";

        if (access(numPwrPath, F_OK) != 0 || access(availPath, F_OK) != 0) {
            return false;
        }

        int numPwrlevels = utils.readInt(numPwrPath);
        if (numPwrlevels <= 0) return false;

        int fd = open(availPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;

        char readBuf[1024];
        ssize_t n = read(fd, readBuf, sizeof(readBuf) - 1);
        close(fd);

        if (n <= 0) return false;

        readBuf[n] = '\0';

        int freqs[64];
        int freqCount = 0;

        for (char* p = readBuf; *p && freqCount < 64;) {
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;

            if (*p == '\0') break;

            freqs[freqCount++] = atoi(p);

            while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        }

        if (freqCount <= 0) return false;

        bool success = false;

        if (mhzMax > 0) {
            long long targetHzLL = static_cast<long long>(mhzMax) * 1000000LL;
            if (targetHzLL > 2147483647LL) return false;

            int targetHz = static_cast<int>(targetHzLL);
            int maxPwr = 0;
            int bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < freqCount; i++) {
                if (freqs[i] <= targetHz) {
                    int diff = targetHz - freqs[i];

                    if (diff < bestDiff) {
                        bestDiff = diff;
                        maxPwr = i;
                    }
                }
            }

            char pwrBuf[16];
            int pwrLen = FastSnprintf(pwrBuf, sizeof(pwrBuf), "%d", maxPwr);

            if (writeSysfsLocked(maxPwrPath, pwrBuf, pwrLen)) {
                success = true;
            }
        }

        if (mhzMin > 0) {
            long long targetHzLL = static_cast<long long>(mhzMin) * 1000000LL;
            if (targetHzLL > 2147483647LL) return false;

            int targetHz = static_cast<int>(targetHzLL);
            int minPwr = freqCount - 1;
            int bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < freqCount; i++) {
                if (freqs[i] >= targetHz) {
                    int diff = freqs[i] - targetHz;

                    if (diff < bestDiff) {
                        bestDiff = diff;
                        minPwr = i;
                    }
                }
            }

            char pwrBuf[16];
            int pwrLen = FastSnprintf(pwrBuf, sizeof(pwrBuf), "%d", minPwr);

            if (writeSysfsLocked(minPwrPath, pwrBuf, pwrLen)) {
                success = true;
            }
        }

        return success;
    }

    void gpuFreqControl() {
        int mhzMin = Fastatoi(GpuFreq::min_freq.c_str());
        int mhzMax = Fastatoi(GpuFreq::max_freq.c_str());

        if (mhzMin <= 0 && mhzMax <= 0) return;

        bool minOk = true;
        bool maxOk = true;

        if (mhzMin > 0) {
            minOk = tryGpuMinPaths(mhzMin);
            if (minOk) {
                latestGpuMinMhz.store(mhzMin);
            }
        }

        if (mhzMax > 0) {
            maxOk = tryGpuMaxPaths(mhzMax);
            if (maxOk) {
                latestGpuMaxMhz.store(mhzMax);
            }
        }

        if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
            if (tryGpuPwrlevel(mhzMax, mhzMin)) {
                if (!minOk) minOk = true;
                if (!maxOk) maxOk = true;
            }
        }

        if (minOk && maxOk) {
            logger.Info("GPU频率已设置: min=%dMHz max=%dMHz", mhzMin, mhzMax);
        } else {
            logger.Warn("GPU频率设置可能未完全生效: minOk=%d maxOk=%d",
                        minOk ? 1 : 0,
                        maxOk ? 1 : 0);
        }
    }

    // v4.2: 使用自定义频率值设置 GPU（应用画像用）
    void gpuFreqControlCustom(const string_t& customMin, const string_t& customMax) {
        int mhzMin = Fastatoi(customMin.c_str());
        int mhzMax = Fastatoi(customMax.c_str());

        if (mhzMin <= 0 && mhzMax <= 0) return;

        bool minOk = true;
        bool maxOk = true;

        if (mhzMin > 0) {
            minOk = tryGpuMinPaths(mhzMin);
            if (minOk) {
                latestGpuMinMhz.store(mhzMin);
            }
        }

        if (mhzMax > 0) {
            maxOk = tryGpuMaxPaths(mhzMax);
            if (maxOk) {
                latestGpuMaxMhz.store(mhzMax);
            }
        }

        if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
            tryGpuPwrlevel(mhzMax, mhzMin);
        }

        logger.Debug("GPU频率(画像): min=%dMHz max=%dMHz", mhzMin, mhzMax);
    }

    void gpuFreqGuard() {
        if (!GpuFreq::enable) return;

        logger.Info("GPU守护已启动，周期: 3秒");

        // [Fix v4.1] 记录上一轮目标值，未变化时不重写（省 sysfs 抖动）
        // 如果其他进程篡改了 GPU 频率，sysfs 上的实际值会与 latestGpu*Mhz 不一致，
        // 但要每次都读回比较代价更大；折中方案：值未变化时只在每 N 轮强制写一次确认。
        int lastWrittenMin = -1;
        int lastWrittenMax = -1;
        int forceCounter   = 0;

        while (!gpuGuardShouldExit.load()) {
            int mhzMin = Fastatoi(GpuFreq::min_freq.c_str());
            int mhzMax = Fastatoi(GpuFreq::max_freq.c_str());

            // [Fix v4.6] 两个频率都未设置（都是 0/空） → 直接 sleep，避免误报"未生效"
            //            （这是合法状态：base mode 没配 GPU，AppProfile 也没配）
            if (mhzMin <= 0 && mhzMax <= 0) {
                for (int i = 0; i < 30 && !gpuGuardShouldExit.load(); i++) {
                    utils.sleep_ms(100);
                }
                continue;
            }

            {
                bool needWrite = (mhzMin != lastWrittenMin) || (mhzMax != lastWrittenMax);
                // 每 5 轮（15s）强制写一次，防止被其他模块覆盖
                if (++forceCounter >= 5) {
                    needWrite  = true;
                    forceCounter = 0;
                }
                if (needWrite) {
                    bool minOk = true, maxOk = true;
                    if (mhzMin > 0) {
                        minOk = tryGpuMinPaths(mhzMin);
                        if (minOk) latestGpuMinMhz.store(mhzMin);
                    }
                    if (mhzMax > 0) {
                        maxOk = tryGpuMaxPaths(mhzMax);
                        if (maxOk) latestGpuMaxMhz.store(mhzMax);
                    }
                    if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
                        bool pwrOk = tryGpuPwrlevel(mhzMax, mhzMin);
                        // [Fix v4.6] 三条路径都失败 → 明确告警，方便用户排查权限/驱动问题
                        if (!pwrOk) {
                            logger.Warn("GPU频率写入全部失败 (min=%d max=%d) — 检查 root/SELinux/驱动支持",
                                        mhzMin, mhzMax);
                        }
                    }
                    lastWrittenMin = mhzMin;
                    lastWrittenMax = mhzMax;
                }
            }

            for (int i = 0; i < 30 && !gpuGuardShouldExit.load(); i++) {
                utils.sleep_ms(100);
            }
        }

        logger.Info("GPU守护已停止");
    }

    void CfsSchedOpt() {
        if (!Scheduler::enable) return;

        utils.FileWrite("/proc/sys/kernel/sched_schedstats", Scheduler::Sched_schedstats ? "1" : "0");
        utils.FileWrite("/proc/sys/kernel/sched_latency_ns", Scheduler::Sched_latency_ns);
        utils.FileWrite("/proc/sys/kernel/sched_migration_cost_ns", Scheduler::Sched_migration_cost_ns);
        utils.FileWrite("/proc/sys/kernel/sched_min_granularity_ns", Scheduler::Sched_min_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_wakeup_granularity_ns", Scheduler::Sched_wakeup_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_nr_migrate", Scheduler::Sched_nr_migrate);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", Scheduler::Sched_util_clamp_min);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", Scheduler::Sched_util_clamp_max);

        if (checkEasSched()) {
            utils.FileWrite("/proc/sys/kernel/sched_energy_aware", Scheduler::Sched_energy_aware ? "1" : "0");
            logger.Info(Scheduler::Sched_energy_aware ? "已开启EAS调度器" : "已关闭EAS调度器");
        } else {
            logger.Warn("您的设备并不支持EAS调度器");
        }

        logger.Debug("Sched_energy_aware调整为: %s", Scheduler::Sched_energy_aware ? "开启" : "关闭");
        logger.Debug("Sched_schedstats调整为: %s", Scheduler::Sched_schedstats ? "开启" : "关闭");
        logger.Debug("Sched_latency_ns调整为: %s", Scheduler::Sched_latency_ns.c_str());
        logger.Debug("Sched_migration_cost_ns调整为: %s", Scheduler::Sched_migration_cost_ns.c_str());
        logger.Debug("Sched_wakeup_granularity_ns调整为: %s", Scheduler::Sched_wakeup_granularity_ns.c_str());
        logger.Debug("Sched_nr_migrate调整为: %s", Scheduler::Sched_nr_migrate.c_str());
        logger.Debug("Sched_util_clamp_min调整为: %s", Scheduler::Sched_util_clamp_min.c_str());
        logger.Debug("Sched_util_clamp_max调整为: %s", Scheduler::Sched_util_clamp_max.c_str());

        logger.Info("CFS调度器已优化完毕");
    }

    bool FeasFunc(bool Enable) {
        if (checkQcomFeas()) {
            utils.FileWrite(qcomFeas, Enable ? "1" : "0");
            logger.Debug("QCOM Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        }

        if (checkMtkFeas()) {
            utils.FileWrite(mtkFeas, Enable ? "1" : "0");
            logger.Debug("MTK Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        }

        return false;
    }

    bool checkQcom() const {
        return !access(qcomGpuPath, F_OK);
    }

private:
    bool checkQcomFeas() const {
        return !access(qcomFeas, F_OK);
    }

    bool checkMtkFeas() const {
        return !access(mtkFeas, F_OK);
    }

    bool checkCpuset() const {
        return !access(cpusetPath, F_OK);
    }

    bool checkEasSched() const {
        return !access(easSchedPath, F_OK);
    }

    bool isSchedulerAvailable(const char* schedulerPath, const char* schedulerName) const {
        char buf[256];

        int fd = open(schedulerPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;

        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (len <= 0) return false;

        buf[len] = '\0';

        return strstr(buf, schedulerName) != nullptr;
    }

    int findBlockDevices(char devices[][32], int maxDevices) const {
        DIR* dir = opendir("/sys/block/");
        if (!dir) return 0;

        int count = 0;
        struct dirent* entry;

        while ((entry = readdir(dir)) != nullptr && count < maxDevices) {
            if (entry->d_name[0] == '.') continue;

            if (strstr(entry->d_name, "sd") || strstr(entry->d_name, "mmcblk")) {
                strncpy(devices[count], entry->d_name, 31);
                devices[count][31] = '\0';
                count++;
            }
        }

        closedir(dir);

        return count;
    }

};