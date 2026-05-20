//  kurono os  -  audio format conversion (implementation)
#include "audio_format.h"

namespace AudioFormat {

const char* FormatName(SampleFormat fmt) {
    switch (fmt) {
        case FMT_U8:     return "U8";
        case FMT_S16_LE: return "S16_LE";
        case FMT_S24_LE: return "S24_LE";
        case FMT_S32_LE: return "S32_LE";
        case FMT_F32_LE: return "F32_LE";
    }
    return "?";
}

uint32_t DecodeToCanonical(SampleFormat fmt, int channels,
                           const void* src, MixSample* canonical,
                           uint32_t frame_count) {
    const uint32_t samples = frame_count * static_cast<uint32_t>(channels);
    const uint8_t* p = static_cast<const uint8_t*>(src);

    switch (fmt) {
        case FMT_U8: {
            for (uint32_t i = 0; i < samples; i++) {
                canonical[i] = CanonicalFromU8(p[i]);
            }
            return samples;
        }
        case FMT_S16_LE: {
            const int16_t* s = reinterpret_cast<const int16_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                canonical[i] = CanonicalFromS16(s[i]);
            }
            return samples * 2;
        }
        case FMT_S24_LE: {
            for (uint32_t i = 0; i < samples; i++) {
                canonical[i] = CanonicalFromS24(p + i * 3);
            }
            return samples * 3;
        }
        case FMT_S32_LE: {
            const int32_t* s = reinterpret_cast<const int32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                canonical[i] = CanonicalFromS32(s[i]);
            }
            return samples * 4;
        }
        case FMT_F32_LE: {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                canonical[i] = CanonicalFromF32Bits(s[i]);
            }
            return samples * 4;
        }
    }
    return 0;
}

uint32_t EncodeFromCanonical(SampleFormat fmt, int channels,
                             const MixSample* canonical, void* dst,
                             uint32_t frame_count) {
    const uint32_t samples = frame_count * static_cast<uint32_t>(channels);
    uint8_t* p = static_cast<uint8_t*>(dst);

    switch (fmt) {
        case FMT_U8: {
            for (uint32_t i = 0; i < samples; i++) {
                p[i] = ClampToU8(canonical[i]);
            }
            return samples;
        }
        case FMT_S16_LE: {
            int16_t* s = reinterpret_cast<int16_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                s[i] = ClampToS16(canonical[i]);
            }
            return samples * 2;
        }
        case FMT_S24_LE: {
            for (uint32_t i = 0; i < samples; i++) {
                ClampToS24(canonical[i], p + i * 3);
            }
            return samples * 3;
        }
        case FMT_S32_LE: {
            int32_t* s = reinterpret_cast<int32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                s[i] = ClampToS32(canonical[i]);
            }
            return samples * 4;
        }
        case FMT_F32_LE: {
            uint32_t* s = reinterpret_cast<uint32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                s[i] = ClampToF32Bits(canonical[i]);
            }
            return samples * 4;
        }
    }
    return 0;
}

uint32_t ConvertChannels(int in_channels, int out_channels,
                         const MixSample* src, MixSample* dst,
                         uint32_t frame_count) {
    if (in_channels == out_channels) {
        // straight copy  -  caller may have aliased src and dst
        if (src != dst) {
            const uint32_t samples = frame_count * static_cast<uint32_t>(in_channels);
            for (uint32_t i = 0; i < samples; i++) dst[i] = src[i];
        }
        return frame_count;
    }

    if (in_channels == 1 && out_channels == 2) {
        // mono -> stereo
        for (uint32_t f = frame_count; f-- > 0; ) {
            MixSample m = src[f];
            dst[f * 2 + 0] = m;
            dst[f * 2 + 1] = m;
        }
        return frame_count;
    }

    if (in_channels == 2 && out_channels == 1) {
        // stereo -> mono (average to avoid clipping)
        for (uint32_t f = 0; f < frame_count; f++) {
            MixSample l = src[f * 2 + 0];
            MixSample r = src[f * 2 + 1];
            dst[f] = (l + r) >> 1;
        }
        return frame_count;
    }

    if (in_channels == 2 && out_channels == 6) {
        // stereo -> 5.1 (FL, FR, C, LFE, RL, RR)
        for (uint32_t f = frame_count; f-- > 0; ) {
            MixSample l = src[f * 2 + 0];
            MixSample r = src[f * 2 + 1];
            dst[f * 6 + 0] = l;
            dst[f * 6 + 1] = r;
            dst[f * 6 + 2] = (l + r) >> 1;       // C  = (L+R)/2
            dst[f * 6 + 3] = ((l + r) * 181) >> 9; // LFE ~= 0.707 * (L+R)/2 * 2
            dst[f * 6 + 4] = 0;
            dst[f * 6 + 5] = 0;
        }
        return frame_count;
    }

    if (in_channels == 6 && out_channels == 2) {
        // 5.1 downmix to stereo, ITU-R BS.775 coefficients (0.707 ≈ 181/256)
        for (uint32_t f = 0; f < frame_count; f++) {
            MixSample fl = src[f * 6 + 0];
            MixSample fr = src[f * 6 + 1];
            MixSample c  = src[f * 6 + 2];
            // skip LFE (4)  -  usually low-passed sub channel, not part of L/R
            MixSample rl = src[f * 6 + 4];
            MixSample rr = src[f * 6 + 5];
            int64_t L = static_cast<int64_t>(fl) +
                        ((static_cast<int64_t>(c)  * 181) >> 8) +
                        ((static_cast<int64_t>(rl) * 181) >> 8);
            int64_t R = static_cast<int64_t>(fr) +
                        ((static_cast<int64_t>(c)  * 181) >> 8) +
                        ((static_cast<int64_t>(rr) * 181) >> 8);
            if (L > kMixMax) L = kMixMax; else if (L < kMixMin) L = kMixMin;
            if (R > kMixMax) R = kMixMax; else if (R < kMixMin) R = kMixMin;
            dst[f * 2 + 0] = static_cast<MixSample>(L);
            dst[f * 2 + 1] = static_cast<MixSample>(R);
        }
        return frame_count;
    }

    // Generic fallback: replicate first channel into all outs (loses info,
    // but never crashes, never returns garbage).
    for (uint32_t f = 0; f < frame_count; f++) {
        MixSample v = src[f * static_cast<uint32_t>(in_channels)];
        for (int c = 0; c < out_channels; c++) {
            dst[f * static_cast<uint32_t>(out_channels) + c] = v;
        }
    }
    return frame_count;
}

uint32_t ResampleLinear(int channels,
                        uint32_t rate_in, uint32_t rate_out,
                        const MixSample* src, uint32_t in_frames,
                        MixSample* dst, uint32_t dst_capacity_frames) {
    if (rate_in == 0 || rate_out == 0 || channels <= 0) return 0;
    if (in_frames == 0)  return 0;

    if (rate_in == rate_out) {
        uint32_t to_copy = in_frames < dst_capacity_frames ? in_frames : dst_capacity_frames;
        const uint32_t samples = to_copy * static_cast<uint32_t>(channels);
        for (uint32_t i = 0; i < samples; i++) dst[i] = src[i];
        return to_copy;
    }

    // Phase accumulator in 32.32 fixed point.  step = rate_in / rate_out.
    // Output frame n samples input frame at position n * step.
    const uint64_t step = (static_cast<uint64_t>(rate_in) << 32) / rate_out;
    uint64_t phase = 0;
    uint32_t out_frames = 0;

    while (out_frames < dst_capacity_frames) {
        uint64_t i_idx   = phase >> 32;
        uint32_t i_frac  = static_cast<uint32_t>(phase & 0xFFFFFFFFu);
        if (i_idx + 1 >= in_frames) break;

        // 16-bit fractional weight in [0, 65536]
        uint32_t w_hi = i_frac >> 16;
        uint32_t w_lo = 0x10000 - w_hi;

        for (int c = 0; c < channels; c++) {
            MixSample a = src[i_idx       * static_cast<uint64_t>(channels) + c];
            MixSample b = src[(i_idx + 1) * static_cast<uint64_t>(channels) + c];
            int64_t lerp = (static_cast<int64_t>(a) * w_lo +
                            static_cast<int64_t>(b) * w_hi) >> 16;
            dst[out_frames * static_cast<uint32_t>(channels) + c] =
                static_cast<MixSample>(lerp);
        }
        out_frames++;
        phase += step;
    }
    return out_frames;
}

uint32_t Convert(SampleFormat in_fmt,  int in_ch,  uint32_t in_rate,
                 const void* in,  uint32_t in_bytes,
                 SampleFormat out_fmt, int out_ch, uint32_t out_rate,
                 void* out, uint32_t out_capacity_bytes) {
    if (!in || !out || in_ch <= 0 || out_ch <= 0) return 0;

    static MixSample scratch_a[16384];   // 16 K samples  -  64 KB at 4 B
    static MixSample scratch_b[16384];

    const uint32_t in_frame_bytes = FrameSize(in_fmt, in_ch);
    if (in_frame_bytes == 0) return 0;
    uint32_t in_frames = in_bytes / in_frame_bytes;
    if (in_frames == 0) return 0;

    // Cap input frames to scratch capacity (decode buffer is in-channels).
    if (in_frames * static_cast<uint32_t>(in_ch) > 16384) {
        in_frames = 16384 / static_cast<uint32_t>(in_ch);
    }

    // 1. Decode to canonical
    DecodeToCanonical(in_fmt, in_ch, in, scratch_a, in_frames);

    MixSample* cur     = scratch_a;
    MixSample* other   = scratch_b;
    uint32_t   frames  = in_frames;
    int        cur_ch  = in_ch;

    // 2. Channel convert (only if needed)
    if (cur_ch != out_ch) {
        ConvertChannels(cur_ch, out_ch, cur, other, frames);
        MixSample* t = cur; cur = other; other = t;
        cur_ch = out_ch;
    }

    // 3. Sample rate convert (only if needed)
    if (in_rate != out_rate) {
        const uint32_t cap_frames = 16384 / static_cast<uint32_t>(cur_ch);
        uint32_t produced = ResampleLinear(cur_ch, in_rate, out_rate,
                                           cur, frames, other, cap_frames);
        MixSample* t = cur; cur = other; other = t;
        frames = produced;
    }

    // 4. Encode to output format
    const uint32_t out_frame_bytes = FrameSize(out_fmt, out_ch);
    if (out_frame_bytes == 0) return 0;
    if (frames * out_frame_bytes > out_capacity_bytes) {
        frames = out_capacity_bytes / out_frame_bytes;
    }
    return EncodeFromCanonical(out_fmt, out_ch, cur, out, frames);
}

} // namespace AudioFormat
