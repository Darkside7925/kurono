// intel display stage 2 maths. see intel_modeset_calc.h. every function cites
// the linux source it transcribes (torvalds/linux master,
// drivers/gpu/drm/i915/display/ unless noted). (satoru)
#include "intel_modeset_calc.h"

namespace IgpuCalc {

namespace {

inline uint64_t div_round_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
inline uint64_t div_round_closest(uint64_t a, uint64_t b) { return (a + b / 2) / b; }
template <typename T> inline T max_(T a, T b) { return a > b ? a : b; }
template <typename T> inline T min_(T a, T b) { return a < b ? a : b; }

// intel_fixed.h uint_fixed_16_16_t helpers (satoru)
inline uint32_t clamp_fx(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
inline uint32_t u32_to_fx(uint32_t v) { return v << 16; }
inline uint32_t div_fx(uint32_t val, uint32_t d) { return clamp_fx(div_round_up((uint64_t)val << 16, d)); }
inline uint32_t add_fx_u32(uint32_t a, uint32_t b) { return clamp_fx((uint64_t)a + ((uint64_t)b << 16)); }
inline uint32_t mul_u32_fx(uint32_t v, uint32_t fx) { return clamp_fx((uint64_t)v * fx); }
inline uint32_t fx_round_up(uint32_t fx) { return (uint32_t)div_round_up(fx, 1u << 16); }
inline uint32_t div_round_up_fx(uint32_t a, uint32_t b) { return (uint32_t)div_round_up(a, b); }
constexpr uint32_t FP_16_16_MAX = 0xFFFFFFFFu;

// drm_dp_link_data_symbol_cycles + drm_dp_bw_overhead for 8b/10b sst, no
// dsc, no ssc, no fec, then intel_dp_link_bw_overhead's max with the no-fec
// 1000000 (satoru)
uint32_t bw_overhead_8b10b(int lanes, uint32_t hactive, uint32_t bpp_x16) {
    if (!lanes || !hactive || !bpp_x16) return 0;
    const uint32_t symbol_size = 8;
    uint64_t cycles = div_round_up((uint64_t)hactive * bpp_x16, 16ull * symbol_size * (uint64_t)lanes);
    uint64_t ov = div_round_up(cycles * symbol_size * (uint64_t)lanes * (1000000ull * 16), (uint64_t)hactive * bpp_x16);
    return (uint32_t)max_<uint64_t>(ov, 1000000ull);
}

} // namespace

bool ModeFromDtd(uint32_t pixclk_khz, uint16_t hact, uint16_t hblank, uint16_t hfp, uint16_t hsync,
                 uint16_t vact, uint16_t vblank, uint16_t vfp, uint16_t vsync,
                 bool hpos, bool vpos, bool interlaced, Mode* m) {
    // drm_edid.c drm_mode_detailed: hsync_start = hactive + hsync_offset,
    // hsync_end = hsync_start + hsync_pulse_width, htotal = hactive + hblank
    // (same for v) (satoru)
    if (!pixclk_khz || !hact || !vact || !hsync || !vsync) return false;
    if ((uint32_t)hfp + hsync > hblank || (uint32_t)vfp + vsync > vblank) return false;
    m->clock_khz = pixclk_khz;
    m->hdisplay = hact;
    m->hsync_start = (uint16_t)(hact + hfp);
    m->hsync_end = (uint16_t)(hact + hfp + hsync);
    m->htotal = (uint16_t)(hact + hblank);
    m->vdisplay = vact;
    m->vsync_start = (uint16_t)(vact + vfp);
    m->vsync_end = (uint16_t)(vact + vfp + vsync);
    m->vtotal = (uint16_t)(vact + vblank);
    m->phsync = hpos;
    m->pvsync = vpos;
    m->interlaced = interlaced;
    m->refresh_mhz = (uint32_t)div_round_closest((uint64_t)pixclk_khz * 1000000ull,
                                                 (uint64_t)m->htotal * m->vtotal);
    return true;
}

uint32_t ModeDataRate(uint32_t pixel_khz, int bpp) {
    // intel_dp_effective_data_rate(pixel_clock, bpp_x16, 1000000) (satoru)
    uint64_t bpp_x16 = (uint64_t)bpp * 16;
    return (uint32_t)div_round_up((uint64_t)pixel_khz * bpp_x16 * 1000000ull, 1000000ull * 16 * 8);
}

uint32_t LinkDataRate(uint32_t link_khz, int lanes) {
    // drm_dp_max_dprx_data_rate: rate * 10 * lanes * 800000 / (1000000 * 8),
    // rate in 10 kbit/s units == the khz value i915 uses (satoru)
    return (uint32_t)(((uint64_t)link_khz * 10 * (uint64_t)lanes * 800000ull) / (1000000ull * 8));
}

uint32_t LinkRequired(int lanes, uint32_t hdisplay, uint32_t pixel_khz, int bpp) {
    // intel_dp_link_required: intel_dp_effective_data_rate(clock, bpp_x16,
    // intel_dp_link_bw_overhead(...)) (satoru)
    uint32_t bpp_x16 = (uint32_t)bpp * 16;
    uint32_t ov = bw_overhead_8b10b(lanes, hdisplay, bpp_x16);
    return (uint32_t)div_round_up((uint64_t)pixel_khz * bpp_x16 * ov, 1000000ull * 16 * 8);
}

int LinkConfigs(uint32_t max_rate_khz, int max_lanes, uint32_t pixel_khz, uint32_t hdisplay, int bpp,
                LinkCfg* out, int maxn) {
    static const uint32_t rates[] = { 162000, 270000, 540000, 810000 };
    static const int lanes_tab[] = { 1, 2, 4 };
    int n = 0;
    // intel_dp_compute_link_config_wide: mode_rate = intel_dp_link_required(
    // rate, lanes, clock, hdisplay, bpp_x16) must be <= link_avail =
    // intel_dp_max_link_data_rate(rate, lanes) (satoru)
    for (uint32_t r : rates) {
        if (r > max_rate_khz) continue;
        for (int l : lanes_tab) {
            if (l > max_lanes) continue;
            if (LinkRequired(l, hdisplay, pixel_khz, bpp) <= LinkDataRate(r, l) && n < maxn)
                out[n++] = LinkCfg{ r, l };
        }
    }
    return n;
}

uint32_t BwCodeToRate(uint8_t code) { return (uint32_t)code * 27000u; }
uint8_t RateToBwCode(uint32_t rate_khz) { return (uint8_t)(rate_khz / 27000u); }

// intel_display.c intel_reduce_m_n_ratio / compute_m_n (satoru)
static void compute_m_n(uint32_t* ret_m, uint32_t* ret_n, uint32_t m, uint32_t n, uint32_t constant_n) {
    const uint32_t DATA_LINK_M_N_MASK = 0xFFFFFF;   // intel_display_regs.h genmask(23, 0) (satoru)
    *ret_n = constant_n;
    *ret_m = (uint32_t)(((uint64_t)m * *ret_n) / n);
    while (*ret_m > DATA_LINK_M_N_MASK || *ret_n > DATA_LINK_M_N_MASK) {
        *ret_m >>= 1;
        *ret_n >>= 1;
    }
}

void ComputeMN(int bpp, int lanes, uint32_t pixel_khz, uint32_t link_khz, MN* mn) {
    // intel_link_compute_m_n with bw_overhead = intel_dp_bw_fec_overhead(false)
    // = 1000000: data_m = intel_dp_effective_data_rate, data_n =
    // drm_dp_max_dprx_data_rate, link m/n = pixel clock / link symbol clock,
    // fixed n 0x8000000 / 0x80000, tu 64 (satoru)
    uint32_t data_m = ModeDataRate(pixel_khz, bpp);
    uint32_t data_n = LinkDataRate(link_khz, lanes);
    mn->tu = 64;
    compute_m_n(&mn->data_m, &mn->data_n, data_m, data_n, 0x8000000);
    compute_m_n(&mn->link_m, &mn->link_n, pixel_khz, LinkSymbolClock(link_khz), 0x80000);
}

uint32_t DataMReg(const MN& mn) {
    // tu_size(x) = field 30:25 = x - 1 (intel_display_regs.h) (satoru)
    return ((mn.tu - 1) & 0x3F) << 25 | (mn.data_m & 0xFFFFFF);
}

void TranscoderRegs(const Mode& m, TransRegs* r) {
    // intel_set_transcoder_timings: each register holds (end - 1) << 16 |
    // (start - 1). hblank start/end = hdisplay / htotal (crtc_hblank_* from
    // drm_mode_set_crtcinfo), vblank_start forced to 1 on display ver >= 13
    // (scl owns the vblank start, scl = 0 without psr) (satoru)
    r->htotal = ((uint32_t)(m.htotal - 1) << 16) | (uint32_t)(m.hdisplay - 1);
    r->hblank = ((uint32_t)(m.htotal - 1) << 16) | (uint32_t)(m.hdisplay - 1);
    r->hsync = ((uint32_t)(m.hsync_end - 1) << 16) | (uint32_t)(m.hsync_start - 1);
    r->vtotal = ((uint32_t)(m.vtotal - 1) << 16) | (uint32_t)(m.vdisplay - 1);
    r->vblank = ((uint32_t)(m.vtotal - 1) << 16) | (uint32_t)(1 - 1);
    r->vsync = ((uint32_t)(m.vsync_end - 1) << 16) | (uint32_t)(m.vsync_start - 1);
    // intel_set_pipe_src_size: pipesrc_width(w - 1) bits 31:16, height 15:0 (satoru)
    r->pipesrc = ((uint32_t)(m.hdisplay - 1) << 16) | (uint32_t)(m.vdisplay - 1);
}

// intel_cx0_phy.c mtl_c20_dp_rbr/hbr1/hbr2/hbr3 (dp 1.4 tables, mpllb). (satoru)
static const C20Pll C20_DP[] = {
    { "rbr", 162000, { 0xbe88, 0x5800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },
      { 0x50a8, 0x2120, 0xcd9a, 0xbfc1, 0x5ab8, 0x4c34, 0x2000, 0x0001, 0x6000, 0x0000, 0x0000 } },
    { "hbr1", 270000, { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },
      { 0x308c, 0x2110, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81, 0x2000, 0x0001, 0x5000, 0x0000, 0x0000 } },
    { "hbr2", 540000, { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },
      { 0x108c, 0x2108, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81, 0x2000, 0x0001, 0x5000, 0x0000, 0x0000 } },
    { "hbr3", 810000, { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },
      { 0x10d2, 0x2108, 0x8d98, 0xbfc1, 0x7166, 0x5f42, 0x2000, 0x0001, 0x7800, 0x0000, 0x0000 } },
};

const C20Pll* C20DpTable(uint32_t rate_khz) {
    // intel_c20_pll_find_table: the entry whose computed port clock matches
    // (intel_dpll_clock_matches, +-1 khz) (satoru)
    for (const C20Pll& p : C20_DP) {
        uint32_t c = C20PortClock(p);
        uint32_t d = c > rate_khz ? c - rate_khz : rate_khz - c;
        if (d <= 1) return &p;
    }
    return nullptr;
}

bool C20UsesMpllb(const C20Pll& p) { return (p.tx[0] >> 7) & 1; }

uint32_t C20PortClock(const C20Pll& p) {
    // intel_c20pll_calc_port_clock, mpllb branch (all dp 1.4 tables). field
    // masks from intel_cx0_phy_regs.h: c20_phy_tx_rate 2:0, c20_multiplier
    // 11:0, c20_mpllb_tx_clk_div 15:13, c20_mpllb_fracen bit 13,
    // c20_ref_clk_mpllb_div 12:10 (satoru)
    const uint32_t refclk = 38400;
    if (!C20UsesMpllb(p)) return 0;
    uint32_t tx_rate = p.tx[0] & 0x7;
    uint32_t tx_rate_mult = 1;
    uint32_t frac_en = (p.mpllb[6] >> 13) & 1;
    uint32_t frac_quot = p.mpllb[8], frac_rem = p.mpllb[9], frac_den = p.mpllb[7];
    uint32_t multiplier = p.mpllb[0] & 0xFFF;
    uint32_t tx_clk_div = (p.mpllb[0] >> 13) & 0x7;
    uint32_t ref_div = (p.mpllb[6] >> 10) & 0x7;
    uint32_t fb_clk_div4_en = 0;
    uint32_t frac = 0;
    if (frac_en) frac = frac_quot + (frac_den ? (uint32_t)div_round_closest(frac_rem, frac_den) : 0);
    uint32_t ref = (uint32_t)div_round_closest((uint64_t)refclk * (1u << (1 + fb_clk_div4_en)), 1u << ref_div);
    uint64_t prod = ((uint64_t)ref * (((uint64_t)multiplier << (17 - 2)) + frac)) >> 17;
    uint32_t vco = (uint32_t)div_round_closest(prod, 10);
    return ((vco << tx_rate_mult) >> tx_clk_div) >> tx_rate;
}

uint8_t C20DpRate(uint32_t rate_khz) {
    // intel_c20_get_dp_rate (dp 1.4 rates only) (satoru)
    switch (rate_khz) {
        case 162000: return 0;
        case 270000: return 1;
        case 540000: return 2;
        case 810000: return 3;
        default: return 0xFF;
    }
}

uint32_t LinkSymbolClock(uint32_t rate_khz) {
    // intel_dp_link_symbol_clock: round_closest(rate * 10, 10) for 8b/10b (satoru)
    return (uint32_t)div_round_closest((uint64_t)rate_khz * 10, 10);
}

// intel_ddi.c index_to_dp_signal_levels: vswing bits 1:0, pre-emphasis 4:3 (satoru)
static const uint8_t SIGNAL_LEVELS[C20_DP14_LEVELS] = {
    0x00, 0x08, 0x10, 0x18, 0x01, 0x09, 0x11, 0x02, 0x0A, 0x03,
};

int TrainSetToLevel(uint8_t train_set) {
    // intel_ddi_dp_level -> translate_signal_level (satoru)
    uint8_t s = train_set & (0x3 | 0x18);
    for (int i = 0; i < C20_DP14_LEVELS; i++) if (SIGNAL_LEVELS[i] == s) return i;
    return -1;
}

Snps C20Dp14Level(int level) {
    // intel_ddi_buf_trans.c _mtl_c20_trans_dp14 (vswing, pre_cursor, post_cursor) (satoru)
    static const Snps t[C20_DP14_LEVELS] = {
        { 20, 0, 0 }, { 24, 0, 4 }, { 30, 0, 9 }, { 34, 0, 14 }, { 29, 0, 0 },
        { 34, 0, 5 }, { 38, 0, 10 }, { 36, 0, 0 }, { 40, 0, 6 }, { 48, 0, 0 },
    };
    if (level < 0) level = 0;
    if (level >= C20_DP14_LEVELS) level = C20_DP14_LEVELS - 1;
    return t[level];
}

uint64_t MtlGgttPte(uint64_t phys, int pat_index) {
    // mtl_ggtt_pte_encode: addr | gen8_page_present (bit 0), pat bit 0 ->
    // mtl_ggtt_pte_pat0 (bit 52), pat bit 1 -> mtl_ggtt_pte_pat1 (bit 53),
    // address mask gen12_ggtt_pte_addr_mask 45:12 (gt/intel_gtt.h) (satoru)
    uint64_t pte = (phys & 0x00003FFFFFFFF000ull) | 1ull;
    if (pat_index & 1) pte |= 1ull << 52;
    if (pat_index & 2) pte |= 1ull << 53;
    return pte;
}

bool GgttPteLive(uint64_t pte, uint64_t phys_top) {
    // a live mtl pte is present, optionally lm, address 45:12, pat 52:53 with
    // the address inside ram. anything else is stale firmware contents, not a
    // mapping. (satoru)
    const uint64_t kAddr = 0x00003FFFFFFFF000ull;
    const uint64_t kOk = 1ull | 2ull | kAddr | (1ull << 52) | (1ull << 53);
    if (!(pte & 1) || (pte & ~kOk)) return false;
    uint64_t pa = pte & kAddr;
    return pa != 0 && pa < phys_top;
}

void WmParamsLinear(uint32_t pixel_rate_khz, uint32_t htotal, uint32_t width, uint32_t cpp, WmParams* wp) {
    // skl_compute_wm_params, linear (not x/y tiled), display ver >= 10:
    // dbuf block 512, interm_pbpl = div_round_up(bytes per line, 512) + 1,
    // linetime_us = div_round_up(htotal * 1000, pixel rate) (satoru)
    wp->pixel_rate_khz = pixel_rate_khz;
    wp->htotal = htotal;
    wp->cpp = cpp;
    wp->dbuf_block_size = 512;
    wp->plane_bytes_per_line = width * cpp;
    uint32_t interm = (uint32_t)div_round_up(wp->plane_bytes_per_line, wp->dbuf_block_size) + 1;
    wp->blocks_per_line_fx = u32_to_fx(interm);
    wp->linetime_us = (uint32_t)div_round_up((uint64_t)htotal * 1000, pixel_rate_khz);
}

void PlaneWm(const WmParams& wp, uint32_t latency_us, const WmLevel& prev, WmLevel* out) {
    // skl_compute_plane_wm, display ver 14, linear plane (satoru)
    *out = WmLevel{ false, 0, 0, WM_REJECT };
    if (latency_us == 0) return;
    // skl_wm_method1: latency * rate * cpp / (1000 * block), +1 on ver >= 10 (satoru)
    uint32_t method1 = add_fx_u32(div_fx(latency_us * wp.pixel_rate_khz * wp.cpp, 1000 * wp.dbuf_block_size), 1);
    // skl_wm_method2: div_round_up(latency * rate, htotal * 1000) * blocks per line (satoru)
    uint32_t m2i = (uint32_t)div_round_up((uint64_t)latency_us * wp.pixel_rate_khz, (uint64_t)wp.htotal * 1000);
    uint32_t method2 = mul_u32_fx(m2i, wp.blocks_per_line_fx);
    uint32_t selected;
    if ((wp.cpp * wp.htotal / wp.dbuf_block_size < 1) && (wp.plane_bytes_per_line / wp.dbuf_block_size < 1))
        selected = method2;
    else if (latency_us >= wp.linetime_us)
        selected = method2;
    else
        selected = method1;
    uint32_t blocks = fx_round_up(selected) + 1;           // ver < 30 (satoru)
    blocks = max_(blocks, fx_round_up(wp.blocks_per_line_fx));   // skl_wm_has_lines on ver >= 10 (satoru)
    uint32_t lines = div_round_up_fx(selected, wp.blocks_per_line_fx);
    blocks = max_<uint32_t>(blocks, prev.blocks);
    lines = max_<uint32_t>(lines, prev.lines);
    uint32_t min_ddb = blocks + (uint32_t)div_round_up(blocks, 10);  // ver >= 11, not y tiled (satoru)
    if (lines > 255) return;                               // skl_wm_max_lines ver >= 13 (satoru)
    out->blocks = (uint16_t)blocks;
    out->lines = (uint16_t)lines;
    out->min_ddb = (uint16_t)min_(max_(min_ddb, blocks) + 1, 0xFFFEu);
    out->enable = true;
}

void TransWm(const WmLevel& wm0, WmLevel* out) {
    // skl_compute_transition_wm, ver >= 11: trans_min 4, trans_amount 10,
    // blocks = (wm0.blocks - 1) + 14 + 1 for a linear plane (satoru)
    *out = WmLevel{ false, 0, 0, WM_REJECT };
    if (!wm0.enable) return;
    uint16_t blocks = (uint16_t)(wm0.blocks - 1 + 4 + 10 + 1);
    out->blocks = blocks;
    out->lines = 0;
    out->min_ddb = max_<uint16_t>(wm0.min_ddb, (uint16_t)(blocks + 1));
    out->enable = true;
}

void AdjustLatency(uint16_t wm[6]) {
    // adjust_wm_latency -> sanitize_wm_latency, make_wm_latency_monotonic,
    // waWmMemoryReadLatency (+6 us on ver >= 14 when level 0 reads 0) (satoru)
    const int n = 6;
    int level;
    for (level = 1; level < n; level++) if (wm[level] == 0) break;
    for (level = level + 1; level < n; level++) wm[level] = 0;
    for (level = 1; level < n; level++) {
        if (wm[level] == 0) break;
        wm[level] = max_(wm[level], wm[level - 1]);
    }
    if (wm[0] == 0) {
        // increase_wm_latency(display, wm_read_latency = 6) (satoru)
        wm[0] += 6;
        for (level = 1; level < n; level++) {
            if (wm[level] == 0) break;
            wm[level] += 6;
        }
    }
}

uint32_t WmRegVal(const WmLevel& l) {
    // skl_plane_wm_reg_val: plane_wm_en bit 31, lines 26:14, blocks 12:0 (satoru)
    uint32_t v = 0;
    if (l.enable) v |= 1u << 31;
    v |= ((uint32_t)l.lines & 0x1FFF) << 14;
    v |= (uint32_t)l.blocks & 0x1FFF;
    return v;
}

uint32_t DdbRegVal(uint16_t start, uint16_t end) {
    // skl_plane_ddb_reg_val: plane_buf_end(end - 1) 28:16 | plane_buf_start 12:0 (satoru)
    if (!end) return 0;
    return (((uint32_t)(end - 1) & 0x1FFF) << 16) | ((uint32_t)start & 0x1FFF);
}

uint32_t LinetimeWm(uint32_t htotal, uint32_t pixel_rate_khz) {
    // skl_linetime_wm: div_round_up(htotal * 1000 * 8, pixel rate), max 0x1ff (satoru)
    uint32_t v = (uint32_t)div_round_up((uint64_t)htotal * 1000 * 8, pixel_rate_khz);
    return v > 0x1FF ? 0x1FF : v;
}

uint32_t MbusDboxCtl(bool joined_mbus, bool only_pipe_in_bank) {
    // pipe_mbus_dbox_ctl, display 14.00 (skl_watermark_regs.h fields) (satoru)
    uint32_t v = 0;
    v |= 2u << 5;                         // mbus_dbox_i_credit(2) (satoru)
    v |= 16u << 20;                       // b2b_transactions_max(16) (satoru)
    v |= 1u << 17;                        // b2b_transactions_delay(1) (satoru)
    v |= 1u << 16;                        // regulate_b2b_transactions_en (satoru)
    v |= joined_mbus ? 12u : 8u;          // a_credit (satoru)
    v |= 0xAu << 8;                       // b_credit (satoru)
    v |= (only_pipe_in_bank ? 3u : 2u) << 14;   // bw_8credits_mtl / 4credits (satoru)
    return v;
}

bool PmDemandRaise(PmDemand* p, uint32_t active_pipes, uint32_t active_dbufs, uint32_t ddiclk_mhz) {
    // intel_display_regs.h: request(0) pipes 7:6, dbufs 5:4; request(1)
    // ddiclk_freq 18:8. update_reg keeps max(old, new, current) (satoru)
    PmDemand o = *p;
    uint32_t cur_pipes = (p->reg0 >> 6) & 3, cur_dbufs = (p->reg0 >> 4) & 3;
    uint32_t cur_ddi = (p->reg1 >> 8) & 0x7FF;
    if (active_pipes > 3) active_pipes = 3;
    if (active_dbufs > 3) active_dbufs = 3;
    uint32_t np = max_(cur_pipes, active_pipes), nd = max_(cur_dbufs, active_dbufs);
    uint32_t ndc = max_(cur_ddi, ddiclk_mhz & 0x7FF);
    p->reg0 = (p->reg0 & ~(0x3u << 6) & ~(0x3u << 4)) | (np << 6) | (nd << 4);
    p->reg1 = (p->reg1 & ~(0x7FFu << 8)) | (ndc << 8);
    return o.reg0 != p->reg0 || o.reg1 != p->reg1;
}

int LttprCount(uint8_t cnt) {
    // drm_dp_lttpr_count: one bit set -> 8 - ilog2, none -> 0, all -> -range,
    // else -einval (satoru)
    int bits = 0, top = -1;
    for (int i = 0; i < 8; i++) if (cnt & (1u << i)) { bits++; top = i; }
    if (bits == 0) return 0;
    if (bits == 1) return 8 - top;
    if (bits == 8) return -34;
    return -22;
}

uint32_t CrDelayUs(uint8_t dpcd_rev, uint8_t rd_interval, bool lttpr) {
    // __read_delay(cr=true, 8b/10b): lttpr phys and dpcd rev >= 1.4 dprx use
    // a fixed 100 us, else __8b10b_clock_recovery_delay_us (satoru)
    if (lttpr || dpcd_rev >= 0x14) return 100;
    rd_interval &= 0x7F;
    if (rd_interval == 0) return 100;
    return (uint32_t)rd_interval * 4000;
}

uint32_t EqDelayUs(uint8_t rd_interval) {
    // __8b10b_channel_eq_delay_us (dp_training_aux_rd_mask 0x7f) (satoru)
    rd_interval &= 0x7F;
    if (rd_interval == 0) return 400;
    return (uint32_t)rd_interval * 4000;
}

}

namespace IgpuCalc {

uint32_t CursorPosVal(int x, int y) {
    uint32_t v = 0;
    if (x < 0) { v |= 1u << 15; x = -x; }
    v |= (uint32_t)x & 0x7FFFu;
    if (y < 0) { v |= 1u << 31; y = -y; }
    v |= ((uint32_t)y & 0x7FFFu) << 16;
    return v;
}

bool CursorLocal(int ui_scale, bool multi, int out_x, int out_y, int out_w, int out_h,
                 int phys_w, int phys_h, int px, int py, int* lx, int* ly) {
    if (ui_scale != 100 && !multi) return false;
    if (out_w != phys_w || out_h != phys_h) return false;
    if (px < out_x || px >= out_x + out_w || py < out_y || py >= out_y + out_h) return false;
    *lx = px - out_x;
    *ly = py - out_y;
    return true;
}

}

// end (satoru)
