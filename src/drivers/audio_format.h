#pragma once
//  kurono os  -  audio format conversion utilities
//
//  Given:
//    * input  PCM in (rate_in, channels_in, bits_in)
//    * output PCM in (rate_out, channels_out, bits_out)
//  AudioFormat::Convert() rewrites the samples in-place / into a destination
//  buffer.  Three independent operations are layered:
//
//    1. Bit-depth conversion        (8u <-> 16s <-> 24s <-> 32s <-> f32)
//    2. Channel  conversion         (mono <-> stereo <-> 5.1 downmix)
//    3. Sample-rate conversion      (linear interpolation + integer ratios)
//
//  All math is done in the canonical internal format `int32_t` (signed
//  24-bit-in-32-bit headroom).  The intermediate format gives us 8 bits of
//  headroom for sums of up to 256 streams without saturating before the
//  final clamp.
//
//  The mixer uses `MixSample` (int32_t) as its working type.  All
//  conversions go through CanonicalFromU8/S16/S24/S32/F32 and back via
//  ClampTo*().  Sample-rate conversion uses linear interpolation when the
//  ratio is non-integer and bypasses the SRC entirely when rates match.

#include "../kernel/types.h"

namespace AudioFormat {

// Working sample type used by the software mixer and converters.
// Range: -(1 << 23) .. (1 << 23) - 1, with 8 bits of mixing headroom.
using MixSample = int32_t;

constexpr int32_t kMixMin = -(1 << 23);
constexpr int32_t kMixMax =  (1 << 23) - 1;

// Standard PCM sample formats Kurono supports as input and output.
enum SampleFormat : uint8_t {
    FMT_U8       = 0,    // unsigned 8-bit      (0..255, silence=128)
    FMT_S16_LE   = 1,    // signed 16-bit LE
    FMT_S24_LE   = 2,    // signed 24-bit packed LE (3 bytes/sample)
    FMT_S32_LE   = 3,    // signed 32-bit LE
    FMT_F32_LE   = 4,    // IEEE-754 float, normalised to [-1, +1]
};

// Returns the byte size of one sample in the given format.
constexpr int BytesPerSample(SampleFormat fmt) {
    return (fmt == FMT_U8)     ? 1 :
           (fmt == FMT_S16_LE) ? 2 :
           (fmt == FMT_S24_LE) ? 3 :
           (fmt == FMT_S32_LE) ? 4 :
           (fmt == FMT_F32_LE) ? 4 : 2;
}

// Number of *frames* per second is `sample_rate`.  Number of *samples*
// per second is `sample_rate * channels`.  These helpers keep the math
// honest at call sites that mix the two.
constexpr uint32_t FrameSize(SampleFormat fmt, int channels) {
    return static_cast<uint32_t>(BytesPerSample(fmt)) * static_cast<uint32_t>(channels);
}

// ---- format -> canonical ----
//
// CanonicalFromX(p) reads one sample from the source pointer and returns
// it as a signed 24-bit-in-32-bit MixSample.  The source pointer is
// advanced by BytesPerSample(fmt) externally.

static inline MixSample CanonicalFromU8(uint8_t v) {
    // Map [0, 255] -> [-128, 127] -> shift left 16 to fill the 24-bit range.
    return (static_cast<MixSample>(static_cast<int8_t>(v ^ 0x80))) << 16;
}

static inline MixSample CanonicalFromS16(int16_t v) {
    // Shift left 8 to fill the 24-bit range.
    return static_cast<MixSample>(v) << 8;
}

static inline MixSample CanonicalFromS24(const uint8_t* p) {
    // Little-endian 24-bit signed; sign-extend the high byte.
    int32_t v = static_cast<int32_t>(p[0]) |
                (static_cast<int32_t>(p[1]) << 8) |
                (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16);
    return v;
}

static inline MixSample CanonicalFromS32(int32_t v) {
    // Compress 32-bit signed to 24-bit signed by arithmetic right shift.
    return v >> 8;
}

// IEEE 754 single-precision -> MixSample, decoded by integer bit math
// because the kernel is compiled with -mno-sse / -mno-mmx and cannot use
// the FPU directly.  Layout: 1 sign | 8 exponent (bias 127) | 23 mantissa.
static inline MixSample CanonicalFromF32Bits(uint32_t bits) {
    uint32_t sign     = bits >> 31;
    int32_t  exp_raw  = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
    uint32_t mantissa = (bits & 0x7FFFFF) | 0x800000;   // implicit leading 1
    if ((bits & 0x7FFFFFFF) == 0) return 0;             // +/- 0
    if (exp_raw >= 0) {
        // Magnitude >= 1.0 -> clamp to full scale.
        return sign ? kMixMin : kMixMax;
    }
    // exp in [-127, -1]; shift mantissa right by (-exp_raw - 1) extra bits
    // beyond the canonical 23-bit alignment.
    int shift = -exp_raw;
    if (shift >= 24) return 0;                          // underflow
    int32_t mag = static_cast<int32_t>(mantissa >> shift);
    return sign ? -mag : mag;
}

static inline MixSample CanonicalFromF32(float v) {
    union { float f; uint32_t u; } x{}; x.f = v;
    return CanonicalFromF32Bits(x.u);
}


// ---- canonical -> format ----

static inline uint8_t  ClampToU8(MixSample s) {
    if (s < kMixMin) s = kMixMin;
    if (s > kMixMax) s = kMixMax;
    int v = (s >> 16) + 128;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}
static inline int16_t  ClampToS16(MixSample s) {
    if (s < kMixMin) s = kMixMin;
    if (s > kMixMax) s = kMixMax;
    return static_cast<int16_t>(s >> 8);
}
static inline void     ClampToS24(MixSample s, uint8_t* dst) {
    if (s < kMixMin) s = kMixMin;
    if (s > kMixMax) s = kMixMax;
    dst[0] = static_cast<uint8_t>(s & 0xFF);
    dst[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
}
static inline int32_t  ClampToS32(MixSample s) {
    if (s < kMixMin) s = kMixMin;
    if (s > kMixMax) s = kMixMax;
    return s << 8;
}
// MixSample -> 32-bit IEEE 754 bit pattern.  Caller stores the bits as a
// uint32_t in the destination buffer; everyone treats the buffer as float.
static inline uint32_t ClampToF32Bits(MixSample s) {
    if (s < kMixMin) s = kMixMin;
    if (s > kMixMax) s = kMixMax;
    if (s == 0) return 0;
    uint32_t sign = (s < 0) ? 0x80000000u : 0;
    uint32_t mag  = (s < 0) ? static_cast<uint32_t>(-s) : static_cast<uint32_t>(s);
    // Find MSB position to pick exponent.  mag is in [1, 2^23].
    int msb = 23;
    while (msb > 0 && (mag & (1u << msb)) == 0) msb--;
    int exp_unbiased = msb - 23;          // mag has 23 fractional bits
    int exp_biased   = exp_unbiased + 127;
    if (exp_biased <= 0)   return sign;   // underflow to zero
    if (exp_biased >= 255) exp_biased = 254;
    // Shift mantissa so the implicit leading 1 sits at bit 23.
    uint32_t mantissa;
    if (msb >= 23) mantissa = (mag >> (msb - 23)) & 0x7FFFFF;
    else           mantissa = (mag << (23 - msb)) & 0x7FFFFF;
    return sign | (static_cast<uint32_t>(exp_biased) << 23) | mantissa;
}

static inline float ClampToF32(MixSample s) {
    union { float f; uint32_t u; } x{}; x.u = ClampToF32Bits(s);
    return x.f;
}

// ---- bulk decode/encode ----
//
// Decode `frame_count` frames from `src` (in the given format) into
// `canonical` (interleaved MixSamples).  The caller guarantees that
// `canonical` has room for `frame_count * channels` MixSamples.
//
// Returns the number of bytes consumed from `src`.
uint32_t DecodeToCanonical(SampleFormat fmt, int channels,
                           const void* src, MixSample* canonical,
                           uint32_t frame_count);

// Encode `frame_count` frames from `canonical` (interleaved MixSamples)
// into `dst` (in the given format).  Returns bytes written.
uint32_t EncodeFromCanonical(SampleFormat fmt, int channels,
                             const MixSample* canonical, void* dst,
                             uint32_t frame_count);

// ---- channel conversion ----
//
// Performs an in-place or out-of-place channel remix on canonical PCM.
// Supported conversions:
//   1 -> 2  (mono to stereo: L = R = mono)
//   2 -> 1  (stereo to mono: avg of L,R)
//   2 -> 6  (stereo to 5.1: front L/R, others 0, LFE = (L+R)/2 * 0.7)
//   6 -> 2  (5.1 downmix:   L = FL + 0.707*C + 0.707*RL,
//                            R = FR + 0.707*C + 0.707*RR)
//
// `src` and `dst` may alias only if in_channels == out_channels.
// Returns the number of frames written.
uint32_t ConvertChannels(int in_channels, int out_channels,
                         const MixSample* src, MixSample* dst,
                         uint32_t frame_count);

// ---- sample rate conversion ----
//
// Converts `in_frames` of canonical interleaved PCM at rate `rate_in` to
// rate `rate_out`.  Uses linear interpolation across adjacent input
// frames.  When rate_in == rate_out the function performs a memcpy and
// returns in_frames.
//
// The caller must size `dst` for at least
//   (in_frames * rate_out + rate_in - 1) / rate_in
// frames worth of MixSamples.  The actual number of frames produced is
// returned.
uint32_t ResampleLinear(int channels,
                        uint32_t rate_in, uint32_t rate_out,
                        const MixSample* src, uint32_t in_frames,
                        MixSample* dst, uint32_t dst_capacity_frames);

// ---- end-to-end convenience ----
//
// Convert(in_fmt, in_ch, in_rate, in, in_bytes, out_fmt, out_ch, out_rate,
//         out, out_capacity_bytes) -> bytes written
//
// Performs decode -> SRC -> channel-remix -> encode in one call, allocating
// no heap memory (uses an internal static scratch buffer of 64 KB).
// Returns 0 on failure (capacity exhausted, unsupported channel pair).
uint32_t Convert(SampleFormat in_fmt,  int in_ch,  uint32_t in_rate,
                 const void* in,  uint32_t in_bytes,
                 SampleFormat out_fmt, int out_ch, uint32_t out_rate,
                 void* out, uint32_t out_capacity_bytes);

// ---- utility ----
const char* FormatName(SampleFormat fmt);

} // namespace AudioFormat
