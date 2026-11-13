//  kurono os - per vcpu local apic (see vlapic.h)
#include "vlapic.h"
#include "vmexit.h"
#include "../drivers/serial.h"

namespace {
    struct Lapic {
        uint32_t reg[0x400 / 4];
        uint32_t irr[8], isr[8];
        bool     enabled;
        uint32_t t_init, t_div;         // initial count, divisor
        uint64_t t_start_ns;            // when the current count started
        bool     t_armed;
    };
    Lapic g_l[VLAPIC_MAX];
    int g_n = 1;
    VLapic::IpiFn g_ipi = nullptr;

    enum { ID = 0x20, VER = 0x30, TPR = 0x80, PPR = 0xA0, EOI = 0xB0, LDR = 0xD0, DFR = 0xE0, SVR = 0xF0,
           ESR = 0x280, ICR_LO = 0x300, ICR_HI = 0x310, LVT_TIMER = 0x320, LVT_THERM = 0x330, LVT_PERF = 0x340,
           LVT_LINT0 = 0x350, LVT_LINT1 = 0x360, LVT_ERR = 0x370, T_INIT = 0x380, T_CUR = 0x390, T_DIV = 0x3E0 };

    inline uint32_t& R(Lapic& l, uint32_t off) { return l.reg[off / 4]; }

    uint64_t period_ns(const Lapic& l) { return (uint64_t)l.t_init * l.t_div * 10ULL; }   // 100 mhz base clock

    void reset_regs(int c) {
        Lapic& l = g_l[c];
        for (unsigned i = 0; i < sizeof(l.reg) / 4; i++) l.reg[i] = 0;
        for (int i = 0; i < 8; i++) l.irr[i] = l.isr[i] = 0;
        R(l, ID) = (uint32_t)c << 24;
        R(l, VER) = 0x00050014;
        R(l, DFR) = 0xFFFFFFFF;
        R(l, SVR) = 0xFF;
        for (uint32_t o = LVT_TIMER; o <= LVT_ERR; o += 0x10) R(l, o) = 0x10000;   // masked
        l.enabled = false;
        l.t_init = 0; l.t_div = 2; l.t_armed = false;
    }

    int highest(const uint32_t* v) {
        for (int i = 7; i >= 0; i--) {
            if (!v[i]) continue;
            for (int b = 31; b >= 0; b--) if (v[i] & (1u << b)) return i * 32 + b;
        }
        return -1;
    }

    void deliver(int dest, uint8_t vec, int mode) {
        if (dest < 0 || dest >= g_n) return;
        if (mode == 0 || mode == 1) VLapic::Raise(dest, vec);
        else if ((mode == 5 || mode == 6) && g_ipi) g_ipi(dest, mode, vec);
        // smi / nmi / startup of a running cpu: ignored
    }

    void send_icr(int src, uint32_t lo, uint32_t hi) {
        int mode = (lo >> 8) & 7, shorthand = (lo >> 18) & 3;
        bool logical = (lo >> 11) & 1;
        uint8_t vec = (uint8_t)lo;
        uint8_t dest = (uint8_t)(hi >> 24);
        if (mode == 5 && (lo & (1u << 15)) && !(lo & (1u << 14))) return;   // init level de-assert
        for (int c = 0; c < g_n; c++) {
            bool hit = false;
            switch (shorthand) {
                case 0:
                    if (!logical) hit = (dest == 0xFF) || dest == c;
                    else hit = (R(g_l[c], LDR) >> 24) & dest;     // flat model
                    break;
                case 1: hit = (c == src); break;
                case 2: hit = true; break;
                case 3: hit = (c != src); break;
            }
            if (hit) deliver(c, vec, mode);
        }
    }

    void timer_fire(int c) {
        Lapic& l = g_l[c];
        uint32_t lvt = R(l, LVT_TIMER);
        if (!(lvt & 0x10000)) VLapic::Raise(c, (uint8_t)lvt);
    }
}

void VLapic::Reset(int ncpu, IpiFn ipi) {
    g_n = ncpu < 1 ? 1 : (ncpu > VLAPIC_MAX ? VLAPIC_MAX : ncpu);
    g_ipi = ipi;
    for (int c = 0; c < VLAPIC_MAX; c++) reset_regs(c);
}
int VLapic::Count() { return g_n; }
void VLapic::ResetOne(int c) { if (c >= 0 && c < g_n) reset_regs(c); }
bool VLapic::Enabled(int c) { return c >= 0 && c < g_n && g_l[c].enabled; }

uint32_t VLapic::Read(int c, uint32_t off) {
    if (c < 0 || c >= g_n || off >= 0x400) return 0;
    Lapic& l = g_l[c];
    if (off >= 0x100 && off < 0x180) return l.isr[(off - 0x100) / 0x10];
    if (off >= 0x200 && off < 0x280) return l.irr[(off - 0x200) / 0x10];
    if (off >= 0x180 && off < 0x200) return 0;                  // tmr
    if (off == PPR) { int h = highest(l.isr); uint32_t t = R(l, TPR) & 0xF0; uint32_t s = h < 0 ? 0 : (uint32_t)h & 0xF0; return t > s ? t : s; }
    if (off == T_CUR) {
        if (!l.t_armed || !l.t_init) return 0;
        uint64_t now = VMExitHandler::GuestClockNs();
        uint64_t per = period_ns(l);
        uint64_t el = now - l.t_start_ns;
        if (el >= per) {
            if (((R(l, LVT_TIMER) >> 17) & 3) != 1) return 0;     // one-shot done
            el %= per;
        }
        return (uint32_t)((per - el) / (10ULL * l.t_div));
    }
    return R(l, off);
}

void VLapic::Write(int c, uint32_t off, uint32_t v) {
    if (c < 0 || c >= g_n || off >= 0x400) return;
    Lapic& l = g_l[c];
    switch (off) {
        case ID: R(l, ID) = v & 0xFF000000; break;
        case TPR: R(l, TPR) = v & 0xFF; break;
        case EOI: { int h = highest(l.isr); if (h >= 0) l.isr[h / 32] &= ~(1u << (h % 32)); break; }
        case LDR: R(l, LDR) = v & 0xFF000000; break;
        case DFR: R(l, DFR) = v | 0x0FFFFFFF; break;
        case SVR: R(l, SVR) = v; l.enabled = (v & 0x100) != 0;
            if (!l.enabled) for (uint32_t o = LVT_TIMER; o <= LVT_ERR; o += 0x10) R(l, o) |= 0x10000;
            break;
        case ESR: R(l, ESR) = 0; break;
        case ICR_HI: R(l, ICR_HI) = v; break;
        case ICR_LO: R(l, ICR_LO) = v & ~0x1000u; send_icr(c, v, R(l, ICR_HI)); break;
        case LVT_TIMER: case LVT_THERM: case LVT_PERF: case LVT_LINT0: case LVT_LINT1: case LVT_ERR:
            R(l, off) = l.enabled ? v : (v | 0x10000);
            break;
        case T_INIT:
            R(l, T_INIT) = v; l.t_init = v;
            l.t_start_ns = VMExitHandler::GuestClockNs();
            l.t_armed = v != 0;
            break;
        case T_DIV: {
            R(l, T_DIV) = v;
            static const uint32_t tab[8] = { 2, 4, 8, 16, 32, 64, 128, 1 };
            l.t_div = tab[((v & 8) >> 1 | (v & 3)) & 7];
            break;
        }
        case 0x20 + 0x1000: break;
        default: R(l, off) = v; break;
    }
}

void VLapic::Poll(uint64_t now) {
    for (int c = 0; c < g_n; c++) {
        Lapic& l = g_l[c];
        if (!l.t_armed || !l.t_init) continue;
        uint64_t per = period_ns(l);
        if (now - l.t_start_ns < per) continue;
        timer_fire(c);
        if (((R(l, LVT_TIMER) >> 17) & 3) == 1) {                // periodic
            uint64_t n = (now - l.t_start_ns) / per;
            l.t_start_ns += n * per;                             // missed periods collapse into one tick
        } else l.t_armed = false;
    }
}

int VLapic::Pending(int c) {
    if (c < 0 || c >= g_n || !g_l[c].enabled) return -1;
    Lapic& l = g_l[c];
    int h = highest(l.irr);
    if (h < 0) return -1;
    uint32_t tpr = R(l, TPR) & 0xF0;
    int hi = highest(l.isr);
    if (hi >= 0 && (h & 0xF0) <= (hi & 0xF0)) return -1;
    if (((uint32_t)h & 0xF0) <= tpr) return -1;
    return h;
}

uint32_t g_cnt[VLAPIC_MAX][256];
void VLapic::Accept(int c, int vec) {
    if (c < 0 || c >= g_n || vec < 0) return;
    g_cnt[c][vec & 255]++;
    Lapic& l = g_l[c];
    l.irr[vec / 32] &= ~(1u << (vec % 32));
    l.isr[vec / 32] |= 1u << (vec % 32);
}

static void (*g_kickfn)(int) = nullptr;
void VLapic::SetKick(void (*fn)(int)) { g_kickfn = fn; }
void VLapic::Raise(int c, uint8_t vec) {
    if (c < 0 || c >= g_n || vec < 16) return;
    g_l[c].irr[vec / 32] |= 1u << (vec % 32);
    if (g_kickfn) g_kickfn(c);
}

void VLapic::DumpCounts() {
    for (int c = 0; c < g_n; c++) for (int v = 0; v < 256; v++) if (g_cnt[c][v] > 20) {
        SerialLogger::Log("[lapic] cpu "); SerialLogger::LogDec(c); SerialLogger::Log(" vec 0x"); SerialLogger::LogHex(v);
        SerialLogger::Log(" x"); SerialLogger::LogDec((int)g_cnt[c][v]);
        SerialLogger::Log(" lvt_timer=0x"); SerialLogger::LogHex(g_l[c].reg[0x320 / 4]);
        SerialLogger::Log(" init="); SerialLogger::LogDec((int)g_l[c].t_init); SerialLogger::Log(" div="); SerialLogger::LogDec((int)g_l[c].t_div);
        SerialLogger::Log("\r\n");
    }
}

// end (satoru)
