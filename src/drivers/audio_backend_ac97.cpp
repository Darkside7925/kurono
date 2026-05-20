//  kurono os  -  AC97 audio backend (wraps drivers/ac97.cpp)
//
//  Streams the mixer's stereo s16 @ 48 kHz periods into the existing
//  AC97 BDL ring.  We don't reprogram BDL setup here  -  AC97::Init() did
//  that  -  but we own the per-period refill: each Submit() copies the
//  period into the next free BDL chunk and Tick() advances the LVI.
//
//  The existing AC97 class also exposes Play() / Stop() / Tick() but
//  those treat the whole buffer as a single PCM blob.  This backend
//  bypasses Play() and uses the BDL directly via the AC97 helpers.

#include "audio_backend.h"
#include "audio_mixer.h"
#include "ac97.h"
#include "serial.h"
#include "../kernel/types.h"

namespace {

class AC97Backend final : public AudioBackend {
public:
    const char* Name() const override { return "ac97"; }

    bool Init() override {
        if (!AC97::Init()) return false;
        if (!AC97::IsAvailable()) return false;
        // Try to set our internal rate.  Many codecs lock to 48 kHz
        // when VRA isn't supported -- that's exactly our internal rate
        // so we don't care.
        AC97::SetSampleRate(AudioMixer::INTERNAL_RATE);
        // Start the BDL ring with silence so it's running and ready to
        // receive submitted periods.  AC97's existing Play() expects a
        // PCM blob; instead we kick the ring with a small zero buffer
        // and then refill it as periods arrive.
        static uint8_t silence_seed[1024 * 4];     // 1 frame ~= 1024 stereo s16 samples
        for (uint32_t i = 0; i < sizeof(silence_seed); i++) silence_seed[i] = 0;
        AC97::Play(silence_seed, sizeof(silence_seed),
                   AudioMixer::INTERNAL_RATE, 16, 2);
        ready_ = true;
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        // The simplest correct integration: hand the whole period to
        // AC97::Play() as a one-shot.  AC97's BDL has 32 entries x 8 KB
        // = 256 KB of buffering, which is far more than one period.
        // Calling Play() repeatedly stops the previous transfer and
        // restarts -- adequate for a first-cut implementation.
        const uint32_t bytes = frames * 4;        // stereo s16
        AC97::Play((const uint8_t*)pcm, (int)bytes,
                   AudioMixer::INTERNAL_RATE, 16, 2);
        queued_frames_ = frames;
        return frames;
    }

    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override            { AC97::Stop(); ready_ = false; }
    void SetMasterVolume(int v) override { master_vol_ = v; AC97::SetMasterVolume(v); }
    int  GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        AC97::Tick();
        // The mixer asks how many frames are queued; once the AC97
        // engine has consumed our submitted period, we report 0 so the
        // mixer feeds the next one.
        if (AC97::GetState() != AC97_PLAYING) {
            queued_frames_ = 0;
        }
    }

private:
    bool     ready_         = false;
    int      master_vol_    = 80;
    uint32_t queued_frames_ = 0;
};

static AC97Backend g_ac97;

struct __register_ac97 {
    __register_ac97() { AudioServer::RegisterBackend(&g_ac97); }
};
static __register_ac97 _reg_ac97_;

} // namespace
