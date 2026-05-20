//  kurono os  -  Intel HD Audio backend (wraps drivers/hda.cpp)
//
//  Probes for an HDA controller via the existing HDAudio class and
//  forwards mixer periods through HDAudio::Play().  Like the AC97
//  backend, this is a thin shim until the underlying driver supports a
//  proper period-based API.

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
        ready_ = true;
        SerialLogger::Log("[HDA-be] Ready\r\n");
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        const uint32_t bytes = frames * 4;
        if (HDAudio::Play(pcm, bytes)) {
            queued_frames_ = frames;
            return frames;
        }
        return 0;
    }

    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override               { HDAudio::Stop(); ready_ = false; }
    void SetMasterVolume(int v) override { master_vol_ = v;
        // HDAudio uses 0..255 for vol
        HDAudio::SetVolume((uint8_t)((v * 255) / 100));
    }
    int  GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        if (!HDAudio::IsPlaying()) {
            queued_frames_ = 0;
        }
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
