#pragma once

#include <iostream>
#include <fstream>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <stdarg.h>
#include <stddef.h>
#include <chrono>
#include <cstdio>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <memory>
#include "LibUtils.hpp"
#include "Json/string.hpp"

// Build configuration
#define DEBUG_DURATION 0
#define MAX_PKG_LEN 128
#define MAX_THREAD_LEN 128
#define CPU_POLICY 8 

using namespace LibUtils;

using string_t = qlib::string_t;

using std::atomic;
using std::stringstream;
using std::unordered_map;
using std::lock_guard;
using std::unique_ptr;
using std::ifstream;
using std::vector;
using std::string;
using std::string_view;
using std::thread;
using std::mutex;
using std::exception;
using std::make_unique;
using std::to_string;
using std::move;

enum class LOG_LEVEL : uint32_t {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

// [v4.7 cleanup] 已移除一批零调用方的死方法：
//   CachedWrite / WriteFile(O_TRUNC) / mkdirRecursive / InotifyMain / exec /
//   getPid / getTid / getActivity / readFrequencies / readString / getScreenProperty /
//   以及整组温控读取（getMaxCpuTemp / openZonePath / readTemp / checkSensorPath）——
//   后者随 Function::latestThermalLevel 一同删除（温控分支从未生效）。
class Utils {
public:
    void FileWrite(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath, 0666);
            // O_CREAT 必须带 mode 参数（POSIX + fortify-source）
            fd = open(filePath, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
        }

        if (fd >= 0) {
            write(fd, content, Faststrlen(content));
            close(fd);
        }
    }


    void FileWrite(const string& filePath, const string& content) noexcept {
        int fd = open(filePath.c_str(), O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath.c_str(), 0666);
            fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
            chmod(filePath.c_str(), 0444);
        }
    }


    void FileWrite(const char* filePath, const string_t& content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath, 0666);
            fd = open(filePath, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
        }
    }

    void sleep_ms(const int ms) {
        usleep(1000 * ms);
    }

    // 阻塞式写入: 用于 scaling_governor 等切换时需内核同步初始化的节点。
    // 不用 O_NONBLOCK(否则内核返回 EAGAIN 导致写入静默失败,表现为"调速器设置不生效");
    // 检查 write 返回值,失败则解锁权限重试一次。
    bool FileWriteBlocking(const char* filePath, const string_t& content) noexcept {
        for (int attempt = 0; attempt < 2; ++attempt) {
            int fd = open(filePath, O_WRONLY);
            if (fd < 0) {
                chmod(filePath, 0666);
                fd = open(filePath, O_WRONLY);
            }
            if (fd >= 0) {
                ssize_t n = write(fd, content.data(), content.size());
                close(fd);
                if (n == (ssize_t)content.size()) return true;
            }
            usleep(20000); // 20ms 后重试
        }
        return false;
    }

    int readInt(const char* path) noexcept {
        auto fd = open(path, O_RDONLY);
        if (fd < 0) return 0;

        char buff[16] = { 0 };
        auto len = read(fd, buff, sizeof(buff));
        close(fd);

        if (len <= 0) return 0;

        buff[15] = 0;
        return atoi(buff);
    }

    // 读文件首个非空白字符是否为数字。用于判断 cpufreq policyN/affected_cpus 是否非空
    // (该节点仅列出当前在线的核;全簇 offline 时为空)。返回 true=有在线核。
    bool FileStartsWithDigit(const char* path) noexcept {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return false;
        char buff[8] = { 0 };
        ssize_t len = read(fd, buff, sizeof(buff) - 1);
        close(fd);
        if (len <= 0) return false;
        for (ssize_t i = 0; i < len; i++) {
            char c = buff[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            return (c >= '0' && c <= '9');
        }
        return false;
    }

    void WriteInt(const char* path, int value) noexcept {
        auto fd = open(path, O_WRONLY);
        if (fd < 0) {
            chmod(path, 0666);
            fd = open(path, O_WRONLY);
        }
        if (fd < 0) return;  // Fix: 两次 open 均失败时直接返回，避免 write(-1,...) UB

        char tmp[16];
        auto len = FastSnprintf(tmp, sizeof(tmp), "%d", value);
        write(fd, tmp, len);
        close(fd);
        chmod(path, 0444);
    }

    // [Fix v4.3] 原版漏了大括号导致 pclose 在 while 循环体内，第二次 fgets 用已关闭 FILE* → UB
    // 只读第一行（dumpsys window | grep mCurrentFocus 输出本就只有一行）
    void popenShell(const char* cmd, char* buf, size_t buf_size) {
        if (!buf || buf_size == 0) return;
        buf[0] = '\0';
        auto fp = popen(cmd, "r");
        if (!fp) return;
        if (fgets(buf, buf_size, fp) == nullptr) buf[0] = '\0';
        pclose(fp);
    }

    // [优化] 轻量取前台包名: 读 /dev/cpuset/top-app/tasks 拿前台进程, 再读
    //   /proc/<pid>/cmdline 得包名。全程读文件, 无 popen(fork+exec shell) 开销,
    //   比 dumpsys(100~300ms)快两个数量级。失败返回空串, 调用方回退 dumpsys。
    //   取最后一个含包名特征('.'且无'/')的 cmdline 作为前台 app(top-app 组里
    //   靠后的通常是最近置顶的 app 进程, 而非其子服务)。
    string getTopAppFast() {
        // 候选 cpuset 路径(不同 Android 版本/cgroup 布局)
        static const char* taskPaths[] = {
            "/dev/cpuset/top-app/tasks",
            "/dev/cpuset/top-app/cgroup.procs",
        };
        for (const char* tp : taskPaths) {
            int fd = open(tp, O_RDONLY);
            if (fd < 0) continue;
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';

            string best;
            char* save = nullptr;
            for (char* tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(nullptr, "\n", &save)) {
                int pid = atoi(tok);
                if (pid <= 0) continue;
                char cmdPath[64];
                snprintf(cmdPath, sizeof(cmdPath), "/proc/%d/cmdline", pid);
                int cfd = open(cmdPath, O_RDONLY);
                if (cfd < 0) continue;
                char cmd[256] = { 0 };
                ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
                close(cfd);
                if (cn <= 0) continue;
                cmd[cn] = '\0';                 // cmdline 以 \0 分隔, 第一段即进程名
                // 去掉进程名里的 ":xxx" 子进程后缀
                char* colon = strchr(cmd, ':');
                if (colon) *colon = '\0';
                // 像包名: 含 '.', 不含 '/'(排除 /system/bin/... 这类原生进程)
                if (strchr(cmd, '.') && !strchr(cmd, '/'))
                    best = cmd;                  // 保留最后一个匹配
            }
            if (!best.empty()) return best;
        }
        return "";
    }

    // 指定包名是否仍在 top-app cpuset(即仍是前台/可见进程)。
    //   用于判断"在游戏上盖了个 App 小窗/浮窗": 浮窗 App 获焦时, 后台的全屏游戏进程
    //   通常仍留在 top-app → 据此判定游戏还在前台跑, 不应被小窗抢走画像。全程读文件。
    bool isPackageInTopApp(const char* pkg) {
        if (!pkg || !*pkg) return false;
        static const char* taskPaths[] = {
            "/dev/cpuset/top-app/tasks",
            "/dev/cpuset/top-app/cgroup.procs",
        };
        for (const char* tp : taskPaths) {
            int fd = open(tp, O_RDONLY);
            if (fd < 0) continue;
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            char* save = nullptr;
            for (char* tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(nullptr, "\n", &save)) {
                int pid = atoi(tok);
                if (pid <= 0) continue;
                char cmdPath[64];
                snprintf(cmdPath, sizeof(cmdPath), "/proc/%d/cmdline", pid);
                int cfd = open(cmdPath, O_RDONLY);
                if (cfd < 0) continue;
                char cmd[256] = { 0 };
                ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
                close(cfd);
                if (cn <= 0) continue;
                cmd[cn] = '\0';
                char* colon = strchr(cmd, ':');   // 去掉 ":xxx" 子进程后缀
                if (colon) *colon = '\0';
                if (strcmp(cmd, pkg) == 0) return true;
            }
        }
        return false;
    }

    // 指定包名的 Task 是否仍"可见"(visible=true)。ROM 无关的"游戏被小窗盖住"判定:
    //   开小窗时游戏还在后面渲染 → 游戏 Task 仍 visible=true; 真正切走/被全屏 App 完全
    //   盖住(occluded) → 游戏 visible=false。比"freeform 模式"可靠得多 —— 本机 ColorOS
    //   小窗(灵动窗口)报的是 mode=fullscreen 不是 freeform, 按窗口模式根本判不出。
    //   实现: dumpsys activity activities 的每个 Task 头行形如
    //     `Task{... A=10357:pkg U=0 visible=true visibleRequested=true mode=... }`
    //   只看 Task{ 头行(grep 缩小输出), 同一行里既含本包名又含 visible=true 即判可见。
    //   带 1.2s TTL 缓存(按包名), ROM 反复收窄 cpuset 触发本判定时不至于 dumpsys 连打。
    bool isPackageVisible(const char* pkg) {
        if (!pkg || !*pkg) return false;
        using clock = std::chrono::steady_clock;
        static std::mutex m;
        static std::string lastPkg;
        static bool lastResult = false;
        static clock::time_point lastTime{};
        auto now = clock::now();
        {
            std::lock_guard<std::mutex> lk(m);
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
            if (lastTime.time_since_epoch().count() != 0 && since < 1200 && lastPkg == pkg)
                return lastResult;
        }

        char buf[16384] = { 0 };
        // 只取 Task 头行(含 A=uid:pkg + visible= + mode=), 大幅缩小 dumpsys 输出
        popenRead("dumpsys activity activities 2>/dev/null | grep 'Task{'",
                  buf, sizeof(buf));
        bool found = false;
        char* save = nullptr;
        for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
            if (strstr(line, pkg) && strstr(line, "visible=true")) { found = true; break; }
        }
        {
            std::lock_guard<std::mutex> lk(m);
            lastPkg.assign(pkg);
            lastResult = found;
            lastTime   = now;
        }
        return found;
    }


    // dumpsys window 的 mCurrentFocus: 真正"获焦窗口"的包名, 是前台 App 的权威来源。
    //   "mCurrentFocus=null"(熄屏/锁屏无焦点)→ 返回 "null"; 解析不出(部分 ROM 无 window
    //   服务或格式不同)→ 返回空串, 由调用方回退轻量路径。
    string getTopAppFocus() {
        char data[256] = { 0 };  // 初始化，防止 popen 失败时读垃圾内存
        popenShell("dumpsys window | grep mCurrentFocus", data, sizeof(data));
        if (strstr(data, "mCurrentFocus=null")) return "null";

        // strstr/strchr 返回 nullptr 时直接 +offset 会 segfault，必须先判空
        const char* u0pos = strstr(data, "u0");
        if (!u0pos) return "";
        const char* ptr = u0pos + 3;

        const char* end_pos = strchr(ptr, '/');
        if (!end_pos) return "";

        char temp[256];
        ptrdiff_t len = end_pos - ptr;
        if (len <= 0 || len >= (ptrdiff_t)sizeof(temp)) return "";
        memcpy(temp, ptr, len);
        temp[len] = '\0';
        return string(temp);
    }

    string getTopApp() {
        // [Fix 前台检测] dumpsys mCurrentFocus 是真正"获焦窗口"的权威来源, 优先用它。
        //   top-app cpuset 的"取最后一个像包名的进程"启发式: 当多个 app 同时驻留 top-app
        //   (游戏 + 其后台/小窗/分屏进程)时会选错 → 返回错误或旧包名, 且因非空而永不回退
        //   dumpsys → 画像不切、卡在限核档(就是"打开王者也不切、不出 log"的根因)。
        //   故改为 dumpsys 优先; 仅当 dumpsys 不可用/解析不出时才回退轻量 cpuset 路径。
        //   dumpsys 只在前台切换事件 + 空闲复查时调用(上层已事件/TTL 节流), 不会高频 fork。
        string focus = getTopAppFocus();
        if (focus == "null") return "null";     // 明确无获焦窗口(熄屏/锁屏)→ 不切
        if (!focus.empty()) return focus;

        // dumpsys 拿不到(被裁剪/格式异常)→ 回退轻量 cpuset 路径
        string fast = getTopAppFast();
        if (!fast.empty()) return fast;
        return "";
    }

    // [新增 v4.3] 带 TTL 缓存的 getTopApp，省电关键
    //   dumpsys 是个昂贵操作（每次约 100~300ms），频繁调用很费电。
    //   minIntervalMs 内的请求直接返回上次结果，调用方应配合 inotify 触发
    string getTopAppCached(int minIntervalMs = 1500) {
        using clock = std::chrono::steady_clock;
        static std::mutex   m;
        static std::string  lastResult = "null";
        static clock::time_point lastTime{};
        auto now = clock::now();
        {
            std::lock_guard<std::mutex> lk(m);
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
            if (since < minIntervalMs && !lastResult.empty()) return lastResult;
        }
        std::string r = getTopApp();
        {
            std::lock_guard<std::mutex> lk(m);
            lastResult = r;
            lastTime   = now;
        }
        return r;
    }

    size_t popenRead(const char* cmd, char* buf, size_t len) {
        // [Fix] len==0 时 len-1 会回绕成 SIZE_MAX，fread 将无界写入 buf → 堆/栈溢出。
        if (!buf || len == 0) return 0;
        auto fp = popen(cmd, "r");
        if (!fp) { buf[0] = '\0'; return 0; }
        // Leave 1 byte for null terminator to prevent strtok/printf overflow
        auto readLen = fread(buf, 1, len - 1, fp);
        pclose(fp);
        buf[readLen] = '\0';  // CRITICAL: null-terminate to prevent buffer overflow
        return readLen;
    }
};
