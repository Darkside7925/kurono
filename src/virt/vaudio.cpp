//  kurono os - guest audio bridge (see vaudio.h)
#include "vaudio.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_format.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/types.h"

namespace {
    // about four seconds of 48 khz stereo s16: enough headroom for a host
    // pump gap without letting a stuck guest grow the buffer forever. (satoru)
    const uint32_t RING_BYTES = 768 * 1024;
    const uint32_t HDR_BYTES  = 16;
    const char     HDR_MAGIC[4] = { 'K', 'W', 'A', 'U' };

    uint8_t  g_buf[RING_BYTES];
    uint32_t g_rd = 0, g_wr = 0, g_count = 0;

    bool     g_have_fmt = false;
    AudioFormat::SampleFormat g_fmt = AudioFormat::FMT_S16_LE;
    uint32_t g_rate = 48000;
    int      g_channels = 2;
    uint32_t g_frame_bytes = 4;

    AudioMixer::StreamID g_stream = AudioMixer::INVALID_STREAM;
    uint64_t g_last_data_ms = 0;
    uint64_t g_bytes_in = 0, g_bytes_played = 0;
    uint32_t g_drops = 0;
    bool     g_bad_hdr_logged = false;
    bool     g_open_fail_logged = false;

    void Advance(uint32_t n) {
        if (n > g_count) n = g_count;
        g_rd = (g_rd + n) % RING_BYTES;
        g_count -= n;
    }

    void Peek(uint8_t* dst, uint32_t n) {
        uint32_t first = RING_BYTES - g_rd;
        if (first > n) first = n;
        for (uint32_t i = 0; i < first; i++) dst[i] = g_buf[g_rd + i];
        for (uint32_t i = first; i < n; i++) dst[i] = g_buf[i - first];
    }

    void Append(const uint8_t* p, uint32_t n) {
        if (n == 0) return;
        g_bytes_in += n;
        if (n > RING_BYTES) { p += n - RING_BYTES; n = RING_BYTES; }
        if (g_count + n > RING_BYTES) {
            uint32_t drop = g_count + n - RING_BYTES;
            if (g_have_fmt) {
                drop -= drop % g_frame_bytes;      // keep frame alignment
                if (drop > g_count) drop = g_count;
                Advance(drop);
                g_drops += drop;
            } else {
                // no format yet: nothing in the ring is meaningful. (satoru)
                g_rd = g_wr = g_count = 0;
            }
        }
        uint32_t first = RING_BYTES - g_wr;
        if (first > n) first = n;
        for (uint32_t i = 0; i < first; i++) g_buf[g_wr + i] = p[i];
        for (uint32_t i = first; i < n; i++) g_buf[i - first] = p[i];
        g_wr = (g_wr + n) % RING_BYTES;
        g_count += n;
    }

    const char* FmtName(AudioFormat::SampleFormat f) {
        switch (f) {
            case AudioFormat::FMT_U8:     return "u8";
            case AudioFormat::FMT_S16_LE: return "s16le";
            case AudioFormat::FMT_S24_LE: return "s24le";
            case AudioFormat::FMT_S32_LE: return "s32le";
            case AudioFormat::FMT_F32_LE: return "f32le";
            default:                      return "?";
        }
    }

    void CloseStream() {
        if (g_stream != AudioMixer::INVALID_STREAM) {
            AudioServer::CloseStream(g_stream);
            g_stream = AudioMixer::INVALID_STREAM;
        }
    }

    // one header, sent by kwl-audio when it opens the channel: magic, version,
    // sample format (kurono's AudioFormat code), channels, rate. (satoru)
    bool TakeHeader() {
        uint8_t h[HDR_BYTES];
        Peek(h, HDR_BYTES);
        if (h[0] != HDR_MAGIC[0] || h[1] != HDR_MAGIC[1] ||
            h[2] != HDR_MAGIC[2] || h[3] != HDR_MAGIC[3]) {
            if (!g_bad_hdr_logged) {
                g_bad_hdr_logged = true;
                SerialLogger::Log("[vaudio] no format header from the guest, dropping bytes\r\n");
            }
            g_rd = g_wr = g_count = 0;
            return false;
        }
        uint8_t  fmt  = h[5];
        uint32_t ch   = (uint32_t)h[6] | ((uint32_t)h[7] << 8);
        uint32_t rate = (uint32_t)h[8] | ((uint32_t)h[9] << 8) |
                        ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
        if (h[4] != 1 || fmt > AudioFormat::FMT_F32_LE ||
            ch < 1 || ch > 6 || rate < 4000 || rate > 192000) {
            if (!g_bad_hdr_logged) {
                g_bad_hdr_logged = true;
                SerialLogger::Log("[vaudio] bad format header from the guest, dropping bytes\r\n");
            }
            g_rd = g_wr = g_count = 0;
            return false;
        }
        g_fmt         = (AudioFormat::SampleFormat)fmt;
        g_channels    = (int)ch;
        g_rate        = rate;
        g_frame_bytes = AudioFormat::FrameSize(g_fmt, g_channels);
        g_have_fmt    = true;
        Advance(HDR_BYTES);
        SerialLogger::Log("[vaudio] guest pcm: ");
        SerialLogger::Log(FmtName(g_fmt));
        SerialLogger::Log(" ");
        SerialLogger::LogDec((int)g_rate);
        SerialLogger::Log(" Hz ");
        SerialLogger::LogDec(g_channels);
        SerialLogger::Log(" ch\r\n");
        return true;
    }
}

void VAudio::OnGuestBytes(int idx, const uint8_t* data, int n) {
    (void)idx;
    if (!data || n <= 0) return;
    Append(data, (uint32_t)n);
    g_last_data_ms = Timer::GetRealMs64();
}

bool VAudio::CanAccept(int idx) {
    (void)idx;
    // let the header through, then keep about 0.7 s queued here. the guest
    // tty blocks when this says no, which is the only backpressure the
    // virtio console gives us. (satoru)
    if (!g_have_fmt) return true;
    return g_count < 128 * 1024;
}

void VAudio::Pump() {
    if (!g_have_fmt) {
        if (g_count < HDR_BYTES) return;
        if (!TakeHeader()) return;
    }
    if (g_count < g_frame_bytes) {
        // silence: release the mixer slot once the guest has been quiet for a
        // while. the format stays, so the next burst reopens without a header. (satoru)
        if (g_stream != AudioMixer::INVALID_STREAM &&
            Timer::GetRealMs64() - g_last_data_ms > 5000) {
            CloseStream();
        }
        return;
    }
    if (g_stream == AudioMixer::INVALID_STREAM) {
        g_stream = AudioServer::OpenStream("alpine", g_fmt, g_rate, g_channels);
        if (g_stream == AudioMixer::INVALID_STREAM) {
            if (!g_open_fail_logged) {
                g_open_fail_logged = true;
                SerialLogger::Log("[vaudio] no free mixer stream for the guest\r\n");
            }
            return;
        }
        g_open_fail_logged = false;
    }
    static uint8_t tmp[16384];
    for (;;) {
        uint32_t frames = g_count / g_frame_bytes;
        if (frames == 0) break;
        uint32_t max_frames = (uint32_t)sizeof(tmp) / g_frame_bytes;
        if (frames > max_frames) frames = max_frames;
        Peek(tmp, frames * g_frame_bytes);
        uint32_t got = AudioServer::WriteStream(g_stream, tmp, frames);
        if (got == 0) break;
        Advance(got * g_frame_bytes);
        g_bytes_played += (uint64_t)got * g_frame_bytes;
        g_last_data_ms = Timer::GetRealMs64();
    }
}

void VAudio::Reset() {
    CloseStream();
    g_rd = g_wr = g_count = 0;
    g_have_fmt = false;
    g_bad_hdr_logged = false;
    g_open_fail_logged = false;
    g_bytes_in = g_bytes_played = 0;
    g_drops = 0;
}

static int va_app(char* o, int p, int mx, const char* s) { while (*s && p < mx - 1) o[p++] = *s++; o[p] = 0; return p; }
static int va_appu(char* o, int p, int mx, uint64_t v) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) { if (p < mx - 1) o[p++] = t[--k]; else --k; } o[p] = 0; return p; }

int VAudio::Status(char* out, int mx) {
    int p = 0;
    p = va_app(out, p, mx, "  audio: ");
    if (!g_have_fmt) {
        p = va_app(out, p, mx, "no guest stream");
    } else {
        p = va_app(out, p, mx, g_stream != AudioMixer::INVALID_STREAM ? "playing " : "idle ");
        p = va_app(out, p, mx, FmtName(g_fmt));
        p = va_app(out, p, mx, " "); p = va_appu(out, p, mx, g_rate);
        p = va_app(out, p, mx, " Hz "); p = va_appu(out, p, mx, (uint64_t)g_channels);
        p = va_app(out, p, mx, " ch, guest "); p = va_appu(out, p, mx, g_bytes_in >> 10);
        p = va_app(out, p, mx, " KB, played "); p = va_appu(out, p, mx, g_bytes_played >> 10);
        p = va_app(out, p, mx, " KB, dropped "); p = va_appu(out, p, mx, g_drops >> 10);
        p = va_app(out, p, mx, " KB");
    }
    p = va_app(out, p, mx, "\n");
    return p;
}

// end (satoru)
