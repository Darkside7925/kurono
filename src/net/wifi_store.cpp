// wifi_store: see wifi_store.h. (satoru)
//
// ---- the KLD wi-fi container contract (for the KLD lead to wire) ----------
// the kernel side (this file plus WiFi:: in net/network.cpp) and the container
// talk through four RAM-only KVFS files under /system/run/kld (the container
// sees them as /run/kld/...). none of them is persisted.
//
//   wifi.conf   kernel -> container. "SSID=<name>\nPSK=<passphrase>\n", the
//               format kld/tests/wifi_ctl.c already parses. written by
//               HandoffToKld right before a connect request; removed by the
//               kernel when the container reports a result in wifi_state, or
//               after 30 s. the container should read it once and keep the
//               psk only in memory (it already wipes its buffers).
//   wifi_ctl    kernel -> container. one command per write, the whole file:
//               "scan\n", "connect\n" (credentials in wifi.conf) or
//               "disconnect\n". the container polls it (or the lead adds a
//               broker doorbell), acts, and unlinks it.
//   wifi_scan   container -> kernel. one line per bss, tab separated:
//               "<ssid>\t<signal dbm>\t<security 0 open,1 wep,2 wpa,3 wpa2,
//               4 wpa3>\t<channel>\t<bssid aa:bb:cc:dd:ee:ff>\n". rewritten
//               after every scan. WiFi::Scan/Tick read it into the picker.
//   wifi_state  container -> kernel. one line: "connected\t<ssid>\t<dbm>\n",
//               "connecting\t<ssid>\n", "disconnected\n" or
//               "failed\t<reason word>\n" (reason: auth, notfound, timeout,
//               fw). WiFi::Tick turns it into the state the menu bar and
//               Settings show.
//
// a broker op is the safer long-term channel for wifi.conf (any ring-3
// process can read /run/kld while the file exists): add an op, check that the
// caller is the registered wi-fi container, and return
// WifiStore::TakeForContainer(pid, buf, cap); then HandoffToKld can stop
// writing the file. the container side then only swaps lx_os_read_file for
// that op. (satoru)
#include "wifi_store.h"
#include "../security/kcrypto.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/nvme.h"
#include "../system/platform_test.h"

namespace {

const char* kPath = "/etc/kurono/wifi.kws";
const char* kHandoffPath = "/system/run/kld/wifi.conf";
const uint32_t kMagic = 0x3153574Bu;   // 'KWS1' (satoru)
const uint32_t kKdfIters = 20000;
const int kHdr = 64;
const int kEnt = 160;
const int kTagOff = 120;   // tag covers bytes 0..119 of an entry (satoru)

struct Ent {
    uint8_t  ssid_len;
    char     ssid[32];
    uint8_t  security;
    uint8_t  flags;
    uint8_t  psk_len;
    uint32_t last_used;
    uint8_t  nonce[16];
    uint8_t  ct[64];
    uint8_t  tag[16];
    bool     used;
};

Ent      g_ent[WifiStore::MAX_NETWORKS];
uint8_t  g_salt[16];
uint8_t  g_enc[16], g_mac[32];
bool     g_keyed = false;
bool     g_loaded = false;
bool     g_foreign = false;
bool     g_test = false;       // self-test: fixed key, no disk (satoru)
uint32_t g_clock = 0;

char     g_pending[160];
int      g_pending_len = 0;
uint32_t g_pending_ms = 0;
bool     g_file_live = false;

int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
bool s_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

void fingerprint(uint8_t out[32]) {
    KCrypto::Sha256 c; KCrypto::Sha256Init(&c);
    KCrypto::Sha256Update(&c, "kurono-wifi-v1", 14);
    uint32_t r[4];
    static const uint32_t leaves[5] = {0u, 1u, 0x80000002u, 0x80000003u, 0x80000004u};
    for (int li = 0; li < 5; li++) {
        uint32_t leaf = leaves[li];
        uint32_t a = leaf, b, cc = 0, d;
        __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "+c"(cc), "=d"(d));
        if (leaf == 1) b &= 0x0000FFFFu;   // drop the apic id + cpu count byte (differs per cpu) (satoru)
        r[0] = a; r[1] = b; r[2] = cc; r[3] = d;
        if (leaf == 1) r[3] = 0;   // feature bits can change with firmware settings (satoru)
        KCrypto::Sha256Update(&c, r, sizeof(r));
    }
    if (!g_test) {
        const NVMeControllerInfo& ni = NVMe::GetInfo();
        if (ni.detected) {
            KCrypto::Sha256Update(&c, ni.serial, (uint32_t)s_len(ni.serial));
            KCrypto::Sha256Update(&c, ni.model, (uint32_t)s_len(ni.model));
        }
    }
    KCrypto::Sha256Final(&c, out);
}

void derive() {
    uint8_t fp[32], dk[48];
    fingerprint(fp);
    KCrypto::Pbkdf2Sha256(fp, 32, g_salt, 16, g_test ? 10 : kKdfIters, dk, 48);
    for (int i = 0; i < 16; i++) g_enc[i] = dk[i];
    for (int i = 0; i < 32; i++) g_mac[i] = dk[16 + i];
    KCrypto::Wipe(fp, sizeof(fp)); KCrypto::Wipe(dk, sizeof(dk));
    g_keyed = true;
}

void kcv(uint8_t out[16]) {
    uint8_t h[32];
    KCrypto::HmacSha256(g_mac, 32, "kurono-wifi-kcv", 15, h);
    for (int i = 0; i < 16; i++) out[i] = h[i];
    KCrypto::Wipe(h, sizeof(h));
}

void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void ent_bytes(const Ent& e, uint8_t b[kEnt]) {
    for (int i = 0; i < kEnt; i++) b[i] = 0;
    b[0] = e.ssid_len;
    for (int i = 0; i < 32; i++) b[1 + i] = (uint8_t)e.ssid[i];
    b[33] = e.security; b[34] = e.flags; b[35] = e.psk_len;
    put32(b + 36, e.last_used);
    for (int i = 0; i < 16; i++) b[40 + i] = e.nonce[i];
    for (int i = 0; i < 64; i++) b[56 + i] = e.ct[i];
    for (int i = 0; i < 16; i++) b[kTagOff + i] = e.tag[i];
}

void ent_from(Ent& e, const uint8_t b[kEnt]) {
    e.ssid_len = b[0] > 32 ? 32 : b[0];
    for (int i = 0; i < 32; i++) e.ssid[i] = (char)b[1 + i];
    e.security = b[33]; e.flags = b[34]; e.psk_len = b[35] > 64 ? 64 : b[35];
    e.last_used = get32(b + 36);
    for (int i = 0; i < 16; i++) e.nonce[i] = b[40 + i];
    for (int i = 0; i < 64; i++) e.ct[i] = b[56 + i];
    for (int i = 0; i < 16; i++) e.tag[i] = b[kTagOff + i];
}

void ent_tag(const Ent& e, uint8_t out[16]) {
    uint8_t b[kEnt], h[32];
    ent_bytes(e, b);
    KCrypto::HmacSha256(g_mac, 32, b, kTagOff, h);
    for (int i = 0; i < 16; i++) out[i] = h[i];
    KCrypto::Wipe(h, sizeof(h));
}

bool ent_ok(const Ent& e) {
    uint8_t t[16];
    ent_tag(e, t);
    return KCrypto::Equal(t, e.tag, 16);
}

int find(const char* ssid) {
    int n = s_len(ssid);
    for (int i = 0; i < WifiStore::MAX_NETWORKS; i++) {
        if (!g_ent[i].used || g_ent[i].ssid_len != n) continue;
        bool same = true;
        for (int k = 0; k < n; k++) if (g_ent[i].ssid[k] != ssid[k]) { same = false; break; }
        if (same) return i;
    }
    return -1;
}

void persist() {
    if (g_test || g_foreign) return;
    static uint8_t buf[kHdr + kEnt * WifiStore::MAX_NETWORKS];
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    int n = 0;
    for (int i = 0; i < WifiStore::MAX_NETWORKS; i++) {
        if (!g_ent[i].used) continue;
        ent_bytes(g_ent[i], buf + kHdr + n * kEnt);
        n++;
    }
    put32(buf, kMagic);
    buf[4] = 1; buf[5] = 0;
    buf[6] = (uint8_t)n; buf[7] = 0;
    for (int i = 0; i < 16; i++) buf[8 + i] = g_salt[i];
    kcv(buf + 24);
    KVFS::Mkdirs("/etc/kurono", 0700);
    KVFS::WriteFile(kPath, buf, (uint32_t)(kHdr + n * kEnt));
    KVFS::Chmod(kPath, 0600);
}

void load() {
    if (g_loaded) return;
    g_loaded = true;
    for (int i = 0; i < WifiStore::MAX_NETWORKS; i++) g_ent[i].used = false;
    static uint8_t buf[kHdr + kEnt * WifiStore::MAX_NETWORKS];
    int n = g_test ? 0 : KVFS::ReadFile(kPath, buf, sizeof(buf));
    if (n < kHdr || get32(buf) != kMagic) {
        // fresh store: a new random salt. (satoru)
        if (g_test) for (int i = 0; i < 16; i++) g_salt[i] = (uint8_t)i;
        else KCrypto::Random(g_salt, 16);
        derive();
        return;
    }
    for (int i = 0; i < 16; i++) g_salt[i] = buf[8 + i];
    derive();
    uint8_t want[16];
    kcv(want);
    if (!KCrypto::Equal(want, buf + 24, 16)) {
        g_foreign = true;
        SerialLogger::Log("[wifistore] saved networks belong to another machine or disk; kept, not used\r\n");
        return;
    }
    int cnt = buf[6];
    if (cnt > WifiStore::MAX_NETWORKS) cnt = WifiStore::MAX_NETWORKS;
    int good = 0;
    for (int i = 0; i < cnt && kHdr + (i + 1) * kEnt <= n; i++) {
        Ent e;
        ent_from(e, buf + kHdr + i * kEnt);
        e.used = true;
        if (!ent_ok(e)) continue;   // damaged entry: drop it (satoru)
        g_ent[good++] = e;
        if (e.last_used > g_clock) g_clock = e.last_used;
    }
    KCrypto::Wipe(buf, sizeof(buf));
    SerialLogger::Log("[wifistore] ");
    SerialLogger::LogDec(good);
    SerialLogger::Log(" saved network(s)\r\n");
}

}  // namespace

namespace WifiStore {

void Init() { load(); }
bool Foreign() { load(); return g_foreign; }

int Count() {
    load();
    int n = 0;
    for (int i = 0; i < MAX_NETWORKS; i++) if (g_ent[i].used) n++;
    return n;
}

bool Get(int idx, Info* out) {
    load();
    int n = 0;
    for (int i = 0; i < MAX_NETWORKS; i++) {
        if (!g_ent[i].used) continue;
        if (n++ != idx) continue;
        int l = g_ent[i].ssid_len;
        for (int k = 0; k < l; k++) out->ssid[k] = g_ent[i].ssid[k];
        out->ssid[l] = 0;
        out->security = g_ent[i].security;
        out->flags = g_ent[i].flags;
        out->last_used = g_ent[i].last_used;
        return true;
    }
    return false;
}

bool Has(const char* ssid) { load(); return find(ssid) >= 0; }

bool Save(const char* ssid, const char* psk, uint8_t security) {
    load();
    if (g_foreign || !g_keyed) return false;
    int sl = s_len(ssid), pl = s_len(psk);
    if (sl <= 0 || sl > MAX_SSID || pl > MAX_PSK) return false;
    int found = find(ssid);
    int i = found;
    if (i < 0) {
        for (int k = 0; k < MAX_NETWORKS; k++) if (!g_ent[k].used) { i = k; break; }
    }
    if (i < 0) {
        // full: replace the least recently used one. (satoru)
        uint32_t lo = 0xFFFFFFFFu;
        for (int k = 0; k < MAX_NETWORKS; k++) if (g_ent[k].last_used < lo) { lo = g_ent[k].last_used; i = k; }
    }
    Ent& e = g_ent[i];
    uint8_t keep_flags = found >= 0 ? e.flags : (uint8_t)F_AUTOJOIN;
    for (int k = 0; k < 32; k++) e.ssid[k] = k < sl ? ssid[k] : 0;
    e.ssid_len = (uint8_t)sl;
    e.security = security;
    e.flags = keep_flags;
    e.psk_len = (uint8_t)pl;
    e.last_used = ++g_clock;
    KCrypto::Random(e.nonce, 16);
    uint8_t plain[64];
    for (int k = 0; k < 64; k++) plain[k] = k < pl ? (uint8_t)psk[k] : 0;
    KCrypto::Aes128Ctr(g_enc, e.nonce, plain, e.ct, 64);
    KCrypto::Wipe(plain, sizeof(plain));
    e.used = true;
    ent_tag(e, e.tag);
    persist();
    return true;
}

bool Forget(const char* ssid) {
    load();
    int i = find(ssid);
    if (i < 0) return false;
    KCrypto::Wipe(&g_ent[i], sizeof(Ent));
    g_ent[i].used = false;
    persist();
    return true;
}

bool SetAutoJoin(const char* ssid, bool on) {
    load();
    int i = find(ssid);
    if (i < 0 || g_foreign) return false;
    if (on) g_ent[i].flags |= F_AUTOJOIN; else g_ent[i].flags &= (uint8_t)~F_AUTOJOIN;
    ent_tag(g_ent[i], g_ent[i].tag);
    persist();
    return true;
}

void Touch(const char* ssid) {
    load();
    int i = find(ssid);
    if (i < 0 || g_foreign) return;
    g_ent[i].last_used = ++g_clock;
    ent_tag(g_ent[i], g_ent[i].tag);
    persist();
}

bool Passphrase(const char* ssid, char* out, int cap) {
    load();
    int i = find(ssid);
    if (i < 0 || g_foreign || !g_keyed || cap < 1) return false;
    if (!ent_ok(g_ent[i])) return false;
    uint8_t plain[64];
    KCrypto::Aes128Ctr(g_enc, g_ent[i].nonce, g_ent[i].ct, plain, 64);
    int l = g_ent[i].psk_len;
    if (l > cap - 1) { KCrypto::Wipe(plain, sizeof(plain)); return false; }
    for (int k = 0; k < l; k++) out[k] = (char)plain[k];
    out[l] = 0;
    KCrypto::Wipe(plain, sizeof(plain));
    return true;
}

bool BestAutoJoin(const char* const* visible, int nvisible, char* ssid_out) {
    load();
    int best = -1;
    for (int i = 0; i < MAX_NETWORKS; i++) {
        if (!g_ent[i].used || !(g_ent[i].flags & F_AUTOJOIN)) continue;
        if (visible) {
            bool seen = false;
            char nm[33];
            int l = g_ent[i].ssid_len;
            for (int k = 0; k < l; k++) nm[k] = g_ent[i].ssid[k];
            nm[l] = 0;
            for (int v = 0; v < nvisible; v++) if (s_eq(visible[v], nm)) { seen = true; break; }
            if (!seen) continue;
        }
        if (best < 0 || g_ent[i].last_used > g_ent[best].last_used) best = i;
    }
    if (best < 0) return false;
    int l = g_ent[best].ssid_len;
    for (int k = 0; k < l; k++) ssid_out[k] = g_ent[best].ssid[k];
    ssid_out[l] = 0;
    return true;
}

bool HandoffToKld(const char* ssid) {
    load();
    char name[MAX_SSID + 1];
    if (!ssid || !ssid[0]) {
        if (!BestAutoJoin(nullptr, 0, name)) return false;
        ssid = name;
    }
    char psk[MAX_PSK + 1];
    if (!Passphrase(ssid, psk, sizeof(psk))) return false;
    int p = 0;
    const char* a = "SSID=";
    while (*a) g_pending[p++] = *a++;
    for (int k = 0; ssid[k] && p < 100; k++) g_pending[p++] = ssid[k];
    g_pending[p++] = '\n';
    a = "PSK=";
    while (*a) g_pending[p++] = *a++;
    for (int k = 0; psk[k] && p < (int)sizeof(g_pending) - 2; k++) g_pending[p++] = psk[k];
    g_pending[p++] = '\n';
    g_pending_len = p;
    g_pending_ms = Timer::GetRealMs();
    KCrypto::Wipe(psk, sizeof(psk));
    KVFS::Mkdirs("/system/run/kld");
    int wr = KVFS::WriteFile(kHandoffPath, g_pending, (uint32_t)p);
    if (wr < 0) {
        SerialLogger::Log("[wifistore] handoff write failed\r\n");
        return false;
    }
    KVFS::Chmod(kHandoffPath, 0600);
    g_file_live = true;
    Touch(ssid);
    SerialLogger::Log("[wifistore] handoff ");
    SerialLogger::LogDec(p);
    SerialLogger::Log(" bytes\r\n");
    return true;
}

void ClearHandoff() {
    if (g_file_live) {
        // overwrite before unlink so the freed kvfs node holds no secret. (satoru)
        char z[160];
        for (int i = 0; i < 160; i++) z[i] = 0;
        KVFS::WriteFile(kHandoffPath, z, (uint32_t)(g_pending_len > 0 ? g_pending_len : 1));
        KVFS::Unlink(kHandoffPath);
        g_file_live = false;
    }
    KCrypto::Wipe(g_pending, sizeof(g_pending));
    g_pending_len = 0;
}

int TakeForContainer(uint32_t pid, char* out, int cap) {
    (void)pid;   // the broker op checks the caller; recorded here for the audit later (satoru)
    if (g_pending_len <= 0 || cap < g_pending_len) return 0;
    for (int i = 0; i < g_pending_len; i++) out[i] = g_pending[i];
    int n = g_pending_len;
    ClearHandoff();
    return n;
}

void Tick() {
    if (!g_file_live && g_pending_len <= 0) return;
    if (Timer::GetRealMs() - g_pending_ms > kHandoffMs) {
        ClearHandoff();
        SerialLogger::Log("[wifistore] handoff expired\r\n");
    }
}

void SelfTest() {
    // private table: save the real state, run, restore. (satoru)
    static Ent saved[MAX_NETWORKS];
    for (int i = 0; i < MAX_NETWORKS; i++) saved[i] = g_ent[i];
    bool s_loaded = g_loaded, s_keyed = g_keyed, s_foreign = g_foreign;
    uint8_t s_enc[16], s_mac[32], s_salt[16];
    for (int i = 0; i < 16; i++) { s_enc[i] = g_enc[i]; s_salt[i] = g_salt[i]; }
    for (int i = 0; i < 32; i++) s_mac[i] = g_mac[i];
    uint32_t s_clock = g_clock;

    g_test = true; g_loaded = false; g_foreign = false; g_keyed = false;
    load();
    bool ok1 = Save("HomeNet", "correct horse battery", 3) && Save("Cafe", "", 0);
    char pw[64];
    bool ok2 = Passphrase("HomeNet", pw, sizeof(pw)) && s_eq(pw, "correct horse battery");
    bool ok3 = Passphrase("Cafe", pw, sizeof(pw)) && pw[0] == 0;
    // the ciphertext must not contain the passphrase. (satoru)
    int hn = find("HomeNet");
    bool ok4 = hn >= 0;
    if (ok4) for (int k = 0; k + 7 <= 64; k++) {
        const char* w = "correct";
        bool m = true;
        for (int q = 0; q < 7; q++) if ((char)g_ent[hn].ct[k + q] != w[q]) { m = false; break; }
        if (m) ok4 = false;
    }
    // a flipped ciphertext bit fails the tag. (satoru)
    bool ok5 = false;
    if (hn >= 0) {
        g_ent[hn].ct[3] ^= 1;
        ok5 = !Passphrase("HomeNet", pw, sizeof(pw));
        g_ent[hn].ct[3] ^= 1;
    }
    const char* vis[] = { "Other", "HomeNet" };
    char best[33];
    bool ok6 = BestAutoJoin(vis, 2, best) && s_eq(best, "HomeNet");
    bool ok7 = Forget("Cafe") && !Has("Cafe") && Count() == 1;
    KCrypto::Wipe(pw, sizeof(pw));

    for (int i = 0; i < MAX_NETWORKS; i++) { KCrypto::Wipe(&g_ent[i], sizeof(Ent)); g_ent[i] = saved[i]; }
    g_test = false; g_loaded = s_loaded; g_keyed = s_keyed; g_foreign = s_foreign; g_clock = s_clock;
    for (int i = 0; i < 16; i++) { g_enc[i] = s_enc[i]; g_salt[i] = s_salt[i]; }
    for (int i = 0; i < 32; i++) g_mac[i] = s_mac[i];

    PlatformTest::Register("wifistore-roundtrip", ok1 && ok2 && ok3);
    PlatformTest::Register("wifistore-ciphertext-opaque", ok4);
    PlatformTest::Register("wifistore-tamper-detected", ok5);
    PlatformTest::Register("wifistore-autojoin-forget", ok6 && ok7);
}

}  // namespace WifiStore

// end (satoru)
