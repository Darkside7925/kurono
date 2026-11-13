// usb audio feedback (sync endpoint) math, shared by the kernel driver and the
// host unit test. usb audio 2.0 5.12.4.2: a full speed sink reports the rate in
// 10.14 format in 3 bytes as samples per 1 ms frame; a high speed sink reports
// 16.16 in 4 bytes as samples per 125 us microframe. linux sound/usb/endpoint.c
// snd_usb_handle_sync_urb auto-detects the format by shifting the first value
// until it is near the nominal rate, then clamps it; the same rules are used
// here so devices with a non-standard format still lock on. (satoru)
#pragma once
#include <stdint.h>

namespace UsbAudioFb {

// int_min: the format shift has not been detected yet (satoru)
constexpr int32_t kShiftAuto = -2147483647 - 1;

// nominal frequency in q16.16 samples per feedback frame (per 1 ms frame at
// full speed, per 125 us microframe at high speed), rounded like linux. (satoru)
inline uint32_t Nominal(uint32_t rate, bool high_speed) {
    if (!rate) return 0;
    if (high_speed) return (uint32_t)((((uint64_t)rate << 10) + 62) / 125);
    return (uint32_t)((((uint64_t)rate << 13) + 62) / 125);
}

inline uint32_t Le24(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
inline uint32_t Le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// parse one feedback packet. returns 0 when the packet is unusable (short,
// zero, or so far from nominal that the shift is wrong). freqshift is the
// per-endpoint autodetect state and must start at kShiftAuto. (satoru)
inline uint32_t Parse(const uint8_t* p, uint32_t len, uint32_t freqn, int32_t* freqshift) {
    if (!p || !freqn || !freqshift || len < 3) return 0;
    uint32_t f = (len == 3) ? (Le24(p) & 0x00FFFFFFu) : (Le32(p) & 0x0FFFFFFFu);
    if (!f) return 0;
    int32_t sh = *freqshift;
    if (sh == kShiftAuto) {
        sh = 0;
        while (f < freqn - freqn / 4) { f <<= 1; sh++; }
        while (f > freqn + freqn / 2) { f >>= 1; sh--; }
        *freqshift = sh;
    } else if (sh >= 0) {
        f <<= sh;
    } else {
        f >>= -sh;
    }
    // linux trusts only [nominal - 12.5%, nominal + 50%]; anything else means
    // the format guess was wrong, so detect again on the next packet. (satoru)
    if (f < freqn - freqn / 8 || f > freqn + freqn / 2) {
        *freqshift = kShiftAuto;
        return 0;
    }
    return f;
}

// one-pole smoothing of the feedback value. feedback arrives every service
// interval and jitters by a sample or two; feeding that straight into the
// packet pacer would modulate the packet size at the jitter rate. (satoru)
struct Smooth {
    uint32_t cur;
    bool init;
    Smooth() : cur(0), init(false) {}
    void Reset() { cur = 0; init = false; }
    uint32_t Update(uint32_t f) {
        if (!f) return cur;
        if (!init) { cur = f; init = true; return cur; }
        int64_t d = (int64_t)f - (int64_t)cur;
        // round the one-pole step; a plain arithmetic shift floors negative
        // differences and biases the tracked rate low in steady state. (satoru)
        int64_t step = d >= 0 ? (d + 8) / 16 : -((-d + 8) / 16);
        cur = (uint32_t)((int64_t)cur + step);
        return cur;
    }
    bool Valid() const { return init; }
};

// packet pacer: the frames in one service interval are the (smoothed) feedback
// frequency times the number of feedback frames in that interval (1 << shift).
// the fraction carries in q16.16, so the long-run frame count stays exact even
// when the device rate is not a whole number of frames per packet. (satoru)
struct Pacer {
    uint32_t phase;
    Pacer() : phase(0) {}
    void Reset() { phase = 0; }
    // peek the next packet size without advancing the fraction (satoru)
    uint32_t Peek(uint32_t freq, uint32_t shift, uint32_t max_frames) const {
        uint64_t acc = (uint64_t)phase + ((uint64_t)freq << (shift & 31));
        uint32_t f = (uint32_t)(acc >> 16);
        if (max_frames && f > max_frames) f = max_frames;
        return f;
    }
    // advance the fraction for a packet that was actually queued (satoru)
    void Commit(uint32_t freq, uint32_t shift, uint32_t max_frames) {
        uint64_t acc = (uint64_t)phase + ((uint64_t)freq << (shift & 31));
        uint32_t f = (uint32_t)(acc >> 16);
        if (max_frames && f > max_frames) {
            // the device cannot take the excess; drop it instead of banking
            // samples that were never sent. (satoru)
            phase = 0;
        } else {
            phase = (uint32_t)(acc & 0xFFFFu);
        }
    }
};

} // namespace UsbAudioFb

// end (satoru)
