//  kurono os  -  software audio mixer (implementation)
#include "audio_mixer.h"
#include "audio_backend.h"
#include "audio_format.h"
#include "serial.h"
#include "../kernel/types.h"

namespace AudioMixer {

using AudioFormat::MixSample;

// ---- per-stream state ----
//
// The ring stores canonical interleaved MixSamples already converted to
// the mixer's internal rate + channel count.  Write() does the format
// conversion eagerly so Tick() is just a sum loop.
struct Stream {
    char     name[24];
    StreamState state;

    // Source format hints.  Used by Write() to know how to convert.
    AudioFormat::SampleFormat src_fmt;
    uint32_t src_rate;
    int      src_channels;

    // Per-stream gain.  Applied on the read-side during mixing.
    int      volume_pct;       // 0..100
    int      pan;              // -100..+100
    bool     muted;

    // Fade ramp.  When `fade_remaining > 0`, every period subtracts
    // (32768 / fade_total) from `fade_gain` and decrements
    // fade_remaining by PERIOD_FRAMES; once fade_gain hits 0 the
    // stream auto-closes (state = FREE).
    int32_t  fade_gain;        // q15, 32768 == 1.0
    int32_t  fade_total;       // total frames in the fade
    int32_t  fade_remaining;   // frames left in the fade

    // Ring buffer (interleaved canonical, INTERNAL_CHANNELS wide).
    MixSample ring[STREAM_RING_FRAMES * INTERNAL_CHANNELS];
    uint32_t  rd;              // read frame index (mod STREAM_RING_FRAMES)
    uint32_t  wr;              // write frame index
    uint32_t  count;           // valid frames available

    StreamStats stats;
};

static Stream g_streams[MAX_STREAMS];
static int    g_active_count = 0;

// ---- master state ----
static int     g_master_volume = 80;       // 0..100
static bool    g_master_mute   = false;
static int32_t g_eq_bass   = 4096;          // q12 gain (4096 = 1.0 = 0 dB)
static int32_t g_eq_mid    = 4096;
static int32_t g_eq_treble = 4096;
static bool    g_limiter_on= true;
static bool    g_initialised = false;

// ---- EQ filter state (3-band shelving via 1-pole IIR pairs) ----
//
// The EQ runs at the internal rate, stereo.  Each band is a single-pole
// low-pass / high-pass pair with crossover frequencies chosen for 250 Hz
// and 4 kHz at 48 kHz.  We use q15 fixed-point coefficients.
//
// alpha = 1 - exp(-2pi*fc/fs), pre-computed:
//   fc=250  -> alpha ~= 0.0323 -> q15 = 1058
//   fc=4000 -> alpha ~= 0.4067 -> q15 = 13327
constexpr int32_t kAlphaLow_q15  = 1058;
constexpr int32_t kAlphaHigh_q15 = 13327;

static MixSample g_lp_state_low[INTERNAL_CHANNELS]  = {};   // < 250 Hz residual
static MixSample g_lp_state_high[INTERNAL_CHANNELS] = {};   // < 4 kHz residual

static inline MixSample EqProcess(int ch, MixSample s) {
    // y_lp = y_lp + alpha * (x - y_lp)
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

// ---- soft limiter ----
//
// Rolling 1024-sample peak in q15 attenuation.  When peak exceeds the
// threshold (kMixMax), reduce gain by `peak / kMixMax` until peak
// recovers.  Smoothing factor 0.99 in q15 = 32440.
static int32_t g_limiter_gain_q15 = 32768;
constexpr int32_t kLimiterRecover_q15 = 32440;

static inline MixSample LimiterProcess(MixSample s) {
    int32_t mag = s < 0 ? -s : s;
    if (mag > AudioFormat::kMixMax) {
        int32_t need = (static_cast<int64_t>(AudioFormat::kMixMax) << 15) / mag;
        if (need < g_limiter_gain_q15) g_limiter_gain_q15 = need;
    } else {
        // exponential recovery toward unity gain
        if (g_limiter_gain_q15 < 32768) {
            g_limiter_gain_q15 += (32768 - g_limiter_gain_q15) >> 9;
            if (g_limiter_gain_q15 > 32768) g_limiter_gain_q15 = 32768;
        }
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
        g_streams[i].fade_gain = 32768;
        g_streams[i].fade_total = 0;
        g_streams[i].fade_remaining = 0;
        g_streams[i].stats = {};
    }
    for (int c = 0; c < INTERNAL_CHANNELS; c++) {
        g_lp_state_low[c]  = 0;
        g_lp_state_high[c] = 0;
    }
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
    // schedule a 5 ms fade-out so we don't click.
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
    if (s->state == STREAM_FREE || s->state == STREAM_FADING) return 0;

    // Convert source -> canonical stereo @ INTERNAL_RATE in chunks that
    // fit in our 8 KB scratch (1024 frames * 2 ch * 4 B = 8 KB).
    static MixSample scratch[2048];
    constexpr uint32_t SCRATCH_FRAMES = 1024;

    const uint8_t* in_p   = static_cast<const uint8_t*>(src);
    const uint32_t in_step= AudioFormat::FrameSize(s->src_fmt, s->src_channels);
    uint32_t in_left      = frames;
    uint32_t produced_total = 0;

    while (in_left > 0) {
        // How many input frames we can decode this round?  Bounded by
        // whatever fits in scratch *as input channels*.
        uint32_t in_chunk = SCRATCH_FRAMES;
        if (in_chunk > in_left) in_chunk = in_left;
        if (in_chunk * static_cast<uint32_t>(s->src_channels) > 2048) {
            in_chunk = 2048 / static_cast<uint32_t>(s->src_channels);
        }

        // Decode + channel-convert + resample using the format helper.
        // We do the whole pipeline into scratch, then copy into the ring.
        static MixSample scratch_b[2048];
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
        if (s->src_rate != INTERNAL_RATE) {
            uint32_t cap = 2048 / INTERNAL_CHANNELS;
            cur_frames = AudioFormat::ResampleLinear(
                INTERNAL_CHANNELS, s->src_rate, INTERNAL_RATE,
                cur, cur_frames, other, cap);
            MixSample* t = cur; cur = other; other = t;
        }

        // Copy into the ring (frame at a time, INTERNAL_CHANNELS wide).
        for (uint32_t f = 0; f < cur_frames; f++) {
            if (s->count >= STREAM_RING_FRAMES) {
                s->stats.overflows++;
                // drop the rest of this batch  -  caller can retry next tick.
                in_left = 0;
                goto done_this_chunk;
            }
            for (int c = 0; c < INTERNAL_CHANNELS; c++) {
                s->ring[s->wr * INTERNAL_CHANNELS + c] =
                    cur[f * INTERNAL_CHANNELS + c];
            }
            s->wr = (s->wr + 1) % STREAM_RING_FRAMES;
            s->count++;
            s->stats.frames_written++;
            produced_total++;
        }
done_this_chunk:
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

// ---- the actual mixer pump ----

uint32_t Tick() {
    if (!g_initialised) return 0;
    AudioBackend* be = AudioServer::ActiveBackend();
    if (!be || !be->IsReady()) return 0;

    // Let the backend poll its DMA / refill its hw ring.
    be->Tick();

    // Don't outpace the hardware: if more than ~3 periods are queued,
    // skip this tick to keep latency bounded but the ring fed.
    if (be->QueuedFrames() > PERIOD_FRAMES * 3) return 0;

    // Mixing accumulator (interleaved stereo, q24 with 8 bits headroom).
    static int64_t  accum[PERIOD_FRAMES * INTERNAL_CHANNELS];
    static int16_t  output[PERIOD_FRAMES * INTERNAL_CHANNELS];

    for (uint32_t i = 0; i < PERIOD_FRAMES * INTERNAL_CHANNELS; i++) accum[i] = 0;

    // Pre-compute master gain in q15.
    int32_t master_q15 = g_master_mute ? 0
                       : (int32_t)((g_master_volume * 32768L) / 100);

    uint32_t period_peak = 0;

    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream* s = &g_streams[i];
        if (s->state == STREAM_FREE || s->state == STREAM_PAUSED) continue;
        if (s->muted) {
            // still consume the ring so audio doesn't drift on un-mute
            uint32_t to_drop = s->count < PERIOD_FRAMES ? s->count : PERIOD_FRAMES;
            s->rd     = (s->rd + to_drop) % STREAM_RING_FRAMES;
            s->count -= to_drop;
            s->stats.frames_consumed += to_drop;
            continue;
        }

        // Pre-compute pan gains (left/right q15) and stream gain.
        int32_t stream_q15 = (int32_t)((s->volume_pct * 32768L) / 100);
        int32_t pan_l_q15 = 32768, pan_r_q15 = 32768;
        if (s->pan < 0) pan_r_q15 = 32768 + (s->pan * 32768 / 100); // pan<0 -> reduce R
        if (s->pan > 0) pan_l_q15 = 32768 - (s->pan * 32768 / 100); // pan>0 -> reduce L
        int32_t lg_q15 = (int32_t)(((int64_t)stream_q15 * pan_l_q15) >> 15);
        int32_t rg_q15 = (int32_t)(((int64_t)stream_q15 * pan_r_q15) >> 15);

        // How many frames can we pull?
        uint32_t avail = s->count < PERIOD_FRAMES ? s->count : PERIOD_FRAMES;
        if (avail < PERIOD_FRAMES && s->state == STREAM_PLAYING) {
            s->stats.underruns++;
        }

        for (uint32_t f = 0; f < avail; f++) {
            MixSample l = s->ring[s->rd * INTERNAL_CHANNELS + 0];
            MixSample r = s->ring[s->rd * INTERNAL_CHANNELS + 1];

            // Apply fade ramp if active.
            if (s->state == STREAM_FADING && s->fade_total > 0) {
                int32_t g = (int32_t)((int64_t)s->fade_gain *
                                      (s->fade_total - (s->fade_total - s->fade_remaining + (int32_t)f)) /
                                      s->fade_total);
                if (g < 0) g = 0;
                l = (MixSample)(((int64_t)l * g) >> 15);
                r = (MixSample)(((int64_t)r * g) >> 15);
            }

            accum[f * INTERNAL_CHANNELS + 0] += ((int64_t)l * lg_q15) >> 15;
            accum[f * INTERNAL_CHANNELS + 1] += ((int64_t)r * rg_q15) >> 15;

            s->rd = (s->rd + 1) % STREAM_RING_FRAMES;
            s->count--;
            s->stats.frames_consumed++;
        }

        // Update fade.
        if (s->state == STREAM_FADING) {
            s->fade_remaining -= (int32_t)avail;
            if (s->fade_remaining <= 0) {
                s->state = STREAM_FREE;
                g_active_count--;
            }
        }

        // Auto-free drained streams once their ring is empty.
        if (s->state == STREAM_DRAINING && s->count == 0) {
            s->state = STREAM_FREE;
            g_active_count--;
        }
    }

    // Apply master gain + EQ + limiter, encode to s16.
    for (uint32_t f = 0; f < PERIOD_FRAMES; f++) {
        for (int c = 0; c < INTERNAL_CHANNELS; c++) {
            int64_t v = accum[f * INTERNAL_CHANNELS + c];
            // Master gain
            v = (v * master_q15) >> 15;
            // Clamp to MixSample range before EQ
            if (v > AudioFormat::kMixMax) v = AudioFormat::kMixMax;
            if (v < AudioFormat::kMixMin) v = AudioFormat::kMixMin;
            MixSample sm = (MixSample)v;
            // EQ
            if (g_eq_bass != 4096 || g_eq_mid != 4096 || g_eq_treble != 4096) {
                sm = EqProcess(c, sm);
            }
            // Limiter
            if (g_limiter_on) sm = LimiterProcess(sm);
            // Track period peak
            uint32_t mag = (uint32_t)(sm < 0 ? -sm : sm);
            if (mag > period_peak) period_peak = mag;
            // Encode to s16 for the backend
            output[f * INTERNAL_CHANNELS + c] = AudioFormat::ClampToS16(sm);
        }
    }

    // Update per-stream peak (cheap  -  same period peak for all).
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
