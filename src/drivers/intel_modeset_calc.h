#pragma once
#include <stdint.h>

// intel display stage 2 maths: pure functions with no mmio, no kernel calls
// and no libc, so the same file builds on the host for the unit test. every
// formula is a transcription of the linux function named next to it,
// torvalds/linux master, drivers/gpu/drm/i915/display/ unless noted. (satoru)
namespace IgpuCalc {

// a display mode in drm_display_mode crtc_* terms (start/end/total) (satoru)
struct Mode {
    uint32_t clock_khz;
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    bool phsync, pvsync, interlaced;
    uint32_t refresh_mhz;
};

// edid detailed timing (active, blank, front porch, sync width) to a mode,
// the way drm_mode_detailed builds hsync_start = hact + hfp etc. (satoru)
bool ModeFromDtd(uint32_t pixclk_khz, uint16_t hact, uint16_t hblank, uint16_t hfp, uint16_t hsync,
                 uint16_t vact, uint16_t vblank, uint16_t vfp, uint16_t vsync,
                 bool hpos, bool vpos, bool interlaced, Mode* m);

// ---- dp link bandwidth (intel_dp.c, drm_dp_helper.c) (satoru) ----
// intel_dp_effective_data_rate with bw_overhead 1000000 (no fec, no dsc):
// kbytes/s the stream needs (satoru)
uint32_t ModeDataRate(uint32_t pixel_khz, int bpp);
// drm_dp_max_dprx_data_rate for 8b/10b: kbytes/s the link carries (satoru)
uint32_t LinkDataRate(uint32_t link_khz, int lanes);

// intel_dp_link_required for 8b/10b sst without fec/dsc/ssc (satoru)
uint32_t LinkRequired(int lanes, uint32_t hdisplay, uint32_t pixel_khz, int bpp);

struct LinkCfg { uint32_t rate_khz; int lanes; };
// every 8b/10b config (rbr..hbr3 x 1/2/4) up to the caps that carries the
// mode, lowest rate first, then fewest lanes, the order
// intel_dp_compute_link_config_wide walks. returns the count. (satoru)
int LinkConfigs(uint32_t max_rate_khz, int max_lanes, uint32_t pixel_khz, uint32_t hdisplay, int bpp,
                LinkCfg* out, int maxn);
// drm_dp_bw_code_to_link_rate / drm_dp_link_rate_to_bw_code (satoru)
uint32_t BwCodeToRate(uint8_t code);
uint8_t RateToBwCode(uint32_t rate_khz);

// ---- m/n (intel_display.c intel_link_compute_m_n, compute_m_n) (satoru) ----
struct MN { uint32_t tu, data_m, data_n, link_m, link_n; };
void ComputeMN(int bpp, int lanes, uint32_t pixel_khz, uint32_t link_khz, MN* mn);
// the four register values intel_set_m_n writes (tu folded into data_m) (satoru)
uint32_t DataMReg(const MN& mn);

// ---- transcoder timing register values (intel_set_transcoder_timings,
// display ver 14: vblank_start written as 1, scl 0) (satoru) ----
struct TransRegs { uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, pipesrc; };
void TranscoderRegs(const Mode& m, TransRegs* r);

// ---- c20 pll (intel_cx0_phy.c mtl_c20_dp_tables, 38.4 mhz refclk) (satoru) ----
struct C20Pll { const char* name; uint32_t clock_khz; uint16_t tx[3]; uint16_t cmn[4]; uint16_t mpllb[11]; };
const C20Pll* C20DpTable(uint32_t rate_khz);       // nullptr for a rate not in the table (satoru)
uint32_t C20PortClock(const C20Pll& p);            // intel_c20pll_calc_port_clock (mpllb path) (satoru)
bool C20UsesMpllb(const C20Pll& p);                // c20_phy_use_mpllb, tx[0] bit 7 (satoru)
uint8_t C20DpRate(uint32_t rate_khz);              // intel_c20_get_dp_rate, 0xff unknown (satoru)
uint32_t LinkSymbolClock(uint32_t rate_khz);       // intel_dp_link_symbol_clock (8b/10b) (satoru)

// ---- dp signal levels (intel_ddi.c index_to_dp_signal_levels,
// intel_ddi_buf_trans.c _mtl_c20_trans_dp14) (satoru) ----
struct Snps { uint8_t vswing, pre_cursor, post_cursor; };
int TrainSetToLevel(uint8_t train_set);            // -1 if not in the table (satoru)
Snps C20Dp14Level(int level);
constexpr int C20_DP14_LEVELS = 10;

// ---- ggtt (gt/intel_ggtt.c mtl_ggtt_pte_encode, i915_pci.c MTL_CACHELEVEL) (satoru) ----
constexpr int MTL_PAT_UC = 2;                      // cachelevel_to_pat[I915_CACHE_NONE] (satoru)
uint64_t MtlGgttPte(uint64_t phys, int pat_index);
// true when a ggtt pte is a live ram mapping: present, only the lm, address and
// pat bits set, and the address inside managed ram. the firmware leaves the
// unused ggtt as stale stolen-memory contents, so this is the rule that tells a
// used window from an unused one (ggtt_setup in intel_modeset.cpp). (satoru)
bool GgttPteLive(uint64_t pte, uint64_t phys_top);

// ---- watermarks (skl_watermark.c) (satoru) ----
struct WmParams {
    uint32_t pixel_rate_khz;
    uint32_t htotal;
    uint32_t cpp;
    uint32_t dbuf_block_size;       // 512 (satoru)
    uint32_t plane_bytes_per_line;
    uint32_t blocks_per_line_fx;    // uint_fixed_16_16_t (satoru)
    uint32_t linetime_us;
};
struct WmLevel { bool enable; uint16_t blocks, lines; uint16_t min_ddb; };
constexpr uint16_t WM_REJECT = 0xFFFF;
// skl_compute_wm_params for a linear, unrotated, single plane fb (satoru)
void WmParamsLinear(uint32_t pixel_rate_khz, uint32_t htotal, uint32_t width, uint32_t cpp, WmParams* wp);
// skl_compute_plane_wm, display ver 14 (satoru)
void PlaneWm(const WmParams& wp, uint32_t latency_us, const WmLevel& prev, WmLevel* out);
// skl_compute_transition_wm, display ver 14 with ipc on (satoru)
void TransWm(const WmLevel& wm0, WmLevel* out);
// mtl_read_wm_latency raw fields -> adjust_wm_latency (ver 14, no dg2, no
// 16gb dimm wa) (satoru)
void AdjustLatency(uint16_t wm[6]);
// skl_plane_wm_reg_val (satoru)
uint32_t WmRegVal(const WmLevel& l);
// skl_plane_ddb_reg_val, end exclusive (satoru)
uint32_t DdbRegVal(uint16_t start, uint16_t end);
// skl_linetime_wm (satoru)
uint32_t LinetimeWm(uint32_t htotal, uint32_t pixel_rate_khz);
// pipe_mbus_dbox_ctl for display 14.00 (satoru)
uint32_t MbusDboxCtl(bool joined_mbus, bool only_pipe_in_bank);

// ---- pmdemand (intel_pmdemand.c intel_pmdemand_update_params, the pre
// plane max(old, new, current) rule) (satoru) ----
struct PmDemand { uint32_t reg0, reg1; };
// raises the fields of the current register pair to at least the given
// values, never lowers any. returns true when something changed. (satoru)
bool PmDemandRaise(PmDemand* p, uint32_t active_pipes, uint32_t active_dbufs, uint32_t ddiclk_mhz);

// ---- dp link training helpers (drm_dp_helper.c) (satoru) ----
int LttprCount(uint8_t repeater_cnt);              // drm_dp_lttpr_count (satoru)
uint32_t CrDelayUs(uint8_t dpcd_rev, uint8_t rd_interval_dprx, bool lttpr);   // __read_delay cr, 8b/10b (satoru)
uint32_t EqDelayUs(uint8_t rd_interval);           // __8b10b_channel_eq_delay_us (satoru)

}

// end (satoru)
