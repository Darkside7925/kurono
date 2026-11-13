// kcrypto: small freestanding crypto the platform pieces share (password
// hashing, the saved wi-fi network store). sha-256 (fips 180-4), hmac-sha256
// (rfc 2104), pbkdf2-hmac-sha256 (rfc 8018), aes-128-ctr (fips 197 through
// WifiCrypto's block cipher), a constant time compare, a wipe that the
// compiler keeps, and a random source (rdrand when the cpu has it, mixed with
// the tsc). no libc, no heap. (satoru)
#pragma once
#include "../kernel/types.h"

namespace KCrypto {

struct Sha256 {
    uint32_t s[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t n;
};
void Sha256Init(Sha256* c);
void Sha256Update(Sha256* c, const void* data, uint32_t len);
void Sha256Final(Sha256* c, uint8_t out[32]);
void Sha256Once(const void* data, uint32_t len, uint8_t out[32]);

void HmacSha256(const uint8_t* key, uint32_t key_len, const void* data, uint32_t len,
                uint8_t out[32]);
// derives dk_len bytes. (satoru)
void Pbkdf2Sha256(const uint8_t* pass, uint32_t pass_len, const uint8_t* salt, uint32_t salt_len,
                  uint32_t iterations, uint8_t* dk, uint32_t dk_len);

// aes-128 in counter mode: the same call encrypts and decrypts. iv is the
// 16 byte initial counter block (big endian increment of the last 4 bytes). (satoru)
void Aes128Ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out,
               uint32_t len);

bool Equal(const void* a, const void* b, uint32_t n);   // constant time (satoru)
void Wipe(void* p, uint32_t n);                           // never optimised away (satoru)
void Random(void* out, uint32_t n);

void ToHex(const uint8_t* in, uint32_t n, char* out);    // out holds 2n+1 (satoru)
bool FromHex(const char* in, uint8_t* out, uint32_t n);  // exactly 2n hex digits (satoru)

// known answer self-test (sha-256 "abc", hmac rfc 4231 case 2, pbkdf2 rfc 7914
// vector). true when all pass. (satoru)
bool SelfTest();

}  // namespace KCrypto

// end (satoru)
