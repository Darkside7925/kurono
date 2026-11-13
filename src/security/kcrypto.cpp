// kcrypto: see kcrypto.h. (satoru)
#include "kcrypto.h"
#ifndef KCRYPTO_HOSTTEST
#include "../net/wifi_crypto.h"
#endif

namespace KCrypto {

namespace {

const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

inline uint32_t rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t s[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

// hmac state with the inner and outer pads already absorbed, so pbkdf2 costs
// two compressions per iteration instead of four. (satoru)
struct Hmac { Sha256 in, out; };

void hmac_init(Hmac* h, const uint8_t* key, uint32_t key_len) {
    uint8_t k[64];
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (key_len > 64) Sha256Once(key, key_len, k);
    else for (uint32_t i = 0; i < key_len; i++) k[i] = key[i];
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    Sha256Init(&h->in); Sha256Update(&h->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    Sha256Init(&h->out); Sha256Update(&h->out, pad, 64);
    Wipe(k, sizeof(k)); Wipe(pad, sizeof(pad));
}

void hmac_finish(const Hmac* base, Sha256* inner, uint8_t out[32]) {
    uint8_t ih[32];
    Sha256Final(inner, ih);
    Sha256 o = base->out;
    Sha256Update(&o, ih, 32);
    Sha256Final(&o, out);
    Wipe(ih, sizeof(ih));
}

bool cpu_has_rdrand() {
    uint32_t a = 1, b, c, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 30) & 1;
}

uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

}  // namespace

void Sha256Init(Sha256* c) {
    static const uint32_t H[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    for (int i = 0; i < 8; i++) c->s[i] = H[i];
    c->bits = 0; c->n = 0;
}

void Sha256Update(Sha256* c, const void* data, uint32_t len) {
    const uint8_t* d = (const uint8_t*)data;
    c->bits += (uint64_t)len * 8u;
    while (len) {
        uint32_t t = 64 - c->n;
        if (t > len) t = len;
        for (uint32_t i = 0; i < t; i++) c->buf[c->n + i] = d[i];
        c->n += t; d += t; len -= t;
        if (c->n == 64) { compress(c->s, c->buf); c->n = 0; }
    }
}

void Sha256Final(Sha256* c, uint8_t out[32]) {
    uint64_t bits = c->bits;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        while (c->n < 64) c->buf[c->n++] = 0;
        compress(c->s, c->buf); c->n = 0;
    }
    while (c->n < 56) c->buf[c->n++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->n++] = (uint8_t)(bits >> (i * 8));
    compress(c->s, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->s[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->s[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->s[i] >> 8); out[i * 4 + 3] = (uint8_t)c->s[i];
    }
    Wipe(c, sizeof(*c));
}

void Sha256Once(const void* data, uint32_t len, uint8_t out[32]) {
    Sha256 c; Sha256Init(&c); Sha256Update(&c, data, len); Sha256Final(&c, out);
}

void HmacSha256(const uint8_t* key, uint32_t key_len, const void* data, uint32_t len,
                uint8_t out[32]) {
    Hmac h; hmac_init(&h, key, key_len);
    Sha256 in = h.in;
    Sha256Update(&in, data, len);
    hmac_finish(&h, &in, out);
    Wipe(&h, sizeof(h));
}

void Pbkdf2Sha256(const uint8_t* pass, uint32_t pass_len, const uint8_t* salt, uint32_t salt_len,
                  uint32_t iterations, uint8_t* dk, uint32_t dk_len) {
    if (iterations == 0) iterations = 1;
    Hmac h; hmac_init(&h, pass, pass_len);
    uint32_t block = 1;
    while (dk_len) {
        uint8_t ctr[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16),
                          (uint8_t)(block >> 8), (uint8_t)block};
        Sha256 in = h.in;
        Sha256Update(&in, salt, salt_len);
        Sha256Update(&in, ctr, 4);
        uint8_t u[32], t[32];
        hmac_finish(&h, &in, u);
        for (int i = 0; i < 32; i++) t[i] = u[i];
        for (uint32_t it = 1; it < iterations; it++) {
            Sha256 in2 = h.in;
            Sha256Update(&in2, u, 32);
            hmac_finish(&h, &in2, u);
            for (int i = 0; i < 32; i++) t[i] ^= u[i];
        }
        uint32_t take = dk_len < 32 ? dk_len : 32;
        for (uint32_t i = 0; i < take; i++) dk[i] = t[i];
        dk += take; dk_len -= take; block++;
        Wipe(u, sizeof(u)); Wipe(t, sizeof(t));
    }
    Wipe(&h, sizeof(h));
}

#ifndef KCRYPTO_HOSTTEST
void Aes128Ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out,
               uint32_t len) {
    WifiCrypto::Aes128Ctx ctx;
    WifiCrypto::Aes128Init(&ctx, key);
    uint8_t ctr[16], ks[16];
    for (int i = 0; i < 16; i++) ctr[i] = iv[i];
    uint32_t off = 0;
    while (off < len) {
        WifiCrypto::Aes128EncryptBlock(&ctx, ctr, ks);
        uint32_t take = len - off < 16 ? len - off : 16;
        for (uint32_t i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        off += take;
        for (int i = 15; i >= 12; i--) if (++ctr[i]) break;
    }
    Wipe(&ctx, sizeof(ctx)); Wipe(ks, sizeof(ks)); Wipe(ctr, sizeof(ctr));
}
#endif

bool Equal(const void* a, const void* b, uint32_t n) {
    const volatile uint8_t* x = (const volatile uint8_t*)a;
    const volatile uint8_t* y = (const volatile uint8_t*)b;
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

void Wipe(void* p, uint32_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    while (n--) *b++ = 0;
}

void Random(void* out, uint32_t n) {
    // whitened through sha-256 so a weak source (tsc only) still yields
    // uniform bytes; rdrand adds real entropy when present. (satoru)
    static uint64_t counter = 0;
    static int have = -1;
    if (have < 0) have = cpu_has_rdrand() ? 1 : 0;
    uint8_t* o = (uint8_t*)out;
    while (n) {
        uint64_t mix[6];
        mix[0] = rdtsc();
        mix[1] = ++counter;
        mix[2] = mix[3] = mix[4] = 0;
        if (have) {
            for (int i = 2; i < 5; i++) {
                for (int tries = 0; tries < 10; tries++) {
                    uint64_t v; uint8_t ok;
                    __asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
                    if (ok) { mix[i] = v; break; }
                }
            }
        }
        mix[5] = rdtsc();
        uint8_t d[32];
        Sha256Once(mix, sizeof(mix), d);
        uint32_t take = n < 32 ? n : 32;
        for (uint32_t i = 0; i < take; i++) o[i] = d[i];
        o += take; n -= take;
        Wipe(d, sizeof(d)); Wipe(mix, sizeof(mix));
    }
}

void ToHex(const uint8_t* in, uint32_t n, char* out) {
    static const char* H = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) { out[i * 2] = H[in[i] >> 4]; out[i * 2 + 1] = H[in[i] & 15]; }
    out[n * 2] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool FromHex(const char* in, uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int hi = hexval(in[i * 2]), lo = hi < 0 ? -1 : hexval(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

bool SelfTest() {
    uint8_t d[32]; char hex[65];
    Sha256Once("abc", 3, d); ToHex(d, 32, hex);
    const char* want1 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    HmacSha256((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, d);
    char hex2[65]; ToHex(d, 32, hex2);
    const char* want2 = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    uint8_t dk[64];
    Pbkdf2Sha256((const uint8_t*)"passwd", 6, (const uint8_t*)"salt", 4, 1, dk, 64);
    char hex3[129]; ToHex(dk, 64, hex3);
    const char* want3 = "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                        "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783";
    for (int i = 0; i < 64; i++) if (hex[i] != want1[i] || hex2[i] != want2[i]) return false;
    for (int i = 0; i < 128; i++) if (hex3[i] != want3[i]) return false;
    return true;
}

}  // namespace KCrypto

// end (satoru)
