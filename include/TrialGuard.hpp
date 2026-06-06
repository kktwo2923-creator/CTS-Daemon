#pragma once

// ============================================================
//  CTS 体验版守卫(24 小时试用) — 加强版
//
//  仅当编译期定义了 CTS_TRIAL 才启用; 正式版不定义 → 全部为空操作, 无任何限制。
//
//  机制: 首次启动记录(带签名的)时间戳; 超过 TRIAL_SECONDS(24h)后, 进程启动即失败退出
//        (main 返回非 0), 运行中到期也会自动停止。
//
//  ⚠ 重要: 这是 root 环境下的本地二进制, 无法做到"不可破解"(root 可改系统时间、删文件、
//     反编译/patch 二进制)。本守卫只负责"大幅抬高门槛", 挡住普通用户与脚本。真正强授权
//     需联网由服务器签发/校验 token。
//
//  加强点(相对明文时间戳):
//   1) 签名校验 —— 时间戳带 keyed-hash 签名(密钥编译期嵌入并拆分异或, 不以明文连续出现)。
//      手动编辑/伪造时间戳(改 first 成未来、伪造 lastSeen)→ 签名不符 → 判作弊, 直接到期。
//   2) 多点隐蔽冗余 —— 写多个不显眼位置, 删其一不能重置(以最早合法首启时间为准);
//      任一位置"存在但签名非法"→ 判作弊到期(强威慑被改文件)。
//   3) 防系统时间往回调 —— now 明显早于"上次见到时间"判作弊到期(1h 容差容 NTP 微调);
//      因 lastSeen 受签名保护无法伪造, 此防护才真正有效。
//   4) 防开机初期时间未同步误判 —— now 早于 SANE_MIN(2020-01-01)说明时钟未就绪 → 放行
//      且不计时, 等同步后再起算。
// ============================================================

#ifdef CTS_TRIAL

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>

namespace TrialGuard {

    static constexpr long TRIAL_SECONDS = 24L * 60 * 60;   // 试用时长: 24 小时
    static constexpr long SANE_MIN      = 1577836800L;     // 2020-01-01, 低于此视为时钟未同步
    static constexpr long BACK_TOL      = 3600L;           // 时间回调容差(1h, 容 NTP 微调)

    // 时间戳存放位置(多点冗余 + 不显眼命名)。删全部才能重置。
    inline const char* const* paths(int& n) {
        static const char* P[] = {
            "/data/adb/.cts_lic",
            "/data/local/tmp/.sys_cache_d",
            "/sdcard/Android/CTS/.lic",
            "/sdcard/Android/.media_idx",
            "/data/system/.cts_s",
        };
        n = (int)(sizeof(P) / sizeof(P[0]));
        return P;
    }

    // 64-bit 混合(finalizer), 用于 keyed-hash
    inline unsigned long long mix(unsigned long long x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }

    // 编译期密钥(拆成片段并异或还原, 避免在二进制里以明文连续字符串/常量出现)
    inline unsigned long long secret() {
        volatile unsigned long long a = 0x9E3779B97F4A7C15ULL;
        volatile unsigned long long b = 0xD1B54A32D192ED03ULL;
        volatile unsigned long long c = 0xA0761D6478BD642FULL;
        return (unsigned long long)a ^ ((unsigned long long)b * 0x100000001B3ULL) ^ (unsigned long long)c;
    }

    // 对 (first,last) 计算 keyed-hash 签名
    inline unsigned long long sign(long first, long last) {
        unsigned long long k = secret();
        unsigned long long h = mix(k ^ (unsigned long long)first);
        h = mix(h ^ (unsigned long long)(unsigned long long)last);
        h = mix(h ^ (k * 0x2545F4914F6CDD1DULL));
        return h;
    }

    enum RecState { ABSENT, VALID, TAMPERED };

    inline RecState readOne(const char* p, long& first, long& last) {
        first = 0; last = 0;
        int fd = open(p, O_RDONLY);
        if (fd < 0) return ABSENT;
        char b[96] = { 0 };
        ssize_t r = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (r <= 0) return ABSENT;
        b[r] = '\0';
        long f = 0, l = 0;
        unsigned long long sig = 0;
        if (sscanf(b, "%ld %ld %llx", &f, &l, &sig) != 3) return TAMPERED;
        if (sign(f, l) != (unsigned long long)sig) return TAMPERED;   // 签名不符 = 被改过
        first = f; last = l;
        return VALID;
    }

    inline void writeAll(long first, long last) {
        unsigned long long sig = sign(first, last);
        char b[96];
        int len = snprintf(b, sizeof(b), "%ld %ld %llx", first, last, (unsigned long long)sig);
        if (len <= 0) return;
        int n = 0; const char* const* P = paths(n);
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
        if (now < SANE_MIN) return true;          // 时钟未同步 → 放行, 不记录(留待同步后计时)

        long first = 0, lastSeen = 0;
        int n = 0; const char* const* P = paths(n);
        for (int i = 0; i < n; i++) {
            long f = 0, l = 0;
            RecState st = readOne(P[i], f, l);
            if (st == TAMPERED) return false;     // 存在但签名非法 → 作弊, 直接到期
            if (st == VALID) {
                if (f >= SANE_MIN && (first == 0 || f < first)) first = f;
                if (l > lastSeen) lastSeen = l;
            }
        }

        if (first == 0) first = now;              // 首次运行(无任何合法记录)

        // 防回调: 当前时间明显早于上次见到 → 判作弊, 直接到期
        if (lastSeen >= SANE_MIN && now < lastSeen - BACK_TOL) return false;

        long newest = now > lastSeen ? now : lastSeen;
        writeAll(first, newest);                  // 回写(顺带修复被删的位置, 刷新签名)

        return (now - first) < TRIAL_SECONDS;
    }

    // 剩余秒数(用于提示; 已到期/作弊返回 0)。
    inline long remainSeconds() {
        long now = (long)time(nullptr);
        if (now < SANE_MIN) return TRIAL_SECONDS;
        long first = 0;
        int n = 0; const char* const* P = paths(n);
        for (int i = 0; i < n; i++) {
            long f = 0, l = 0;
            if (readOne(P[i], f, l) == VALID && f >= SANE_MIN && (first == 0 || f < first))
                first = f;
        }
        if (first == 0) first = now;
        long left = TRIAL_SECONDS - (now - first);
        return left > 0 ? left : 0;
    }

}   // namespace TrialGuard

#endif  // CTS_TRIAL
