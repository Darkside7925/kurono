// kurono os  -  native video player implementation
// see video_player.h.  uses stb_image for jpeg decode, the new
// AudioServer/AudioMixer for output, and Graphics for blit.
#include "video_player.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_format.h"
#include "../kernel/heap.h"
#include "../system/logging.h"
#include "../drivers/serial.h"

// stb_image is implemented in third_party/stb_image_glue.cpp; here we
// declare just the entry points we need.
extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                     int* x, int* y, int* comp, int req_comp);
void stbi_image_free(void* retval_from_stbi_load);
const char* stbi_failure_reason(void);
}

namespace VideoPlayer {

static int s_decode_log_budget = 8;
static int s_frame_log_budget = 24;
static bool s_logged_fast_blit = false;

// ----------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------
static void Scpy(char* d, const char* s, int mx) {
    int i = 0; if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static int Iappend(char* dst, int pos, int max, int v) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else {
        bool neg = v < 0;
        unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
        char rev[16]; int rn = 0;
        while (u && rn < 15) { rev[rn++] = (char)('0' + (u % 10)); u /= 10; }
        if (neg && pos < max - 1) dst[pos++] = '-';
        while (rn > 0 && pos < max - 1) dst[pos++] = rev[--rn];
        return pos;
    }
    int i = 0;
    while (i < n && pos < max - 1) dst[pos++] = tmp[i++];
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
static void FreeRgba(State& st) {
    if (st.rgba_frame && st.rgba_owns) stbi_image_free(st.rgba_frame);
    st.rgba_frame = nullptr;
    st.rgba_w = 0;
    st.rgba_h = 0;
    st.rgba_owns = false;
}

static bool DecodeJpegInto(State& st, const uint8_t* jpeg, uint32_t len) {
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(jpeg, (int)len, &w, &h, &comp, 4);
    if (!rgba) {
        SerialLogger::Log("[VideoPlayer] jpeg decode failed: ");
        const char* r = stbi_failure_reason();
        if (r) SerialLogger::Log(r);
        SerialLogger::Log("\r\n");
        return false;
    }
    FreeRgba(st);
    st.rgba_frame = rgba;
    st.rgba_w     = w;
    st.rgba_h     = h;
    st.rgba_owns  = true;
    if (s_decode_log_budget > 0) {
        s_decode_log_budget--;
        SerialLogger::Log("[VideoPlayer] decoded jpeg ");
        SerialLogger::LogDec(w);
        SerialLogger::Log("x");
        SerialLogger::LogDec(h);
        SerialLogger::Log(" bytes=");
        SerialLogger::LogDec((int)len);
        SerialLogger::Log("\r\n");
    }
    return true;
}

// ----------------------------------------------------------------------
// open
// ----------------------------------------------------------------------
bool Open(const uint8_t* data, uint32_t size, State& st) {
    Close(st);
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
            // open audio mixer stream if file has audio
            if ((st.kvid.hdr.flags & KVID::kFlagHasAudio) &&
                st.kvid.hdr.audio_sample_rate > 0) {
                st.audio_stream_id = AudioServer::OpenStream(
                    "kvid",
                    AudioFormat::FMT_S16_LE,
                    st.kvid.hdr.audio_sample_rate,
                    (int)st.kvid.hdr.audio_channels);
            } else {
                st.audio_stream_id = -1;
            }
            // pre-decode the first frame so the player has something to show
            uint32_t jpeg_size = 0;
            const uint8_t* jpeg = KVID::GetFrameJpeg(st.kvid, 0, &jpeg_size);
            if (jpeg && jpeg_size >= 4) DecodeJpegInto(st, jpeg, jpeg_size);
            SerialLogger::Log("[VideoPlayer] opened KVID ");
            SerialLogger::LogDec((int)st.kvid.hdr.width);
            SerialLogger::Log("x");
            SerialLogger::LogDec((int)st.kvid.hdr.height);
            SerialLogger::Log(" frames=");
            SerialLogger::LogDec((int)st.kvid.hdr.frame_count);
            SerialLogger::Log(" fps=");
            SerialLogger::LogDec((int)st.kvid.hdr.fps_num);
            SerialLogger::Log("/");
            SerialLogger::LogDec((int)st.kvid.hdr.fps_den);
            SerialLogger::Log("\r\n");
            return true;
        }
    }

    // try mp4
    if (MP4::Open(data, size, st.mp4)) {
        st.kind = SRC_MP4;
        st.mp4_video_track = MP4::FindFirstTrack(st.mp4, MP4::TRACK_VIDEO);
        st.mp4_audio_track = MP4::FindFirstTrack(st.mp4, MP4::TRACK_AUDIO);
        // try to extract codec metadata if h264 video
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
                    if (s.file_offset + s.size <= size) {
                        DecodeJpegInto(st, data + s.file_offset, s.size);
                    }
                }
            }
        }
        if (st.mp4_audio_track >= 0) {
            const MP4::Track& at = st.mp4.tracks[st.mp4_audio_track];
            if (at.codec == MP4::Box::mp4a && at.codec_priv && at.codec_priv_len >= 2) {
                // esds is wrapped; the AudioSpecificConfig sits inside.
                // robust extraction: find the 0x05 tag (DecSpecificInfo).
                const uint8_t* p = at.codec_priv;
                uint32_t n = at.codec_priv_len;
                for (uint32_t i = 0; i + 1 < n; i++) {
                    if (p[i] == 0x05) {
                        // skip BER length
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
    if (st.audio_stream_id >= 0) {
        AudioServer::CloseStream(st.audio_stream_id);
        st.audio_stream_id = -1;
    }
    if (st.kind == SRC_MP4) MP4::Close(st.mp4);
    FreeRgba(st);
    st = State{};
    st.audio_stream_id = -1;
    st.mp4_video_track = -1;
    st.mp4_audio_track = -1;
}

// ----------------------------------------------------------------------
// transport
// ----------------------------------------------------------------------
void Play(State& st) {
    if (st.playing && !st.paused) return;
    uint32_t now = Timer::GetRealMs();
    if (st.paused) {
        // resume  -  adjust play_started_ms so play_started_ms+now-pos==now
        st.play_started_ms = now - st.pause_remainder_ms;
    } else {
        st.play_started_ms = now - st.pos_ms;
    }
    st.playing = true;
    st.paused  = false;
}

void Pause(State& st) {
    if (!st.playing || st.paused) return;
    st.pause_remainder_ms = PositionMs(st);
    st.pos_ms             = st.pause_remainder_ms;
    st.paused             = true;
}

void TogglePause(State& st) {
    if (st.paused || !st.playing) Play(st);
    else                          Pause(st);
}

void SeekMs(State& st, uint32_t ms) {
    if (ms > DurationMs(st)) ms = DurationMs(st);
    st.pos_ms = ms;
    if (st.kind == SRC_KVID) {
        st.kvid_cur_frame = KVID::FrameAtMs(st.kvid, ms);
    }
    uint32_t now = Timer::GetRealMs();
    st.play_started_ms = now - ms;
    st.pause_remainder_ms = ms;
}

uint32_t PositionMs(const State& st) {
    if (st.paused || !st.playing) return st.pos_ms;
    uint32_t now = Timer::GetRealMs();
    if (now < st.play_started_ms) return 0;
    uint32_t pos = now - st.play_started_ms;
    uint32_t dur = DurationMs(st);
    if (pos > dur) pos = dur;
    return pos;
}

uint32_t DurationMs(const State& st) {
    if (st.kind == SRC_KVID) return KVID::DurationMs(st.kvid);
    if (st.kind == SRC_MP4)  return MP4::DurationMs(st.mp4);
    return 0;
}

uint32_t ProgressPermil(const State& st) {
    uint32_t d = DurationMs(st);
    if (d == 0) return 0;
    uint32_t p = PositionMs(st);
    return (uint32_t)(((uint64_t)p * 1000ull) / d);
}

// ----------------------------------------------------------------------
// time-driven decode
// ----------------------------------------------------------------------
void Tick(State& st) {
    if (!st.playing || st.paused) return;
    if (st.kind == SRC_KVID) {
        // figure out which frame we should be on at "now"
        uint32_t pos_ms = PositionMs(st);
        uint32_t target_frame = KVID::FrameAtMs(st.kvid, pos_ms);
        if (target_frame >= st.kvid.hdr.frame_count) {
            // end of file → pause
            st.paused = true;
            st.pos_ms = DurationMs(st);
            return;
        }
        // decode frames we've passed (usually just the next one)
        if (target_frame != st.kvid_cur_frame) {
            uint32_t jpeg_size = 0;
            const uint8_t* jpeg = KVID::GetFrameJpeg(st.kvid, target_frame, &jpeg_size);
            if (s_frame_log_budget > 0) {
                s_frame_log_budget--;
                SerialLogger::Log("[VideoPlayer] frame=");
                SerialLogger::LogDec((int)target_frame);
                SerialLogger::Log(" pos_ms=");
                SerialLogger::LogDec((int)pos_ms);
                SerialLogger::Log(" jpeg=");
                SerialLogger::LogDec((int)jpeg_size);
                SerialLogger::Log("\r\n");
            }
            if (jpeg && jpeg_size >= 4) {
                DecodeJpegInto(st, jpeg, jpeg_size);
            } else {
                SerialLogger::Log("[VideoPlayer] missing/short jpeg frame\r\n");
            }
            // push audio for this frame interval
            if (st.audio_stream_id >= 0) {
                uint32_t a_size = 0;
                const uint8_t* a = KVID::GetFrameAudio(st.kvid, target_frame, &a_size);
                if (a && a_size >= 4) {
                    uint32_t bytes_per_frame =
                        (uint32_t)st.kvid.hdr.audio_channels *
                        (uint32_t)(st.kvid.hdr.audio_bits_per_sample / 8);
                    if (bytes_per_frame > 0) {
                        AudioServer::WriteStream(st.audio_stream_id, a,
                                                 a_size / bytes_per_frame);
                    }
                }
            }
            st.kvid_cur_frame = target_frame;
        }
    } else if (st.kind == SRC_MP4) {
        // mjpeg-in-mp4 fast path
        if (st.mp4_video_track >= 0) {
            const MP4::Track& vt = st.mp4.tracks[st.mp4_video_track];
            if (vt.codec == MP4::Box::mjpg || vt.codec == MP4::Box::jpeg) {
                uint32_t pos_ms = PositionMs(st);
                if (vt.timescale == 0 || vt.sample_count == 0) return;
                uint64_t target_units = MP4::MsToUnits(vt, pos_ms);
                uint32_t target_idx = MP4::SeekKeyframe(vt, target_units);
                if (target_idx >= vt.sample_count) {
                    st.paused = true;
                    return;
                }
                if (target_idx != st.kvid_cur_frame) {
                    const MP4::Sample& s = vt.samples[target_idx];
                    if (s.file_offset + s.size <= st.size) {
                        DecodeJpegInto(st, st.data + s.file_offset, s.size);
                    }
                    st.kvid_cur_frame = target_idx;
                }
            }
        }
    }
}

// ----------------------------------------------------------------------
// drawing
// ----------------------------------------------------------------------
static const uint32_t COL_BG     = 0xFF101018;
static const uint32_t COL_PANEL  = 0xFF1A1A28;
static const uint32_t COL_FILL   = 0xFF5C8AFF;
static const uint32_t COL_TRACK  = 0xFF353550;
static const uint32_t COL_TEXT   = 0xFFE0E0F0;
static const uint32_t COL_DIM    = 0xFF888899;

static void BlitRgba(const State& st, int x, int y, int dst_w, int dst_h) {
    if (!st.rgba_frame || st.rgba_w <= 0 || st.rgba_h <= 0) return;
    int src_w = st.rgba_w, src_h = st.rgba_h;
    // compute fit-with-aspect rect
    int sw = dst_w, sh = (int)((int64_t)dst_w * src_h / src_w);
    if (sh > dst_h) { sh = dst_h; sw = (int)((int64_t)dst_h * src_w / src_h); }
    int ox = x + (dst_w - sw) / 2;
    int oy = y + (dst_h - sh) / 2;
    if (sw <= 0 || sh <= 0) return;
    uint32_t x_step_q16 = ((uint32_t)src_w << 16) / (uint32_t)sw;
    uint32_t y_step_q16 = ((uint32_t)src_h << 16) / (uint32_t)sh;

    uint8_t* dst = Graphics::GetActiveBuffer();
    if (dst && Graphics::GetBpp() == 32) {
        int screen_w = Graphics::GetWidth();
        int screen_h = Graphics::GetHeight();
        int draw_x0 = ox < 0 ? 0 : ox;
        int draw_y0 = oy < 0 ? 0 : oy;
        int draw_x1 = ox + sw;
        int draw_y1 = oy + sh;
        if (draw_x1 > screen_w) draw_x1 = screen_w;
        if (draw_y1 > screen_h) draw_y1 = screen_h;

        if (draw_x0 < draw_x1 && draw_y0 < draw_y1) {
            if (!s_logged_fast_blit) {
                SerialLogger::Log("[VideoPlayer] using direct 32bpp blit\r\n");
                s_logged_fast_blit = true;
            }
            int start_i = draw_x0 - ox;
            int start_j = draw_y0 - oy;
            int visible_w = draw_x1 - draw_x0;
            int visible_h = draw_y1 - draw_y0;
            uint32_t pitch = Graphics::GetPitch();
            uint32_t y_q16 = (uint32_t)start_j * y_step_q16;

            for (int j = 0; j < visible_h; j++) {
                int src_y = (int)(y_q16 >> 16);
                if (src_y >= src_h) src_y = src_h - 1;
                const uint8_t* row = st.rgba_frame + src_y * src_w * 4;
                uint32_t* dst_row = (uint32_t*)(dst + (draw_y0 + j) * pitch) + draw_x0;
                uint32_t x_q16 = (uint32_t)start_i * x_step_q16;
                for (int i = 0; i < visible_w; i++) {
                    int src_x = (int)(x_q16 >> 16);
                    if (src_x >= src_w) src_x = src_w - 1;
                    const uint8_t* p = row + src_x * 4;
                    dst_row[i] = 0xFF000000u |
                                 ((uint32_t)p[0] << 16) |
                                 ((uint32_t)p[1] << 8) |
                                 (uint32_t)p[2];
                    x_q16 += x_step_q16;
                }
                y_q16 += y_step_q16;
            }
            return;
        }
    }

    // fallback for non-32bpp targets
    uint32_t y_q16 = 0;
    for (int j = 0; j < sh; j++) {
        const uint8_t* row = st.rgba_frame + (y_q16 >> 16) * src_w * 4;
        uint32_t x_q16 = 0;
        for (int i = 0; i < sw; i++) {
            const uint8_t* p = row + (x_q16 >> 16) * 4;
            uint32_t c = 0xFF000000u |
                         ((uint32_t)p[0] << 16) |
                         ((uint32_t)p[1] << 8) |
                         (uint32_t)p[2];
            Graphics::DrawPixel(ox + i, oy + j, c);
            x_q16 += x_step_q16;
        }
        y_q16 += y_step_q16;
    }
}

void Render(const State& st, int x, int y, int w, int h) {
    Graphics::FillRect(x, y, w, h, COL_BG);

    // controls strip at bottom (32px)
    int ctl_h = 36;
    int video_h = h - ctl_h - 8;
    if (video_h < 32) video_h = 32;
    int video_w = w - 16;
    int video_x = x + 8;
    int video_y = y + 4;

    // video / placeholder
    Graphics::FillRect(video_x, video_y, video_w, video_h, 0xFF000000);
    if (st.rgba_frame) {
        BlitRgba(st, video_x, video_y, video_w, video_h);
    } else {
        // metadata view
        char line[128];
        DescribeShort(st, line, sizeof(line));
        Graphics::DrawString(video_x + 12, video_y + 12, line, COL_TEXT, 0xFF000000);
        if (st.kind == SRC_MP4) {
            int cy = video_y + 36;
            for (int i = 0; i < st.mp4.track_count && cy + 16 < video_y + video_h; i++) {
                const MP4::Track& t = st.mp4.tracks[i];
                int p = 0;
                p = Sappend(line, p, sizeof(line), "  track ");
                p = Iappend(line, p, sizeof(line), i);
                p = Sappend(line, p, sizeof(line), ": ");
                p = Sappend(line, p, sizeof(line), MP4::CodecName(t.codec));
                p = Sappend(line, p, sizeof(line), " · samples=");
                p = Iappend(line, p, sizeof(line), (int)t.sample_count);
                if (t.kind == MP4::TRACK_VIDEO) {
                    p = Sappend(line, p, sizeof(line), " · ");
                    p = Iappend(line, p, sizeof(line), t.video.width);
                    p = Sappend(line, p, sizeof(line), "x");
                    p = Iappend(line, p, sizeof(line), t.video.height);
                } else if (t.kind == MP4::TRACK_AUDIO) {
                    p = Sappend(line, p, sizeof(line), " · ");
                    p = Iappend(line, p, sizeof(line), (int)t.audio.sample_rate);
                    p = Sappend(line, p, sizeof(line), "Hz x");
                    p = Iappend(line, p, sizeof(line), t.audio.channels);
                    p = Sappend(line, p, sizeof(line), "ch");
                }
                Graphics::DrawString(video_x + 12, cy, line, COL_TEXT, 0xFF000000);
                cy += 16;
            }
            if (st.h264.valid) {
                int p = 0;
                p = Sappend(line, p, sizeof(line), "  H.264 ");
                p = Sappend(line, p, sizeof(line), H264::ProfileName(st.h264.sps.profile_idc));
                p = Sappend(line, p, sizeof(line), " level ");
                p = Iappend(line, p, sizeof(line), st.h264.sps.level_idc / 10);
                p = Sappend(line, p, sizeof(line), ".");
                p = Iappend(line, p, sizeof(line), st.h264.sps.level_idc % 10);
                if (st.h264.fps_num) {
                    p = Sappend(line, p, sizeof(line), " · ");
                    p = Iappend(line, p, sizeof(line),
                                (int)(st.h264.fps_num / (st.h264.fps_den ? st.h264.fps_den : 1)));
                    p = Sappend(line, p, sizeof(line), " fps");
                }
                Graphics::DrawString(video_x + 12, cy, line, COL_DIM, 0xFF000000);
                cy += 16;
            }
            if (st.aac.audio_object_type) {
                int p = 0;
                p = Sappend(line, p, sizeof(line), "  ");
                p = Sappend(line, p, sizeof(line), AAC::ProfileName(st.aac.audio_object_type));
                p = Sappend(line, p, sizeof(line), " · ");
                p = Iappend(line, p, sizeof(line), (int)st.aac.sample_rate);
                p = Sappend(line, p, sizeof(line), " Hz · ");
                p = Iappend(line, p, sizeof(line), st.aac.channels);
                p = Sappend(line, p, sizeof(line), "ch");
                Graphics::DrawString(video_x + 12, cy, line, COL_DIM, 0xFF000000);
            }
        } else if (st.kind == SRC_NONE) {
            Graphics::DrawString(video_x + 12, video_y + 36,
                                 "no media loaded", COL_DIM, 0xFF000000);
        }
    }

    // controls
    int cy = y + h - ctl_h;
    Graphics::FillRect(x, cy, w, ctl_h, COL_PANEL);
    // play/pause icon (simple square / triangle marker)
    int btn = cy + 8;
    Graphics::FillRect(x + 10, btn, 20, 20, COL_FILL);
    if (st.playing && !st.paused) {
        Graphics::FillRect(x + 14, btn + 4, 4, 12, 0xFFFFFFFF);
        Graphics::FillRect(x + 22, btn + 4, 4, 12, 0xFFFFFFFF);
    } else {
        for (int i = 0; i < 12; i++) {
            int len = 12 - i;
            Graphics::FillRect(x + 16, btn + 4 + i, len, 1, 0xFFFFFFFF);
        }
    }
    // scrubber
    int sx = x + 40;
    int sw = w - 130;
    int sy = cy + 16;
    Graphics::FillRect(sx, sy, sw, 4, COL_TRACK);
    uint32_t prog = ProgressPermil(st);
    int fill_w = (int)(((uint64_t)sw * prog) / 1000ull);
    if (fill_w > 0) Graphics::FillRect(sx, sy, fill_w, 4, COL_FILL);
    // time text "mm:ss / mm:ss"
    char tt[32]; int tp = 0;
    uint32_t pos = PositionMs(st), dur = DurationMs(st);
    tp = Iappend(tt, tp, sizeof(tt), (int)(pos / 60000));
    tp = Sappend(tt, tp, sizeof(tt), ":");
    int sec = (int)((pos / 1000) % 60);
    if (sec < 10) tp = Sappend(tt, tp, sizeof(tt), "0");
    tp = Iappend(tt, tp, sizeof(tt), sec);
    tp = Sappend(tt, tp, sizeof(tt), " / ");
    tp = Iappend(tt, tp, sizeof(tt), (int)(dur / 60000));
    tp = Sappend(tt, tp, sizeof(tt), ":");
    sec = (int)((dur / 1000) % 60);
    if (sec < 10) tp = Sappend(tt, tp, sizeof(tt), "0");
    tp = Iappend(tt, tp, sizeof(tt), sec);
    Graphics::DrawString(x + w - 84, cy + 12, tt, COL_TEXT, COL_PANEL);
}

void DescribeShort(const State& st, char* out, int max) {
    if (!out || max < 4) return;
    int p = 0;
    if (st.kind == SRC_NONE) { Scpy(out, "(empty)", max); return; }
    if (st.kind == SRC_KVID) {
        p = Sappend(out, p, max, "KVID · ");
        p = Iappend(out, p, max, (int)st.kvid.hdr.width);
        p = Sappend(out, p, max, "x");
        p = Iappend(out, p, max, (int)st.kvid.hdr.height);
        p = Sappend(out, p, max, " · ");
        p = Iappend(out, p, max, (int)st.kvid.hdr.frame_count);
        p = Sappend(out, p, max, " frames @ ");
        p = Iappend(out, p, max, (int)(st.kvid.hdr.fps_den
                                       ? st.kvid.hdr.fps_num / st.kvid.hdr.fps_den
                                       : 0));
        p = Sappend(out, p, max, " fps");
        if (st.kvid.hdr.flags & KVID::kFlagHasAudio) {
            p = Sappend(out, p, max, " + ");
            p = Iappend(out, p, max, (int)st.kvid.hdr.audio_sample_rate);
            p = Sappend(out, p, max, " Hz audio");
        }
    } else if (st.kind == SRC_MP4) {
        p = Sappend(out, p, max, "MP4 · ");
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
