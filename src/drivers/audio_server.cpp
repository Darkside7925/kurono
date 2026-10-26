//  kurono os - unified audio server (implementation)
#include "audio_server.h"
#include "audio_dma.h"
#include "audio_mixer.h"
#include "audio_backend.h"
#include "serial.h"
#include "timer.h"
#include "../kernel/types.h"
#include "../proc/spinlock.h"

namespace AudioServer {

// the audio pump lock: serializes the mixer/backend pump against stream
// writes arriving from linux syscall contexts on other cpus (the pulse
// server's on_data runs in the CLIENT's syscall context - unlocked mixer
// ring updates from two cpus tore rd/wr/count and clicked). all the public
// stream entry points below take it; the pit backup pump try-locks it. (satoru)
static Spinlock g_pump_lock;
// last successful pump timestamp - the pit backup fires when this goes
// stale (audio process starved by userland threads). (satoru)
static volatile uint32_t g_last_pump_ms = 0;

// Backend registry.  Backends register a static instance via
// RegisterBackend() before AudioServer::Init() runs (called from each
// backend's translation unit).  Registration order doesn't matter; Init()
// re-orders by priority.
static constexpr int kMaxBackends = 8;
static AudioBackend* g_backends[kMaxBackends] = {};
static int           g_backend_count          = 0;
static AudioBackend* g_active                  = nullptr;
static uint64_t      g_periods_submitted       = 0;

// Stable priority by name - earlier in the list wins.
static int BackendPriority(const char* name) {
    if (!name) return 99;
    if (name[0] == 'h' && name[1] == 'd' && name[2] == 'a') return 0;
    if (name[0] == 'a' && name[1] == 'c' && name[2] == '9') return 1;
    if (name[0] == 's' && name[1] == 'b' && name[2] == '1') return 2;
    if (name[0] == 'p' && name[1] == 'c' && name[2] == 's') return 3;
    if (name[0] == 'n' && name[1] == 'u' && name[2] == 'l') return 9;
    return 5;
}

void RegisterBackend(AudioBackend* be) {
    if (!be || g_backend_count >= kMaxBackends) return;
    g_backends[g_backend_count++] = be;
    SerialLogger::Log("[AudioServer] Registered backend: ");
    SerialLogger::Log(be->Name());
    SerialLogger::Log("\r\n");
}

AudioBackend* ActiveBackend() { return g_active; }
const char* ActiveBackendName() { return g_active ? g_active->Name() : "(none)"; }

void Init() {
    // Allocator and mixer first.  Both are idempotent.
    AudioDMA::Init();
    AudioMixer::Init();

    // Sort by priority (insertion sort - n <= 8).
    for (int i = 1; i < g_backend_count; i++) {
        for (int j = i; j > 0; j--) {
            int pa = BackendPriority(g_backends[j-1]->Name());
            int pb = BackendPriority(g_backends[j]->Name());
            if (pb < pa) {
                AudioBackend* t = g_backends[j-1];
                g_backends[j-1] = g_backends[j];
                g_backends[j]   = t;
            } else break;
        }
    }

    // Try each in priority order.
    for (int i = 0; i < g_backend_count; i++) {
        AudioBackend* be = g_backends[i];
        SerialLogger::Log("[AudioServer] Probing backend: ");
        SerialLogger::Log(be->Name());
        SerialLogger::Log("...\r\n");
        if (be->Init() && be->IsReady()) {
            g_active = be;
            SerialLogger::Log("[AudioServer] Active backend: ");
            SerialLogger::Log(be->Name());
            SerialLogger::Log(" @ ");
            SerialLogger::LogDec((int)be->SampleRate());
            SerialLogger::Log(" Hz\r\n");
            return;
        }
    }
    SerialLogger::Log("[AudioServer] No usable audio backend!\r\n");
}

void Tick() {
    if (!g_active) return;
    // nothing playing -> touch NO audio hardware. polling the backend's status
    // / DMA-position registers on every 10ms tick while silent was pure VM-exit
    // overhead on VMware (each in/out traps). the DMA engine idles fine and the
    // next OpenStream re-kicks it. (satoru)
    if (AudioMixer::ActiveStreamCount() == 0) return;
    // refresh the backend's hardware queue depth + run codec housekeeping
    // ONCE here (a single I/O-port read), then mix against a LOCAL estimate so
    // the loop does zero further port I/O. reading the hardware queue on every
    // loop iteration / gate check was a VM-exit storm under VMware that stole
    // cpu from the gui/input tiers and caused microstutter. (satoru)
    g_active->Tick();
    uint32_t q = g_active->QueuedFrames();
    // keep ~6 periods (~128ms) buffered. the pit backup pump (TickFromTimer)
    // makes long pump gaps structurally impossible, so the old 10-period
    // anti-starvation cushion shrinks to a tighter a/v-sync depth with the
    // same underrun safety. matches the gate in AudioMixer::Tick(). (satoru)
    for (int i = 0; i < 20; i++) {
        if (q >= AudioMixer::PERIOD_FRAMES * 6) break;
        uint32_t produced = AudioMixer::Tick();
        if (produced == 0) break;
        g_periods_submitted++;
        q += AudioMixer::PERIOD_FRAMES;   // Submit() queued ~one more period
    }
    g_last_pump_ms = Timer::GetRealMs();
}

void LockedTick() {
    SpinLockCpuGuard guard(g_pump_lock);
    Tick();
}

// expose the pump lock so pulse's pacing pass (bsp audio process) can make
// its per-stream check + stats read + request accounting atomic vs stream
// create/close arriving in client syscall context on other cpus. (satoru)
Spinlock& PumpLock() { return g_pump_lock; }

void TickFromTimer() {
    // intentionally a no-op: running the mixer pump from the irq0 tick with
    // interrupts disabled destabilized firefox startup timing. kept as a
    // symbol so the scheduler call site + header stay stable; the audio
    // process pump is the sole pump now. (satoru)
}

void PauseStream(AudioMixer::StreamID id, bool paused) {
    SpinLockCpuGuard guard(g_pump_lock);
    AudioMixer::SetPaused(id, paused);
}

// ---- one-shot tone synthesis ----
//
// Generates a 16-bit signed mono sine using a precomputed 256-entry
// quarter-table; the mixer will resample + upmix as needed.

static const int16_t kSineTable[256] = {
       0,    201,    402,    603,    803,   1004,   1205,   1405,
    1605,   1805,   2005,   2205,   2404,   2602,   2801,   2998,
    3196,   3393,   3589,   3785,   3980,   4175,   4369,   4562,
    4755,   4946,   5137,   5327,   5516,   5704,   5891,   6077,
    6261,   6444,   6626,   6807,   6987,   7165,   7341,   7517,
    7691,   7863,   8034,   8204,   8371,   8537,   8702,   8865,
    9026,   9185,   9343,   9498,   9651,   9803,   9953,  10100,
   10245,  10388,  10530,  10668,  10805,  10940,  11072,  11202,
   11329,  11455,  11577,  11697,  11815,  11930,  12043,  12153,
   12260,  12365,  12467,  12567,  12663,  12757,  12848,  12937,
   13022,  13105,  13185,  13262,  13336,  13407,  13475,  13540,
   13602,  13662,  13718,  13771,  13822,  13869,  13913,  13955,
   13993,  14028,  14060,  14089,  14115,  14138,  14157,  14174,
   14188,  14198,  14206,  14210,  14211,  14210,  14206,  14198,
   14188,  14174,  14157,  14138,  14115,  14089,  14060,  14028,
   13993,  13955,  13913,  13869,  13822,  13771,  13718,  13662,
   13602,  13540,  13475,  13407,  13336,  13262,  13185,  13105,
   13022,  12937,  12848,  12757,  12663,  12567,  12467,  12365,
   12260,  12153,  12043,  11930,  11815,  11697,  11577,  11455,
   11329,  11202,  11072,  10940,  10805,  10668,  10530,  10388,
   10245,  10100,   9953,   9803,   9651,   9498,   9343,   9185,
    9026,   8865,   8702,   8537,   8371,   8204,   8034,   7863,
    7691,   7517,   7341,   7165,   6987,   6807,   6626,   6444,
    6261,   6077,   5891,   5704,   5516,   5327,   5137,   4946,
    4755,   4562,   4369,   4175,   3980,   3785,   3589,   3393,
    3196,   2998,   2801,   2602,   2404,   2205,   2005,   1805,
    1605,   1405,   1205,   1004,    803,    603,    402,    201,
       0,   -201,   -402,   -603,   -803,  -1004,  -1205,  -1405,
   -1605,  -1805,  -2005,  -2205,  -2404,  -2602,  -2801,  -2998,
   -3196,  -3393,  -3589,  -3785,  -3980,  -4175,  -4369,  -4562,
   -4755,  -4946,  -5137,  -5327,  -5516,  -5704,  -5891,  -6077,
   -6261,  -6444,  -6626,  -6807,  -6987,  -7165,  -7341,  -7517,
};
// Note: we intentionally store only one half-period;  PlayTone uses
// modulo + sign flip to reach the lower half.  Above table is one full
// period of sin(2pi*i/256) * 14210 (approx).

void PlayTone(int freq_hz, int duration_ms, int vol) {
    if (freq_hz < 30 || freq_hz > 20000) return;
    if (duration_ms <= 0 || duration_ms > 30000) return;

    constexpr uint32_t RATE = AudioMixer::INTERNAL_RATE;
    AudioMixer::StreamID id =
        AudioServer::OpenStream("beep", AudioFormat::FMT_S16_LE, RATE, 1);
    if (id == AudioMixer::INVALID_STREAM) return;
    AudioMixer::SetVolume(id, vol);

    // Phase increment in 16.16 fixed point.
    uint32_t phase_inc = (static_cast<uint64_t>(freq_hz) * 256ull * 65536ull) / RATE;
    uint32_t phase = 0;
    uint32_t total_frames = static_cast<uint32_t>((RATE * duration_ms) / 1000);

    // Write in chunks of 1024 frames so the mixer can pull periods as
    // we go (avoid blocking the kernel for the whole tone duration).
    static int16_t chunk[1024];
    uint32_t remaining = total_frames;

    // Linear attack/release envelope (5 ms each) to avoid clicks.
    uint32_t env_len = (RATE * 5) / 1000;
    uint32_t emitted = 0;

    while (remaining > 0) {
        uint32_t this_chunk = remaining < 1024 ? remaining : 1024;
        for (uint32_t i = 0; i < this_chunk; i++) {
            int16_t s = kSineTable[(phase >> 16) & 0xFF];
            // envelope
            uint32_t pos = emitted + i;
            int32_t  env = 32767;
            if (pos < env_len) env = (int32_t)((pos * 32767ull) / env_len);
            else if (pos > total_frames - env_len)
                env = (int32_t)(((total_frames - pos) * 32767ull) / env_len);
            if (env < 0) env = 0;
            chunk[i] = (int16_t)((s * env) >> 15);
            phase += phase_inc;
        }
        WriteStream(id, chunk, this_chunk);
        emitted   += this_chunk;
        remaining -= this_chunk;
    }
    DrainStream(id);
}

void Beep() { PlayTone(880, 60, 60); }

} // namespace AudioServer

// ---- C++-name proxy for the legacy Audio::Beep() forwarder ----
//
// Defined as a free function so audio.cpp doesn't need to pull in the
// full AudioServer header (it lives in the legacy SB16 implementation
// translation unit and is compiled with -mno-sse just like everything
// else).
extern "C++" void __audio_server_play_tone_proxy(int hz, int ms, int vol) {
    AudioServer::PlayTone(hz, ms, vol);
}

namespace AudioServer {

bool PlayPCM(const void* pcm, uint32_t bytes,
             AudioFormat::SampleFormat fmt, uint32_t rate, int channels) {
    if (!pcm || bytes == 0) return false;
    AudioMixer::StreamID id = OpenStream("pcm", fmt, rate, channels);
    if (id == AudioMixer::INVALID_STREAM) return false;
    uint32_t frame_bytes = AudioFormat::FrameSize(fmt, channels);
    if (frame_bytes == 0) { CloseStream(id); return false; }
    uint32_t frames = bytes / frame_bytes;
    WriteStream(id, pcm, frames);
    DrainStream(id);
    return true;
}

bool PlayBuffer(const int16_t* pcm, size_t samples,
                uint32_t sampleRate, uint8_t channels) {
    if (!pcm || samples == 0) return false;
    // `samples` counts every interleaved int16 across all channels, so the
    // byte count is just samples * 2.  forward to PlayPCM as s16 le. (satoru)
    uint32_t bytes = (uint32_t)(samples * sizeof(int16_t));
    return PlayPCM(pcm, bytes, AudioFormat::FMT_S16_LE, sampleRate, (int)channels);
}

// every stream entry point serializes against the pump: pulse writes arrive
// in the client's syscall context on ANY cpu, and unlocked ring updates
// racing the mixer tick tore rd/wr/count (audible clicks + lost audio). (satoru)
AudioMixer::StreamID OpenStream(const char* name,
                                AudioFormat::SampleFormat fmt,
                                uint32_t rate, int channels) {
    SpinLockCpuGuard guard(g_pump_lock);
    return AudioMixer::Open(name, fmt, rate, channels);
}
uint32_t WriteStream(AudioMixer::StreamID id, const void* src, uint32_t frames) {
    SpinLockCpuGuard guard(g_pump_lock);
    return AudioMixer::Write(id, src, frames);
}
void CloseStream(AudioMixer::StreamID id) {
    SpinLockCpuGuard guard(g_pump_lock);
    AudioMixer::Close(id);
}
void DrainStream(AudioMixer::StreamID id) {
    SpinLockCpuGuard guard(g_pump_lock);
    AudioMixer::Drain(id);
}

ServerStatus GetStatus() {
    ServerStatus s{};
    s.backend_name           = ActiveBackendName();
    s.backend_ready          = g_active && g_active->IsReady();
    s.backend_rate           = g_active ? g_active->SampleRate() : 0;
    s.active_streams         = AudioMixer::ActiveStreamCount();
    s.master_volume          = AudioMixer::GetMasterVolume();
    s.master_muted           = AudioMixer::GetMasterMute();
    s.total_periods_submitted= g_periods_submitted;
    return s;
}

void Dump() {
    SerialLogger::Log("[AudioServer] backend=");
    SerialLogger::Log(ActiveBackendName());
    SerialLogger::Log(" registered=");
    SerialLogger::LogDec(g_backend_count);
    SerialLogger::Log(" periods_submitted=");
    SerialLogger::LogDec((int)g_periods_submitted);
    SerialLogger::Log("\r\n");
    AudioMixer::Dump();
}

} // namespace AudioServer
