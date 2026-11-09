//  kurono os - usb audio class (uac1 / uac2) playback driver
//
//  design, after linux sound/usb:
//   - parse every configuration of the device (config.c / format.c / clock.c /
//     mixer.c ideas): interface association, audio control units, streaming
//     alternate settings (format type i pcm), rates, clock source, feature unit
//     volume/mute. uac3 configurations are recognised and skipped; if the device
//     offers a uac1/uac2 configuration (or badd fallback) that one is used.
//   - best alt = stereo 16 bit at 48000, then 24 bit, then 44100 (the mixer is
//     48 kHz, other rates are linearly resampled here).
//   - streaming: one isochronous OUT td per service interval (single isoch trb,
//     sia set, as linux xhci_queue_isoc_tx with URB_ISO_ASAP). tds are queued
//     just in time from the audio pump with a ~40 ms lead; the lead is paced by
//     the clock (the ring is not walked for completions, events only feed the
//     statistics and a safety bound). missed service / underrun events make the
//     pacer re-baseline.
//   - sync: adaptive and synchronous endpoints are fed at the nominal rate.
//     asynchronous endpoints run OPEN LOOP: the explicit feedback endpoint is
//     parsed and logged but not read yet (a long stream can drift).
//   - registered as the "usbaudio" backend; used automatically when a device
//     is present (hotplug switches to it, unplug falls back). (satoru)
#include "usb_audio.h"
#include "usb.h"
#include "audio_backend.h"
#include "audio_mixer.h"
#include "audio_server.h"
#include "serial.h"
#include "timer.h"
#include "../kernel/pmm.h"
#include "../proc/spinlock.h"
#include <stdarg.h>

namespace {

// ---- logging (same tiny %d %x %s formatter the usb driver uses) ----
void ua_log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char run[128]; int ri = 0;
    auto flush = [&](){ if (ri) { run[ri] = 0; SerialLogger::Log(run); ri = 0; } };
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            if (ri >= (int)sizeof(run) - 1) flush();
            run[ri++] = *p;
            continue;
        }
        ++p;
        flush();
        if      (*p == 'd') SerialLogger::LogDec(va_arg(ap, int));
        else if (*p == 'x') SerialLogger::LogHex((uint32_t)va_arg(ap, unsigned int));
        else if (*p == 's') SerialLogger::Log(va_arg(ap, const char*));
        else if (*p == '%') { run[ri++] = '%'; }
        else if (!*p) break;
    }
    flush();
    va_end(ap);
}

void ua_zero(void* p, uint32_t n) { uint8_t* d = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) d[i] = 0; }

inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint32_t le24(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

// ---- descriptor model ----
constexpr int MAX_ALTS  = 16;
constexpr int MAX_RATES = 12;
constexpr int MAX_FU    = 4;
constexpr int MAX_IT    = 6;
constexpr int MAX_CLK   = 4;

struct UacAlt {
    uint8_t  iface, alt;
    uint8_t  nch, subslot, bits, fmt_type;
    bool     pcm, have_fmt, has_out;
    uint8_t  ep_addr, ep_attr, b_interval;
    uint16_t wmps;
    uint8_t  burst, mult;                // ss companion (satoru)
    uint16_t bytes_per_interval;
    uint8_t  synch_addr;                 // uac1 bSynchAddress (satoru)
    uint8_t  fb_ep, fb_interval;         // explicit feedback endpoint, 0 = none (satoru)
    uint16_t fb_wmps;
    uint8_t  term_link;
    bool     ep_freq_ctl;                // uac1 cs endpoint: sampling frequency control (satoru)
    bool     implicit_fb;                // data endpoint usage = implicit feedback (satoru)
    uint8_t  nrates;
    bool     continuous;
    uint32_t rates[MAX_RATES];
    uint32_t rmin, rmax;
    int      score;
};

struct UacFu { uint8_t id, src; bool mute_m, vol_m, mute_c[2], vol_c[2]; };
struct UacIt { uint8_t id; uint16_t type; uint8_t clock; };
struct UacClk { uint8_t id, ctl; };

struct UacCfg {
    uint8_t  cfg_value, cfg_index;
    int      ver;                        // 1, 2; 3 = uac3 (unsupported) (satoru)
    bool     saw_ac;
    uint8_t  ac_iface;
    int      nfu;  UacFu  fu[MAX_FU];
    int      nit;  UacIt  it[MAX_IT];
    int      nclk; UacClk clk[MAX_CLK];
    bool     clk_selector;
    int      nalts; UacAlt alts[MAX_ALTS];
    int      best, best_score;
};

bool alt_has_rate(const UacAlt& a, uint32_t r) {
    if (a.continuous) return r >= a.rmin && r <= a.rmax;
    for (int i = 0; i < a.nrates; i++) if (a.rates[i] == r) return true;
    return false;
}

void uac_score(UacCfg& c) {
    c.best = -1; c.best_score = -1;
    if (c.ver != 1 && c.ver != 2) return;
    for (int i = 0; i < c.nalts; i++) {
        UacAlt& a = c.alts[i];
        a.score = -1;
        if (!a.have_fmt || !a.has_out || a.fmt_type != 1 || !a.pcm) continue;
        if (a.subslot < 2 || a.subslot > 4) continue;
        if (a.bits != 16 && a.bits != 24 && a.bits != 32) continue;
        if (a.nch < 1) continue;
        int s = 1000;
        s += (a.nch == 2) ? 200 : (a.nch > 2 ? 100 : 50);
        s += (a.bits == 16) ? 60 : (a.bits == 24 ? 40 : 10);
        if (c.ver == 1) s += alt_has_rate(a, 48000) ? 100 : (alt_has_rate(a, 44100) ? 50 : 10);
        else            s += 75;                       // uac2 rates are asked of the clock later (satoru)
        int sync = (a.ep_attr >> 2) & 3;               // 1 async, 2 adaptive, 3 sync (satoru)
        if (sync == 1 && !a.fb_ep) s -= 5;
        a.score = s;
        if (s > c.best_score) { c.best_score = s; c.best = i; }
    }
}

// walk one configuration descriptor block. true when a playback alt is usable.
bool uac_parse(const uint8_t* d, int len, UacCfg& c) {
    ua_zero(&c, sizeof(c));
    c.best = -1;
    int cur_proto = 0, cur_alt = -1;
    bool in_ac = false, in_as = false;
    int last_ep = 0;                                   // 1 data, 2 feedback (satoru)
    int p = 0;
    while (p + 2 <= len) {
        int l = d[p], t = d[p + 1];
        if (l < 2 || p + l > len) break;
        const uint8_t* q = d + p;
        if (t == 0x02 && l >= 9) {
            c.cfg_value = q[5];
        } else if (t == 0x04 && l >= 9) {
            int cls = q[5], sub = q[6]; cur_proto = q[7];
            in_ac = (cls == 1 && sub == 1);
            in_as = (cls == 1 && sub == 2);
            cur_alt = -1; last_ep = 0;
            if (in_ac) {
                c.saw_ac = true; c.ac_iface = q[2];
                if (cur_proto == 0x30) c.ver = 3;
                else if (cur_proto == 0x20 && c.ver != 3) c.ver = 2;
                else if (cur_proto == 0 && c.ver == 0) c.ver = 1;
            }
            if (in_as && cur_proto == 0x30) c.ver = 3;
            if (in_as && q[3] != 0 && q[4] > 0 && c.nalts < MAX_ALTS) {
                cur_alt = c.nalts++;
                c.alts[cur_alt].iface = q[2];
                c.alts[cur_alt].alt = q[3];
            }
        } else if (t == 0x24 && l >= 3) {              // class specific interface (satoru)
            int st = q[2];
            if (in_ac) {
                if (st == 0x01 && l >= 5) {            // header: bcdADC (satoru)
                    uint16_t bcd = le16(q + 3);
                    if (c.ver != 3) c.ver = (bcd >= 0x0200) ? 2 : 1;
                } else if (st == 0x02 && l >= 8 && c.nit < MAX_IT) {   // input terminal (satoru)
                    UacIt& it = c.it[c.nit++];
                    it.id = q[3]; it.type = le16(q + 4); it.clock = (l >= 17) ? q[7] : 0;
                } else if (st == 0x06 && l >= 7 && c.nfu < MAX_FU) {   // feature unit (satoru)
                    UacFu& f = c.fu[c.nfu++];
                    ua_zero(&f, sizeof(f));
                    f.id = q[3]; f.src = q[4];
                    if (c.ver == 2) {
                        // 4 bytes of controls per channel; 2 bits per control (satoru)
                        if (l >= 9)  { uint32_t m = le32(q + 5);  f.mute_m = (m & 3) != 0; f.vol_m = ((m >> 2) & 3) != 0; }
                        if (l >= 13) { uint32_t m = le32(q + 9);  f.mute_c[0] = (m & 3) != 0; f.vol_c[0] = ((m >> 2) & 3) != 0; }
                        if (l >= 17) { uint32_t m = le32(q + 13); f.mute_c[1] = (m & 3) != 0; f.vol_c[1] = ((m >> 2) & 3) != 0; }
                    } else {
                        int sz = q[5]; if (sz < 1) sz = 1;
                        auto ctl = [&](int ch) -> uint32_t {
                            int off = 6 + ch * sz;
                            if (off + sz > l) return 0;
                            return sz >= 2 ? le16(q + off) : q[off];
                        };
                        uint32_t m = ctl(0); f.mute_m = m & 1; f.vol_m = (m >> 1) & 1;
                        m = ctl(1); f.mute_c[0] = m & 1; f.vol_c[0] = (m >> 1) & 1;
                        m = ctl(2); f.mute_c[1] = m & 1; f.vol_c[1] = (m >> 1) & 1;
                    }
                } else if (st == 0x0A && l >= 6 && c.nclk < MAX_CLK) { // uac2 clock source (satoru)
                    c.clk[c.nclk].id = q[3]; c.clk[c.nclk].ctl = q[5]; c.nclk++;
                } else if (st == 0x0B) {
                    c.clk_selector = true;
                }
            } else if (in_as && cur_alt >= 0) {
                UacAlt& a = c.alts[cur_alt];
                if (st == 0x01) {                      // AS_GENERAL (satoru)
                    if (cur_proto == 0x20 && l >= 16) {
                        a.term_link = q[3]; a.fmt_type = q[5];
                        a.pcm = (le32(q + 6) & 1) != 0;
                        a.nch = q[10];
                    } else if (cur_proto != 0x20 && l >= 7) {
                        a.term_link = q[3];
                        a.pcm = (le16(q + 5) == 1);
                    }
                } else if (st == 0x02) {               // FORMAT_TYPE (satoru)
                    if (cur_proto == 0x20 && l >= 6) {
                        a.fmt_type = q[3]; a.subslot = q[4]; a.bits = q[5]; a.have_fmt = true;
                    } else if (cur_proto != 0x20 && l >= 8) {
                        a.fmt_type = q[3]; a.nch = q[4]; a.subslot = q[5]; a.bits = q[6];
                        a.have_fmt = true;
                        int nf = q[7];
                        if (nf == 0 && l >= 14) {
                            a.continuous = true; a.rmin = le24(q + 8); a.rmax = le24(q + 11);
                        } else {
                            for (int i = 0; i < nf && i < MAX_RATES && 8 + i * 3 + 3 <= l; i++)
                                a.rates[a.nrates++] = le24(q + 8 + i * 3);
                        }
                    }
                }
            }
        } else if (t == 0x05 && l >= 7 && cur_alt >= 0 && in_as) {   // endpoint (satoru)
            UacAlt& a = c.alts[cur_alt];
            uint8_t addr = q[2], attr = q[3];
            uint16_t wmps = le16(q + 4);
            uint8_t bint = q[6];
            int usage = (attr >> 4) & 3;
            last_ep = 0;
            if ((attr & 3) == 1) {                                    // isochronous (satoru)
                if (!(addr & 0x80)) {
                    if (!a.has_out) {
                        a.has_out = true; a.ep_addr = addr; a.ep_attr = attr; a.wmps = wmps; a.b_interval = bint;
                        if (cur_proto != 0x20 && l >= 9) a.synch_addr = q[8];
                        last_ep = 1;
                    }
                } else if (usage == 1 || (a.synch_addr && addr == a.synch_addr)) {
                    if (!a.fb_ep) { a.fb_ep = addr; a.fb_interval = bint; a.fb_wmps = wmps; }
                    last_ep = 2;
                } else if (usage == 2) {
                    a.implicit_fb = true;
                }
                if (!(addr & 0x80) && usage == 2) a.implicit_fb = true;
            }
        } else if (t == 0x30 && l >= 6 && cur_alt >= 0) {             // ss endpoint companion (satoru)
            UacAlt& a = c.alts[cur_alt];
            if (last_ep == 1) { a.burst = q[2]; a.mult = q[3] & 3; a.bytes_per_interval = le16(q + 4); }
        } else if (t == 0x25 && l >= 4 && cur_alt >= 0 && in_as && q[2] == 0x01) {   // cs endpoint (satoru)
            if (cur_proto != 0x20) c.alts[cur_alt].ep_freq_ctl = (q[3] & 1) != 0;
        }
        p += l;
    }
    uac_score(c);
    return c.best >= 0;
}

const UacFu* uac_find_fu(const UacCfg& c, const UacAlt& a) {
    for (int i = 0; i < c.nfu; i++) if (c.fu[i].src == a.term_link && (c.fu[i].mute_m || c.fu[i].vol_m || c.fu[i].vol_c[0] || c.fu[i].mute_c[0])) return &c.fu[i];
    for (int i = 0; i < c.nfu; i++) if (c.fu[i].src == a.term_link) return &c.fu[i];
    return nullptr;
}
uint8_t uac_find_clock(const UacCfg& c, const UacAlt& a) {
    for (int i = 0; i < c.nit; i++) if (c.it[i].id == a.term_link) return c.it[i].clock;
    return c.nclk ? c.clk[0].id : 0;
}

// ---- the one playback device we drive ----
struct Dev {
    bool     have;                 // parsed and selected (satoru)
    bool     open;                 // iso ep configured + alt set + rate set (satoru)
    int      gid;
    uint8_t  speed;
    uint16_t vid, pid;
    char     name[40];
    UacCfg   cfg;
    UacAlt   alt;
    uint8_t  fu_id; const char* fu_desc;
    bool     fu_mute, fu_vol, fu_master;
    uint8_t  clock_id; bool clk_prog;
    uint32_t rate;                 // device sample rate (satoru)
    uint32_t period_us;
    int16_t  vmin, vmax, vres;
    int      nconfigs, cfg_chosen;
} g_dev;

// feeder state; touched under the audio pump lock except where noted (satoru)
constexpr uint32_t RING_FRAMES = 16384;
constexpr uint32_t NSLOTS      = 480;
int16_t  g_ring[RING_FRAMES * 2];
uint32_t g_rd = 0, g_wr = 0, g_cnt = 0;
uint8_t* g_slots = nullptr; uint32_t g_slot_size = 0; uint32_t g_slots_bytes = 0;
uint64_t g_q_total = 0, g_q_base = 0;
uint32_t g_t_base = 0;
uint64_t g_acc = 0;                                 // fractional frames, units of 1e-6 (satoru)
uint32_t g_lead_tds = 0, g_frame_bytes = 0, g_nominal_frames = 0;
uint32_t g_rs_pos = 0; int16_t g_rs_hist[2] = {0, 0};   // resampler state (satoru)
volatile uint64_t g_compl_total = 0; volatile uint32_t g_last_k = 0;
volatile uint32_t g_ev_ok = 0, g_ev_underrun = 0, g_ev_overrun = 0, g_ev_missed = 0, g_ev_err = 0;
volatile uint32_t g_resyncs = 0, g_recoveries = 0, g_ev_last_cc = 0;
volatile bool g_need_recover = false;
uint32_t g_ring_max = 0;

// service() flags, set from hooks / other contexts (satoru)
volatile bool g_pending_attach = false, g_pending_detach = false, g_pending_restart = false;
volatile bool g_audio_up = false;                   // AudioServer::Init has run (satoru)
bool g_user_pref_set = false;

constexpr int LEAD_MS = 40;

// ---- control requests ----
bool ctl_out(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    return USB::GidControl(g_dev.gid, bm, req, val, idx, len, data);
}
bool ctl_in(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    return USB::GidControl(g_dev.gid, bm, req, val, idx, len, data);
}

// uac2: ask the clock source which rates it can run (satoru)
int v2_ranges(uint32_t (*out)[3], int max) {
    uint8_t buf[2 + 12 * 8];
    ua_zero(buf, sizeof(buf));
    uint16_t widx = (uint16_t)((g_dev.clock_id << 8) | g_dev.cfg.ac_iface);
    if (!ctl_in(0xA1, 0x02, 0x0100, widx, buf, sizeof(buf))) return -1;
    int n = le16(buf);
    if (n > 8) n = 8;
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        out[k][0] = le32(buf + 2 + i * 12);
        out[k][1] = le32(buf + 6 + i * 12);
        out[k][2] = le32(buf + 10 + i * 12);
        k++;
    }
    return k;
}
bool v2_range_has(uint32_t (*r)[3], int n, uint32_t rate) {
    for (int i = 0; i < n; i++) {
        if (rate < r[i][0] || rate > r[i][1]) continue;
        if (r[i][2] == 0 || ((rate - r[i][0]) % r[i][2]) == 0) return true;
    }
    return false;
}

const uint32_t kRatePref[] = { 48000, 44100, 96000, 88200, 32000, 192000, 176400, 24000, 22050, 16000, 12000, 11025, 8000 };

// choose the device rate and, when the device can be told, program it (satoru)
bool pick_and_set_rate() {
    UacAlt& a = g_dev.alt;
    uint32_t chosen = 0;
    if (g_dev.cfg.ver == 1) {
        for (unsigned i = 0; i < sizeof(kRatePref) / sizeof(kRatePref[0]) && !chosen; i++)
            if (alt_has_rate(a, kRatePref[i])) chosen = kRatePref[i];
        if (!chosen && a.nrates) chosen = a.rates[0];
        if (!chosen && a.continuous) chosen = a.rmin;
        if (!chosen) return false;
        if (a.ep_freq_ctl) {
            uint8_t d3[3] = { (uint8_t)chosen, (uint8_t)(chosen >> 8), (uint8_t)(chosen >> 16) };
            bool ok = ctl_out(0x22, 0x01, 0x0100, a.ep_addr, d3, 3);
            ua_log("[usbaudio] uac1 set sampling freq %d on ep %x: %s\n", (int)chosen, (uint32_t)a.ep_addr, ok ? "ok" : "refused");
        }
    } else {
        uint32_t r[8][3];
        int n = v2_ranges(r, 8);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                ua_log("[usbaudio] uac2 clock %d range %d..%d step %d\n", (int)g_dev.clock_id, (int)r[i][0], (int)r[i][1], (int)r[i][2]);
            for (unsigned i = 0; i < sizeof(kRatePref) / sizeof(kRatePref[0]) && !chosen; i++)
                if (v2_range_has(r, n, kRatePref[i])) chosen = kRatePref[i];
            if (!chosen) chosen = r[0][0];
        } else {
            chosen = 48000;
            ua_log("[usbaudio] uac2 rate range query failed, assuming 48000\n");
        }
        if (g_dev.clock_id) {
            uint16_t widx = (uint16_t)((g_dev.clock_id << 8) | g_dev.cfg.ac_iface);
            uint8_t d4[4] = { (uint8_t)chosen, (uint8_t)(chosen >> 8), (uint8_t)(chosen >> 16), (uint8_t)(chosen >> 24) };
            bool ok = ctl_out(0x21, 0x01, 0x0100, widx, d4, 4);
            uint8_t g4[4] = {0, 0, 0, 0};
            bool ok2 = ctl_in(0xA1, 0x01, 0x0100, widx, g4, 4);
            uint32_t got = ok2 ? le32(g4) : 0;
            ua_log("[usbaudio] uac2 set clock %d to %d: set %s, readback %d\n", (int)g_dev.clock_id, (int)chosen,
                   ok ? "ok" : "refused", (int)got);
            if (ok2 && got >= 8000 && got <= 384000) chosen = got;
        }
    }
    g_dev.rate = chosen;
    return true;
}

// ---- feature unit volume / mute ----
bool fu_get(uint8_t req_v1, uint8_t cs, uint8_t ch, int16_t* out) {
    uint8_t b[2] = {0, 0};
    uint16_t widx = (uint16_t)((g_dev.fu_id << 8) | g_dev.cfg.ac_iface);
    if (!ctl_in(0xA1, req_v1, (uint16_t)((cs << 8) | ch), widx, b, 2)) return false;
    *out = (int16_t)le16(b);
    return true;
}
void fu_read_range(uint8_t ch) {
    g_dev.vmin = g_dev.vmax = g_dev.vres = 0;
    uint16_t widx = (uint16_t)((g_dev.fu_id << 8) | g_dev.cfg.ac_iface);
    if (g_dev.cfg.ver == 2) {
        // uac2 volume is a 2-byte control, so its RANGE parameter block is
        // wNumSubRanges + wMIN + wMAX + wRES = 8 bytes. asking for more made the
        // device stall ep0 (code 6) and left the range at 0..0 (no volume). (satoru)
        uint8_t b[2 + 3 * 2];
        ua_zero(b, sizeof(b));
        if (ctl_in(0xA1, 0x02, (uint16_t)((2 << 8) | ch), widx, b, sizeof(b)) && le16(b) >= 1) {
            g_dev.vmin = (int16_t)le16(b + 2); g_dev.vmax = (int16_t)le16(b + 4); g_dev.vres = (int16_t)le16(b + 6);
        }
    } else {
        int16_t mn = 0, mx = 0, rs = 0;
        if (fu_get(0x82, 2, ch, &mn) && fu_get(0x83, 2, ch, &mx)) {
            g_dev.vmin = mn; g_dev.vmax = mx;
            if (fu_get(0x84, 2, ch, &rs)) g_dev.vres = rs;
        }
    }
}
bool fu_set_vol_ch(uint8_t ch, int16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)((uint16_t)v >> 8) };
    uint16_t widx = (uint16_t)((g_dev.fu_id << 8) | g_dev.cfg.ac_iface);
    return ctl_out(0x21, 0x01, (uint16_t)((2 << 8) | ch), widx, b, 2);
}
bool fu_set_mute_ch(uint8_t ch, bool mute) {
    uint8_t b = mute ? 1 : 0;
    uint16_t widx = (uint16_t)((g_dev.fu_id << 8) | g_dev.cfg.ac_iface);
    return ctl_out(0x21, 0x01, (uint16_t)((1 << 8) | ch), widx, &b, 1);
}
void fu_apply_volume(int pct) {
    if (!g_dev.fu_id || !g_dev.fu_vol || g_dev.vmax <= g_dev.vmin) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int32_t span = (int32_t)g_dev.vmax - (int32_t)g_dev.vmin;
    int32_t v = (int32_t)g_dev.vmin + span * pct / 100;
    if (g_dev.vres > 0) v = g_dev.vmin + ((v - g_dev.vmin) / g_dev.vres) * g_dev.vres;
    if (g_dev.fu_master) fu_set_vol_ch(0, (int16_t)v);
    else { fu_set_vol_ch(1, (int16_t)v); fu_set_vol_ch(2, (int16_t)v); }
}

// ---- the backend ----
int g_master_vol = 80;

class UsbAudioBackend final : public AudioBackend {
public:
    const char* Name() const override { return "usbaudio"; }
    bool Init() override;
    bool IsReady() const override { return ready_ && g_dev.have && g_dev.open; }
    uint32_t Submit(const int16_t* pcm, uint32_t frames) override;
    uint32_t QueuedFrames() const override { return queued_; }
    uint32_t SampleRate() const override { return AudioMixer::INTERNAL_RATE; }   // mixer rate; resampled inside (satoru)
    void Stop() override;
    void SetMasterVolume(int v) override { g_master_vol = v; if (ready_) fu_apply_volume(v); }
    int  GetMasterVolume() const override { return g_master_vol; }
    void Tick() override;
    bool Idle(bool first) override;
    void Describe(char* out, int max_len) const override;

    bool Start();
    void Close();
    void Pump();
    void OnEvent(uint64_t ptr, uint8_t cc);
    bool ready_ = false;
    uint32_t queued_ = 0;
};
UsbAudioBackend g_be;

// number of frames the next packet carries, without committing the accumulator (satoru)
uint32_t next_packet_frames(uint64_t* acc_out) {
    uint64_t acc = g_acc + (uint64_t)g_dev.rate * g_dev.period_us;
    uint32_t fr = (uint32_t)(acc / 1000000u);
    *acc_out = acc % 1000000u;
    return fr;
}

bool UsbAudioBackend::Start() {
    if (!g_dev.have) return false;
    if (g_dev.open) { ready_ = true; return true; }
    UacAlt& a = g_dev.alt;
    const USBDeviceInfo* info = USB::GidInfo(g_dev.gid);
    if (!info) return false;
    g_dev.speed = info->speed;

    if (!pick_and_set_rate()) { ua_log("[usbaudio] no usable sample rate\n"); return false; }

    UsbIsoParams ip;
    ua_zero(&ip, sizeof(ip));
    ip.ep_addr = a.ep_addr;
    ip.max_packet = (uint16_t)(a.wmps & 0x7FF);
    ip.b_interval = a.b_interval;
    if (g_dev.speed == USB_SPEED_SUPER) { ip.burst = a.burst; ip.mult = a.mult; ip.bytes_per_interval = a.bytes_per_interval; }
    else if (g_dev.speed == USB_SPEED_HIGH) { ip.burst = (uint8_t)((a.wmps >> 11) & 3); }
    g_dev.period_us = 0;
    // the nominal packet size tells the controller how much bandwidth to reserve (satoru)
    uint32_t fb = (uint32_t)a.nch * a.subslot;
    ip.avg_bytes = 0;
    uint32_t period = 0;
    // period depends on the interval; IsoOpen reports it back (satoru)
    {
        uint32_t bi = a.b_interval; if (bi < 1) bi = 1; if (bi > 16) bi = 16;
        uint32_t x = bi - 1; if (g_dev.speed == USB_SPEED_FULL) x += 3; if (x > 15) x = 15;
        period = 125u << x;
    }
    uint32_t nom = (uint32_t)(((uint64_t)g_dev.rate * period + 999999u) / 1000000u);
    ip.avg_bytes = nom * fb;
    uint32_t maxpay = (uint32_t)ip.max_packet * (ip.burst + 1u);
    if (g_dev.speed == USB_SPEED_SUPER && ip.bytes_per_interval) maxpay = ip.bytes_per_interval;
    if (nom * fb > maxpay) {
        ua_log("[usbaudio] packet %d bytes does not fit max payload %d (rate %d, interval %d us)\n",
               (int)(nom * fb), (int)maxpay, (int)g_dev.rate, (int)period);
        return false;
    }
    if (!USB::IsoOpen(g_dev.gid, ip, &g_dev.period_us)) { ua_log("[usbaudio] iso endpoint open failed\n"); return false; }

    bool si = ctl_out(0x01, 0x0B, a.alt, a.iface, nullptr, 0);
    ua_log("[usbaudio] set interface %d alt %d: %s\n", (int)a.iface, (int)a.alt, si ? "ok" : "REFUSED");
    if (!si) { USB::IsoClose(g_dev.gid); return false; }

    // slot buffer: one power-of-two slot per td, so no td ever crosses a 64 kb boundary (satoru)
    g_frame_bytes = fb;
    g_nominal_frames = nom;
    uint32_t need = nom * fb, sz = 16;
    while (sz < need) sz <<= 1;
    if (sz > 4096) { ua_log("[usbaudio] td too large (%d)\n", (int)need); ctl_out(0x01, 0x0B, 0, a.iface, nullptr, 0); USB::IsoClose(g_dev.gid); return false; }
    if (!g_slots || g_slots_bytes < sz * NSLOTS) {
        if (g_slots) PMM::FreeBytes(g_slots, g_slots_bytes);
        g_slots_bytes = sz * NSLOTS;
        g_slots = (uint8_t*)PMM::AllocBytes(g_slots_bytes);
        if (!g_slots) { g_slots_bytes = 0; USB::IsoClose(g_dev.gid); return false; }
    }
    g_slot_size = sz;
    for (uint32_t i = 0; i < g_slots_bytes; i++) g_slots[i] = 0;

    uint32_t lead = (uint32_t)((uint64_t)LEAD_MS * 1000u / g_dev.period_us);
    if (lead < 8) lead = 8;
    if (lead > 400) lead = 400;
    g_lead_tds = lead;

    // volume / mute: pick the feature unit that feeds this stream
    const UacFu* fu = uac_find_fu(g_dev.cfg, a);
    g_dev.fu_id = 0; g_dev.fu_vol = g_dev.fu_mute = g_dev.fu_master = false;
    if (fu) {
        g_dev.fu_id = fu->id;
        g_dev.fu_master = fu->vol_m || fu->mute_m;
        g_dev.fu_vol = fu->vol_m || fu->vol_c[0];
        g_dev.fu_mute = fu->mute_m || fu->mute_c[0];
        if (g_dev.fu_mute) {
            if (fu->mute_m) fu_set_mute_ch(0, false);
            else { fu_set_mute_ch(1, false); fu_set_mute_ch(2, false); }
        }
        if (g_dev.fu_vol) {
            g_dev.fu_master = fu->vol_m;
            fu_read_range(g_dev.fu_master ? 0 : 1);
            fu_apply_volume(g_master_vol);
        }
        ua_log("[usbaudio] feature unit %d: mute %d volume %d (%s) range %d..%d step %d (1/256 dB)\n", (int)fu->id,
               g_dev.fu_mute ? 1 : 0, g_dev.fu_vol ? 1 : 0, g_dev.fu_master ? "master" : "per channel",
               (int)g_dev.vmin, (int)g_dev.vmax, (int)g_dev.vres);
    } else {
        ua_log("[usbaudio] no feature unit for terminal %d: device volume not controllable\n", (int)a.term_link);
    }

    {
        SpinLockCpuGuard guard(AudioServer::PumpLock());
        g_rd = g_wr = g_cnt = 0; g_acc = 0; g_q_total = g_q_base = 0; g_t_base = Timer::GetRealMs();
        g_compl_total = 0; g_last_k = 0; g_rs_pos = 0; g_rs_hist[0] = g_rs_hist[1] = 0;
        g_dev.open = true; ready_ = true; queued_ = 0;
    }
    ua_log("[usbaudio] streaming ready: %d Hz, %d ch, %d bit in %d byte slots, %s endpoint, %d us interval, lead %d tds%s\n",
           (int)g_dev.rate, (int)a.nch, (int)a.bits, (int)a.subslot,
           ((a.ep_attr >> 2) & 3) == 1 ? "asynchronous" : (((a.ep_attr >> 2) & 3) == 2 ? "adaptive" : "synchronous"),
           (int)g_dev.period_us, (int)g_lead_tds,
           (((a.ep_attr >> 2) & 3) == 1) ? (a.fb_ep ? " (feedback ep present but NOT read: open loop)" : " (no feedback ep: open loop)") : "");
    return true;
}

void UsbAudioBackend::Close() {
    {
        SpinLockCpuGuard guard(AudioServer::PumpLock());
        ready_ = false; g_dev.open = false;
    }
    if (g_dev.have) {
        // alt 0 first (device stops), then give the bandwidth back (satoru)
        ctl_out(0x01, 0x0B, 0, g_dev.alt.iface, nullptr, 0);
        USB::IsoClose(g_dev.gid);
    }
}

bool UsbAudioBackend::Init() {
    if (!g_dev.have) return false;
    return Start();
}

void UsbAudioBackend::Stop() { Close(); }

// build and queue as many tds as the lead allows (pump lock held) (satoru)
void UsbAudioBackend::Pump() {
    if (!ready_ || !g_dev.open || !g_slots) return;
    uint32_t now = Timer::GetRealMs();
    uint64_t est = g_q_base + ((uint64_t)(now - g_t_base) * 1000u) / g_dev.period_us;
    if (g_need_recover) { g_need_recover = false; est = g_q_total + 1; g_resyncs++; }
    if (est >= g_q_total) {                 // drained (idle gap or underrun): new baseline (satoru)
        g_q_base = g_q_total; g_t_base = now; est = g_q_total;
    }
    uint64_t inflight = g_q_total - est;
    bool any = false;
    int guard = 0;
    while (inflight < g_lead_tds && guard++ < 600) {
        uint64_t nacc;
        uint32_t fr = next_packet_frames(&nacc);
        if (g_cnt < fr) break;
        const UacAlt& a = g_dev.alt;
        uint8_t* slot = g_slots + (uint32_t)(g_q_total % NSLOTS) * g_slot_size;
        uint32_t o = 0;
        for (uint32_t f = 0; f < fr; f++) {
            int16_t l = g_ring[g_rd * 2], r = g_ring[g_rd * 2 + 1];
            g_rd = (g_rd + 1) % RING_FRAMES;
            for (uint32_t ch = 0; ch < a.nch; ch++) {
                int16_t s = (a.nch == 1) ? (int16_t)(((int32_t)l + r) / 2) : (ch == 0 ? l : (ch == 1 ? r : 0));
                if (a.subslot == 2)      { slot[o++] = (uint8_t)s; slot[o++] = (uint8_t)((uint16_t)s >> 8); }
                else if (a.subslot == 3) { slot[o++] = 0; slot[o++] = (uint8_t)s; slot[o++] = (uint8_t)((uint16_t)s >> 8); }
                else                     { slot[o++] = 0; slot[o++] = 0; slot[o++] = (uint8_t)s; slot[o++] = (uint8_t)((uint16_t)s >> 8); }
            }
        }
        g_cnt -= fr;
        g_acc = nacc;
        bool ioc = ((g_q_total & 15) == 15);
        if (!USB::IsoQueue(g_dev.gid, slot, o, ioc)) { g_ev_err++; break; }
        g_q_total++; inflight++; any = true;
    }
    if (any) USB::IsoKick(g_dev.gid);
}

void UsbAudioBackend::Tick() {
    if (!ready_) { queued_ = 0; return; }
    Pump();
    uint32_t now = Timer::GetRealMs();
    uint64_t est = g_q_base + ((uint64_t)(now - g_t_base) * 1000u) / g_dev.period_us;
    uint64_t inflight = (est >= g_q_total) ? 0 : g_q_total - est;
    uint64_t fr = g_cnt + inflight * g_nominal_frames;      // device-rate frames (satoru)
    fr = fr * AudioMixer::INTERNAL_RATE / (g_dev.rate ? g_dev.rate : 48000);
    queued_ = (uint32_t)fr;
}

bool UsbAudioBackend::Idle(bool first) {
    (void)first;
    if (!ready_) return false;
    Pump();
    uint64_t acc; uint32_t need = next_packet_frames(&acc);
    if (g_cnt < need) { g_cnt = 0; g_rd = g_wr; g_rs_pos = 0; return false; }   // tail shorter than a packet: drop (satoru)
    return true;
}

uint32_t UsbAudioBackend::Submit(const int16_t* pcm, uint32_t frames) {
    if (!ready_ || !pcm || !g_dev.open) return 0;
    uint32_t drate = g_dev.rate ? g_dev.rate : 48000;
    if (drate == AudioMixer::INTERNAL_RATE) {
        if (RING_FRAMES - g_cnt < frames) return 0;
        for (uint32_t i = 0; i < frames; i++) {
            g_ring[g_wr * 2] = pcm[i * 2]; g_ring[g_wr * 2 + 1] = pcm[i * 2 + 1];
            g_wr = (g_wr + 1) % RING_FRAMES;
        }
        g_cnt += frames;
    } else {
        // linear resample 48000 -> drate. y[0] is the previous block's last frame,
        // y[1..frames] this block; pos advances by 48000/drate input frames per output (satoru)
        uint32_t step = (uint32_t)(((uint64_t)AudioMixer::INTERNAL_RATE << 16) / drate);
        uint32_t maxout = (uint32_t)(((uint64_t)frames * drate) / AudioMixer::INTERNAL_RATE) + 2;
        if (RING_FRAMES - g_cnt < maxout) return 0;
        uint32_t pos = g_rs_pos;
        while ((pos >> 16) + 1 <= frames) {
            uint32_t i = pos >> 16, fr = pos & 0xFFFF;
            int32_t a0[2], a1[2];
            for (int c = 0; c < 2; c++) {
                a0[c] = (i == 0) ? g_rs_hist[c] : pcm[(i - 1) * 2 + c];
                a1[c] = pcm[i * 2 + c];
                int32_t v = a0[c] + (int32_t)(((int64_t)(a1[c] - a0[c]) * fr) >> 16);
                g_ring[g_wr * 2 + c] = (int16_t)v;
            }
            g_wr = (g_wr + 1) % RING_FRAMES; g_cnt++;
            pos += step;
        }
        g_rs_pos = pos - (frames << 16);
        g_rs_hist[0] = pcm[(frames - 1) * 2]; g_rs_hist[1] = pcm[(frames - 1) * 2 + 1];
    }
    if (g_cnt > g_ring_max) g_ring_max = g_cnt;
    Pump();
    return frames;
}

void UsbAudioBackend::OnEvent(uint64_t ptr, uint8_t cc) {
    g_ev_last_cc = cc;
    if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) g_ev_ok++;
    else if (cc == 14) { g_ev_underrun++; g_need_recover = true; }
    else if (cc == 15) { g_ev_overrun++; g_need_recover = true; }
    else if (cc == 23) { g_ev_missed++; g_need_recover = true; }
    else { g_ev_err++; g_need_recover = true; }
    const USBDeviceRuntime* rt = USB::GidRuntime(g_dev.gid);
    if (rt && rt->iso_ring) {
        uint64_t base = (uint64_t)(uintptr_t)rt->iso_ring;
        if (ptr >= base && ptr < base + 4096ull * 2) {
            uint32_t k = (uint32_t)((ptr - base) / 16);
            uint32_t last = g_last_k;
            uint32_t delta = (k + (USB_ISO_RING_TRBS - 1) - last) % (USB_ISO_RING_TRBS - 1);
            g_compl_total += delta;
            g_last_k = k;
        }
    }
}

void app_str(char* out, int& p, int mx, const char* s) { while (*s && p < mx - 1) out[p++] = *s++; out[p] = 0; }
void app_num(char* out, int& p, int mx, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) { if (p < mx - 1) out[p++] = t[--n]; else --n; }
    out[p] = 0;
}

void UsbAudioBackend::Describe(char* out, int max_len) const {
    int p = 0;
    out[0] = 0;
    if (!g_dev.have) { app_str(out, p, max_len, "usbaudio: no device\n"); return; }
    const UacAlt& a = g_dev.alt;
    app_str(out, p, max_len, "usbaudio: "); app_str(out, p, max_len, g_dev.name);
    app_str(out, p, max_len, "  vid:pid ");
    {
        const char* hx = "0123456789abcdef";
        char t[10]; t[0] = hx[(g_dev.vid >> 12) & 15]; t[1] = hx[(g_dev.vid >> 8) & 15]; t[2] = hx[(g_dev.vid >> 4) & 15]; t[3] = hx[g_dev.vid & 15];
        t[4] = ':'; t[5] = hx[(g_dev.pid >> 12) & 15]; t[6] = hx[(g_dev.pid >> 8) & 15]; t[7] = hx[(g_dev.pid >> 4) & 15]; t[8] = hx[g_dev.pid & 15]; t[9] = 0;
        app_str(out, p, max_len, t);
    }
    app_str(out, p, max_len, "  ctl "); app_num(out, p, max_len, (uint32_t)(g_dev.gid / USB_MAX_DEVICES));
    app_str(out, p, max_len, " dev "); app_num(out, p, max_len, (uint32_t)(g_dev.gid % USB_MAX_DEVICES));
    app_str(out, p, max_len, g_dev.open ? "  STREAMING\n" : "  idle (closed)\n");
    app_str(out, p, max_len, "  uac"); app_num(out, p, max_len, (uint32_t)g_dev.cfg.ver);
    app_str(out, p, max_len, " config "); app_num(out, p, max_len, g_dev.cfg.cfg_value);
    app_str(out, p, max_len, " of "); app_num(out, p, max_len, (uint32_t)g_dev.nconfigs);
    app_str(out, p, max_len, ", iface "); app_num(out, p, max_len, a.iface);
    app_str(out, p, max_len, " alt "); app_num(out, p, max_len, a.alt);
    app_str(out, p, max_len, ", ep "); app_num(out, p, max_len, a.ep_addr);
    app_str(out, p, max_len, ((a.ep_attr >> 2) & 3) == 1 ? " async" : (((a.ep_attr >> 2) & 3) == 2 ? " adaptive" : " sync"));
    app_str(out, p, max_len, ", interval "); app_num(out, p, max_len, g_dev.period_us); app_str(out, p, max_len, " us\n");
    app_str(out, p, max_len, "  format: "); app_num(out, p, max_len, g_dev.rate); app_str(out, p, max_len, " Hz, ");
    app_num(out, p, max_len, a.nch); app_str(out, p, max_len, " ch, "); app_num(out, p, max_len, a.bits);
    app_str(out, p, max_len, " bit (slot "); app_num(out, p, max_len, a.subslot);
    app_str(out, p, max_len, " B), mixer 48000 Hz");
    if (g_dev.rate != 48000) app_str(out, p, max_len, " (resampled)");
    app_str(out, p, max_len, "\n  volume: ");
    app_str(out, p, max_len, g_dev.fu_vol ? "device feature unit" : "none (software only)");
    app_str(out, p, max_len, "\n  tds queued "); app_num(out, p, max_len, g_q_total);
    app_str(out, p, max_len, " completed "); app_num(out, p, max_len, g_compl_total);
    app_str(out, p, max_len, " ok-events "); app_num(out, p, max_len, g_ev_ok);
    app_str(out, p, max_len, " underrun "); app_num(out, p, max_len, g_ev_underrun);
    app_str(out, p, max_len, " overrun "); app_num(out, p, max_len, g_ev_overrun);
    app_str(out, p, max_len, " missed "); app_num(out, p, max_len, g_ev_missed);
    app_str(out, p, max_len, " other "); app_num(out, p, max_len, g_ev_err);
    app_str(out, p, max_len, " resync "); app_num(out, p, max_len, g_resyncs);
    app_str(out, p, max_len, " recover "); app_num(out, p, max_len, g_recoveries);
    app_str(out, p, max_len, "\n");
}

// ---- probe on attach ----
bool read_name(int gid, uint8_t istr, char* out, int cap) {
    out[0] = 0;
    if (!istr) return false;
    uint8_t b[64];
    ua_zero(b, sizeof(b));
    if (!USB::GidControl(gid, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | istr), 0x0409, sizeof(b), b)) return false;
    int n = b[0];
    if (n > (int)sizeof(b)) n = sizeof(b);
    int o = 0;
    for (int i = 2; i + 1 < n && o < cap - 1; i += 2) out[o++] = (b[i + 1] == 0 && b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '?';
    out[o] = 0;
    return o > 0;
}

uint8_t g_cfgbuf[2048];
UacCfg  g_parsed;

void on_attach(int gid, bool attach) {
    if (!attach) {
        if (g_dev.have && gid == g_dev.gid) {
            {
                SpinLockCpuGuard guard(AudioServer::PumpLock());
                g_be.ready_ = false; g_dev.open = false; g_dev.have = false;
            }
            ua_log("[usbaudio] device unplugged (gid %d)\n", gid);
            g_pending_detach = true;
        }
        return;
    }
    if (g_dev.have) { ua_log("[usbaudio] second audio device ignored (gid %d)\n", gid); return; }
    const USBDeviceInfo* info = USB::GidInfo(gid);
    if (!info) return;
    ua_log("[usbaudio] audio interface on gid %d (ctl %d port %d, %x:%x)\n", gid, gid / USB_MAX_DEVICES,
           (int)info->port, (uint32_t)info->vendor_id, (uint32_t)info->product_id);
    uint8_t dd[18];
    ua_zero(dd, sizeof(dd));
    if (!USB::GidControl(gid, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_DEVICE << 8), 0, 18, dd)) {
        ua_log("[usbaudio] device descriptor read failed\n");
        return;
    }
    int ncfg = dd[17];
    if (ncfg < 1) ncfg = 1;
    if (ncfg > 6) ncfg = 6;
    int best_cfg = -1, best_score = -1;
    static UacCfg chosen;
    for (int ci = 0; ci < ncfg; ci++) {
        int len = 0;
        if (!USB::GidGetConfig(gid, ci, g_cfgbuf, sizeof(g_cfgbuf), &len)) {
            ua_log("[usbaudio] config %d: read failed\n", ci);
            continue;
        }
        bool ok = uac_parse(g_cfgbuf, len, g_parsed);
        g_parsed.cfg_index = (uint8_t)ci;
        ua_log("[usbaudio] config %d (value %d, %d bytes): uac%d, %d streaming alts, %d feature units, clock sources %d%s -> %s\n",
               ci, (int)g_parsed.cfg_value, len, g_parsed.ver, g_parsed.nalts, g_parsed.nfu, g_parsed.nclk,
               g_parsed.clk_selector ? " (+selector)" : "", ok ? "usable" : (g_parsed.ver == 3 ? "uac3, unsupported" : "no usable playback alt"));
        for (int i = 0; i < g_parsed.nalts; i++) {
            const UacAlt& a = g_parsed.alts[i];
            ua_log("[usbaudio]   alt if %d/%d: ch %d %d bit (slot %d) %s ep %x attr %x mps %x bInt %d fb %x rates %d%s score %d\n",
                   (int)a.iface, (int)a.alt, (int)a.nch, (int)a.bits, (int)a.subslot, a.pcm ? "pcm" : "non-pcm",
                   (uint32_t)a.ep_addr, (uint32_t)a.ep_attr, (uint32_t)a.wmps, (int)a.b_interval, (uint32_t)a.fb_ep,
                   (int)(a.continuous ? 0 : a.nrates), a.continuous ? " (continuous)" : "", a.score);
        }
        if (ok && g_parsed.best_score > best_score) {
            best_score = g_parsed.best_score; best_cfg = ci;
            chosen = g_parsed;
        }
    }
    if (best_cfg < 0) { ua_log("[usbaudio] no usable playback configuration on this device\n"); return; }

    // the hid/bulk code configured configuration 0 already; switching drops those endpoints (satoru)
    if (!USB::GidSetConfig(gid, chosen.cfg_value)) { ua_log("[usbaudio] SET_CONFIGURATION %d failed\n", (int)chosen.cfg_value); return; }

    g_dev.cfg = chosen;
    g_dev.alt = chosen.alts[chosen.best];
    g_dev.gid = gid;
    g_dev.vid = info->vendor_id; g_dev.pid = info->product_id;
    g_dev.nconfigs = ncfg; g_dev.cfg_chosen = best_cfg;
    g_dev.clock_id = (chosen.ver == 2) ? uac_find_clock(chosen, g_dev.alt) : 0;
    g_dev.open = false;
    g_dev.rate = 0; g_dev.period_us = 0;
    char nm[40];
    if (!read_name(gid, dd[15], nm, sizeof(nm))) { nm[0] = 'U'; nm[1] = 'S'; nm[2] = 'B'; nm[3] = ' '; nm[4] = 'a'; nm[5] = 'u'; nm[6] = 'd'; nm[7] = 'i'; nm[8] = 'o'; nm[9] = 0; }
    int ni = 0; while (nm[ni] && ni < 39) { g_dev.name[ni] = nm[ni]; ni++; } g_dev.name[ni] = 0;
    g_dev.have = true;
    ua_log("[usbaudio] selected config %d, interface %d alt %d, ep %x (%s)\n", chosen.cfg_value,
           (int)g_dev.alt.iface, (int)g_dev.alt.alt, (uint32_t)g_dev.alt.ep_addr, g_dev.name);
    g_pending_attach = true;
}

void on_iso_event(int gid, uint64_t ptr, uint8_t cc, uint32_t residual) {
    (void)residual;
    if (!g_dev.have || gid != g_dev.gid) return;
    g_be.OnEvent(ptr, cc);
}

void on_service() {
    static uint32_t n = 0;
    n++;
    if (g_dev.have && g_dev.open && (n & 31) == 0) {
        uint32_t st = USB::IsoEpState(g_dev.gid);
        if (st == 2 || st == 3 || st == 4) {
            ua_log("[usbaudio] iso endpoint state %d, recovering\n", (int)st);
            USB::IsoRecover(g_dev.gid);
            g_recoveries++;
            g_need_recover = true;
        }
    }
    if (g_pending_attach && g_audio_up) {
        g_pending_attach = false;
        if (!g_user_pref_set && AudioServer::ActiveBackend() != &g_be) {
            ua_log("[usbaudio] device present: switching the mixer to it\n");
            AudioServer::UseBackend("usbaudio");
        }
    }
    if (g_pending_detach) {
        g_pending_detach = false;
        if (AudioServer::ActiveBackend() == &g_be) {
            ua_log("[usbaudio] falling back to another backend\n");
            static const char* const fb[] = { "hda", "ac97", "sb16", "pcspk" };
            for (unsigned i = 0; i < sizeof(fb) / sizeof(fb[0]); i++) if (AudioServer::UseBackend(fb[i])) break;
        }
    }
    if (g_pending_restart) { g_pending_restart = false; }
}

struct Registrar {
    Registrar() { AudioServer::RegisterBackend(&g_be); }
};
Registrar g_registrar;

} // namespace

namespace UsbAudio {

void EarlyInit() { USB::SetAudioHooks(on_iso_event, on_attach, on_service); }
bool Present() { return g_dev.have; }
bool Streaming() { return g_dev.have && g_dev.open; }
void Describe(char* out, int max_len) { g_be.Describe(out, max_len); }

bool Restart() {
    if (!g_dev.have) return false;
    g_be.Close();
    // the controller keeps no state we need to clear; open everything again (satoru)
    bool ok = g_be.Start();
    ua_log("[usbaudio] restart %s\n", ok ? "ok" : "FAILED");
    return ok;
}

// the server calls this after AudioServer::Init so hotplug knows backends exist (satoru)
void NoteAudioUp() { g_audio_up = true; }
void NoteUserChoice() { g_user_pref_set = true; }

// ---- self test: fabricated descriptors ----
namespace {

int put(uint8_t* b, int n, const uint8_t* s, int l) { for (int i = 0; i < l; i++) b[n + i] = s[i]; return n + l; }
#define PUT(arr) n = put(b, n, arr, (int)sizeof(arr))

int build_uac1(uint8_t* b) {
    static const uint8_t cfg[]  = { 9,2,0,0,2,1,0,0x80,50 };
    static const uint8_t ac[]   = { 9,4,0,0,0,1,1,0,0 };
    static const uint8_t hdr[]  = { 9,0x24,1,0,1,0,0,1,1 };
    static const uint8_t it[]   = { 12,0x24,2,1,1,1,0,2,3,0,0,0 };
    static const uint8_t fu[]   = { 10,0x24,6,2,1,1,3,0,0,0 };
    static const uint8_t ot[]   = { 9,0x24,3,3,1,3,0,2,0 };
    static const uint8_t as0[]  = { 9,4,1,0,0,1,2,0,0 };
    static const uint8_t as1[]  = { 9,4,1,1,1,1,2,0,0 };
    static const uint8_t gen[]  = { 7,0x24,1,1,1,1,0 };
    static const uint8_t f16[]  = { 14,0x24,2,1,2,2,16,2,0x44,0xAC,0,0x80,0xBB,0 };
    static const uint8_t ep1[]  = { 9,5,1,9,0xC8,0,1,0,0 };
    static const uint8_t cs[]   = { 7,0x25,1,1,0,0,0 };
    static const uint8_t as2[]  = { 9,4,1,2,1,1,2,0,0 };
    static const uint8_t f24[]  = { 14,0x24,2,1,2,3,24,2,0x44,0xAC,0,0x80,0xBB,0 };
    static const uint8_t ep2[]  = { 9,5,1,9,0x20,1,1,0,0 };
    int n = 0;
    PUT(cfg); PUT(ac); PUT(hdr); PUT(it); PUT(fu); PUT(ot);
    PUT(as0); PUT(as1); PUT(gen); PUT(f16); PUT(ep1); PUT(cs);
    PUT(as2); PUT(gen); PUT(f24); PUT(ep2); PUT(cs);
    b[2] = (uint8_t)n; b[3] = (uint8_t)(n >> 8);
    return n;
}

int build_uac2(uint8_t* b) {
    static const uint8_t cfg[]  = { 9,2,0,0,2,1,0,0x80,50 };
    static const uint8_t iad[]  = { 8,0x0B,0,2,1,0,0x20,0 };
    static const uint8_t ac[]   = { 9,4,0,0,0,1,1,0x20,0 };
    static const uint8_t hdr[]  = { 9,0x24,1,0,2,1,0,0,0 };
    static const uint8_t clk[]  = { 8,0x24,0x0A,4,3,7,0,0 };
    static const uint8_t it[]   = { 17,0x24,2,1,1,1,0,4,2,3,0,0,0,0,0,0,0 };
    static const uint8_t fu[]   = { 18,0x24,6,2,1,0x0F,0,0,0, 0,0,0,0, 0,0,0,0, 0 };
    static const uint8_t ot[]   = { 12,0x24,3,3,1,3,0,2,4,0,0,0 };
    static const uint8_t as0[]  = { 9,4,1,0,0,1,2,0x20,0 };
    static const uint8_t as1[]  = { 9,4,1,1,2,1,2,0x20,0 };
    static const uint8_t gen[]  = { 16,0x24,1,1,0,1,1,0,0,0,2,3,0,0,0,0 };
    static const uint8_t f24[]  = { 6,0x24,2,1,3,24 };
    static const uint8_t ep1[]  = { 7,5,1,5,0x28,1,4 };
    static const uint8_t cs[]   = { 8,0x25,1,0,0,0,0,0 };
    static const uint8_t fb[]   = { 7,5,0x81,0x11,4,0,4 };
    static const uint8_t as2[]  = { 9,4,1,2,2,1,2,0x20,0 };
    static const uint8_t f16[]  = { 6,0x24,2,1,2,16 };
    static const uint8_t ep2[]  = { 7,5,1,5,0xC0,0,4 };
    int n = 0;
    PUT(cfg); PUT(iad); PUT(ac); PUT(hdr); PUT(clk); PUT(it); PUT(fu); PUT(ot);
    PUT(as0); PUT(as1); PUT(gen); PUT(f24); PUT(ep1); PUT(cs); PUT(fb);
    PUT(as2); PUT(gen); PUT(f16); PUT(ep2); PUT(cs); PUT(fb);
    b[2] = (uint8_t)n; b[3] = (uint8_t)(n >> 8);
    return n;
}

int build_uac3(uint8_t* b) {
    static const uint8_t cfg[]  = { 9,2,0,0,2,2,0,0x80,50 };
    static const uint8_t ac[]   = { 9,4,0,0,0,1,1,0x30,0 };
    static const uint8_t as1[]  = { 9,4,1,1,1,1,2,0x30,0 };
    static const uint8_t ep1[]  = { 7,5,1,5,0xC0,0,4 };
    int n = 0;
    PUT(cfg); PUT(ac); PUT(as1); PUT(ep1);
    b[2] = (uint8_t)n; b[3] = (uint8_t)(n >> 8);
    return n;
}

int g_fails = 0;
void expect(bool c, const char* what) {
    ua_log("[uactest] %s: %s\n", what, c ? "ok" : "FAIL");
    if (!c) g_fails++;
}

} // namespace

bool SelfTest() {
    static uint8_t buf[512];
    static UacCfg c1, c2, c3;
    g_fails = 0;
    int l1 = build_uac1(buf);
    bool ok1 = uac_parse(buf, l1, c1);
    expect(ok1 && c1.ver == 1, "uac1 parsed");
    expect(c1.ac_iface == 0 && c1.nalts == 2, "uac1 ac iface 0, two streaming alts");
    if (c1.best >= 0) {
        const UacAlt& a = c1.alts[c1.best];
        expect(a.iface == 1 && a.alt == 1 && a.bits == 16 && a.nch == 2 && a.ep_addr == 1, "uac1 picks 16 bit stereo alt 1");
        expect(alt_has_rate(a, 48000) && alt_has_rate(a, 44100), "uac1 rates 44100 and 48000");
        expect(a.ep_freq_ctl && a.term_link == 1 && ((a.ep_attr >> 2) & 3) == 2, "uac1 freq ctl, term link 1, adaptive");
        const UacFu* fu = uac_find_fu(c1, a);
        expect(fu && fu->id == 2 && fu->mute_m && fu->vol_m, "uac1 feature unit 2 master mute+volume");
    } else expect(false, "uac1 best alt");

    int l2 = build_uac2(buf);
    bool ok2 = uac_parse(buf, l2, c2);
    expect(ok2 && c2.ver == 2, "uac2 parsed");
    expect(c2.nclk == 1 && c2.clk[0].id == 4 && (c2.clk[0].ctl & 3) == 3, "uac2 clock source 4 programmable");
    if (c2.best >= 0) {
        const UacAlt& a = c2.alts[c2.best];
        expect(a.alt == 2 && a.bits == 16 && a.subslot == 2 && a.nch == 2, "uac2 picks 16 bit alt 2 over 24 bit");
        expect(a.fb_ep == 0x81 && a.fb_interval == 4 && ((a.ep_attr >> 2) & 3) == 1, "uac2 async with feedback ep 0x81");
        const UacFu* fu = uac_find_fu(c2, a);
        expect(fu && fu->id == 2 && fu->mute_m && fu->vol_m, "uac2 feature unit 2 master mute+volume");
        expect(uac_find_clock(c2, a) == 4, "uac2 clock id via input terminal");
    } else expect(false, "uac2 best alt");

    int l3 = build_uac3(buf);
    bool ok3 = uac_parse(buf, l3, c3);
    expect(!ok3 && c3.ver == 3 && c3.best < 0, "uac3 recognised and rejected");
    // a device with a uac3 config and a uac2 config must resolve to the uac2 one
    expect(c2.best_score > c3.best_score, "uac2 config outranks uac3 config");
    ua_log("[uactest] %s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails == 0;
}

} // namespace UsbAudio

// end (satoru)
