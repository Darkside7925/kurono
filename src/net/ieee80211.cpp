//  kurono os - ieee 802.11 software mac implementation (satoru)
//  see ieee80211.h. frame layer + scan/auth/assoc state machine + wpa2 4-way
//  handshake supplicant, all driven over the WifiRadioOps vendor vtable. no
//  register access here - that's the vendor driver's job. (satoru)
//  ref: ieee 802.11-2016 §9 (frames) §12 (rsna/ccmp); linux mac80211 +
//  wpa_supplicant for the sequence. original code. (satoru)

#include "ieee80211.h"
#include "wifi_crypto.h"
#include "../drivers/wifi_dev.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"

namespace Ieee80211 {

using namespace WifiCrypto;

// ── small freestanding helpers ───────────────────────────────────────── (satoru)
static inline void i_memset(void* p, uint8_t v, uint32_t n) {
    uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = v;
}
static inline void i_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* db = (uint8_t*)d; const uint8_t* sb = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) db[i] = sb[i];
}
static inline bool i_maceq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 6; i++) { if (a[i] != b[i]) return false; }
    return true;
}
static inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void put_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int i_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void log2(const char* a, const char* b) {
    SerialLogger::Log(a); if (b) SerialLogger::Log(b); SerialLogger::Log("\r\n");
}

// ── module state ─────────────────────────────────────────────────────── (satoru)
static const WifiRadioOps* g_ops = nullptr;
static void*               g_ctx = nullptr;
static WifiDevice*         g_dev = nullptr;
static SupplicantState     g_state = SUPP_IDLE;

static uint8_t  g_sta_mac[6]  = {0};   // our station mac (satoru)
static uint8_t  g_bssid[6]    = {0};   // the ap we're joining (satoru)
static char     g_ssid[33]    = {0};
static uint16_t g_seq         = 0;     // outgoing sequence-number counter (satoru)

// pmk + handshake material (satoru)
static uint8_t  g_pmk[32]     = {0};
static bool     g_have_pmk    = false;
static uint8_t  g_anonce[32]  = {0};
static uint8_t  g_snonce[32]  = {0};
static uint8_t  g_ptk[48]     = {0};   // kck(16)|kek(16)|tk(16) (satoru)
static bool     g_have_ptk    = false;
static uint8_t  g_gtk[32]     = {0};
static int      g_gtk_len     = 0;
static uint8_t  g_replay[8]   = {0};   // last replay counter we acked (satoru)
static uint64_t g_tx_pn       = 1;     // ccmp packet number (per-tx counter) (satoru)

// a tiny single-slot rx ring for DeliverRx() (driver-irq push path) (satoru)
#define RX_RING_SLOTS 4
#define RX_FRAME_MAX  2048
static uint8_t  g_rx_ring[RX_RING_SLOTS][RX_FRAME_MAX];
static int      g_rx_len[RX_RING_SLOTS] = {0};
static int      g_rx_head = 0, g_rx_tail = 0;

void DeliverRx(const uint8_t* frame, int len) {
    if (len <= 0 || len > RX_FRAME_MAX) return;
    int next = (g_rx_tail + 1) % RX_RING_SLOTS;
    if (next == g_rx_head) return;            // ring full - drop (satoru)
    i_memcpy(g_rx_ring[g_rx_tail], frame, len);
    g_rx_len[g_rx_tail] = len;
    g_rx_tail = next;
}

// pull one frame: prefer the irq-push ring, else poll the driver. (satoru)
static int rx_one(uint8_t* buf, int buf_max) {
    if (g_rx_head != g_rx_tail) {
        int len = g_rx_len[g_rx_head];
        if (len > buf_max) len = buf_max;
        i_memcpy(buf, g_rx_ring[g_rx_head], len);
        g_rx_head = (g_rx_head + 1) % RX_RING_SLOTS;
        return len;
    }
    if (g_ops && g_ops->rx_poll) return g_ops->rx_poll(g_ctx, buf, buf_max);
    return 0;
}

// ── frame-control assembly ───────────────────────────────────────────── (satoru)
static inline uint16_t make_fc(uint8_t ftype, uint8_t stype, uint16_t flags) {
    // fc layout (§9.2.4.1): bits0-1 version(0), 2-3 type, 4-7 subtype, 8-15 flags
    return (uint16_t)((ftype << 2) | (stype << 4)) | flags;
}

int BuildMacHeader(uint8_t* buf, uint8_t ftype, uint8_t stype, uint16_t fc_flags,
                   const uint8_t addr1[6], const uint8_t addr2[6],
                   const uint8_t addr3[6], uint16_t seq) {
    Mac80211Hdr* h = (Mac80211Hdr*)buf;
    put_le16((uint8_t*)&h->frame_control, make_fc(ftype, stype, fc_flags));
    h->duration_id = 0;
    i_memcpy(h->addr1, addr1, 6);
    i_memcpy(h->addr2, addr2, 6);
    i_memcpy(h->addr3, addr3, 6);
    put_le16((uint8_t*)&h->seq_ctrl, (uint16_t)(seq << 4));
    return (int)sizeof(Mac80211Hdr);
}

// append an information element (id, len, value). returns new pos. (satoru)
static int append_ie(uint8_t* buf, int pos, int buf_max, uint8_t eid,
                     const uint8_t* val, int val_len) {
    if (pos + 2 + val_len > buf_max) return pos;
    buf[pos++] = eid;
    buf[pos++] = (uint8_t)val_len;
    for (int i = 0; i < val_len; i++) buf[pos++] = val[i];
    return pos;
}

// the default supported-rates ie value (1,2,5.5,11,6,9,12,18 mbps; high bit of
// the first set marks them basic). 802.11-2016 §9.4.2.3. (satoru)
static const uint8_t DEFAULT_RATES[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };

// the rsn (wpa2) information element: version 1, group=ccmp, one pairwise=ccmp,
// one akm=psk, default rsn capabilities. ieee 802.11-2016 §9.4.2.25. (satoru)
static int append_rsn_ie(uint8_t* buf, int pos, int buf_max) {
    static const uint8_t RSN_BODY[] = {
        0x01, 0x00,                   // rsn version 1 (satoru)
        0x00, 0x0f, 0xac, 0x04,       // group cipher suite = ccmp (00-0f-ac:4) (satoru)
        0x01, 0x00,                   // pairwise cipher count = 1 (satoru)
        0x00, 0x0f, 0xac, 0x04,       // pairwise = ccmp (satoru)
        0x01, 0x00,                   // akm count = 1 (satoru)
        0x00, 0x0f, 0xac, 0x02,       // akm = psk (00-0f-ac:2) (satoru)
        0x00, 0x00                    // rsn capabilities (satoru)
    };
    return append_ie(buf, pos, buf_max, IEEE80211_EID_RSN, RSN_BODY, sizeof(RSN_BODY));
}

int BuildProbeRequest(uint8_t* buf, int buf_max,
                      const uint8_t sta[6], const char* ssid, uint16_t seq) {
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    int pos = BuildMacHeader(buf, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_PROBE_REQ,
                             0, bcast, sta, bcast, seq);
    int sl = i_strlen(ssid);
    if (sl > 32) sl = 32;
    pos = append_ie(buf, pos, buf_max, IEEE80211_EID_SSID, (const uint8_t*)ssid, sl);
    pos = append_ie(buf, pos, buf_max, IEEE80211_EID_SUPP_RATES, DEFAULT_RATES, sizeof(DEFAULT_RATES));
    return pos;
}

int BuildAuthRequest(uint8_t* buf, int buf_max,
                     const uint8_t sta[6], const uint8_t bssid[6], uint16_t seq) {
    int pos = BuildMacHeader(buf, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_AUTH,
                             0, bssid, sta, bssid, seq);
    // auth body: algorithm number (0 = open system), transaction seq (1), status (0)
    if (pos + 6 > buf_max) return pos;
    put_le16(buf + pos, 0); pos += 2;   // open system (satoru)
    put_le16(buf + pos, 1); pos += 2;   // transaction sequence number 1 (satoru)
    put_le16(buf + pos, 0); pos += 2;   // status code 0 (reserved in a request) (satoru)
    return pos;
}

int BuildAssocRequest(uint8_t* buf, int buf_max,
                      const uint8_t sta[6], const uint8_t bssid[6],
                      const char* ssid, bool wpa2, uint16_t seq) {
    int pos = BuildMacHeader(buf, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_ASSOC_REQ,
                             0, bssid, sta, bssid, seq);
    // assoc body: capability info + listen interval (satoru)
    if (pos + 4 > buf_max) return pos;
    put_le16(buf + pos, 0x0431); pos += 2; // ess + privacy + short-preamble caps (satoru)
    put_le16(buf + pos, 10);     pos += 2; // listen interval (satoru)
    int sl = i_strlen(ssid); if (sl > 32) sl = 32;
    pos = append_ie(buf, pos, buf_max, IEEE80211_EID_SSID, (const uint8_t*)ssid, sl);
    pos = append_ie(buf, pos, buf_max, IEEE80211_EID_SUPP_RATES, DEFAULT_RATES, sizeof(DEFAULT_RATES));
    if (wpa2) pos = append_rsn_ie(buf, pos, buf_max);
    return pos;
}

const uint8_t* FindIE(const uint8_t* ies, int len, uint8_t eid, int* out_len) {
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i];
        uint8_t l  = ies[i + 1];
        if (i + 2 + l > len) break;            // malformed / truncated (satoru)
        if (id == eid) { if (out_len) *out_len = l; return ies + i + 2; }
        i += 2 + l;
    }
    return nullptr;
}

// classify the security of a beacon from its rsn/wpa ies. (satoru)
static WiFiSecurity beacon_security(const uint8_t* ies, int len, bool privacy) {
    int rl = 0;
    if (FindIE(ies, len, IEEE80211_EID_RSN, &rl)) return WIFI_WPA2; // rsn ie => wpa2 (satoru)
    // wpa1 hides in a vendor ie with oui 00-50-f2 type 1 (satoru)
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], l = ies[i + 1];
        if (i + 2 + l > len) break;
        if (id == IEEE80211_EID_VENDOR && l >= 4 &&
            ies[i+2] == 0x00 && ies[i+3] == 0x50 && ies[i+4] == 0xf2 && ies[i+5] == 0x01)
            return WIFI_WPA;
        i += 2 + l;
    }
    return privacy ? WIFI_WEP : WIFI_OPEN;     // privacy bit w/o rsn/wpa = wep (satoru)
}

bool ParseBeacon(const uint8_t* frame, int len, WiFiNetwork* out) {
    if (len < (int)sizeof(Mac80211Hdr) + 12) return false;
    const Mac80211Hdr* h = (const Mac80211Hdr*)frame;
    uint16_t fc = le16((const uint8_t*)&h->frame_control);
    uint8_t ftype = (fc >> 2) & 0x3;
    uint8_t stype = (fc >> 4) & 0xF;
    if (ftype != IEEE80211_FTYPE_MGMT) return false;
    if (stype != IEEE80211_STYPE_BEACON && stype != IEEE80211_STYPE_PROBE_RESP) return false;

    // fixed beacon/probe-resp body: timestamp(8) + beacon interval(2) + caps(2) (satoru)
    const uint8_t* body = frame + sizeof(Mac80211Hdr);
    int body_len = len - (int)sizeof(Mac80211Hdr);
    uint16_t caps = le16(body + 10);
    bool privacy = (caps & 0x0010) != 0;       // privacy capability bit (satoru)
    const uint8_t* ies = body + 12;
    int ies_len = body_len - 12;

    i_memset(out, 0, sizeof(*out));
    i_memcpy(out->bssid.bytes, h->addr3, 6);   // bssid is addr3 (satoru)

    // ssid ie (satoru)
    int sl = 0;
    const uint8_t* ssid = FindIE(ies, ies_len, IEEE80211_EID_SSID, &sl);
    if (ssid && sl > 0) {
        int n = sl > NET_MAX_SSID - 1 ? NET_MAX_SSID - 1 : sl;
        i_memcpy(out->ssid, ssid, n);
        out->ssid[n] = 0;
    } else {
        out->ssid[0] = 0;                       // hidden ssid (satoru)
    }

    // ds-params ie carries the channel (satoru)
    int dl = 0;
    const uint8_t* ds = FindIE(ies, ies_len, IEEE80211_EID_DS_PARAMS, &dl);
    out->channel = (ds && dl >= 1) ? ds[0] : 0;

    out->security = beacon_security(ies, ies_len, privacy);
    out->signal_strength = -60;                 // driver overrides via get_signal (satoru)
    out->connected = false;
    return true;
}

// ── radio registration ───────────────────────────────────────────────── (satoru)
void RegisterRadio(const WifiRadioOps* ops, void* ctx, WifiDevice* dev) {
    g_ops = ops; g_ctx = ctx; g_dev = dev;
    g_state = SUPP_IDLE;
    g_rx_head = g_rx_tail = 0;
    // derive our station mac. prefer a driver-provided one later; for now build a
    // locally-administered mac from the pci device address (deterministic). a real
    // vendor driver will read the chip's otp/efuse mac and we'll adopt it. (satoru)
    if (dev) {
        g_sta_mac[0] = 0x02;                    // locally administered (satoru)
        g_sta_mac[1] = 0x00;
        g_sta_mac[2] = (uint8_t)(dev->vendor & 0xFF);
        g_sta_mac[3] = (uint8_t)(dev->device & 0xFF);
        g_sta_mac[4] = dev->slot;
        g_sta_mac[5] = dev->func;
    }
    log2("[80211] radio registered: ", dev ? dev->model : "(generic)");
}

bool HasRadio() { return g_ops != nullptr; }

void UnregisterRadio() {
    if (g_ops && g_ops->stop) g_ops->stop(g_ctx);
    g_ops = nullptr; g_ctx = nullptr; g_dev = nullptr;
    g_state = SUPP_IDLE;
}

SupplicantState State() { return g_state; }
int GetSignal() {
    if (g_ops && g_ops->get_signal) return g_ops->get_signal(g_ctx);
    return -100;
}

// ── scan ─────────────────────────────────────────────────────────────── (satoru)
// for each 2.4ghz channel: tune, send a broadcast probe-req, dwell ~30ms while
// draining rx for beacons/probe-resps, dedup by bssid into out[]. (satoru)
int Scan(WiFiNetwork* out, int max) {
    if (!g_ops || max <= 0) return 0;
    if (g_ops->start && !g_ops->start(g_ctx)) { log2("[80211] scan: radio start failed", nullptr); return 0; }
    g_state = SUPP_SCANNING;

    int found = 0;
    static const int CHANNELS[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13 };
    uint8_t frame[256];
    uint8_t rx[RX_FRAME_MAX];

    for (unsigned ci = 0; ci < sizeof(CHANNELS)/sizeof(CHANNELS[0]); ci++) {
        int ch = CHANNELS[ci];
        if (g_ops->set_channel && !g_ops->set_channel(g_ctx, ch)) continue;

        int flen = BuildProbeRequest(frame, sizeof(frame), g_sta_mac, "", g_seq++);
        if (g_ops->tx_frame) g_ops->tx_frame(g_ctx, frame, flen);

        // dwell on this channel, draining beacons/probe-resps. (satoru)
        uint32_t start = Timer::GetTicks();
        while ((uint32_t)(Timer::GetTicks() - start) < 30u) {
            int rlen = rx_one(rx, sizeof(rx));
            if (rlen > 0) {
                WiFiNetwork n;
                if (ParseBeacon(rx, rlen, &n)) {
                    if (n.channel == 0) n.channel = ch;  // fall back to tuned ch (satoru)
                    // dedup by bssid (satoru)
                    int idx = -1;
                    for (int i = 0; i < found; i++)
                        if (i_maceq(out[i].bssid.bytes, n.bssid.bytes)) { idx = i; break; }
                    if (idx < 0 && found < max) idx = found++;
                    if (idx >= 0) {
                        out[idx] = n;
                        if (g_ops->get_signal) out[idx].signal_strength = g_ops->get_signal(g_ctx);
                    }
                }
            } else {
                Scheduler::SleepMs(1);            // yield while waiting (satoru)
            }
        }
    }

    g_state = SUPP_IDLE;
    {
        char nb[8]; int p = 0, v = found;
        if (v == 0) nb[p++] = '0';
        else { char t[8]; int ti = 0; while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; } while (ti) nb[p++] = t[--ti]; }
        nb[p] = 0;
        log2("[80211] scan complete, networks=", nb);
    }
    return found;
}

// ── tx + wait-for-frame helper ───────────────────────────────────────── (satoru)
// send a frame, then poll rx up to `timeout_ms` for one matching the predicate.
// returns the matched frame length (copied into rx_buf) or 0 on timeout. (satoru)
typedef bool (*FrameMatch)(const uint8_t* f, int len);
static int tx_and_wait(const uint8_t* tx, int tx_len, FrameMatch match,
                       uint8_t* rx_buf, int rx_max, uint32_t timeout_ms) {
    if (g_ops && g_ops->tx_frame && tx_len > 0) g_ops->tx_frame(g_ctx, tx, tx_len);
    uint32_t start = Timer::GetTicks();
    while ((uint32_t)(Timer::GetTicks() - start) < timeout_ms) {
        int rlen = rx_one(rx_buf, rx_max);
        if (rlen > 0 && match(rx_buf, rlen)) return rlen;
        if (rlen <= 0) Scheduler::SleepMs(1);
    }
    return 0;
}

// frame matchers (check it's the management subtype we're waiting for) (satoru)
static bool is_mgmt(const uint8_t* f, int len, uint8_t want) {
    if (len < (int)sizeof(Mac80211Hdr)) return false;
    uint16_t fc = le16(f);
    return ((fc >> 2) & 0x3) == IEEE80211_FTYPE_MGMT && ((fc >> 4) & 0xF) == want;
}
static bool match_auth(const uint8_t* f, int len)        { return is_mgmt(f, len, IEEE80211_STYPE_AUTH); }
static bool match_assoc_resp(const uint8_t* f, int len)  { return is_mgmt(f, len, IEEE80211_STYPE_ASSOC_RESP); }

// is this an eapol-key data frame? (llc/snap ethertype 0x888e). (satoru)
// data-frame layout: mac hdr(24) [+qos(2)] then llc/snap(8) then eapol. (satoru)
static const uint8_t* eapol_body(const uint8_t* f, int len, int* out_len) {
    if (len < (int)sizeof(Mac80211Hdr) + 8) return nullptr;
    uint16_t fc = le16(f);
    if (((fc >> 2) & 0x3) != IEEE80211_FTYPE_DATA) return nullptr;
    uint8_t stype = (fc >> 4) & 0xF;
    int hdr = sizeof(Mac80211Hdr);
    if (stype & 0x08) hdr += 2;                // qos-data has a 2-byte qos ctrl (satoru)
    if (len < hdr + 8) return nullptr;
    const uint8_t* snap = f + hdr;
    // llc/snap: aa aa 03 00 00 00 <ethertype:2> (satoru)
    if (!(snap[0] == 0xaa && snap[1] == 0xaa && snap[2] == 0x03)) return nullptr;
    uint16_t et = be16(snap + 6);
    if (et != 0x888e) return nullptr;          // 802.1x / eapol (satoru)
    if (out_len) *out_len = len - (hdr + 8);
    return f + hdr + 8;
}

// ── eapol-key tx (msg 2 + msg 4) ─────────────────────────────────────── (satoru)
// build a data frame carrying an eapol-key body and send it. the eapol body is
// supplied; we wrap it in mac hdr + llc/snap. (satoru)
static bool send_eapol(const uint8_t* eapol, int eapol_len) {
    uint8_t frame[512];
    // data frame, to-ds (sta->ap): addr1=bssid, addr2=sta, addr3=bssid. (satoru)
    int pos = BuildMacHeader(frame, IEEE80211_FTYPE_DATA, IEEE80211_STYPE_DATA,
                             IEEE80211_FC_TODS, g_bssid, g_sta_mac, g_bssid, g_seq++);
    // llc/snap header for 802.1x (satoru)
    static const uint8_t SNAP[8] = { 0xaa,0xaa,0x03,0x00,0x00,0x00,0x88,0x8e };
    i_memcpy(frame + pos, SNAP, 8); pos += 8;
    if (pos + eapol_len > (int)sizeof(frame)) return false;
    i_memcpy(frame + pos, eapol, eapol_len); pos += eapol_len;
    if (g_ops && g_ops->tx_frame) return g_ops->tx_frame(g_ctx, frame, pos);
    return false;
}

// fill an EapolKey reply (msg 2 or 4). key_info is the flags to set; mic is
// computed over the whole eapol frame with mic field zeroed, using kck. (satoru)
static int build_eapol_reply(uint8_t* buf, uint16_t key_info,
                             const uint8_t* snonce_or_null,
                             const uint8_t replay[8]) {
    i_memset(buf, 0, sizeof(EapolKey));
    EapolKey* k = (EapolKey*)buf;
    k->version = EAPOL_VERSION;
    k->type    = EAPOL_TYPE_KEY;
    k->descriptor_type = EAPOL_KEY_TYPE_RSN;
    put_be16((uint8_t*)&k->key_info, key_info);
    put_be16((uint8_t*)&k->key_length, 16);     // ccmp tk length (satoru)
    i_memcpy(k->replay_counter, replay, 8);
    if (snonce_or_null) i_memcpy(k->key_nonce, snonce_or_null, 32);
    put_be16((uint8_t*)&k->key_data_length, 0);
    // eapol body length = everything after the first 4 bytes (satoru)
    int body_len = (int)sizeof(EapolKey) - 4;
    put_be16((uint8_t*)&k->length, (uint16_t)body_len);

    int total = (int)sizeof(EapolKey);
    // compute mic over the full eapol frame with the mic field zeroed (satoru)
    if (key_info & WPA_KEY_INFO_MIC) {
        uint8_t mic[16];
        // kck is the first 16 bytes of the ptk (satoru)
        HmacSha1(g_ptk /*kck*/, 16, buf, total, mic);   // first 16 of the 20-byte digest (satoru)
        i_memcpy(k->key_mic, mic, 16);
    }
    return total;
}

// generate a station nonce. cooperative kernel, no rng device: mix the tsc and
// the timer ticks into a sha1 to get 32 reasonable bytes. NOT cryptographically
// strong, but the snonce only needs to be non-repeating per handshake; a real
// build would seed this from a hardware rng. (satoru)
static void gen_snonce(uint8_t out[32]) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));   // x86-64: edx:eax (satoru)
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    uint32_t t = Timer::GetTicks();
    uint8_t seed[20];
    for (int i = 0; i < 8; i++) seed[i] = (uint8_t)(tsc >> (8 * i));
    for (int i = 0; i < 4; i++) seed[8 + i] = (uint8_t)(t >> (8 * i));
    for (int i = 0; i < 6; i++) seed[12 + i] = g_sta_mac[i];
    seed[18] = (uint8_t)(g_seq & 0xff);
    seed[19] = (uint8_t)(g_tx_pn & 0xff);
    uint8_t d1[20], d2[20];
    Sha1(seed, 20, d1);
    seed[19] ^= 0xa5;
    Sha1(seed, 20, d2);
    i_memcpy(out, d1, 20);
    i_memcpy(out + 20, d2, 12);
}

// ── the wpa2 4-way handshake (supplicant side) ───────────────────────── (satoru)
//   ap → sta  msg1: ANonce                          (key_info: ack)
//   sta → ap  msg2: SNonce + mic                    (key_info: mic)
//   ap → sta  msg3: ANonce + gtk(encrypted) + mic   (key_info: ack|mic|install|secure|encr)
//   sta → ap  msg4: mic                             (key_info: mic|secure)
// ieee 802.11-2016 §12.7.6. (satoru)
static bool do_4way() {
    uint8_t rx[RX_FRAME_MAX];
    uint8_t reply[256];

    // ── wait for msg 1 (ack set, mic clear, has anonce) ──────────────── (satoru)
    g_state = SUPP_4WAY_MSG1;
    uint32_t start = Timer::GetTicks();
    const EapolKey* m1 = nullptr;
    while ((uint32_t)(Timer::GetTicks() - start) < 3000u) {
        int rlen = rx_one(rx, sizeof(rx));
        int elen = 0;
        const uint8_t* e = (rlen > 0) ? eapol_body(rx, rlen, &elen) : nullptr;
        if (e && elen >= (int)sizeof(EapolKey)) {
            const EapolKey* k = (const EapolKey*)e;
            uint16_t ki = be16((const uint8_t*)&k->key_info);
            if ((ki & WPA_KEY_INFO_ACK) && !(ki & WPA_KEY_INFO_MIC)) { m1 = k; break; }
        }
        if (rlen <= 0) Scheduler::SleepMs(1);
    }
    if (!m1) { log2("[80211] 4way: msg1 timeout", nullptr); return false; }

    // derive the ptk from pmk + anonce(from msg1) + our snonce + the two macs (satoru)
    i_memcpy(g_anonce, m1->key_nonce, 32);
    gen_snonce(g_snonce);
    DerivePtk(g_pmk, g_bssid /*aa*/, g_sta_mac /*spa*/, g_anonce, g_snonce, g_ptk, 48);
    g_have_ptk = true;
    i_memcpy(g_replay, m1->replay_counter, 8);

    // ── send msg 2 (snonce + mic, same replay counter) ───────────────── (satoru)
    int rlen2 = build_eapol_reply(reply, (uint16_t)(WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC),
                                  g_snonce, g_replay);
    if (!send_eapol(reply, rlen2)) { log2("[80211] 4way: msg2 tx failed", nullptr); return false; }
    log2("[80211] 4way: sent msg2 (snonce+mic)", nullptr);

    // ── wait for msg 3 (mic+ack+install+secure, encrypted key-data=gtk) ─ (satoru)
    g_state = SUPP_4WAY_MSG3;
    start = Timer::GetTicks();
    const EapolKey* m3 = nullptr;
    uint8_t m3buf[RX_FRAME_MAX];
    int m3_elen = 0;
    while ((uint32_t)(Timer::GetTicks() - start) < 3000u) {
        int rlen = rx_one(rx, sizeof(rx));
        int elen = 0;
        const uint8_t* e = (rlen > 0) ? eapol_body(rx, rlen, &elen) : nullptr;
        if (e && elen >= (int)sizeof(EapolKey)) {
            const EapolKey* k = (const EapolKey*)e;
            uint16_t ki = be16((const uint8_t*)&k->key_info);
            if ((ki & WPA_KEY_INFO_MIC) && (ki & WPA_KEY_INFO_ACK)) {
                i_memcpy(m3buf, e, elen); m3_elen = elen;
                m3 = (const EapolKey*)m3buf; break;
            }
        }
        if (rlen <= 0) Scheduler::SleepMs(1);
    }
    if (!m3) { log2("[80211] 4way: msg3 timeout", nullptr); return false; }

    // verify msg3 mic (recompute over the frame with mic zeroed, compare). (satoru)
    {
        uint8_t saved_mic[16];
        EapolKey* km = (EapolKey*)m3buf;
        i_memcpy(saved_mic, km->key_mic, 16);
        i_memset(km->key_mic, 0, 16);
        uint8_t calc[20];
        HmacSha1(g_ptk /*kck*/, 16, m3buf, m3_elen, calc);
        i_memcpy(km->key_mic, saved_mic, 16);   // restore (satoru)
        bool ok = true;
        for (int i = 0; i < 16; i++) if (calc[i] != saved_mic[i]) { ok = false; break; }
        if (!ok) { log2("[80211] 4way: msg3 MIC mismatch (bad passphrase?)", nullptr); g_state = SUPP_FAILED; return false; }
    }

    // unwrap the gtk from msg3's encrypted key-data using kek (aes key wrap). (satoru)
    {
        uint16_t kdl = be16((const uint8_t*)&m3->key_data_length);
        const uint8_t* kd = m3buf + sizeof(EapolKey);
        if (kdl >= 24 && (kdl % 8) == 0 && sizeof(EapolKey) + kdl <= (uint32_t)m3_elen) {
            uint8_t unwrapped[256];
            uint32_t uw_len = 0;
            // kek is ptk[16..31] (satoru)
            if (AesUnwrap(g_ptk + 16, kd, kdl, unwrapped, &uw_len)) {
                // key-data is rsn key-data: a kde (dd len 00-0f-ac 01 keyid ... gtk).
                // for simplicity grab the gtk from the first gtk-kde we find. (satoru)
                int i = 0;
                while (i + 2 <= (int)uw_len) {
                    uint8_t id = unwrapped[i], l = unwrapped[i + 1];
                    if (i + 2 + l > (int)uw_len) break;
                    if (id == IEEE80211_EID_VENDOR && l >= 6 &&
                        unwrapped[i+2]==0x00 && unwrapped[i+3]==0x0f && unwrapped[i+4]==0xac &&
                        unwrapped[i+5]==0x01 /*gtk kde*/) {
                        int gl = l - 6;
                        if (gl > 0 && gl <= 32) { g_gtk_len = gl; i_memcpy(g_gtk, unwrapped + i + 8, gl); }
                        break;
                    }
                    i += 2 + l;
                }
                log2("[80211] 4way: gtk unwrapped", nullptr);
            } else {
                log2("[80211] 4way: gtk unwrap failed (kek/integrity)", nullptr);
            }
        }
    }

    // ── send msg 4 (mic + secure, no key-data) ───────────────────────── (satoru)
    i_memcpy(g_replay, m3->replay_counter, 8);
    int rlen4 = build_eapol_reply(reply, (uint16_t)(WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE),
                                  nullptr, g_replay);
    if (!send_eapol(reply, rlen4)) { log2("[80211] 4way: msg4 tx failed", nullptr); return false; }
    log2("[80211] 4way: sent msg4 - handshake complete", nullptr);

    // install the pairwise key (tk = ptk[32..47]) into the hardware crypto engine
    // (or fall back to software ccmp if the driver can't offload). (satoru)
    if (g_ops->set_key) g_ops->set_key(g_ctx, 0, g_ptk + 32, 16, WIFI_KEY_CCMP);
    if (g_gtk_len > 0 && g_ops->set_key) g_ops->set_key(g_ctx, 1, g_gtk, g_gtk_len, WIFI_KEY_CCMP);

    g_state = SUPP_HANDSHAKE_DONE;
    g_tx_pn = 1;
    return true;
}

// derive the pmk for a wpa2-psk network from passphrase + ssid. (satoru)
static void derive_pmk(const char* pass, const char* ssid) {
    Pbkdf2HmacSha1((const uint8_t*)pass, (uint32_t)i_strlen(pass),
                   (const uint8_t*)ssid, (uint32_t)i_strlen(ssid),
                   4096, g_pmk, 32);
    g_have_pmk = true;
}

// ── connect: auth → assoc → (4-way if secured) ───────────────────────── (satoru)
bool Connect(const char* ssid, const char* pass, uint8_t bssid_out[6]) {
    if (!g_ops) { log2("[80211] connect: no radio registered", nullptr); return false; }

    // first scan to find the bssid + channel + security of the target ssid. (satoru)
    static WiFiNetwork found[NET_MAX_WIFI_NETS];
    int n = Scan(found, NET_MAX_WIFI_NETS);
    int target = -1;
    for (int i = 0; i < n; i++) {
        if (i_strlen(found[i].ssid) == i_strlen(ssid)) {
            bool eq = true;
            for (int j = 0; ssid[j]; j++) if (found[i].ssid[j] != ssid[j]) { eq = false; break; }
            if (eq) { target = i; break; }
        }
    }
    if (target < 0) { log2("[80211] connect: ssid not found in scan: ", ssid); return false; }

    i_memcpy(g_bssid, found[target].bssid.bytes, 6);
    if (bssid_out) i_memcpy(bssid_out, g_bssid, 6);
    int n2 = i_strlen(ssid); if (n2 > 32) n2 = 32;
    i_memcpy(g_ssid, ssid, n2); g_ssid[n2] = 0;
    WiFiSecurity sec = found[target].security;
    bool secured = (sec == WIFI_WPA2 || sec == WIFI_WPA);

    if (secured && (!pass || !*pass)) {
        log2("[80211] connect: network is secured but no passphrase given", nullptr);
        return false;
    }

    // tune + program the bss (satoru)
    if (g_ops->set_channel && found[target].channel > 0) g_ops->set_channel(g_ctx, found[target].channel);
    if (g_ops->config_bss) g_ops->config_bss(g_ctx, g_bssid, g_ssid);

    uint8_t frame[256];
    uint8_t rx[RX_FRAME_MAX];

    // ── open-system authentication ───────────────────────────────────── (satoru)
    g_state = SUPP_AUTHENTICATING;
    int flen = BuildAuthRequest(frame, sizeof(frame), g_sta_mac, g_bssid, g_seq++);
    int got = tx_and_wait(frame, flen, match_auth, rx, sizeof(rx), 2000u);
    if (got == 0) { log2("[80211] connect: auth timeout", nullptr); g_state = SUPP_FAILED; return false; }
    // check auth status code (offset hdr + algo(2) + seq(2)) == 0 (satoru)
    {
        int off = sizeof(Mac80211Hdr) + 4;
        if (got >= off + 2 && le16(rx + off) != 0) {
            log2("[80211] connect: auth rejected by ap", nullptr); g_state = SUPP_FAILED; return false;
        }
    }
    log2("[80211] connect: authenticated (open system)", nullptr);

    // ── association ──────────────────────────────────────────────────── (satoru)
    g_state = SUPP_ASSOCIATING;
    flen = BuildAssocRequest(frame, sizeof(frame), g_sta_mac, g_bssid, g_ssid,
                             sec == WIFI_WPA2, g_seq++);
    got = tx_and_wait(frame, flen, match_assoc_resp, rx, sizeof(rx), 2000u);
    if (got == 0) { log2("[80211] connect: assoc timeout", nullptr); g_state = SUPP_FAILED; return false; }
    {
        // assoc-resp body: caps(2) + status(2) + aid(2) (satoru)
        int off = sizeof(Mac80211Hdr) + 2;
        if (got >= off + 2 && le16(rx + off) != 0) {
            log2("[80211] connect: assoc rejected by ap", nullptr); g_state = SUPP_FAILED; return false;
        }
    }
    log2("[80211] connect: associated", nullptr);

    if (!secured) { g_state = SUPP_HANDSHAKE_DONE; return true; }

    // ── wpa2 4-way handshake ─────────────────────────────────────────── (satoru)
    derive_pmk(pass, g_ssid);
    log2("[80211] connect: pmk derived, starting 4-way handshake", nullptr);
    if (!do_4way()) { g_state = SUPP_FAILED; return false; }
    return true;
}

void Disconnect() {
    if (!g_ops) { g_state = SUPP_IDLE; return; }
    // send a deauth (reason 3 = sta leaving). (satoru)
    uint8_t frame[64];
    int pos = BuildMacHeader(frame, IEEE80211_FTYPE_MGMT, IEEE80211_STYPE_DEAUTH,
                             0, g_bssid, g_sta_mac, g_bssid, g_seq++);
    put_le16(frame + pos, 3); pos += 2;       // reason code (satoru)
    if (g_ops->tx_frame) g_ops->tx_frame(g_ctx, frame, pos);
    if (g_ops->stop) g_ops->stop(g_ctx);
    g_have_ptk = false; g_have_pmk = false; g_gtk_len = 0;
    g_state = SUPP_IDLE;
    log2("[80211] disconnected", nullptr);
}

const char* StateString() {
    switch (g_state) {
        case SUPP_IDLE:            return "idle";
        case SUPP_SCANNING:        return "scanning";
        case SUPP_AUTHENTICATING:  return "authenticating";
        case SUPP_ASSOCIATING:     return "associating";
        case SUPP_4WAY_MSG1:       return "4way-msg1";
        case SUPP_4WAY_MSG3:       return "4way-msg3";
        case SUPP_HANDSHAKE_DONE:  return "connected";
        case SUPP_FAILED:          return "failed";
    }
    return "unknown";
}

// ── ccmp data hooks (post-handshake) ─────────────────────────────────── (satoru)
// these build the ccmp nonce + aad from the negotiated tk and encrypt/decrypt a
// payload. like real ccmp, the 6-byte packet number (pn) travels in front of the
// ciphertext (the ccmp header carries the pn on the wire) so the rx side can
// reconstruct the exact nonce - making EncryptData/DecryptData a true round-trip.
// the aad here is a minimal fixed example of the masked header fields; a full
// data path derives aad from the live frame per §12.5.3.3.3. (satoru)
//
// out layout: pn(6) || ciphertext(len) || mic(8)  → out_len = len + 14. (satoru)
static void ccmp_nonce_aad(uint64_t pn, uint8_t nonce[13], uint8_t aad[22]) {
    nonce[0] = 0;                               // priority/tid 0, no mgmt bit (satoru)
    i_memcpy(nonce + 1, g_sta_mac, 6);          // a2 = transmitter (satoru)
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
    i_memset(aad, 0, 22);
    aad[0] = 0x88;                              // fc lo (data, qos) masked (satoru)
    i_memcpy(aad + 2, g_bssid, 6);
    i_memcpy(aad + 8, g_sta_mac, 6);
    i_memcpy(aad + 14, g_bssid, 6);
}

bool EncryptData(const uint8_t* plain, int len, uint8_t* out, int* out_len) {
    if (!g_have_ptk || len < 0) return false;
    const uint8_t* tk = g_ptk + 32;
    uint64_t pn = g_tx_pn++;
    uint8_t nonce[13], aad[22];
    ccmp_nonce_aad(pn, nonce, aad);
    // emit the pn (big-endian, 6 bytes) ahead of the ciphertext. (satoru)
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)(pn >> (8 * (5 - i)));
    CcmpEncrypt(tk, nonce, aad, 22, plain, (uint32_t)len, out + 6);
    if (out_len) *out_len = len + 14;           // pn(6) + cipher(len) + mic(8) (satoru)
    return true;
}
bool DecryptData(const uint8_t* cipher, int len, uint8_t* out, int* out_len) {
    if (!g_have_ptk || len < 6 + 8) return false;
    const uint8_t* tk = g_ptk + 32;
    // recover the pn from the leading 6 bytes, rebuild the identical nonce. (satoru)
    uint64_t pn = 0;
    for (int i = 0; i < 6; i++) pn = (pn << 8) | cipher[i];
    uint8_t nonce[13], aad[22];
    ccmp_nonce_aad(pn, nonce, aad);
    int clen = len - 6;                          // ciphertext + mic (satoru)
    if (!CcmpDecrypt(tk, nonce, aad, 22, cipher + 6, (uint32_t)clen, out)) return false;
    if (out_len) *out_len = clen - 8;
    return true;
}

} // namespace Ieee80211
// end (satoru)
