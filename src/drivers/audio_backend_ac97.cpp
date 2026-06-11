//  kurono os  -  AC97 audio backend (wraps drivers/ac97.cpp)
//
//  Uses the streaming API on AC97 (EnsureStreaming + WriteRingChunk)
//  so the BDL ring runs continuously and the mixer just refills the
//  next chunk each tick.  No per-period restarts.

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
        AC97::SetSampleRate(AudioMixer::INTERNAL_RATE);
        if (!AC97::EnsureStreaming(AudioMixer::INTERNAL_RATE, 16, 2)) return false;
        ready_ = true;
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        const uint32_t bytes = frames * 4;     // stereo s16
        uint32_t written = AC97::WriteRingChunk((const void*)pcm, bytes);
        // bump the cached depth by what we just queued (no port I/O). the
        // exact depth (accounting for hardware draining since) is reconciled
        // once per pump in Tick(). (satoru)
        queued_frames_ += written / 4;
        return written / 4;
    }

    // return the CACHED queue depth  -  no port I/O. Tick() reconciles it from
    // the hardware (CIV/LVI) exactly once per pump (called at the top of
    // AudioServer::Tick before this gate is read), and Submit() bumps it per
    // period, so the value the back-pressure gate sees is fresh without a
    // per-call VM-exit storm. (satoru)
    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override            { AC97::Stop(); ready_ = false; queued_frames_ = 0; }
    void SetMasterVolume(int v) override { master_vol_ = v; AC97::SetMasterVolume(v); }
    int  GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        AC97::Tick();
        queued_frames_ = AC97::RingQueuedBytes() / 4;
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
