#pragma once
#include <stdint.h>

// intel display stage 2: displayport 8b/10b link training state machine, a
// transcription of drivers/gpu/drm/i915/display/intel_dp_link_training.c
// (intel_dp_link_train_all_phys and the clock recovery / channel
// equalization loops) plus the drm_dp_helper.c status helpers. it touches no
// hardware itself: the source side (dp_tp_ctl pattern, phy swing) and the
// sink side (dpcd over aux) are callbacks, so the host unit test drives it
// against a mocked sink. (satoru)
namespace DpLt {

constexpr uint8_t TP_DISABLE = 0, TP1 = 1, TP2 = 2, TP3 = 3, TP4 = 7;
constexpr uint8_t SCRAMBLING_DISABLE = 1u << 5;

struct Ops {
    void* ctx;
    // return the byte count on success, < 0 on an aux failure (satoru)
    int (*dpcd_read)(void* ctx, uint32_t addr, uint8_t* buf, int n);
    int (*dpcd_write)(void* ctx, uint32_t addr, const uint8_t* buf, int n);
    // intel_ddi_set_link_train: program the source dp_tp_ctl pattern (satoru)
    void (*set_source_pattern)(void* ctx, uint8_t dp_train_pat);
    // encoder->set_signal_levels with the given per lane train_set (satoru)
    void (*set_source_levels)(void* ctx, const uint8_t train_set[4], int lanes);
    // intel_ddi_set_idle_link_train: dp_tp_ctl idle + wait idle_done (satoru)
    void (*set_source_idle)(void* ctx);
    void (*delay_us)(void* ctx, uint32_t us);
    void (*log)(void* ctx, const char* line);
};

struct Params {
    uint32_t rate_khz;
    int lanes;
    bool enhanced_framing;
    uint8_t dpcd[16];               // dprx receiver caps (effective, 0x2200 when ext) (satoru)
    int lttpr_count;                // 0 = transparent / none (satoru)
    uint8_t lttpr_phy_caps[8][3];   // 0xf0020 + 0x50 * (phy - 1): rd interval, tx cap, 128b rd (satoru)
    uint8_t source_vswing_max;      // dp_train_voltage_swing_level_3 for c20 dp1.4 (satoru)
    uint8_t source_preemph_max;     // dp_train_pre_emph_level_3 (satoru)
};

struct Result {
    bool ok;
    int phy_failed;                 // dp_phy that failed (0 dprx, n lttpr n), -1 none (satoru)
    const char* reason;
    int cr_loops, eq_loops;
    uint8_t status[6];              // last link status read (satoru)
    uint8_t train_set[4];
    uint8_t pattern;                // channel eq pattern used (satoru)
};

// drm_dp_helper.c (satoru)
bool ClockRecoveryOk(const uint8_t st[6], int lanes);
bool ChannelEqOk(const uint8_t st[6], int lanes);

// dpcd 0x107/0x108 (intel_dp_link_training_set_mode) and 0x100/0x101
// (intel_dp_link_training_set_bw), the dpcd half of
// intel_dp_prepare_link_train. the source half (dp_tp_ctl, ddi_buf_ctl) is
// done by the caller before this. (satoru)
bool PrepareSink(const Ops& o, const Params& p);

// intel_dp_link_train_all_phys for 8b/10b, without the post-lt adjust
// request phase (we never grant it, see the .cpp). ends with the dprx
// training pattern disabled and the source in idle; the caller then sets
// the source to normal (intel_dp_stop_link_train). (satoru)
bool TrainAllPhys(const Ops& o, const Params& p, Result* r);

}

// end (satoru)
