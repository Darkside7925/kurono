// kurono os  -  native video player
// =================================================================
// industrial-grade video player capable of two playback paths:
//
//   1. KVID native  -  guaranteed-to-work (mjpeg + s16 pcm).  the player
//      decodes one jpeg frame per video tick via stb_image and pushes
//      audio samples to AudioServer's mixer for proper sync.
//
//   2. MP4 inspect  -  full demux, h.264/aac metadata extraction; if the
//      mp4 happens to contain mjpeg or kvid tracks we play those, else
//      we display the parsed structure so the user knows exactly
//      what's in the file.
//
// the player exposes a small wm-friendly api:
//   * Open() / Close()  -  load buffer
//   * Tick()            -  call once per frame from the main loop
//   * Render()          -  paint into the window
//   * Input()           -  handle clicks / keys
//
// integrates with the new AudioServer + AudioMixer for sound output.
#pragma once
#include "../kernel/types.h"
#include "kvid.h"
#include "mp4_demux.h"
#include "h264_parse.h"
#include "aac_parse.h"

namespace VideoPlayer {

enum SourceKind : uint8_t {
    SRC_NONE = 0,
    SRC_KVID = 1,
    SRC_MP4  = 2,
};

struct State {
    SourceKind kind;
    const uint8_t* data;
    uint32_t       size;

    // kvid mode
    KVID::File kvid;
    uint32_t   kvid_cur_frame;        // index of the frame currently displayed

    // mp4 mode
    MP4::Movie       mp4;
    H264::StreamInfo h264;
    AAC::Config      aac;
    int              mp4_video_track;
    int              mp4_audio_track;

    // shared playback state
    bool     playing;
    bool     paused;
    uint32_t play_started_ms;          // wall ms at play start
    uint32_t pause_remainder_ms;       // when paused, where we were
    uint32_t pos_ms;                   // last known position (also used while paused)

    // last decoded RGBA frame (for redraw without re-decoding jpeg)
    uint8_t* rgba_frame;
    int      rgba_w;
    int      rgba_h;
    bool     rgba_owns;                 // true if rgba_frame allocated by stb

    // persistent decode scratch: one owned rgba buffer reused across frames
    // so we don't malloc/free w*h*4 every frame. grows only when a larger
    // frame appears, freed in Close. (satoru)
    uint8_t* decode_buf;
    uint32_t decode_cap;                // capacity in bytes

    // decode-ahead: on a compositor frame where the displayed video frame hasn't
    // advanced (display caught up to the clock), we decode the NEXT video frame
    // into this second buffer. when the clock then crosses into it, advancing is
    // a cheap pointer swap instead of an inline jpeg decode that stalls the
    // compositor  -  so frames land on time. (satoru)
    uint8_t* decode_buf2;
    uint32_t decode_cap2;
    int      prefetch_frame;            // frame index held in decode_buf2, -1 = none
    int      prefetch_w;
    int      prefetch_h;

    // scaled-output cache: the letterboxed bgra result for the current
    // (frame,dst) blitted straight via row memcpy on a static frame so the
    // per-pixel scale runs only when the frame or window size changes. (satoru)
    uint8_t* scaled_buf;                // owned, cap == scaled_cap bytes
    uint32_t scaled_cap;
    int      scaled_w;                  // pixel dims of cached scaled image
    int      scaled_h;
    uint32_t scaled_key_frame;          // kvid_cur_frame the cache was built for
    int      scaled_key_dw;             // dst_w the cache was built for
    int      scaled_key_dh;             // dst_h the cache was built for
    bool     scaled_valid;

    // audio mixer stream id (kvid only)
    int audio_stream_id;
};

// open a buffer; auto-detects KVID magic vs MP4 ftyp box.  returns
// false if neither.  buffer is borrowed.
bool Open(const uint8_t* data, uint32_t size, State& st);

void Close(State& st);

void Play(State& st);
void Pause(State& st);
void TogglePause(State& st);
void SeekMs(State& st, uint32_t ms);

// progress query in 0..1000 (per mil) for ui scrub bar
uint32_t ProgressPermil(const State& st);
uint32_t PositionMs(const State& st);
uint32_t DurationMs(const State& st);

// pump time-driven decoding; safe to call every main-loop iteration.
void Tick(State& st);

// decode-only pump: advances to the frame due at "now" and decodes it,
// WITHOUT any drawing. this is the heavy half of Tick split out so a
// caller can run the decode on its own cadence (e.g. a scheduler step)
// separate from the gui blit. Tick() forwards to this. returns true if a
// new frame was decoded this call (i.e. the displayed frame changed). a
// cooperative scheduler has no background thread, so "decouple" means
// calling this from a lighter cadence than the compositor, not real
// concurrency. (satoru)
bool PumpDecode(State& st);

// blit current frame + overlay HUD into pixel rectangle (x,y,w,h).
// non-const: maintains an internal scaled-output cache so a static frame
// re-blits without re-running the per-pixel scale. (satoru)
void Render(State& st, int x, int y, int w, int h);

// produce a one-line summary like "h264 high 1280x720 24fps + aac lc"
void DescribeShort(const State& st, char* out, int max);

} // namespace VideoPlayer
