#include "idle_sleep.h"   // kurono.sleep=idle (satoru)
#include "power.h"
#include "../drivers/disklog.h"
#include "../drivers/intel_accel.h"   // CursorHideAll before reset (satoru)
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../hal/hal.h"
#include "../linux/linux_syscall.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../shell/shell.h"
#include "../security/ksa.h"
#include "../system/kinit.h"
#include "../ui/boot_anim.h"
#include "../ui/control_center.h"
#include "../ui/notification.h"

namespace {

// 0 idle, 1 queued, 2 running. a single manager state keeps a second request
// from replacing one already in flight. (satoru)
volatile uint32_t g_state = 0;
volatile int      g_action = PowerManager::ACTION_NONE;
char              g_origin[48] = {0};
volatile bool     g_button = false;
bool              g_worker_spawned = false;
Process*          g_worker = nullptr;
uint64_t          g_last_pump_kick_ms = 0;
uint64_t          g_last_auto_kick_ms = 0;

// test hooks (kurono.power.test=...). (satoru)
char     g_test[48] = {0};
uint32_t g_test_delay_ms = 15000;
bool     g_test_force_user = false;
bool     g_test_fast_autosave = false;

uint64_t g_last_autosave_ms = 0;
uint64_t g_last_claude_check_ms = 0;
uint64_t g_last_claude_fp = 0;

void log_str(const char* s) { SerialLogger::Log(s); }
void log_dec(int v) { SerialLogger::LogDec(v); }
bool streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; i++; }
    return a[i] == 0 && b[i] == 0;
}

void log_action_name(int a) {
    if (a == PowerManager::ACTION_SLEEP) log_str("sleep");
    else if (a == PowerManager::ACTION_RESTART) log_str("restart");
    else if (a == PowerManager::ACTION_SHUTDOWN) log_str("shutdown");
    else log_str("none");
}

bool user_task_alive() {
    if (g_test_force_user) return true;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (p && p->active && !p->exited && p->task && !p->task->is_kernel_proc) return true;
    }
    return false;
}

bool claude_running() {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (!p || !p->active || p->exited || !p->task) continue;
        const char* n = p->name;
        if (n[0] == 'c' && n[1] == 'l' && n[2] == 'a' && n[3] == 'u' &&
            n[4] == 'd' && n[5] == 'e') return true;
    }
    return false;
}

int live_user_tasks() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (p && p->active && !p->exited && p->task && !p->task->is_kernel_proc) n++;
    }
    return n;
}

// ask every ring-3 program to end. claude handles SIGTERM by writing its
// session/history, and the shell command that launched it then saves too. (satoru)
int term_ring3() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (!p || !p->active || p->exited || !p->task) continue;
        if (p->task->is_kernel_proc) continue;
        LinuxSyscall::PostSignalIdx(i, 15);   // SIGTERM (satoru)
        n++;
    }
    return n;
}

// after the bounded SIGTERM grace, take down whatever is still alive. the
// user-data save runs after this, so nothing that could still flush is lost. (satoru)
int force_ring3() {
    uint64_t seen[64];
    int nseen = 0, killed = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* p = LinuxSyscall::GetProcess(i);
        if (!p || !p->active || p->exited || !p->task) continue;
        if (p->task->is_kernel_proc) continue;
        uint64_t as = p->task->address_space;
        bool dup = false;
        for (int k = 0; k < nseen; k++) if (seen[k] == as) { dup = true; break; }
        if (dup) continue;
        if (nseen < 64) seen[nseen++] = as;
        Scheduler::KillProcessGroupAsync(p->task->pid, nullptr);
        killed++;
    }
    return killed;
}

void wait_ring3(uint32_t max_ms) {
    // hlt one irq at a time while staying the current process: a SleepMs here
    // hands the cpu back to a scheduler that can leave this worker Ready and
    // never pick it again under a firefox load. pumping the ui from here also
    // shows the shutdown film during the wait. (satoru)
    uint64_t t0 = Timer::GetRealMs64();
    while (live_user_tasks() > 0 && Timer::GetRealMs64() - t0 < max_ms) {
        // kurono.svccpu: this worker lives on the desktop cpu with the gui and
        // no program runs there, so a plain sleep keeps the film playing. (satoru)
        if (Scheduler::IsServiceCpu(SMP::CpuIndex())) { Scheduler::SleepMs(2); continue; }
        HAL::WaitForInterrupt();
        if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();
    }
}

bool ask_continue_after_failed_save() {
    KSARequest req;
    req.title     = "User data was not saved";
    req.detail    = "The power action can continue, but unsaved work will be lost. Continue?";
    req.username  = "root";
    req.want_cred = false;
    KSAVerdict v;
    return KSA::Prompt(req, v) && v.approved;
}

volatile uint32_t g_auto_busy = 0;

void autosave_tick() {
    if (!PersistStore::Available()) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_auto_busy, &expected, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    struct Release { ~Release() { __atomic_store_n(&g_auto_busy, 0u, __ATOMIC_RELEASE); } } rel;
    uint64_t now = Timer::GetRealMs64();
    bool user = user_task_alive();
    bool due = false;

    uint32_t normal_ms = g_test_fast_autosave ? 3000u : 120000u;
    if (user && now - g_last_autosave_ms >= normal_ms) due = true;

    // right after a claude session file changes: hash only ~/.claude, and do
    // not turn a fast-writing session into a save every few seconds. (satoru)
    if (!due && claude_running() && KVFS::Exists("/home/user/.claude") &&
        now - g_last_claude_check_ms >= 30000u) {
        g_last_claude_check_ms = now;
        uint64_t fp = PersistStore::ClaudeFingerprint();
        if (g_last_claude_fp && fp != g_last_claude_fp &&
            now - g_last_autosave_ms >= 45000u)
            due = true;
        g_last_claude_fp = fp;
    }
    if (!due) return;

    bool ok = PersistStore::SaveTree();
    g_last_autosave_ms = Timer::GetRealMs64();
    g_last_claude_fp = PersistStore::ClaudeFingerprint();
    if (!ok) {
        SerialLogger::Log("[persist] autosave FAILED, next attempt in ");
        log_dec((int)(normal_ms / 1000u));
        log_str(" s\r\n");
    }
}

void sleep_action() {
    log_str("[power] sleep requested, saving user data first\r\n");
    if (PersistStore::Available()) {
        bool ok = PersistStore::SaveTree();
        log_str(ok ? "[power] save before sleep ok\r\n" : "[power] save before sleep FAILED\r\n");
        if (!ok && !ask_continue_after_failed_save()) {
            log_str("[power] sleep cancelled after a failed save\r\n");
            return;
        }
    }
    // kurono.sleep=idle: screen off + apps paused + cpus idle (the S0 first
    // step; S3 does not exist on S0ix-only laptops). (satoru)
    if (IdleSleep::Enabled()) {
        IdleSleep::Run();
        return;
    }
    log_str("[power] entering ACPI suspend\r\n");
    if (!HAL::Suspend()) {
        log_str("[power] suspend unsupported on this platform\r\n");
        NotificationManager::Post("Sleep", "ACPI S3 is not available on this platform",
                                  NotificationManager::ICON_WARNING, 3200);
    }
}

void power_action(int action) {
    log_str("[power] ");
    log_action_name(action);
    log_str(" requested by ");
    log_str(g_origin[0] ? g_origin : "unknown");
    log_str("\r\n");

    if (action == PowerManager::ACTION_SLEEP) {
        sleep_action();
        return;
    }

    // a visible "something is happening" frame before the potentially long
    // term/save steps, and it doubles as the shutdown screen. (satoru)
    log_str("[power] starting the shutdown film\r\n");
    BootAnim::StartShutdown(0);

    int asked = term_ring3();
    log_str("[power] sent SIGTERM to ");
    log_dec(asked);
    log_str(" ring-3 programs\r\n");
    uint64_t t0 = Timer::GetRealMs64();
    wait_ring3(5000);
    log_str("[power] ring-3 wait done in ");
    log_dec((int)(Timer::GetRealMs64() - t0));
    log_str(" ms, still live=");
    log_dec(live_user_tasks());
    log_str("\r\n");
    if (live_user_tasks() > 0) {
        int forced = force_ring3();
        log_str("[power] forced down ");
        log_dec(forced);
        log_str(" remaining thread groups\r\n");
        wait_ring3(500);
    }

    if (PersistStore::Available()) {
        log_str("[power] saving user data\r\n");
        uint64_t s0 = Timer::GetRealMs64();
        bool ok = PersistStore::SaveTree();
        log_str(ok ? "[power] save ok in " : "[power] save FAILED in ");
        log_dec((int)(Timer::GetRealMs64() - s0));
        log_str(" ms\r\n");
        if (!ok) {
            // the film would hide the prompt, so put the desktop back first. (satoru)
            BootAnim::CancelShutdown();
            if (!ask_continue_after_failed_save()) {
                log_str("[power] cancelled after a failed save - nothing was reset\r\n");
                return;
            }
            log_str("[power] user chose to continue without a save\r\n");
            BootAnim::StartShutdown(0);
        }
    } else {
        log_str("[power] no persistent store, nothing to save\r\n");
    }

    log_str("[power] stopping kinit targets\r\n");
    KInit::Shutdown();
    log_str("[power] kinit targets stopped\r\n");

    log_str(action == PowerManager::ACTION_RESTART
                ? "[power] resetting the machine\r\n"
                : "[power] powering the machine off\r\n");
    // no hardware cursor plane may stay lit through the reset: the firmware
    // would show it frozen on top of its own screens. raw mmio, no locks. (satoru)
    IntelAccel::CursorHideAll();
    DiskLog::Flush();
    if (action == PowerManager::ACTION_RESTART) HAL::Reboot();
    else HAL::PowerOff();
}

uint64_t g_test_at_ms = 0;
bool     g_test_armed = false;

void test_tick() {
    if (!g_test[0] || !g_test_armed) return;
    if (Timer::GetRealMs64() < g_test_at_ms) return;
    g_test_armed = false;

    if (g_test[0] == 'a' && g_test[1] == 'u') {   // "autosave"
        static const char* path = "/home/user/power_autosave_marker";
        if (KVFS::Exists(path)) {
            log_str("[power-test] autosave PASS: marker survived a hard reset\r\n");
        } else {
            KVFS::WriteString(path, "kurono-power-autosave-v1");
            log_str("[power-test] autosave marker written, waiting for a save\r\n");
        }
        // no user app is needed for this test; the 3 s cadence proves the
        // worker path, the hard reset proves it reached the disk. (satoru)
        g_test_force_user = true;
        g_test_fast_autosave = true;
        return;
    }
    // one-shot action tests: a restart boots the same iso again, so a marker
    // keeps the second boot from starting another action. the marker is in
    // /home and therefore also proves the save reached the disk. (satoru)
    static const char* done_path = "/home/user/.power_test_done";
    if (KVFS::Exists(done_path)) {
        log_str("[power-test] action already ran on a previous boot, skipping\r\n");
        return;
    }
    if (g_test[0] == 'c' && g_test[1] == 'c') {    // "cc-restart" / "cc-shutdown"
        int act = (g_test[4] == 's' || g_test[3] == 's')
                      ? PowerManager::ACTION_SHUTDOWN
                      : PowerManager::ACTION_RESTART;
        KVFS::WriteString(done_path, "cc");
        log_str("[power-test] driving ControlCenter::ExecutePowerAction\r\n");
        ControlCenter::ExecutePowerActionForTest(act);
        return;
    }
    if (g_test[0] == 'b') {                        // "button"
        KVFS::WriteString(done_path, "button");
        log_str("[power-test] simulating the ACPI power button\r\n");
        g_button = true;
        return;
    }
    if (g_test[0] == 'r') { KVFS::WriteString(done_path, "restart"); PowerManager::Request(PowerManager::ACTION_RESTART, "test"); }
    else if (g_test[0] == 's' && g_test[1] == 'h') { KVFS::WriteString(done_path, "shutdown"); PowerManager::Request(PowerManager::ACTION_SHUTDOWN, "test"); }
    else if (g_test[0] == 's') { KVFS::WriteString(done_path, "sleep"); PowerManager::Request(PowerManager::ACTION_SLEEP, "test"); }
}

void test_arm() {
    if (!g_test[0]) return;
    if (streq(g_test, "buttonpoll")) {   // diagnostic: print the PM1 status each second
        for (int i = 0; i < 90; i++) {
            log_str("[power-test] pm1 status = ");
            log_dec((int)HAL::AcpiPowerButtonStatus());
            log_str("\r\n");
            Scheduler::SleepMs(1000);
        }
        g_test[0] = 0;
        return;
    }
    // arm the action on the worker's own 250 ms loop instead of sleeping for
    // seconds in a row: a long single sleep was not resumed under firefox load. (satoru)
    g_test_at_ms = Timer::GetRealMs64() + g_test_delay_ms;
    g_test_armed = true;
    log_str("[power-test] armed in ");
    log_dec((int)(g_test_delay_ms / 1000u));
    log_str(" s\r\n");
}

void background_pass() {
    if (g_test[0]) {
        log_str("[power-test] bg t=");
        log_dec((int)(Timer::GetRealMs64() / 1000u));
        log_str(" armed=");
        log_dec(g_test_armed ? 1 : 0);
        log_str("\r\n");
    }
    test_tick();
    if (HAL::AcpiPowerButtonPressed()) g_button = true;
    if (g_button) {
        g_button = false;
        log_str("[power] ACPI power button pressed\r\n");
        PowerManager::Request(PowerManager::ACTION_SHUTDOWN, "acpi power button");
    }
    uint32_t st = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    if (st == 1) {
        uint32_t expected = 1;
        if (__atomic_compare_exchange_n(&g_state, &expected, 2, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int action = (int)__atomic_load_n(&g_action, __ATOMIC_ACQUIRE);
            power_action(action);
            __atomic_store_n(&g_state, 0, __ATOMIC_RELEASE);
        }
    }
    autosave_tick();
}

[[noreturn]] void worker_entry() {
    log_str("[power] worker up (autosave every 120 s while a user program runs)\r\n");
    if (!HAL::AcpiPowerButtonInit())
        log_str("[power] ACPI power button not available on this machine\r\n");
    test_arm();
    for (;;) {
        PowerManager::BackgroundTick();
        Scheduler::SleepMs(1000);
    }
}

}  // namespace

void PowerManager::Start() {
    if (g_worker_spawned) return;
    g_worker_spawned = true;
    // high tier: under a firefox cpu load the NORMAL kernel procs (gui, shell)
    // can be starved for a long time, and a power request must never wait. (satoru)
    g_worker = Scheduler::SpawnKernelProcess("power", worker_entry, PRIO_HIGH, 64, 512);
}

// serialize the request fields before publishing the queued state, so the
// worker never reads a half-written action/origin. (satoru)
static volatile uint32_t g_req_lock = 0;

bool PowerManager::Request(Action action, const char* origin) {
    if (action == ACTION_NONE) return false;
    while (__atomic_exchange_n(&g_req_lock, 1u, __ATOMIC_ACQ_REL)) {
        if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 0) {
            SerialLogger::Log("[power] request ignored: another power action is in progress\r\n");
            return false;
        }
        Scheduler::SleepMs(1);
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_state, &expected, 3u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_req_lock, 0u, __ATOMIC_RELEASE);
        SerialLogger::Log("[power] request ignored: another power action is in progress\r\n");
        return false;
    }
    __atomic_store_n(&g_action, (int)action, __ATOMIC_RELAXED);
    int i = 0;
    if (origin) {
        while (origin[i] && i < (int)sizeof(g_origin) - 1) { g_origin[i] = origin[i]; i++; }
    }
    g_origin[i] = 0;
    __atomic_store_n(&g_state, 1u, __ATOMIC_RELEASE);   // publish (satoru)
    __atomic_store_n(&g_req_lock, 0u, __ATOMIC_RELEASE);
    SerialLogger::Log("[power] queued ");
    log_action_name((int)action);
    SerialLogger::Log(" from ");
    SerialLogger::Log(g_origin);
    SerialLogger::Log("\r\n");
    return true;
}

bool PowerManager::Busy() {
    return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 0;
}

void PowerManager::BackgroundTick() {
    background_pass();
}

bool PowerManager::PumpKickDue() {
    // while an action is queued or in progress, always. otherwise a slow
    // background kick only after the desktop is well past the firefox startup
    // handshake (a 1 s cadence here caused 0/3 paint), so autosave can run even
    // when the normal kernel picker starves the worker. (satoru)
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 0) return true;
    uint64_t now = Timer::GetRealMs64();
    if (now < 90000u) return false;
    if (now - g_last_auto_kick_ms < 30000u) return false;
    g_last_auto_kick_ms = now;
    return true;
}

bool PowerManager::PumpKick() {
    // the acpi button is polled from this always-running path as well, so a
    // press is seen even when the power worker itself is starved. (satoru)
    uint64_t now = Timer::GetRealMs64();
    if (now - g_last_pump_kick_ms >= 200u) {
        g_last_pump_kick_ms = now;
        if (HAL::AcpiPowerButtonPressed()) {
            log_str("[power] ACPI power button pressed\r\n");
            PowerManager::Request(PowerManager::ACTION_SHUTDOWN, "acpi power button");
        }
    }
    if (!g_worker || !PumpKickDue()) return false;
    return Scheduler::DonateToKernelProc(g_worker, 2);
}

void PowerManager::NotifyPowerButton() {
    g_button = true;
}

void PowerManager::SetTest(const char* spec, uint32_t delay_ms) {
    if (!spec) { g_test[0] = 0; return; }
    int i = 0;
    while (spec[i] && i < (int)sizeof(g_test) - 1) { g_test[i] = spec[i]; i++; }
    g_test[i] = 0;
    g_test_delay_ms = delay_ms ? delay_ms : 15000;
}

// end (satoru)
