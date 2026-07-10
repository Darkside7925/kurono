//  kurono os - 802.11i security crypto primitives (satoru)
//  see wifi_crypto.h. from-scratch sha1/hmac/pbkdf2/aes/cmac/prf/ccmp, no libc.
//  validated against published 802.11i + rfc test vectors (ieee80211_test.cpp).
//  ref: fips-197 (aes), rfc 3174 (sha1), rfc 2104 (hmac), rfc 2898 (pbkdf2),
//  rfc 3394 (key wrap), rfc 4493 (cmac), ieee 802.11-2016 §12 (prf + ccmp).
//  code is original; only the algorithms (open standards) follow those docs. (satoru)

#include "wifi_crypto.h"

namespace WifiCrypto {

// small freestanding helpers so we don't drag in libc here (memcpy/memset exist
// in the kernel but local primitives keep this file self-contained). (satoru)
static inline void wc_memset(void* p, uint8_t v, uint32_t n) {
    uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = v;
}
static inline void wc_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* db = (uint8_t*)d; const uint8_t* sb = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) db[i] = sb[i];
}
static inline void wc_xor(uint8_t* dst, const uint8_t* a, const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = a[i] ^ b[i];
}
static inline uint32_t wc_rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// ────────────────────────────────────────────────────────────────────────
//  sha1 (rfc 3174)
// ────────────────────────────────────────────────────────────────────────
static void sha1_compress(uint32_t h[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++)
        w[i] = wc_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        uint32_t t = wc_rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = wc_rotl32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void Sha1Init(Sha1Ctx* c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->total_len = 0; c->block_len = 0;
}

void Sha1Update(Sha1Ctx* c, const uint8_t* data, uint32_t len) {
    c->total_len += len;
    while (len > 0) {
        uint32_t take = WC_SHA1_BLOCK - c->block_len;
        if (take > len) take = len;
        wc_memcpy(c->block + c->block_len, data, take);
        c->block_len += take; data += take; len -= take;
        if (c->block_len == WC_SHA1_BLOCK) {
            sha1_compress(c->h, c->block);
            c->block_len = 0;
        }
    }
}

void Sha1Final(Sha1Ctx* c, uint8_t out[WC_SHA1_DIGEST]) {
    uint64_t bits = c->total_len * 8;
    uint8_t pad = 0x80;
    Sha1Update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->block_len != 56) Sha1Update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    Sha1Update(c, lenbuf, 8);
    // block_len is now 0; emit big-endian state. (satoru)
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void Sha1(const uint8_t* data, uint32_t len, uint8_t out[WC_SHA1_DIGEST]) {
    Sha1Ctx c; Sha1Init(&c); Sha1Update(&c, data, len); Sha1Final(&c, out);
}

// ────────────────────────────────────────────────────────────────────────
//  hmac-sha1 (rfc 2104)
// ────────────────────────────────────────────────────────────────────────
void HmacSha1_2(const uint8_t* key, uint32_t key_len,
                const uint8_t* d1, uint32_t l1,
                const uint8_t* d2, uint32_t l2,
                uint8_t out[WC_SHA1_DIGEST]) {
    uint8_t k0[WC_SHA1_BLOCK];
    wc_memset(k0, 0, WC_SHA1_BLOCK);
    if (key_len > WC_SHA1_BLOCK) {
        Sha1(key, key_len, k0);   // keys longer than the block are hashed first (satoru)
    } else {
        wc_memcpy(k0, key, key_len);
    }
    uint8_t ipad[WC_SHA1_BLOCK], opad[WC_SHA1_BLOCK];
    for (int i = 0; i < WC_SHA1_BLOCK; i++) { ipad[i] = k0[i] ^ 0x36; opad[i] = k0[i] ^ 0x5C; }

    uint8_t inner[WC_SHA1_DIGEST];
    Sha1Ctx c; Sha1Init(&c);
    Sha1Update(&c, ipad, WC_SHA1_BLOCK);
    if (d1 && l1) Sha1Update(&c, d1, l1);
    if (d2 && l2) Sha1Update(&c, d2, l2);
    Sha1Final(&c, inner);

    Sha1Init(&c);
    Sha1Update(&c, opad, WC_SHA1_BLOCK);
    Sha1Update(&c, inner, WC_SHA1_DIGEST);
    Sha1Final(&c, out);
}

void HmacSha1(const uint8_t* key, uint32_t key_len,
              const uint8_t* data, uint32_t data_len,
              uint8_t out[WC_SHA1_DIGEST]) {
    HmacSha1_2(key, key_len, data, data_len, nullptr, 0, out);
}

// ────────────────────────────────────────────────────────────────────────
//  pbkdf2-hmac-sha1 (rfc 2898)
// ────────────────────────────────────────────────────────────────────────
void Pbkdf2HmacSha1(const uint8_t* pass, uint32_t pass_len,
                    const uint8_t* salt, uint32_t salt_len,
                    uint32_t iterations,
                    uint8_t* dk, uint32_t dk_len) {
    uint32_t blocks = (dk_len + WC_SHA1_DIGEST - 1) / WC_SHA1_DIGEST;
    uint32_t out_pos = 0;
    for (uint32_t i = 1; i <= blocks; i++) {
        // u1 = prf(pass, salt || int(i)) - big-endian block index. (satoru)
        uint8_t ibuf[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16),
                            (uint8_t)(i >> 8), (uint8_t)(i) };
        uint8_t u[WC_SHA1_DIGEST], t[WC_SHA1_DIGEST];
        HmacSha1_2(pass, pass_len, salt, salt_len, ibuf, 4, u);
        wc_memcpy(t, u, WC_SHA1_DIGEST);
        for (uint32_t j = 1; j < iterations; j++) {
            uint8_t un[WC_SHA1_DIGEST];
            HmacSha1(pass, pass_len, u, WC_SHA1_DIGEST, un);
            wc_memcpy(u, un, WC_SHA1_DIGEST);
            for (int k = 0; k < WC_SHA1_DIGEST; k++) t[k] ^= u[k];
        }
        uint32_t copy = dk_len - out_pos;
        if (copy > WC_SHA1_DIGEST) copy = WC_SHA1_DIGEST;
        wc_memcpy(dk + out_pos, t, copy);
        out_pos += copy;
    }
}

// ────────────────────────────────────────────────────────────────────────
//  aes-128 (fips-197)
// ────────────────────────────────────────────────────────────────────────
static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t AES_INV_SBOX[256];
static bool aes_inv_ready = false;
static void aes_build_inv_sbox() {
    if (aes_inv_ready) return;
    for (int i = 0; i < 256; i++) AES_INV_SBOX[AES_SBOX[i]] = (uint8_t)i;
    aes_inv_ready = true;
}

static const uint8_t AES_RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

// gf(2^8) multiply (russian-peasant), used by mixcolumns. (satoru)
static inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

void Aes128Init(Aes128Ctx* c, const uint8_t key[16]) {
    aes_build_inv_sbox();
    uint32_t* rk = c->rk;
    for (int i = 0; i < 4; i++)
        rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
                ((uint32_t)key[4*i+2] << 8) | ((uint32_t)key[4*i+3]);
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            // rotword + subword + rcon. (satoru)
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)AES_SBOX[(t >> 24) & 0xFF] << 24) |
                ((uint32_t)AES_SBOX[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)AES_SBOX[(t >> 8) & 0xFF] << 8) |
                ((uint32_t)AES_SBOX[t & 0xFF]);
            t ^= ((uint32_t)AES_RCON[i / 4] << 24);
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

static inline void aes_add_round_key(uint8_t s[16], const uint32_t* rk) {
    for (int c = 0; c < 4; c++) {
        uint32_t k = rk[c];
        s[4*c]   ^= (uint8_t)(k >> 24);
        s[4*c+1] ^= (uint8_t)(k >> 16);
        s[4*c+2] ^= (uint8_t)(k >> 8);
        s[4*c+3] ^= (uint8_t)(k);
    }
}

void Aes128EncryptBlock(const Aes128Ctx* c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    wc_memcpy(s, in, 16);
    aes_add_round_key(s, &c->rk[0]);
    for (int round = 1; round <= 10; round++) {
        // subbytes (satoru)
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
        // shiftrows: state is column-major (s[4*col+row]). (satoru)
        uint8_t t[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                t[4*col + row] = s[4*((col + row) & 3) + row];
        wc_memcpy(s, t, 16);
        // mixcolumns (skipped on the final round) (satoru)
        if (round != 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t a0 = s[4*col], a1 = s[4*col+1], a2 = s[4*col+2], a3 = s[4*col+3];
                s[4*col]   = (uint8_t)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
                s[4*col+1] = (uint8_t)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
                s[4*col+2] = (uint8_t)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
                s[4*col+3] = (uint8_t)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
            }
        }
        aes_add_round_key(s, &c->rk[4*round]);
    }
    wc_memcpy(out, s, 16);
}

void Aes128DecryptBlock(const Aes128Ctx* c, const uint8_t in[16], uint8_t out[16]) {
    aes_build_inv_sbox();
    uint8_t s[16];
    wc_memcpy(s, in, 16);
    aes_add_round_key(s, &c->rk[40]);
    for (int round = 9; round >= 0; round--) {
        // inv shiftrows (row r shifts right by r) (satoru)
        uint8_t t[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                t[4*col + row] = s[4*((col - row) & 3) + row];
        wc_memcpy(s, t, 16);
        // inv subbytes (satoru)
        for (int i = 0; i < 16; i++) s[i] = AES_INV_SBOX[s[i]];
        aes_add_round_key(s, &c->rk[4*round]);
        // inv mixcolumns (not on the round that follows the initial key add) (satoru)
        if (round != 0) {
            for (int col = 0; col < 4; col++) {
                uint8_t a0 = s[4*col], a1 = s[4*col+1], a2 = s[4*col+2], a3 = s[4*col+3];
                s[4*col]   = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
                s[4*col+1] = (uint8_t)(gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
                s[4*col+2] = (uint8_t)(gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11));
                s[4*col+3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14));
            }
        }
    }
    wc_memcpy(out, s, 16);
}

// ────────────────────────────────────────────────────────────────────────
//  aes key wrap / unwrap (rfc 3394)
// ────────────────────────────────────────────────────────────────────────
bool AesUnwrap(const uint8_t kek[16], const uint8_t* cipher, uint32_t cipher_len,
               uint8_t* out, uint32_t* out_len) {
    if (cipher_len < 24 || (cipher_len % 8) != 0) return false;
    uint32_t n = cipher_len / 8 - 1;            // number of 64-bit data blocks (satoru)
    Aes128Ctx c; Aes128Init(&c, kek);

    uint8_t a[8];
    wc_memcpy(a, cipher, 8);                     // a = c[0] (the integrity iv) (satoru)
    // r[1..n] = c[1..n] (we store them in out, 0-indexed) (satoru)
    for (uint32_t i = 0; i < n; i++) wc_memcpy(out + i * 8, cipher + (i + 1) * 8, 8);

    // 6 rounds, j = 5..0, i = n..1 (rfc 3394 §2.2.2) (satoru)
    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint32_t)j + (uint32_t)i;
            uint8_t blk[16], dec[16];
            // a ^ t (t is a 64-bit big-endian counter xored into a) (satoru)
            uint8_t at[8];
            wc_memcpy(at, a, 8);
            for (int b = 0; b < 8; b++) at[7 - b] ^= (uint8_t)(t >> (8 * b));
            wc_memcpy(blk, at, 8);
            wc_memcpy(blk + 8, out + (i - 1) * 8, 8);
            Aes128DecryptBlock(&c, blk, dec);
            wc_memcpy(a, dec, 8);                 // a = msb64(dec) (satoru)
            wc_memcpy(out + (i - 1) * 8, dec + 8, 8); // r[i] = lsb64(dec) (satoru)
        }
    }

    // integrity check: a must equal the default iv a6a6a6a6a6a6a6a6 (satoru)
    for (int b = 0; b < 8; b++) if (a[b] != 0xA6) return false;
    if (out_len) *out_len = n * 8;
    return true;
}

bool AesWrap(const uint8_t kek[16], const uint8_t* plain, uint32_t plain_len,
             uint8_t* out, uint32_t* out_len) {
    if (plain_len < 16 || (plain_len % 8) != 0) return false;
    uint32_t n = plain_len / 8;
    Aes128Ctx c; Aes128Init(&c, kek);

    uint8_t a[8];
    for (int b = 0; b < 8; b++) a[b] = 0xA6;
    uint8_t* r = out + 8;                         // r[1..n] start at out+8 (satoru)
    wc_memcpy(r, plain, plain_len);

    for (int j = 0; j <= 5; j++) {
        for (uint32_t i = 1; i <= n; i++) {
            uint8_t blk[16], enc[16];
            wc_memcpy(blk, a, 8);
            wc_memcpy(blk + 8, r + (i - 1) * 8, 8);
            Aes128EncryptBlock(&c, blk, enc);
            wc_memcpy(a, enc, 8);
            uint64_t t = (uint64_t)n * (uint32_t)j + i;
            for (int b = 0; b < 8; b++) a[7 - b] ^= (uint8_t)(t >> (8 * b));
            wc_memcpy(r + (i - 1) * 8, enc + 8, 8);
        }
    }
    wc_memcpy(out, a, 8);
    if (out_len) *out_len = plain_len + 8;
    return true;
}

// ────────────────────────────────────────────────────────────────────────
//  aes-cmac (rfc 4493)
// ────────────────────────────────────────────────────────────────────────
// left-shift a 16-byte block by one bit (used to derive subkeys k1/k2) (satoru)
static void cmac_lshift(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry = (b & 0x80) ? 1 : 0;
    }
}

void AesCmac(const uint8_t key[16], const uint8_t* msg, uint32_t msg_len,
             uint8_t mac[16]) {
    Aes128Ctx c; Aes128Init(&c, key);
    // subkey generation (satoru)
    uint8_t zero[16]; wc_memset(zero, 0, 16);
    uint8_t l[16]; Aes128EncryptBlock(&c, zero, l);
    uint8_t k1[16], k2[16];
    cmac_lshift(l, k1);
    if (l[0] & 0x80) k1[15] ^= 0x87;
    cmac_lshift(k1, k2);
    if (k1[0] & 0x80) k2[15] ^= 0x87;

    uint32_t n = (msg_len + 15) / 16;
    bool complete;
    if (n == 0) { n = 1; complete = false; }
    else complete = (msg_len % 16) == 0;

    uint8_t mlast[16];
    uint32_t last_off = (n - 1) * 16;
    if (complete) {
        wc_xor(mlast, msg + last_off, k1, 16);
    } else {
        uint32_t rem = msg_len - last_off;
        uint8_t pad[16]; wc_memset(pad, 0, 16);
        for (uint32_t i = 0; i < rem; i++) pad[i] = msg[last_off + i];
        pad[rem] = 0x80;
        wc_xor(mlast, pad, k2, 16);
    }

    uint8_t x[16]; wc_memset(x, 0, 16);
    uint8_t y[16];
    for (uint32_t i = 0; i < n - 1; i++) {
        wc_xor(y, x, msg + i * 16, 16);
        Aes128EncryptBlock(&c, y, x);
    }
    wc_xor(y, x, mlast, 16);
    Aes128EncryptBlock(&c, y, mac);
}

// ────────────────────────────────────────────────────────────────────────
//  ieee 802.11 prf (§12.7.1.2)
// ────────────────────────────────────────────────────────────────────────
void Prf(const uint8_t* key, uint32_t key_len,
         const char* label,
         const uint8_t* data, uint32_t data_len,
         uint8_t* out, uint32_t out_bits) {
    uint32_t out_len = (out_bits + 7) / 8;
    uint32_t label_len = 0; while (label[label_len]) label_len++;
    uint32_t pos = 0;
    uint8_t i = 0;
    // prf(k, a, b) = HMAC-SHA1(k, a || 0x00 || b || i) for i = 0,1,2,... (satoru)
    while (pos < out_len) {
        uint8_t digest[WC_SHA1_DIGEST];
        // build a || 0x00 || b || i in one buffer (label is short). (satoru)
        // we feed it segment-wise via a small temp to avoid a big stack array. (satoru)
        Sha1Ctx ic, oc;
        // manual hmac so we can stream the four pieces (label, sep, data, i) (satoru)
        uint8_t k0[WC_SHA1_BLOCK]; wc_memset(k0, 0, WC_SHA1_BLOCK);
        if (key_len > WC_SHA1_BLOCK) Sha1(key, key_len, k0);
        else wc_memcpy(k0, key, key_len);
        uint8_t ipad[WC_SHA1_BLOCK], opad[WC_SHA1_BLOCK];
        for (int b = 0; b < WC_SHA1_BLOCK; b++) { ipad[b] = k0[b] ^ 0x36; opad[b] = k0[b] ^ 0x5C; }
        uint8_t sep = 0x00;
        Sha1Init(&ic);
        Sha1Update(&ic, ipad, WC_SHA1_BLOCK);
        Sha1Update(&ic, (const uint8_t*)label, label_len);
        Sha1Update(&ic, &sep, 1);
        if (data && data_len) Sha1Update(&ic, data, data_len);
        Sha1Update(&ic, &i, 1);
        Sha1Final(&ic, digest);
        Sha1Init(&oc);
        Sha1Update(&oc, opad, WC_SHA1_BLOCK);
        Sha1Update(&oc, digest, WC_SHA1_DIGEST);
        Sha1Final(&oc, digest);

        uint32_t copy = out_len - pos;
        if (copy > WC_SHA1_DIGEST) copy = WC_SHA1_DIGEST;
        wc_memcpy(out + pos, digest, copy);
        pos += copy;
        i++;
    }
}

void DerivePtk(const uint8_t pmk[32],
               const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t anonce[32], const uint8_t snonce[32],
               uint8_t* ptk, uint32_t ptk_len) {
    // data = min(aa,spa) || max(aa,spa) || min(anonce,snonce) || max(anonce,snonce)
    // ieee 802.11-2016 §12.7.1.3. (satoru)
    uint8_t data[6 + 6 + 32 + 32];
    int cmp_mac = 0;
    for (int i = 0; i < 6; i++) { if (aa[i] != spa[i]) { cmp_mac = (aa[i] < spa[i]) ? -1 : 1; break; } }
    const uint8_t* m_lo = (cmp_mac <= 0) ? aa : spa;
    const uint8_t* m_hi = (cmp_mac <= 0) ? spa : aa;
    int cmp_n = 0;
    for (int i = 0; i < 32; i++) { if (anonce[i] != snonce[i]) { cmp_n = (anonce[i] < snonce[i]) ? -1 : 1; break; } }
    const uint8_t* n_lo = (cmp_n <= 0) ? anonce : snonce;
    const uint8_t* n_hi = (cmp_n <= 0) ? snonce : anonce;

    wc_memcpy(data + 0, m_lo, 6);
    wc_memcpy(data + 6, m_hi, 6);
    wc_memcpy(data + 12, n_lo, 32);
    wc_memcpy(data + 44, n_hi, 32);

    Prf(pmk, 32, "Pairwise key expansion", data, sizeof(data), ptk, ptk_len * 8);
}

// ────────────────────────────────────────────────────────────────────────
//  ccmp = aes-ccm with 802.11 framing (§12.5.3)
//  L=2 (length field), M=8 (mic). nonce is 13 bytes -> flags use L'=L-1=1. (satoru)
// ────────────────────────────────────────────────────────────────────────
void CcmpEncrypt(const uint8_t tk[16],
                 const uint8_t nonce[13],
                 const uint8_t* aad, uint32_t aad_len,
                 const uint8_t* data, uint32_t data_len,
                 uint8_t* out) {
    Aes128Ctx c; Aes128Init(&c, tk);

    // ── cbc-mac to compute the mic ───────────────────────────────────── (satoru)
    // b0: flags || nonce || l(data). flags = 64*Adata + 8*M' + L'. (satoru)
    uint8_t mac_flags = (uint8_t)(0x40 /*adata*/ + (((8 - 2) / 2) << 3) /*M'=3*/ + 1 /*L'=1*/);
    uint8_t b[16];
    b[0] = mac_flags;
    wc_memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(data_len >> 8);
    b[15] = (uint8_t)(data_len);
    uint8_t x[16];
    Aes128EncryptBlock(&c, b, x);

    // aad block: 2-byte length prefix (aad < 2^16-2^8) then aad, zero-padded. (satoru)
    uint8_t ab[16]; wc_memset(ab, 0, 16);
    ab[0] = (uint8_t)(aad_len >> 8);
    ab[1] = (uint8_t)(aad_len);
    uint32_t a_in_first = (aad_len < 14) ? aad_len : 14;
    wc_memcpy(ab + 2, aad, a_in_first);
    for (int i = 0; i < 16; i++) ab[i] ^= x[i];
    Aes128EncryptBlock(&c, ab, x);
    uint32_t a_done = a_in_first;
    while (a_done < aad_len) {
        uint8_t blk[16]; wc_memset(blk, 0, 16);
        uint32_t take = (aad_len - a_done < 16) ? (aad_len - a_done) : 16;
        wc_memcpy(blk, aad + a_done, take);
        for (int i = 0; i < 16; i++) blk[i] ^= x[i];
        Aes128EncryptBlock(&c, blk, x);
        a_done += take;
    }

    // payload blocks into the mac (satoru)
    uint32_t p_done = 0;
    while (p_done < data_len) {
        uint8_t blk[16]; wc_memset(blk, 0, 16);
        uint32_t take = (data_len - p_done < 16) ? (data_len - p_done) : 16;
        wc_memcpy(blk, data + p_done, take);
        for (int i = 0; i < 16; i++) blk[i] ^= x[i];
        Aes128EncryptBlock(&c, blk, x);
        p_done += take;
    }
    // x now holds T (the full 16-byte mac); we keep the first 8 bytes. (satoru)

    // ── ctr mode to encrypt payload + mic ─────────────────────────────── (satoru)
    // a_i: flags(L'=1) || nonce || counter(2). counter 0 encrypts the mic. (satoru)
    uint8_t ctr_flags = 0x01;   // L'=1 (satoru)
    auto ctr_keystream = [&](uint16_t counter, uint8_t ks[16]) {
        uint8_t a[16];
        a[0] = ctr_flags;
        wc_memcpy(a + 1, nonce, 13);
        a[14] = (uint8_t)(counter >> 8);
        a[15] = (uint8_t)(counter);
        Aes128EncryptBlock(&c, a, ks);
    };

    // encrypt mic with s0 (satoru)
    uint8_t s0[16]; ctr_keystream(0, s0);
    uint8_t mic[8];
    for (int i = 0; i < 8; i++) mic[i] = x[i] ^ s0[i];

    // encrypt payload with s1, s2, ... (satoru)
    uint16_t counter = 1;
    uint32_t off = 0;
    while (off < data_len) {
        uint8_t ks[16]; ctr_keystream(counter, ks);
        uint32_t take = (data_len - off < 16) ? (data_len - off) : 16;
        for (uint32_t i = 0; i < take; i++) out[off + i] = data[off + i] ^ ks[i];
        off += take; counter++;
    }
    wc_memcpy(out + data_len, mic, 8);
}

bool CcmpDecrypt(const uint8_t tk[16],
                 const uint8_t nonce[13],
                 const uint8_t* aad, uint32_t aad_len,
                 const uint8_t* cipher, uint32_t cipher_len,
                 uint8_t* out) {
    if (cipher_len < 8) return false;
    uint32_t data_len = cipher_len - 8;
    Aes128Ctx c; Aes128Init(&c, tk);

    uint8_t ctr_flags = 0x01;
    auto ctr_keystream = [&](uint16_t counter, uint8_t ks[16]) {
        uint8_t a[16];
        a[0] = ctr_flags;
        wc_memcpy(a + 1, nonce, 13);
        a[14] = (uint8_t)(counter >> 8);
        a[15] = (uint8_t)(counter);
        Aes128EncryptBlock(&c, a, ks);
    };

    // decrypt payload (satoru)
    uint16_t counter = 1;
    uint32_t off = 0;
    while (off < data_len) {
        uint8_t ks[16]; ctr_keystream(counter, ks);
        uint32_t take = (data_len - off < 16) ? (data_len - off) : 16;
        for (uint32_t i = 0; i < take; i++) out[off + i] = cipher[off + i] ^ ks[i];
        off += take; counter++;
    }
    // recover the transmitted mic (satoru)
    uint8_t s0[16]; ctr_keystream(0, s0);
    uint8_t recv_mic[8];
    for (int i = 0; i < 8; i++) recv_mic[i] = cipher[data_len + i] ^ s0[i];

    // recompute the cbc-mac over aad + decrypted payload and compare (satoru)
    uint8_t mac_flags = (uint8_t)(0x40 + (((8 - 2) / 2) << 3) + 1);
    uint8_t b[16];
    b[0] = mac_flags;
    wc_memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(data_len >> 8);
    b[15] = (uint8_t)(data_len);
    uint8_t x[16];
    Aes128EncryptBlock(&c, b, x);

    uint8_t ab[16]; wc_memset(ab, 0, 16);
    ab[0] = (uint8_t)(aad_len >> 8);
    ab[1] = (uint8_t)(aad_len);
    uint32_t a_in_first = (aad_len < 14) ? aad_len : 14;
    wc_memcpy(ab + 2, aad, a_in_first);
    for (int i = 0; i < 16; i++) ab[i] ^= x[i];
    Aes128EncryptBlock(&c, ab, x);
    uint32_t a_done = a_in_first;
    while (a_done < aad_len) {
        uint8_t blk[16]; wc_memset(blk, 0, 16);
        uint32_t take = (aad_len - a_done < 16) ? (aad_len - a_done) : 16;
        wc_memcpy(blk, aad + a_done, take);
        for (int i = 0; i < 16; i++) blk[i] ^= x[i];
        Aes128EncryptBlock(&c, blk, x);
        a_done += take;
    }
    uint32_t p_done = 0;
    while (p_done < data_len) {
        uint8_t blk[16]; wc_memset(blk, 0, 16);
        uint32_t take = (data_len - p_done < 16) ? (data_len - p_done) : 16;
        wc_memcpy(blk, out + p_done, take);
        for (int i = 0; i < 16; i++) blk[i] ^= x[i];
        Aes128EncryptBlock(&c, blk, x);
        p_done += take;
    }
    // constant-ish compare of the first 8 mac bytes (satoru)
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)(x[i] ^ recv_mic[i]);
    return diff == 0;
}

} // namespace WifiCrypto
// end (satoru)
