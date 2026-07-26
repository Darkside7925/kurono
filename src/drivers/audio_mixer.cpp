//  kurono os - software audio mixer (implementation)
#include "audio_mixer.h"
#include "audio_backend.h"
#include "audio_format.h"
#include "serial.h"
#include "../kernel/types.h"

namespace AudioMixer {

using AudioFormat::MixSample;

struct Stream {
    char     name[24];
    StreamState state;

    AudioFormat::SampleFormat src_fmt;
    uint32_t src_rate;
    int      src_channels;

    int      volume_pct;
    int      pan;
    bool     muted;

    // Smoothed gain values in q15 - updated once per period so per-stream
    // volume / pan changes don't click.  Target values are derived from
    // volume_pct + pan; smoothing is a one-pole filter with ~4 ms tau.
    int32_t  cur_lg_q15;
    int32_t  cur_rg_q15;

    int32_t  fade_gain;        // q15, 32768 == 1.0
    int32_t  fade_total;
    int32_t  fade_remaining;

    MixSample ring[STREAM_RING_FRAMES * INTERNAL_CHANNELS];
    uint32_t  rd;
    uint32_t  wr;
    uint32_t  count;

    StreamStats stats;
};

static Stream g_streams[MAX_STREAMS];
static int    g_active_count = 0;

static int     g_master_volume = 80;
static bool    g_master_mute   = false;
static int32_t g_eq_bass   = 4096;
static int32_t g_eq_mid    = 4096;
static int32_t g_eq_treble = 4096;
static bool    g_limiter_on= true;
static bool    g_initialised = false;

constexpr int32_t kAlphaLow_q15  = 1058;
constexpr int32_t kAlphaHigh_q15 = 13327;

static MixSample g_lp_state_low[INTERNAL_CHANNELS]  = {};
static MixSample g_lp_state_high[INTERNAL_CHANNELS] = {};

static inline MixSample EqProcess(int ch, MixSample s) {
    int32_t diff_low  = s - g_lp_state_low[ch];
    g_lp_state_low[ch] += static_cast<MixSample>((diff_low * kAlphaLow_q15) >> 15);
    int32_t bass = g_lp_state_low[ch];

    int32_t diff_high = s - g_lp_state_high[ch];
    g_lp_state_high[ch] += static_cast<MixSample>((diff_high * kAlphaHigh_q15) >> 15);
    int32_t lp_high = g_lp_state_high[ch];

    int32_t mid    = lp_high - bass;
    int32_t treble = s - lp_high;

    int64_t out = (static_cast<int64_t>(bass)   * g_eq_bass   +
                   static_cast<int64_t>(mid)    * g_eq_mid    +
                   static_cast<int64_t>(treble) * g_eq_treble) >> 12;
    if (out > AudioFormat::kMixMax) out = AudioFormat::kMixMax;
    if (out < AudioFormat::kMixMin) out = AudioFormat::kMixMin;
    return static_cast<MixSample>(out);
}

static int32_t g_limiter_gain_q15 = 32768;

static inline MixSample LimiterProcess(MixSample s) {
    int32_t mag = s < 0 ? -s : s;
    if (mag > AudioFormat::kMixMax) {
        int32_t need = (static_cast<int64_t>(AudioFormat::kMixMax) << 15) / mag;
        if (need < g_limiter_gain_q15) g_limiter_gain_q15 = need;
    } else if (g_limiter_gain_q15 < 32768) {
        g_limiter_gain_q15 += (32768 - g_limiter_gain_q15) >> 9;
        if (g_limiter_gain_q15 > 32768) g_limiter_gain_q15 = 32768;
    }
    return static_cast<MixSample>((static_cast<int64_t>(s) * g_limiter_gain_q15) >> 15);
}

void Init() {
    if (g_initialised) return;
    for (int i = 0; i < MAX_STREAMS; i++) {
        g_streams[i].state = STREAM_FREE;
        g_streams[i].name[0] = '\0';
        g_streams[i].rd = g_streams[i].wr = g_streams[i].count = 0;
        g_streams[i].volume_pct = 100;
        g_streams[i].pan = 0;
        g_streams[i].muted = false;
        g_streams[i].cur_lg_q15 = 32768;
        g_streams[i].cur_rg_q15 = 32768;
        g_streams[i].fade_gain = 32768;
        g_streams[i].fade_total = 0;
        g_streams[i].fade_remaining = 0;
        g_streams[i].stats = {};
    }
    for (int c = 0; c < INTERNAL_CHANNELS; c++) {
        g_lp_state_low[c]  = 0;
        g_lp_state_high[c] = 0;
    }
    g_limiter_gain_q15 = 32768;
    g_active_count = 0;
    g_initialised = true;
    SerialLogger::Log("[AudioMixer] Init: ");
    SerialLogger::LogDec(MAX_STREAMS);
    SerialLogger::Log(" stream slots, period=");
    SerialLogger::LogDec(PERIOD_FRAMES);
    SerialLogger::Log(" frames, internal=");
    SerialLogger::LogDec(INTERNAL_RATE);
    SerialLogger::Log(" Hz stereo\r\n");
}

static int FindFreeSlot() {
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_streams[i].state == STREAM_FREE) return i;
    }
    return -1;
}

static void CopyName(Stream* s, const char* name) {
    if (!name) { s->name[0] = '?'; s->name[1] = '\0'; return; }
    int i = 0;
    while (i < 23 && name[i]) { s->name[i] = name[i]; i++; }
    s->name[i] = '\0';
}

StreamID Open(const char* name, AudioFormat::SampleFormat fmt,
              uint32_t sample_rate, int channels) {
    if (!g_initialised) Init();
    if (channels < 1 || channels > 6 || sample_rate < 4000 || sample_rate > 192000) {
        return INVALID_STREAM;
    }
    int slot = FindFreeSlot();
    if (slot < 0) {
        SerialLogger::Log("[AudioMixer] Open(): no free slots\r\n");
        return INVALID_STREAM;
    }
    Stream* s = &g_streams[slot];
    CopyName(s, name);
    s->state          = STREAM_PLAYING;
    s->src_fmt        = fmt;
    s->src_rate       = sample_rate;
    s->src_channels   = channels;
    s->volume_pct     = 100;
    s->pan            = 0;
    s->muted          = false;
    // Start gain ramps muted so the first period fades up - eliminates
    // the click some apps get when opening a stream while another is
    // playing at full scale.
    // Snap to full gain on Open so transient sounds (taps, beeps) don't
    // start with an audible fade-in.  Volume changes mid-stream still
    // smooth via the per-period glide in Tick().
    s->cur_lg_q15     = 32768;
    s->cur_rg_q15     = 32768;
    s->fade_gain      = 32768;
    s->fade_total     = 0;
    s->fade_remaining = 0;
    s->rd = s->wr = s->count = 0;
    s->stats = {};
    g_active_count++;
    return slot;
}

void Close(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    Stream* s = &g_streams[id];
    if (s->state == STREAM_FREE) return;
    int32_t fade_frames = static_cast<int32_t>((INTERNAL_RATE * 5) / 1000);
    s->state          = STREAM_FADING;
    s->fade_total     = fade_frames;
    s->fade_remaining = fade_frames;
    s->fade_gain      = 32768;
}

void Drain(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    Stream* s = &g_streams[id];
    if (s->state == STREAM_FREE) return;
    s->state = STREAM_DRAINING;
}

uint32_t Write(StreamID id, const void* src, uint32_t frames) {
    if (id < 0 || id >= MAX_STREAMS || !src || frames == 0) return 0;
    Stream* s = &g_streams[id];
    if (s->state == STREAM_FREE || s->state == STREAM_FADING ||
        s->state == STREAM_DRAINING) return 0;

    // Pool of two scratch buffers reused across all Write() calls.
    // Single-threaded by design (kernel main loop owns the mixer); the
    // statics replace per-call heap allocs.
    static MixSample scratch[2048];
    static MixSample scratch_b[2048];
    constexpr uint32_t SCRATCH_FRAMES = 1024;

    const uint8_t* in_p   = static_cast<const uint8_t*>(src);
    const uint32_t in_step= AudioFormat::FrameSize(s->src_fmt, s->src_channels);
    uint32_t in_left      = frames;
    uint32_t produced_total = 0;

    while (in_left > 0) {
        uint32_t in_chunk = SCRATCH_FRAMES;
        if (in_chunk > in_left) in_chunk = in_left;
        if (in_chunk * static_cast<uint32_t>(s->src_channels) > 2048) {
            in_chunk = 2048 / static_cast<uint32_t>(s->src_channels);
        }
        if (in_chunk == 0) break;

        MixSample* cur   = scratch;
        MixSample* other = scratch_b;
        uint32_t   cur_frames = in_chunk;
        int        cur_ch     = s->src_channels;

        AudioFormat::DecodeToCanonical(s->src_fmt, s->src_channels,
                                        in_p, cur, in_chunk);
        if (cur_ch != INTERNAL_CHANNELS) {
            AudioFormat::ConvertChannels(cur_ch, INTERNAL_CHANNELS,
                                          cur, other, cur_frames);
            MixSample* t = cur; cur = other; other = t;
            cur_ch = INTERNAL_CHANNELS;
        }
        (void)cur_ch;
        if (s->src_rate != INTERNAL_RATE) {
            uint32_t cap = 2048 / INTERNAL_CHANNELS;
            cur_frames = AudioFormat::ResampleLinear(
                INTERNAL_CHANNELS, s->src_rate, INTERNAL_RATE,
                cur, cur_frames, other, cap);
            MixSample* t = cur; cur = other; other = t;
        }

        // Bulk-copy as many contiguous frames as the ring allows in one
        // go to avoid per-frame modulo.
        uint32_t writable = STREAM_RING_FRAMES - s->count;
        if (writable == 0) { s->stats.overflows++; break; }
        uint32_t to_take = cur_frames < writable ? cur_frames : writable;
        uint32_t first_chunk = STREAM_RING_FRAMES - s->wr;
        if (first_chunk > to_take) first_chunk = to_take;
        // Copy first contiguous segment
        for (uint32_t f = 0; f < first_chunk; f++) {
            MixSample* dst = &s->ring[(s->wr + f) * INTERNAL_CHANNELS];
            dst[0] = cur[f * INTERNAL_CHANNELS + 0];
            dst[1] = cur[f * INTERNAL_CHANNELS + 1];
        }
        // Wrap-around segment
        uint32_t second_chunk = to_take - first_chunk;
        for (uint32_t f = 0; f < second_chunk; f++) {
            MixSample* dst = &s->ring[f * INTERNAL_CHANNELS];
            dst[0] = cur[(first_chunk + f) * INTERNAL_CHANNELS + 0];
            dst[1] = cur[(first_chunk + f) * INTERNAL_CHANNELS + 1];
        }
        s->wr = (s->wr + to_take) % STREAM_RING_FRAMES;
        s->count += to_take;
        s->stats.frames_written += to_take;
        produced_total += to_take;

        if (to_take < cur_frames) {
            // Ring is now full; drop the rest of this chunk.
            s->stats.overflows++;
            in_left = 0;
            break;
        }

        in_p     += in_chunk * in_step;
        in_left  -= in_chunk;
    }
    return produced_total;
}

void SetVolume(StreamID id, int v) {
    if (id < 0 || id >= MAX_STREAMS) return;
    if (v < 0) v = 0; if (v > 100) v = 100;
    g_streams[id].volume_pct = v;
}

void SetPaused(StreamID id, bool paused) {
    if (id < 0 || id >= MAX_STREAMS) return;
    Stream* s = &g_streams[id];
    // only flip between the two live playback states; fading/draining
    // streams are already on their way out. (satoru)
    if (paused  && s->state == STREAM_PLAYING) s->state = STREAM_PAUSED;
    if (!paused && s->state == STREAM_PAUSED)  s->state = STREAM_PLAYING;
}
int GetVolume(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return 0;
    return g_streams[id].volume_pct;
}
void SetPan(StreamID id, int p) {
    if (id < 0 || id >= MAX_STREAMS) return;
    if (p < -100) p = -100; if (p > 100) p = 100;
    g_streams[id].pan = p;
}
void SetMute(StreamID id, bool m) {
    if (id < 0 || id >= MAX_STREAMS) return;
    g_streams[id].muted = m;
}
bool IsActive(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return false;
    return g_streams[id].state != STREAM_FREE;
}
StreamState GetState(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return STREAM_FREE;
    return g_streams[id].state;
}
StreamStats GetStats(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return {};
    return g_streams[id].stats;
}

void SetMasterVolume(int v) { if (v<0)v=0; if(v>100)v=100; g_master_volume = v; }
int  GetMasterVolume()      { return g_master_volume; }
void SetMasterMute(bool m)  { g_master_mute = m; }
bool GetMasterMute()        { return g_master_mute; }

void SetEqGains(int32_t b, int32_t m, int32_t t) {
    if (b < 256) b = 256; if (b > 16384) b = 16384;
    if (m < 256) m = 256; if (m > 16384) m = 16384;
    if (t < 256) t = 256; if (t > 16384) t = 16384;
    g_eq_bass = b; g_eq_mid = m; g_eq_treble = t;
}
void GetEqGains(int32_t* b, int32_t* m, int32_t* t) {
    if (b) *b = g_eq_bass;
    if (m) *m = g_eq_mid;
    if (t) *t = g_eq_treble;
}
void SetLimiterEnabled(bool on) { g_limiter_on = on; }
bool IsLimiterEnabled()         { return g_limiter_on; }

int  ActiveStreamCount() {
    int n = 0;
    for (int i = 0; i < MAX_STREAMS; i++) if (g_streams[i].state != STREAM_FREE) n++;
    return n;
}
const char* StreamName(StreamID id) {
    if (id < 0 || id >= MAX_STREAMS) return "";
    return g_streams[id].name;
}

uint32_t Tick() {
    if (!g_initialised) return 0;
    AudioBackend* be = AudioServer::ActiveBackend();
    if (!be || !be->IsReady()) return 0;

    // note: be->Tick() (hardware-queue refresh + codec housekeeping) is done
    // ONCE per pump by AudioServer::Tick(), not here per-period - calling it
    // every period meant an I/O-port read storm (each is a VM exit under
    // VMware) that stole cpu from the gui/input tiers and caused microstutter.
    // the gate below uses the cached queue depth (cheap, no port I/O). (satoru)
    // 6 periods (~128ms): the pit-timer backup pump in Scheduler::Tick makes
    // pump gaps structurally impossible past a few ms, so the old 10-period
    // (~213ms) anti-starvation cushion is no longer needed - shorter queue =
    // tighter a/v sync with the same underrun safety. matches AudioServer::Tick. (satoru)
    if (be->QueuedFrames() > PERIOD_FRAMES * 6) return 0;

    static int64_t  accum[PERIOD_FRAMES * INTERNAL_CHANNELS];
    static int16_t  output[PERIOD_FRAMES * INTERNAL_CHANNELS];

    for (uint32_t i = 0; i < PERIOD_FRAMES * INTERNAL_CHANNELS; i++) accum[i] = 0;

    int32_t master_q15 = g_master_mute ? 0
                       : (int32_t)((g_master_volume * 32768L) / 100);

    uint32_t period_peak = 0;
    const bool eq_active = (g_eq_bass != 4096 || g_eq_mid != 4096 || g_eq_treble != 4096);

    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream* s = &g_streams[i];
        if (s->state == STREAM_FREE || s->state == STREAM_PAUSED) continue;

        // Compute target per-stream gains.  Muted streams ramp to 0 but
        // we still consume from the ring (so audio doesn't drift when
        // un-muted, and so DRAINING streams still free themselves).
        int32_t stream_q15 = s->muted ? 0
                                       : (int32_t)((s->volume_pct * 32768L) / 100);
        int32_t pan_l_q15 = 32768, pan_r_q15 = 32768;
        if (s->pan < 0) pan_r_q15 = 32768 + (s->pan * 32768 / 100);
        if (s->pan > 0) pan_l_q15 = 32768 - (s->pan * 32768 / 100);
        int32_t tgt_lg = (int32_t)(((int64_t)stream_q15 * pan_l_q15) >> 15);
        int32_t tgt_rg = (int32_t)(((int64_t)stream_q15 * pan_r_q15) >> 15);

        // Per-period exponential glide (≈ 4 ms tau @ 48 kHz, period 1024).
        // Cuts pop/click on volume changes and on stream open/close.
        s->cur_lg_q15 += (tgt_lg - s->cur_lg_q15) >> 3;
        s->cur_rg_q15 += (tgt_rg - s->cur_rg_q15) >> 3;
        int32_t lg_q15 = s->cur_lg_q15;
        int32_t rg_q15 = s->cur_rg_q15;

        uint32_t avail = s->count < PERIOD_FRAMES ? s->count : PERIOD_FRAMES;
        if (avail < PERIOD_FRAMES && s->state == STREAM_PLAYING) {
            s->stats.underruns++;
        }

        const bool is_fading = (s->state == STREAM_FADING && s->fade_total > 0);
        const int32_t fade_total = s->fade_total;
        const int32_t fade_rem   = s->fade_remaining;

        // Bulk copy with contiguous segments to avoid per-frame modulo
        // in the hot loop.
        uint32_t consumed = 0;
        while (consumed < avail) {
            uint32_t first_run = STREAM_RING_FRAMES - s->rd;
            uint32_t run = avail - consumed;
            if (run > first_run) run = first_run;

            for (uint32_t k = 0; k < run; k++) {
                uint32_t f = consumed + k;
                const MixSample* sp = &s->ring[(s->rd + k) * INTERNAL_CHANNELS];
                MixSample l = sp[0];
                MixSample r = sp[1];
                if (is_fading) {
                    int32_t fade_pos = fade_rem - (int32_t)f;
                    if (fade_pos < 0) fade_pos = 0;
                    int32_t g = (int32_t)(((int64_t)fade_pos << 15) / fade_total);
                    if (g > 32768) g = 32768;
                    l = (MixSample)(((int64_t)l * g) >> 15);
                    r = (MixSample)(((int64_t)r * g) >> 15);
                }
                accum[f * INTERNAL_CHANNELS + 0] += ((int64_t)l * lg_q15) >> 15;
                accum[f * INTERNAL_CHANNELS + 1] += ((int64_t)r * rg_q15) >> 15;
            }
            s->rd = (s->rd + run) % STREAM_RING_FRAMES;
            s->count -= run;
            s->stats.frames_consumed += run;
            consumed += run;
        }

        if (s->state == STREAM_FADING) {
            s->fade_remaining -= (int32_t)avail;
            if (s->fade_remaining <= 0) {
                s->state = STREAM_FREE;
                g_active_count--;
            }
        }

        if (s->state == STREAM_DRAINING && s->count == 0) {
            s->state = STREAM_FREE;
            g_active_count--;
        }
    }

    // Master gain + optional EQ + optional limiter + s16 encode.
    // Hot loop is constant-stride to give the auto-vectoriser a chance.
    if (!eq_active && !g_limiter_on) {
        const uint32_t total = PERIOD_FRAMES * INTERNAL_CHANNELS;
        for (uint32_t i = 0; i < total; i++) {
            int64_t v = (accum[i] * master_q15) >> 15;
            if (v > AudioFormat::kMixMax) v = AudioFormat::kMixMax;
            else if (v < AudioFormat::kMixMin) v = AudioFormat::kMixMin;
            uint32_t mag = (uint32_t)(v < 0 ? -v : v);
            if (mag > period_peak) period_peak = mag;
            output[i] = AudioFormat::ClampToS16((MixSample)v);
        }
    } else {
        for (uint32_t f = 0; f < PERIOD_FRAMES; f++) {
            for (int c = 0; c < INTERNAL_CHANNELS; c++) {
                int64_t v = (accum[f * INTERNAL_CHANNELS + c] * master_q15) >> 15;
                if (v > AudioFormat::kMixMax) v = AudioFormat::kMixMax;
                else if (v < AudioFormat::kMixMin) v = AudioFormat::kMixMin;
                MixSample sm = (MixSample)v;
                if (eq_active)  sm = EqProcess(c, sm);
                if (g_limiter_on) sm = LimiterProcess(sm);
                uint32_t mag = (uint32_t)(sm < 0 ? -sm : sm);
                if (mag > period_peak) period_peak = mag;
                output[f * INTERNAL_CHANNELS + c] = AudioFormat::ClampToS16(sm);
            }
        }
    }

    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_streams[i].state != STREAM_FREE) g_streams[i].stats.peak_level = period_peak;
    }

    return be->Submit(output, PERIOD_FRAMES);
}

void Dump() {
    SerialLogger::Log("[AudioMixer] active=");
    SerialLogger::LogDec(ActiveStreamCount());
    SerialLogger::Log(" master=");
    SerialLogger::LogDec(g_master_volume);
    SerialLogger::Log(g_master_mute ? " (muted)" : "");
    SerialLogger::Log("\r\n");
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream* s = &g_streams[i];
        if (s->state == STREAM_FREE) continue;
        SerialLogger::Log("  [");
        SerialLogger::LogDec(i);
        SerialLogger::Log("] ");
        SerialLogger::Log(s->name);
        SerialLogger::Log("  vol=");
        SerialLogger::LogDec(s->volume_pct);
        SerialLogger::Log(" rate=");
        SerialLogger::LogDec((int)s->src_rate);
        SerialLogger::Log(" ch=");
        SerialLogger::LogDec(s->src_channels);
        SerialLogger::Log(" buf=");
        SerialLogger::LogDec((int)s->count);
        SerialLogger::Log("\r\n");
    }
}

} // namespace AudioMixer
