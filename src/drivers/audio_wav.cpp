//  kurono os - minimal RIFF/WAV parser (implementation)
#include "audio_wav.h"
#include "../kernel/types.h"

namespace AudioWAV {

static inline uint32_t Rd32LE(const uint8_t* p) {
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint16_t Rd16LE(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline void Wr32LE(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void Wr16LE(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
}
static inline bool TagEq(const uint8_t* p, const char* s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

bool LooksLikeWAV(const uint8_t* buf, uint32_t len) {
    if (!buf || len < 12) return false;
    return TagEq(buf, "RIFF") && TagEq(buf + 8, "WAVE");
}

ParseResult Parse(const uint8_t* buf, uint32_t len) {
    ParseResult r{};
    r.valid = false;
    r.error = nullptr;

    if (!buf || len < 44) { r.error = "buffer too short"; return r; }
    if (!TagEq(buf, "RIFF")) { r.error = "missing RIFF magic"; return r; }
    if (!TagEq(buf + 8, "WAVE")) { r.error = "missing WAVE tag"; return r; }

    // Walk chunks starting after "WAVE".
    uint32_t off = 12;
    bool fmt_found = false, data_found = false;

    uint16_t format_code  = 0;
    uint16_t channels     = 0;
    uint32_t sample_rate  = 0;
    uint16_t bits         = 0;

    while (off + 8 <= len) {
        const uint8_t* tag  = buf + off;
        uint32_t       size = Rd32LE(buf + off + 4);
        // 64-bit compare: a crafted size near 0xFFFFFFFF would wrap the 32-bit
        // sum and slip past the guard, then the walk reads out of bounds. (satoru)
        if ((uint64_t)off + 8u + (uint64_t)size > (uint64_t)len) { r.error = "chunk overflows buffer"; return r; }

        if (TagEq(tag, "fmt ")) {
            if (size < 16) { r.error = "fmt chunk too small"; return r; }
            format_code  = Rd16LE(buf + off + 8);
            channels     = Rd16LE(buf + off + 10);
            sample_rate  = Rd32LE(buf + off + 12);
            // skip byte_rate (16..19) and block_align (20..21)
            bits         = Rd16LE(buf + off + 22);
            fmt_found = true;
        } else if (TagEq(tag, "data")) {
            r.pcm_start = buf + off + 8;
            r.pcm_bytes = size;
            data_found = true;
            // we can stop now - we have everything we need
            break;
        }
        off += 8 + size;
        if (size & 1) off++;     // chunks are word-padded
    }

    if (!fmt_found)  { r.error = "no fmt chunk";  return r; }
    if (!data_found) { r.error = "no data chunk"; return r; }
    if (channels < 1 || channels > 6) { r.error = "unsupported channels"; return r; }
    if (sample_rate < 4000 || sample_rate > 192000) {
        r.error = "unsupported sample rate"; return r;
    }

    if (format_code == 1) {                // PCM int
        switch (bits) {
            case 8:  r.fmt = AudioFormat::FMT_U8;     break;
            case 16: r.fmt = AudioFormat::FMT_S16_LE; break;
            case 24: r.fmt = AudioFormat::FMT_S24_LE; break;
            case 32: r.fmt = AudioFormat::FMT_S32_LE; break;
            default: r.error = "unsupported bit depth"; return r;
        }
    } else if (format_code == 3 && bits == 32) {     // IEEE float
        r.fmt = AudioFormat::FMT_F32_LE;
    } else {
        r.error = "unsupported codec";
        return r;
    }

    r.sample_rate  = sample_rate;
    r.channels     = channels;
    r.total_frames = r.pcm_bytes / AudioFormat::FrameSize(r.fmt, r.channels);
    r.valid        = true;
    return r;
}

uint32_t BuildHeader(uint8_t* out, AudioFormat::SampleFormat fmt,
                     uint32_t rate, int channels, uint32_t pcm_bytes) {
    if (!out) return 0;
    uint16_t bits;
    uint16_t code = 1;          // PCM int
    switch (fmt) {
        case AudioFormat::FMT_U8:     bits = 8;  break;
        case AudioFormat::FMT_S16_LE: bits = 16; break;
        case AudioFormat::FMT_S24_LE: bits = 24; break;
        case AudioFormat::FMT_S32_LE: bits = 32; break;
        case AudioFormat::FMT_F32_LE: bits = 32; code = 3; break;
        default: return 0;
    }
    uint16_t block_align = (uint16_t)((bits / 8) * channels);
    uint32_t byte_rate   = rate * block_align;
    uint32_t riff_size   = 36 + pcm_bytes;

    out[0]='R'; out[1]='I'; out[2]='F'; out[3]='F';
    Wr32LE(out + 4, riff_size);
    out[8]='W'; out[9]='A'; out[10]='V'; out[11]='E';
    out[12]='f'; out[13]='m'; out[14]='t'; out[15]=' ';
    Wr32LE(out + 16, 16);
    Wr16LE(out + 20, code);
    Wr16LE(out + 22, (uint16_t)channels);
    Wr32LE(out + 24, rate);
    Wr32LE(out + 28, byte_rate);
    Wr16LE(out + 32, block_align);
    Wr16LE(out + 34, bits);
    out[36]='d'; out[37]='a'; out[38]='t'; out[39]='a';
    Wr32LE(out + 40, pcm_bytes);
    return 44;
}

} // namespace AudioWAV
