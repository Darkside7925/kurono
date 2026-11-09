// media_audio.cpp: MpAudio on kui::Audio (see media_audio.h) (satoru)
#include "media_audio.h"
#include "../kui/kui.h"
#ifndef KUI_BACKEND_WAYLAND
#include "../drivers/audio_server.h"
#endif

namespace MpAudio {

static int      g_id     = -1;        // kui::Audio stream, -1 = none (satoru)
static int16_t* g_buf    = nullptr;   // the whole track as interleaved s16 (satoru)
static int      g_frames = 0;         // frames in g_buf (satoru)
static int      g_pos    = 0;         // frames already handed to the stream (satoru)
static int      g_ch     = 2;
static bool     g_paused = false;
static int      g_vol    = 80;
static bool     g_no_out = false;     // the last open found no output device (satoru)

bool PlayPCM(const uint8_t* data, int len, int rate, int bits, int channels) {
    Stop();
    if (!data || len <= 0 || rate <= 0) return false;
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;
    int bytes = bits == 8 ? 1 : bits == 32 ? 4 : 2;
    int frames = len / (bytes * channels);
    if (frames <= 0) return false;

    int id = kui::Audio::Open(rate, channels);
    if (id <= 0) { g_no_out = true; return false; }
    g_no_out = false;
    int16_t* buf = (int16_t*)kui::Alloc((size_t)frames * channels * 2);
    if (!buf) { kui::Audio::Close(id); return false; }
    int n = frames * channels;
    for (int i = 0; i < n; i++) {
        if (bytes == 1) buf[i] = (int16_t)(((int)data[i] - 128) << 8);          // unsigned 8 bit
        else if (bytes == 4) buf[i] = (int16_t)(((const int32_t*)data)[i] >> 16);
        else buf[i] = ((const int16_t*)data)[i];
    }
    g_id = id; g_buf = buf; g_frames = frames; g_pos = 0; g_ch = channels; g_paused = false;
    kui::Audio::SetVolume(id, g_vol);
    Pump();
    return true;
}

void Pump() {
    if (g_id <= 0 || !g_buf) return;
    int guard = 8;
    while (g_pos < g_frames && guard-- > 0) {
        int want = g_frames - g_pos;
        if (want > 4096) want = 4096;
        int took = kui::Audio::Write(g_id, g_buf + (size_t)g_pos * g_ch, want);
        if (took <= 0) break;
        g_pos += took;
    }
}

void Stop() {
    if (g_id > 0) kui::Audio::Close(g_id);
    g_id = -1;
    if (g_buf) kui::Free(g_buf);
    g_buf = nullptr; g_frames = 0; g_pos = 0; g_paused = false;
}

void Pause(bool on) {
    if (g_id <= 0) return;
    kui::Audio::Pause(g_id, on);
    g_paused = on;
}

void SetMasterVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_vol = pct;
    if (g_id > 0) kui::Audio::SetVolume(g_id, pct);
}

const char* BackendName() {
#ifndef KUI_BACKEND_WAYLAND
    // reflects the unified AudioServer's chosen backend (hda/ac97/sb16/pcspk) (satoru)
    const char* backend = AudioServer::ActiveBackendName();
    if (!backend || backend[0] == 0 || backend[0] == '(') backend = "No Audio";
    return backend;
#else
    return g_no_out ? "No Audio" : "Audio";
#endif
}

const char* StatusText() {
    if (g_id <= 0) return "Stopped";
    if (g_paused) return "Paused";
    static char s[40];
    const char* b = BackendName();
    int n = 0;
    const char* p = "Playing (";
    while (*p) s[n++] = *p++;
    for (int i = 0; b[i] && n < 30; i++) s[n++] = (b[i] >= 'a' && b[i] <= 'z') ? (char)(b[i] - 32) : b[i];
    s[n++] = ')'; s[n] = 0;
    return s;
}

int SampleRateHz() { return 0; }   // the old line only ever showed the sb16 rate (satoru)

bool Drained() {
    return g_id > 0 && g_buf && g_pos >= g_frames && kui::Audio::Queued(g_id) <= 0;
}

}  // namespace MpAudio

// end (satoru)
