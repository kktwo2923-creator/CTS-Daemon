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

class Utils {
public:
    void FileWrite(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath, 0666);
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

    // scaling_governor 等节点不能用 O_NONBLOCK（内核返回 EAGAIN 导致静默失败），失败时解锁权限重试
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

    // affected_cpus 全簇 offline 时为空，首字符不是数字即无在线核
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
        if (fd < 0) return;

        char tmp[16];
        auto len = FastSnprintf(tmp, sizeof(tmp), "%d", value);
        write(fd, tmp, len);
        close(fd);
    }

    void popenShell(const char* cmd, char* buf, size_t buf_size) {
        if (!buf || buf_size == 0) return;
        buf[0] = '\0';
        auto fp = popen(cmd, "r");
        if (!fp) return;
        if (fgets(buf, buf_size, fp) == nullptr) buf[0] = '\0';
        pclose(fp);
    }

    // 读 /dev/cpuset/top-app/tasks + /proc/<pid>/cmdline 取前台包名，无 popen 开销；失败返回空串
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
                cmd[cn] = '\0';
                // 去掉 ":xxx" 子进程后缀
                char* colon = strchr(cmd, ':');
                if (colon) *colon = '\0';
                if (strchr(cmd, '.') && !strchr(cmd, '/'))
                    best = cmd;
            }
            if (!best.empty()) return best;
        }
        return "";
    }

    // 判断包名是否仍在 top-app cpuset：小窗盖住游戏时游戏进程通常仍留在 top-app
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
                char* colon = strchr(cmd, ':');
                if (colon) *colon = '\0';
                if (strcmp(cmd, pkg) == 0) return true;
            }
        }
        return false;
    }

    // 包进程是否还存活：扫 /proc/<pid>/cmdline，主进程或 pkg:子进程任一在即算运行。
    // 用于"游戏档保活到退出"——前台离开但进程未死时维持画像。
    bool isPackageRunning(const char* pkg) {
        if (!pkg || !*pkg) return false;
        DIR* d = opendir("/proc");
        if (!d) return false;
        bool found = false;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            bool num = true;
            for (const char* p = e->d_name; *p; ++p)
                if (*p < '0' || *p > '9') { num = false; break; }
            if (!num) continue;
            char cmdPath[64];
            snprintf(cmdPath, sizeof(cmdPath), "/proc/%s/cmdline", e->d_name);
            int cfd = open(cmdPath, O_RDONLY);
            if (cfd < 0) continue;
            char cmd[256] = { 0 };
            ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
            close(cfd);
            if (cn <= 0) continue;
            cmd[cn] = '\0';
            char* colon = strchr(cmd, ':');
            if (colon) *colon = '\0';
            if (strcmp(cmd, pkg) == 0) { found = true; break; }
        }
        closedir(d);
        return found;
    }

    // topPkg = Z 序最顶的可见标准 Task（过滤输入法/通知栏等非 standard Task）
    // visible = 所有 visible=true 的 Task 包名（供 isPackageVisible 判小窗场景）
    struct ForegroundSnapshot {
        std::string topPkg;
        std::vector<std::string> visible;
        bool valid = false;
    };

    ForegroundSnapshot parseForegroundOnce() {
        ForegroundSnapshot s;
        char buf[16384] = { 0 };
        popenRead("dumpsys activity activities 2>/dev/null | grep 'Task{'", buf, sizeof(buf));
        if (!buf[0]) return s;
        s.valid = true;
        char* save = nullptr;
        for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
            char* a = strstr(line, "A=");       // 形如 A=<uid>:<pkg>
            if (!a) continue;
            char* colon = strchr(a, ':');
            if (!colon) continue;
            char pkg[160];
            int k = 0;
            for (char* p = colon + 1; *p && *p != ' ' && *p != '}' && *p != '\t'
                                       && k < (int)sizeof(pkg) - 1; ++p)
                pkg[k++] = *p;
            pkg[k] = '\0';
            if (k == 0) continue;
            if (!strstr(line, "visible=true")) continue;
            s.visible.emplace_back(pkg);
            if (s.topPkg.empty() && strstr(line, "type=standard"))
                s.topPkg = pkg;
        }
        return s;
    }

    ForegroundSnapshot getForegroundCached(int ttlMs) {
        using clock = std::chrono::steady_clock;
        static std::mutex m;
        static ForegroundSnapshot last;
        static clock::time_point lastTime{};
        auto now = clock::now();
        {
            std::lock_guard<std::mutex> lk(m);
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
            if (lastTime.time_since_epoch().count() != 0 && since < ttlMs && last.valid)
                return last;
        }
        ForegroundSnapshot s = parseForegroundOnce();
        if (s.valid) {
            std::lock_guard<std::mutex> lk(m);
            last = s;
            lastTime = now;
        }
        return s;
    }

    // ColorOS 小窗报 mode=fullscreen 而非 freeform，故用 visible=true 判游戏是否仍在渲染
    bool isPackageVisible(const char* pkg) {
        if (!pkg || !*pkg) return false;
        ForegroundSnapshot s = getForegroundCached(1200);
        if (!s.valid) return false;
        for (const auto& v : s.visible)
            if (v == pkg) return true;
        return false;
    }


    // mCurrentFocus 作为 getTopApp 回退：支持多用户（u0/u10...），"null" 表示熄屏/锁屏
    string getTopAppFocus() {
        char data[1024] = { 0 };
        popenRead("dumpsys window 2>/dev/null | grep mCurrentFocus", data, sizeof(data));
        if (!data[0]) return "";
        bool sawNull = (strstr(data, "mCurrentFocus=null") != nullptr);

        // 扫描 " u<digits> " 模式，取其后到 '/' 的包名
        for (char* w = data; (w = strstr(w, " u")) != nullptr; ++w) {
            char* p = w + 2;
            if (*p < '0' || *p > '9') continue;
            while (*p >= '0' && *p <= '9') ++p;
            if (*p != ' ') continue;
            ++p;
            char* slash = strchr(p, '/');
            if (!slash || slash <= p) continue;
            ptrdiff_t len = slash - p;
            if (len <= 0 || len >= 256) continue;
            char temp[256];
            memcpy(temp, p, len);
            temp[len] = '\0';
            if (strchr(temp, ' ') || strchr(temp, '{') || strchr(temp, '}'))
                continue;
            return string(temp);
        }
        return sawNull ? string("null") : string("");
    }

    string getTopApp() {
        // 三级回退：① activities 最顶可见标准 Task → ② mCurrentFocus → ③ top-app cpuset
        ForegroundSnapshot s = getForegroundCached(0);
        if (s.valid && !s.topPkg.empty()) return s.topPkg;

        string focus = getTopAppFocus();
        if (focus == "null") return "null";
        if (!focus.empty()) return focus;

        string fast = getTopAppFast();
        if (!fast.empty()) return fast;
        return "";
    }

    // dumpsys 每次约 100~300ms，TTL 缓存减少调用频率
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
        if (!buf || len == 0) return 0;
        auto fp = popen(cmd, "r");
        if (!fp) { buf[0] = '\0'; return 0; }
        auto readLen = fread(buf, 1, len - 1, fp);
        pclose(fp);
        buf[readLen] = '\0';
        return readLen;
    }
};
