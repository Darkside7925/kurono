// host unit test for the stage 2 maths and the dp link training state
// machine. not part of the kernel build. run from src/drivers:
//   g++ -std=c++17 -O1 -Wall -o /tmp/igpu2_test intel_modeset_hosttest.cpp
//       intel_modeset_calc.cpp intel_dp_lt.cpp dp_edid_decode.cpp && /tmp/igpu2_test
// expected values are recomputed here straight from the linux formulas
// (or worked by hand in the comments), not copied from the code under
// test. (satoru)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include "intel_modeset_calc.h"
#include "intel_dp_lt.h"
#include "dp_edid_decode.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace IgpuCalc;

static void test_c20() {
    // intel_c20pll_calc_port_clock on each mtl_c20_dp table must give the
    // table's own rate (linux warns in intel_c20_pll_find_table otherwise) (satoru)
    const uint32_t rates[] = { 162000, 270000, 540000, 810000 };
    for (uint32_t r : rates) {
        const C20Pll* p = C20DpTable(r);
        CHECK(p != nullptr, "table for %u", r);
        if (!p) continue;
        CHECK(C20PortClock(*p) == r, "port clock %u != %u", C20PortClock(*p), r);
        CHECK(p->clock_khz == r, "table rate");
        CHECK(C20UsesMpllb(*p), "dp1.4 uses mpllb");
    }
    // hbr1 by hand: mult 0x08c = 140, tx_clk_div 1, frac 0x5000, ref 76800:
    // ((140 << 15) + 20480) * 76800 >> 17 = 2700000, /10 = 270000, << 1 >> 1 (satoru)
    CHECK(C20DpTable(300000) == nullptr, "no table for a non dp1.4 rate");
    CHECK(C20DpRate(270000) == 1 && C20DpRate(540000) == 2 && C20DpRate(810000) == 3 && C20DpRate(162000) == 0, "dp rate codes");
    CHECK(LinkSymbolClock(270000) == 270000 && LinkSymbolClock(810000) == 810000, "symbol clock");
}

static void test_mode_and_link() {
    // the aoc edid 60 hz timing: 262430 khz, h 2560 48 32 80 (2720), v 1440 3 5 160 (1608) (satoru)
    Mode m;
    CHECK(ModeFromDtd(262430, 2560, 160, 48, 32, 1440, 168, 3, 5, true, true, false, &m), "dtd ok");
    CHECK(m.htotal == 2720 && m.vtotal == 1608 && m.hsync_start == 2608 && m.hsync_end == 2640, "h timings %u %u %u", m.htotal, m.hsync_start, m.hsync_end);
    CHECK(m.vsync_start == 1443 && m.vsync_end == 1448, "v sync");
    CHECK(m.refresh_mhz == 60001, "refresh %u", m.refresh_mhz);
    // data rate: 262430 * 24 / 8 = 787290 kB/s (satoru)
    CHECK(ModeDataRate(262430, 24) == 787290, "data rate %u", ModeDataRate(262430, 24));
    // 2560 px * 24 bpp / (8 * 4 lanes) = 1920 symbol cycles exactly -> no overhead (satoru)
    CHECK(LinkRequired(4, 2560, 262430, 24) == 787290, "link required %u", LinkRequired(4, 2560, 262430, 24));
    CHECK(LinkDataRate(270000, 4) == 1080000 && LinkDataRate(162000, 4) == 648000, "link data rates");
    LinkCfg c[12];
    int n = LinkConfigs(810000, 4, 262430, 2560, 24, c, 12);
    CHECK(n >= 1 && c[0].rate_khz == 270000 && c[0].lanes == 4, "first config hbr x4, got %u x%d", n ? c[0].rate_khz : 0, n ? c[0].lanes : 0);
    n = LinkConfigs(810000, 2, 262430, 2560, 24, c, 12);
    CHECK(n >= 1 && c[0].rate_khz == 540000 && c[0].lanes == 2, "pin d: hbr2 x2");
    // 144 hz: 592370 khz needs 1777110 kB/s: hbr3 x2 (1620000) is short (satoru)
    n = LinkConfigs(810000, 4, 592370, 2560, 24, c, 12);
    CHECK(n >= 1 && c[0].rate_khz == 540000 && c[0].lanes == 4, "144 hz first config hbr2 x4");
    bool has_hbr3x2 = false;
    for (int i = 0; i < n; i++) if (c[i].rate_khz == 810000 && c[i].lanes == 2) has_hbr3x2 = true;
    CHECK(!has_hbr3x2, "hbr3 x2 cannot carry 144 hz");
    CHECK(RateToBwCode(270000) == 0x0A && RateToBwCode(810000) == 0x1E && BwCodeToRate(0x14) == 540000, "bw codes");

    // m/n, recomputed from intel_link_compute_m_n / compute_m_n (satoru)
    MN mn;
    ComputeMN(24, 4, 262430, 270000, &mn);
    uint64_t dm = (uint64_t)787290 * 0x8000000ull / 1080000ull;
    uint32_t dn = 0x8000000;
    while (dm > 0xFFFFFF || dn > 0xFFFFFF) { dm >>= 1; dn >>= 1; }
    uint64_t lm = (uint64_t)262430 * 0x80000ull / 270000ull;
    CHECK(mn.tu == 64 && mn.data_m == dm && mn.data_n == dn, "data m/n %u/%u want %llu/%u", mn.data_m, mn.data_n, (unsigned long long)dm, dn);
    CHECK(mn.link_m == lm && mn.link_n == 0x80000, "link m/n %u/%u", mn.link_m, mn.link_n);
    CHECK(DataMReg(mn) == ((63u << 25) | mn.data_m), "data_m reg");
    printf("info: hbr x4 60 hz m/n data %u/%u (%.6f) link %u/%u (%.6f)\n", mn.data_m, mn.data_n, (double)mn.data_m / mn.data_n,
           mn.link_m, mn.link_n, (double)mn.link_m / mn.link_n);

    TransRegs tr;
    TranscoderRegs(m, &tr);
    CHECK(tr.htotal == ((2719u << 16) | 2559u), "htotal reg %08x", tr.htotal);
    CHECK(tr.hblank == ((2719u << 16) | 2559u), "hblank reg");
    CHECK(tr.hsync == ((2639u << 16) | 2607u), "hsync reg %08x", tr.hsync);
    CHECK(tr.vtotal == ((1607u << 16) | 1439u), "vtotal reg");
    CHECK(tr.vblank == (1607u << 16), "vblank reg (start 1 on ver >= 13)");
    CHECK(tr.vsync == ((1447u << 16) | 1442u), "vsync reg %08x", tr.vsync);
    CHECK(tr.pipesrc == ((2559u << 16) | 1439u), "pipesrc");
}

static void test_levels_ggtt_misc() {
    CHECK(TrainSetToLevel(0x00) == 0, "vs0 pe0");
    CHECK(TrainSetToLevel(0x0A) == 8, "vs2 pe1");
    CHECK(TrainSetToLevel(0x03) == 9, "vs3 pe0");
    CHECK(TrainSetToLevel(0x05) == 4, "vs1 + max swing flag");
    CHECK(TrainSetToLevel(0x0B) == -1, "vs3 pe1 invalid");
    Snps s = C20Dp14Level(9);
    CHECK(s.vswing == 48 && s.pre_cursor == 0 && s.post_cursor == 0, "preset 9");
    s = C20Dp14Level(3);
    CHECK(s.vswing == 34 && s.post_cursor == 14, "preset 3");
    // mtl_ggtt_pte_encode(addr, pat 2): present bit 0, pat1 bit 53 (satoru)
    CHECK(MtlGgttPte(0x123456000ull, 2) == 0x0020000123456001ull, "pte %llx", (unsigned long long)MtlGgttPte(0x123456000ull, 2));
    CHECK(MtlGgttPte(0x1000ull, 3) == ((1ull << 52) | (1ull << 53) | 0x1001ull), "pte pat 3");
    CHECK(MtlGgttPte(0x123456789ull, 0) == 0x123456001ull, "pte drops low bits");
    // the zero-copy window live-pte rule: present + address inside ram, with
    // only lm/pat allowed on top. an absent pte, a stale non-pte value and an
    // address above the managed top are not live mappings. (satoru)
    const uint64_t top = 0x100000000ull;
    CHECK(GgttPteLive(MtlGgttPte(0x800000ull, MTL_PAT_UC), top), "lm scanout pte is live");
    CHECK(GgttPteLive(MtlGgttPte(0x1000ull, 0), top), "pte without pat is live");
    CHECK(!GgttPteLive(0, top), "absent pte is not live");
    CHECK(!GgttPteLive(MtlGgttPte(0x800000ull, MTL_PAT_UC) & ~1ull, top), "not present");
    CHECK(!GgttPteLive(MtlGgttPte(0x800000ull, MTL_PAT_UC) | (1ull << 3), top), "foreign bits are stale");
    CHECK(!GgttPteLive(MtlGgttPte(0x100000000ull, MTL_PAT_UC), top), "above ram top is stale");
    CHECK(DdbRegVal(0, 2016) == 0x07DF0000u && DdbRegVal(0, 0) == 0, "ddb reg");
    CHECK(LinetimeWm(2720, 262430) == 83, "linetime %u", LinetimeWm(2720, 262430));
    CHECK(MbusDboxCtl(false, true) == 0x0103CA48u, "dbox %08x", MbusDboxCtl(false, true));
    CHECK(MbusDboxCtl(true, false) == ((0x0103CA48u & ~0xFu & ~(3u << 14)) | 12u | (2u << 14)), "dbox joined");
    PmDemand pd = { (1u << 6) | (2u << 4) | 1u | (0x1234u << 16), (810u << 8) | (0x200u << 20) | 2u };
    bool ch = PmDemandRaise(&pd, 2, 4, 270);
    CHECK(ch && ((pd.reg0 >> 6) & 3) == 2 && ((pd.reg0 >> 4) & 3) == 3 && (pd.reg0 >> 16) == 0x1234 && (pd.reg0 & 7) == 1, "pmdemand reg0 %08x", pd.reg0);
    CHECK(((pd.reg1 >> 8) & 0x7FF) == 810 && ((pd.reg1 >> 20) & 0x7FF) == 0x200 && (pd.reg1 & 7) == 2, "pmdemand reg1 keeps the bigger ddiclk");
    PmDemand pd2 = pd;
    CHECK(!PmDemandRaise(&pd2, 1, 1, 100) && pd2.reg0 == pd.reg0 && pd2.reg1 == pd.reg1, "pmdemand never lowers");
    CHECK(LttprCount(0x80) == 1 && LttprCount(0x40) == 2 && LttprCount(0x01) == 8 && LttprCount(0) == 0, "lttpr counts");
    CHECK(LttprCount(0xFF) < 0 && LttprCount(0x03) < 0, "lttpr bad counts");
    CHECK(CrDelayUs(0x14, 4, false) == 100 && CrDelayUs(0x12, 1, false) == 4000 && CrDelayUs(0x12, 0, false) == 100 && CrDelayUs(0x12, 3, true) == 100, "cr delays");
    CHECK(EqDelayUs(0) == 400 && EqDelayUs(2) == 8000 && EqDelayUs(0x82) == 8000, "eq delays");
}

static void test_wm() {
    WmParams wp;
    WmParamsLinear(262430, 2720, 2560, 4, &wp);
    // 2560 * 4 = 10240 bytes / 512 = 20, + 1 = 21 blocks per line; linetime
    // ceil(2720000 / 262430) = 11 us (satoru)
    CHECK((wp.blocks_per_line_fx >> 16) == 21 && (wp.blocks_per_line_fx & 0xFFFF) == 0, "blocks per line");
    CHECK(wp.linetime_us == 11, "linetime us %u", wp.linetime_us);
    WmLevel z = { false, 0, 0, 0 }, l0, l3;
    // latency 2 < 11: method1 = 2 * 262430 * 4 / 512000 = 2.05 (+1) -> 4 + 1 = 5,
    // raised to 21 blocks; lines ceil(3.05 / 21) = 1; min_ddb max(21 + 3, 21) + 1 = 25 (satoru)
    PlaneWm(wp, 2, z, &l0);
    CHECK(l0.enable && l0.blocks == 21 && l0.lines == 1 && l0.min_ddb == 25, "wm0 %u/%u/%u", l0.blocks, l0.lines, l0.min_ddb);
    // latency 16 >= 11: method2 = ceil(16 * 262430 / 2720000) = 2 lines * 21 = 42;
    // blocks 43, lines 2, min_ddb 43 + 5 + 1 = 49 (satoru)
    PlaneWm(wp, 16, l0, &l3);
    CHECK(l3.enable && l3.blocks == 43 && l3.lines == 2 && l3.min_ddb == 49, "wm16 %u/%u/%u", l3.blocks, l3.lines, l3.min_ddb);
    WmLevel r;
    PlaneWm(wp, 0, l0, &r);
    CHECK(!r.enable && r.min_ddb == WM_REJECT, "latency 0 rejected");
    WmLevel t;
    TransWm(l0, &t);
    CHECK(t.enable && t.blocks == 21 - 1 + 14 + 1 && t.min_ddb == 36, "trans wm %u %u", t.blocks, t.min_ddb);
    CHECK(WmRegVal(l0) == ((1u << 31) | (1u << 14) | 21u), "wm reg %08x", WmRegVal(l0));
    uint16_t lat[6] = { 0, 4, 0, 16, 30, 40 };
    AdjustLatency(lat);
    CHECK(lat[0] == 6 && lat[1] == 10 && lat[2] == 0 && lat[3] == 0 && lat[5] == 0, "latency adjust %u %u %u %u", lat[0], lat[1], lat[2], lat[3]);
    uint16_t lat2[6] = { 2, 1, 8, 16, 30, 40 };
    AdjustLatency(lat2);
    CHECK(lat2[0] == 2 && lat2[1] == 2 && lat2[2] == 8 && lat2[5] == 40, "latency monotonic");
}

// ---- mocked dp sink for the link training state machine (satoru) ----
struct Sink {
    uint8_t dpcd[0x3000];
    uint8_t lttpr[0x400];         // 0xf0000.. (satoru)
    int lanes_ok_vswing;          // cr done once train_set vswing >= this (satoru)
    int eq_preemph;               // eq done once pre-emphasis >= this (satoru)
    bool never_cr, never_eq;
    int writes;
    uint8_t src_pattern;
    int idle_calls, level_calls;
    uint32_t log_lines;
    char trace[8192];
    int tpos;
    int lttpr_phy_trained[8];
};
static Sink g_k;

static uint8_t* sink_at(uint32_t a) {
    if (a < sizeof(g_k.dpcd)) return &g_k.dpcd[a];
    if (a >= 0xf0000 && a < 0xf0000 + sizeof(g_k.lttpr)) return &g_k.lttpr[a - 0xf0000];
    return nullptr;
}

static void sink_update_status() {
    // dprx: lane status from the lane set and the pattern (satoru)
    uint8_t pat = g_k.dpcd[0x102] & 0xF;
    int lanes = g_k.dpcd[0x101] & 0x1F;
    uint8_t st[4] = {};
    uint8_t adj[4] = {};
    for (int l = 0; l < lanes; l++) {
        uint8_t ts = g_k.dpcd[0x103 + l];
        int v = ts & 3, p = (ts >> 3) & 3;
        bool cr = !g_k.never_cr && v >= g_k.lanes_ok_vswing;
        bool eq = cr && !g_k.never_eq && (pat == 7 || pat == 3 || pat == 2) && p >= g_k.eq_preemph;
        st[l] = (uint8_t)((cr ? 1 : 0) | (eq ? 6 : 0));
        int av = g_k.never_cr ? (v < 3 ? v + 1 : 3) : (v < g_k.lanes_ok_vswing ? g_k.lanes_ok_vswing : v);
        int ap = p < g_k.eq_preemph ? g_k.eq_preemph : p;
        adj[l] = (uint8_t)(av | (ap << 2));
    }
    g_k.dpcd[0x202] = (uint8_t)(st[0] | (st[1] << 4));
    g_k.dpcd[0x203] = (uint8_t)(st[2] | (st[3] << 4));
    bool all_eq = lanes > 0;
    for (int l = 0; l < lanes; l++) if ((st[l] & 7) != 7) all_eq = false;
    g_k.dpcd[0x204] = all_eq ? 1 : 0;
    g_k.dpcd[0x205] = 1;
    g_k.dpcd[0x206] = (uint8_t)(adj[0] | (adj[1] << 4));
    g_k.dpcd[0x207] = (uint8_t)(adj[2] | (adj[3] << 4));
    // lttpr 1: trained straight away when its pattern is set (satoru)
    uint8_t lp = g_k.lttpr[0x10] & 0xF;
    uint8_t ls = lp == 1 ? 0x11 : (lp == 7 ? 0x77 : 0);
    g_k.lttpr[0x30] = ls; g_k.lttpr[0x31] = ls; g_k.lttpr[0x32] = lp == 7 ? 1 : 0;
    g_k.lttpr[0x33] = 0; g_k.lttpr[0x34] = 0;
    if (lp) g_k.lttpr_phy_trained[1] |= lp == 7 ? 2 : 1;
}

static void tr(const char* s) { int n = (int)strlen(s); if (g_k.tpos + n + 2 < (int)sizeof(g_k.trace)) { memcpy(g_k.trace + g_k.tpos, s, n); g_k.tpos += n; g_k.trace[g_k.tpos++] = '\n'; g_k.trace[g_k.tpos] = 0; } }

static int m_rd(void*, uint32_t a, uint8_t* b, int n) {
    sink_update_status();
    for (int i = 0; i < n; i++) { uint8_t* p = sink_at(a + (uint32_t)i); if (!p) return -1; b[i] = *p; }
    return n;
}
static int m_wr(void*, uint32_t a, const uint8_t* b, int n) {
    char t[128]; int k = snprintf(t, sizeof(t), "w %05x:", a);
    for (int i = 0; i < n; i++) { uint8_t* p = sink_at(a + (uint32_t)i); if (!p) return -1; *p = b[i]; k += snprintf(t + k, sizeof(t) - k, " %02x", b[i]); }
    tr(t);
    g_k.writes++;
    return n;
}
static void m_pat(void*, uint8_t p) { g_k.src_pattern = p; char t[32]; snprintf(t, sizeof(t), "src pattern %02x", p); tr(t); }
static void m_lv(void*, const uint8_t ts[4], int lanes) { g_k.level_calls++; char t[64]; snprintf(t, sizeof(t), "src levels %02x %02x %02x %02x x%d", ts[0], ts[1], ts[2], ts[3], lanes); tr(t); }
static void m_idle(void*) { g_k.idle_calls++; tr("src idle"); }
static void m_delay(void*, uint32_t) {}
static void m_log(void*, const char* s) { g_k.log_lines++; if (getenv("LT_VERBOSE")) printf("  lt: %s\n", s); }

static void sink_reset() {
    memset(&g_k, 0, sizeof(g_k));
    // dp 1.4, hbr3, 4 lanes, tps3 + enhanced, tps4, aux rd interval 0 (satoru)
    g_k.dpcd[0] = 0x14; g_k.dpcd[1] = 0x1E; g_k.dpcd[2] = 0xC4; g_k.dpcd[3] = 0x81;
    g_k.lanes_ok_vswing = 1;
    g_k.eq_preemph = 1;
}

static DpLt::Ops ops() { return DpLt::Ops{ nullptr, m_rd, m_wr, m_pat, m_lv, m_idle, m_delay, m_log }; }

static DpLt::Params params(int lttprs) {
    DpLt::Params p = {};
    p.rate_khz = 270000; p.lanes = 4; p.enhanced_framing = true;
    for (int i = 0; i < 16; i++) p.dpcd[i] = g_k.dpcd[i];
    p.lttpr_count = lttprs;
    p.source_vswing_max = 3; p.source_preemph_max = 3 << 3;
    return p;
}

static void test_lt() {
    sink_reset();
    DpLt::Ops o = ops();
    DpLt::Params p = params(0);
    CHECK(DpLt::PrepareSink(o, p), "prepare");
    CHECK(g_k.dpcd[0x107] == 0x00 && g_k.dpcd[0x108] == 0x01, "downspread / 8b10b");
    CHECK(g_k.dpcd[0x100] == 0x0A && g_k.dpcd[0x101] == 0x84, "link bw 0x0a, 4 lanes + enhanced");
    DpLt::Result r;
    bool ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(ok && r.ok, "training passes: %s", r.reason);
    CHECK(r.pattern == DpLt::TP4, "tps4 chosen for a dp1.4 tps4 sink");
    CHECK(r.cr_loops == 2, "cr loops %d (1 adjust)", r.cr_loops);
    CHECK(r.eq_loops == 1, "eq loops %d (pre-emphasis already granted during cr)", r.eq_loops);
    CHECK((r.train_set[0] & 3) == 1 && ((r.train_set[0] >> 3) & 3) == 1, "train set vs1 pe1: %02x", r.train_set[0]);
    CHECK(g_k.dpcd[0x102] == 0, "dprx pattern disabled at the end");
    CHECK(g_k.idle_calls == 1, "source idle once");
    CHECK(strstr(g_k.trace, "w 00102: 21 00 00 00 00") != nullptr, "tps1 | scrambling disable with zero lane sets");
    CHECK(strstr(g_k.trace, "w 00103: 09 09 09 09") != nullptr, "cr adjust to vs1 pe1 on all lanes");
    CHECK(strstr(g_k.trace, "w 00102: 07 09 09 09 09") != nullptr, "tps4 (scrambled) with the cr lane sets");
    CHECK(strstr(g_k.trace, "src pattern 21") != nullptr && strstr(g_k.trace, "src pattern 07") != nullptr, "source patterns");

    // tps3 sink (no tps4): pattern 3 | scrambling disable (satoru)
    sink_reset();
    g_k.dpcd[3] = 0x01;
    p = params(0);
    DpLt::PrepareSink(o, p);
    ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(ok && r.pattern == DpLt::TP3, "tps3 path");
    CHECK(strstr(g_k.trace, "w 00102: 23") != nullptr, "tps3 sent with scrambling disabled");

    // a sink that never locks: bounded, fails cleanly (satoru)
    sink_reset();
    g_k.never_cr = true;
    p = params(0);
    DpLt::PrepareSink(o, p);
    ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(!ok && r.phy_failed == 0, "never-cr sink fails");
    CHECK(r.cr_loops <= 10, "cr loop bounded by 10 for dpcd 1.4: %d", r.cr_loops);
    CHECK(g_k.dpcd[0x102] == 0 && g_k.idle_calls == 1, "pattern disabled + idle even on failure");
    printf("info: never-cr sink: %d cr loops, reason \"%s\"\n", r.cr_loops, r.reason);

    // eq that never completes: 5 tries (satoru)
    sink_reset();
    g_k.never_eq = true;
    p = params(0);
    DpLt::PrepareSink(o, p);
    ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(!ok && r.eq_loops == 5, "eq bounded by 5 tries: %d", r.eq_loops);

    // one lttpr in non-transparent mode: phy 1 first via 0xf0010, then dprx (satoru)
    sink_reset();
    p = params(1);
    p.lttpr_phy_caps[0][0] = 0; p.lttpr_phy_caps[0][1] = 3;
    DpLt::PrepareSink(o, p);
    ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(ok, "lttpr + dprx training: %s", r.reason);
    const char* a = strstr(g_k.trace, "w f0010: 21");
    const char* b = strstr(g_k.trace, "w 00102: 21");
    CHECK(a && b && a < b, "lttpr trained before the dprx");
    CHECK(g_k.lttpr[0x10] == 0, "lttpr pattern disabled after its training");
    CHECK(g_k.lttpr_phy_trained[1] == 3, "lttpr saw tps1 and tps4");

    // two lanes (pin assignment d) (satoru)
    sink_reset();
    p = params(0);
    p.lanes = 2; p.rate_khz = 540000;
    CHECK(DpLt::PrepareSink(o, p) && g_k.dpcd[0x100] == 0x14 && g_k.dpcd[0x101] == 0x82, "hbr2 x2 config");
    ok = DpLt::TrainAllPhys(o, p, &r);
    CHECK(ok, "2 lane training");
    CHECK(strstr(g_k.trace, "w 00102: 21 00 00\n") != nullptr, "only 2 lane sets written");
}

static void timing(uint32_t hz_milli, uint32_t pixclk, uint16_t h, uint16_t v, DpEdid::Timing* t) {
    *t = DpEdid::Timing();
    t->refresh_mhz = hz_milli;
    t->pixclk_khz = pixclk;
    t->hact = h; t->vact = v;
    t->hblank = 160; t->hfp = 48; t->hsync = 32;
    t->vblank = 40; t->vfp = 3; t->vsync = 5;
    t->hpos = t->vpos = true;
}

static void test_timing_pick() {
    // a 2560x1440 monitor whose edid offers 60, 143.912 and 165 hz, plus a
    // 1080p 240 hz entry that must never be picked at the native size. (satoru)
    DpEdid::Edid e;
    e.preferred = 0;
    e.nt = 4;
    timing(60001, 262430, 2560, 1440, &e.t[0]);   e.t[0].preferred = true;
    timing(143912, 586590, 2560, 1440, &e.t[1]);
    timing(165000, 674500, 2560, 1440, &e.t[2]);
    timing(240000, 610000, 1920, 1080, &e.t[3]);

    CHECK(DpEdid::PickTimingAtResolution(e, 165000) == 2, "exact 165 hz timing");
    CHECK(DpEdid::PickTimingAtResolution(e, 60001) == 0, "exact 60 hz timing");
    CHECK(DpEdid::PickTimingAtResolution(e, 120000) == 1, "120 asks for the closest (144)");
    CHECK(DpEdid::PickTimingAtResolution(e, 200000) == 2, "200 asks for the closest (165)");
    CHECK(DpEdid::PickTimingAtResolution(e, 1000000) == 2, "above every timing picks the fastest at this size");
    CHECK(DpEdid::PickTimingAtResolution(e, 1) == 0, "below every timing picks the slowest at this size");

    uint32_t rates[8] = {};
    int n = DpEdid::RatesAtPreferred(e, rates, 8);
    CHECK(n == 3, "three rates at 2560x1440, got %d", n);
    CHECK(n == 3 && rates[0] == 60 && rates[1] == 144 && rates[2] == 165, "rates ascending: %u %u %u",
          n > 0 ? rates[0] : 0, n > 1 ? rates[1] : 0, n > 2 ? rates[2] : 0);

    // only the native 60 and a 1080p 240: an unavailable 165 falls back to 60.
    DpEdid::Edid f;
    f.preferred = 0;
    f.nt = 2;
    timing(60001, 262430, 2560, 1440, &f.t[0]); f.t[0].preferred = true;
    timing(240000, 610000, 1920, 1080, &f.t[1]);
    CHECK(DpEdid::PickTimingAtResolution(f, 165000) == 0, "falls back to the preferred 60 hz timing");
    n = DpEdid::RatesAtPreferred(f, rates, 8);
    CHECK(n == 1 && rates[0] == 60, "only the native rate is offered, got %d", n);

    // a 59.94 hz edn rate rounds to 60 in the human list. (satoru)
    DpEdid::Edid g;
    g.preferred = 0;
    g.nt = 1;
    timing(59940, 262430, 2560, 1440, &g.t[0]); g.t[0].preferred = true;
    n = DpEdid::RatesAtPreferred(g, rates, 8);
    CHECK(n == 1 && rates[0] == 60, "59.94 rounds to 60, got %u", n ? rates[0] : 0);
}

// PickFittingTiming: the preferred 4k60 does not fit a 300 mhz pixel clock
// cap, so the largest fitting timing wins, refresh nearest 60 among equal
// sizes, lower clock on a tie; nothing fits -> -1. (satoru)
static bool fits_300mhz(const DpEdid::Timing& t, void* ctx) { return t.pixclk_khz <= *(uint32_t*)ctx; }

static void test_fitting_pick() {
    DpEdid::Edid e;
    memset(&e, 0, sizeof(e));
    e.preferred = 0;
    e.nt = 5;
    timing(60000, 594000, 3840, 2160, &e.t[0]); e.t[0].preferred = true;
    timing(30000, 297000, 3840, 2160, &e.t[1]);
    timing(60000, 148500, 1920, 1080, &e.t[2]);
    timing(144000, 325000, 1920, 1080, &e.t[3]);
    timing(60000, 241500, 2560, 1440, &e.t[4]);
    uint32_t cap = 300000;
    int i = DpEdid::PickFittingTiming(e, 60000, fits_300mhz, &cap);
    CHECK(i == 1, "4k30 (297 mhz) is the largest that fits 300 mhz, got %d", i);
    cap = 250000;
    i = DpEdid::PickFittingTiming(e, 60000, fits_300mhz, &cap);
    CHECK(i == 4, "1440p60 is the largest under 250 mhz, got %d", i);
    cap = 600000;
    i = DpEdid::PickFittingTiming(e, 60000, fits_300mhz, &cap);
    CHECK(i == 0, "everything fits: 4k60 beats 4k30 on refresh, got %d", i);
    cap = 100000;
    i = DpEdid::PickFittingTiming(e, 60000, fits_300mhz, &cap);
    CHECK(i == -1, "nothing fits 100 mhz, got %d", i);
    e.t[1].interlaced = true;
    cap = 300000;
    i = DpEdid::PickFittingTiming(e, 60000, fits_300mhz, &cap);
    CHECK(i == 4, "interlaced 4k30 skipped, got %d", i);
}

int main() {
    test_c20();
    test_fitting_pick();
    test_mode_and_link();
    test_levels_ggtt_misc();
    test_timing_pick();
    test_wm();
    test_lt();
    printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "ALL PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// end (satoru)
