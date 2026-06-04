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

static int32_t g_u8_table[256];
static bool    g_u8_table_ready = false;

static inline void EnsureU8Table() {
    if (g_u8_table_ready) return;
    for (int i = 0; i < 256; i++) {
        int8_t centred = static_cast<int8_t>(static_cast<uint8_t>(i) ^ 0x80);
        g_u8_table[i] = static_cast<int32_t>(centred) << 16;
    }
    g_u8_table_ready = true;
}

uint32_t DecodeToCanonical(SampleFormat fmt, int channels,
                           const void* src, MixSample* canonical,
                           uint32_t frame_count) {
    const uint32_t samples = frame_count * static_cast<uint32_t>(channels);
    const uint8_t* __restrict__ p = static_cast<const uint8_t*>(src);
    MixSample* __restrict__ d = canonical;

    switch (fmt) {
        case FMT_U8: {
            EnsureU8Table();
            const int32_t* __restrict__ tbl = g_u8_table;
            uint32_t i = 0;
            // unroll by 4 to give the compiler something to vectorise
            for (; i + 4 <= samples; i += 4) {
                d[i + 0] = tbl[p[i + 0]];
                d[i + 1] = tbl[p[i + 1]];
                d[i + 2] = tbl[p[i + 2]];
                d[i + 3] = tbl[p[i + 3]];
            }
            for (; i < samples; i++) d[i] = tbl[p[i]];
            return samples;
        }
        case FMT_S16_LE: {
            const int16_t* __restrict__ s = reinterpret_cast<const int16_t*>(p);
            uint32_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                d[i + 0] = static_cast<int32_t>(s[i + 0]) << 8;
                d[i + 1] = static_cast<int32_t>(s[i + 1]) << 8;
                d[i + 2] = static_cast<int32_t>(s[i + 2]) << 8;
                d[i + 3] = static_cast<int32_t>(s[i + 3]) << 8;
            }
            for (; i < samples; i++) d[i] = static_cast<int32_t>(s[i]) << 8;
            return samples * 2;
        }
        case FMT_S24_LE: {
            for (uint32_t i = 0; i < samples; i++) {
                const uint8_t* q = p + i * 3;
                int32_t v = static_cast<int32_t>(q[0]) |
                            (static_cast<int32_t>(q[1]) << 8) |
                            (static_cast<int32_t>(static_cast<int8_t>(q[2])) << 16);
                d[i] = v;
            }
            return samples * 3;
        }
        case FMT_S32_LE: {
            const int32_t* __restrict__ s = reinterpret_cast<const int32_t*>(p);
            uint32_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                d[i + 0] = s[i + 0] >> 8;
                d[i + 1] = s[i + 1] >> 8;
                d[i + 2] = s[i + 2] >> 8;
                d[i + 3] = s[i + 3] >> 8;
            }
            for (; i < samples; i++) d[i] = s[i] >> 8;
            return samples * 4;
        }
        case FMT_F32_LE: {
            const uint32_t* __restrict__ s = reinterpret_cast<const uint32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                d[i] = CanonicalFromF32Bits(s[i]);
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
    uint8_t* __restrict__ p = static_cast<uint8_t*>(dst);
    const MixSample* __restrict__ s = canonical;

    switch (fmt) {
        case FMT_U8: {
            uint32_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                p[i + 0] = ClampToU8(s[i + 0]);
                p[i + 1] = ClampToU8(s[i + 1]);
                p[i + 2] = ClampToU8(s[i + 2]);
                p[i + 3] = ClampToU8(s[i + 3]);
            }
            for (; i < samples; i++) p[i] = ClampToU8(s[i]);
            return samples;
        }
        case FMT_S16_LE: {
            int16_t* __restrict__ o = reinterpret_cast<int16_t*>(p);
            uint32_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                o[i + 0] = ClampToS16(s[i + 0]);
                o[i + 1] = ClampToS16(s[i + 1]);
                o[i + 2] = ClampToS16(s[i + 2]);
                o[i + 3] = ClampToS16(s[i + 3]);
            }
            for (; i < samples; i++) o[i] = ClampToS16(s[i]);
            return samples * 2;
        }
        case FMT_S24_LE: {
            for (uint32_t i = 0; i < samples; i++) {
                ClampToS24(s[i], p + i * 3);
            }
            return samples * 3;
        }
        case FMT_S32_LE: {
            int32_t* __restrict__ o = reinterpret_cast<int32_t*>(p);
            uint32_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                o[i + 0] = ClampToS32(s[i + 0]);
                o[i + 1] = ClampToS32(s[i + 1]);
                o[i + 2] = ClampToS32(s[i + 2]);
                o[i + 3] = ClampToS32(s[i + 3]);
            }
            for (; i < samples; i++) o[i] = ClampToS32(s[i]);
            return samples * 4;
        }
        case FMT_F32_LE: {
            uint32_t* __restrict__ o = reinterpret_cast<uint32_t*>(p);
            for (uint32_t i = 0; i < samples; i++) {
                o[i] = ClampToF32Bits(s[i]);
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
        if (src != dst) {
            const uint32_t samples = frame_count * static_cast<uint32_t>(in_channels);
            for (uint32_t i = 0; i < samples; i++) dst[i] = src[i];
        }
        return frame_count;
    }

    if (in_channels == 1 && out_channels == 2) {
        // mono -> stereo; walk backwards so the routine is safe even when
        // src and dst alias (dst[f*2] can overwrite src[f] for f > 0).
        for (uint32_t f = frame_count; f-- > 0; ) {
            MixSample m = src[f];
            dst[f * 2 + 0] = m;
            dst[f * 2 + 1] = m;
        }
        return frame_count;
    }

    if (in_channels == 2 && out_channels == 1) {
        for (uint32_t f = 0; f < frame_count; f++) {
            MixSample l = src[f * 2 + 0];
            MixSample r = src[f * 2 + 1];
            dst[f] = (l + r) >> 1;
        }
        return frame_count;
    }

    if (in_channels == 2 && out_channels == 6) {
        for (uint32_t f = frame_count; f-- > 0; ) {
            MixSample l = src[f * 2 + 0];
            MixSample r = src[f * 2 + 1];
            MixSample mix = (l + r) >> 1;
            dst[f * 6 + 0] = l;
            dst[f * 6 + 1] = r;
            dst[f * 6 + 2] = mix;
            dst[f * 6 + 3] = (mix * 181) >> 8;
            dst[f * 6 + 4] = 0;
            dst[f * 6 + 5] = 0;
        }
        return frame_count;
    }

    if (in_channels == 6 && out_channels == 2) {
        for (uint32_t f = 0; f < frame_count; f++) {
            MixSample fl = src[f * 6 + 0];
            MixSample fr = src[f * 6 + 1];
            MixSample c  = src[f * 6 + 2];
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

    // Generic fallback: replicate first input channel into all outs.
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

    if (in_frames < 2) return 0;  // need at least 2 samples to lerp
    const uint64_t step = (static_cast<uint64_t>(rate_in) << 32) / rate_out;
    uint64_t phase = 0;
    uint32_t out_frames = 0;
    const uint32_t max_idx = in_frames - 1;     // last valid i_idx for the lerp pair

    // Hoist channel-count specialisation: stereo is the hot path.
    if (channels == 2) {
        while (out_frames < dst_capacity_frames) {
            uint32_t i_idx  = static_cast<uint32_t>(phase >> 32);
            if (i_idx >= max_idx) break;
            uint32_t i_frac = static_cast<uint32_t>(phase & 0xFFFFFFFFu);
            uint32_t w_hi = i_frac >> 16;
            uint32_t w_lo = 0x10000 - w_hi;
            const MixSample* p = src + i_idx * 2;
            int64_t l = (static_cast<int64_t>(p[0]) * w_lo +
                         static_cast<int64_t>(p[2]) * w_hi) >> 16;
            int64_t r = (static_cast<int64_t>(p[1]) * w_lo +
                         static_cast<int64_t>(p[3]) * w_hi) >> 16;
            dst[out_frames * 2 + 0] = static_cast<MixSample>(l);
            dst[out_frames * 2 + 1] = static_cast<MixSample>(r);
            out_frames++;
            phase += step;
        }
        return out_frames;
    }

    while (out_frames < dst_capacity_frames) {
        uint32_t i_idx  = static_cast<uint32_t>(phase >> 32);
        if (i_idx >= max_idx) break;
        uint32_t i_frac = static_cast<uint32_t>(phase & 0xFFFFFFFFu);
        uint32_t w_hi = i_frac >> 16;
        uint32_t w_lo = 0x10000 - w_hi;
        for (int c = 0; c < channels; c++) {
            MixSample a = src[i_idx       * static_cast<uint32_t>(channels) + c];
            MixSample b = src[(i_idx + 1) * static_cast<uint32_t>(channels) + c];
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

    static MixSample scratch_a[16384];
    static MixSample scratch_b[16384];

    const uint32_t in_frame_bytes = FrameSize(in_fmt, in_ch);
    if (in_frame_bytes == 0) return 0;
    uint32_t in_frames = in_bytes / in_frame_bytes;
    if (in_frames == 0) return 0;

    if (in_frames * static_cast<uint32_t>(in_ch) > 16384) {
        in_frames = 16384 / static_cast<uint32_t>(in_ch);
    }

    DecodeToCanonical(in_fmt, in_ch, in, scratch_a, in_frames);

    MixSample* cur     = scratch_a;
    MixSample* other   = scratch_b;
    uint32_t   frames  = in_frames;
    int        cur_ch  = in_ch;

    if (cur_ch != out_ch) {
        ConvertChannels(cur_ch, out_ch, cur, other, frames);
        MixSample* t = cur; cur = other; other = t;
        cur_ch = out_ch;
    }

    if (in_rate != out_rate) {
        const uint32_t cap_frames = 16384 / static_cast<uint32_t>(cur_ch);
        uint32_t produced = ResampleLinear(cur_ch, in_rate, out_rate,
                                           cur, frames, other, cap_frames);
        MixSample* t = cur; cur = other; other = t;
        frames = produced;
    }

    const uint32_t out_frame_bytes = FrameSize(out_fmt, out_ch);
    if (out_frame_bytes == 0) return 0;
    if (frames * out_frame_bytes > out_capacity_bytes) {
        frames = out_capacity_bytes / out_frame_bytes;
    }
    return EncodeFromCanonical(out_fmt, out_ch, cur, out, frames);
}

} // namespace AudioFormat
