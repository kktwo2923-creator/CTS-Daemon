#pragma once
// 授权校验(离线):App 联网验证后把服务器签发的令牌写入 license.json,
// 守护开机读取并本地校验 —— 令牌 = HMAC_SHA256(SIGNING_SECRET, "卡密|设备|到期")。
// 设备绑定核对本机 ro.serialno,防跨机复制;到期按 UTC 比较。无需联网、无需 TLS。
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace lic {

// 项目里 qlib 命名空间也定义了 uint*,且常被 using namespace 引入全局,
// 这里显式锚定到标准库类型,避免无限定名查找时二义。
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;
using std::size_t;

// ---------------- SHA-256 (public-domain 实现) ----------------
struct Sha256 {
    uint32_t s[8];
    uint64_t len = 0;
    uint8_t  buf[64];
    size_t   blen = 0;
    static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    void init() {
        static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        memcpy(s, iv, sizeof iv); len = 0; blen = 0;
    }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^((~e)&g);
            uint32_t t1=h+S1+ch+k[i]+w[i];
            uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+mj;
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = 64 - blen; if (take > n) take = n;
            memcpy(buf+blen, p, take); blen += take; p += take; n -= take;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0; while (blen != 56) update(&z, 1);
        uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = uint8_t(bits >> (56 - i*8));
        update(L, 8);
        for (int i = 0; i < 8; i++) { out[i*4]=uint8_t(s[i]>>24);out[i*4+1]=uint8_t(s[i]>>16);out[i*4+2]=uint8_t(s[i]>>8);out[i*4+3]=uint8_t(s[i]); }
    }
};

inline void sha256(const uint8_t* p, size_t n, uint8_t out[32]) {
    Sha256 c; c.init(); c.update(p, n); c.final(out);
}

// HMAC-SHA256 → 64 字符小写 hex
inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    uint8_t k[64] = {0};
    if (key.size() > 64) { sha256((const uint8_t*)key.data(), key.size(), k); }
    else memcpy(k, key.data(), key.size());
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i]^0x36; opad[i] = k[i]^0x5c; }
    uint8_t inner[32];
    { Sha256 c; c.init(); c.update(ipad,64); c.update((const uint8_t*)msg.data(), msg.size()); c.final(inner); }
    uint8_t mac[32];
    { Sha256 c; c.init(); c.update(opad,64); c.update(inner,32); c.final(mac); }
    static const char* hx = "0123456789abcdef";
    std::string out; out.resize(64);
    for (int i = 0; i < 32; i++) { out[i*2]=hx[mac[i]>>4]; out[i*2+1]=hx[mac[i]&0xf]; }
    return out;
}

// ---------------- 简单 JSON 扁平字段提取 ----------------
// 取 "name":"value"(字符串)或 "name":null/数字。找不到返回空串。
inline std::string jsonStr(const std::string& j, const char* name) {
    std::string pat = std::string("\"") + name + "\"";
    size_t p = j.find(pat); if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size()); if (p == std::string::npos) return "";
    p++;
    while (p < j.size() && (j[p]==' '||j[p]=='\t'||j[p]=='\n'||j[p]=='\r')) p++;
    if (p >= j.size()) return "";
    if (j[p] == '"') {
        p++; std::string out;
        while (p < j.size() && j[p] != '"') { if (j[p]=='\\' && p+1<j.size()) p++; out += j[p++]; }
        return out;
    }
    // null / 数字 / 其它裸值
    size_t e = p; while (e < j.size() && j[e]!=','&&j[e]!='}'&&j[e]!=' '&&j[e]!='\n'&&j[e]!='\r') e++;
    std::string v = j.substr(p, e-p);
    if (v == "null") return "";
    return v;
}

// 读文件全部内容
inline std::string readAll(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return "";
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b,1,sizeof b,f)) > 0) s.append(b, n);
    fclose(f); return s;
}

// 本机设备号 ro.serialno
inline std::string deviceId() {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = {0};
    int n = __system_property_get("ro.serialno", v);
    if (n > 0) return std::string(v, n);
    return "";
#else
    const char* e = getenv("FAKE_SERIALNO");
    return e ? std::string(e) : std::string("");
#endif
}

// 当前 UTC，ISO8601(与服务器 expires_at 同格式，可字典序比较)
inline std::string nowIsoUtc() {
    time_t t = time(nullptr); struct tm g; gmtime_r(&t, &g);
    char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S.000Z", &g);
    return b;
}

struct Result { bool ok; std::string reason; std::string licenseKey; std::string expiresAt; };

// 校验 license.json。SIGNING_SECRET 必须与服务器一致。
inline Result check(const std::string& signingSecret,
                    const char* path = "/sdcard/Android/CTS/license.json") {
    std::string j = readAll(path);
    if (j.empty()) return {false, "未找到授权文件(license.json),请在 App 内输入卡密激活", "", ""};
    std::string key = jsonStr(j, "license_key");
    std::string dev = jsonStr(j, "device_id");
    std::string exp = jsonStr(j, "expires_at");   // 永久为空
    std::string tok = jsonStr(j, "token");
    if (key.empty() || dev.empty() || tok.empty())
        return {false, "授权文件字段缺失", key, exp};
    std::string self = deviceId();
    if (self.empty()) return {false, "无法读取本机设备号(ro.serialno)", key, exp};
    if (self != dev)  return {false, "授权绑定的是其它设备,请联系卖家解绑", key, exp};
    std::string msg = key + "|" + dev + "|" + (exp.empty() ? std::string("0") : exp);
    std::string want = hmac_sha256_hex(signingSecret, msg);
    if (want != tok) return {false, "授权令牌无效(被篡改或密钥不符)", key, exp};
    if (!exp.empty() && exp < nowIsoUtc())
        return {false, "授权已过期,请续费", key, exp};
    return {true, "ok", key, exp};
}

} // namespace lic
