//  kurono os - 802.11i security self-test (satoru)
//  see ieee80211_test.h. asserts the wpa2 crypto core against published vectors:
//   - sha1 (rfc 3174), hmac-sha1 (rfc 2202)
//   - pbkdf2-hmac-sha1: the 802.11i wpa-psk vector (ssid "IEEE", pass "password"
//     -> pmk f42c6fc52df0ebef9ebb4b90b38a5f90...) - task-mandated. (satoru)
//   - aes-128 (fips-197 c.1), aes-cmac (rfc 4493), aes key wrap (rfc 3394)
//   - the ieee 802.11 prf-384 ptk derivation vector
//   - ccmp (aes-ctr + cbc-mac): rfc 3610 packet vector #1 + mic verify + a
//     decrypt round-trip + tamper rejection.
//  every one is verified byte-exact; together they prove the security core
//  independent of any radio. (satoru)

#include "ieee80211_test.h"
#include "wifi_crypto.h"
#include "../drivers/serial.h"

using namespace WifiCrypto;

namespace {

// compare n bytes of buf against a hex string (2 chars/byte). (satoru)
static bool eq_hex(const uint8_t* buf, int n, const char* hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < n; i++) {
        int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (buf[i] != (uint8_t)((hi << 4) | lo)) return false;
    }
    return true;
}

static void log_result(const char* name, bool pass) {
    SerialLogger::Log("WIFI-TEST: ");
    SerialLogger::Log(name);
    SerialLogger::Log(pass ? " PASS\r\n" : " FAIL\r\n");
}

static int i_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void i_memset(void* p, uint8_t v, int n) { uint8_t* b = (uint8_t*)p; for (int i = 0; i < n; i++) b[i] = v; }
static int i_memcmp(const void* a, const void* b, int n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    for (int i = 0; i < n; i++) { if (x[i] != y[i]) return x[i] - y[i]; } return 0;
}

} // namespace

int Ieee80211Test::RunAll() {
    int pass = 0, total = 0;
    SerialLogger::Log("WIFI-TEST: 802.11i security core vectors\r\n");

    // ── sha1 (rfc 3174) ──────────────────────────────────────────────── (satoru)
    {
        uint8_t d[20];
        Sha1((const uint8_t*)"abc", 3, d);
        bool ok = eq_hex(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
        log_result("sha1(\"abc\")", ok); total++; pass += ok;
    }
    // ── hmac-sha1 (rfc 2202 case 1) ──────────────────────────────────── (satoru)
    {
        uint8_t key[20]; i_memset(key, 0x0b, 20);
        uint8_t mac[20];
        HmacSha1(key, 20, (const uint8_t*)"Hi There", 8, mac);
        bool ok = eq_hex(mac, 20, "b617318655057264e28bc0b6fb378c8ef146be00");
        log_result("hmac-sha1 (rfc2202 #1)", ok); total++; pass += ok;
    }
    // ── pbkdf2-hmac-sha1: the wpa-psk vector (TASK-MANDATED) ──────────── (satoru)
    //   ssid "IEEE", passphrase "password", 4096 iters -> 256-bit pmk. (satoru)
    {
        uint8_t pmk[32];
        Pbkdf2HmacSha1((const uint8_t*)"password", 8, (const uint8_t*)"IEEE", 4, 4096, pmk, 32);
        bool ok = eq_hex(pmk, 32,
            "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
        log_result("pbkdf2 wpa-psk (ssid=IEEE pass=password)", ok); total++; pass += ok;
    }
    // ── aes-128 encrypt (fips-197 appendix c.1) ──────────────────────── (satoru)
    {
        uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
        uint8_t pt[16]  = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        uint8_t ct[16];
        Aes128Ctx c; Aes128Init(&c, key);
        Aes128EncryptBlock(&c, pt, ct);
        bool ok = eq_hex(ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
        log_result("aes-128 encrypt (fips-197 c.1)", ok); total++; pass += ok;
        // and decrypt back (satoru)
        uint8_t dec[16]; Aes128DecryptBlock(&c, ct, dec);
        bool ok2 = i_memcmp(dec, pt, 16) == 0;
        log_result("aes-128 decrypt round-trip", ok2); total++; pass += ok2;
    }
    // ── aes-cmac (rfc 4493) ──────────────────────────────────────────── (satoru)
    {
        uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
        uint8_t mac[16];
        AesCmac(key, (const uint8_t*)"", 0, mac);
        bool ok = eq_hex(mac, 16, "bb1d6929e95937287fa37d129b756746");
        log_result("aes-cmac empty (rfc4493)", ok); total++; pass += ok;
        uint8_t msg[16] = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
        uint8_t mac2[16];
        AesCmac(key, msg, 16, mac2);
        bool ok2 = eq_hex(mac2, 16, "070a16b46b4d4144f79bdd9dd04a287c");
        log_result("aes-cmac 16B (rfc4493)", ok2); total++; pass += ok2;
    }
    // ── aes key wrap / unwrap (rfc 3394 §4.1) ────────────────────────── (satoru)
    //   used to unwrap the gtk from eapol-key msg 3. (satoru)
    {
        uint8_t kek[16]; for (int i = 0; i < 16; i++) kek[i] = (uint8_t)i;
        uint8_t key[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        uint8_t wrapped[24]; uint32_t wl = 0;
        AesWrap(kek, key, 16, wrapped, &wl);
        bool ok = (wl == 24) && eq_hex(wrapped, 24,
            "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
        log_result("aes key-wrap (rfc3394 4.1)", ok); total++; pass += ok;
        uint8_t unw[16]; uint32_t ul = 0;
        bool ok2 = AesUnwrap(kek, wrapped, 24, unw, &ul) && ul == 16 && i_memcmp(unw, key, 16) == 0;
        log_result("aes key-unwrap round-trip", ok2); total++; pass += ok2;
    }
    // ── ieee 802.11 prf-384 ptk derivation (TASK-MANDATED) ───────────── (satoru)
    //   pmk = the wpa-psk pmk above; fixed macs + nonces -> 48-byte ptk. the
    //   expected value was cross-checked against a reference hmac-sha1 prf. (satoru)
    {
        uint8_t pmk[32];
        Pbkdf2HmacSha1((const uint8_t*)"password", 8, (const uint8_t*)"IEEE", 4, 4096, pmk, 32);
        uint8_t aa[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
        uint8_t spa[6] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        uint8_t anonce[32], snonce[32];
        for (int i = 0; i < 32; i++) { anonce[i] = (uint8_t)i; snonce[i] = (uint8_t)(0x80 + i); }
        uint8_t ptk[48];
        DerivePtk(pmk, aa, spa, anonce, snonce, ptk, 48);
        bool ok = eq_hex(ptk, 48,
            "bfb8b9c38fb0f99e920055b6c4fbf27d"   // kck (satoru)
            "1804af1f7b17d3fc9e86c4d45b400e25"   // kek (satoru)
            "add073ac91c9e01a3bcb320dbc2b1b2a"); // tk  (satoru)
        log_result("prf-384 ptk derivation", ok); total++; pass += ok;
    }
    // ── ccmp (aes-ctr + cbc-mac): rfc 3610 packet vector #1 ──────────── (satoru)
    //   m=8 l=2 (== ccmp). 8-byte aad header, 23-byte payload. (satoru)
    {
        uint8_t key[16]   = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF};
        uint8_t nonce[13] = {0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5};
        uint8_t aad[8]    = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        uint8_t pt[23];   for (int i = 0; i < 23; i++) pt[i] = (uint8_t)(0x08 + i);
        uint8_t out[23 + 8];
        CcmpEncrypt(key, nonce, aad, 8, pt, 23, out);
        bool okp = eq_hex(out, 23, "588c979a61c663d2f066d0c2c0f989806d5f6b61dac384");
        bool okm = eq_hex(out + 23, 8, "17e8d12cfdf926e0");
        log_result("ccmp encrypt (rfc3610 pv#1 ciphertext)", okp); total++; pass += okp;
        log_result("ccmp encrypt (rfc3610 pv#1 mic)", okm); total++; pass += okm;
        // decrypt + mic-verify round-trip (satoru)
        uint8_t dec[23];
        bool okd = CcmpDecrypt(key, nonce, aad, 8, out, 31, dec) && i_memcmp(dec, pt, 23) == 0;
        log_result("ccmp decrypt + mic-verify", okd); total++; pass += okd;
        // tamper one ciphertext byte: the mic check MUST reject it (satoru)
        out[3] ^= 0x01;
        bool oktamper = !CcmpDecrypt(key, nonce, aad, 8, out, 31, dec);
        log_result("ccmp tamper rejected by mic", oktamper); total++; pass += oktamper;
    }

    // ── summary ──────────────────────────────────────────────────────── (satoru)
    SerialLogger::Log("WIFI-TEST: SUMMARY ");
    {
        char buf[24]; int p = 0;
        auto emit = [&](int v) {
            if (v == 0) { buf[p++] = '0'; return; }
            char t[12]; int ti = 0; while (v > 0) { t[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti > 0) buf[p++] = t[--ti];
        };
        emit(pass); buf[p++] = '/'; emit(total); buf[p] = 0;
        SerialLogger::Log(buf);
    }
    SerialLogger::Log(pass == total ? "  ALL PASS\r\n" : "  SOME FAILED\r\n");
    (void)i_strlen;
    return pass;
}
// end (satoru)
