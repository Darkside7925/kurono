// kurono os - native video player
// =================================================================
// video player engine with two playback paths:
//
//   1. KVID native - mjpeg + s16 pcm.  the player decodes one jpeg frame
//      per video frame via stb_image and feeds the pcm to AudioServer's
//      mixer from its own cursor, so audio is independent of paint rate.
//
//   2. MP4 inspect - full demux, h.264/aac metadata extraction; if the
//      mp4 happens to contain mjpeg tracks we play those, else we
//      display the parsed structure so the user knows what is in the file.
//
// the engine has no ui of its own: apps/denji_app.cpp owns the window,
// the control bar and the input handling and calls into this api.  all
// timing comes from the real clock (Timer::GetRealMs64), never from
// frame counts, so playback speed is independent of the desktop refresh
// rate (60 to 240 hz).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kvid.h"
#include "mp4_demux.h"
#include "h264_parse.h"
#include "aac_parse.h"
#include "../kui/kui.h"

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

    // transport.  the clock is origin based: position = now - clock_origin_ms
    // while running, pos_ms while paused / stopped / ended. (satoru)
    bool     playing;                  // clock started (stays true while paused)
    bool     paused;
    bool     ended;                    // reached the end; Play() restarts
    uint64_t clock_origin_ms;          // Timer::GetRealMs64() value at position 0
    uint32_t pos_ms;                   // position while not running

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

    // decode-ahead: while the displayed frame is current we decode the NEXT
    // frame into this second buffer, so advancing is a pointer swap. (satoru)
    uint8_t* decode_buf2;
    uint32_t decode_cap2;
    int      prefetch_frame;            // frame index held in decode_buf2, -1 = none
    int      prefetch_w;
    int      prefetch_h;

    // scaled-output cache: the letterboxed bgra result for the current
    // (frame,dst) so a static frame re-blits with row memcpy. (satoru)
    uint8_t* scaled_buf;                // owned, cap == scaled_cap bytes
    uint32_t scaled_cap;
    int      scaled_w;                  // pixel dims of cached scaled image
    int      scaled_h;
    uint32_t scaled_key_frame;          // kvid_cur_frame the cache was built for
    int      scaled_key_dw;             // dst_w the cache was built for
    int      scaled_key_dh;             // dst_h the cache was built for
    bool     scaled_valid;

    // audio (kvid only).  audio_cursor is the next frame whose pcm has not
    // been handed to the mixer; audio_part is how many sample frames of that
    // chunk the ring already accepted. (satoru)
    int      audio_stream_id;
    uint32_t audio_cursor;
    uint32_t audio_part;
    uint32_t audio_underruns_seen;
    uint64_t audio_last_resync_ms;
    uint32_t audio_resyncs;
    uint32_t audio_base_ms;             // media time of the first sample written to the current stream
    uint64_t audio_written;             // frames handed to the current stream, for the heard position (satoru)
    int      volume;                    // 0..100 stream volume
    bool     muted;
};

// open a buffer; auto-detects KVID magic vs MP4 ftyp box.  returns
// false if neither.  buffer is borrowed.
bool Open(const uint8_t* data, uint32_t size, State& st);

// stops audio, frees every buffer and zeroes the state.
void Close(State& st);

void Play(State& st);              // resume / start; restarts from 0 after the end
void Pause(State& st);
void TogglePause(State& st);
void SeekMs(State& st, uint32_t ms);    // also valid while paused / ended
void SeekRelativeMs(State& st, int32_t delta_ms);

// transport queries
bool     IsPlaying(const State& st);    // clock running (not paused, not ended)
bool     IsEnded(const State& st);
uint32_t PositionMs(const State& st);
uint32_t DurationMs(const State& st);
uint32_t ProgressPermil(const State& st);
bool     HasAudio(const State& st);
bool     HasPicture(const State& st);   // a frame has been decoded and can be drawn
int      FrameWidth(const State& st);
int      FrameHeight(const State& st);

// volume 0..100 and mute, applied to the player's own mixer stream.
void SetVolume(State& st, int vol);
void SetMuted(State& st, bool muted);

// feed the mixer from the audio cursor.  cheap; safe to call every loop
// iteration whether or not the window is being painted. (satoru)
void PumpAudio(State& st);

// advance to the frame due at "now" and decode it, without drawing.
// returns true if the displayed frame changed.  works while paused too, so
// a seek on a paused video shows the new frame. (satoru)
bool PumpDecode(State& st);

// legacy forwarder
void Tick(State& st);

// draw the picture (or the metadata view when there is no picture) into the
// pixel rectangle on a black background.  no controls: the app draws those.
void DrawVideo(State& st, kui::Canvas& c, int x, int y, int w, int h);

// produce a one-line summary like "h264 high 1280x720 24fps + aac lc"
void DescribeShort(const State& st, char* out, int max);

} // namespace VideoPlayer
