#pragma once
//  kurono os - software audio mixer
//
//  Up to 16 simultaneous streams.  Each stream owns a ring buffer of
//  canonical (24-bit-in-32) MixSamples at the mixer's internal rate
//  (default 48 000 Hz, stereo).  Apps push samples into the ring; the
//  mixer pulls from every active stream, sums, applies master volume +
//  EQ + soft limiter, then hands the period to the active backend.
//
//  Threading model: the mixer assumes single-threaded ownership.  The
//  kernel main loop calls AudioMixer::Tick() to fill the next backend
//  period; apps call Write() between ticks.  No locks needed.
//
//  Backend abstraction: a single `AudioBackend* g_backend` pulls periods.
//  Concrete backends (SB16, AC97, HDA) implement the interface in
//  audio_backend.h and register themselves on Init().

#include "../kernel/types.h"
#include "audio_format.h"

namespace AudioMixer {

constexpr int      MAX_STREAMS         = 16;
constexpr uint32_t INTERNAL_RATE       = 48000;
constexpr int      INTERNAL_CHANNELS   = 2;
constexpr uint32_t PERIOD_FRAMES       = 1024;     // 21.3 ms @ 48 kHz
constexpr uint32_t STREAM_RING_FRAMES  = 32768;    // 683 ms ring per stream - bigger
                                                   // headroom so a compositor / decode
                                                   // stall drains the ring without an
                                                   // underrun click (producer can fall
                                                   // ~0.68 s behind). (satoru)

using StreamID = int;
constexpr StreamID INVALID_STREAM = -1;

enum StreamState : uint8_t {
    STREAM_FREE     = 0,    // slot is unused
    STREAM_PLAYING  = 1,    // pulling samples from the ring
    STREAM_PAUSED   = 2,    // ring drained but still owned
    STREAM_FADING   = 3,    // volume ramping toward zero, will close on silence
    STREAM_DRAINING = 4,    // app closed write side; play remaining then free
};

struct StreamStats {
    uint64_t frames_written;
    uint64_t frames_consumed;
    uint32_t underruns;     // ring went empty mid-playback
    uint32_t overflows;     // app overran the ring
    uint32_t peak_level;    // last-period peak (0..kMixMax)
};

// Open a new stream at the given source format/rate/channels.  Samples
// written to this stream will be converted to the mixer's internal
// (24-bit, INTERNAL_RATE, INTERNAL_CHANNELS) format on the fly.
//
// `name` is for diagnostics only and may be nullptr.
// Returns INVALID_STREAM if no slots are free.
StreamID Open(const char* name,
              AudioFormat::SampleFormat fmt,
              uint32_t sample_rate,
              int channels);

// Close the stream.  Any unplayed samples in the ring are discarded
// after a 5 ms fade-out.  Safe to call on INVALID_STREAM.
void Close(StreamID id);

// Tell the mixer "this is the last batch of samples - when the ring
// drains, free the stream automatically".  Useful for one-shot sounds.
void Drain(StreamID id);

// Push samples into the stream.  Returns the number of *frames*
// actually accepted (may be less than `frames` if the ring is near
// full).  Format conversion happens here.
uint32_t Write(StreamID id, const void* src, uint32_t frames);

// Per-stream controls.
void  SetVolume(StreamID id, int volume_0_to_100);
int   GetVolume(StreamID id);
void  SetPan   (StreamID id, int pan_minus100_to_100);    // -100=L, +100=R
void  SetMute  (StreamID id, bool mute);
// pause/resume playback without dropping the ring (pulse CORK): a paused
// stream keeps accepting Write()s but the mixer stops pulling from it. (satoru)
void  SetPaused(StreamID id, bool paused);
bool  IsActive (StreamID id);
StreamState GetState(StreamID id);
StreamStats GetStats(StreamID id);

// Master controls.
void  SetMasterVolume(int volume_0_to_100);
int   GetMasterVolume();
void  SetMasterMute  (bool mute);
bool  GetMasterMute  ();

// Master 3-band shelving EQ in q12 fixed point gain (4096 = 0 dB).
//   bass:    < 250 Hz
//   mid:     250 Hz .. 4 kHz
//   treble:  > 4 kHz
// Each gain in [256 (-12 dB), 16384 (+12 dB)].  4096 is bypass.
void  SetEqGains(int32_t bass_q12, int32_t mid_q12, int32_t treble_q12);
void  GetEqGains(int32_t* bass_q12, int32_t* mid_q12, int32_t* treble_q12);

// Optional soft-knee limiter.  When enabled, the master sum is run
// through a per-sample attenuator that prevents hard-clipping.  Adds
// ~0.5 ms of look-back smoothing.
void  SetLimiterEnabled(bool on);
bool  IsLimiterEnabled();

// Boot the mixer.  Must be called after AudioDMA::Init() and before any
// driver registers a backend.  Idempotent.
void  Init();

// Per-frame pump.  Called from the kernel main loop.  Pulls one period
// worth of frames from every active stream, mixes them, applies master
// effects, and submits to the active backend.  Return value is the
// number of frames the backend actually accepted.
uint32_t Tick();

// Diagnostics.
int   ActiveStreamCount();
const char* StreamName(StreamID id);
void  Dump();

// Backend selection helpers - implemented in audio_server.cpp.
// AudioMixer itself only knows about the AudioBackend interface.

} // namespace AudioMixer
