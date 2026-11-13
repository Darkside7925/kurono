// kurono os - native video player implementation
// see video_player.h.  uses stb_image for jpeg decode, kui::Audio for output
// and kui::Canvas for the blit.  every time value comes from the monotonic
// clock, so speed never depends on how often the desktop paints. (satoru)
#include "video_player.h"
#include "mplat.h"
#include "../kui/kui.h"

// stb_image is implemented in third_party/stb_image_glue.cpp; here we
// declare just the entry points we need. (satoru)
extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                     int* x, int* y, int* comp, int req_comp);
void stbi_image_free(void* retval_from_stbi_load);
const char* stbi_failure_reason(void);
}

namespace VideoPlayer {

static int s_decode_log_budget = 4;
static int s_frame_log_budget = 8;

// how far ahead of the playback position pcm is queued, and the margin past
// which a late audio cursor is flushed instead of played stale. (satoru)
static const uint32_t kAudioLeadMs  = 220;
static const uint32_t kAudioLateMs  = 150;

static inline uint64_t NowMs() { return MPlat::NowMs64(); }

// ----------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------
static void Scpy(char* d, const char* s, int mx) {
    int i = 0; if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static int Iappend(char* dst, int pos, int max, int v) {
    char rev[16]; int rn = 0;
    bool neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    if (u == 0) rev[rn++] = '0';
    while (u && rn < 15) { rev[rn++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && pos < max - 1) dst[pos++] = '-';
    while (rn > 0 && pos < max - 1) dst[pos++] = rev[--rn];
    if (pos < max) dst[pos] = 0;
    return pos;
}
static int Sappend(char* dst, int pos, int max, const char* s) {
    if (!s) return pos;
    while (*s && pos < max - 1) dst[pos++] = *s++;
    if (pos < max) dst[pos] = 0;
    return pos;
}

// ----------------------------------------------------------------------
// rgba buffer management
// ----------------------------------------------------------------------
// drop only the view fields; the pixels live in st.decode_buf. (satoru)
static void FreeRgba(State& st) {
    if (st.rgba_frame && st.rgba_owns) stbi_image_free(st.rgba_frame);
    st.rgba_frame = nullptr;
    st.rgba_w = 0;
    st.rgba_h = 0;
    st.rgba_owns = false;
}

static void FreeDecodeBuf(State& st) {
    if (st.decode_buf) MPlat::Free(st.decode_buf);
    st.decode_buf = nullptr;
    st.decode_cap = 0;
    if (st.decode_buf2) MPlat::Free(st.decode_buf2);
    st.decode_buf2 = nullptr;
    st.decode_cap2 = 0;
    st.prefetch_frame = -1;
}

static void FreeScaledBuf(State& st) {
    if (st.scaled_buf) MPlat::Free(st.scaled_buf);
    st.scaled_buf = nullptr;
    st.scaled_cap = 0;
    st.scaled_valid = false;
    st.scaled_w = 0;
    st.scaled_h = 0;
}

// cap for the decode scratch so a garbage frame size can't ask for an
// unbounded allocation; the scaled cache has its own larger cap (fullscreen
// on a 1440p panel) and falls back to direct scaling beyond it. (satoru)
static const uint32_t kMaxFramePixelsBytes = 1920u * 1080u * 4u;
static const uint32_t kMaxScaledBytes      = 2560u * 1440u * 4u;

// decode a jpeg into *buf (growing it as needed); sets *ow,*oh on success. (satoru)
static bool DecodeJpegToBuf(const uint8_t* jpeg, uint32_t len,
                            uint8_t** buf, uint32_t* cap, int* ow, int* oh) {
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(jpeg, (int)len, &w, &h, &comp, 4);
    if (!rgba) {
        MPlat::Log("[VideoPlayer] jpeg decode failed: ");
        const char* r = stbi_failure_reason();
        if (r) MPlat::Log(r);
        MPlat::Log("\r\n");
        return false;
    }
    if (w <= 0 || h <= 0) { stbi_image_free(rgba); return false; }

    uint64_t need = (uint64_t)w * (uint64_t)h * 4ull;
    if (need > (uint64_t)kMaxFramePixelsBytes) {
        stbi_image_free(rgba);
        MPlat::Log("[VideoPlayer] frame too large, skipping\r\n");
        return false;
    }

    if (!*buf || *cap < (uint32_t)need) {
        if (*buf) MPlat::Free(*buf);
        *buf = (uint8_t*)MPlat::Alloc((uint32_t)need);
        *cap = *buf ? (uint32_t)need : 0;
        if (!*buf) {
            stbi_image_free(rgba);
            MPlat::Log("[VideoPlayer] scratch alloc failed\r\n");
            return false;
        }
    }

    memcpy(*buf, rgba, (size_t)need);
    stbi_image_free(rgba);
    *ow = w; *oh = h;
    return true;
}

static bool DecodeJpegInto(State& st, const uint8_t* jpeg, uint32_t len) {
    if (!DecodeJpegToBuf(jpeg, len, &st.decode_buf, &st.decode_cap, &st.rgba_w, &st.rgba_h)) {
        // keep showing the previous picture rather than dropping to black. (satoru)
        return false;
    }
    st.rgba_frame = st.decode_buf;
    st.rgba_owns  = false;
    st.scaled_valid = false;   // a new frame invalidates the scaled cache (satoru)
    if (s_decode_log_budget > 0) {
        s_decode_log_budget--;
        MPlat::Log("[VideoPlayer] decoded jpeg ");
        MPlat::LogDec(st.rgba_w);
        MPlat::Log("x");
        MPlat::LogDec(st.rgba_h);
        MPlat::Log(" bytes=");
        MPlat::LogDec((int)len);
        MPlat::Log("\r\n");
    }
    return true;
}

// ----------------------------------------------------------------------
// audio stream
// ----------------------------------------------------------------------
static void AudioApplyVolume(State& st) {
    if (st.audio_stream_id <= 0) return;
    kui::Audio::SetVolume(st.audio_stream_id, st.muted ? 0 : st.volume);
}

static void AudioOpen(State& st) {
    if (st.kind != SRC_KVID) return;
    if (!(st.kvid.hdr.flags & KVID::kFlagHasAudio) || st.kvid.hdr.audio_sample_rate == 0) return;
    if (st.kvid.hdr.audio_bits_per_sample != 16 || st.kvid.hdr.audio_channels == 0) return;
    st.audio_stream_id = kui::Audio::Open(st.kvid.hdr.audio_sample_rate, (int)st.kvid.hdr.audio_channels);
    st.audio_part = 0;
    st.audio_written = 0;
    st.audio_underruns_seen = 0;
    AudioApplyVolume(st);
    if (st.audio_stream_id > 0 && st.paused) kui::Audio::Pause(st.audio_stream_id, true);
}

// drop the stream (the fade-out hides the cut).  the id is cleared at once so
// a recycled mixer slot can never be closed by us a second time. (satoru)
static void AudioClose(State& st) {
    if (st.audio_stream_id > 0) {
        kui::Audio::Close(st.audio_stream_id);
        st.audio_stream_id = -1;
    }
}

// flush everything queued and restart the pcm from the video position. this
// is the seek path and the recovery path after an underrun or a long stall. (satoru)
static void AudioResync(State& st, uint32_t pos_ms) {
    if (st.kind != SRC_KVID || !(st.kvid.hdr.flags & KVID::kFlagHasAudio)) return;
    AudioClose(st);
    AudioOpen(st);
    // opening the stream is a synchronous round trip to the audio server (tens of ms; a
    // second if it is busy) and the picture clock kept running through it. aim the pcm at
    // where the clock is NOW: aiming at the position the caller saw made every start and
    // every seek begin 100 to 600 ms late, which the lag check then "fixed" with a second
    // close/open, an audible gap about a second in. (satoru)
    uint32_t fresh = PositionMs(st);
    if (fresh > pos_ms && fresh - pos_ms < 3000) pos_ms = fresh;
    st.audio_cursor = KVID::FrameAtMs(st.kvid, pos_ms);
    st.audio_part = 0;
    st.audio_written = 0;
    st.audio_base_ms = (uint32_t)(st.kvid.index[st.audio_cursor].dts_us / 1000ull);
    // start inside the chunk, at the clock, not at its first sample (up to one video frame early). (satoru)
    if (pos_ms > st.audio_base_ms && st.kvid.hdr.audio_sample_rate) {
        uint32_t skip = (uint32_t)(((uint64_t)(pos_ms - st.audio_base_ms) * st.kvid.hdr.audio_sample_rate) / 1000ull);
        uint32_t a_size = 0;
        const uint8_t* a = KVID::GetFrameAudio(st.kvid, st.audio_cursor, &a_size);
        uint32_t bpf = (uint32_t)st.kvid.hdr.audio_channels * (uint32_t)(st.kvid.hdr.audio_bits_per_sample / 8);
        if (a && bpf && skip < a_size / bpf) { st.audio_part = skip; st.audio_base_ms = pos_ms; }
    }
    st.audio_last_resync_ms = NowMs();
}

// ----------------------------------------------------------------------
// open / close
// ----------------------------------------------------------------------
static void LogOpened(const State& st) {
    MPlat::Log("[VideoPlayer] opened KVID ");
    MPlat::LogDec((int)st.kvid.hdr.width);
    MPlat::Log("x");
    MPlat::LogDec((int)st.kvid.hdr.height);
    MPlat::Log(" frames=");
    MPlat::LogDec((int)st.kvid.hdr.frame_count);
    MPlat::Log(" fps=");
    MPlat::LogDec((int)st.kvid.hdr.fps_num);
    MPlat::Log("/");
    MPlat::LogDec((int)st.kvid.hdr.fps_den);
    MPlat::Log(" audio=");
    MPlat::LogDec((int)st.kvid.hdr.audio_sample_rate);
    MPlat::Log("\r\n");
}

bool Open(const uint8_t* data, uint32_t size, State& st) {
    int keep_volume = st.volume;
    bool keep_muted = st.muted;
    Close(st);
    st.volume = keep_volume > 0 || keep_muted ? keep_volume : 100;
    st.muted = keep_muted;
    if (!data || size < 16) return false;
    st.data = data;
    st.size = size;

    // KVID first (cheaper sniff)
    if (data[0] == 'K' && data[1] == 'V' && data[2] == 'I' && data[3] == 'D') {
        if (KVID::Open(data, size, st.kvid)) {
            st.kind = SRC_KVID;
            st.kvid_cur_frame = 0;
            st.pos_ms = 0;
            st.mp4_video_track = -1;
            st.mp4_audio_track = -1;
            st.audio_stream_id = -1;
            st.audio_cursor = 0;
            // the audio stream opens on Play(), so a window that is opened and
            // never started holds no mixer slot. (satoru)
            uint32_t jpeg_size = 0;
            const uint8_t* jpeg = KVID::GetFrameJpeg(st.kvid, 0, &jpeg_size);
            if (jpeg && jpeg_size >= 4) DecodeJpegInto(st, jpeg, jpeg_size);
            LogOpened(st);
            return true;
        }
    }

    // try mp4
    if (MP4::Open(data, size, st.mp4)) {
        st.kind = SRC_MP4;
        st.mp4_video_track = MP4::FindFirstTrack(st.mp4, MP4::TRACK_VIDEO);
        st.mp4_audio_track = MP4::FindFirstTrack(st.mp4, MP4::TRACK_AUDIO);
        if (st.mp4_video_track >= 0) {
            const MP4::Track& vt = st.mp4.tracks[st.mp4_video_track];
            if (vt.codec == MP4::Box::avc1 && vt.codec_priv && vt.codec_priv_len >= 7) {
                uint8_t nal_size = 4;
                H264::ParseAvcC(vt.codec_priv, vt.codec_priv_len, st.h264, &nal_size);
            }
            // mjpeg-in-mp4 path: decode the first sample as a jpeg
            if (vt.codec == MP4::Box::mjpg || vt.codec == MP4::Box::jpeg) {
                if (vt.sample_count > 0) {
                    const MP4::Sample& s = vt.samples[0];
                    // bounds-check without addition so a malformed sample table
                    // can't overflow file_offset+size. (satoru)
                    if (s.size <= size && s.file_offset <= size - s.size) {
                        DecodeJpegInto(st, data + s.file_offset, s.size);
                    }
                }
            }
        }
        if (st.mp4_audio_track >= 0) {
            const MP4::Track& at = st.mp4.tracks[st.mp4_audio_track];
            if (at.codec == MP4::Box::mp4a && at.codec_priv && at.codec_priv_len >= 2) {
                // esds is wrapped; the AudioSpecificConfig sits inside the 0x05 tag. (satoru)
                const uint8_t* p = at.codec_priv;
                uint32_t n = at.codec_priv_len;
                for (uint32_t i = 0; i + 1 < n; i++) {
                    if (p[i] == 0x05) {
                        uint32_t j = i + 1;
                        uint32_t dsi_len = 0;
                        for (int k = 0; k < 4 && j < n; k++) {
                            uint8_t b = p[j++];
                            dsi_len = (dsi_len << 7) | (b & 0x7f);
                            if (!(b & 0x80)) break;
                        }
                        if (j + dsi_len <= n) {
                            AAC::ParseConfig(p + j, dsi_len, st.aac);
                        }
                        break;
                    }
                }
            }
        }
        st.audio_stream_id = -1;
        st.pos_ms = 0;
        MP4::DumpToSerial(st.mp4);
        return true;
    }
    return false;
}

void Close(State& st) {
    AudioClose(st);
    if (st.kind == SRC_MP4) MP4::Close(st.mp4);
    FreeRgba(st);
    FreeDecodeBuf(st);
    FreeScaledBuf(st);
    st = State{};
    st.audio_stream_id = -1;
    st.mp4_video_track = -1;
    st.mp4_audio_track = -1;
    st.prefetch_frame = -1;
}

// ----------------------------------------------------------------------
// transport
// ----------------------------------------------------------------------
uint32_t DurationMs(const State& st) {
    if (st.kind == SRC_KVID) return KVID::DurationMs(st.kvid);
    if (st.kind == SRC_MP4)  return MP4::DurationMs(st.mp4);
    return 0;
}

// unclamped clock position, used to detect the end. (satoru)
static uint64_t RawPositionMs(const State& st) {
    if (!st.playing || st.paused || st.ended) return st.pos_ms;
    uint64_t now = NowMs();
    return now > st.clock_origin_ms ? now - st.clock_origin_ms : 0;
}

uint32_t PositionMs(const State& st) {
    uint64_t p = RawPositionMs(st);
    uint32_t dur = DurationMs(st);
    if (p > dur) p = dur;
    return (uint32_t)p;
}

bool IsPlaying(const State& st) { return st.playing && !st.paused && !st.ended; }
bool IsEnded(const State& st)   { return st.ended; }

uint32_t ProgressPermil(const State& st) {
    uint32_t d = DurationMs(st);
    if (d == 0) return 0;
    return (uint32_t)(((uint64_t)PositionMs(st) * 1000ull) / d);
}

bool HasAudio(const State& st) {
    return st.kind == SRC_KVID && (st.kvid.hdr.flags & KVID::kFlagHasAudio) &&
           st.kvid.hdr.audio_sample_rate > 0;
}
bool HasPicture(const State& st) { return st.rgba_frame && st.rgba_w > 0 && st.rgba_h > 0; }
int FrameWidth(const State& st)  { return st.rgba_w; }
int FrameHeight(const State& st) { return st.rgba_h; }

void Play(State& st) {
    if (st.kind == SRC_NONE) return;
    if (st.ended) {
        // replay from the top. (satoru)
        st.ended = false;
        st.pos_ms = 0;
        st.paused = false;
        st.playing = false;
        AudioResync(st, 0);
    }
    if (st.playing && !st.paused) return;
    uint32_t at = st.pos_ms;
    st.clock_origin_ms = NowMs() - at;
    st.playing = true;
    st.paused = false;
    if (st.audio_stream_id > 0) {
        kui::Audio::Pause(st.audio_stream_id, false);
    } else if (HasAudio(st)) {
        AudioResync(st, at);
    }
}

void Pause(State& st) {
    if (!st.playing || st.paused || st.ended) return;
    st.pos_ms = PositionMs(st);
    st.paused = true;
    if (st.audio_stream_id > 0) kui::Audio::Pause(st.audio_stream_id, true);
}

void TogglePause(State& st) {
    if (st.ended || st.paused || !st.playing) Play(st);
    else                                      Pause(st);
}

void SeekMs(State& st, uint32_t ms) {
    if (st.kind == SRC_NONE) return;
    uint32_t dur = DurationMs(st);
    if (ms > dur) ms = dur;
    bool was_running = st.playing && !st.paused && !st.ended;
    st.pos_ms = ms;
    st.clock_origin_ms = NowMs() - ms;
    if (st.ended && ms < dur) {
        // scrubbing back from the end leaves the clip paused on that frame. (satoru)
        st.ended = false;
        st.playing = true;
        st.paused = true;
    }
    if (st.prefetch_frame >= 0 && st.kind == SRC_KVID &&
        (uint32_t)st.prefetch_frame != KVID::FrameAtMs(st.kvid, ms)) {
        st.prefetch_frame = -1;   // a seek discards a decode-ahead frame that is not the target (satoru)
    }
    // pcm restarts from the new position; a paused video keeps its stream
    // paused so resuming is instant. (satoru)
    if (HasAudio(st) && (was_running || st.audio_stream_id > 0)) AudioResync(st, ms);
}

void SeekRelativeMs(State& st, int32_t delta_ms) {
    int64_t p = (int64_t)PositionMs(st) + (int64_t)delta_ms;
    if (p < 0) p = 0;
    SeekMs(st, (uint32_t)p);
}

void SetVolume(State& st, int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    st.volume = vol;
    AudioApplyVolume(st);
}

void SetMuted(State& st, bool muted) {
    st.muted = muted;
    AudioApplyVolume(st);
}

// ----------------------------------------------------------------------
// audio pump
// ----------------------------------------------------------------------
void PumpAudio(State& st) {
    if (st.kind != SRC_KVID || !HasAudio(st)) return;
    if (st.ended) {
        // playback is over; keep the stream until the queued tail has played, then free it (satoru)
        if (st.audio_stream_id > 0 && kui::Audio::Queued(st.audio_stream_id) <= 0) {
            kui::Audio::Close(st.audio_stream_id);
            st.audio_stream_id = -1;
        }
        return;
    }
    if (!IsPlaying(st)) return;
    uint64_t now = NowMs();
    if (st.audio_stream_id <= 0) {
        // the mixer had no free slot (or the backend was busy): retry about
        // once a second instead of every iteration. (satoru)
        if (now - st.audio_last_resync_ms < 1000) return;
        st.audio_last_resync_ms = now;
        AudioResync(st, PositionMs(st));
        if (st.audio_stream_id <= 0) return;
    }

    uint32_t pos_ms = PositionMs(st);
    uint64_t pos_us = (uint64_t)pos_ms * 1000ull;
    uint32_t frame_count = st.kvid.hdr.frame_count;
    uint32_t bpf = (uint32_t)st.kvid.hdr.audio_channels *
                   (uint32_t)(st.kvid.hdr.audio_bits_per_sample / 8);
    if (bpf == 0) return;

    // the cursor fell far behind the clock (the window was hidden, the cpu
    // stalled): playing the backlog would run the sound late for good, so
    // restart from here. (satoru)
    if (st.audio_cursor < frame_count &&
        st.kvid.index[st.audio_cursor].dts_us + (uint64_t)kAudioLateMs * 1000ull < pos_us &&
        st.audio_part == 0) {
        st.audio_resyncs++;
        MPlat::Log("[VideoPlayer] audio late, resync at ms=");
        MPlat::LogDec((int)pos_ms);
        MPlat::Log("\r\n");
        AudioResync(st, pos_ms);
        if (st.audio_stream_id <= 0) return;
    }

    // how far the sound the speakers are playing has fallen behind the clock.
    // written frames minus what still sits in the stream and backend queues is
    // the sample being heard. a gap (underrun, stalled pump) shows up as a
    // growing lag; a sink that drains faster than real time shows up as audio
    // ahead, which flushing cannot fix, so only a late stream is resynced,
    // once a second at most. (satoru)
    int64_t queued = (int64_t)kui::Audio::Queued(st.audio_stream_id);
    int64_t played = (int64_t)st.audio_written - queued;
    if (played > 0 && now - st.audio_last_resync_ms > 1000) {
        uint32_t rate = st.kvid.hdr.audio_sample_rate;
        int64_t heard_ms = (int64_t)st.audio_base_ms + (played * 1000) / (int64_t)rate;
        int64_t lag_ms = (int64_t)pos_ms - heard_ms;
        if (lag_ms > (int64_t)kAudioLateMs && lag_ms < 5000) {
            st.audio_resyncs++;
            MPlat::Log("[VideoPlayer] audio lag ");
            MPlat::LogDec((int)lag_ms);
            MPlat::Log("ms, resync at ms=");
            MPlat::LogDec((int)pos_ms);
            MPlat::Log("\r\n");
            AudioResync(st, pos_ms);
            if (st.audio_stream_id <= 0) return;
        }
    }

    uint64_t limit_us = pos_us + (uint64_t)kAudioLeadMs * 1000ull;
    int guard = 64;
    while (st.audio_cursor < frame_count && guard-- > 0) {
        if (st.kvid.index[st.audio_cursor].dts_us > limit_us) break;
        uint32_t a_size = 0;
        const uint8_t* a = KVID::GetFrameAudio(st.kvid, st.audio_cursor, &a_size);
        uint32_t total = a ? a_size / bpf : 0;
        if (total > st.audio_part) {
            int acc = kui::Audio::Write(st.audio_stream_id,
                                        (const int16_t*)(a + (size_t)st.audio_part * bpf),
                                        (int)(total - st.audio_part));
            if (acc > 0) {
                st.audio_part += (uint32_t)acc;
                st.audio_written += (uint32_t)acc;
            }
            if (st.audio_part < total) break;   // ring full: continue next pump
        }
        st.audio_part = 0;
        st.audio_cursor++;
    }
}

// ----------------------------------------------------------------------
// time-driven decode
// ----------------------------------------------------------------------
// the picture follows the clock. the audio queue is not a delay: the first
// queued sample is the first one heard, so no compensation is applied. (satoru)
static uint32_t VideoPositionMs(const State& st) { return PositionMs(st); }

static void FinishPlayback(State& st) {
    st.ended = true;
    st.pos_ms = DurationMs(st);
    st.playing = false;
    st.paused = false;
    // keep the stream; PumpAudio closes it once the queued tail has played. (satoru)
    MPlat::Log("[VideoPlayer] end of file\r\n");
}

bool PumpDecode(State& st) {
    if (st.kind == SRC_NONE) return false;
    if (st.kind == SRC_KVID) {
        if (IsPlaying(st) && RawPositionMs(st) >= DurationMs(st)) {
            // the last frame stays on screen. (satoru)
            FinishPlayback(st);
        }
        uint32_t pos_ms = VideoPositionMs(st);
        uint32_t target_frame = KVID::FrameAtMs(st.kvid, pos_ms);
        if (target_frame >= st.kvid.hdr.frame_count) target_frame = st.kvid.hdr.frame_count - 1;
        if (target_frame != st.kvid_cur_frame) {
            if (s_frame_log_budget > 0) {
                s_frame_log_budget--;
                MPlat::Log("[VideoPlayer] frame=");
                MPlat::LogDec((int)target_frame);
                MPlat::Log(" pos_ms=");
                MPlat::LogDec((int)pos_ms);
                MPlat::Log("\r\n");
            }
            // fast path: this frame was decoded ahead on an idle pass -> swap. (satoru)
            if (st.prefetch_frame == (int)target_frame && st.decode_buf2) {
                uint8_t* tb = st.decode_buf; uint32_t tc = st.decode_cap;
                st.decode_buf  = st.decode_buf2; st.decode_cap  = st.decode_cap2;
                st.decode_buf2 = tb;             st.decode_cap2 = tc;
                st.rgba_w = st.prefetch_w; st.rgba_h = st.prefetch_h;
                st.rgba_frame = st.decode_buf; st.rgba_owns = false;
                st.scaled_valid = false;
                st.prefetch_frame = -1;
            } else {
                uint32_t jpeg_size = 0;
                const uint8_t* jpeg = KVID::GetFrameJpeg(st.kvid, target_frame, &jpeg_size);
                if (jpeg && jpeg_size >= 4) DecodeJpegInto(st, jpeg, jpeg_size);
                else MPlat::Log("[VideoPlayer] missing/short jpeg frame\r\n");
                st.prefetch_frame = -1;
            }
            st.kvid_cur_frame = target_frame;
            return true;
        }
        if (IsPlaying(st)) {
            // display caught up to the clock: decode the NEXT frame ahead so the
            // upcoming boundary is a cheap swap. one prefetch in flight. (satoru)
            uint32_t nxt = target_frame + 1;
            if (st.prefetch_frame != (int)nxt && nxt < st.kvid.hdr.frame_count) {
                uint32_t js = 0;
                const uint8_t* j = KVID::GetFrameJpeg(st.kvid, nxt, &js);
                if (j && js >= 4 &&
                    DecodeJpegToBuf(j, js, &st.decode_buf2, &st.decode_cap2,
                                    &st.prefetch_w, &st.prefetch_h)) {
                    st.prefetch_frame = (int)nxt;
                }
            }
        }
    } else if (st.kind == SRC_MP4) {
        // mjpeg-in-mp4 fast path
        if (st.mp4_video_track >= 0) {
            const MP4::Track& vt = st.mp4.tracks[st.mp4_video_track];
            if (vt.codec == MP4::Box::mjpg || vt.codec == MP4::Box::jpeg) {
                if (IsPlaying(st) && RawPositionMs(st) >= DurationMs(st)) FinishPlayback(st);
                uint32_t pos_ms = PositionMs(st);
                if (vt.timescale == 0 || vt.sample_count == 0) return false;
                uint64_t target_units = MP4::MsToUnits(vt, pos_ms);
                uint32_t target_idx = MP4::SeekKeyframe(vt, target_units);
                if (target_idx >= vt.sample_count) target_idx = vt.sample_count - 1;
                if (target_idx != st.kvid_cur_frame) {
                    const MP4::Sample& s = vt.samples[target_idx];
                    if (s.size <= st.size && s.file_offset <= st.size - s.size) {
                        DecodeJpegInto(st, st.data + s.file_offset, s.size);
                    }
                    st.kvid_cur_frame = target_idx;
                    return true;
                }
            }
        } else if (IsPlaying(st) && RawPositionMs(st) >= DurationMs(st)) {
            FinishPlayback(st);
        }
    }
    return false;
}

void Tick(State& st) {
    PumpAudio(st);
    (void)PumpDecode(st);
}

// ----------------------------------------------------------------------
// drawing
// ----------------------------------------------------------------------
static const uint32_t COL_BG     = 0xFF08080C;
static const uint32_t COL_TEXT   = 0xFFE6E6EA;
static const uint32_t COL_DIM    = 0xFF8E8E96;

static inline uint32_t PackPx(const uint8_t* p) {
    return 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

// one output row, bilinear, from source rows r0/r1 with vertical weight fy
// (0..255), columns [x0,x1) of an sw-wide scaled image. downscaling a video
// with plain nearest-neighbour shimmers; two taps per axis is cheap and
// removes most of it. (satoru)
static void ScaleRow(const State& st, const uint8_t* r0, const uint8_t* r1, int fy,
                     int sw, int x0, int x1, uint32_t* out) {
    int src_w = st.rgba_w;
    uint32_t x_step = ((uint32_t)src_w << 16) / (uint32_t)sw;
    uint32_t xq = (uint32_t)x0 * x_step;
    int ify = 256 - fy;
    for (int i = x0; i < x1; i++) {
        int sx = (int)(xq >> 16);
        int fx = (int)((xq >> 8) & 0xFF);
        int sx1 = sx + 1 < src_w ? sx + 1 : src_w - 1;
        if (sx >= src_w) { sx = src_w - 1; sx1 = sx; }
        const uint8_t* a = r0 + sx * 4;
        const uint8_t* b = r0 + sx1 * 4;
        const uint8_t* c = r1 + sx * 4;
        const uint8_t* d = r1 + sx1 * 4;
        int ifx = 256 - fx;
        uint32_t rr = 0, gg = 0, bb = 0;
        {
            int t0 = (a[0] * ifx + b[0] * fx) >> 8, t1 = (c[0] * ifx + d[0] * fx) >> 8;
            rr = (uint32_t)((t0 * ify + t1 * fy) >> 8);
            t0 = (a[1] * ifx + b[1] * fx) >> 8; t1 = (c[1] * ifx + d[1] * fx) >> 8;
            gg = (uint32_t)((t0 * ify + t1 * fy) >> 8);
            t0 = (a[2] * ifx + b[2] * fx) >> 8; t1 = (c[2] * ifx + d[2] * fx) >> 8;
            bb = (uint32_t)((t0 * ify + t1 * fy) >> 8);
        }
        out[i - x0] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        xq += x_step;
    }
}

// scale output row j of an sw*sh fit into out[x0..x1). (satoru)
static void ScaleRowAt(const State& st, int j, int sw, int sh, int x0, int x1, uint32_t* out) {
    int src_h = st.rgba_h, src_w = st.rgba_w;
    uint32_t y_step = ((uint32_t)src_h << 16) / (uint32_t)sh;
    uint32_t yq = (uint32_t)j * y_step;
    int sy = (int)(yq >> 16);
    int fy = (int)((yq >> 8) & 0xFF);
    int sy1 = sy + 1 < src_h ? sy + 1 : src_h - 1;
    if (sy >= src_h) { sy = src_h - 1; sy1 = sy; }
    ScaleRow(st, st.rgba_frame + (size_t)sy * src_w * 4, st.rgba_frame + (size_t)sy1 * src_w * 4,
             fy, sw, x0, x1, out);
}

// build the whole fitted picture once per frame / size change. false on alloc
// failure; the caller then scales straight into the framebuffer. (satoru)
static bool BuildScaledCache(State& st, int sw, int sh) {
    uint64_t need = (uint64_t)sw * (uint64_t)sh * 4ull;
    if (need == 0 || need > (uint64_t)kMaxScaledBytes) return false;
    if (!st.scaled_buf || st.scaled_cap < (uint32_t)need) {
        if (st.scaled_buf) MPlat::Free(st.scaled_buf);
        st.scaled_buf = (uint8_t*)MPlat::Alloc((uint32_t)need);
        st.scaled_cap = st.scaled_buf ? (uint32_t)need : 0;
        if (!st.scaled_buf) return false;
    }
    uint32_t* out = (uint32_t*)st.scaled_buf;
    for (int j = 0; j < sh; j++) ScaleRowAt(st, j, sw, sh, 0, sw, out + (size_t)j * sw);
    st.scaled_w = sw;
    st.scaled_h = sh;
    return true;
}

static void BlitFrame(State& st, kui::Canvas& c, int x, int y, int dst_w, int dst_h) {
    if (!st.rgba_frame || st.rgba_w <= 0 || st.rgba_h <= 0) return;
    int src_w = st.rgba_w, src_h = st.rgba_h;
    int sw = dst_w, sh = (int)((int64_t)dst_w * src_h / src_w);
    if (sh > dst_h) { sh = dst_h; sw = (int)((int64_t)dst_h * src_w / src_h); }
    if (sw <= 0 || sh <= 0) return;
    int ox = x + (dst_w - sw) / 2;
    int oy = y + (dst_h - sh) / 2;

    bool key_match = st.scaled_valid &&
                     st.scaled_key_frame == st.kvid_cur_frame &&
                     st.scaled_key_dw == dst_w && st.scaled_key_dh == dst_h &&
                     st.scaled_w == sw && st.scaled_h == sh;
    bool have_cache = key_match;
    if (!key_match && BuildScaledCache(st, sw, sh)) {
        st.scaled_key_frame = st.kvid_cur_frame;
        st.scaled_key_dw = dst_w;
        st.scaled_key_dh = dst_h;
        st.scaled_valid = true;
        have_cache = true;
    }

    // the canvas clips to the window, so the whole fitted picture can be handed over at once (satoru)
    if (have_cache) {
        c.Blit(ox, oy, (const uint32_t*)st.scaled_buf, sw, sh, sw);
        return;
    }
    // no cache (too big or out of memory): scale one row at a time into a small buffer (satoru)
    static uint32_t row[4096];
    int n = sw < 4096 ? sw : 4096;
    for (int j = 0; j < sh; j++) {
        ScaleRowAt(st, j, sw, sh, 0, n, row);
        c.Blit(ox, oy + j, row, n, 1, n);
    }
}

void DrawVideo(State& st, kui::Canvas& c, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    c.FillRect(x, y, w, h, COL_BG);
    if (st.rgba_frame) {
        BlitFrame(st, c, x, y, w, h);
        return;
    }
    // no decodable picture: show what the container holds. text is the whole-pixel twin because this file is
    // built without sse. (satoru)
    int px = 16 * kui::CurrentTheme().ui_scale / 100;
    char line[128];
    DescribeShort(st, line, sizeof(line));
    c.TextI(x + 20, y + 20, kui::Face::Sans, px, line, COL_TEXT);
    if (st.kind == SRC_MP4) {
        int cy = y + 46;
        for (int i = 0; i < st.mp4.track_count && cy + 16 < y + h; i++) {
            const MP4::Track& t = st.mp4.tracks[i];
            int p = 0;
            p = Sappend(line, p, sizeof(line), "track ");
            p = Iappend(line, p, sizeof(line), i);
            p = Sappend(line, p, sizeof(line), ": ");
            p = Sappend(line, p, sizeof(line), MP4::CodecName(t.codec));
            if (t.kind == MP4::TRACK_VIDEO) {
                p = Sappend(line, p, sizeof(line), "  ");
                p = Iappend(line, p, sizeof(line), t.video.width);
                p = Sappend(line, p, sizeof(line), "x");
                p = Iappend(line, p, sizeof(line), t.video.height);
            } else if (t.kind == MP4::TRACK_AUDIO) {
                p = Sappend(line, p, sizeof(line), "  ");
                p = Iappend(line, p, sizeof(line), (int)t.audio.sample_rate);
                p = Sappend(line, p, sizeof(line), " Hz");
            }
            c.TextI(x + 20, cy, kui::Face::Sans, px, line, COL_DIM);
            cy += 18;
        }
        c.TextI(x + 20, cy + 10, kui::Face::Sans, px,
                "this codec cannot be decoded here. convert it to kvid to play it.", COL_DIM);
    } else if (st.kind == SRC_NONE) {
        c.TextI(x + 20, y + 46, kui::Face::Sans, px, "no media loaded", COL_DIM);
    }
}

void DescribeShort(const State& st, char* out, int max) {
    if (!out || max < 4) return;
    int p = 0;
    if (st.kind == SRC_NONE) { Scpy(out, "(empty)", max); return; }
    if (st.kind == SRC_KVID) {
        p = Sappend(out, p, max, "KVID  ");
        p = Iappend(out, p, max, (int)st.kvid.hdr.width);
        p = Sappend(out, p, max, "x");
        p = Iappend(out, p, max, (int)st.kvid.hdr.height);
        p = Sappend(out, p, max, "  ");
        p = Iappend(out, p, max, (int)(st.kvid.hdr.fps_den
                                       ? st.kvid.hdr.fps_num / st.kvid.hdr.fps_den
                                       : 0));
        p = Sappend(out, p, max, " fps");
        if (st.kvid.hdr.flags & KVID::kFlagHasAudio) {
            p = Sappend(out, p, max, "  ");
            p = Iappend(out, p, max, (int)st.kvid.hdr.audio_sample_rate);
            p = Sappend(out, p, max, " Hz audio");
        }
    } else if (st.kind == SRC_MP4) {
        p = Sappend(out, p, max, "MP4  ");
        if (st.mp4_video_track >= 0) {
            const MP4::Track& v = st.mp4.tracks[st.mp4_video_track];
            p = Sappend(out, p, max, MP4::CodecName(v.codec));
            p = Sappend(out, p, max, " ");
            p = Iappend(out, p, max, v.video.width);
            p = Sappend(out, p, max, "x");
            p = Iappend(out, p, max, v.video.height);
        }
        if (st.mp4_audio_track >= 0) {
            const MP4::Track& a = st.mp4.tracks[st.mp4_audio_track];
            p = Sappend(out, p, max, " + ");
            p = Sappend(out, p, max, MP4::CodecName(a.codec));
        }
    }
}

} // namespace VideoPlayer
