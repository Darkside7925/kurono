// wpa_psk_crypto: the six crypto primitives the supplicant needs.
//
// the library is pure logic and includes only this header, so it can be built
// with the kernel crypto API in the container (wpa_psk_crypto_kernel.c) or
// with another crypto library in a host unit test. no function here logs or
// returns intermediate secrets beyond its explicit output. (satoru)

#ifndef KLD_WPA_PSK_CRYPTO_H
#define KLD_WPA_PSK_CRYPTO_H

#include <linux/types.h>

// PBKDF2-HMAC-SHA1, the 4096-iteration derivation of 802.11-2020 12.7.1.2.
// out_len is 32 for a WPA2 PMK. (satoru)
int wpa_crypto_pbkdf2_sha1(const u8 *pass, unsigned int pass_len,
                           const u8 *salt, unsigned int salt_len,
                           unsigned int iterations, u8 *out,
                           unsigned int out_len);

// one-shot HMAC-SHA1 / HMAC-SHA256. (satoru)
int wpa_crypto_hmac_sha1(const u8 *key, unsigned int key_len,
                         const u8 *data, unsigned int data_len, u8 out[20]);
int wpa_crypto_hmac_sha256(const u8 *key, unsigned int key_len,
                           const u8 *data, unsigned int data_len, u8 out[32]);

// AES-128-CMAC, full 16 byte tag (802.11-2020 12.7.1.3 descriptor version 3).
int wpa_crypto_cmac_aes(const u8 *key, unsigned int key_len,
                        const u8 *data, unsigned int data_len, u8 out[16]);

// RFC 3394 AES key unwrap with a 128-bit KEK. returns the plaintext length
// (in_len - 8) or -EBADMSG when the integrity check fails. (satoru)
int wpa_crypto_aes_unwrap(const u8 kek[16], const u8 *in, unsigned int in_len,
                          u8 *out);

// SNonce source. never returns a repeated value by design of the OS crng.
void wpa_crypto_random_bytes(void *buf, unsigned int len);

#endif
// end (satoru)
