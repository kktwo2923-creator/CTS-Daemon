#pragma once

// CTS 体验版 24h 试用守卫（仅 CTS_TRIAL 时启用，正式版为空操作）
// 加固：keyed-hash 签名防篡改、设备指纹防文件迁移、TracerPid 反调试、路径 XOR 混淆

#ifdef CTS_TRIAL

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

namespace TrialGuard {

    static constexpr long TRIAL_SECONDS = 24L * 60 * 60;
    static constexpr long SANE_MIN      = 1577836800L;     // 2020-01-01，低于此视为时钟未同步
    static constexpr long BACK_TOL      = 3600L;           // NTP 微调容差
    static constexpr unsigned char XK   = 0x5A;

    // 编译期 XOR 混淆，strings 工具无法抓到明文路径
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

    inline unsigned long long mix(unsigned long long x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }

    // 密钥拆片异或，防止明文连续出现在二进制中
    inline unsigned long long secret() {
        volatile unsigned long long a = 0x9E3779B97F4A7C15ULL;
        volatile unsigned long long b = 0xD1B54A32D192ED03ULL;
        volatile unsigned long long c = 0xA0761D6478BD642FULL;
        return (unsigned long long)a ^ ((unsigned long long)b * 0x100000001B3ULL) ^ (unsigned long long)c;
    }

    // ro.serialno 派生的设备指纹（FNV-1a），取不到则退化为固定值
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

    inline unsigned long long sign(long first, long last) {
        unsigned long long k = secret() ^ deviceFingerprint();
        unsigned long long h = mix(k ^ (unsigned long long)first);
        h = mix(h ^ (unsigned long long)last);
        h = mix(h ^ (k * 0x2545F4914F6CDD1DULL));
        return h;
    }

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
        if (sign(f, l) != sig) return TAMPERED;
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

    inline bool allowed() {
        if (beingTraced()) return false;

        long now = (long)time(nullptr);
        if (now < SANE_MIN) return true;            // 时钟未同步放行

        long first = 0, lastSeen = 0;
        for (int i = 0; i < PATH_CNT; i++) {
            char path[64]; getPath(i, path);
            long f = 0, l = 0;
            RecState st = readOne(path, f, l);
            if (st == TAMPERED) return false;
            if (st == VALID) {
                if (f >= SANE_MIN && (first == 0 || f < first)) first = f;
                if (l > lastSeen) lastSeen = l;
            }
        }

        if (first == 0) first = now;

        if (lastSeen >= SANE_MIN && now < lastSeen - BACK_TOL) return false;  // 防时钟回调

        long newest = now > lastSeen ? now : lastSeen;
        writeAll(first, newest);

        return (now - first) < TRIAL_SECONDS;
    }

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
