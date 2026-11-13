// i915 register model group 5 (d4.1): ports, cx0 phys, transcoders and
// pipes of display ver 14, for a modeset up to the port pll enable.
//  - XELPDP_PORT_BUF_CTL1/2: tcss power, soc phy ready, d2d link, lane
//    reset and powerdown updates complete at once; phy idle follows
//    DDI_BUF_CTL enable.
//  - XELPDP_PORT_CLOCK_CTL: refclk and pll acknowledge = request (lanes 0, 1),
//    tbt clock ack = request.
//  - M2P/P2M message bus: per port and lane a 4 KiB phy register file
//    (c10 and c20 registers alike), c20 sram through its indirect address /
//    data registers; read and committed write responses as the protocol says.
//  - type-c: TCSS_DDI_STATUS ready, FIA DFLEXPA1 4 dp lanes, PICA live status
//    of the dp-alt sink on tc1 (variants edp, tc).
//  - transcoder state follows enable, pipe scanline and frame counter run
//    while the transcoder is on (60 Hz timing), DP_TP_STATUS idle/act done.
//  - PP_STATUS follows PP_CONTROL power on (edp panel power sequencer).
// what this does NOT prove: pll lock, phy calibration, link training, real
// timings. survey: scratch/kld/gfx/survey_regs.md section 5-7. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"

u32 i915m_peek(unsigned long off);
void i915m_poke(unsigned long off, u32 v);

struct mport { unsigned long buf1, m2p, clk, ddi_buf; };
static const struct mport ports[] = {
    { 0x64004,  0x64040,  0x640e0,  0x64000 },   // a (c10)
    { 0x64104,  0x64140,  0x641e0,  0x64100 },   // b (c10)
    { 0x16f200, 0x16f240, 0x16f260, 0x64300 },   // tc1 (c20)
    { 0x16f400, 0x16f440, 0x16f460, 0x64400 },   // tc2
    { 0x16f600, 0x16f640, 0x16f660, 0x64500 },   // tc3
    { 0x16f800, 0x16f840, 0x16f860, 0x64600 },   // tc4
};
#define NPORTS 6

static u8 *g_phy[NPORTS][2];          // phy register file per lane (satoru)
static u16 *g_sram[NPORTS];           // c20 sram (tc ports) (satoru)
static int g_tc_sink;

void i915_model_phy_init(const char *variant)
{
    g_tc_sink = variant && (variant[0] == 'e' || (variant[0] == 't' && variant[1] == 'c' && !variant[2]));
}

static u8 *phy(int p, int lane)
{
    if (!g_phy[p][lane]) {
        g_phy[p][lane] = lx_os_malloc(4096);
        if (g_phy[p][lane]) memset(g_phy[p][lane], 0, 4096);
    }
    return g_phy[p][lane];
}

static u16 *sram(int p)
{
    if (!g_sram[p]) {
        g_sram[p] = lx_os_malloc(65536 * 2);
        if (g_sram[p]) memset(g_sram[p], 0, 65536 * 2);
    }
    return g_sram[p];
}

static u32 mirror(u32 v, int from, int to)
{
    return (v & ~(1u << to)) | (((v >> from) & 1u) << to);
}

// ── transcoders and pipes ───────────────────────────────────────────────────
static int trans_on(int t) { return (i915m_peek(0x70008 + 0x1000 * t) >> 31) & 1; }
u32 i915_model_frame(int pipe);

static int pipe_read(unsigned long off, u32 *v)
{
    unsigned long t = (off - 0x60000) >> 12;
    if (off >= 0x70000 && off < 0x74000) {
        unsigned long r = off & 0xfff;
        int p = (int)((off - 0x70000) >> 12);
        u64 now = lx_uptime_ns();
        if (r == 0x008) { *v = mirror(i915m_peek(off), 31, 30); return 1; }      // transconf state
        if (r == 0x000) {                                    // pipedsl: position in the frame
            u32 vt = ((i915m_peek(0x6000c + 0x1000 * p) >> 16) & 0xffff) + 1;
            if (vt < 2) vt = 1125;
            *v = trans_on(p) ? (u32)((now % 16666667) * vt / 16666667) : 0;
            return 1;
        }
        if (r == 0x040) { *v = i915_model_frame(p); return 1; }   // frame count (vblank thread)
    }
    if (off >= 0x60000 && off < 0x64000 && (off & 0xfff) == 0x544 && t < 4) {
        *v = i915m_peek(off) | (1u << 25) | (1u << 24);   // dp_tp_status idle done, act sent
        return 1;
    }
    return 0;
}

// ── ports ──────────────────────────────────────────────────────────────────
static int port_of(unsigned long off, unsigned long base_off, unsigned long span)
{
    for (int i = 0; i < NPORTS; i++) {
        unsigned long b = *(const unsigned long *)((const char *)&ports[i] + base_off);
        if (off >= b && off < b + span)
            return i;
    }
    return -1;
}

int i915_model_phy_read(unsigned long off, u32 *v)
{
    int p;
    u32 s;
    if (pipe_read(off, v))
        return 1;
    if ((p = port_of(off, offsetof(struct mport, buf1), 12)) >= 0) {
        s = i915m_peek(off);
        switch (off - ports[p].buf1) {
        case 0:   // buf_ctl1
            s = mirror(s, 5, 4) | (1u << 24);                   // tcss power, soc phy ready
            s = mirror(s, 29, 28);                              // d2d link state
            s = (s & ~(1u << 7)) | ((i915m_peek(ports[p].ddi_buf) >> 31) ? 0 : (1u << 7));   // phy idle
            break;
        case 4:   // buf_ctl2
            s = mirror(mirror(s, 31, 29), 30, 28) & ~((1u << 25) | (1u << 24));
            break;
        }
        *v = s;
        return 1;
    }
    if ((p = port_of(off, offsetof(struct mport, clk), 4)) >= 0) {
        s = i915m_peek(off);
        *v = (s & ~0x55000000u) | ((s & 0xaa000000u) >> 1);    // refclk / pll ack (satoru)
        *v = mirror(*v, 19, 18);                               // tbt clock ack
        return 1;
    }
    if ((p = port_of(off, offsetof(struct mport, m2p), 16)) >= 0) {
        unsigned long r = off - ports[p].m2p;
        if (r < 8) { *v = i915m_peek(off) & ~((1u << 31) | (1u << 15)); return 1; }   // pending/reset done
        return 0;   // p2m status: as stored by the write handler (satoru)
    }
    if (off >= 0x161500 && off < 0x161510) { *v = 0x4; return 1; }     // tcss ddi status ready
    if (off == 0x163880 || off == 0x16e880) { *v = 0x33; return 1; }   // dflexpa1: 4 dp lanes, both tcs of the fia
    if (off == 0x16fe50) { *v = g_tc_sink ? (1u << 16) : 0; return 1; }   // pica isr: dp-alt hpd tc1
    if (off == 0xc7200) {                                                // pp_status
        *v = (i915m_peek(0xc7204) & 1) ? 0xc0000008u : 0;
        return 1;
    }
    return 0;
}

// m2p write: run the message on the lane's phy register file. (satoru)
static void msgbus(int p, int lane, u32 v)
{
    unsigned long st = ports[p].m2p + 8 + 4 * lane;
    u32 cmd = (v >> 27) & 0xf, addr = v & 0xfff, data = (v >> 16) & 0xff;
    u8 *r = phy(p, lane);
    if (!r)
        return;
    if (cmd == 3) {                                   // read
        i915m_poke(st, (1u << 31) | (4u << 27) | ((u32)r[addr] << 16));
        return;
    }
    if (cmd == 1 || cmd == 2) {                       // write (uncommitted / committed)
        r[addr] = (u8)data;
        if (p >= 2) {                                 // c20 indirect sram access
            u16 *sr = sram(p);
            u16 wa = (u16)((r[0xc03] << 8) | r[0xc02]);
            u16 ra = (u16)((r[0xc07] << 8) | r[0xc06]);
            if (sr && addr == 0xc04)
                sr[wa] = (u16)((r[0xc05] << 8) | r[0xc04]);
            if (sr && addr == 0xc06) {
                r[0xc08] = (u8)sr[ra];
                r[0xc09] = (u8)(sr[ra] >> 8);
            }
        }
        if (cmd == 2)
            i915m_poke(st, (1u << 31) | (5u << 27));
    }
}

int i915_model_phy_write(unsigned long off, u32 *v)
{
    int p = port_of(off, offsetof(struct mport, m2p), 16);
    if (p >= 0) {
        unsigned long r = off - ports[p].m2p;
        if (r < 8 && (*v & (1u << 31))) {
            msgbus(p, (int)(r / 4), *v);
            *v &= ~(1u << 31);
            return 1;
        }
        if (r < 8 && (*v & (1u << 15))) {             // transaction reset
            *v &= ~(1u << 15);
            return 1;
        }
        if (r >= 8) {                                 // p2m status: w1c ready, error
            *v = i915m_peek(off) & ~(*v & ((1u << 31) | (1u << 15)));
            return 1;
        }
    }
    return 0;
}

// end (satoru)
