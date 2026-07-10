//  kurono os - Intel HD Audio backend (wraps drivers/hda.cpp)
//
//  Streams mixer periods into the HDA cyclic buffer through HDAudio's
//  ring API.  The DMA engine runs continuously after StartStream(); we
//  only refill the write head each Submit().

#include "audio_backend.h"
#include "audio_mixer.h"
#include "hda.h"
#include "serial.h"
#include "../kernel/types.h"

namespace {

class HDABackend final : public AudioBackend {
public:
    const char* Name() const override { return "hda"; }

    bool Init() override {
        if (!HDAudio::Init()) return false;
        if (!HDAudio::IsDetected()) return false;
        if (!HDAudio::SetFormat(AudioMixer::INTERNAL_RATE, 16, 2)) return false;
        if (!HDAudio::StartStream()) return false;
        ready_ = true;
        SerialLogger::Log("[HDA-be] Ready (streaming)\r\n");
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        const uint32_t bytes = frames * 4;
        uint32_t written = HDAudio::WriteRing(pcm, bytes);
        queued_frames_ = HDAudio::RingQueuedBytes() / 4;
        return written / 4;
    }

    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override               { HDAudio::Stop(); ready_ = false; queued_frames_ = 0; }
    void SetMasterVolume(int v) override { master_vol_ = v;
        HDAudio::SetVolume((uint8_t)((v * 255) / 100));
    }
    int  GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        queued_frames_ = HDAudio::RingQueuedBytes() / 4;
    }

private:
    bool     ready_         = false;
    int      master_vol_    = 80;
    uint32_t queued_frames_ = 0;
};

static HDABackend g_hda;

struct __register_hda {
    __register_hda() { AudioServer::RegisterBackend(&g_hda); }
};
static __register_hda _reg_hda_;

} // namespace
