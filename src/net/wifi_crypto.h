#pragma once
//  kurono os - 802.11i security crypto primitives (satoru)
//
//  freestanding-from-scratch crypto used by the wpa2/wpa2-psk supplicant in the
//  ieee80211 stack. no libc, no openssl. everything here is byte-exact against
//  the published 802.11i / rfc test vectors (see ieee80211_test.cpp). (satoru)
//
//  contents:
//    - sha1 + hmac-sha1            (rfc 3174 / rfc 2104)
//    - pbkdf2-hmac-sha1           (rfc 2898; wpa psk = pbkdf2(pass, ssid, 4096, 256))
//    - aes-128 block cipher        (fips-197) - encrypt + decrypt
//    - aes key wrap / unwrap       (rfc 3394; used to unwrap the gtk in eapol-key 3)
//    - aes-cmac                    (rfc 4493 / nist sp 800-38b; akm 802.1x mic)
//    - ieee 802.11 prf             (ieee 802.11-2016 §12.7.1.2; ptk derivation)
//    - ccmp encrypt/decrypt        (ieee 802.11-2016 §12.5.3; aes-ctr + cbc-mac)
//  ref: linux net/mac80211 + wpa_supplicant for the protocol; code is original. (satoru)

#include "../kernel/types.h"

namespace WifiCrypto {

// ── sha1 ─────────────────────────────────────────────────────────────── (satoru)
// 20-byte digest. rfc 3174. (satoru)
#define WC_SHA1_DIGEST 20
#define WC_SHA1_BLOCK  64

struct Sha1Ctx {
    uint32_t h[5];
    uint64_t total_len;        // total message length in bytes (satoru)
    uint8_t  block[WC_SHA1_BLOCK];
    uint32_t block_len;        // bytes currently buffered in block[] (satoru)
};

void Sha1Init(Sha1Ctx* c);
void Sha1Update(Sha1Ctx* c, const uint8_t* data, uint32_t len);
void Sha1Final(Sha1Ctx* c, uint8_t out[WC_SHA1_DIGEST]);
// one-shot convenience. (satoru)
void Sha1(const uint8_t* data, uint32_t len, uint8_t out[WC_SHA1_DIGEST]);

// ── hmac-sha1 ────────────────────────────────────────────────────────── (satoru)
// rfc 2104. out is always 20 bytes. (satoru)
void HmacSha1(const uint8_t* key, uint32_t key_len,
              const uint8_t* data, uint32_t data_len,
              uint8_t out[WC_SHA1_DIGEST]);
// two-segment variant (data1 || data2) so callers needn't pre-concatenate. (satoru)
void HmacSha1_2(const uint8_t* key, uint32_t key_len,
                const uint8_t* d1, uint32_t l1,
                const uint8_t* d2, uint32_t l2,
                uint8_t out[WC_SHA1_DIGEST]);

// ── pbkdf2-hmac-sha1 ─────────────────────────────────────────────────── (satoru)
// rfc 2898. derives dk_len bytes. for wpa: pbkdf2(pass, ssid, 4096, 32). (satoru)
void Pbkdf2HmacSha1(const uint8_t* pass, uint32_t pass_len,
                    const uint8_t* salt, uint32_t salt_len,
                    uint32_t iterations,
                    uint8_t* dk, uint32_t dk_len);

// ── aes-128 ──────────────────────────────────────────────────────────── (satoru)
// fips-197. key schedule expanded once into the context. block = 16 bytes. (satoru)
#define WC_AES_BLOCK 16
#define WC_AES128_KEY 16

struct Aes128Ctx {
    uint32_t rk[44];           // 11 round keys × 4 words (aes-128 = 10 rounds) (satoru)
};

void Aes128Init(Aes128Ctx* c, const uint8_t key[WC_AES128_KEY]);
void Aes128EncryptBlock(const Aes128Ctx* c, const uint8_t in[16], uint8_t out[16]);
void Aes128DecryptBlock(const Aes128Ctx* c, const uint8_t in[16], uint8_t out[16]);

// ── aes key unwrap (rfc 3394) ────────────────────────────────────────── (satoru)
// unwraps n 64-bit blocks. cipher_len must be a multiple of 8 and >= 24 (one
// 8-byte iv + at least two data blocks). out_len receives cipher_len-8. returns
// true iff the integrity check (default iv 0xa6a6...) passes. used to recover
// the gtk from eapol-key msg 3's key-data field. (satoru)
bool AesUnwrap(const uint8_t kek[16], const uint8_t* cipher, uint32_t cipher_len,
               uint8_t* out, uint32_t* out_len);
// the matching wrap (for self-test round-trips). out_len = plain_len+8. (satoru)
bool AesWrap(const uint8_t kek[16], const uint8_t* plain, uint32_t plain_len,
             uint8_t* out, uint32_t* out_len);

// ── aes-cmac (rfc 4493) ──────────────────────────────────────────────── (satoru)
// 16-byte mac. used as the eapol-key mic when the akm negotiates aes (the
// "aes-128-cmac" key-descriptor version 3). (satoru)
void AesCmac(const uint8_t key[16], const uint8_t* msg, uint32_t msg_len,
             uint8_t mac[16]);

// ── ieee 802.11 prf ──────────────────────────────────────────────────── (satoru)
// ieee 802.11-2016 §12.7.1.2. prf-n: hmac-sha1 expansion of (key, label, data)
// to bits/8 bytes. ptk derivation uses prf-384 (ccmp) or prf-512 (tkip). the
// label for ptk is "Pairwise key expansion"; data = min(aa,spa)||max||min(anonce,
// snonce)||max. (satoru)
void Prf(const uint8_t* key, uint32_t key_len,
         const char* label,
         const uint8_t* data, uint32_t data_len,
         uint8_t* out, uint32_t out_bits);

// ── ptk derivation helper ────────────────────────────────────────────── (satoru)
// builds the prf data block from the two macs + two nonces in canonical (sorted)
// order and runs prf to produce ptk_len bytes (48 for ccmp, 64 for tkip). the
// ptk layout is kck(16) || kek(16) || tk(16[+8 tkip]). (satoru)
void DerivePtk(const uint8_t pmk[32],
               const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t anonce[32], const uint8_t snonce[32],
               uint8_t* ptk, uint32_t ptk_len);

// ── ccmp (ieee 802.11-2016 §12.5.3) ──────────────────────────────────── (satoru)
// aes-128 ccm with a 13-byte nonce, 8-byte mic, 2-byte L. encrypts `data_len`
// bytes of payload in place into out (which must hold data_len+8 for the mic).
// aad is the (masked) 802.11 header fields; nonce = priority||a2||pn(6). returns
// true always (no failure mode on encrypt). (satoru)
void CcmpEncrypt(const uint8_t tk[16],
                 const uint8_t nonce[13],
                 const uint8_t* aad, uint32_t aad_len,
                 const uint8_t* data, uint32_t data_len,
                 uint8_t* out /* data_len + 8 */);
// decrypts cipher_len bytes (payload+8 mic). writes data_len = cipher_len-8 bytes
// to out and returns true iff the mic verifies. (satoru)
bool CcmpDecrypt(const uint8_t tk[16],
                 const uint8_t nonce[13],
                 const uint8_t* aad, uint32_t aad_len,
                 const uint8_t* cipher, uint32_t cipher_len,
                 uint8_t* out /* cipher_len - 8 */);

} // namespace WifiCrypto
// end (satoru)
