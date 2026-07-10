#pragma once
//  kurono os - unified audio server (high-level API)
//
//  AudioServer is the single entry point apps and drivers call.  It owns
//  the active backend (selected at boot from sb16/ac97/hda/pcspk) and
//  exposes a stream-oriented API on top of the software mixer.
//
//  Compatibility shim: the legacy `Audio::` and `HDAudio::` and `AC97::`
//  classes still exist for now, but their Beep/PlayTone/Play helpers
//  forward to AudioServer underneath so every sound goes through the
//  mixer.

#include "../kernel/types.h"
#include "audio_format.h"
#include "audio_mixer.h"
#include "audio_backend.h"

namespace AudioServer {

// One-time bring-up.  Calls AudioDMA::Init(), AudioMixer::Init(), then
// probes each registered backend in priority order:
//   1. HDA  (preferred - handles QEMU intel-hda + most modern PCs)
//   2. AC97 (mid-90s onward - lots of integrated chipsets)
//   3. SB16 (legacy ISA - QEMU's `-device sb16`)
//   4. PCSPK (PIT square wave - never fails, last resort)
// The first one whose Init() returns true becomes ActiveBackend().
void Init();

// Backend registry helpers (called by each backend's static init).
void          RegisterBackend(AudioBackend* be);
AudioBackend* ActiveBackend();
const char*   ActiveBackendName();

// One-shot tone synthesis.  Generates a sine wave of `freq` Hz for
// `duration_ms`, opens a transient mixer stream, writes the samples,
// and schedules auto-Drain so the stream frees when the tone ends.
//
// Used by the system Beep / accessibility notifications / settings UI.
// `volume` is 0..100; the master volume still applies on top of it.
void PlayTone(int freq_hz, int duration_ms, int volume_0_to_100);

// Convenience: a single keystroke beep (880 Hz, 60 ms, vol 60).
void Beep();

// Play one-shot PCM through a transient mixer stream.  Returns true if
// the stream was successfully opened and the data queued.  The caller
// keeps ownership of `pcm`; the mixer copies into its ring.
bool PlayPCM(const void* pcm, uint32_t bytes,
             AudioFormat::SampleFormat fmt, uint32_t rate, int channels);

// spec-named one-shot playback for s16 pcm.  thin wrapper over PlayPCM:
// `samples` is the total number of int16 samples across all channels
// (i.e. interleaved, not per-channel frames), so the byte count is
// samples * sizeof(int16_t).  format is fixed to signed 16-bit le. (satoru)
bool PlayBuffer(const int16_t* pcm, size_t samples,
                uint32_t sampleRate, uint8_t channels);

// Stream-oriented API (thin wrapper around AudioMixer for symmetry).
AudioMixer::StreamID OpenStream(const char* name,
                                AudioFormat::SampleFormat fmt,
                                uint32_t rate, int channels);
uint32_t WriteStream(AudioMixer::StreamID id, const void* src, uint32_t frames);
void     CloseStream(AudioMixer::StreamID id);
void     DrainStream(AudioMixer::StreamID id);

// Per-tick pump.  Called from the kernel main loop.
void Tick();

// Diagnostics.
struct ServerStatus {
    const char* backend_name;
    bool        backend_ready;
    uint32_t    backend_rate;
    int         active_streams;
    int         master_volume;
    bool        master_muted;
    uint64_t    total_periods_submitted;
};
ServerStatus GetStatus();
void Dump();

} // namespace AudioServer
