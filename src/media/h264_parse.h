// kurono os  -  h.264 (avc) bitstream metadata parser
// =================================================================
// parses just enough of an h.264 elementary stream to expose codec
// metadata: profile, level, picture width/height, chroma format,
// frame rate (when an in-band vui block is present), and per-NALU
// type / boundaries.
//
// this is deliberately NOT a decoder.  it lets the os tell the user
// "this video is h.264 high profile, 1280x720, 24 fps" and lets a
// future decoder skip the hand-wavy ffmpeg dance to find slices.
//
// supports both wire formats:
//   * "annex b" byte streams  -  start codes 0x000001 / 0x00000001
//   * "avcC" length-prefixed format used inside mp4  -  1/2/3/4-byte
//     length fields followed by the nal payload
//
// integer-only, freestanding-clean.
#pragma once
#include "../kernel/types.h"

namespace H264 {

enum NalType : uint8_t {
    NAL_SLICE        = 1,
    NAL_SLICE_DPA    = 2,
    NAL_SLICE_DPB    = 3,
    NAL_SLICE_DPC    = 4,
    NAL_IDR          = 5,    // instantaneous decoder refresh (keyframe)
    NAL_SEI          = 6,
    NAL_SPS          = 7,
    NAL_PPS          = 8,
    NAL_AUD          = 9,
    NAL_END_SEQ      = 10,
    NAL_END_STREAM   = 11,
    NAL_FILLER       = 12,
    NAL_SPS_EXT      = 13,
    NAL_PREFIX       = 14,
    NAL_SUBSET_SPS   = 15,
};

struct SPS {
    uint8_t  profile_idc;
    uint8_t  level_idc;
    uint8_t  constraint_flags; // bits 0..7 = constraint_set0..7_flag
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma_minus8;
    uint32_t bit_depth_chroma_minus8;
    uint32_t pic_width;        // computed from MBs (in samples)
    uint32_t pic_height;
    bool     frame_mbs_only;
    // vui timing info (only valid when timing_info_present)
    bool     timing_info_present;
    uint32_t num_units_in_tick;
    uint32_t time_scale;       // fps = time_scale / (2 * num_units_in_tick)
    bool     fixed_frame_rate;
};

struct StreamInfo {
    bool     valid;            // true if at least one SPS was parsed
    SPS      sps;
    uint32_t fps_num;          // simplified rational fps (0 if unknown)
    uint32_t fps_den;
    uint32_t nal_count;        // total NAL units walked
    uint32_t idr_count;        // total IDR keyframes seen
};

// callback for raw NAL walk; return false to stop early.
using NalCallback = bool (*)(NalType type, const uint8_t* rbsp,
                             uint32_t rbsp_len, void* user);

// walk an annex-b byte stream (start codes), invoking cb for each NAL.
// returns the number of NALs emitted.
uint32_t WalkAnnexB(const uint8_t* data, uint32_t size,
                    NalCallback cb, void* user);

// walk a length-prefixed NAL stream (mp4 sample, esp. avcC layout).
// the nal_length_size is taken from the avcC config (1, 2 or 4).
uint32_t WalkLengthPrefixed(const uint8_t* data, uint32_t size,
                            uint32_t nal_length_size,
                            NalCallback cb, void* user);

// parse an avcC (avcDecoderConfigurationRecord) blob; on success fills
// `info` with at least one SPS.  returns true if any SPS parsed.
bool ParseAvcC(const uint8_t* avcC, uint32_t avcC_len,
               StreamInfo& info, uint8_t* out_nal_length_size);

// parse a single SPS RBSP payload.  returns true on success.
bool ParseSPS(const uint8_t* rbsp, uint32_t rbsp_len, SPS& out);

// human-readable profile name
const char* ProfileName(uint8_t profile_idc);

} // namespace H264
