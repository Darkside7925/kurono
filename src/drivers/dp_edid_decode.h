#pragma once
#include <stdint.h>

// displayport dpcd and edid decoders for the intel stage 1 aux probe. pure
// functions over byte arrays: no mmio, no kernel calls, no libc, so the same
// file builds on the host for the offline unit test. (satoru)
//
// field layouts: include/drm/display/drm_dp.h (dpcd), drivers/gpu/drm/
// drm_edid.c (edid base block, cta-861 and displayid type i / vii detailed
// timings, see drm_mode_detailed and drm_mode_displayid_detailed). (satoru)
namespace DpEdid {

// one output line, without a trailing newline (satoru)
typedef void (*LineFn)(void* ctx, const char* line);

struct Dpcd {
    int n;                      // bytes decoded (satoru)
    uint8_t rev_major, rev_minor;
    uint8_t link_rate_code;     // dpcd 0x001 (satoru)
    uint32_t link_rate_mbps;    // per lane, 0 when the code is unknown (satoru)
    uint8_t max_lanes;
    bool tps3, tps4, enhanced_frame, post_lt_adj;
    bool downspread, no_aux_handshake;
    bool dfp_present;
    uint8_t dfp_type, dfp_count;
    bool coding_8b10b, coding_128b132b;
    bool msa_ignored, oui;
    uint8_t edp_cfg;
    uint8_t aux_rd_interval;
    bool ext_caps;              // dpcd 0x00e bit 7: real caps live at 0x2200 (satoru)
};

// decode dpcd 0x000.. (at least 15 bytes for every field). (satoru)
void DecodeDpcd(const uint8_t* raw, int n, Dpcd* d);
// dpcd 0x200 sink count, the 7 bit value split over bits 7 and 5:0 (satoru)
int SinkCount(uint8_t v);
// link rate code to "rbr" / "hbr" / "hbr2" / "hbr3" / "?" (satoru)
const char* LinkRateName(uint8_t code);
// format the decoded caps as lines (satoru)
void FormatDpcd(const Dpcd& d, LineFn fn, void* ctx);

enum TimingSource : uint8_t { SRC_BASE_DTD = 0, SRC_CTA_DTD = 1, SRC_DID_T1 = 2, SRC_DID_T7 = 3 };

struct Timing {
    uint32_t pixclk_khz;
    uint16_t hact, hblank, hfp, hsync;
    uint16_t vact, vblank, vfp, vsync;
    uint16_t hmm, vmm;
    bool interlaced, hpos, vpos, preferred;
    uint8_t source;             // TimingSource (satoru)
    uint8_t block;              // edid block it came from (satoru)
    uint32_t refresh_mhz;       // millihertz (satoru)
};

struct StdTiming { uint16_t w, h; uint8_t hz; };

constexpr int MAX_TIMINGS = 32;
constexpr int MAX_BLOCKS = 4;

struct Edid {
    int blocks;                 // blocks handed to DecodeEdid (satoru)
    bool header_ok;
    bool sum_ok[MAX_BLOCKS];
    char mfg[4];
    uint16_t product;
    uint32_t serial;
    uint8_t week;
    uint16_t year;
    uint8_t ver, rev;
    bool digital;
    uint8_t bpc;                // 0 = undefined (satoru)
    uint8_t iface;              // 1 dvi, 2 hdmi-a, 3 hdmi-b, 4 mddi, 5 dp (satoru)
    uint8_t hcm, vcm;
    char name[14];
    char serial_str[14];
    bool has_range;
    uint16_t vmin_hz, vmax_hz, hmin_khz, hmax_khz, max_pclk_mhz;
    uint8_t ext_count;
    uint8_t ext_tag[MAX_BLOCKS];
    StdTiming std[8];
    int nstd;
    Timing t[MAX_TIMINGS];
    int nt;
    int preferred;              // index into t, -1 if none (satoru)
    // cta-861 (first cta block only) (satoru)
    bool cta;
    uint8_t cta_rev, cta_flags;
    uint8_t vic[32];
    int nvic;
    bool hdmi_vsdb, hf_vsdb;
    // displayid (satoru)
    bool did;
    uint8_t did_ver;
};

// decode len bytes of edid (a multiple of 128, base block first). returns
// false when the base block header is wrong. (satoru)
bool DecodeEdid(const uint8_t* data, int len, Edid* e);
// "2560x1440@143.912 pixclk 586590khz h 2560 48 32 80 (2720) v 1440 3 5 33
// (1481) +h +v" (satoru)
void FormatTiming(const Timing& t, char* buf, int max);
// the timing with the highest refresh at the preferred timing's resolution,
// -1 if there is no preferred timing (satoru)
int MaxRefreshAtPreferred(const Edid& e);
// the timing at the preferred timing's resolution whose refresh (millihertz)
// is closest to target_mhz. ties go to the lower pixel clock. falls back to
// the preferred timing when that resolution has no other entry. (satoru)
int PickTimingAtResolution(const Edid& e, uint32_t target_mhz);
// distinct integer refresh rates the edid offers at the preferred timing's
// resolution, ascending. returns the count written (<= max). (satoru)
int RatesAtPreferred(const Edid& e, uint32_t* hz, int max);
// the largest progressive timing (by active area) that fits(t, ctx)
// accepts; ties go to the refresh closest to target_mhz, then the lower
// pixel clock. -1 when none fits. used when the preferred timing is more
// than the link or cdclk can carry. (satoru)
int PickFittingTiming(const Edid& e, uint32_t target_mhz, bool (*fits)(const Timing& t, void* ctx), void* ctx);
// format the decoded edid as lines (satoru)
void FormatEdid(const Edid& e, LineFn fn, void* ctx);

// tiny freestanding text helpers shared with the aux probe (satoru)
struct Txt {
    char* b;
    int max, n;
    Txt(char* buf, int m) : b(buf), max(m), n(0) { if (max > 0) b[0] = 0; }
    Txt& s(const char* t);
    Txt& c(char ch);
    Txt& d(uint64_t v);
    Txt& x(uint32_t v, int digits);
    Txt& hz(uint32_t mhz);      // millihertz as "143.912" (satoru)
};

}

// end (satoru)
