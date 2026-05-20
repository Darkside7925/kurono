// kurono os  -  h.264 bitstream metadata parser
// see h264_parse.h for the api surface.
//
// implements:
//   * annex-b start-code scanner with leading-zero coalescing
//   * mp4 length-prefixed nal walker (1 / 2 / 4 byte length)
//   * emulation-prevention byte removal (0x000003 → 0x0000)
//   * exp-golomb (ue / se) decoders
//   * sps parser per iso/iec 14496-10 §7.3.2.1.1
//   * avcC blob parser per iso/iec 14496-15 §5.2.4.1
//
// freestanding, integer-only.
#include "h264_parse.h"
#include "../kernel/heap.h"

namespace H264 {

// ----------------------------------------------------------------------
// emulation-prevention byte removal  -  many parsers do this in-place but
// we want immutable input, so we copy into a small heap scratch buffer
// and operate from there.  the rbsp is always shorter than the ebsp,
// so the same length is a safe upper bound.
// ----------------------------------------------------------------------
static uint8_t* StripEPB(const uint8_t* ebsp, uint32_t in_len, uint32_t* out_len) {
    uint8_t* out = (uint8_t*)KernelHeap::Alloc(in_len ? in_len : 1);
    if (!out) { *out_len = 0; return nullptr; }
    uint32_t j = 0;
    for (uint32_t i = 0; i < in_len; ) {
        if (i + 2 < in_len && ebsp[i] == 0 && ebsp[i + 1] == 0 && ebsp[i + 2] == 3) {
            out[j++] = 0;
            out[j++] = 0;
            i += 3;
        } else {
            out[j++] = ebsp[i++];
        }
    }
    *out_len = j;
    return out;
}

// ----------------------------------------------------------------------
// bit reader for exp-golomb codes
// ----------------------------------------------------------------------
struct BitReader {
    const uint8_t* data;
    uint32_t       size;
    uint32_t       bit_pos;

    bool Empty() const { return bit_pos >= size * 8; }

    uint32_t U1() {
        if (bit_pos >= size * 8) return 0;
        uint32_t v = (data[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1;
        bit_pos++;
        return v;
    }
    uint32_t U(uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++) v = (v << 1) | U1();
        return v;
    }
    uint32_t UE() {
        // count leading zeros
        uint32_t zeros = 0;
        while (!Empty() && U1() == 0 && zeros < 32) zeros++;
        if (zeros == 0) return 0;
        uint32_t v = U(zeros);
        return (1u << zeros) - 1u + v;
    }
    int32_t SE() {
        uint32_t k = UE();
        if (k & 1) return (int32_t)((k + 1) >> 1);
        return -(int32_t)(k >> 1);
    }
};

// ----------------------------------------------------------------------
// scaling list skipper (used by SPS for high-profile encodings)
// ----------------------------------------------------------------------
static void SkipScalingList(BitReader& br, uint32_t size) {
    int32_t last_scale = 8, next_scale = 8;
    for (uint32_t i = 0; i < size; i++) {
        if (next_scale != 0) {
            int32_t delta = br.SE();
            next_scale = (last_scale + delta + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

// ----------------------------------------------------------------------
// annex-b walker
// ----------------------------------------------------------------------
uint32_t WalkAnnexB(const uint8_t* data, uint32_t size,
                    NalCallback cb, void* user) {
    if (!data || size < 4) return 0;
    uint32_t i = 0; uint32_t emitted = 0;
    auto IsStart3 = [&](uint32_t k) {
        return k + 2 < size && data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1;
    };
    auto IsStart4 = [&](uint32_t k) {
        return k + 3 < size && data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 0 && data[k + 3] == 1;
    };

    // find first start code
    while (i + 3 < size && !IsStart3(i) && !IsStart4(i)) i++;
    while (i < size) {
        uint32_t sc = IsStart4(i) ? 4 : (IsStart3(i) ? 3 : 0);
        if (!sc) break;
        uint32_t nal_start = i + sc;
        // find next start code
        uint32_t j = nal_start;
        while (j + 3 < size && !IsStart3(j) && !IsStart4(j)) j++;
        uint32_t nal_end = (j + 3 >= size) ? size : j;
        if (nal_end <= nal_start) break;
        uint8_t nal_hdr = data[nal_start];
        NalType type    = (NalType)(nal_hdr & 0x1f);
        const uint8_t* ebsp = data + nal_start + 1;
        uint32_t ebsp_len   = nal_end - nal_start - 1;
        uint32_t rbsp_len = 0;
        uint8_t* rbsp = StripEPB(ebsp, ebsp_len, &rbsp_len);
        bool keep_going = true;
        if (rbsp) {
            keep_going = cb(type, rbsp, rbsp_len, user);
            KernelHeap::Free(rbsp);
        }
        emitted++;
        if (!keep_going) return emitted;
        i = j;
    }
    return emitted;
}

// ----------------------------------------------------------------------
// length-prefixed walker
// ----------------------------------------------------------------------
uint32_t WalkLengthPrefixed(const uint8_t* data, uint32_t size,
                            uint32_t nal_length_size,
                            NalCallback cb, void* user) {
    if (!data || nal_length_size == 0 || nal_length_size > 4) return 0;
    uint32_t i = 0, emitted = 0;
    while (i + nal_length_size <= size) {
        uint32_t nal_size = 0;
        for (uint32_t k = 0; k < nal_length_size; k++) {
            nal_size = (nal_size << 8) | data[i + k];
        }
        i += nal_length_size;
        if (nal_size == 0) continue;
        if (i + nal_size > size) break;
        uint8_t nal_hdr = data[i];
        NalType type    = (NalType)(nal_hdr & 0x1f);
        const uint8_t* ebsp = data + i + 1;
        uint32_t ebsp_len   = nal_size - 1;
        uint32_t rbsp_len = 0;
        uint8_t* rbsp = StripEPB(ebsp, ebsp_len, &rbsp_len);
        bool keep_going = true;
        if (rbsp) {
            keep_going = cb(type, rbsp, rbsp_len, user);
            KernelHeap::Free(rbsp);
        }
        emitted++;
        if (!keep_going) return emitted;
        i += nal_size;
    }
    return emitted;
}

// ----------------------------------------------------------------------
// sps parser
// ----------------------------------------------------------------------
bool ParseSPS(const uint8_t* rbsp, uint32_t rbsp_len, SPS& out) {
    if (!rbsp || rbsp_len < 4) return false;
    out = SPS{};
    out.profile_idc      = rbsp[0];
    out.constraint_flags = rbsp[1];
    out.level_idc        = rbsp[2];
    BitReader br{rbsp + 3, rbsp_len - 3, 0};
    (void)br.UE();                                  // seq_parameter_set_id
    out.chroma_format_idc = 1;                      // default 4:2:0
    out.bit_depth_luma_minus8   = 0;
    out.bit_depth_chroma_minus8 = 0;
    if (out.profile_idc == 100 || out.profile_idc == 110 ||
        out.profile_idc == 122 || out.profile_idc == 244 ||
        out.profile_idc ==  44 || out.profile_idc ==  83 ||
        out.profile_idc ==  86 || out.profile_idc == 118 ||
        out.profile_idc == 128 || out.profile_idc == 138 ||
        out.profile_idc == 139 || out.profile_idc == 134) {
        out.chroma_format_idc = br.UE();
        if (out.chroma_format_idc == 3) (void)br.U1(); // separate_colour_plane
        out.bit_depth_luma_minus8   = br.UE();
        out.bit_depth_chroma_minus8 = br.UE();
        (void)br.U1();                              // qpprime_y_zero_transform_bypass
        uint32_t seq_scaling_present = br.U1();
        if (seq_scaling_present) {
            uint32_t lim = (out.chroma_format_idc == 3) ? 12 : 8;
            for (uint32_t i = 0; i < lim; i++) {
                if (br.U1()) SkipScalingList(br, (i < 6) ? 16 : 64);
            }
        }
    }
    (void)br.UE();                                  // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = br.UE();
    if (pic_order_cnt_type == 0) {
        (void)br.UE();                              // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        (void)br.U1();                              // delta_pic_order_always_zero_flag
        (void)br.SE();                              // offset_for_non_ref_pic
        (void)br.SE();                              // offset_for_top_to_bottom_field
        uint32_t n = br.UE();                       // num_ref_frames_in_pic_order_cnt_cycle
        for (uint32_t i = 0; i < n && i < 256; i++) (void)br.SE();
    }
    (void)br.UE();                                  // max_num_ref_frames
    (void)br.U1();                                  // gaps_in_frame_num_value_allowed
    uint32_t pic_w_in_mbs_minus1     = br.UE();
    uint32_t pic_h_in_map_units_min1 = br.UE();
    out.frame_mbs_only = (br.U1() != 0);
    if (!out.frame_mbs_only) (void)br.U1();         // mb_adaptive_frame_field
    (void)br.U1();                                  // direct_8x8_inference
    uint32_t frame_cropping = br.U1();
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (frame_cropping) {
        crop_l = br.UE();
        crop_r = br.UE();
        crop_t = br.UE();
        crop_b = br.UE();
    }
    out.pic_width  = (pic_w_in_mbs_minus1 + 1u) * 16u;
    out.pic_height = (pic_h_in_map_units_min1 + 1u) * 16u *
                     (out.frame_mbs_only ? 1u : 2u);
    // sub-width/height per chroma_format (we only use this for cropping)
    uint32_t sub_w = (out.chroma_format_idc == 1 || out.chroma_format_idc == 2) ? 2 : 1;
    uint32_t sub_h = (out.chroma_format_idc == 1) ? 2 : 1;
    if (out.chroma_format_idc == 0) { sub_w = 1; sub_h = 1; }
    out.pic_width  -= sub_w * (crop_l + crop_r);
    uint32_t crop_v_factor = sub_h * (out.frame_mbs_only ? 1u : 2u);
    out.pic_height -= crop_v_factor * (crop_t + crop_b);

    // vui parameters (optional  -  used for fps)
    out.timing_info_present = false;
    if (!br.Empty() && br.U1()) {
        // vui_parameters
        if (br.U1()) {                              // aspect_ratio_info_present
            uint32_t ar_idc = br.U(8);
            if (ar_idc == 255) { (void)br.U(16); (void)br.U(16); }
        }
        if (br.U1()) (void)br.U1();                 // overscan_info_present
        if (br.U1()) {                              // video_signal_type_present
            (void)br.U(3);                          // video_format
            (void)br.U1();                          // video_full_range
            if (br.U1()) {                          // colour_description_present
                (void)br.U(8); (void)br.U(8); (void)br.U(8);
            }
        }
        if (br.U1()) { (void)br.UE(); (void)br.UE(); } // chroma_loc_info
        if (br.U1()) {                              // timing_info_present
            uint32_t num_units = br.U(32);
            uint32_t time_scale = br.U(32);
            uint32_t fixed = br.U1();
            out.timing_info_present = true;
            out.num_units_in_tick   = num_units;
            out.time_scale          = time_scale;
            out.fixed_frame_rate    = (fixed != 0);
        }
    }
    return true;
}

// ----------------------------------------------------------------------
// avcC parser
// ----------------------------------------------------------------------
bool ParseAvcC(const uint8_t* avcC, uint32_t avcC_len,
               StreamInfo& info, uint8_t* out_nal_length_size) {
    if (!avcC || avcC_len < 7) return false;
    info = StreamInfo{};
    *out_nal_length_size = (uint8_t)((avcC[4] & 0x03) + 1);
    uint32_t num_sps = avcC[5] & 0x1f;
    uint32_t i = 6;
    bool got_sps = false;
    for (uint32_t k = 0; k < num_sps && i + 2 <= avcC_len; k++) {
        uint16_t sps_len = ((uint16_t)avcC[i] << 8) | avcC[i + 1];
        i += 2;
        if (i + sps_len > avcC_len) break;
        // strip leading nal header (1 byte) before passing to ParseSPS
        if (sps_len >= 1) {
            uint32_t rbsp_len = 0;
            uint8_t* rbsp = StripEPB(avcC + i + 1, sps_len - 1, &rbsp_len);
            if (rbsp) {
                if (ParseSPS(rbsp, rbsp_len, info.sps)) {
                    got_sps    = true;
                    info.valid = true;
                    if (info.sps.timing_info_present &&
                        info.sps.num_units_in_tick != 0) {
                        info.fps_num = info.sps.time_scale;
                        info.fps_den = 2u * info.sps.num_units_in_tick;
                    }
                }
                KernelHeap::Free(rbsp);
            }
        }
        i += sps_len;
    }
    return got_sps;
}

const char* ProfileName(uint8_t profile_idc) {
    switch (profile_idc) {
        case  66: return "Baseline";
        case  77: return "Main";
        case  88: return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 244: return "High 4:4:4 Predictive";
        case  44: return "CAVLC 4:4:4 Intra";
        default:  return "Unknown";
    }
}

} // namespace H264
