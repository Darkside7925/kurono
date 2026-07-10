// kurono os - kvid: kurono native video container
// =================================================================
// .kvid is a deliberately tiny, deliberately simple container we can
// guarantee plays correctly on a freestanding kernel without an h264
// decoder.  it is the playable counterpart to mp4_demux: any source
// video can be transcoded to .kvid via the host-side ffmpeg helper
// (see tools/transcode_to_kvid.ps1) and then played natively.
//
// file layout (all integers little-endian - easier from this side):
//
//   offset  size  field
//   ------  ----  -------------------------------------------------
//   0       4     magic = "KVID"
//   4       4     version = 1
//   8       4     flags  (bit 0 = has_audio)
//  12       2     width
//  14       2     height
//  16       4     frame_count
//  20       2     fps_num
//  22       2     fps_den
//  24       4     audio_sample_rate (0 if no audio)
//  28       2     audio_channels
//  30       2     audio_bits_per_sample (16)
//  32       4     index_offset (file offset of frame index)
//  36       4     index_count  (== frame_count)
//  40       8     reserved
//  48       N     payload (interleaved frames + audio chunks)
//  ...     32*N   frame index (one entry per frame):
//                    uint32 offset (from start of file)
//                    uint32 video_size (jpeg bytes)
//                    uint32 audio_offset (from start of file, 0 if none)
//                    uint32 audio_size (pcm s16 bytes for this frame's
//                                       interval; sum may be padded)
//                    uint64 dts_us (microseconds)
//                    uint64 reserved
//
// "frame" payload = a complete jpeg (jfif) image.  audio payload =
// raw signed-16 little-endian interleaved pcm at audio_sample_rate.
// every frame is a keyframe (jpeg has no inter prediction) so seeking
// is exact and free.
#pragma once
#include "../kernel/types.h"

namespace KVID {

static constexpr uint32_t kMagic   = 0x44495652;  // 'KVID' little-endian read
static constexpr uint32_t kVersion = 1;
static constexpr uint32_t kFlagHasAudio = 1u << 0;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint16_t width;
    uint16_t height;
    uint32_t frame_count;
    uint16_t fps_num;
    uint16_t fps_den;
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    uint16_t audio_bits_per_sample;
    uint32_t index_offset;
    uint32_t index_count;
    uint64_t reserved;
} __attribute__((packed));

struct IndexEntry {
    uint32_t offset;
    uint32_t video_size;
    uint32_t audio_offset;
    uint32_t audio_size;
    uint64_t dts_us;
    uint64_t reserved;
} __attribute__((packed));

struct File {
    const uint8_t*    data;        // borrowed
    uint32_t          size;
    Header            hdr;
    const IndexEntry* index;       // points into data, no copy
};

// open / validate the file.  returns false on bad magic / truncation.
bool Open(const uint8_t* data, uint32_t size, File& out);

// duration of the file in milliseconds.
uint32_t DurationMs(const File& f);

// pointer to the jpeg payload of frame N + its size; returns nullptr on
// out-of-range.
const uint8_t* GetFrameJpeg(const File& f, uint32_t frame_idx, uint32_t* out_size);

// pointer to the audio chunk for frame N; nullptr if no audio in file.
const uint8_t* GetFrameAudio(const File& f, uint32_t frame_idx, uint32_t* out_size);

// frame index nearest to a given millisecond timestamp (binary search).
uint32_t FrameAtMs(const File& f, uint32_t ms);

} // namespace KVID
