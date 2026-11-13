// freeze: see freeze.h. (satoru)
#include "freeze.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/serial.h"

namespace {

uint32_t g_pid[LINUX_MAX_PROCS];   // pid we stopped in that slot, 0 = none (satoru)
bool     g_frozen = false;
bool     g_was_muted = false;

bool is_driver_container(const LinuxProcess* q) {
    const char* n = q->name;
    // "/usr/bin/kld-iwlwifi", "kld-e1000e", ...: the basename starts with kld- (satoru)
    const char* base = n;
    for (int i = 0; n[i]; i++) if (n[i] == '/') base = n + i + 1;
    return base[0] == 'k' && base[1] == 'l' && base[2] == 'd' && base[3] == '-';
}

}  // namespace

namespace ProcessFreeze {

int FreezeAll() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* q = LinuxSyscall::GetProcess(i);
        if (!q || !q->active || q->exited || !q->task) continue;
        if (q->task->is_thread()) continue;           // the group follows its leader (satoru)
        if (is_driver_container(q)) continue;
        if (g_pid[i] == q->pid) continue;              // already ours (satoru)
        g_pid[i] = q->pid;
        LinuxSyscall::PostSignalIdx(i, 19);           // SIGSTOP (satoru)
        n++;
    }
    if (!g_frozen) {
        g_was_muted = AudioMixer::GetMasterMute();
        AudioMixer::SetMasterMute(true);
    }
    g_frozen = true;
    SerialLogger::Log("[freeze] paused ");
    SerialLogger::LogDec(n);
    SerialLogger::Log(" thread group(s)\r\n");
    return n;
}

int ThawAll() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        if (!g_pid[i]) continue;
        LinuxProcess* q = LinuxSyscall::GetProcess(i);
        if (q && q->active && !q->exited && q->pid == g_pid[i]) {
            LinuxSyscall::PostSignalIdx(i, 18);       // SIGCONT (satoru)
            n++;
        }
        g_pid[i] = 0;
    }
    if (g_frozen) AudioMixer::SetMasterMute(g_was_muted);
    g_frozen = false;
    SerialLogger::Log("[freeze] resumed ");
    SerialLogger::LogDec(n);
    SerialLogger::Log(" thread group(s)\r\n");
    return n;
}

bool Frozen() { return g_frozen; }

}  // namespace ProcessFreeze

// end (satoru)
