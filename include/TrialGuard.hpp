#pragma once

// ============================================================
//  CTS 体验版守卫(24 小时试用)
//
//  仅当编译期定义了 CTS_TRIAL 才启用; 正式版不定义 → 全部为空操作, 无任何限制。
//
//  机制: 首次启动记录时间戳; 超过 TRIAL_SECONDS(24h)后, 进程启动即失败退出
//        (main 返回非 0), 运行中到期也会自动停止。
//
//  防绕过(体验版级别, 非强加密):
//   1) 时间戳写多个隐藏位置 —— 删其中之一不能重置(以最早的首启时间为准)。
//   2) 防系统时间往回调 —— 当前时间显著早于"上次见到的时间"判为作弊, 直接到期。
//   3) 防开机初期时间未同步误判 —— 当前时间早于 SANE_MIN(2020-01-01)说明 RTC/NTP
//      还没就绪, 此时放行且不记录, 等时间同步后再开始计时。
// ============================================================

#ifdef CTS_TRIAL

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>

namespace TrialGuard {

    static constexpr long TRIAL_SECONDS = 24L * 60 * 60;   // 试用时长: 24 小时
    static constexpr long SANE_MIN      = 1577836800L;     // 2020-01-01, 低于此视为时间未同步
    static constexpr long BACK_TOL      = 3600L;           // 允许的时间回调容差(1h, 容 NTP 微调)

    // 时间戳存放位置(多点冗余, 删一个不能重置)
    inline const char* const* paths(int& n) {
        static const char* P[] = {
            "/data/adb/.cts_trial",
            "/sdcard/Android/CTS/.trial",
            "/data/local/tmp/.cts_trial",
        };
        n = (int)(sizeof(P) / sizeof(P[0]));
        return P;
    }

    inline void readOne(const char* p, long& first, long& last) {
        first = 0; last = 0;
        int fd = open(p, O_RDONLY);
        if (fd < 0) return;
        char b[64] = { 0 };
        ssize_t r = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (r <= 0) return;
        b[r] = '\0';
        long f = 0, l = 0;
        if (sscanf(b, "%ld %ld", &f, &l) >= 1) { first = f; last = l; }
    }

    inline void writeAll(long first, long last) {
        int n = 0; const char* const* P = paths(n);
        char b[64];
        int len = snprintf(b, sizeof(b), "%ld %ld", first, last);
        if (len <= 0) return;
        for (int i = 0; i < n; i++) {
            int fd = open(P[i], O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) continue;
            ssize_t w = write(fd, b, (size_t)len);
            (void)w;
            close(fd);
        }
    }

    // 返回 true = 允许启动/继续运行; false = 已到期(或检测到作弊)。
    inline bool allowed() {
        long now = (long)time(nullptr);
        if (now < SANE_MIN) return true;          // 时间未同步 → 放行, 不记录(留待同步后计时)

        // 汇总所有位置: 取最早的首启时间, 最新的"上次见到时间"
        long first = 0, lastSeen = 0;
        int n = 0; const char* const* P = paths(n);
        for (int i = 0; i < n; i++) {
            long f = 0, l = 0;
            readOne(P[i], f, l);
            if (f >= SANE_MIN && (first == 0 || f < first)) first = f;
            if (l > lastSeen) lastSeen = l;
        }

        // 首次运行(或此前记录的是不可信的早时间)→ 以现在为首启时间
        if (first == 0) first = now;

        // 防回调: 当前时间明显早于上次见到 → 判作弊, 直接到期
        if (lastSeen >= SANE_MIN && now < lastSeen - BACK_TOL) return false;

        long newest = now > lastSeen ? now : lastSeen;
        writeAll(first, newest);                  // 回写(顺带修复被删的位置)

        return (now - first) < TRIAL_SECONDS;
    }

    // 剩余秒数(用于提示; 已到期返回 0)。
    inline long remainSeconds() {
        long now = (long)time(nullptr);
        if (now < SANE_MIN) return TRIAL_SECONDS;
        long first = 0, lastSeen = 0;
        int n = 0; const char* const* P = paths(n);
        for (int i = 0; i < n; i++) {
            long f = 0, l = 0; readOne(P[i], f, l);
            if (f >= SANE_MIN && (first == 0 || f < first)) first = f;
            (void)lastSeen;
        }
        if (first == 0) first = now;
        long left = TRIAL_SECONDS - (now - first);
        return left > 0 ? left : 0;
    }

}   // namespace TrialGuard

#endif  // CTS_TRIAL
