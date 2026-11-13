// wpa_psk_internal: derivation helpers exposed only so the apitest family can
// check them against the python oracle. they are the same functions the state
// machine uses; nothing here is a second implementation. (satoru)

#ifndef KLD_WPA_PSK_INTERNAL_H
#define KLD_WPA_PSK_INTERNAL_H

#include <linux/types.h>

#define WPA_PSK_KCK_LEN 16
#define WPA_PSK_KEK_LEN 16
#define WPA_PSK_TK_LEN  16
#define WPA_PSK_PTK_LEN 48

// 802.11-2020 12.7.1.2: pmk = pbkdf2-hmac-sha1(passphrase, ssid, 4096, 32).
// a 64 character hex passphrase is an already derived pmk (common ap config).
int wpa_psk_pmk(const char *pass, unsigned int pass_len,
                const u8 *ssid, unsigned int ssid_len, u8 pmk[32]);

// 802.11-2020 12.7.1.3: ptk = PRF/KDF(pmk, "Pairwise key expansion",
// min/max mac || min/max nonce). akm 2 selects PRF-SHA1, akm 6 KDF-SHA256.
int wpa_psk_derive_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                       const u8 anonce[32], const u8 snonce[32],
                       unsigned int akm, u8 ptk[WPA_PSK_PTK_LEN]);

// 802.11-2020 12.7.3: MIC over the whole EAPOL-Key frame with the MIC field
// zeroed. descriptor version 2 = HMAC-SHA1-128, version 3 = AES-128-CMAC.
int wpa_psk_calc_mic(const u8 kck[16], const u8 *frame, unsigned int len,
                     unsigned int descriptor_version, u8 mic[16]);

#endif
// end (satoru)
