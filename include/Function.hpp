#pragma once

#include <cstring>
#include <cerrno>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
    std::mutex guardMtx_;
    std::condition_variable guardCv_;

    // GPU 写路径缓存：命中后跳过整条探测级联(opendir+多次access)，节点失效自动回退重探
    struct GpuPathCache { char path[128] = {0}; bool isHz = false; bool needsGov = false; bool valid = false; };
    GpuPathCache gpuMinCache_, gpuMaxCache_;
    std::mutex gpuPathMtx_;

    // kgsl 可用频点表硬件固定，探测一次后缓存
    int kgslAvailFreqs_[64];
    int kgslAvailCount_ = -1;

    // 回读 min 频率的有效节点缓存
    const char* gpuMinReadPath_ = nullptr;
    bool gpuMinReadIsMhz_ = false;

public:
    Function() = default;

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    ~Function() {
        stopGuards();
    }

    // Init 时调用一次；重载 config.json 只需走 ReloadFunC
    void AllFunC() {
        cpusetFunction();
        gpuFreqControl();
        CfsSchedOpt();
        startGuards();
    }

    // 运行时重载用，不重启已运行的守护线程
    void ReloadFunC() {
        cpusetFunction();
        gpuFreqControl();
        CfsSchedOpt();
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
        {
            std::lock_guard<std::mutex> lk(guardMtx_);
            gpuGuardShouldExit.store(true);
        }
        guardCv_.notify_all();

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

        // 新 SoC（如 SM8850）GPU sysfs 默认 0444，open 失败时 chmod 后重试
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

    // 恢复 msm-adreno-tz 以保留 [min,max] 区间 DVFS；旧实现强制 performance 会使 min 失效
    void ensureKgslDvfsGovernor() {
        const char* govPath = "/sys/class/kgsl/kgsl-3d0/devfreq/governor";

        if (access(govPath, F_OK) != 0) return;

        char curGov[64] = {0};

        int fd = open(govPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        ssize_t n = read(fd, curGov, sizeof(curGov) - 1);
        close(fd);
        if (n <= 0) return;

        for (int i = 0; curGov[i]; i++) {
            if (curGov[i] == '\n' || curGov[i] == '\r') {
                curGov[i] = '\0';
                break;
            }
        }

        // 仅当 governor 被钉死在 performance 时介入，否则尊重系统现状
        if (!strstr(curGov, "performance")) return;

        chmod(govPath, 0666);

        int fdw = open(govPath, O_WRONLY | O_CLOEXEC);
        if (fdw >= 0) {
            const char gov[] = "msm-adreno-tz\n";
            write(fdw, gov, sizeof(gov) - 1);
            close(fdw);
            logger.Debug("GPU governor 由 performance 恢复为 msm-adreno-tz(启用区间 DVFS)");
        }
    }

    int readKgslAvailableFreqs(int freqsOut[]) {
        // SM8850+ 新内核可能不存在 gpu_available_frequencies，用 devfreq 兜底
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
            ensureKgslDvfsGovernor();
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
            if (kgslAvailCount_ < 0) kgslAvailCount_ = readKgslAvailableFreqs(kgslAvailFreqs_);
            int availCount = kgslAvailCount_;

            if (availCount > 0) {
                int snapped = snapGpuFreq(valueToWrite, kgslAvailFreqs_, availCount, isMax);

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

    // 联发科 Mali devfreq 地址前缀不固定，遍历 /sys/class/devfreq/ 找 ".mali" 节点
    bool findMaliDevfreqPath(const char* which, char* outPath, size_t outLen) {
        DIR* dir = opendir("/sys/class/devfreq/");
        if (!dir) return false;
        struct dirent* entry;
        bool found = false;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            if (strstr(entry->d_name, ".mali") == nullptr) continue;
            FastSnprintf(outPath, outLen, "/sys/class/devfreq/%s/%s", entry->d_name, which);
            if (access(outPath, F_OK) == 0) { found = true; break; }
        }
        closedir(dir);
        return found;
    }

    // 写成功则记入缓存，后续直写该节点
    bool tryGpuPathCache(GpuPathCache& c, const char* path, int mhz,
                         bool isHz, bool needsGov, const char* label) {
        if (!tryGpuPath(path, mhz, isHz, needsGov, label)) return false;
        snprintf(c.path, sizeof(c.path), "%s", path);
        c.isHz = isHz;
        c.needsGov = needsGov;
        c.valid = true;
        return true;
    }

    bool tryGpuMaxPaths(int mhzMax) {
        if (mhzMax <= 0) return true;
        std::lock_guard<std::mutex> lk(gpuPathMtx_);

        if (gpuMaxCache_.valid) {
            if (tryGpuPath(gpuMaxCache_.path, mhzMax, gpuMaxCache_.isHz, gpuMaxCache_.needsGov, "max"))
                return true;
            gpuMaxCache_.valid = false; // 节点失效，重走级联探测
        }

        // 联发科 Mali：动态探测前缀，再固定地址兜底
        {
            char maliPath[128];
            if (findMaliDevfreqPath("max_freq", maliPath, sizeof(maliPath)) &&
                tryGpuPathCache(gpuMaxCache_, maliPath, mhzMax, true, false, "max"))
                return true;
        }
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/13000000.mali/max_freq", mhzMax, true, false, "max"))
            return true;
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/13040000.mali/max_freq", mhzMax, true, false, "max"))
            return true;

        // SM8850+ 新内核 devfreq 挂载在 /sys/class/devfreq/<addr>.qcom,kgsl-3d0/，旧软链可能不存在
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/5000000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        // 骁龙855/SM8150=2c00000, 骁龙800/801=fdb00000
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/2c00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;
        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/devfreq/fdb00000.qcom,kgsl-3d0/max_freq",
                       mhzMax, true, true, "max"))
            return true;

        if (tryGpuPathCache(gpuMaxCache_, "/sys/kernel/gpu/gpu_max_clock", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", mhzMax, true, true, "max"))
            return true;

        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/kgsl/kgsl-3d0/max_gpuclk", mhzMax, true, false, "max"))
            return true;

        if (tryGpuPathCache(gpuMaxCache_, "/sys/class/kgsl/kgsl-3d0/max_clock_mhz", mhzMax, false, false, "max"))
            return true;

        return false;
    }

    bool tryGpuMinPaths(int mhzMin) {
        if (mhzMin <= 0) return true;
        std::lock_guard<std::mutex> lk(gpuPathMtx_);

        if (gpuMinCache_.valid) {
            if (tryGpuPath(gpuMinCache_.path, mhzMin, gpuMinCache_.isHz, gpuMinCache_.needsGov, "min"))
                return true;
            gpuMinCache_.valid = false;
        }

        {
            char maliPath[128];
            if (findMaliDevfreqPath("min_freq", maliPath, sizeof(maliPath)) &&
                tryGpuPathCache(gpuMinCache_, maliPath, mhzMin, true, false, "min"))
                return true;
        }
        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/13000000.mali/min_freq", mhzMin, true, false, "min"))
            return true;
        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/13040000.mali/min_freq", mhzMin, true, false, "min"))
            return true;

        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/5000000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        // 骁龙855/SM8150=2c00000, 骁龙800/801=fdb00000
        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/2c00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;
        if (tryGpuPathCache(gpuMinCache_, "/sys/class/devfreq/fdb00000.qcom,kgsl-3d0/min_freq",
                       mhzMin, true, true, "min"))
            return true;

        if (tryGpuPathCache(gpuMinCache_, "/sys/kernel/gpu/gpu_min_clock", mhzMin, false, false, "min"))
            return true;

        if (tryGpuPathCache(gpuMinCache_, "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq", mhzMin, true, true, "min"))
            return true;

        if (tryGpuPathCache(gpuMinCache_, "/sys/class/kgsl/kgsl-3d0/min_clock_mhz", mhzMin, false, false, "min"))
            return true;

        return false;
    }

    bool tryGpuPwrlevel(int mhzMax, int mhzMin, bool& maxDone, bool& minDone) {
        maxDone = (mhzMax <= 0);
        minDone = (mhzMin <= 0);

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

        if (mhzMax > 0) {
            long long targetHzLL = static_cast<long long>(mhzMax) * 1000000LL;
            if (targetHzLL > 2147483647LL) return maxDone && minDone;

            int targetHz = static_cast<int>(targetHzLL);
            int maxPwr = freqCount - 1; // 默认最低档，防目标低于全部档位时误写 max_pwrlevel=0
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
                maxDone = true;
            }
        }

        if (mhzMin > 0) {
            long long targetHzLL = static_cast<long long>(mhzMin) * 1000000LL;
            if (targetHzLL > 2147483647LL) return maxDone && minDone;

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
                minDone = true;
            }
        }

        return maxDone && minDone;
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
            bool pwrMaxDone = false, pwrMinDone = false;
            tryGpuPwrlevel(maxOk ? 0 : mhzMax, minOk ? 0 : mhzMin, pwrMaxDone, pwrMinDone);
            if (!maxOk && pwrMaxDone) maxOk = true;
            if (!minOk && pwrMinDone) minOk = true;
        }

        if (minOk && maxOk) {
            logger.Info("GPU频率已设置: 目标 min=%dMHz max=%dMHz (实测 min=%dMHz)",
                        mhzMin, mhzMax, readCurrentGpuMinMhz());
        } else {
            logger.Warn("GPU频率设置可能未完全生效: minOk=%d maxOk=%d",
                        minOk ? 1 : 0,
                        maxOk ? 1 : 0);
        }
    }

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
            bool pwrMaxDone = false, pwrMinDone = false;
            tryGpuPwrlevel(maxOk ? 0 : mhzMax, minOk ? 0 : mhzMin, pwrMaxDone, pwrMinDone);
        }

        logger.Debug("GPU频率(画像): min=%dMHz max=%dMHz", mhzMin, mhzMax);
    }

    // 回读当前 GPU 最低频(MHz)。OnePlus 等 ROM 的 GPU 管理会周期性覆盖 min,
    // 守护据此判断是否被改动并立即纠正。读不到返回 -1。
    int readCurrentGpuMinMhz() {
        // 缓存有效节点：稳态每次回读只 open 一个节点，不重复探测 4 条路径
        if (gpuMinReadPath_) {
            int v = utils.readInt(gpuMinReadPath_);
            if (v > 0) return gpuMinReadIsMhz_ ? v : v / 1000000;
            gpuMinReadPath_ = nullptr;
        }
        static constexpr const char* mhzPath = "/sys/class/kgsl/kgsl-3d0/min_clock_mhz"; // 高通 kgsl, MHz
        int v = utils.readInt(mhzPath);
        if (v > 0) { gpuMinReadPath_ = mhzPath; gpuMinReadIsMhz_ = true; return v; }
        static const char* hzPaths[] = {
            "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/min_freq",
            "/sys/class/devfreq/5000000.qcom,kgsl-3d0/min_freq",
            "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq",
        };
        for (auto p : hzPaths) {
            v = utils.readInt(p);
            if (v > 0) { gpuMinReadPath_ = p; gpuMinReadIsMhz_ = false; return v / 1000000; } // Hz -> MHz
        }
        return -1;
    }

    void applyGpuFreqOnce(int mhzMin, int mhzMax) {
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
            bool pwrMaxDone = false, pwrMinDone = false;
            tryGpuPwrlevel(maxOk ? 0 : mhzMax, minOk ? 0 : mhzMin, pwrMaxDone, pwrMinDone);
            if (!maxOk && pwrMaxDone) maxOk = true;
            if (!minOk && pwrMinDone) minOk = true;
            if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
                logger.Warn("GPU频率写入未完全生效 (min=%d max=%d, minOk=%d maxOk=%d) — 检查 root/SELinux/驱动支持",
                            mhzMin, mhzMax, minOk ? 1 : 0, maxOk ? 1 : 0);
            }
        }
    }

    void gpuFreqGuard() {
        if (!GpuFreq::enable) return;

        // 自适应轮询：1s 快轮询防御 ROM(风驰/perfd)覆盖；连续 kStableN 次回读无漂移则
        // 退避一级(+1s, 上限 3s)；检测到漂移/目标变化立即恢复 1s。兜底强制重写仍按 ~10s。
        constexpr int kFastMs  = 1000;
        constexpr int kMaxMs   = 3000;
        constexpr int kStableN = 5;
        constexpr int kForceMs = 10000;

        logger.Info("GPU守护已启动: 1秒回读纠正, 稳定后退避至 %d 秒", kMaxMs / 1000);

        int lastTargetMin = -1;
        int lastTargetMax = -1;
        int intervalMs    = kFastMs;
        int stableCount   = 0;
        int sinceForceMs  = 0;

        std::unique_lock<std::mutex> lk(guardMtx_);
        while (!gpuGuardShouldExit.load()) {
            lk.unlock();
            int mhzMin = Fastatoi(GpuFreq::min_freq.c_str());
            int mhzMax = Fastatoi(GpuFreq::max_freq.c_str());

            if (mhzMin <= 0 && mhzMax <= 0) {
                intervalMs   = kFastMs;
                stableCount  = 0;
                sinceForceMs = 0;
            } else {
                bool targetChanged = (mhzMin != lastTargetMin) || (mhzMax != lastTargetMax);
                // 回读真实 min: 被 ROM 改回则立刻纠正
                bool drifted = false;
                if (mhzMin > 0) {
                    int cur = readCurrentGpuMinMhz();
                    if (cur > 0 && cur != mhzMin) drifted = true;
                }
                sinceForceMs += intervalMs;
                bool force = (sinceForceMs >= kForceMs);  // ~10s 兜底强制一次

                if (targetChanged || drifted || force) {
                    applyGpuFreqOnce(mhzMin, mhzMax);
                    lastTargetMin = mhzMin;
                    lastTargetMax = mhzMax;
                    if (force) sinceForceMs = 0;
                }

                if (targetChanged || drifted) {
                    if (drifted && intervalMs != kFastMs)
                        logger.Debug("GPU min 漂移(被系统覆盖), 恢复 1s 快轮询");
                    intervalMs  = kFastMs;
                    stableCount = 0;
                } else if (intervalMs < kMaxMs && ++stableCount >= kStableN) {
                    intervalMs  = (intervalMs + kFastMs > kMaxMs) ? kMaxMs : intervalMs + kFastMs;
                    stableCount = 0;
                }
            }

            lk.lock();
            // 单次定时等待替代 10×100ms 碎片睡眠，唤醒数 10/s → ≤1/s；退出时被 notify 即刻返回
            guardCv_.wait_for(lk, std::chrono::milliseconds(intervalMs),
                              [this] { return gpuGuardShouldExit.load(); });
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