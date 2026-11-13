//  kurono os - ring-3 audio daemon broker (see audio_r3.h)
#include "audio_r3.h"
#include "audio_server.h"
#include "audio_mixer.h"
#include "audio_backend.h"
#include "serial.h"
#include "timer.h"
#include "../linux/uaccess.h"

namespace R3Audio {

static bool     g_enabled   = false;
static uint64_t g_owner     = 0;
static int      g_stream    = AudioMixer::INVALID_STREAM;
static uint32_t g_last_ms   = 0;      // last broker call from the daemon
static constexpr uint32_t kStaleMs = 3000;

void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }
void SetOwner(uint64_t as) { g_owner = as; }

static bool stream_live() {
    if (g_stream == AudioMixer::INVALID_STREAM) return false;
    AudioMixer::StreamState st = AudioMixer::GetState(g_stream);
    return st == AudioMixer::STREAM_PLAYING || st == AudioMixer::STREAM_PAUSED;
}

int64_t Call(uint64_t as, uint32_t op, uint64_t a0, uint64_t a1) {
    if (!g_enabled || as == 0 || as != g_owner) return E_PERM;
    g_last_ms = Timer::GetRealMs();
    switch (op) {
        case OP_PING: {
            AudioBackend* be = AudioServer::ActiveBackend();
            return (be && be->IsReady()) ? 1 : 0;
        }
        case OP_OPEN: {
            AudioBackend* be = AudioServer::ActiveBackend();
            if (!be || !be->IsReady()) return E_NODEV;
            if (!stream_live()) {
                g_stream = AudioServer::OpenStream("audiod", AudioFormat::FMT_S16_LE,
                                                   AudioMixer::INTERNAL_RATE, 2);
                if (g_stream == AudioMixer::INVALID_STREAM) return E_BUSY;
            }
            return (int64_t)AudioMixer::INTERNAL_RATE;
        }
        case OP_WRITE: {
            if (!stream_live()) return E_INVAL;
            uint32_t frames = (uint32_t)a1;
            if (frames == 0) return 0;
            if (frames > 4096) frames = 4096;
            static int16_t tmp[4096 * 2];   // single owner pid, single caller at a time
            if (UAccess::CopyFromUser(tmp, a0, (uint64_t)frames * 4)) return E_FAULT;
            return (int64_t)AudioServer::WriteStream(g_stream, tmp, frames);
        }
        case OP_QUEUED: {
            if (!stream_live()) return 0;
            AudioMixer::StreamStats s = AudioMixer::GetStats(g_stream);
            return (int64_t)(s.frames_written - s.frames_consumed);
        }
        case OP_CONSUMED: {
            if (g_stream == AudioMixer::INVALID_STREAM) return 0;
            return (int64_t)AudioMixer::GetStats(g_stream).frames_consumed;
        }
        case OP_LATENCY: {
            AudioBackend* be = AudioServer::ActiveBackend();
            return be ? (int64_t)be->QueuedFrames() : 0;
        }
        case OP_CLOSE: {
            if (g_stream != AudioMixer::INVALID_STREAM) {
                AudioServer::DrainStream(g_stream);   // plays out, then the mixer frees it
                g_stream = AudioMixer::INVALID_STREAM;
            }
            return 0;
        }
    }
    return E_INVAL;
}

bool DaemonAlive() {
    return g_enabled && g_owner != 0 && g_last_ms != 0 &&
           (uint32_t)(Timer::GetRealMs() - g_last_ms) < kStaleMs;
}

void Tick() {
    if (!g_enabled || g_stream == AudioMixer::INVALID_STREAM) return;
    if ((uint32_t)(Timer::GetRealMs() - g_last_ms) > kStaleMs) {
        SerialLogger::Log("[r3audio] daemon went quiet, closing its stream\r\n");
        AudioServer::DrainStream(g_stream);
        g_stream = AudioMixer::INVALID_STREAM;
    }
}

}  // namespace R3Audio
