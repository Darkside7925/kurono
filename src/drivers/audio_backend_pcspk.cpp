//  kurono os  -  PC speaker fallback audio backend
//
//  Last-resort backend when nothing else is present.  Uses the PIT
//  channel 2 + KB controller port 0x61 to drive the motherboard speaker
//  with a square wave whose frequency tracks the dominant tone in each
//  submitted period.  Quality is awful  -  this is for "did we boot?"
//  feedback, not for music playback.
//
//  Implementation:
//    * Submit() scans the period for the largest absolute peak; if the
//      peak exceeds a threshold, it estimates the dominant frequency
//      via a zero-crossing rate and drives the PIT to that frequency.
//      If the peak is below threshold, the speaker is muted.

#include "audio_backend.h"
#include "audio_mixer.h"
#include "../kernel/types.h"

namespace {

static inline void out8(uint16_t port, uint8_t v) {
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

class PCSpeakerBackend final : public AudioBackend {
public:
    const char* Name() const override { return "pcspk"; }

    bool Init() override {
        // Always succeeds  -  this is the last-resort fallback.
        ready_ = true;
        return true;
    }
    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm || frames == 0) return 0;
        // Find peak amplitude (left channel) and zero-crossing count.
        // Sign change detection uses bit 15 so unsigned widening doesn't
        // mask the sign bit (the previous `(s ^ prev) < 0` ran on an
        // implicitly-promoted int and missed crossings).
        int32_t peak = 0;
        uint32_t zc = 0;
        int16_t prev = pcm[0];
        for (uint32_t f = 0; f < frames; f++) {
            int16_t s = pcm[f * 2];           // left channel
            int32_t mag = s < 0 ? -s : s;
            if (mag > peak) peak = mag;
            if ((((uint16_t)s ^ (uint16_t)prev) & 0x8000) != 0) zc++;
            prev = s;
        }
        if (peak < 1500) {
            // Too quiet  -  mute the speaker.
            uint8_t v = in8(0x61);
            v &= ~0x03;
            out8(0x61, v);
            return frames;
        }
        // Estimate frequency from zero crossings: zc per period ~= 2 * f * (frames / rate)
        // -> f ~= zc * rate / (2 * frames)
        uint32_t freq = (zc * AudioMixer::INTERNAL_RATE) / (2 * frames);
        if (freq < 50)    freq = 50;
        if (freq > 12000) freq = 12000;
        SetPITFreq(freq);
        // Enable speaker.
        uint8_t v = in8(0x61);
        v |= 0x03;
        out8(0x61, v);
        return frames;
    }

    uint32_t QueuedFrames() const override { return 0; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override {
        uint8_t v = in8(0x61);
        v &= ~0x03;
        out8(0x61, v);
    }

    void SetMasterVolume(int v) override { master_vol_ = v; /* no HW vol */ }
    int  GetMasterVolume() const override { return master_vol_; }

private:
    bool ready_      = false;
    int  master_vol_ = 100;

    void SetPITFreq(uint32_t hz) {
        if (hz == 0) return;
        uint32_t div = 1193182u / hz;
        if (div > 0xFFFF) div = 0xFFFF;
        // PIT channel 2, mode 3 (square wave), LSB then MSB
        out8(0x43, 0xB6);
        out8(0x42, (uint8_t)(div & 0xFF));
        out8(0x42, (uint8_t)((div >> 8) & 0xFF));
    }
};

static PCSpeakerBackend g_pcspk;

struct __register_pcspk {
    __register_pcspk() { AudioServer::RegisterBackend(&g_pcspk); }
};
static __register_pcspk _reg_pcspk_;

} // namespace
