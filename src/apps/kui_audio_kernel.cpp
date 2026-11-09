// kui_audio_kernel.cpp: kui::Audio for the in-kernel backend, on the audio mixer streams (satoru)
// lives in apps/ until the kui agent folds it into src/kui/kui_kernel.cpp. a kui stream id is the mixer
// stream id plus one so that 0 never names a stream. (satoru)
#include "../kui/kui.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"

namespace kui {
namespace Audio {

static const int MAXS = AudioMixer::MAX_STREAMS;
static struct { int id; int rate; } g_rates[MAXS];   // source rate per open stream, for Queued (satoru)

static void remember(int id, int rate) {
    for (int i = 0; i < MAXS; i++) if (g_rates[i].id == 0 || g_rates[i].id == id) { g_rates[i].id = id; g_rates[i].rate = rate; return; }
}
static int rate_of(int id) {
    for (int i = 0; i < MAXS; i++) if (g_rates[i].id == id) return g_rates[i].rate;
    return 48000;
}

int Open(int rate, int channels) {
    AudioMixer::StreamID s = AudioServer::OpenStream("kui", AudioFormat::FMT_S16_LE, (uint32_t)rate, channels);
    if (s == AudioMixer::INVALID_STREAM || s < 0) return -1;
    remember(s + 1, rate);
    return s + 1;
}

int Write(int id, const int16_t* frames, int nframes) {
    if (id <= 0 || !frames || nframes <= 0) return 0;
    return (int)AudioServer::WriteStream(id - 1, frames, (uint32_t)nframes);
}

int Queued(int id) {
    if (id <= 0) return 0;
    uint64_t rate = (uint64_t)rate_of(id);
    AudioMixer::StreamStats ss = AudioMixer::GetStats(id - 1);
    uint64_t written = (ss.frames_written * rate) / AudioMixer::INTERNAL_RATE;   // in source frames (satoru)
    uint64_t played  = (ss.frames_consumed * rate) / AudioMixer::INTERNAL_RATE;  // in source frames (satoru)
    uint64_t queued  = written > played ? written - played : 0;
    // the backend still holds the period being heard, so count it too; the estimate
    // then matches the old consumed-minus-backend-queue heard position. (satoru)
    AudioBackend* be = AudioServer::ActiveBackend();
    if (be && be->SampleRate() > 0)
        queued += ((uint64_t)be->QueuedFrames() * rate) / be->SampleRate();
    return queued > 0x7fffffffull ? 0x7fffffff : (int)queued;
}

void Pause(int id, bool on) { if (id > 0) AudioMixer::SetPaused(id - 1, on); }
void SetVolume(int id, int pct) { if (id > 0) AudioMixer::SetVolume(id - 1, pct); }

void Close(int id) {
    if (id <= 0) return;
    AudioServer::CloseStream(id - 1);
    for (int i = 0; i < MAXS; i++) if (g_rates[i].id == id) g_rates[i].id = 0;
}

const char* BackendName() {
    const char* b = AudioServer::ActiveBackendName();
    if (!b || !b[0] || b[0] == '(') return "none";   // no hda/ac97/sb16 backend (satoru)
    return b;
}

}  // namespace Audio
}  // namespace kui

// end (satoru)
