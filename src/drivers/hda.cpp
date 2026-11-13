//  kurono os - intel hd audio (hda) controller driver implementation
//  pci class 04:03:00 (multimedia audio controller)
#include "hda.h"
#include "../kernel/kimage.h"   // heap direct-map buffers -> phys (satoru)
#include "timer.h"
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"
#include "../kernel/vmm.h"   // map the (possibly high 64-bit) bar0 before deref (satoru)
#include "serial.h"

bool HDAudio::detected = false;
volatile uint8_t* HDAudio::bar0 = nullptr;

void*     HDAudio::corb_raw = nullptr;
void*     HDAudio::rirb_raw = nullptr;
uint32_t* HDAudio::corb = nullptr;
uint64_t* HDAudio::rirb = nullptr;
int HDAudio::corb_size = 0;
int HDAudio::rirb_size = 0;
int HDAudio::rirb_rp = 0;

int HDAudio::codec_count = 0;
uint32_t HDAudio::codec_vendors[HDA_MAX_CODECS] = {};
int HDAudio::output_nid = -1;
int HDAudio::pin_nid = -1;
int HDAudio::codec_addr = 0;

void* HDAudio::bdl_raw = nullptr;
HDA_BDL_Entry* HDAudio::bdl = nullptr;
void* HDAudio::dma_buffer = nullptr;
bool HDAudio::playing = false;
uint8_t HDAudio::volume = 200;
HDAStreamFormat HDAudio::current_format = {48000, 16, 2};
uint32_t HDAudio::stream_base = 0;

// streaming state - independent of the legacy one-shot Play() path.
static bool     g_hda_stream_live  = false;
static uint32_t g_hda_write_ptr    = 0;     // byte offset into cyclic buffer
static uint32_t g_hda_last_lpib    = 0;     // last observed read position

// clamp the link position (lpib)'s forward advance to at most one period (4096
// bytes) per call.  `raw` is the freshly-read SD_LPIB already taken mod buffer
// size by the caller (Read32/stream_base are private statics, so the read stays
// at the call site).  qemu's emulated lpib can lag or jump in big steps; an
// unbounded jump would manufacture huge "free space" and let WriteRing lap the
// read pointer (overwriting samples the dma is about to play = crackle), while a
// backward jump (a near-N advance mod N) would corrupt the free/queued math the
// same way.  clamping the *effective* read position to last+period is uniformly
// conservative: free space can only shrink, queued can only grow, so we never
// over-write on a bogus reading.  updates g_hda_last_lpib to the clamped value
// so successive ticks track smoothly.  period == one bdl entry == 4096 bytes;
// buffer == HDA_BUFFER_SIZE == 128 kb. (satoru)
static uint32_t HDA_ClampLpibAdvance(uint32_t raw) {
    uint32_t adv = (raw + HDA_BUFFER_SIZE - g_hda_last_lpib) % HDA_BUFFER_SIZE;
    const uint32_t kPeriod = 4096;
    uint32_t eff = (adv > kPeriod)
                 ? ((g_hda_last_lpib + kPeriod) % HDA_BUFFER_SIZE)
                 : raw;
    g_hda_last_lpib = eff;
    return eff;
}

uint8_t  HDAudio::Read8(uint32_t offset)  { return *(volatile uint8_t*)(bar0 + offset); }
uint16_t HDAudio::Read16(uint32_t offset) { return *(volatile uint16_t*)(bar0 + offset); }
uint32_t HDAudio::Read32(uint32_t offset) { return *(volatile uint32_t*)(bar0 + offset); }
void HDAudio::Write8(uint32_t offset, uint8_t val)   { *(volatile uint8_t*)(bar0 + offset) = val; }
void HDAudio::Write16(uint32_t offset, uint16_t val) { *(volatile uint16_t*)(bar0 + offset) = val; }
void HDAudio::Write32(uint32_t offset, uint32_t val) { *(volatile uint32_t*)(bar0 + offset) = val; }

// busy-wait `us` microseconds off the hda wall clock (HDA_WALCLK, offset 0x30,
// free-running at 24.0 mhz => 24 ticks per microsecond). a real, calibrated
// delay that survives -O2; the old for(volatile int) loops optimized away so
// codec power-up/settle never actually waited. if the link isn't up yet the
// wall clock reads 0 and never advances, so fall back to a coarse pit-derived
// spin (1us ~ a few thousand pause iterations is unreliable, so we lean on the
// timer for the fallback path). (satoru)
void HDAudio::DelayUs(uint32_t us) {
    if (!bar0) {
        // no mmio yet - approximate with a bounded volatile spin. (satoru)
        for (uint32_t i = 0; i < us; i++)
            for (volatile int d = 0; d < 30; d++) { __asm__ __volatile__("pause"); }
        return;
    }
    uint32_t start = Read32(HDA_WALCLK);
    // probe whether the wall clock is actually ticking. (satoru)
    bool ticking = false;
    for (int i = 0; i < 16; i++) {      // 16 mmio reads is ample at 24 mhz (satoru)
        if (Read32(HDA_WALCLK) != start) { ticking = true; break; }
    }
    if (!ticking) {
        // wall clock dead (link not up) - fall back to a coarse volatile spin
        // so we still wait *something* rather than nothing at all. (satoru)
        for (uint32_t i = 0; i < us; i++)
            for (volatile int d = 0; d < 30; d++) { __asm__ __volatile__("pause"); }
        return;
    }
    const uint32_t target = us * 24u;   // 24 ticks per microsecond. (satoru)
    // iteration cap as well as the clock limit: if the wall clock stalls
    // mid-wait this still terminates (>=24 ticks pass per us on any real read rate). (satoru)
    uint32_t cap = us * 32u + 4096u;
    while ((Read32(HDA_WALCLK) - start) < target && cap--) {
        __asm__ __volatile__("pause");
    }
}

// millisecond wait via the pit timer (accurate and irq/cadence independent
// through Timer::WaitMs). used for the longer analog/charge-pump settles. (satoru)
void HDAudio::DelayMs(uint32_t ms) {
    DelayUs(ms * 1000u);   // wall clock based, no dependency on the pit being alive (satoru)
}

// ---------------------------------------------------------------------------
//  controller bring-up, codec scan and generic output path programming.
//  modelled on linux sound/hda (controllers/intel.c azx_init_pci, core/
//  controller.c snd_hdac_bus_reset_link, common/ generic parser) and logged
//  with an [hda] prefix so one boot log diagnoses real hardware. every wait is
//  iteration capped as well as clock limited. (satoru)
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxW    = 256;   // nids are 8 bit
constexpr int kMaxConn = 16;

struct HdaW {
    uint8_t  present;
    uint8_t  type;
    uint8_t  nconn;
    uint8_t  conn[kMaxConn];
    uint32_t wcap;
    uint32_t pincap;
    uint32_t cfg;
    uint32_t ampo;      // effective output amp caps (0 = none)
    uint32_t ampi;      // effective input amp caps (0 = none)
};
HdaW g_w[kMaxW];

struct HdaCodec {
    int      cad;
    uint32_t vendor, subsys, rev;
    int      afg;
    int      n_dac_a, n_pin_a, n_dac_d, n_pin_d;
};
HdaCodec g_ci[HDA_MAX_CODECS];

struct HdaPath {
    uint8_t nodes[8];   // pin first, dac last
    uint8_t idx[8];     // connection index chosen at each node
    int     n;
    int     pin;
    int     dev;        // default device of the pin
};
HdaPath g_paths[8];
int     g_npaths = 0;
int     g_dacs[8];
int     g_dac_steps[8];
int     g_ndacs = 0;
bool    g_digital = false;
int     g_chosen = -1;

bool g_dead = false;       // bus gave up after repeated verb timeouts
int  g_fail = 0;           // consecutive timeouts
int  g_timeouts = 0;       // total timeouts this bring-up

struct HdaCand {
    uint8_t  bus, dev, fn;
    uint16_t ven, did;
    uint32_t subsys, cls;
    uint64_t bar;
};
HdaCand g_cand;
bool    g_have_cand = false;
uint32_t g_gcap = 0;

void LS(const char* s) { SerialLogger::Log(s); }
void LX(uint32_t v)    { SerialLogger::LogHex(v); }
void LD(int v)         { SerialLogger::LogDec(v); }

uint32_t cfg_r32(int b, int d, int f, int off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    return inl(0xCFC);
}
void cfg_w32(int b, int d, int f, int off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    outl(0xCFC, v);
}
// byte / word config access through the 0xcf8 window (data port + low offset bits), as
// linux pci_read/write_config_byte/word. a dword write to 0x78 would also write back the
// neighbouring DEVSTS half, whose bits are write-1-to-clear. (satoru)
uint16_t cfg_r16(int b, int d, int f, int off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    return inw((uint16_t)(0xCFC + (off & 2)));
}
void cfg_w16(int b, int d, int f, int off, uint16_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    outw((uint16_t)(0xCFC + (off & 2)), v);
}
uint8_t cfg_r8(int b, int d, int f, int off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    return inb((uint16_t)(0xCFC + (off & 3)));
}
void cfg_w8(int b, int d, int f, int off, uint8_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (uint32_t)(off & 0xFC));
    outb((uint16_t)(0xCFC + (off & 3)), v);
}

} // namespace

struct HdaImpl {
    static uint32_t Param(int cad, int nid, int p) {
        uint32_t r = 0;
        if (!HDAudio::SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, p), &r)) return 0;
        return r;
    }
    // 12-bit verb with 8-bit payload (get/set 0xFxx / 0x7xx) (satoru)
    static uint32_t Get(int cad, int nid, uint32_t verb, uint32_t payload) {
        uint32_t r = 0;
        if (!HDAudio::SendVerb(HDA_VERB(cad, nid, verb, payload), &r)) return 0;
        return r;
    }
    static void Set(int cad, int nid, uint32_t verb, uint32_t payload) {
        uint32_t r = 0;
        HDAudio::SendVerb(HDA_VERB(cad, nid, verb, payload), &r);
    }
    // 4-bit verb with 16-bit payload (amp, format, coef) (satoru)
    static uint32_t Get4(int cad, int nid, uint32_t verb4, uint32_t payload) {
        uint32_t r = 0;
        if (!HDAudio::SendVerb(HDA_VERB4(cad, nid, verb4, payload), &r)) return 0;
        return r;
    }
    static void Set4(int cad, int nid, uint32_t verb4, uint32_t payload) {
        uint32_t r = 0;
        HDAudio::SendVerb(HDA_VERB4(cad, nid, verb4, payload), &r);
    }

    // ---- controller discovery ------------------------------------------
    static int FindControllers(HdaCand* out, int max) {
        int n = 0;
        for (int bus = 0; bus < 256; bus++) {
            for (int dev = 0; dev < 32; dev++) {
                // function 0 absent => whole slot absent (skips 7 reads per slot) (satoru)
                if ((cfg_r32(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF) continue;
                for (int fn = 0; fn < 8; fn++) {
                    uint32_t id = cfg_r32(bus, dev, fn, 0);
                    if ((id & 0xFFFF) == 0xFFFF) continue;
                    uint32_t cls = cfg_r32(bus, dev, fn, 8);
                    if (((cls >> 24) & 0xFF) != 0x04 || ((cls >> 16) & 0xFF) != 0x03) continue;
                    uint32_t bar_lo = cfg_r32(bus, dev, fn, 0x10);
                    if (bar_lo & 1) continue;                    // io bar, not hda mmio
                    uint64_t bar = (uint64_t)(bar_lo & ~0xFu);
                    if (((bar_lo >> 1) & 0x3) == 0x2)            // 64-bit bar: high half in bar1
                        bar |= ((uint64_t)cfg_r32(bus, dev, fn, 0x14) << 32);
                    if (n >= max) continue;
                    out[n].bus = bus; out[n].dev = dev; out[n].fn = fn;
                    out[n].ven = id & 0xFFFF; out[n].did = id >> 16;
                    out[n].subsys = cfg_r32(bus, dev, fn, 0x2C);
                    out[n].cls = cls;
                    out[n].bar = bar;
                    n++;
                }
            }
        }
        // intel first, nvidia (gpu hdmi) last: never pick the gpu function over the
        // pch one when both exist. (satoru)
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0; j--) {
                int ra = Rank(out[j - 1].ven), rb = Rank(out[j].ven);
                if (rb < ra) { HdaCand t = out[j - 1]; out[j - 1] = out[j]; out[j] = t; }
                else break;
            }
        }
        return n;
    }
    static int Rank(uint16_t ven) { return ven == 0x8086 ? 0 : (ven == 0x10DE ? 2 : 1); }

    // intel extended multilink capability (hda link power), cf. sound/hda/core/
    // ext/controller.c. only touches link 0 and only when it reports powered off. (satoru)
    static void MlLinkUp() {
        uint32_t off = HDAudio::Read32(0x14) & 0xFFFF;
        for (int i = 0; i < 16 && off >= 0x40 && off < 0xF000 && (off & 3) == 0; i++) {
            uint32_t hdr = HDAudio::Read32(off);
            uint32_t id = (hdr >> 16) & 0xFFF;
            if (id == 2) {
                uint32_t lcap = HDAudio::Read32(off + 0x40);
                uint32_t lctl = HDAudio::Read32(off + 0x44);
                LS("[hda] ml cap @"); LX(off); LS(" links="); LD((int)(HDAudio::Read32(off + 4) & 0x1F) + 1);
                LS(" lcap0="); LX(lcap); LS(" lctl0="); LX(lctl);
                if (!(lctl & (1u << 23))) {
                    HDAudio::Write32(off + 0x44, lctl | (1u << 16));      // SPA
                    for (int k = 0; k < 200; k++) {
                        if (HDAudio::Read32(off + 0x44) & (1u << 23)) break;   // CPA
                        HDAudio::DelayUs(50);
                    }
                    LS(" -> lctl0="); LX(HDAudio::Read32(off + 0x44));
                }
                LS("\r\n");
                return;
            }
            off = hdr & 0xFFFF;
        }
    }

    // reset the link and wait for codecs to announce themselves (statests).
    // timing from snd_hdac_bus_reset_link: >=100us in reset, >=540us after. (satoru)
    static bool ResetLink() {
        for (int attempt = 0; attempt < 2; attempt++) {
            uint32_t gctl = HDAudio::Read32(HDA_GCTL);
            if (gctl & HDA_GCTL_CRST) HDAudio::Write16(HDA_STATESTS, 0x7FFF);
            HDAudio::Write32(HDA_GCTL, gctl & ~(uint32_t)HDA_GCTL_CRST);
            int w0 = 0;
            for (; w0 < 2000; w0++) {
                if (!(HDAudio::Read32(HDA_GCTL) & HDA_GCTL_CRST)) break;
                HDAudio::DelayUs(10);
            }
            HDAudio::DelayUs(600);
            HDAudio::Write32(HDA_GCTL, HDAudio::Read32(HDA_GCTL) | HDA_GCTL_CRST);
            int w1 = 0;
            for (; w1 < 2000; w1++) {
                if (HDAudio::Read32(HDA_GCTL) & HDA_GCTL_CRST) break;
                HDAudio::DelayUs(10);
            }
            HDAudio::DelayUs(1500);
            uint16_t sm = 0;
            int w2 = 0;
            for (; w2 < 120; w2++) {
                sm = HDAudio::Read16(HDA_STATESTS) & 0x7FFF;
                if (sm) break;
                HDAudio::DelayUs(1000);
            }
            LS("[hda] reset try="); LD(attempt); LS(" gctl="); LX(HDAudio::Read32(HDA_GCTL));
            LS(" statests="); LX(sm); LS(" waits(in,out,codec)="); LD(w0); LS(","); LD(w1); LS(","); LD(w2);
            LS("\r\n");
            if (!(HDAudio::Read32(HDA_GCTL) & HDA_GCTL_CRST)) continue;
            if (sm) return true;
        }
        return false;
    }

    // ---- codec scan -----------------------------------------------------
    static int ReadConn(int cad, int nid, HdaW& w) {
        uint32_t cl = Param(cad, nid, HDA_PARAM_CONN_LEN);
        int len = cl & 0x7F;
        bool lng = (cl & 0x80) != 0;
        w.nconn = 0;
        if (len == 0) return 0;
        int per = lng ? 2 : 4, bits = lng ? 16 : 8;
        uint32_t emask = lng ? 0xFFFFu : 0xFFu, rbit = lng ? 0x8000u : 0x80u, idmask = lng ? 0x7FFFu : 0x7Fu;
        int prev = 0, n = 0;
        for (int i = 0; i < len && n < kMaxConn; i += per) {
            uint32_t r = Get(cad, nid, HDA_VERB_GET_CONNLIST, i);
            for (int k = 0; k < per && i + k < len && n < kMaxConn; k++) {
                uint32_t e = (r >> (k * bits)) & emask;
                int id = (int)(e & idmask);
                if ((e & rbit) && prev) {
                    for (int m = prev + 1; m <= id && n < kMaxConn; m++) w.conn[n++] = (uint8_t)m;
                } else {
                    w.conn[n++] = (uint8_t)id;
                }
                prev = id;
            }
        }
        w.nconn = (uint8_t)n;
        return len;
    }

    static void Power(int cad, int nid, bool wait) {
        Set(cad, nid, HDA_VERB_SET_POWER, 0x00);
        if (!wait) return;
        for (int i = 0; i < 20; i++) {
            uint32_t r = Get(cad, nid, 0xF05, 0);
            if (((r >> 4) & 0xF) == 0) break;      // PS-Act == D0
            HDAudio::DelayUs(500);
        }
    }

    static const char* TypeName(int t) {
        switch (t) {
            case 0: return "DAC"; case 1: return "ADC"; case 2: return "MIX"; case 3: return "SEL";
            case 4: return "PIN"; case 5: return "PWR"; case 6: return "VOL"; case 7: return "BEEP";
            default: return "VEN";
        }
    }

    // scan one codec's audio function group into g_w. returns false if no afg. (satoru)
    static bool Scan(int cad, HdaCodec& ci, bool verbose) {
        for (int i = 0; i < kMaxW; i++) g_w[i].present = 0;
        ci.cad = cad;
        ci.vendor = Param(cad, 0, HDA_PARAM_VENDOR_ID);
        ci.rev = Param(cad, 0, 0x02);
        ci.afg = HDAudio::FindAFG(cad);
        ci.subsys = 0; ci.n_dac_a = ci.n_pin_a = ci.n_dac_d = ci.n_pin_d = 0;
        if (verbose) {
            LS("[hda] codec cad="); LD(cad); LS(" vendor="); LX(ci.vendor); LS(" rev="); LX(ci.rev);
        }
        if (ci.afg < 0) { if (verbose) LS(" no-audio-fg\r\n"); return false; }
        Power(cad, ci.afg, true);
        ci.subsys = Get(cad, ci.afg, 0xF20, 0);
        uint32_t nc = Param(cad, ci.afg, HDA_PARAM_NODE_COUNT);
        int start = (nc >> 16) & 0xFF, count = nc & 0xFF;
        uint32_t afg_ao = Param(cad, ci.afg, HDA_PARAM_AMP_OUT_CAP);
        uint32_t afg_ai = Param(cad, ci.afg, HDA_PARAM_AMP_IN_CAP);
        if (verbose) {
            LS(" subsys="); LX(ci.subsys); LS(" afg="); LX(ci.afg); LS(" nodes="); LX(start); LS("+"); LD(count);
            LS(" pcm="); LX(Param(cad, ci.afg, 0x0A)); LS(" fmts="); LX(Param(cad, ci.afg, 0x0B));
            LS(" afg_ao="); LX(afg_ao); LS(" afg_ai="); LX(afg_ai); LS("\r\n");
        }
        for (int n = 0; n < count; n++) {
            int nid = start + n;
            if (nid >= kMaxW) break;
            HdaW& w = g_w[nid];
            w.wcap = Param(cad, nid, HDA_PARAM_AUDIO_WIDGET);
            w.type = (w.wcap >> 20) & 0xF;
            w.present = 1;
            w.pincap = 0; w.cfg = 0; w.nconn = 0;
            bool digital = (w.wcap & 0x200) != 0;
            w.ampo = (w.wcap & 4) ? ((w.wcap & 8) ? Param(cad, nid, HDA_PARAM_AMP_OUT_CAP) : afg_ao) : 0;
            w.ampi = (w.wcap & 2) ? ((w.wcap & 8) ? Param(cad, nid, HDA_PARAM_AMP_IN_CAP) : afg_ai) : 0;
            int clen = 0;
            if (w.wcap & 0x100) clen = ReadConn(cad, nid, w);
            if (w.type == HDA_WIDGET_PIN) {
                w.pincap = Param(cad, nid, HDA_PARAM_PIN_CAP);
                w.cfg = Get(cad, nid, HDA_VERB_GET_CONFIG_DEF, 0);
                if (w.pincap & 0x10) {
                    int conn = (w.cfg >> 30) & 3, dev = (w.cfg >> 20) & 0xF;
                    if (conn != 1) {
                        if (dev <= 2) ci.n_pin_a++;
                        else if (dev == 4 || dev == 5) ci.n_pin_d++;
                    }
                }
            } else if (w.type == HDA_WIDGET_AUDIO_OUT) {
                if (digital) ci.n_dac_d++; else ci.n_dac_a++;
            }
            if (verbose) {
                LS("[hda]  n"); LX(nid); LS(" "); LS(TypeName(w.type)); LS(" wcap="); LX(w.wcap);
                if (w.type == HDA_WIDGET_PIN) { LS(" pc="); LX(w.pincap); LS(" cfg="); LX(w.cfg); }
                if (w.ampo) { LS(" ao="); LX(w.ampo); }
                if (w.ampi) { LS(" ai="); LX(w.ampi); }
                if (clen) {
                    LS(" cn="); LD(clen); LS(":");
                    for (int k = 0; k < w.nconn; k++) { if (k) LS(","); LX(w.conn[k]); }
                }
                LS("\r\n");
            }
        }
        return true;
    }

    // ---- generic output path (cf. sound/hda/common/auto_parser + generic) ---
    static bool Walk(int nid, int depth, bool digital, bool avoid_used, HdaPath& p) {
        HdaW& w = g_w[nid];
        p.nodes[depth] = (uint8_t)nid;
        p.n = depth + 1;
        if (w.type == HDA_WIDGET_AUDIO_OUT) {
            if (((w.wcap & 0x200) != 0) != digital) return false;
            if (avoid_used) {
                for (int i = 0; i < g_ndacs; i++) if (g_dacs[i] == nid) return false;
            }
            return true;
        }
        if (depth >= 6) return false;
        if (w.type != HDA_WIDGET_PIN && w.type != HDA_WIDGET_AUDIO_MIX && w.type != HDA_WIDGET_AUDIO_SEL)
            return false;
        for (int i = 0; i < w.nconn; i++) {
            int c = w.conn[i];
            if (!g_w[c].present || g_w[c].type == HDA_WIDGET_PIN) continue;
            p.idx[depth] = (uint8_t)i;
            if (Walk(c, depth + 1, digital, avoid_used, p)) return true;
        }
        return false;
    }

    static uint32_t AmpPayload(bool out, int idx, uint32_t cap, bool mute) {
        uint32_t ofs = cap & 0x7F, steps = (cap >> 8) & 0x7F;
        uint32_t g = ofs;                      // 0 dB step
        if (g > steps) g = steps;
        uint32_t v = (out ? 0x8000u : 0x4000u) | 0x3000u;   // set out|in, left+right
        if (!out) v |= ((uint32_t)idx & 0xF) << 8;
        if (mute) v |= 0x80;
        return v | g;
    }
    static uint32_t AmpRead(int cad, int nid, bool out, int idx) {
        return Get4(cad, nid, 0xB, (out ? 0xA000u : 0x2000u) | (out ? 0u : ((uint32_t)idx & 0xF)));
    }

    static int PinSense(int cad, int nid, uint32_t pincap) {
        if (!(pincap & 0x4)) return -1;               // no presence detect
        if (pincap & 0x2) { Set(cad, nid, 0x709, 0); HDAudio::DelayUs(1000); }   // trigger required
        return (int)((Get(cad, nid, 0xF09, 0) >> 31) & 1);
    }

    // coefficient helpers: only used for the documented alc287 headphone sequence (satoru)
    static uint32_t CoefRead(int cad, int nid, int idx) {
        Set4(cad, nid, 0x5, idx);
        return Get4(cad, nid, 0xC, 0);
    }
    static void CoefUpdate(int cad, int nid, int idx, uint32_t mask, uint32_t bits) {
        uint32_t v = CoefRead(cad, nid, idx);
        Set4(cad, nid, 0x5, idx);
        Set4(cad, nid, 0x4, (v & ~mask) | bits);
    }

    static bool BuildPaths(const HdaCodec& ci, bool digital, bool verbose) {
        int cad = ci.cad;
        g_npaths = 0; g_ndacs = 0; g_digital = digital;
        for (int nid = 0; nid < kMaxW && g_npaths < 8; nid++) {
            HdaW& w = g_w[nid];
            if (!w.present || w.type != HDA_WIDGET_PIN || !(w.pincap & 0x10)) continue;
            int conn = (w.cfg >> 30) & 3, dev = (w.cfg >> 20) & 0xF;
            if (conn == 1) continue;                        // no physical connection
            bool ok = digital ? (dev == 4 || dev == 5) : (dev <= 2);
            if (!ok) continue;
            HdaPath p; p.pin = nid; p.dev = dev; p.n = 0;
            bool found = Walk(nid, 0, digital, true, p);
            if (!found) found = Walk(nid, 0, digital, false, p);
            if (!found) {
                if (verbose) { LS("[hda] no dac path for pin "); LX(nid); LS("\r\n"); }
                continue;
            }
            g_paths[g_npaths++] = p;
            int dac = p.nodes[p.n - 1];
            bool have = false;
            for (int i = 0; i < g_ndacs; i++) if (g_dacs[i] == dac) have = true;
            if (!have && g_ndacs < 8) { g_dac_steps[g_ndacs] = (g_w[dac].ampo >> 8) & 0x7F; g_dacs[g_ndacs++] = dac; }
        }
        if (g_npaths == 0) return false;

        // program every path: power, connection select, amps, pin control, eapd. (satoru)
        for (int pi = 0; pi < g_npaths; pi++) {
            HdaPath& p = g_paths[pi];
            for (int k = 0; k < p.n; k++) {
                int nid = p.nodes[k];
                HdaW& w = g_w[nid];
                Power(cad, nid, (w.wcap & 0x400) != 0);
                bool is_dac = (w.type == HDA_WIDGET_AUDIO_OUT);
                if (!is_dac && w.nconn > 1 && (w.type == HDA_WIDGET_PIN || w.type == HDA_WIDGET_AUDIO_SEL))
                    Set(cad, nid, HDA_VERB_SET_CONNSEL, p.idx[k]);
                if (!is_dac && w.ampi) Set4(cad, nid, 0x3, AmpPayload(false, p.idx[k], w.ampi, false));
                if (w.ampo)            Set4(cad, nid, 0x3, AmpPayload(true, 0, w.ampo, false));
            }
            HdaW& pw = g_w[p.pin];
            uint32_t pctl = 0x40;                                       // OUT_EN
            if (p.dev == 2 && (pw.pincap & 0x8)) pctl |= 0x80;          // HP_EN where the pin has an hp amp
            Set(cad, p.pin, HDA_VERB_SET_PINCTL, pctl);
            if (pw.pincap & 0x10000) Set(cad, p.pin, HDA_VERB_SET_EAPDBTL, 0x02);
        }
        HDAudio::DelayUs(2000);

        if (verbose) {
            for (int pi = 0; pi < g_npaths; pi++) {
                HdaPath& p = g_paths[pi];
                HdaW& pw = g_w[p.pin];
                LS("[hda] path pin="); LX(p.pin); LS(" dev="); LD(p.dev); LS(" sense="); LD(PinSense(cad, p.pin, pw.pincap));
                LS(" chain=");
                for (int k = 0; k < p.n; k++) { if (k) LS(">"); LX(p.nodes[k]); }
                LS(" pinctl="); LX(Get(cad, p.pin, 0xF07, 0));
                LS(" eapd="); LX(Get(cad, p.pin, 0xF0C, 0));
                LS(" csel="); LX(Get(cad, p.pin, HDA_VERB_GET_CONNSEL, 0));
                LS(" pwr="); LX(Get(cad, p.pin, 0xF05, 0));
                for (int k = 0; k < p.n; k++) {
                    HdaW& w = g_w[p.nodes[k]];
                    if (w.ampo) { LS(" ao"); LX(p.nodes[k]); LS("="); LX(AmpRead(cad, p.nodes[k], true, 0)); }
                    if (w.ampi && w.type != HDA_WIDGET_AUDIO_OUT) { LS(" ai"); LX(p.nodes[k]); LS("="); LX(AmpRead(cad, p.nodes[k], false, p.idx[k])); }
                }
                LS("\r\n");
            }
        }
        return true;
    }

    // documented alc287 headphone power sequence, from alc225_init() in
    // sound/hda/codecs/realtek/alc269.c (alc287 skips alc285_hp_init). only runs
    // when a headphone is actually sensed on the jack. coefs 0x57/4 and 0x20/0x4a
    // are the same ones linux touches there; nothing else is poked. (satoru)
    static void Alc287Hp(int cad) {
        for (int pi = 0; pi < g_npaths; pi++) {
            HdaPath& p = g_paths[pi];
            if (p.dev != 2) continue;
            if (PinSense(cad, p.pin, g_w[p.pin].pincap) != 1) continue;
            LS("[hda] alc287 hp plugged on pin "); LX(p.pin); LS(": running alc225_init hp sequence\r\n");
            CoefUpdate(cad, 0x57, 0x04, 0x0007, 0x1);          // low power
            Set(cad, p.pin, HDA_VERB_SET_PINCTL, 0x40);
            HDAudio::DelayUs(75000);
            Set4(cad, p.pin, 0x3, 0xB000);                      // AMP_OUT_UNMUTE
            HDAudio::DelayUs(75000);
            CoefUpdate(cad, 0x20, 0x4a, 3u << 10, 0);
            CoefUpdate(cad, 0x57, 0x04, 0x0007, 0x4);          // high power
            return;
        }
    }

    // ---- one controller, start to finish ---------------------------------
    static bool BringUp(const HdaCand& c) {
        HDAudio::detected = false;
        HDAudio::codec_count = 0;
        HDAudio::output_nid = -1; HDAudio::pin_nid = -1;
        g_dead = false; g_fail = 0; g_timeouts = 0; g_chosen = -1; g_npaths = 0; g_ndacs = 0;
        g_cand = c; g_have_cand = true;
        HDAudio::bar0 = nullptr;

        LS("[hda] controller "); LD(c.bus); LS(":"); LD(c.dev); LS("."); LD(c.fn);
        LS(" ven="); LX(c.ven); LS(" dev="); LX(c.did); LS(" subsys="); LX(c.subsys);
        LS(" class="); LX(c.cls >> 8); LS(" bar="); SerialLogger::LogHex64(c.bar); LS("\r\n");
        if (c.bar == 0) { LS("[hda] FAIL bar unassigned\r\n"); return false; }

        uint64_t start = c.bar & ~0xFFFULL;
        for (uint64_t p = start; p < start + 0x10000ULL; p += 0x1000ULL)
            KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
        HDAudio::bar0 = (volatile uint8_t*)(uintptr_t)c.bar;

        // memory space + bus master; mask to the low 16 bits so the status
        // half (write-1-to-clear) is not written back. (satoru)
        uint32_t cmd = cfg_r32(c.bus, c.dev, c.fn, 0x04);
        cfg_w32(c.bus, c.dev, c.fn, 0x04, (cmd & 0xFFFF) | 0x6);
        if (c.ven == 0x8086) {
            // azx_init_pci: clear TCSEL bits 0-2 (pci 0x44) and enable snooping
            // (clear NSNPEN, devc bit 11 at 0x78). (satoru)
            // same widths as linux azx_init_pci: a byte read-modify-write at 0x44 (TCSEL) and a
            // 16-bit one at 0x78 (DEVC), so the neighbouring config bytes (and the write-1-to-
            // clear DEVSTS half at 0x7a) are never written back. (satoru)
            uint8_t t = cfg_r8(c.bus, c.dev, c.fn, 0x44);
            cfg_w8(c.bus, c.dev, c.fn, 0x44, (uint8_t)(t & ~0x7u));
            uint16_t devc = cfg_r16(c.bus, c.dev, c.fn, 0x78);
            LS("[hda] pci cmd="); LX(cmd & 0xFFFF); LS(" tcsel="); LX(t); LS(" devc="); LX(devc);
            if (devc & (1u << 11)) { cfg_w16(c.bus, c.dev, c.fn, 0x78, (uint16_t)(devc & ~(1u << 11))); LS(" -> snoop enabled"); }
            LS("\r\n");
        }

        g_gcap = HDAudio::Read16(HDA_GCAP);
        LS("[hda] gcap="); LX(g_gcap); LS(" vmaj.vmin="); LD(HDAudio::Read8(HDA_VMAJ)); LS("."); LD(HDAudio::Read8(HDA_VMIN));
        LS(" oss="); LD((g_gcap >> 12) & 0xF); LS(" iss="); LD((g_gcap >> 8) & 0xF);
        LS(" 64ok="); LD(g_gcap & 1); LS(" gctl(pre)="); LX(HDAudio::Read32(HDA_GCTL));
        LS(" statests(pre)="); LX(HDAudio::Read16(HDA_STATESTS)); LS("\r\n");
        if (g_gcap == 0xFFFF || g_gcap == 0) { LS("[hda] FAIL mmio reads dead\r\n"); return false; }

        MlLinkUp();
        if (!ResetLink()) { LS("[hda] FAIL no codec after link reset\r\n"); return false; }

        g_gcap = HDAudio::Read16(HDA_GCAP);
        int num_oss = (g_gcap >> 12) & 0xF, num_iss = (g_gcap >> 8) & 0xF;
        if (num_oss == 0) { LS("[hda] FAIL no output streams\r\n"); return false; }
        HDAudio::stream_base = 0x80 + num_iss * 0x20;

        if (!HDAudio::InitCorbRirb()) { LS("[hda] FAIL corb/rirb\r\n"); return false; }

        // scan every codec on the link and prefer one with an analog function
        // group over hdmi. (satoru)
        uint16_t sm = HDAudio::Read16(HDA_STATESTS) & 0x7FFF;
        int nco = 0;
        for (int cad = 0; cad < 15 && nco < HDA_MAX_CODECS && !g_dead; cad++) {
            if (!(sm & (1 << cad))) continue;
            HdaCodec& ci = g_ci[nco];
            Scan(cad, ci, true);
            HDAudio::codec_vendors[nco] = ci.vendor;
            nco++;
        }
        HDAudio::codec_count = nco;
        if (nco == 0 || g_dead) { LS("[hda] FAIL no codec answered\r\n"); return false; }

        bool digital = false;
        for (int i = 0; i < nco && g_chosen < 0; i++)
            if (g_ci[i].afg >= 0 && g_ci[i].n_dac_a > 0 && g_ci[i].n_pin_a > 0) g_chosen = i;
        if (g_chosen < 0) {
            for (int i = 0; i < nco && g_chosen < 0; i++)
                if (g_ci[i].afg >= 0 && g_ci[i].n_dac_d > 0 && g_ci[i].n_pin_d > 0) { g_chosen = i; digital = true; }
        }
        if (g_chosen < 0) { LS("[hda] FAIL no codec has a usable output function\r\n"); return false; }
        HdaCodec ci = g_ci[g_chosen];
        LS("[hda] chosen codec cad="); LD(ci.cad); LS(digital ? " (digital/hdmi fallback)" : " (analog)"); LS("\r\n");

        // rescan quietly to restore the widget table of the chosen codec, then
        // program the paths. (satoru)
        Scan(ci.cad, ci, false);
        HDAudio::codec_addr = ci.cad;
        if (!BuildPaths(ci, digital, true)) { LS("[hda] FAIL no output pin reaches a dac\r\n"); return false; }
        HDAudio::output_nid = g_dacs[0];
        HDAudio::pin_nid = g_paths[0].pin;
        if (ci.vendor == 0x10EC0287) {
            Alc287Hp(ci.cad);
            if (ci.subsys == 0x17AA3906) {
                LS("[hda] NOTE: ALC287 subsys 17aa3906 - the internal speakers are driven by an external I2C smart amp (Awinic); "
                   "no HDA verb can enable it, so only the headphone jack is expected to sound. speaker pins are still driven in case the amp is enabled.\r\n");
            }
        }
        if (g_timeouts) { LS("[hda] verb timeouts during bring-up: "); LD(g_timeouts); LS("\r\n"); }

        // dma structures (allocated once; restart reuses them) and the 64-bit
        // check: without 64ok the controller cannot reach heap above 4 GB. (satoru)
        if (!HDAudio::bdl_raw) {
            HDAudio::bdl_raw = KernelHeap::Alloc(4096 + 128);
            if (!HDAudio::bdl_raw) return false;
            HDAudio::bdl = (HDA_BDL_Entry*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)HDAudio::bdl_raw, 128);
        }
        if (!HDAudio::dma_buffer) {
            int pages_needed = (HDA_BUFFER_SIZE + 4095) / 4096;
            void* dma_raw = (void*)KernelHeap::Alloc(pages_needed * 4096 + 128);
            if (!dma_raw) return false;
            HDAudio::dma_buffer = (void*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)dma_raw, 128);
        }
        uint64_t top = (uint64_t)(uintptr_t)HDAudio::dma_buffer + HDA_BUFFER_SIZE;
        LS("[hda] dma buf="); SerialLogger::LogHex64((uint64_t)(uintptr_t)HDAudio::dma_buffer);
        LS(" bdl="); SerialLogger::LogHex64((uint64_t)(uintptr_t)HDAudio::bdl);
        LS(" corb="); SerialLogger::LogHex64((uint64_t)(uintptr_t)HDAudio::corb);
        LS(" rirb="); SerialLogger::LogHex64((uint64_t)(uintptr_t)HDAudio::rirb); LS("\r\n");
        if (!(g_gcap & 1) && (top >> 32)) {
            LS("[hda] FAIL controller lacks 64-bit dma but buffers sit above 4 GB\r\n");
            return false;
        }
        uint8_t* buf_ptr = (uint8_t*)HDAudio::dma_buffer;
        for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
            HDAudio::bdl[i].address = KernelPhysOf(buf_ptr + i * 4096);
            HDAudio::bdl[i].length = 4096;
            HDAudio::bdl[i].ioc = (i == HDA_BDL_ENTRIES - 1) ? 1 : 0;
        }
        HDAudio::SetupOutputStream();
        HDAudio::detected = true;
        return true;
    }
};

bool HDAudio::Init() {
    detected = false;
    codec_count = 0;
    output_nid = -1;
    pin_nid = -1;

    HdaCand cands[4];
    int n = HdaImpl::FindControllers(cands, 4);
    LS("[hda] found "); LD(n); LS(" hd audio controller(s)\r\n");
    for (int i = 0; i < n; i++) {
        if (HdaImpl::BringUp(cands[i])) return true;
        LS("[hda] controller failed, trying next\r\n");
    }
    return false;
}

// re-initialise the controller and codecs without a reboot ("audio restart"). the
// backend object stays registered; stream_live is dropped first so the mixer's
// WriteRing calls return 0 while the link is down. (satoru)
bool HDAudio::Restart() {
    if (!g_have_cand) return false;
    LS("[hda] restart requested\r\n");
    if (detected) Stop();
    detected = false;
    if (!HdaImpl::BringUp(g_cand)) { LS("[hda] restart FAILED\r\n"); return false; }
    if (!SetFormat(current_format.sample_rate, current_format.bits, current_format.channels)) return false;
    if (!StartStream()) { LS("[hda] restart FAILED (stream)\r\n"); return false; }
    SetVolume(volume);
    LS("[hda] restart ok\r\n");
    return true;
}

bool HDAudio::InitCorbRirb() {
    // sizes: pick the largest supported ring (spec sizecap bits 6/5/4) (satoru)
    uint8_t corbsz = Read8(HDA_CORBSIZE);
    if (corbsz & 0x40) { corb_size = 256; Write8(HDA_CORBSIZE, 0x02); }
    else if (corbsz & 0x20) { corb_size = 16; Write8(HDA_CORBSIZE, 0x01); }
    else { corb_size = 2; Write8(HDA_CORBSIZE, 0x00); }

    uint8_t rirbsz = Read8(HDA_RIRBSIZE);
    if (rirbsz & 0x40) { rirb_size = 256; Write8(HDA_RIRBSIZE, 0x02); }
    else if (rirbsz & 0x20) { rirb_size = 16; Write8(HDA_RIRBSIZE, 0x01); }
    else { rirb_size = 2; Write8(HDA_RIRBSIZE, 0x00); }

    // 128-byte aligned rings; allocated once, reused on restart. (satoru)
    if (!corb_raw) {
        corb_raw = KernelHeap::Alloc(4096 + 128);
        if (!corb_raw) return false;
        corb = (uint32_t*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)corb_raw, 128);
    }
    if (!rirb_raw) {
        rirb_raw = KernelHeap::Alloc(4096 + 128);
        if (!rirb_raw) return false;
        rirb = (uint64_t*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)rirb_raw, 128);
    }
    for (int i = 0; i < corb_size; i++) ((volatile uint32_t*)corb)[i] = 0;
    for (int i = 0; i < rirb_size; i++) ((volatile uint64_t*)rirb)[i] = 0;

    Write8(HDA_CORBCTL, 0);
    Write8(HDA_RIRBCTL, 0);
    DelayUs(100);

    // full 64-bit bases (heap can sit above 4 GB) (satoru)
    uint64_t corb_phys = KernelPhysOf(corb);
    Write32(HDA_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    Write32(HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));

    // corb read pointer reset: set bit 15, wait for it to read 1, clear, wait for 0
    Write16(HDA_CORBRP, (1 << 15));
    for (int i = 0; i < 1000; i++) {
        if (Read16(HDA_CORBRP) & (1 << 15)) break;
        DelayUs(10);
    }
    Write16(HDA_CORBRP, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(Read16(HDA_CORBRP) & (1 << 15))) break;
        DelayUs(10);
    }
    Write16(HDA_CORBWP, 0);

    uint64_t rirb_phys = KernelPhysOf(rirb);
    Write32(HDA_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    Write32(HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    Write16(HDA_RIRBWP, (1 << 15));
    DelayUs(10);
    rirb_rp = Read16(HDA_RIRBWP) & 0xFF;

    Write8(HDA_RIRBSTS, (1 << 0) | (1 << 2));
    Write8(HDA_CORBSTS, 0x01);
    Write16(HDA_RINTCNT, 1);

    Write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    Write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN | HDA_RIRBCTL_INT);
    DelayUs(50);
    return true;
}

bool HDAudio::SendVerb(uint32_t verb, uint32_t* response) {
    if (g_dead || !bar0 || !corb || !rirb) return false;
    // resync our read pointer so only this verb's response is returned (satoru)
    rirb_rp = Read16(HDA_RIRBWP) & 0xFF;

    uint16_t wp = Read16(HDA_CORBWP) & 0xFF;
    wp = (wp + 1) % corb_size;
    ((volatile uint32_t*)corb)[wp] = verb;
    __asm__ __volatile__("mfence" ::: "memory");
    Write16(HDA_CORBWP, wp);

    if (WaitRIRB(response, 0)) { g_fail = 0; return true; }
    g_timeouts++;
    g_fail++;
    if (g_timeouts <= 6) { LS("[hda] verb timeout "); LX(verb); LS("\r\n"); }
    if (g_fail >= 6) {
        g_dead = true;
        LS("[hda] codec link not responding - giving up on verbs\r\n");
    }
    return false;
}

bool HDAudio::WaitRIRB(uint32_t* response, int timeout) {
    (void)timeout;
    // bounded by iteration count (3000 x ~3us ~= 9 ms) so a dead link can never
    // hang the caller, whether or not any clock is ticking. (satoru)
    for (int i = 0; i < 3000; i++) {
        uint16_t wp = Read16(HDA_RIRBWP) & 0xFF;
        while (rirb_rp != wp) {
            int next = (rirb_rp + 1) % rirb_size;
            rirb_rp = next;
            __asm__ __volatile__("" ::: "memory");
            uint64_t entry = ((volatile uint64_t*)rirb)[next];
            Write8(HDA_RIRBSTS, (1 << 0) | (1 << 2));
            if ((entry >> 32) & 0x10) continue;          // unsolicited, not ours
            if (response) *response = (uint32_t)(entry & 0xFFFFFFFFu);
            return true;
        }
        DelayUs(3);
    }
    return false;
}

// locate the audio function group node on codec `cad`. (satoru)
int HDAudio::FindAFG(int cad) {
    uint32_t resp = 0;
    SendVerb(HDA_VERB(cad, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT), &resp);
    int start = (resp >> 16) & 0xFF;
    int count = resp & 0xFF;
    for (int i = 0; i < count && i < HDA_MAX_NODES; i++) {
        int fg = start + i;
        uint32_t fgt = 0;
        SendVerb(HDA_VERB(cad, fg, HDA_VERB_GET_PARAM, HDA_PARAM_FN_GROUP), &fgt);
        if ((fgt & 0xFF) == 0x01) return fg;   // 0x01 = audio function group
    }
    return -1;
}

uint16_t HDAudio::EncodeFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    uint16_t fmt = 0;

    // sample rate base and multiplier
    switch (sample_rate) {
        case 8000:  fmt |= (0x05 << 8); break; // 48000 / 6
        case 11025: fmt |= (0x03 << 8) | (1 << 14); break; // 44100 / 4
        case 16000: fmt |= (0x02 << 8); break; // 48000 / 3
        case 22050: fmt |= (0x01 << 8) | (1 << 14); break; // 44100 / 2
        case 32000: fmt |= (0x01 << 11) | (0x02 << 8); break; // 48000 * 2/3: mult field 1 (x2), div field 2 (/3) (satoru)
        case 44100: fmt |= (1 << 14); break; // 44100 base
        case 48000: fmt |= 0; break; // 48000 base
        case 88200: fmt |= (1 << 14) | (1 << 11); break; // 44100 * 2
        case 96000: fmt |= (1 << 11); break; // 48000 * 2
        case 176400: fmt |= (1 << 14) | (3 << 11); break; // 44100 * 4
        case 192000: fmt |= (3 << 11); break; // 48000 * 4
        default:    fmt |= 0; break; // default 48000
    }

    // bits per sample
    switch (bits) {
        case 8:  fmt |= (0 << 4); break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break; // default 16-bit
    }

    // channels (0 = 1ch, 1 = 2ch, ...)
    fmt |= (channels - 1) & 0xF;

    return fmt;
}

bool HDAudio::SetupOutputStream() {
    if (!bdl || !dma_buffer) return false;

    // stream reset: assert SRST, wait for it to read back 1, clear, wait for 0
    Write8(stream_base + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 1000; i++) {
        if (Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        DelayUs(10);
    }
    Write8(stream_base + HDA_SD_CTL, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        DelayUs(10);
    }

    uint16_t fmt = EncodeFormat(current_format.sample_rate, current_format.bits, current_format.channels);
    Write16(stream_base + HDA_SD_FMT, fmt);

    uint64_t bdl_phys = KernelPhysOf(bdl);
    Write32(stream_base + HDA_SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    Write32(stream_base + HDA_SD_BDPU, (uint32_t)(bdl_phys >> 32));
    Write32(stream_base + HDA_SD_CBL, HDA_BUFFER_SIZE);
    Write16(stream_base + HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

    // stream tag 1 in ctl bits [23:20]; write only the upper control byte so the
    // sts byte (write-1-to-clear) is untouched. (satoru)
    Write8(stream_base + HDA_SD_CTL + 2, (uint8_t)(1 << 4));
    Write8(stream_base + HDA_SD_STS, 0x1C);           // clear stale fifoe/dese/bcis

    // every dac that a path uses listens to stream 1 channel 0 with this format,
    // so all output pins (hp, speaker, line) play the same signal. (satoru)
    for (int i = 0; i < g_ndacs; i++) {
        uint32_t resp;
        SendVerb(HDA_VERB(codec_addr, g_dacs[i], HDA_VERB_SET_CONVCTRL, (1 << 4) | 0), &resp);
        SendVerb(HDA_VERB(codec_addr, g_dacs[i], HDA_VERB_SET_FORMAT, fmt), &resp);
        if (g_digital) SendVerb(HDA_VERB(codec_addr, g_dacs[i], 0x70D, 0x01), &resp);   // digital converter enable
    }
    LS("[hda] stream fmt="); LX(fmt); LS(" sd_base="); LX(stream_base);
    for (int i = 0; i < g_ndacs && i < 4; i++) {
        uint32_t f = 0, s = 0;
        SendVerb(HDA_VERB4(codec_addr, g_dacs[i], 0xA, 0), &f);
        SendVerb(HDA_VERB(codec_addr, g_dacs[i], 0xF06, 0), &s);
        LS(" dac"); LX(g_dacs[i]); LS(" fmt="); LX(f); LS(" strm="); LX(s);
    }
    LS("\r\n");
    return true;
}

bool HDAudio::IsDetected() { return detected; }
int HDAudio::GetCodecCount() { return codec_count; }

uint32_t HDAudio::GetCodecVendor(int codec) {
    if (codec < 0 || codec >= codec_count) return 0;
    return codec_vendors[codec];
}

bool HDAudio::SetFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    if (!detected) return false;
    if (playing) Stop();

    current_format.sample_rate = sample_rate;
    current_format.bits = bits;
    current_format.channels = channels;

    return SetupOutputStream();
}

bool HDAudio::Play(const void* pcm_data, uint32_t size) {
    if (!detected || !dma_buffer) return false;
    if (playing) Stop();

    // copy pcm data into dma buffer
    uint32_t copy_size = size;
    if (copy_size > HDA_BUFFER_SIZE) copy_size = HDA_BUFFER_SIZE;

    const uint8_t* src = (const uint8_t*)pcm_data;
    uint8_t* dst = (uint8_t*)dma_buffer;
    for (uint32_t i = 0; i < copy_size; i++) dst[i] = src[i];

    // zero-fill remainder
    for (uint32_t i = copy_size; i < HDA_BUFFER_SIZE; i++) dst[i] = 0;

    // start stream
    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    Write32(stream_base + HDA_SD_CTL, ctl);

    playing = true;
    return true;
}

bool HDAudio::Stop() {
    if (!detected) return false;

    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    Write32(stream_base + HDA_SD_CTL, ctl);

    playing = false;
    g_hda_stream_live = false;
    g_hda_write_ptr = 0;
    g_hda_last_lpib = 0;
    return true;
}

bool HDAudio::StartStream() {
    if (!detected || !dma_buffer) return false;
    if (g_hda_stream_live) return true;

    // Zero the cyclic buffer so the engine plays silence until the mixer
    // catches up.
    uint8_t* b = (uint8_t*)dma_buffer;
    for (uint32_t i = 0; i < HDA_BUFFER_SIZE; i++) b[i] = 0;
    g_hda_write_ptr = 0;
    g_hda_last_lpib = 0;

    // Make sure every BDL entry interrupts on completion so LPIB updates
    // are reliable -- the controller pre-fetches and we need accurate
    // wraparound notification.
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) bdl[i].ioc = 1;

    // set RUN via the low control byte only (the sts byte is write-1-to-clear)
    Write8(stream_base + HDA_SD_CTL, (uint8_t)(HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE));

    // prove the dma engine is really consuming the buffer: sample the link
    // position over ~24 ms (bounded, wall-clock based). if it never moves the
    // stream is dead - stop it and fail so the audio server falls back. (satoru)
    uint32_t l0 = Read32(stream_base + HDA_SD_LPIB);
    uint32_t lmax = l0;
    bool moved = false;
    LS("[hda] stream start lpib0="); LX(l0);
    for (int i = 1; i <= 24; i++) {
        DelayUs(1000);
        uint32_t l = Read32(stream_base + HDA_SD_LPIB);
        if (l != l0) { moved = true; if (l > lmax) lmax = l; }
        if (i == 1 || i == 5 || i == 12 || i == 24) { LS(" +"); LD(i); LS("ms="); LX(l); }
    }
    LS(" sts="); LX(Read8(stream_base + HDA_SD_STS)); LS(" wall="); LX(Read32(HDA_WALCLK)); LS("\r\n");
    if (!moved) {
        Write8(stream_base + HDA_SD_CTL, 0);
        LS("[hda] FAIL stream position did not advance - falling back\r\n");
        return false;
    }

    playing = true;
    g_hda_stream_live = true;
    return true;
}

uint32_t HDAudio::WriteRing(const void* data, uint32_t bytes) {
    if (!g_hda_stream_live || !data || bytes == 0) return 0;
    // dma_buffer is non-null whenever g_hda_stream_live (StartStream gates on it),
    // but guard the deref anyway - zero-risk, can only avert a fault. (satoru)
    if (!dma_buffer) return 0;
    // clamp lpib's per-tick advance so a qemu link-position jump can't fake free
    // space (see HDA_ClampLpibAdvance). (satoru)
    uint32_t raw_lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    uint32_t lpib = HDA_ClampLpibAdvance(raw_lpib);

    // queued = unplayed bytes between the read pointer (lpib) and our write
    // cursor, going forward mod N; free = N - queued.  both branches below
    // already reduce to (N - queued) but computing queued explicitly lets us add
    // a hard lap guard the old code lacked. (satoru)
    uint32_t queued;
    if (g_hda_write_ptr >= lpib) {
        queued = g_hda_write_ptr - lpib;
    } else {
        queued = HDA_BUFFER_SIZE - (lpib - g_hda_write_ptr);
    }

    // lap guard: never let the write cursor reach or pass lpib.  keep at least
    // one period + one frame of slack so even with the full/empty ambiguity at
    // write_ptr==lpib (which the old N-(write-lpib) math mis-read as a totally
    // FREE ring) we can't overwrite samples the dma is mid-flight on.  when the
    // ring is already that full, skip this period - the mixer back-pressure gate
    // will simply try again next tick. (satoru)
    const uint32_t kPeriod = 4096;
    const uint32_t kGuard  = kPeriod + 4;
    if (queued + kGuard >= HDA_BUFFER_SIZE) return 0;
    uint32_t free_bytes = HDA_BUFFER_SIZE - queued - kGuard;
    if (free_bytes == 0) return 0;

    uint32_t to_write = bytes < free_bytes ? bytes : free_bytes;
    const uint8_t* src = (const uint8_t*)data;
    uint8_t* dst_base = (uint8_t*)dma_buffer;

    uint32_t first = HDA_BUFFER_SIZE - g_hda_write_ptr;
    if (first > to_write) first = to_write;
    for (uint32_t i = 0; i < first; i++)
        dst_base[g_hda_write_ptr + i] = src[i];
    uint32_t second = to_write - first;
    for (uint32_t i = 0; i < second; i++)
        dst_base[i] = src[first + i];
    g_hda_write_ptr = (g_hda_write_ptr + to_write) % HDA_BUFFER_SIZE;

    // clean-silence underrun pad: zero exactly one period immediately ahead of
    // the new write cursor so that if the cooperative pump stalls (>~213ms) and
    // the dma read pointer (lpib) catches up to write_ptr, it reads silence
    // instead of replaying the stale samples still sitting in the free region
    // from the previous lap (the audible click/repeat). this is the underrun
    // counterpart to the kGuard overrun lap-guard above, and is provably safe to
    // touch: after this write the guard leaves at least
    //   N - queued_new >= kGuard == kPeriod + 4
    // free bytes forward between write_ptr and lpib (to_write was capped at
    // free_bytes = N - queued - kGuard), so a kPeriod pad ahead of write_ptr
    // stays >=4 bytes short of lpib and never overwrites the bytes the dma is
    // reading now. clamped lpib only lags the true read position, which makes the
    // real margin larger, never smaller. wrap by hand to match the ring math. (satoru)
    uint32_t pad = g_hda_write_ptr;
    for (uint32_t i = 0; i < kPeriod; i++) {
        dst_base[pad] = 0;
        pad++;
        if (pad == HDA_BUFFER_SIZE) pad = 0;
    }
    return to_write;
}

uint32_t HDAudio::RingQueuedBytes() {
    if (!g_hda_stream_live) return 0;
    // use the same clamped link position as WriteRing so the back-pressure gate
    // sees a stable queue depth instead of jittering on qemu lpib jumps. (satoru)
    uint32_t raw_lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    uint32_t lpib = HDA_ClampLpibAdvance(raw_lpib);
    if (g_hda_write_ptr >= lpib) return g_hda_write_ptr - lpib;
    return HDA_BUFFER_SIZE - (lpib - g_hda_write_ptr);
}

uint32_t HDAudio::RingChunkBytes() { return 4096; }

// blank the ring once the mixer stops feeding it. the controller loops the cyclic
// buffer forever, so without this the last ~0.7 s of audio repeats until the next
// sound (measured in the qemu wav capture). first call: zero the free region
// [write_ptr, lpib). later calls: zero whatever the engine has passed since the
// last call. stops after one full lap of link position, at which point the whole
// ring is silent and the server quits touching the hardware. (satoru)
static uint32_t g_scrub_pos  = 0;
static uint32_t g_scrub_left = 0;

bool HDAudio::ScrubIdle(bool first) {
    if (!g_hda_stream_live || !dma_buffer) return false;
    uint8_t* b = (uint8_t*)dma_buffer;
    uint32_t lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    if (first) {
        uint32_t n = (lpib + HDA_BUFFER_SIZE - g_hda_write_ptr) % HDA_BUFFER_SIZE;
        if (n == 0) n = HDA_BUFFER_SIZE;            // caught up: nothing queued
        uint32_t p = g_hda_write_ptr;
        for (uint32_t i = 0; i < n; i++) { b[p] = 0; if (++p == HDA_BUFFER_SIZE) p = 0; }
        g_scrub_pos = lpib;
        g_scrub_left = HDA_BUFFER_SIZE;
        return true;
    }
    uint32_t adv = (lpib + HDA_BUFFER_SIZE - g_scrub_pos) % HDA_BUFFER_SIZE;
    uint32_t p = g_scrub_pos;
    for (uint32_t i = 0; i < adv; i++) { b[p] = 0; if (++p == HDA_BUFFER_SIZE) p = 0; }
    g_scrub_pos = lpib;
    g_scrub_left = (adv >= g_scrub_left) ? 0 : (g_scrub_left - adv);
    return g_scrub_left != 0;
}

bool HDAudio::IsPlaying() { return playing; }

void HDAudio::SetVolume(uint8_t vol) {
    volume = vol;
    if (detected && g_ndacs > 0) {
        // scale across each dac's real step count (read from its amp caps), not
        // a fixed 63, so the top of the slider is the codec's 0 dB point. the pin
        // and mixer amps stay at the unmuted 0 dB set during path programming. (satoru)
        for (int i = 0; i < g_ndacs; i++) {
            uint32_t steps = (uint32_t)g_dac_steps[i];
            if (steps == 0) continue;
            uint32_t gain = ((uint32_t)vol * steps) / 255u;
            uint32_t amp_val = (1u << 15) | (1u << 13) | (1u << 12) | (gain & 0x7F);
            if (vol == 0) amp_val |= 0x80;   // mute at zero
            uint32_t resp;
            SendVerb(HDA_VERB4(codec_addr, g_dacs[i], HDA_VERB_SET_AMP_GAIN4, amp_val), &resp);
        }
    }
}

uint8_t HDAudio::GetVolume() { return volume; }

uint32_t HDAudio::GetPosition() {
    if (!detected) return 0;
    return Read32(stream_base + HDA_SD_LPIB);
}

uint32_t HDAudio::GetBufferSize() { return HDA_BUFFER_SIZE; }

void HDAudio::DumpInfo(char* out, int max_len) {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12]; int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else { char rev[12]; int ri = 0; uint32_t tmp = val;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri]; }
        buf[i] = 0; append(buf);
    };
    auto append_hex = [&](uint32_t val, int digits) {
        const char* hex = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; i--)
            if (pos < max_len - 1) out[pos++] = hex[(val >> (i * 4)) & 0xF];
    };

    if (!detected) {
        append("Intel HD Audio: Not detected\n");
        out[pos] = 0; return;
    }

    append("Intel HD Audio Controller\n");
    append("  Codecs: "); append_num(codec_count); append("\n");

    for (int i = 0; i < codec_count; i++) {
        append("  Codec "); append_num(i);
        append(": Vendor=0x"); append_hex(codec_vendors[i] >> 16, 4);
        append(":0x"); append_hex(codec_vendors[i] & 0xFFFF, 4);
        append("\n");
    }

    append("  Output DAC NID: "); append_num(output_nid >= 0 ? output_nid : 0); append("\n");
    append("  Output Pin NID: "); append_num(pin_nid >= 0 ? pin_nid : 0); append("\n");
    append("  Format: "); append_num(current_format.sample_rate);
    append(" Hz, "); append_num(current_format.bits);
    append("-bit, "); append_num(current_format.channels);
    append(" ch\n");
    append("  Volume: "); append_num(volume); append("/255\n");
    append("  Playing: "); append(playing ? "Yes" : "No"); append("\n");
    append("  Buffer: "); append_num(HDA_BUFFER_SIZE); append(" bytes\n");

    // controller location, chosen codec, every driven output path, live position (satoru)
    append("  Controller: "); append_num(g_cand.bus); append(":"); append_num(g_cand.dev);
    append("."); append_num(g_cand.fn); append(" ven=0x"); append_hex(g_cand.ven, 4);
    append(" dev=0x"); append_hex(g_cand.did, 4); append("\n");
    append("  Codec addr: "); append_num(codec_addr); append("  verb timeouts: "); append_num(g_timeouts);
    append(g_dead ? "  (link gave up)\n" : "\n");
    for (int i = 0; i < g_npaths; i++) {
        append("  Path "); append_num(i); append(": pin 0x"); append_hex(g_paths[i].pin, 2);
        append(" dev="); append_num(g_paths[i].dev); append(" chain=");
        for (int k = 0; k < g_paths[i].n; k++) {
            if (k) append(">");
            append_hex(g_paths[i].nodes[k], 2);
        }
        append("\n");
    }
    append("  Stream: "); append(g_hda_stream_live ? "live" : "stopped");
    append("  LPIB="); append_num(Read32(stream_base + HDA_SD_LPIB));
    append("  writeptr="); append_num(g_hda_write_ptr); append("\n");

    out[pos] = 0;
}
