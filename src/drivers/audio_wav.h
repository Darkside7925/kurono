#pragma once
//  kurono os  -  minimal RIFF/WAV parser
//
//  Supports the canonical PCM subset (format code 1):
//    * 8-bit unsigned, 16-bit signed, 24-bit signed, 32-bit signed PCM
//    * IEEE float (format code 3) at 32 bits
//    * 1, 2, or up to 6 channels
//    * Sample rates 4 kHz .. 192 kHz
//
//  Does not support: ADPCM (format 2), MP3 (format 0x55), or extensible
//  WAVEFORMATEXTENSIBLE (format 0xFFFE) -- caller gets ParseResult with
//  `valid=false` and an error string.

#include "../kernel/types.h"
#include "audio_format.h"

namespace AudioWAV {

struct ParseResult {
    bool                       valid;
    AudioFormat::SampleFormat  fmt;
    uint32_t                   sample_rate;
    int                        channels;
    const uint8_t*             pcm_start;       // pointer into the input buffer
    uint32_t                   pcm_bytes;
    uint32_t                   total_frames;    // pcm_bytes / FrameSize
    const char*                error;           // nullptr if valid
};

// Parse a WAV file image.  `buf`/`len` must remain valid for the
// lifetime of the returned ParseResult.pcm_start pointer.
ParseResult Parse(const uint8_t* buf, uint32_t len);

// Quick header-only check: returns true if the buffer starts with a
// "RIFF....WAVEfmt " sequence.  Does not validate the rest.
bool LooksLikeWAV(const uint8_t* buf, uint32_t len);

// Build a minimal 44-byte RIFF/WAVE PCM header into `out` (must be at
// least 44 bytes).  Returns the number of bytes written.  Used by the
// audio recording path (when we eventually add capture).
uint32_t BuildHeader(uint8_t* out, AudioFormat::SampleFormat fmt,
                     uint32_t rate, int channels, uint32_t pcm_bytes);

} // namespace AudioWAV
