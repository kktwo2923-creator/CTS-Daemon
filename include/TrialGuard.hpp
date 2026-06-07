#pragma once

// ============================================================
//  CTS 体验版守卫(24 小时试用) — 加固版
//
//  仅当编译期定义了 CTS_TRIAL 才启用; 正式版不定义 → 全部为空操作, 无任何限制。
//
//  机制: 首次启动记录(带签名的)时间戳; 超过 TRIAL_SECONDS(24h)后, 进程启动即失败退出
//        (main 返回非 0), 运行中到期也会自动停止。
//
//  ⚠ 重要: root 本地二进制无法做到"不可破解"(root 可改时间/删文件/反编译 patch)。
//     本守卫只"大幅抬高门槛"。真正强授权需联网由服务器签发/校验 token。
//
//  加固点:
//   1) keyed-hash 签名 —— 手改/伪造时间戳(first/last)→ 签名不符 → 判作弊到期; lastSeen 受
//      签名保护, 防回调才真正有效。
//   2) 设备指纹绑定 —— 签名混入 ro.serialno 派生的指纹 → 把已激活的时间戳文件拷到别的设备
//      无效(指纹不同→签名失配→判作弊)。
//   3) 反调试 —— 检测 TracerPid(frida/ptrace 动态 hook)→ 被调试即判到期。
//   4) 字符串混淆 —— 路径与命令编译期 XOR 编码, strings 抓不到明文。
//   5) 多点隐蔽冗余 + 防系统时间回调 + 防开机时钟未同步误判。
// ============================================================

#ifdef CTS_TRIAL

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

namespace TrialGuard {

    static constexpr long TRIAL_SECONDS = 24L * 60 * 60;   // 试用时长: 24 小时
    static constexpr long SANE_MIN      = 1577836800L;     // 2020-01-01, 低于此视为时钟未同步
    static constexpr long BACK_TOL      = 3600L;           // 时间回调容差(1h, 容 NTP 微调)
    static constexpr unsigned char XK   = 0x5A;            // 字符串混淆 XOR key

    // ---- 字符串混淆: 编译期 XOR 编码, 运行时解到栈 buffer(strings 抓不到明文)----
    template<int N> struct Obf {
        char d[N];
        constexpr Obf(const char (&s)[N]) : d{} {
            for (int i = 0; i < N; i++) d[i] = (char)(s[i] ^ XK);
        }
    };
    template<int N> inline void unobf(const Obf<N>& o, char* out) {
        for (int i = 0; i < N; i++) out[i] = (char)(o.d[i] ^ XK);
    }

    static constexpr int PATH_CNT = 5;
    // 取第 i 个时间戳路径(解码到 out, out 需 >= 64)
    inline void getPath(int i, char* out) {
        switch (i) {
            case 0: { constexpr Obf o("/data/adb/.cts_lic");            unobf(o, out); break; }
            case 1: { constexpr Obf o("/data/local/tmp/.sys_cache_d");  unobf(o, out); break; }
            case 2: { constexpr Obf o("/sdcard/Android/CTS/.lic");      unobf(o, out); break; }
            case 3: { constexpr Obf o("/sdcard/Android/.media_idx");    unobf(o, out); break; }
            case 4: { constexpr Obf o("/data/system/.cts_s");           unobf(o, out); break; }
            default: out[0] = '\0';
        }
    }

    // 64-bit 混合(finalizer)
    inline unsigned long long mix(unsigned long long x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }

    // 编译期密钥(拆片异或还原, 不以明文连续出现)
    inline unsigned long long secret() {
        volatile unsigned long long a = 0x9E3779B97F4A7C15ULL;
        volatile unsigned long long b = 0xD1B54A32D192ED03ULL;
        volatile unsigned long long c = 0xA0761D6478BD642FULL;
        return (unsigned long long)a ^ ((unsigned long long)b * 0x100000001B3ULL) ^ (unsigned long long)c;
    }

    // 设备指纹: 由 ro.serialno 派生(缓存)。取不到则退化为固定值(仍有签名, 只是不绑定设备)。
    inline unsigned long long deviceFingerprint() {
        static unsigned long long cached = 0;
        static bool done = false;
        if (done) return cached;
        done = true;
        unsigned long long h = 0xcbf29ce484222325ULL;     // FNV-1a 基
        char cmd[32];
        { constexpr Obf o("getprop ro.serialno"); unobf(o, cmd); }
        FILE* fp = popen(cmd, "r");
        if (fp) {
            char buf[128] = { 0 };
            size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
            pclose(fp);
            for (size_t i = 0; i < r; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') continue;
                h = (h ^ (unsigned char)buf[i]) * 0x100000001b3ULL;
            }
        }
        cached = mix(h);
        return cached;
    }

    // 对 (first,last) 计算 keyed-hash 签名(混入密钥 + 设备指纹)
    inline unsigned long long sign(long first, long last) {
        unsigned long long k = secret() ^ deviceFingerprint();
        unsigned long long h = mix(k ^ (unsigned long long)first);
        h = mix(h ^ (unsigned long long)last);
        h = mix(h ^ (k * 0x2545F4914F6CDD1DULL));
        return h;
    }

    // 反调试: TracerPid != 0 说明被 ptrace/frida 附加
    inline bool beingTraced() {
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd < 0) return false;
        char b[1024];
        ssize_t n = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n <= 0) return false;
        b[n] = '\0';
        const char* t = strstr(b, "TracerPid:");
        if (!t) return false;
        t += 10;
        while (*t == ' ' || *t == '\t') ++t;
        return (*t != '0' && *t >= '0' && *t <= '9');
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
        if (sign(f, l) != sig) return TAMPERED;   // 签名不符(被改 / 换设备)= 作弊
        first = f; last = l;
        return VALID;
    }

    inline void writeAll(long first, long last) {
        unsigned long long sig = sign(first, last);
        char b[96];
        int len = snprintf(b, sizeof(b), "%ld %ld %llx", first, last, sig);
        if (len <= 0) return;
        for (int i = 0; i < PATH_CNT; i++) {
            char path[64]; getPath(i, path);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) continue;
            ssize_t w = write(fd, b, (size_t)len);
            (void)w;
            close(fd);
        }
    }

    // 返回 true = 允许启动/继续; false = 已到期(或检测到作弊/被调试)。
    inline bool allowed() {
        if (beingTraced()) return false;            // 反调试

        long now = (long)time(nullptr);
        if (now < SANE_MIN) return true;            // 时钟未同步 → 放行不计时

        long first = 0, lastSeen = 0;
        for (int i = 0; i < PATH_CNT; i++) {
            char path[64]; getPath(i, path);
            long f = 0, l = 0;
            RecState st = readOne(path, f, l);
            if (st == TAMPERED) return false;       // 存在但签名非法 → 作弊
            if (st == VALID) {
                if (f >= SANE_MIN && (first == 0 || f < first)) first = f;
                if (l > lastSeen) lastSeen = l;
            }
        }

        if (first == 0) first = now;                // 首次运行

        if (lastSeen >= SANE_MIN && now < lastSeen - BACK_TOL) return false;  // 防回调

        long newest = now > lastSeen ? now : lastSeen;
        writeAll(first, newest);

        return (now - first) < TRIAL_SECONDS;
    }

    // 剩余秒数(用于提示; 已到期/作弊返回 0)。
    inline long remainSeconds() {
        long now = (long)time(nullptr);
        if (now < SANE_MIN) return TRIAL_SECONDS;
        long first = 0;
        for (int i = 0; i < PATH_CNT; i++) {
            char path[64]; getPath(i, path);
            long f = 0, l = 0;
            if (readOne(path, f, l) == VALID && f >= SANE_MIN && (first == 0 || f < first))
                first = f;
        }
        if (first == 0) first = now;
        long left = TRIAL_SECONDS - (now - first);
        return left > 0 ? left : 0;
    }

}   // namespace TrialGuard

#endif  // CTS_TRIAL
