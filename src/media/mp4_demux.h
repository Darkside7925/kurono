// kurono os  -  mp4 / iso base media file format (isobmff) demuxer
// =================================================================
// industrial-grade demuxer for the iso base media file format
// (iso/iec 14496-12), the container behind .mp4, .m4a, .m4v, .mov,
// .3gp, .heif and .heic.  parses the entire box tree, builds a
// per-track sample table, and lets you seek to any sample by index
// or by composition time.
//
// what this DOES:
//   * walks every top-level box (ftyp, moov, mdat, free, skip, ...)
//   * parses moov → mvhd / trak{tkhd, mdia{mdhd, hdlr, minf{stbl{*}}}}
//   * supports both 32-bit (stco) and 64-bit (co64) chunk offsets
//   * supports versioned long-form atoms (mvhd v1, tkhd v1, mdhd v1)
//   * decodes the sample-to-chunk run-length table (stsc)
//   * decodes sample sizes (stsz, both fixed and per-sample)
//   * decodes time-to-sample (stts), composition offset (ctts),
//     and sync sample (stss) tables
//   * exposes per-sample: file offset, size, dts, pts, is_keyframe
//   * understands the codec fourcc inside stsd → avc1/avcC, mp4a/esds,
//     hvc1/hvcC, mjpg/jpeg, opus, alaw, ulaw, pcm variants, kvid
//   * extracts the codec-private data blob (avcC, hvcC, esds-decoder-
//     specific-info) which downstream parsers consume to bring up the
//     real codec
//   * pure integer math, no fpu, freestanding-clean
//
// what this DOES NOT do (intentional  -  kept upstairs in the player):
//   * actually decode video / audio samples (handled by codec modules)
//   * fragmented mp4 (moof/mfra)  -  denji.mp4 is non-fragmented
//   * encryption (cenc, pssh, sinf)  -  out of scope
//   * edit lists (elst)  -  composition is taken straight from stts+ctts
//
// design notes:
//   we parse once at Open() and cache everything.  the underlying byte
//   buffer is borrowed (not copied), so the caller must keep it alive
//   for the lifetime of the demuxer.  all sample-table arrays are
//   heap-allocated via KernelHeap and freed in Close().
#pragma once
#include "../kernel/types.h"

namespace MP4 {

// ---- limits -------------------------------------------------------------
static constexpr int kMaxTracks            = 8;     // movie-level
static constexpr int kMaxStsdEntries       = 4;     // per-track codecs
static constexpr int kMaxCodecPrivateBytes = 8192;  // avcC / esds blob
static constexpr int kMaxBoxNestDepth      = 16;    // recursion guard

// ---- fourcc helpers -----------------------------------------------------
constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a << 24) |
           ((uint32_t)(uint8_t)b << 16) |
           ((uint32_t)(uint8_t)c <<  8) |
           ((uint32_t)(uint8_t)d <<  0);
}

// canonical box types we care about (a-z is plenty)
namespace Box {
    constexpr uint32_t ftyp = FourCC('f','t','y','p');
    constexpr uint32_t moov = FourCC('m','o','o','v');
    constexpr uint32_t mvhd = FourCC('m','v','h','d');
    constexpr uint32_t trak = FourCC('t','r','a','k');
    constexpr uint32_t tkhd = FourCC('t','k','h','d');
    constexpr uint32_t mdia = FourCC('m','d','i','a');
    constexpr uint32_t mdhd = FourCC('m','d','h','d');
    constexpr uint32_t hdlr = FourCC('h','d','l','r');
    constexpr uint32_t minf = FourCC('m','i','n','f');
    constexpr uint32_t stbl = FourCC('s','t','b','l');
    constexpr uint32_t stsd = FourCC('s','t','s','d');
    constexpr uint32_t stts = FourCC('s','t','t','s');
    constexpr uint32_t ctts = FourCC('c','t','t','s');
    constexpr uint32_t stsc = FourCC('s','t','s','c');
    constexpr uint32_t stsz = FourCC('s','t','s','z');
    constexpr uint32_t stz2 = FourCC('s','t','z','2');
    constexpr uint32_t stco = FourCC('s','t','c','o');
    constexpr uint32_t co64 = FourCC('c','o','6','4');
    constexpr uint32_t stss = FourCC('s','t','s','s');
    constexpr uint32_t mdat = FourCC('m','d','a','t');
    constexpr uint32_t udta = FourCC('u','d','t','a');
    constexpr uint32_t meta = FourCC('m','e','t','a');
    constexpr uint32_t free_ = FourCC('f','r','e','e');
    constexpr uint32_t skip = FourCC('s','k','i','p');
    constexpr uint32_t avc1 = FourCC('a','v','c','1');
    constexpr uint32_t avcC = FourCC('a','v','c','C');
    constexpr uint32_t hvc1 = FourCC('h','v','c','1');
    constexpr uint32_t hev1 = FourCC('h','e','v','1');
    constexpr uint32_t hvcC = FourCC('h','v','c','C');
    constexpr uint32_t mjpg = FourCC('m','j','p','g');
    constexpr uint32_t jpeg = FourCC('j','p','e','g');
    constexpr uint32_t mp4a = FourCC('m','p','4','a');
    constexpr uint32_t esds = FourCC('e','s','d','s');
    constexpr uint32_t Opus = FourCC('O','p','u','s');
    constexpr uint32_t alaw = FourCC('a','l','a','w');
    constexpr uint32_t ulaw = FourCC('u','l','a','w');
    constexpr uint32_t kvid = FourCC('k','v','i','d');
}

// handler types (mdia/hdlr "handler_type" field)
namespace Handler {
    constexpr uint32_t vide = FourCC('v','i','d','e'); // video
    constexpr uint32_t soun = FourCC('s','o','u','n'); // audio
    constexpr uint32_t hint = FourCC('h','i','n','t'); // hint
    constexpr uint32_t subt = FourCC('s','u','b','t'); // subtitle
    constexpr uint32_t text = FourCC('t','e','x','t'); // timed text
}

enum TrackKind : uint8_t {
    TRACK_UNKNOWN = 0,
    TRACK_VIDEO   = 1,
    TRACK_AUDIO   = 2,
    TRACK_OTHER   = 3,
};

// per-sample record built by the demuxer (canonical, decoder-friendly)
struct Sample {
    uint64_t file_offset;   // byte position of sample's first byte
    uint32_t size;          // byte length
    uint64_t dts;           // decoding timestamp, in track timescale
    uint64_t pts;           // composition timestamp, in track timescale
    bool     is_keyframe;   // true if listed in stss (or all samples for audio)
};

struct VideoFormat {
    uint16_t width;         // from stsd visual sample entry
    uint16_t height;
    uint16_t depth;         // bits per pixel from stsd (typically 24)
};

struct AudioFormat {
    uint16_t channels;      // from stsd audio sample entry
    uint16_t sample_size;   // bits per sample (8/16/24/32)
    uint32_t sample_rate;   // samples per second (no fractional part stored)
};

struct Track {
    bool         used;
    uint32_t     track_id;          // tkhd.track_id
    TrackKind    kind;
    uint32_t     handler;           // raw fourcc from hdlr
    uint32_t     timescale;         // mdhd.timescale (units per second)
    uint64_t     duration_units;    // mdhd.duration in timescale units
    uint32_t     codec;             // first stsd entry fourcc (avc1, mp4a, ...)
    VideoFormat  video;             // valid only if kind==TRACK_VIDEO
    AudioFormat  audio;             // valid only if kind==TRACK_AUDIO

    // codec-private blob (avcC for h264, esds-DSI for aac, hvcC for hevc).
    // borrowed pointer into the source mp4 buffer + length, no copy.
    const uint8_t* codec_priv;
    uint32_t       codec_priv_len;

    // built sample table
    Sample*  samples;           // heap, length == sample_count
    uint32_t sample_count;
};

struct Movie {
    uint32_t timescale;             // mvhd.timescale
    uint64_t duration_units;        // mvhd.duration (in movie timescale)
    uint32_t major_brand;           // ftyp.major_brand
    int      track_count;
    Track    tracks[kMaxTracks];
};

// ---- public api ---------------------------------------------------------

// parse the entire box tree and build sample tables.  returns false on
// malformed input.  on success the movie struct + every track's samples
// array are populated.  the caller must invoke Close() to release the
// per-track sample arrays.  the source buffer (data, size) is borrowed
// and must remain valid until Close().
bool Open(const uint8_t* data, uint32_t size, Movie& out);

void Close(Movie& mv);

// helper: find first track of given kind, returns index or -1
int FindFirstTrack(const Movie& mv, TrackKind kind);

// duration of the entire movie, in milliseconds (integer, rounded)
uint32_t DurationMs(const Movie& mv);

// duration of a single track, in milliseconds
uint32_t TrackDurationMs(const Track& tr);

// nearest keyframe at-or-before `pts_units` for a video track.  returns
// sample index, or 0 if none found (sample 0 is always a keyframe by
// convention for h264 in mp4).  for audio tracks every sample is a
// keyframe, so this returns the sample whose dts<=pts_units.
uint32_t SeekKeyframe(const Track& tr, uint64_t pts_units);

// convert milliseconds to track-timescale units.
uint64_t MsToUnits(const Track& tr, uint32_t ms);

// debug: dump movie structure to serial log.
void DumpToSerial(const Movie& mv);

// codec name for serial / ui display (returns static string, never null).
const char* CodecName(uint32_t fourcc);
const char* HandlerName(uint32_t fourcc);

} // namespace MP4
