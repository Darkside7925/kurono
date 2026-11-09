// intel display stage 1: displayport aux (native + i2c-over-aux) on display
// version 14, read-only towards the display pipeline. see the header for the
// contract. (satoru)
//
// register sources, torvalds/linux master, drivers/gpu/drm/i915/display/:
//  - intel_dp_aux_regs.h: dp_aux_ch_ctl a 0x64010 b 0x64110, xelpdp usbc1
//    0x16f210 usbc2 0x16f410 (+0x200 per tc), data1..5 at ctl + 4 .. + 20,
//    ctl bits 31 send_busy, 30 done, 29 interrupt, 28 time_out_error, 27:26
//    time_out (3 = max), 25 receive_error, 24:20 message_size, 19 xelpdp
//    power_request, 18 xelpdp power_status, 11 tbt_io, 9:5 fw_sync_pulse
//    (len - 1), 4:0 sync_pulse (len - 1). (satoru)
//  - intel_dp_aux.c: skl_get_aux_send_ctl (ver >= 14 keeps power_request
//    set in every send), intel_dp_aux_xfer (3 x 1 ms busy wait, 5 tries,
//    10 ms done wait, clear done/errors, 400 us after a receive error),
//    intel_dp_aux_transfer (header packing, rx size + 1), sync len 32, fw
//    sync len 18. (satoru)
//  - intel_display_power_well.c: xelpdp_aux_power_well_enable (rmw set
//    power_request, then a fixed 600 us wait because the status bit is not
//    reliable without an lt phy) and _disable (rmw clear, 10-30 us). (satoru)
//  - intel_display_power_map.c: xelpdp aux_tc1..4 wells have no parent
//    other than pw_1 / dc off, the usbc aux registers are pica always-on. (satoru)
//  - intel_tc.c: xelpdp_tc_phy_* (ownership = port_buf_ctl1 bit 6, tcss
//    power request/state bits 5/4, phy ready bit 24 in intel_cx0_phy_regs.h,
//    hpd live from picainterrupt_isr dp-alt/tbt and sdeisr legacy, the
//    legacy bit only counts for a vbt legacy port). (satoru)
//  - intel_display_regs.h: tcss_ddi_status 0x161500 + 4 * tc (bit 2 ready,
//    bit 1 tbt live, bit 0 alt live). (satoru)
//  - intel_psr_regs.h: srd_ctl 0x60800 bit 31, psr2_ctl 0x60900 bit 31;
//    intel_display_regs.h trans_dp2_ctl 0x600a0 bit 30 panel replay. (satoru)
// sink side, drivers/gpu/drm/display/drm_dp_helper.c: drm_dp_dpcd_access
// (32 retries, 500 us between non-timeout failures), drm_dp_i2c_do_msg,
// drm_dp_i2c_drain_msg, drm_dp_i2c_xfer (bare address start and stop
// packets, mot on every chunk), drm_dp_i2c_retry_count at 10 khz. edid
// segment pointer 0x30 and address 0x50 from drm_edid.c. (satoru)
#include "intel_dp_aux.h"
#include "dp_edid_decode.h"
#ifdef DPAUX_HOST_MOCK
// host unit test only: simulated mmio + sink, logger and cpu stubs (satoru)
#include "dpaux_host_mock.h"
#else
#include "intel_display_probe.h"
#include "serial.h"
#include "cpu_detect.h"
#endif

namespace {

using DpEdid::Txt;

// ---- registers (sources above) (satoru) ----
constexpr uint32_t R_PWR_WELL_CTL2 = 0x45404;     // pw_1 state is bit 0 (satoru)
constexpr uint32_t R_DC_STATE_EN = 0x45504;
constexpr uint32_t R_SDEISR = 0xc4000;
constexpr uint32_t R_PICA_ISR = 0x16fe50;
constexpr uint32_t R_PP_STATUS0 = 0xc7200;        // pch_pps_base + 0, bit 31 on (satoru)
constexpr uint32_t R_SRD_CTL_A = 0x60800;
constexpr uint32_t R_PSR2_CTL_A = 0x60900;
constexpr uint32_t R_TRANS_DP2_CTL_A = 0x600a0;
constexpr uint32_t R_TCSS_DDI_STATUS = 0x161500;

constexpr uint32_t AUX_SEND_BUSY = 1u << 31;
constexpr uint32_t AUX_DONE = 1u << 30;
constexpr uint32_t AUX_TIME_OUT_ERROR = 1u << 28;
constexpr uint32_t AUX_TIME_OUT_MAX = 3u << 26;
constexpr uint32_t AUX_RECEIVE_ERROR = 1u << 25;
constexpr uint32_t AUX_PWR_REQ = 1u << 19;
constexpr uint32_t AUX_PWR_ST = 1u << 18;
inline uint32_t aux_msg_size(int n) { return ((uint32_t)n & 0x1F) << 20; }
inline uint32_t aux_fw_sync(int len) { return ((uint32_t)(len - 1) & 0x1F) << 5; }
inline uint32_t aux_sync(int len) { return (uint32_t)(len - 1) & 0x1F; }

// port_buf_ctl1 (intel_cx0_phy_regs.h xelpdp_port_buf_ctl1) (satoru)
constexpr uint32_t BUF1_PHY_READY = 1u << 24;
constexpr uint32_t BUF1_TC_OWNED = 1u << 6;
constexpr uint32_t BUF1_TCSS_PWR_ST = 1u << 4;

// drm_dp.h request codes and replies (satoru)
constexpr uint8_t I2C_WRITE = 0x0, I2C_READ = 0x1, I2C_WRITE_STATUS_UPDATE = 0x2, I2C_MOT = 0x4;
constexpr uint8_t NATIVE_READ = 0x9;
constexpr uint8_t NATIVE_WRITE = 0x8;   // stage 2 api only, Run never sends it (satoru)
constexpr uint8_t NATIVE_REPLY_MASK = 0x3, NATIVE_ACK = 0x0, NATIVE_NACK = 0x1, NATIVE_DEFER = 0x2;
constexpr uint8_t I2C_REPLY_MASK = 0xC, I2C_ACK = 0x0, I2C_NACK = 0x4, I2C_DEFER = 0x8;
constexpr uint32_t AUX_RETRY_INTERVAL_US = 500;

// errors (negative returns) (satoru)
enum {
    E_NOTSTARTED = -1,  // send_busy stuck before we started (linux -ebusy) (satoru)
    E_2BIG = -2,
    E_NOTDONE = -3,     // never saw done (linux -ebusy) (satoru)
    E_RCV = -4,         // receive error (linux -eio) (satoru)
    E_TIMEOUT = -5,     // hw aux timeout: nobody answered (linux -etimedout) (satoru)
    E_BADSIZE = -6,     // reply size 0 or > 20 (linux -ebusy) (satoru)
    E_BUDGET = -7,      // our own per-port time / transaction cap hit (satoru)
    E_NACK = -8,
    E_PROTO = -9,       // short reply (satoru)
    E_RETRIES = -10,
};

const char* err_name(int e) {
    switch (e) {
        case E_NOTSTARTED: return "busy-before-start";
        case E_2BIG: return "too-big";
        case E_NOTDONE: return "not-done";
        case E_RCV: return "receive-error";
        case E_TIMEOUT: return "aux-timeout(no reply)";
        case E_BADSIZE: return "bad-reply-size";
        case E_BUDGET: return "budget-exhausted";
        case E_NACK: return "nack";
        case E_PROTO: return "short-reply";
        case E_RETRIES: return "too-many-retries";
        default: return "?";
    }
}

struct AuxPort {
    const char* name;
    int tc;             // 0 combo, 1..4 type-c (satoru)
    int pin;            // hpd pin: combo a=0 b=1, tc1..4 = 0..3 (satoru)
    uint32_t ctl;       // dp_aux_ch_ctl (satoru)
    uint32_t buf1;      // xelpdp_port_buf_ctl1 (satoru)
};

const AuxPort PORTS[] = {
    { "A",   0, 0, 0x64010,  0x64004  },
    { "B",   0, 1, 0x64110,  0x64104  },
    { "TC1", 1, 0, 0x16f210, 0x16f200 },
    { "TC2", 2, 1, 0x16f410, 0x16f400 },
    { "TC3", 3, 2, 0x16f610, 0x16f600 },
    { "TC4", 4, 3, 0x16f810, 0x16f800 },
};
constexpr int NPORTS = (int)(sizeof(PORTS) / sizeof(PORTS[0]));

volatile uint8_t* g_mmio = nullptr;
char* g_out = nullptr;
int g_out_max = 0;
int g_out_pos = 0;

#ifdef DPAUX_HOST_MOCK
inline uint32_t rd(uint32_t off) { return MockMmioRead(off); }
inline void wr_aux(uint32_t off, uint32_t v) { MockMmioWrite(off, v); }
#else
inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }
// the only mmio write in this file. callers pass aux ctl/data offsets only. (satoru)
inline void wr_aux(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_mmio + off) = v;
    __asm__ __volatile__("" ::: "memory");
}
#endif

// ---- output (satoru) ----
void emit(const char* line) {
    SerialLogger::Log("[igpu-aux] ");
    SerialLogger::Log(line);
    SerialLogger::Log("\r\n");
    if (g_out && g_out_pos < g_out_max - 2) {
        static const char pre[] = "[igpu-aux] ";
        for (int i = 0; pre[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = pre[i];
        for (int i = 0; line[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = line[i];
        g_out[g_out_pos++] = '\n';
        g_out[g_out_pos] = 0;
    }
}

struct Ln {
    char b[320];
    Txt t;
    Ln() : t(b, sizeof(b)) {}
    void go() { emit(b); }
};

void say(const char* s) { emit(s); }

// decoder line sink: ctx is the port name (satoru)
void port_line(void* ctx, const char* line) {
    Ln l; l.t.s((const char*)ctx); l.t.c(' '); l.t.s(line); l.go();
}

void hexdump(const char* port, const char* what, uint32_t base, const uint8_t* p, int n) {
    for (int o = 0; o < n; o += 16) {
        Ln l; l.t.s(port); l.t.c(' '); l.t.s(what); l.t.s(" +"); l.t.x(base + (uint32_t)o, 4); l.t.c(':');
        for (int i = o; i < o + 16 && i < n; i++) { l.t.c(' '); l.t.x(p[i], 2); }
        l.go();
    }
}

// ---- time: tsc when the cpu reports its frequency, else port 0x80 writes
// (about 1 us each, the classic io delay). every loop below is also
// iteration capped, so a wrong clock can only shorten or lengthen a wait,
// never make one unbounded. interrupts are never disabled here. (satoru)
uint64_t g_tpu = 0;          // tsc ticks per microsecond, 0 = no tsc clock (satoru)
bool g_clock_init = false;

inline uint64_t tsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void clock_init() {
    if (g_clock_init) return;
    g_clock_init = true;
    CpuInfo ci = CPUDetect::GetInfo();
    uint64_t f = ci.frequency.tsc_frequency;
    if (!f && ci.frequency.base_mhz > 0) f = (uint64_t)ci.frequency.base_mhz * 1000000ull;
    uint64_t t = f / 1000000ull;
    g_tpu = (t >= 100 && t <= 10000) ? t : 0;
}

void udelay(uint32_t us) {
    if (g_tpu) {
        uint64_t end = tsc() + (uint64_t)us * g_tpu;
        // cap: 64 pause per us is far more than a 10 ghz clock needs (satoru)
        for (uint64_t guard = (uint64_t)us * 4096 + 64; guard && tsc() < end; guard--)
            __asm__ __volatile__("pause");
    } else {
        for (uint32_t i = 0; i < us; i++) __asm__ __volatile__("outb %%al, $0x80" : : "a"((uint8_t)0));
    }
}

uint64_t now_us() { return g_tpu ? tsc() / g_tpu : 0; }

// per-port budget: wall time (when the tsc clock works) and a transaction
// count, so an absent or misbehaving sink costs at most ~1.5 s. (satoru)
constexpr int MAX_XFERS_PER_PORT = 600;
constexpr uint64_t PORT_BUDGET_US = 1500000;
int g_xfers = 0;
int g_max_xfers = MAX_XFERS_PER_PORT;   // BeginSession may raise it for stage 2 (satoru)
uint64_t g_deadline = 0;
uint32_t g_last_status = 0;
int g_stat_timeouts = 0, g_stat_defers = 0, g_stat_rcverr = 0;

bool budget_left() {
    if (g_xfers >= g_max_xfers) return false;
    if (g_tpu && g_deadline && now_us() > g_deadline) return false;
    return true;
}

uint32_t pack(const uint8_t* s, int n) {
    uint32_t v = 0;
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) v |= (uint32_t)s[i] << ((3 - i) * 8);
    return v;
}

void unpack(uint32_t v, uint8_t* d, int n) {
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) d[i] = (uint8_t)(v >> ((3 - i) * 8));
}

// intel_dp_aux_wait_done (polling path): wait up to 10 ms for send_busy to
// clear. (satoru)
uint32_t wait_done(uint32_t ctl) {
    uint32_t st = rd(ctl);
    for (int i = 0; i < 2000 && (st & AUX_SEND_BUSY); i++) {
        udelay(5);
        st = rd(ctl);
    }
    return st;
}

// intel_dp_aux_xfer for display ver 14 (skl send ctl with power_request).
// one difference on purpose: the interrupt-on-done bit (29) is left clear,
// because nothing here handles a display interrupt and we poll anyway. and
// send_busy is masked out of the clear-status write so a stuck channel is
// never re-triggered. (satoru)
int aux_xfer(const AuxPort& p, const uint8_t* send, int sn, uint8_t* recv, int rmax, bool tbt_io) {
    if (!budget_left()) return E_BUDGET;
    g_xfers++;
    uint32_t ctl = p.ctl;
    uint32_t st = 0;
    int t;
    for (t = 0; t < 3; t++) {
        st = rd(ctl);
        if (!(st & AUX_SEND_BUSY)) break;
        udelay(1000);
    }
    g_last_status = st;
    if (t == 3) return E_NOTSTARTED;
    if (sn > 20 || rmax > 20 || sn <= 0) return E_2BIG;

    uint32_t send_ctl = AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_TIME_OUT_MAX |
                        AUX_RECEIVE_ERROR | aux_msg_size(sn) | aux_fw_sync(18) | aux_sync(32) |
                        AUX_PWR_REQ;
    if (tbt_io) send_ctl |= 1u << 11;

    for (t = 0; t < 5; t++) {
        for (int i = 0; i < sn; i += 4) wr_aux(ctl + 4 + (uint32_t)i, pack(send + i, sn - i));
        wr_aux(ctl, send_ctl);
        st = wait_done(ctl);
        wr_aux(ctl, (st & ~AUX_SEND_BUSY) | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR);
        g_last_status = st;
        if (st & AUX_SEND_BUSY) continue;          // 10 ms passed, try again (satoru)
        if (st & AUX_TIME_OUT_ERROR) { g_stat_timeouts++; continue; }
        if (st & AUX_RECEIVE_ERROR) { g_stat_rcverr++; udelay(400); continue; }
        if (st & AUX_DONE) break;
    }
    if (!(st & AUX_DONE)) return E_NOTDONE;
    if (st & AUX_RECEIVE_ERROR) return E_RCV;
    if (st & AUX_TIME_OUT_ERROR) return E_TIMEOUT;
    int got = (int)((st >> 20) & 0x1F);
    if (got == 0 || got > 20) return E_BADSIZE;
    if (got > rmax) got = rmax;
    for (int i = 0; i < got; i += 4) unpack(rd(ctl + 4 + (uint32_t)i), recv + i, got - i);
    return got;
}

// intel_dp_aux_transfer: header + payload, returns payload bytes and the
// reply nibble. (satoru)
int aux_transfer(const AuxPort& p, uint8_t req, uint32_t addr, uint8_t* buf, int size, uint8_t* reply, bool tbt_io) {
    uint8_t tx[20], rx[20];
    tx[0] = (uint8_t)((req << 4) | ((addr >> 16) & 0xF));
    tx[1] = (uint8_t)((addr >> 8) & 0xFF);
    tx[2] = (uint8_t)(addr & 0xFF);
    tx[3] = (uint8_t)(size - 1);
    uint8_t base = req & (uint8_t)~I2C_MOT;
    int ret;
    if (base == I2C_WRITE || base == I2C_WRITE_STATUS_UPDATE || base == NATIVE_WRITE) {
        int txs = size ? 4 + size : 3;
        if (txs > 20) return E_2BIG;
        for (int i = 0; i < size; i++) tx[4 + i] = buf[i];
        ret = aux_xfer(p, tx, txs, rx, 2, tbt_io);
        if (ret > 0) {
            *reply = rx[0] >> 4;
            if (ret > 1) { ret = rx[1]; if (ret > size) ret = size; }
            else ret = size;
        }
    } else if (base == I2C_READ || base == NATIVE_READ) {
        int txs = size ? 4 : 3;
        if (size + 1 > 20) return E_2BIG;
        ret = aux_xfer(p, tx, txs, rx, size + 1, tbt_io);
        if (ret > 0) {
            *reply = rx[0] >> 4;
            ret--;
            for (int i = 0; i < ret; i++) buf[i] = rx[1 + i];
        }
    } else {
        return E_2BIG;
    }
    return ret;
}

// drm_dp_dpcd_access for a native read. linux retries 32 times; a hard aux
// timeout (no sink) stops after 3 so an empty port stays cheap. (satoru)
int dpcd_read(const AuxPort& p, uint32_t addr, uint8_t* buf, int size, bool tbt_io) {
    int ret = 0, first = 0, hard_to = 0;
    for (int r = 0; r < 32; r++) {
        if (ret != 0 && ret != E_TIMEOUT) udelay(AUX_RETRY_INTERVAL_US);
        uint8_t reply = 0xF;
        ret = aux_transfer(p, NATIVE_READ, addr, buf, size, &reply, tbt_io);
        if (ret >= 0) {
            uint8_t nr = reply & NATIVE_REPLY_MASK;
            if (nr == NATIVE_ACK) {
                if (ret == size) return ret;
                ret = E_PROTO;
            } else if (nr == NATIVE_DEFER) {
                g_stat_defers++;
                ret = E_RETRIES;
            } else {
                ret = E_NACK;
            }
        }
        if (!first) first = ret;
        if (ret == E_TIMEOUT && ++hard_to >= 3) break;
        if (ret == E_BUDGET || ret == E_NOTSTARTED) break;
    }
    return first ? first : ret;
}

// drm_dp_dpcd_access for a native write (stage 2 only): same retry rules
// as dpcd_read, a short ack counts as an error like linux's -eproto. (satoru)
int dpcd_write(const AuxPort& p, uint32_t addr, const uint8_t* src, int size, bool tbt_io) {
    int ret = 0, first = 0, hard_to = 0;
    uint8_t buf[16];
    if (size <= 0 || size > 16) return E_2BIG;
    for (int r = 0; r < 32; r++) {
        if (ret != 0 && ret != E_TIMEOUT) udelay(AUX_RETRY_INTERVAL_US);
        for (int i = 0; i < size; i++) buf[i] = src[i];
        uint8_t reply = 0xF;
        ret = aux_transfer(p, NATIVE_WRITE, addr, buf, size, &reply, tbt_io);
        if (ret >= 0) {
            uint8_t nr = reply & NATIVE_REPLY_MASK;
            if (nr == NATIVE_ACK) {
                if (ret == size) return ret;
                ret = E_PROTO;
            } else if (nr == NATIVE_DEFER) {
                g_stat_defers++;
                ret = E_RETRIES;
            } else {
                ret = E_NACK;
            }
        }
        if (!first) first = ret;
        if (ret == E_TIMEOUT && ++hard_to >= 3) break;
        if (ret == E_BUDGET || ret == E_NOTSTARTED) break;
    }
    return first ? first : ret;
}

// drm_dp_i2c_retry_count at the default 10 khz i2c speed (satoru)
int i2c_retry_count(uint8_t req, int size) {
    int aux_req = 10 + 20 + 4 + 4 + 20 + 8 + (((req & I2C_READ) == 0) ? size * 8 : 0);
    int aux_rep = 10 + 20 + 4 + 4 + 4 + ((req & I2C_READ) ? size * 8 : 0);
    int i2c_us = ((1 + 9 + size * 9 + 1) * 1000 + 9) / 10;
    int div = aux_req + aux_rep + (int)AUX_RETRY_INTERVAL_US;
    return (i2c_us + div - 1) / div;
}

// drm_dp_i2c_do_msg (satoru)
int i2c_do_msg(const AuxPort& p, uint8_t* req, uint8_t addr, uint8_t* buf, int size, bool tbt_io) {
    int maxr = i2c_retry_count(*req, size);
    if (maxr < 7) maxr = 7;
    int defer_i2c = 0;
    for (int r = 0; r < maxr + defer_i2c; r++) {
        uint8_t reply = 0xF;
        int ret = aux_transfer(p, *req, addr, buf, size, &reply, tbt_io);
        if (ret < 0) {
            if (ret == E_NOTDONE || ret == E_BADSIZE) continue;   // linux -ebusy (satoru)
            return ret;
        }
        switch (reply & NATIVE_REPLY_MASK) {
            case NATIVE_ACK: break;
            case NATIVE_NACK: return E_NACK;
            case NATIVE_DEFER: g_stat_defers++; udelay(AUX_RETRY_INTERVAL_US); continue;
            default: return E_NACK;
        }
        switch (reply & I2C_REPLY_MASK) {
            case I2C_ACK:
                // short write: next try is a write_status_update (satoru)
                if (ret != size && (*req & ~I2C_MOT) == I2C_WRITE)
                    *req = (uint8_t)(I2C_WRITE_STATUS_UPDATE | (*req & I2C_MOT));
                return ret;
            case I2C_NACK: return E_NACK;
            case I2C_DEFER:
                g_stat_defers++;
                if (defer_i2c < 7) defer_i2c++;
                udelay(AUX_RETRY_INTERVAL_US);
                if ((*req & ~I2C_MOT) == I2C_WRITE)
                    *req = (uint8_t)(I2C_WRITE_STATUS_UPDATE | (*req & I2C_MOT));
                continue;
            default: return E_NACK;
        }
    }
    return E_RETRIES;
}

// drm_dp_i2c_drain_msg (satoru)
int i2c_drain(const AuxPort& p, uint8_t req, uint8_t addr, uint8_t* buf, int size, bool tbt_io) {
    int ret = size;
    while (size > 0) {
        uint8_t r = req;
        int got = i2c_do_msg(p, &r, addr, buf, size, tbt_io);
        if (got <= 0) return got == 0 ? E_PROTO : got;
        if (got < size && got < ret) ret = got;
        size -= got;
        buf += got;
    }
    return ret;
}

struct I2cMsg { uint8_t addr; bool rd; uint8_t* buf; int len; };

// drm_dp_i2c_xfer: per message a bare address start (mot), the payload in
// chunks of up to 16 bytes (mot), and one bare address stop (no mot) at
// the end. returns 0 or a negative error. (satoru)
int i2c_xfer(const AuxPort& p, I2cMsg* m, int num, bool tbt_io) {
    int err = 0;
    uint8_t req = 0, addr = 0;
    for (int i = 0; i < num; i++) {
        addr = m[i].addr;
        req = (uint8_t)((m[i].rd ? I2C_READ : I2C_WRITE) | I2C_MOT);
        uint8_t r = req;
        err = i2c_do_msg(p, &r, addr, nullptr, 0, tbt_io);
        if (err < 0) break;
        int ts = 16;
        for (int j = 0; j < m[i].len; ) {
            int n = m[i].len - j;
            if (n > ts) n = ts;
            err = i2c_drain(p, req, addr, m[i].buf + j, n, tbt_io);
            if (err < 0) break;
            j += n;
            ts = err;
            if (ts <= 0) ts = 1;
        }
        if (err < 0) break;
    }
    uint8_t stop = (uint8_t)(req & ~I2C_MOT);
    (void)i2c_do_msg(p, &stop, addr, nullptr, 0, tbt_io);
    return err < 0 ? err : 0;
}

// one 128 byte edid block: segment pointer 0x30 (only for block >= 2),
// offset write and read at 0x50, like drm_do_probe_ddc_edid. (satoru)
int edid_block(const AuxPort& p, int block, uint8_t* out, bool tbt_io) {
    uint8_t seg = (uint8_t)(block / 2);
    uint8_t off = (uint8_t)((block & 1) * 128);
    I2cMsg m[3];
    int n = 0;
    if (seg) m[n++] = I2cMsg{ 0x30, false, &seg, 1 };
    m[n++] = I2cMsg{ 0x50, false, &off, 1 };
    m[n++] = I2cMsg{ 0x50, true, out, 128 };
    int err = 0;
    for (int tries = 0; tries < 3; tries++) {
        err = i2c_xfer(p, m, n, tbt_io);
        if (err == 0 || err == E_BUDGET || err == E_NOTSTARTED) break;
    }
    return err;
}

uint8_t g_edid[128 * DpEdid::MAX_BLOCKS];
DpEdid::Edid g_e;

struct PortResult {
    bool tried, dpcd_ok, edid_ok, edp;
    uint8_t dpcd[16];
    uint8_t sink_count;
};
PortResult g_res[NPORTS];

// what the last Run learned about each type-c sink, kept for stage 2
// (IntelDpAux::LastSink). plain copies, no pointers into g_edid. (satoru)
struct TcSink {
    bool valid;
    uint8_t dpcd[16];
    uint8_t sink_count;
    int edid_len;
    uint8_t edid[128 * DpEdid::MAX_BLOCKS];
};
TcSink g_tc[5];

void log_err(const char* port, const char* what, int e) {
    Ln l; l.t.s(port); l.t.c(' '); l.t.s(what); l.t.s(" failed: "); l.t.s(err_name(e));
    l.t.s(" (last aux_ctl 0x"); l.t.x(g_last_status, 8); l.t.s(")"); l.go();
}

void run_port(int idx, bool edp, bool tbt_io) {
    const AuxPort& p = PORTS[idx];
    PortResult& R = g_res[idx];
    R = PortResult();
    R.tried = true;
    R.edp = edp;
    g_xfers = 0;
    g_max_xfers = MAX_XFERS_PER_PORT;
    g_stat_timeouts = g_stat_defers = g_stat_rcverr = 0;
    g_deadline = g_tpu ? now_us() + PORT_BUDGET_US : 0;

    uint32_t c0 = rd(p.ctl);
    bool we_powered = false;
    {
        Ln l; l.t.s(p.name); l.t.s(" aux_ctl before 0x"); l.t.x(c0, 8);
        l.t.s(" pwr req/st="); l.t.d((c0 >> 19) & 1); l.t.c('/'); l.t.d((c0 >> 18) & 1);
        l.t.s(edp ? " (edp)" : ""); l.t.s(tbt_io ? " (tbt_io)" : "");
        l.go();
    }
    if (c0 & AUX_SEND_BUSY) {
        // another agent (firmware) is mid-transaction: hands off (satoru)
        udelay(3000);
        c0 = rd(p.ctl);
        if (c0 & AUX_SEND_BUSY) { Ln l; l.t.s(p.name); l.t.s(" aux channel stays busy, port skipped"); l.go(); return; }
    }
    if (!(c0 & AUX_PWR_REQ)) {
        if (!p.tc) { Ln l; l.t.s(p.name); l.t.s(" aux power is off on a combo port, not requested, port skipped"); l.go(); return; }
        // xelpdp_aux_power_well_enable: rmw set power_request, then a fixed
        // 600 us (we wait 1000) because the status bit is not trusted on
        // the c10/c20 phys. (satoru)
        wr_aux(p.ctl, (c0 & ~AUX_SEND_BUSY) | AUX_PWR_REQ);
        udelay(1000);
        we_powered = true;
        uint32_t c1 = rd(p.ctl);
        Ln l; l.t.s(p.name); l.t.s(" aux power requested (fw left it off): aux_ctl now 0x"); l.t.x(c1, 8);
        l.t.s(" pwr req/st="); l.t.d((c1 >> 19) & 1); l.t.c('/'); l.t.d((c1 >> 18) & 1); l.go();
    }

    // ---- dpcd receiver capability 0x000-0x00f (satoru) ----
    int r = dpcd_read(p, 0x000, R.dpcd, 16, tbt_io);
    if (r == 16) {
        R.dpcd_ok = true;
        hexdump(p.name, "dpcd", 0x000, R.dpcd, 16);
        DpEdid::Dpcd d;
        DpEdid::DecodeDpcd(R.dpcd, 16, &d);
        DpEdid::FormatDpcd(d, port_line, (void*)p.name);
        if (d.ext_caps) {
            // drm_dp_read_dpcd_caps: with the extended field present the
            // caps at 0x2200 are the real ones (dp 1.3+) (satoru)
            uint8_t ext[16];
            int re = dpcd_read(p, 0x2200, ext, 16, tbt_io);
            if (re == 16) {
                hexdump(p.name, "dpcd-ext", 0x2200, ext, 16);
                DpEdid::Dpcd de;
                DpEdid::DecodeDpcd(ext, 16, &de);
                Ln l; l.t.s(p.name); l.t.s(" dpcd-ext (effective caps):"); l.go();
                DpEdid::FormatDpcd(de, port_line, (void*)p.name);
                if (ext[0] >= R.dpcd[0]) for (int i = 0; i < 16; i++) R.dpcd[i] = ext[i];
            } else {
                log_err(p.name, "dpcd 0x2200 read", re);
            }
        }
        uint8_t sc[2] = {};
        if (dpcd_read(p, 0x200, sc, 2, tbt_io) == 2) {
            R.sink_count = (uint8_t)DpEdid::SinkCount(sc[0]);
            Ln l; l.t.s(p.name); l.t.s(" dpcd 0x200 sink_count 0x"); l.t.x(sc[0], 2); l.t.s(" -> ");
            l.t.d((uint64_t)R.sink_count); l.t.s(" cp_ready="); l.t.d((sc[0] >> 6) & 1);
            l.t.s(" | 0x201 irq_vector 0x"); l.t.x(sc[1], 2); l.go();
        }
        uint8_t lc[2] = {}, ls[6] = {}, pw = 0;
        bool lc_ok = dpcd_read(p, 0x100, lc, 2, tbt_io) == 2;
        bool ls_ok = dpcd_read(p, 0x202, ls, 6, tbt_io) == 6;
        bool pw_ok = dpcd_read(p, 0x600, &pw, 1, tbt_io) == 1;
        {
            Ln l; l.t.s(p.name);
            if (lc_ok) { l.t.s(" link_bw_set 0x"); l.t.x(lc[0], 2); l.t.s(" lane_count_set 0x"); l.t.x(lc[1], 2); }
            if (ls_ok) {
                l.t.s(" | lane_status 0x"); l.t.x(ls[0], 2); l.t.s(" 0x"); l.t.x(ls[1], 2);
                l.t.s(" align 0x"); l.t.x(ls[2], 2); l.t.s(" sink_status 0x"); l.t.x(ls[3], 2);
                // lane 0 cr_done bit 0, channel_eq bit 1, symbol_locked bit 2 (satoru)
                l.t.s(" (lane0 cr/eq/lock="); l.t.d(ls[0] & 1); l.t.c('/'); l.t.d((ls[0] >> 1) & 1); l.t.c('/'); l.t.d((ls[0] >> 2) & 1);
                l.t.s(" interlane_align="); l.t.d(ls[2] & 1); l.t.c(')');
            }
            if (pw_ok) {
                l.t.s(" | set_power 0x"); l.t.x(pw, 2);
                l.t.s((pw & 7) == 1 ? " (d0)" : (pw & 7) == 2 ? " (d3)" : (pw & 7) == 5 ? " (d3 aux on)" : " (?)");
            }
            l.go();
        }
        if (R.dpcd[6] & 2) {
            // dp_128b132b_supported_link_rates 0x2215: bit 0 uhbr10, bit 1
            // uhbr20, bit 2 uhbr13.5 (drm_dp.h) (satoru)
            uint8_t u = 0;
            if (dpcd_read(p, 0x2215, &u, 1, tbt_io) == 1) {
                Ln l; l.t.s(p.name); l.t.s(" dpcd 0x2215 128b132b rates 0x"); l.t.x(u, 2);
                l.t.s(" uhbr10="); l.t.d(u & 1); l.t.s(" uhbr13.5="); l.t.d((u >> 2) & 1); l.t.s(" uhbr20="); l.t.d((u >> 1) & 1);
                l.go();
            }
        }
        if (edp) {
            // dp_edp_dpcd_rev 0x700 and dp_supported_link_rates 0x010, 8 x
            // 16 bit le in 200 khz units (edp 1.4, drm_dp.h) (satoru)
            uint8_t er = 0, rates[16] = {};
            if (dpcd_read(p, 0x700, &er, 1, tbt_io) == 1) {
                Ln l; l.t.s(p.name); l.t.s(" edp dpcd rev 0x"); l.t.x(er, 2); l.go();
            }
            if (dpcd_read(p, 0x010, rates, 16, tbt_io) == 16) {
                hexdump(p.name, "dpcd", 0x010, rates, 16);
                Ln l; l.t.s(p.name); l.t.s(" edp supported link rates (khz):");
                int nr = 0;
                for (int i = 0; i < 8; i++) {
                    uint32_t v = (uint32_t)rates[i * 2] | ((uint32_t)rates[i * 2 + 1] << 8);
                    if (!v) break;
                    l.t.c(' '); l.t.d((uint64_t)v * 200); nr++;
                }
                if (!nr) l.t.s(" none (uses max_link_rate)");
                l.go();
            }
        }
    } else {
        log_err(p.name, "dpcd 0x000 read", r);
    }

    // ---- edid over i2c-over-aux (satoru) ----
    int e = edid_block(p, 0, g_edid, tbt_io);
    int blocks = 0;
    if (e == 0) {
        blocks = 1;
        int ext = g_edid[126];
        if (ext > DpEdid::MAX_BLOCKS - 1) {
            Ln l; l.t.s(p.name); l.t.s(" edid declares "); l.t.d((uint64_t)ext); l.t.s(" extensions, reading the first 3"); l.go();
            ext = DpEdid::MAX_BLOCKS - 1;
        }
        for (int b = 1; b <= ext; b++) {
            int eb = edid_block(p, b, g_edid + b * 128, tbt_io);
            if (eb != 0) { Ln l; l.t.s(p.name); l.t.s(" edid block "); l.t.d((uint64_t)b); l.t.s(" read failed: "); l.t.s(err_name(eb)); l.go(); break; }
            blocks++;
        }
        for (int b = 0; b < blocks; b++) {
            char w[12]; Txt tw(w, sizeof(w)); tw.s("edid blk"); tw.d((uint64_t)b);
            hexdump(p.name, w, 0, g_edid + b * 128, 128);
        }
        if (DpEdid::DecodeEdid(g_edid, blocks * 128, &g_e)) {
            R.edid_ok = true;
            DpEdid::FormatEdid(g_e, port_line, (void*)p.name);
        } else {
            Ln l; l.t.s(p.name); l.t.s(" edid block 0 has a bad header, not decoded"); l.go();
        }
    } else {
        log_err(p.name, "edid block 0 read", e);
    }

    if (p.tc >= 1 && p.tc <= 4) {
        TcSink& s = g_tc[p.tc];
        s.valid = R.dpcd_ok;
        for (int i = 0; i < 16; i++) s.dpcd[i] = R.dpcd[i];
        s.sink_count = R.sink_count;
        s.edid_len = R.edid_ok ? blocks * 128 : 0;
        for (int i = 0; i < s.edid_len; i++) s.edid[i] = g_edid[i];
    }

    {
        Ln l; l.t.s(p.name); l.t.s(" aux stats: xfers "); l.t.d((uint64_t)g_xfers);
        l.t.s(" hw_timeouts "); l.t.d((uint64_t)g_stat_timeouts); l.t.s(" defers "); l.t.d((uint64_t)g_stat_defers);
        l.t.s(" rcv_errors "); l.t.d((uint64_t)g_stat_rcverr);
        l.t.s(budget_left() ? "" : " (budget hit)");
        l.go();
    }

    if (we_powered) {
        // xelpdp_aux_power_well_disable: rmw clear power_request, 10-30 us,
        // back to the state the firmware left (satoru)
        uint32_t c = rd(p.ctl);
        for (int i = 0; i < 10 && (c & AUX_SEND_BUSY); i++) { udelay(1000); c = rd(p.ctl); }
        if (!(c & AUX_SEND_BUSY)) {
            wr_aux(p.ctl, c & ~(AUX_SEND_BUSY | AUX_PWR_REQ));
            udelay(30);
        }
        uint32_t c2 = rd(p.ctl);
        Ln l; l.t.s(p.name); l.t.s(" aux power request released: aux_ctl 0x"); l.t.x(c2, 8); l.go();
    }

    // ---- per-port summary (satoru) ----
    {
        Ln l; l.t.s(p.name); l.t.s(edp ? " sink (edp): " : " sink: ");
        if (R.edid_ok) {
            l.t.s(g_e.mfg); l.t.c(' ');
            if (g_e.name[0]) l.t.s(g_e.name); else { l.t.s("product 0x"); l.t.x(g_e.product, 4); }
            if (g_e.preferred >= 0) {
                char tb[160];
                l.t.s(" preferred ");
                DpEdid::FormatTiming(g_e.t[g_e.preferred], tb, sizeof(tb));
                // just the wxh@hz and the clock for the summary (satoru)
                int k = 0; int sp = 0;
                while (tb[k] && sp < 3) { if (tb[k] == ' ') sp++; if (sp < 3) l.t.c(tb[k]); k++; }
                int best = DpEdid::MaxRefreshAtPreferred(g_e);
                if (best >= 0 && best != g_e.preferred) {
                    const DpEdid::Timing& bt = g_e.t[best];
                    l.t.s(" | max at native "); l.t.d(bt.hact); l.t.c('x'); l.t.d(bt.vact); l.t.c('@'); l.t.hz(bt.refresh_mhz);
                    l.t.s(" pixclk "); l.t.d(bt.pixclk_khz); l.t.s("khz");
                }
            }
        } else {
            l.t.s("no edid");
        }
        if (R.dpcd_ok) {
            l.t.s(" | dpcd "); l.t.d(R.dpcd[0] >> 4); l.t.c('.'); l.t.d(R.dpcd[0] & 0xF);
            l.t.c(' '); l.t.s(DpEdid::LinkRateName(R.dpcd[1])); l.t.s(" x"); l.t.d(R.dpcd[2] & 0x1F);
            l.t.s(" sink_count "); l.t.d(R.sink_count);
        } else {
            l.t.s(" | no dpcd");
        }
        l.go();
    }
}

} // namespace

int IntelDpAux::Run(char* out, int maxo) {
    g_out = out;
    g_out_max = maxo;
    g_out_pos = 0;
    if (out && maxo > 0) out[0] = 0;
    for (int i = 0; i < 5; i++) g_tc[i].valid = false;

    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) {
        say("no usable intel display ver 14 engine from the stage 0 probe (see [igpu] lines), aux skipped");
        return g_out_pos;
    }
    clock_init();
    say("---- intel dp aux, stage 1: dpcd + edid reads, no modeset ----");
    {
        Ln l; l.t.s("clock: "); if (g_tpu) { l.t.s("tsc "); l.t.d(g_tpu); l.t.s(" ticks/us"); } else l.t.s("io-delay fallback (no tsc frequency)");
        l.go();
    }

    // fresh gates every run: pw_1 up and dc states off (an aux transfer
    // with dc5/dc6 allowed would need a dc_state_en write first, which is
    // not ours to make) (satoru)
    uint32_t pw = rd(R_PWR_WELL_CTL2);
    uint32_t dc = rd(R_DC_STATE_EN);
    if (!(pw & 1)) { say("pw1 reports off now, aux skipped"); return g_out_pos; }
    if (dc & 3) {
        Ln l; l.t.s("dc_state_en 0x"); l.t.x(dc, 8); l.t.s(" allows dc5/dc6, aux skipped (would need a dc state write)"); l.go();
        return g_out_pos;
    }

    uint32_t sde = rd(R_SDEISR), pica = rd(R_PICA_ISR);
    uint32_t pps = rd(R_PP_STATUS0);
    uint32_t psr1 = rd(R_SRD_CTL_A), psr2 = rd(R_PSR2_CTL_A), dp2 = rd(R_TRANS_DP2_CTL_A);
    {
        Ln l; l.t.s("gates: sdeisr 0x"); l.t.x(sde, 8); l.t.s(" pica_isr 0x"); l.t.x(pica, 8);
        l.t.s(" pp_status0 0x"); l.t.x(pps, 8); l.t.s(" srd_ctl_a 0x"); l.t.x(psr1, 8);
        l.t.s(" psr2_ctl_a 0x"); l.t.x(psr2, 8); l.t.s(" trans_dp2_ctl_a 0x"); l.t.x(dp2, 8);
        l.go();
    }

    int external = -1;
    for (int i = 0; i < NPORTS; i++) {
        const AuxPort& p = PORTS[i];
        g_res[i] = PortResult();
        uint32_t aux = rd(p.ctl);
        bool pwr_st = aux & AUX_PWR_ST, pwr_req = aux & AUX_PWR_REQ;
        Ln l; l.t.s(p.name); l.t.s(": ");
        if (!p.tc) {
            bool live = (sde >> (16 + p.pin)) & 1;
            bool edp = i == 0 && (pps >> 31);
            l.t.s("hpd live="); l.t.d(live); l.t.s(" aux pwr req/st="); l.t.d(pwr_req); l.t.c('/'); l.t.d(pwr_st);
            if (edp) l.t.s(" edp(pps0 on)");
            if (!pwr_req || !pwr_st) { l.t.s(" -> skip: combo aux power off (not ours to request)"); l.go(); continue; }
            if (!edp && !live) { l.t.s(" -> skip: no live hpd"); l.go(); continue; }
            if (edp && ((psr1 >> 31) || (psr2 >> 31) || ((dp2 >> 30) & 1))) {
                l.t.s(" -> skip: psr/panel replay active on transcoder a, it shares this aux channel"); l.go(); continue;
            }
            l.t.s(" -> read"); l.go();
            run_port(i, edp, false);
        } else {
            int pin = p.pin;
            uint32_t b1 = rd(p.buf1);
            uint32_t ts = rd(R_TCSS_DDI_STATUS + (uint32_t)pin * 4);
            bool dpalt = (pica >> (16 + pin)) & 1, tbt = (pica >> pin) & 1, legacy = (sde >> (24 + pin)) & 1;
            bool owned = b1 & BUF1_TC_OWNED, tcss = b1 & BUF1_TCSS_PWR_ST, rdy = b1 & BUF1_PHY_READY;
            l.t.s("hpd dp-alt="); l.t.d(dpalt); l.t.s(" tbt="); l.t.d(tbt); l.t.s(" legacy="); l.t.d(legacy);
            l.t.s(" | buf_ctl1 0x"); l.t.x(b1, 8); l.t.s(" owned="); l.t.d(owned); l.t.s(" tcss_pwr="); l.t.d(tcss);
            l.t.s(" phy_rdy="); l.t.d(rdy);
            l.t.s(" | tcss_ddi_status 0x"); l.t.x(ts, 8);
            if (ts != 0xFFFFFFFFu) {
                l.t.s(" ready="); l.t.d((ts >> 2) & 1); l.t.s(" alt="); l.t.d(ts & 1); l.t.s(" tbt="); l.t.d((ts >> 1) & 1);
            } else {
                l.t.s(" (tccold)");
            }
            l.t.s(" | aux pwr req/st="); l.t.d(pwr_req); l.t.c('/'); l.t.d(pwr_st);
            if (!dpalt && !tbt && !legacy) { l.t.s(" -> skip: nothing live"); l.go(); continue; }
            if (tbt && !dpalt) {
                l.t.s(" -> skip: tbt-alt only (needs the tc-cold-off domain, out of stage 1 scope)"); l.go(); continue;
            }
            if (!owned || !tcss || !rdy) {
                // a legacy or dp-alt connect would need xelpdp_tc_phy_connect
                // (tcss power request + ownership writes to port_buf_ctl1),
                // which stage 1 does not do. a bare sdeisr legacy bit only
                // counts for a vbt legacy port anyway. (satoru)
                l.t.s(" -> skip: phy not owned/powered by display (connect would write port_buf_ctl1)"); l.go(); continue;
            }
            l.t.s(dpalt ? " -> read (dp-alt, fw owns + powers the phy)" : " -> read (legacy, fw owns + powers the phy)"); l.go();
            run_port(i, false, false);
            if (external < 0 && g_res[i].dpcd_ok) external = i;
        }
    }

    // ---- final summary (satoru) ----
    int tried = 0;
    for (int i = 0; i < NPORTS; i++) if (g_res[i].tried) tried++;
    {
        Ln l; l.t.s("summary: aux tried on "); l.t.d((uint64_t)tried); l.t.s(" port(s):");
        for (int i = 0; i < NPORTS; i++) {
            if (!g_res[i].tried) continue;
            l.t.c(' '); l.t.s(PORTS[i].name); l.t.s("(dpcd="); l.t.s(g_res[i].dpcd_ok ? "ok" : "fail");
            l.t.s(" edid="); l.t.s(g_res[i].edid_ok ? "ok" : "fail"); l.t.c(')');
        }
        if (external >= 0) { l.t.s(" | external dp sink on "); l.t.s(PORTS[external].name); }
        else l.t.s(" | no external dp sink answered");
        l.go();
    }
    say("---- end of intel dp aux ----");
    if (out && g_out_pos >= maxo - 2)
        SerialLogger::Log("[igpu-aux] terminal output truncated, full report is in the serial log\r\n");
    return g_out_pos;
}

// ---- stage 2 api (intel_modeset.cpp). type-c ports only. (satoru) ----

namespace {
const AuxPort* tc_port(int tc) {
    for (int i = 0; i < NPORTS; i++) if (PORTS[i].tc == tc && tc >= 1) return &PORTS[i];
    return nullptr;
}
}

bool IntelDpAux::LastSink(int tc, SinkInfo* out) {
    if (tc < 1 || tc > 4 || !out) return false;
    const TcSink& s = g_tc[tc];
    if (!s.valid) return false;
    for (int i = 0; i < 16; i++) out->dpcd[i] = s.dpcd[i];
    out->sink_count = s.sink_count;
    out->edid_len = s.edid_len;
    out->edid = s.edid;
    return true;
}

bool IntelDpAux::BeginSession(int tc, uint32_t budget_us, int max_xfers) {
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio || !tc_port(tc)) return false;
    clock_init();
    g_out = nullptr;
    g_xfers = 0;
    g_max_xfers = max_xfers > 0 ? max_xfers : MAX_XFERS_PER_PORT;
    g_stat_timeouts = g_stat_defers = g_stat_rcverr = 0;
    g_deadline = g_tpu ? now_us() + budget_us : 0;
    return true;
}

int IntelDpAux::DpcdRead(int tc, uint32_t addr, uint8_t* buf, int n) {
    const AuxPort* p = tc_port(tc);
    if (!p || !g_mmio || n <= 0) return -1;
    // chunks of 16, the drm_dp_dpcd_read_data block size (satoru)
    int done = 0;
    while (done < n) {
        int c = n - done > 16 ? 16 : n - done;
        int r = dpcd_read(*p, addr + (uint32_t)done, buf + done, c, false);
        if (r != c) return r < 0 ? r : -1;
        done += c;
    }
    return done;
}

int IntelDpAux::DpcdWrite(int tc, uint32_t addr, const uint8_t* buf, int n) {
    const AuxPort* p = tc_port(tc);
    if (!p || !g_mmio || n <= 0 || n > 16) return -1;
    int r = dpcd_write(*p, addr, buf, n, false);
    return r == n ? n : (r < 0 ? r : -1);
}

bool IntelDpAux::AuxPower(int tc, bool on, bool* was_on) {
    // xelpdp_aux_power_well_enable / _disable on the port's own aux ctl:
    // rmw power_request (bit 19), 600 us settle (we wait 1000), send_busy
    // masked so a transfer is never triggered by this write (satoru)
    const AuxPort* p = tc_port(tc);
    g_mmio = IntelDisplayProbe::Mmio();
    if (!p || !g_mmio) return false;
    clock_init();
    uint32_t c = rd(p->ctl);
    if (was_on) *was_on = (c & AUX_PWR_REQ) != 0;
    for (int i = 0; i < 10 && (c & AUX_SEND_BUSY); i++) { udelay(1000); c = rd(p->ctl); }
    if (c & AUX_SEND_BUSY) return false;
    if (on && !(c & AUX_PWR_REQ)) { wr_aux(p->ctl, (c & ~AUX_SEND_BUSY) | AUX_PWR_REQ); udelay(1000); }
    if (!on && (c & AUX_PWR_REQ)) { wr_aux(p->ctl, c & ~(AUX_SEND_BUSY | AUX_PWR_REQ)); udelay(30); }
    return true;
}

const char* IntelDpAux::ErrName(int e) { return err_name(e); }

// end (satoru)
