#pragma once
//  kurono os - abstract audio backend interface
//
//  Every concrete audio backend (SB16, AC97, HDA, PC speaker) implements
//  this interface and registers a singleton instance with the mixer via
//  AudioServer::RegisterBackend().  The mixer then pulls a period
//  (PERIOD_FRAMES at INTERNAL_RATE in stereo s16) and hands it to
//  Submit().  The backend is responsible for queuing the period to its
//  hardware ring/BDL and notifying when ready for the next period.
//
//  Backends should be lock-free relative to the mixer: Submit() copies
//  the data into hardware-owned buffers; the mixer never holds a pointer
//  the hardware will read later.

#include "../kernel/types.h"

class AudioBackend {
public:
    virtual ~AudioBackend() {}

    // Stable identifier ("sb16", "ac97", "hda", "pcspk", "null").
    virtual const char* Name() const = 0;

    // Probe + program the hardware.  Returns true if a usable device was
    // found and successfully configured for stereo 16-bit @ 48 kHz.
    virtual bool Init() = 0;

    // True if Init() succeeded and the device is in a runnable state.
    virtual bool IsReady() const = 0;

    // Submit one period of interleaved s16 stereo samples at the mixer's
    // internal rate.  Returns the number of frames accepted (0 if the
    // hardware is back-pressured).  `frames` is always
    // AudioMixer::PERIOD_FRAMES.
    virtual uint32_t Submit(const int16_t* pcm, uint32_t frames) = 0;

    // Number of frames currently buffered in hardware (i.e. the
    // pre-roll / latency).  Used by the mixer to decide whether to push
    // the next period now or wait.
    virtual uint32_t QueuedFrames() const = 0;

    // Hardware sample rate this backend ended up programming.  Most
    // backends will report INTERNAL_RATE; SB16 may end up at 44 100 Hz
    // if 48 000 isn't supported.  The mixer will resample as needed.
    virtual uint32_t SampleRate() const = 0;

    // Stop playback immediately.  Implementations should mute the
    // hardware ring and wait for any in-flight DMA to complete.
    virtual void Stop() = 0;

    // Set / get master output level on the hardware mixer (0..100).
    virtual void SetMasterVolume(int v) = 0;
    virtual int  GetMasterVolume() const = 0;

    // Periodic poll for backends that don't IRQ (currently SB16 + AC97).
    // The mixer calls Tick() before deciding to Submit().
    virtual void Tick() {}

    // Optional: write a one-line diagnostic string into `out` (caller
    // owns the buffer).  Default impl writes the name.
    virtual void Describe(char* out, int max_len) const;
};

namespace AudioServer {
    // Defined in audio_server.cpp.  Registers a backend with the mixer.
    // The first backend whose Init() succeeds becomes the active one.
    void RegisterBackend(AudioBackend* be);
    AudioBackend* ActiveBackend();
}
