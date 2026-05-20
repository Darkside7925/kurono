// kurono os  -  aac (mpeg-4 audio) bitstream metadata parser
// =================================================================
// parses the two layouts you'll actually see in the wild:
//   * adts header (each frame self-describes  -  used in raw .aac files)
//   * audiospecificconfig (ASC)  -  a 2..N-byte blob inside esds in mp4
//
// gives the os profile, sample rate, channel count and frame length so
// the player can show "AAC LC 44100 Hz stereo" and route the right
// number of frames per period to the audio mixer.
//
// not a decoder  -  but is sufficient for the demuxer to feed an aac
// decoder with correct stream parameters when one becomes available.
#pragma once
#include "../kernel/types.h"

namespace AAC {

enum AOT : uint8_t {
    AOT_AAC_MAIN = 1,
    AOT_AAC_LC   = 2,
    AOT_AAC_SSR  = 3,
    AOT_AAC_LTP  = 4,
    AOT_SBR      = 5,
    AOT_PS       = 29,
};

struct Config {
    uint8_t  audio_object_type;     // AAC LC = 2, etc
    uint32_t sample_rate;
    uint8_t  channels;
    bool     sbr_present;           // implicit/explicit SBR signalling
    bool     ps_present;            // parametric stereo
};

// adts header, parsed from the 7-byte (or 9 with crc) header.
struct AdtsFrame {
    uint8_t  profile;               // (audio_object_type - 1)
    uint8_t  freq_index;
    uint32_t sample_rate;
    uint8_t  channel_config;
    uint16_t frame_length;          // includes header
    uint16_t buffer_fullness;
    uint8_t  num_raw_data_blocks;   // typically 1
    bool     crc_present;
};

// parse the audiospecificconfig blob (esds DSI).  returns true on success.
bool ParseConfig(const uint8_t* data, uint32_t size, Config& out);

// parse one adts frame header at `data`.  returns the number of bytes
// consumed (>= 7) on success, or 0 if not a valid header.
uint32_t ParseAdtsFrame(const uint8_t* data, uint32_t size, AdtsFrame& out);

// scan a stream of adts frames; returns the number of frames found.
uint32_t WalkAdts(const uint8_t* data, uint32_t size,
                  bool (*cb)(const AdtsFrame&, const uint8_t* payload,
                             uint32_t payload_len, void* user),
                  void* user);

const char* ProfileName(uint8_t aot);

} // namespace AAC
