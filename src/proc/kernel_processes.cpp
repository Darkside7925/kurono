// Kurono OS - kernel process registry implementation
//
// Each kernel-mode process spawned here owns one subsystem and runs an
// infinite Sleep/Yield loop.  They are scheduled cooperatively via the
// preemptive scheduler in proc/scheduler.cpp; PIT IRQ0 charges runtime
// against the active process and sets need_resched when its timeslice
// is exhausted.

#include "kernel_processes.h"
#include "scheduler.h"
#include "spinlock.h"
#include "kernel_locks.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../kernel/hrtimer.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/pulse_server.h"
#include "../drivers/ac97.h"
#include "../drivers/graphics.h"
#include "../drivers/intel_accel.h"   // hardware cursor frame accounting (satoru)
#include "../drivers/display_mgr.h"
#include "../drivers/usb.h"
#include "../drivers/e1000.h"
#include "../system/ui_config.h"
#include "../net/network.h"
#include "../net/dhcp.h"
#include "../net/tcpip.h"
#include "../linux/linux_netbridge.h"   // task 30: wake inet pollers after the nic drain (satoru)
#include "../system/input_manager.h"
#include "../system/logging.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../ui/ui_diag.h"   // kurono.uidiag=1 pointer/drag counters (satoru)
#include "../ui/wayland_server.h"   // route scroll to the surface under the pointer (satoru)
#include "../drivers/outputs.h"   // OUTPUT_MAX for the soft relayout reloc arrays (satoru)
#include "../ui/gui.h"   // GUI::UpdateBackbuffer for the resolution-change relayout (satoru)
#include "../ui/perf_hud.h"
#include "../ui/boot_anim.h"
#include "../ui/frame_pacer.h"   // sub-ms frame period + adaptive refresh step (satoru)
#include "../ui/lockscreen.h"
#include "../system/user_mgmt.h"
#include "../apps/terminal.h"
#include "../shell/shell.h"   // KuronoShell::Execute for detached desktop launches (satoru)
#include "../kernel/userspace.h"   // sched=2 ctrl+c cancel probe for the launcher wait (satoru)
#include "../linux/kpty.h"            // the exec bridge writes a built-in's text to its pair (satoru)
#include "../linux/linux_syscall.h"   // sched=2: kernel-proc work runs under the syscall-layer lock (satoru)
#include "../apps/task_manager.h"
#include "../apps/system_settings.h"   // @settings gui autorun sentinel (satoru)
#include "../apps/settings.h"
#include "../apps/settings_display.h"   // displays page overlay hook (satoru)
#include "../ui/control_center.h"
#include "../ui/kss.h"
#include "../ui/notification.h"
#include "../apps/kj.h"   // kj-scripted animation demo (kurono.kjdemo) (satoru)
#include "../apps/media_player.h"
#include "../apps/denji_app.h"
#include "../security/ksa.h"   // one-shot interactive prompt demo (kurono.ksa.prompt) (satoru)
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
// test rig globals, defined in hal/hal.cpp (satoru)
extern volatile uint32_t g_ffcrash_secs;
extern volatile uint64_t g_ffcrash_as, g_ffcrash_at;
extern volatile int g_ffreopen_left;
int g_gui_extra_terms = 0;   // kurono.guiterms=N: extra terminal windows after the autorun one (satoru)
static int g_wp_test = 0;    // kurono.wptest=1: deterministic wallpaper damage test (satoru)

namespace KernelProcesses {

namespace {
// graphics-loop heartbeat: GUIProcessEntry stamps this with Timer::GetRealMs()
// every iteration; the watchdog process treats a >3 s gap as a stalled display
// and attempts a backend reinit. volatile because it is written by the gui
// process and read by the watchdog process. (satoru)
volatile uint32_t g_last_frame_ms = 0;
constexpr int  GUI_AUTORUN_MAX = 256;
char           g_gui_autorun_cmd[GUI_AUTORUN_MAX] = {};
volatile bool  g_gui_autorun_armed = false;
// one-shot interactive ksa prompt demo (kurono.ksa.prompt). armed early; the
// gui process fires it once after the desktop has presented some frames so the
// modal lands over a real desktop. (satoru)
volatile bool  g_ksa_prompt_demo_armed = false;
volatile bool  g_ksa_prompt_demo_cred  = false;
// one-shot kj-scripted animation demo (kurono.kjdemo). armed early; the gui
// process runs a shipped .kj script once after the desktop is up, so a real
// on-screen animation (an eased accent transition) is driven through the kj host
// bindings rather than c++. (satoru)
volatile bool  g_kj_demo_armed = false;
// detached shell command slot: written by desktop launchers on the gui
// process, drained by the shell process below - no terminal window involved.
// single slot on purpose: the shell process runs one command at a time, and a
// second desktop launch while one is in flight should be refused, not queued
// behind a possibly minutes-long install. (satoru)
constexpr int  DETACHED_CMD_MAX = 256;
char           g_detached_cmd[DETACHED_CMD_MAX] = {};
volatile bool  g_detached_armed   = false;
volatile bool  g_detached_running = false;
// gui frame pacing: the next frame deadline on the FramePacer ns clock, and
// whether the previous loop iteration rendered (an idle gap breaks the
// streak, so the first frame after it is neither late nor a miss). (satoru)
uint64_t       g_pace_due_ns = 0;
uint64_t       g_pace_last_start_ns = 0;
bool           g_pace_streak = false;
}

void ArmKsaPromptDemo(bool want_cred) {
    g_ksa_prompt_demo_cred  = want_cred;
    g_ksa_prompt_demo_armed = true;
}

void ArmKjDemo() { g_kj_demo_armed = true; }

void SetWpTest(int mode) { g_wp_test = mode; }

// deterministic wallpaper damage test. two plain windows with a static pattern
// are moved, minimized, restored and closed on a fixed ms schedule so a
// headless harness can compare screendumps with kurono.wpdamage on and off.
// window content never animates (no app, no clock), so the frames must match
// pixel for pixel outside the taskbar clock. (satoru)
namespace {
int g_wp_test_id_a = -1, g_wp_test_id_b = -1;
int g_wp_test_stage = -1;
uint32_t g_wp_test_t0 = 0;

void wp_test_render(Window* win, int x, int y, int w, int h) {
    bool odd = win && win->title[0] == 'B';
    Graphics::FillRect(x, y, w, h, odd ? 0xFF2A2038u : 0xFF202A38u);
    for (int yy = 0; yy < h; yy += 40) {
        for (int xx = 0; xx < w; xx += 40) {
            bool checker = (((xx / 40) + (yy / 40) + (odd ? 1 : 0)) & 1) != 0;
            Graphics::FillRect(x + xx + 4, y + yy + 4, 28, 28,
                               checker ? 0xFFE8E8E8u : 0xFF485878u);
        }
    }
    Graphics::FillRect(x + 12, y + h - 34, w - 24, 10, odd ? 0xFFE08040u : 0xFF40C080u);
}

void wp_test_step(int step) {
    SerialLogger::Log("[wptest] step=");
    SerialLogger::LogDec(step);
    SerialLogger::Log("\r\n");
}

void wp_test_tick() {
    if (!g_wp_test) return;
    if (BootAnim::CoversScreen()) return;
    uint32_t now = Timer::GetRealMs();
    if (g_wp_test_stage < 0) {
        g_wp_test_stage = 0;
        g_wp_test_t0 = now + 800;
        return;
    }
    if ((int32_t)(now - g_wp_test_t0) < 0) return;
    switch (g_wp_test_stage) {
    case 0:
        g_wp_test_id_a = WindowManager::CreateWindow("A", 140, 150, 420, 300, wp_test_render, nullptr);
        g_wp_test_id_b = WindowManager::CreateWindow("B", 620, 320, 360, 260, wp_test_render, nullptr);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 1;
        wp_test_step(1);
        break;
    case 1:
        WindowManager::MoveWindow(g_wp_test_id_a, 500, 170);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 2;
        wp_test_step(2);
        break;
    case 2:
        WindowManager::Minimize(g_wp_test_id_a);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 3;
        wp_test_step(3);
        break;
    case 3:
        WindowManager::Unminimize(g_wp_test_id_a);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 4;
        wp_test_step(4);
        break;
    case 4:
        WindowManager::CloseWindow(g_wp_test_id_b);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 5;
        wp_test_step(5);
        break;
    case 5:
        WindowManager::MoveWindow(g_wp_test_id_a, 140, 150);
        g_wp_test_t0 = now + 2200;
        g_wp_test_stage = 6;
        wp_test_step(6);
        break;
    case 6:
        g_wp_test_stage = 7;
        wp_test_step(7);
        break;
    default:
        break;
    }
}
}

uint32_t LastGuiFrameMs() { return g_last_frame_ms; }

// sched=2: desktop launches get their own launcher kernel procs instead of
// queueing behind whatever the shell is running (a terminal's claude used to
// block the firefox icon, and firefox blocked every terminal). each launcher
// owns one slot; RunProcessWithArgs inside it waits cooperatively. (satoru)
constexpr int  LAUNCH_SLOTS = 6;
char           g_launch_cmd[LAUNCH_SLOTS][DETACHED_CMD_MAX] = {};
volatile int   g_launch_state[LAUNCH_SLOTS] = {};   // 0 free, 1 armed, 2 running (satoru)
int            g_launch_pair[LAUNCH_SLOTS] = { -1, -1, -1, -1, -1, -1 };   // bridge: the terminal pair, -1 none (satoru)
void*          g_launch_kproc[LAUNCH_SLOTS] = {};
volatile int   g_launch_cancel[LAUNCH_SLOTS] = {};

static bool launch_slot_arm(const char* cmd) {
    // the same command already armed or running in another launcher: refuse,
    // so the desktop shows "already starting" instead of a second instance. (satoru)
    for (int s = 0; s < LAUNCH_SLOTS; s++) {
        if (__atomic_load_n(&g_launch_state[s], __ATOMIC_ACQUIRE) == 0) continue;
        int i = 0;
        while (cmd[i] && g_launch_cmd[s][i] == cmd[i]) i++;
        if (cmd[i] == 0 && g_launch_cmd[s][i] == 0) return false;
    }
    for (int s = 0; s < LAUNCH_SLOTS; s++) {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&g_launch_state[s], &expected, 3, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        int i = 0;
        while (cmd[i] && i < DETACHED_CMD_MAX - 1) { g_launch_cmd[s][i] = cmd[i]; i++; }
        g_launch_cmd[s][i] = 0;
        __atomic_store_n(&g_launch_state[s], 1, __ATOMIC_RELEASE);
        return true;
    }
    return false;   // every launcher busy (satoru)
}

// sched=2: terminal worker kernel procs - each runs one terminal window's
// command (TerminalApp::WorkerTick), so a long command in one window does not
// block the others. same stack budget as the shell. (satoru)
template <int S>
[[noreturn]] static void TermWorkerEntry() {
    while (true) {
        TerminalApp::WorkerTick(S);
        Scheduler::SleepMs(40);   // 4 workers: ~100 idle wakeups/s on the bsp (satoru)
    }
}

template <int S>
[[noreturn]] static void LauncherProcessEntry() {
    while (true) {
        g_launch_kproc[S] = (void*)Scheduler::GetCurrentProcess();
        if (__atomic_load_n(&g_launch_state[S], __ATOMIC_ACQUIRE) == 1) {
            __atomic_store_n(&g_launch_state[S], 2, __ATOMIC_RELEASE);
            LinuxSyscall::SetBridgeTty(g_launch_pair[S]);
            SerialLogger::Log("[launcher] ");
            SerialLogger::Log(g_launch_cmd[S]);
            SerialLogger::Log("\r\n");
            static char lout[LAUNCH_SLOTS][2048];
            lout[S][0] = 0;
            KuronoShell::Execute(g_launch_cmd[S], lout[S], (int)sizeof(lout[S]));
            if (lout[S][0]) {
                SerialLogger::Log("[launcher] output: ");
                SerialLogger::Log(lout[S]);
                SerialLogger::Log("\r\n");
                // a bridged built-in's returned text goes to its terminal (satoru)
                if (g_launch_pair[S] >= 0) {
                    int ll = 0; while (lout[S][ll]) ll++;
                    Kpty::SlaveWrite(g_launch_pair[S], (const uint8_t*)lout[S], ll);
                    if (ll > 0 && lout[S][ll - 1] != '\n') Kpty::SlaveWrite(g_launch_pair[S], (const uint8_t*)"\n", 1);
                }
            }
            LinuxSyscall::SetBridgeTty(-1);
            g_launch_pair[S] = -1;
            g_launch_cancel[S] = 0;
            __atomic_store_n(&g_launch_state[S], 0, __ATOMIC_RELEASE);
        }
        Scheduler::SleepMs(25);
    }
}

int RunShellCommandBridge(const char* cmd, int pair) {
    if (!cmd || !cmd[0] || !Scheduler::DetachedLaunch()) return -1;
    for (int s = 0; s < LAUNCH_SLOTS; s++) {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&g_launch_state[s], &expected, 3, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        int i = 0;
        while (cmd[i] && i < DETACHED_CMD_MAX - 1) { g_launch_cmd[s][i] = cmd[i]; i++; }
        g_launch_cmd[s][i] = 0;
        g_launch_pair[s] = pair;
        g_launch_cancel[s] = 0;
        __atomic_store_n(&g_launch_state[s], 1, __ATOMIC_RELEASE);
        return s;
    }
    return -1;
}
bool BridgeBusy(int pair) {
    for (int s = 0; s < LAUNCH_SLOTS; s++)
        if (g_launch_pair[s] == pair && __atomic_load_n(&g_launch_state[s], __ATOMIC_ACQUIRE) != 0) return true;
    return false;
}
void BridgeCancel(int pair) {
    for (int s = 0; s < LAUNCH_SLOTS; s++)
        if (g_launch_pair[s] == pair && __atomic_load_n(&g_launch_state[s], __ATOMIC_ACQUIRE) != 0) g_launch_cancel[s] = 1;
}
bool BridgeCancelRequested() {
    void* me = (void*)Scheduler::GetCurrentProcess();
    for (int s = 0; s < LAUNCH_SLOTS; s++)
        if (g_launch_kproc[s] == me && g_launch_pair[s] >= 0 && g_launch_cancel[s]) return true;
    return false;
}

bool RunShellCommandDetached(const char* cmd) {
    if (!cmd || !cmd[0]) return false;
    if (Scheduler::DetachedLaunch()) return launch_slot_arm(cmd);
    if (g_detached_armed || g_detached_running) return false;
    int i = 0;
    while (cmd[i] && i < DETACHED_CMD_MAX - 1) {
        g_detached_cmd[i] = cmd[i];
        i++;
    }
    g_detached_cmd[i] = 0;
    g_detached_armed = true;
    return true;
}

// sched=2: the user programs now run on the aps while these kernel procs run
// on the bsp, truly at the same time. the state they share - unix socket rings,
// the wayland server, kvfs, the poll registry, the ld-kurono tables - was written
// for one cpu and is serialized only by kls, which every syscall body holds. in
// the old model the kernel procs never overlapped a firefox syscall (both lived
// on the bsp), and PumpUI ran this same work INSIDE the syscall under kls. so a
// kernel proc's work runs under kls too: entered after it wakes, left before it
// sleeps or yields, never held across a switch. old mode: no-op. (satoru)
struct KlsSection {
    bool held = false;
    void enter() { if (!held && Scheduler::SchedV2()) { LinuxSyscall::LayerLock(); held = true; } }
    void leave() { if (held) { LinuxSyscall::LayerUnlock(); held = false; } }
    ~KlsSection() { leave(); }
};

// sched=2: concurrent terminal commands (kurono.schedterm=0 keeps the single shell). (satoru)
static bool g_term_workers = true;
void SetTermWorkers(bool on) { g_term_workers = on; }
// kurono.termtest=1 (test rig): 8 s after the autorun, open a second terminal
// and run `help` there while the first window's command is still running. (satoru)
static bool g_term_test = false;
void SetTermTest(bool on) { g_term_test = on; }

// kurono.kstacktest=1 (test rig): a kernel proc that recurses until it runs off
// its stack, to prove the guard page faults and names the proc. (satoru)
__attribute__((noinline)) static uint64_t kst_recurse(uint64_t d) {
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)d;
    if (d > 1000000) return pad[0];
    return kst_recurse(d + 1) + pad[0];
}
[[noreturn]] static void KstackTestEntry() {
    Scheduler::SleepMs(4000);
    SerialLogger::Log("[kstacktest] recursing until the guard page\r\n");
    volatile uint64_t r = kst_recurse(0);
    (void)r;
    for (;;) Scheduler::SleepMs(60000);
}
static bool g_kstack_test = false;
void SetKstackTest(bool on) { g_kstack_test = on; }

// autorun grace in ms; overridden by kurono.fastrun=1 before SpawnAll(). (satoru)
static uint32_t g_autorun_delay_ms = 2000u;
void SetAutorunDelayMs(uint32_t ms) { g_autorun_delay_ms = ms; }

// kurono.fpsbench=1: forced-render benchmark mode (see the gate in the gui
// loop). off by default - it pegs a core on purpose. (satoru)
// 0 = off, 1 = forced render with NO pacing (max pipeline throughput),
// 2 = forced render but KEEP the 60fps pacer - the realistic desktop case,
// where the point is not throughput but how much bsp time the present
// leaves for everything else. (satoru)
static int g_fps_bench = 0;
void SetFpsBench(int mode) { g_fps_bench = mode; }

void SetGuiAutorun(const char* cmd) {
    if (!cmd) {
        g_gui_autorun_cmd[0] = 0;
        g_gui_autorun_armed = false;
        return;
    }
    int i = 0;
    while (cmd[i] && i < GUI_AUTORUN_MAX - 1) {
        g_gui_autorun_cmd[i] = cmd[i];
        i++;
    }
    g_gui_autorun_cmd[i] = 0;
    g_gui_autorun_armed = (g_gui_autorun_cmd[0] != 0);
}

const char* GuiAutorun() { return g_gui_autorun_cmd; }

// ── Network process: TCP/IP polling + E1000 ownership ─────────────────
//
//   `g_net_lock` protects the socket table / ARP cache against
//   concurrent writes from packet RX and from the rest of the kernel.
//   This loop uses a 1ms sleep per spec.
[[noreturn]] static void NetworkProcessEntry() {
    SerialLogger::Log("[NetworkProcess] online\r\n");
    while (true) {
        KlsSection ks; ks.enter();   // sched=2 (satoru)
        {
            SpinLockCpuGuard guard(g_net_lock);
            if (TCPStack::IsUp()) {
                TCPStack::Tick();
            } else if (E1000::IsDetected()) {
                E1000::Poll();
            }
        }
        // task 30: hand the drained rx to any parked poller NOW (outside the
        // net lock - the wake takes the pollreg lock and must not nest). (satoru)
        LinuxNetBridge::WakeRxPollers();
        Network::RefreshLink();   // pick up a late link-up/down on the real nic (satoru)
        Dhcp::Tick();             // lease acquisition, restarts itself on every link-up (satoru)
        WiFi::Tick();             // auto-rejoin a lost wifi association (satoru)
        // mirror a fresh lease into the linux bridge + /etc/resolv.conf. do it
        // after the net lock and kls are released: it may write kvfs. (satoru)
        LinuxNetBridge::RefreshFromKernel();
        ks.leave();
        Scheduler::SleepMs(10);   // RX latency-tolerant; 1ms over-polled an idle NIC (satoru)
    }
}

// ── Input process: keyboard / mouse / input manager polling ───────────
[[noreturn]] static void InputProcessEntry() {
    SerialLogger::Log("[InputProcess] online\r\n");
    while (true) {
        // ksa owns input while an authorization modal is up: do not poll the
        // 8042 and feed the compositor behind the prompt's back. (satoru)
        if (KSA::SecureDesktopActive()) { Scheduler::SleepMs(10); continue; }
        KlsSection ks; ks.enter();   // sched=2 (satoru)
        {
            SpinLockCpuGuard guard(g_input_lock);
            Keyboard::Poll();
            Mouse::Poll();
            USB::PollHID();   // poll usb hid interrupt-in endpoints (satoru)
            InputManager::Poll();
        }
        ks.leave();
        Scheduler::SleepMs(8);   // 125Hz: ample for human input, 8x fewer 8042/backdoor VM-exits (satoru)
    }
}

// ── Audio process: AC97 / mixer pump ──────────────────────────────────
[[noreturn]] static void AudioProcessEntry() {
    SerialLogger::Log("[AudioProcess] online\r\n");
    while (true) {
        // LockedTick owns the audio pump lock (shared with the pit backup
        // pump + the syscall-context stream writers); the old local
        // g_audio_lock guarded only this loop and left pulse writes racing
        // the mixer from other cpus. (satoru)
        KlsSection ks; ks.enter();   // sched=2: pulse injects into client sockets (satoru)
        AudioServer::LockedTick();
        // pace pulse clients AFTER the pump: send REQUEST for the bytes the
        // mixer just consumed so firefox keeps streaming (process context
        // only - it injects into client sockets). (satoru)
        PulseServer::Pace();
        ks.leave();
        // 10ms: the mixer period is ~21ms (1024 frames @ 48kHz); the pit
        // backup pump covers any gap past ~15ms, so this cadence is ample. (satoru)
        Scheduler::SleepMs(10);
    }
}

// ── Timekeeper process: the wallclock as its own service (REALTIME) ────
//
//   ws1 service separation: TimeManager::AdvanceByMs used to be everyone's
//   side job (the gui loop, PumpUI at every syscall, each blocking wait) -
//   so editing any of those could stall the clock that ping/dns/timeouts
//   read. this tiny process owns it instead. Timer::ElapsedSinceLast is a
//   shared stateful baseline, so the remaining callers PARTITION the real
//   delta with us rather than double-count: time now provably advances even
//   if every pump is gated, and the gui/PumpUI advancing it too stays safe. (satoru)
[[noreturn]] static void TimekeeperProcessEntry() {
    SerialLogger::Log("[TimekeeperProcess] online\r\n");
    while (true) {
        uint32_t e = Timer::ElapsedSinceLast();
        if (e > 0) TimeManager::AdvanceByMs(e);
        // network service lives here as well as in the network process: the
        // realtime clock keeper never starves, and an idle desktop can sleep its
        // gui loop for minutes. 10ms matches the network process cadence. (satoru)
        {
            static uint32_t s_net_ms = 0;
            uint32_t nnow = Timer::GetTicks();
            if ((uint32_t)(nnow - s_net_ms) >= 10u) {
                s_net_ms = nnow;
                if (TCPStack::IsUp()) {
                    SpinLockCpuGuard guard(g_net_lock);
                    TCPStack::Tick();
                } else if (E1000::IsDetected()) {
                    SpinLockCpuGuard guard(g_net_lock);
                    E1000::Poll();
                }
                Dhcp::Tick();
                LinuxNetBridge::RefreshFromKernel();
            }
        }
        Scheduler::SleepMs(5);   // 200hz clock service; the irq keeps sub-ms ticks (satoru)
    }
}

// ── Scheduler process: HRTimer + sleep-queue service (REALTIME) ────────
//
//   The IRQ0 hook already calls HRTimer::Tick + wake_due_processes
//   on every PIT pulse; this process is mostly a defensive heartbeat
//   that runs at REALTIME priority so an orphaned sleeper still wakes
//   if the IRQ path is disabled briefly.
[[noreturn]] static void SchedulerProcessEntry() {
    SerialLogger::Log("[SchedulerProcess] online\r\n");
    while (true) {
        KlsSection ks; ks.enter();   // sched=2: proc refresh writes kvfs, reaps touch ld-kurono (satoru)
        HRTimer::Tick();
        Scheduler::ServiceSleepQueue();
        // deferred group reaps (sig-9 from the kls syscall body, hal fault
        // containment, sync quiesce timeouts): reap each queued group once its
        // members are all off-cpu. never sleeps - one check per heartbeat. (satoru)
        Scheduler::DrainDeferredReaps();
        LinuxSyscall::ReapTick();   // sched=2: exited threads -> real reap (satoru)
        LinuxSyscall::FinishIrqKills();   // sched=2: records of async fatal kills (satoru)
        ks.leave();   // the census line below is a uart busy-wait: never under kls (satoru)
        Scheduler::KstackCensus();   // guarded kernel stacks: high-water marks (satoru)
        if (Scheduler::SchedV2()) {
            // one census line whenever it moved, at most every 10 s. (satoru)
            static uint64_t s_last_fr = ~0ull, s_last_ms = 0;
            uint64_t fr = 0, lk = 0; int pe = 0;
            Scheduler::ReapStats(&fr, &pe, &lk);
            uint64_t nw = Timer::GetRealMs64();
            if (fr != s_last_fr && nw - s_last_ms >= 10000) {
                s_last_fr = fr; s_last_ms = nw;
                SerialLogger::Log("[reap] freed="); SerialLogger::LogDec((int)fr);
                SerialLogger::Log(" pending="); SerialLogger::LogDec(pe);
                SerialLogger::Log(" leaked="); SerialLogger::LogDec((int)lk);
                SerialLogger::Log(" heap_kb="); SerialLogger::LogDec((int)(KernelHeap::GetUsed() / 1024));
                SerialLogger::Log("\r\n");
            }
        }
        // 50ms: IRQ0 already calls wake_due_processes() every tick, so this is a
        // backup heartbeat. 1ms was 1000 redundant wakeups/sec that kept the cpu
        // from idling (HLT) - the main VMware host-CPU burn. (satoru)
        Scheduler::SleepMs(50);
    }
}

// ── Logging process: periodic RuntimeLog flush ────────────────────────
//
//   Walks the buffered log lines and flushes them to KVFS every 500ms.
//   RuntimeLog::MirrorSerial does the heavy lifting; this loop is
//   purely about pacing.
[[noreturn]] static void LoggingProcessEntry() {
    SerialLogger::Log("[LoggingProcess] online\r\n");
    while (true) {
        // move the staged serial-mirror ring into kvfs. this is the ONLY place
        // the mirror touches kvfs/heap - MirrorSerial itself just stages into
        // a static ring from whatever context it runs in. (satoru)
        {
            KlsSection ks; ks.enter();   // sched=2: kvfs (satoru)
            SpinLockCpuGuard guard(g_log_lock);
            RuntimeLog::FlushSerialMirror();
        }
        Scheduler::SleepMs(500);
    }
}

// ── Shell process: drains the queued command from TerminalApp ─────────
//
//   `TerminalApp::Tick()` performs the actual KuronoShell::Execute call
//   when a command is pending.  Decoupling that from the GUI thread
//   means `kpkg sync` / `ping 8.8.8.8` no longer freezes the desktop - 
//   the GUI keeps rendering at full FPS while we churn.
[[noreturn]] static void ShellProcessEntry() {
    SerialLogger::Log("[ShellProcess] online\r\n");
    // sched=2: a program this proc launched is killed by ctrl+c in the terminal
    // (the launcher wait polls the shell's cancel flag). (satoru)
    // (the terminal workers register their own probe when they run.) (satoru)
    if (!(Scheduler::DetachedLaunch() && g_term_workers))
        Userspace::SetCancelProbe(Scheduler::GetCurrentProcess(), &KuronoShell::IsCommandCancelRequested);
    while (true) {
        TerminalApp::Tick();
        // detached command (desktop launchers): same worker, no terminal.
        // output is serial-only - the desktop app owns its own feedback
        // (toast + the launched window itself). (satoru)
        if (g_detached_armed) {
            g_detached_armed   = false;
            g_detached_running = true;
            SerialLogger::Log("[detached] ");
            SerialLogger::Log(g_detached_cmd);
            SerialLogger::Log("\r\n");
            static char dout[2048];
            dout[0] = 0;
            KuronoShell::Execute(g_detached_cmd, dout, (int)sizeof(dout));
            if (dout[0]) {
                SerialLogger::Log("[detached] output: ");
                SerialLogger::Log(dout);
                SerialLogger::Log("\r\n");
            }
            g_detached_running = false;
        }
        Scheduler::SleepMs(25);   // drains an almost-always-empty command queue; 2ms was 500 idle wakeups/sec (satoru)
    }
}

extern "C" void ff_main_ring_dump();

namespace {
// Is anything that must keep rendering continuously currently live?
// While ANY of these holds, the damage-gated loop renders every frame so a
// missed MarkUIDirty() can never freeze an in-flight animation or video.
// Conservative by design: when unsure, return true (render). (satoru)
// which source last kept the loop rendering (0 = none), for the [gui] line.
static int g_active_why = 0;
static inline bool ui_activity_active() {
    int w = 0;
    if (WindowManager::IsDragging())            w = 1;   // live drag/resize
    else if (WindowManager::HasActiveAnimations()) w = 2; // open/close/min/max
    else if (Taskbar::IsAnimating())            w = 3;   // menus/popups/cursor
    else if (Desktop::IsAnimating())            w = 4;   // icon hover-pop fade
    else if (ControlCenter::IsAnimating())      w = 5;   // panel slide+fade
    else if (KSS::Anim::Active())               w = 6;   // kss tween engine in flight
    else if (KSS::Sheet::Active())              w = 7;   // kss keyframe track playing
    else if (NotificationManager::ActiveCount() > 0) w = 8; // toasts in/hold/out
    else if (MediaPlayerApp::IsOpen())          w = 9;   // audio/video surface
    else if (DenjiApp::NeedsFrames())           w = 10;  // media player fades / scrubbing (satoru)
    else if (BootAnim::Active())                w = 11;  // boot animation tail (satoru)
    // task manager no longer pins the loop at 60fps: it refreshes at 1hz from its
    // own Render() and the 250ms fallback frame drives that, so an open task
    // manager costs ~4 frames/s instead of 60. (satoru)
    g_active_why = w;
    return w != 0;
}
} // namespace

// ── GUI process: the original main render loop ────────────────────────
//
//   Owns DesktopEnvironment / WindowManager / Graphics::SwapBuffers.
//   Calls Yield when ShouldRender() returns false instead of busy-
//   waiting so other processes get the CPU.  Honours the
//   `kurono.gui.run` autorun by opening Terminal once at boot and
//   queueing the command.
[[noreturn]] static void GUIProcessEntry() {
    SerialLogger::Log("[GUIProcess] online\r\n");
    TaskMem::SetUiTask();   // kurono.taskmem=1: PumpUI work elsewhere is charged here (satoru)
    BootAnim::GuiTakeover();   // the boot animation tail plays over the live desktop (satoru)

    Mouse::SetAutoDraw(false);

    uint32_t frame_counter = 0;
    uint32_t fps_counter = 0;
    uint32_t last_fps_ms = Timer::GetRealMs();
    uint32_t displayed_fps = 0;
    uint32_t last_render_ms = 0;
    uint32_t last_activity_ms = Timer::GetRealMs();   // last input/animation (satoru)
    // autorun grace: 2s halves the old 4s dead time while still comfortably
    // past session init. (a 1s + rendered-frame gate variant was flaky: an
    // idle dirty-driven desktop can render too few frames to ever pass the
    // gate.) (satoru)
    uint32_t autorun_target_ms = Timer::GetTicks() + g_autorun_delay_ms;

    while (true) {
        // a gui-hosted authorization modal keeps this loop running: the gui is
        // its compositor and input pump, so the desktop keeps rendering behind
        // the prompt and usb hid keeps being polled here. when the waiting
        // syscall self-pumps the ui (bsp ring-0), the gui stands down as
        // before so two compositors never run at once. (satoru)
        if (KSA::SecureDesktopActive() && !KSA::ModalGuiHosted()) {
            Scheduler::SleepMs(10); continue;
        }
        KlsSection ks; ks.enter();   // sched=2: left before every sleep/yield below (satoru)
        // stamp the graphics-loop heartbeat for the watchdog before any work,
        // so a hang anywhere in the body is detectable as a stalled tick. (satoru)
        g_last_frame_ms = Timer::GetRealMs();

        // resolution-sync handler: the desktop is first laid out during kernel
        // init, but a display backend can finalize a *different* mode afterwards
        // (virtio-gpu comes up at 1080p, after the boot-fb desktop layout), and
        // the user can also change modes at runtime (settings -> display). in
        // both cases the compositor backbuffer is still sized for the old mode
        // and the desktop is laid out for the old screen, so the larger
        // framebuffer shows uninitialized vram in the margins (desktop stuck in a
        // corner). the -1 sentinel forces a one-time relayout on the first frame
        // so the desktop always matches the *final* framebuffer size; after that
        // we only relayout on an actual change. the wallpaper image survives a
        // relayout (Desktop::Init keeps it). loop top is a safe point, never
        // mid-input. (satoru)
        // a ui scale change requested by settings lands here: this is the one safe
        // point to free/re-size the back buffer. the res-sync below then relayouts
        // the desktop for the new logical size. (satoru)
        Graphics::ApplyPendingUIScale();
        Graphics::ApplyPendingOutputOp();
        { static int __sw = -1, __sh = -1;
          int cw = Graphics::GetWidth(), ch = Graphics::GetHeight();
          bool soft_relayout = Graphics::TakeMultiRelayout();
          if (cw != __sw || ch != __sh || soft_relayout) {
              SerialLogger::Log("[res-sync] relayout to framebuffer mode\r\n");
              // keep the pointer at the same place on the screen across a scale change (satoru)
              if (__sw > 0 && __sh > 0) {
                  Mouse::mx = (int)((long long)Mouse::mx * cw / __sw);
                  Mouse::my = (int)((long long)Mouse::my * ch / __sh);
              }
              GUI::UpdateBackbuffer();
              if (soft_relayout && __sw > 0) {
                  // scale / output arrangement change: keep the windows (satoru)
                  int ox[OUTPUT_MAX], oy[OUTPUT_MAX], sa = -1, sb = -1;
                  bool reloc = Graphics::TakeOutputReloc(ox, oy, &sa, &sb);
                  DesktopEnvironment::Relayout(cw, ch, reloc ? ox : nullptr, reloc ? oy : nullptr, sa, sb);
              } else {
                  DesktopEnvironment::Init(cw, ch);
              }
              Graphics::MarkUIDirty();
          }
          __sw = cw; __sh = ch; }

        // Advance system wall-clock time once per frame.
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);

        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // Poll the input devices from THIS process too, not just the dedicated
        // InputProcess. under a running firefox (4 userland threads pinned
        // across the aps + apthreads) the cooperative InputProcess gets starved
        // - measured: Mouse::Poll ran ONCE then never again, so the scroll wheel
        // (and any device state the gui reads straight from Mouse::) went dead
        // while the browser was up. the gui process is the one kernel process
        // that demonstrably keeps getting scheduled (it paints firefox), so
        // polling here guarantees device servicing regardless of InputProcess
        // starvation. share g_input_lock so the two pollers never reenter the
        // 8042 / backdoor mid-transaction on different cores. (satoru)
        {
            SpinLockCpuGuard guard(g_input_lock);
            Keyboard::Poll();
            Mouse::Poll();
            USB::PollHID();   // inputprocess starves here for usb devices too (satoru)
        }

        // The network process starves under firefox as well (same cooperative
        // class as the input process above). the gui is the one kernel process
        // that always runs, so it services the nic + dhcp at 10hz and lets the
        // wifi reconnect tick at 1hz. the dhcp state machine has its own lock,
        // and the nic drain takes g_net_lock exactly like the network process.
        // (satoru)
        {
            static uint32_t s_net_svc_ms = 0;
            uint32_t nnow = Timer::GetTicks();
            if ((uint32_t)(nnow - s_net_svc_ms) >= 100u) {
                s_net_svc_ms = nnow;
                if (TCPStack::IsUp()) {
                    SpinLockCpuGuard guard(g_net_lock);
                    TCPStack::Tick();
                } else if (E1000::IsDetected()) {
                    SpinLockCpuGuard guard(g_net_lock);
                    E1000::Poll();
                }
                LinuxNetBridge::WakeRxPollers();
                Dhcp::Tick();
                LinuxNetBridge::RefreshFromKernel();
                static uint32_t s_wifi_svc_ms = 0;
                if ((uint32_t)(nnow - s_wifi_svc_ms) >= 1000u) {
                    s_wifi_svc_ms = nnow;
                    WiFi::Tick();
                }
            }
        }

        // Track cursor movement: any pointer motion marks the UI dirty so the
        // cursor stays responsive even on an otherwise static screen. when the
        // intel hardware cursor plane takes the move (kurono.igpu.accel=1) no
        // frame is needed at all; hover/button/scroll still mark dirty below.
        // (satoru)
        static int last_mx = -1, last_my = -1;
        if (Mouse::mx != last_mx || Mouse::my != last_my) {
            if (!IntelAccel::CursorUpdate(Mouse::mx, Mouse::my)) {
                Graphics::MarkUIDirty();
                // software cursor: damage the old and the new footprint before
                // the frame renders, so the wallpaper/windows repaint under the
                // arrow at both places (the damage-gated present no longer
                // re-blits the whole wallpaper to erase the old one). (satoru)
                if (last_mx >= 0) Mouse::DamageCursor(last_mx, last_my);
                Mouse::DamageCursor(Mouse::mx, Mouse::my);
            }
            last_mx = Mouse::mx;
            last_my = Mouse::my;
        }

        // while the ksa modal is up, the devices (usb included) still feed the
        // same rings, but the modal consumes them instead of the desktop. the
        // desktop keeps compositing underneath us and stays live. (satoru)
        bool modal = KSA::ModalActive();
        if (modal) {
            KSA::PumpPromptInput();
            Graphics::MarkUIDirty();   // hover/typed echo/cursor move this frame (satoru)
        } else {
            // Drain mouse events.
            int scroll_delta = 0;
            while (Mouse::HasEvent()) {
                Mouse::Event mevt = Mouse::GetEvent();
                Graphics::MarkUIDirty();             // any button/scroll changes state
                if (mevt.type == 3) { scroll_delta += mevt.dz; continue; }
                if (mevt.type == 1 || mevt.type == 2) {
                    WindowManager::HandlePointerButton(mevt.x, mevt.y,
                        (int)mevt.button, mevt.type == 1);
                }
            }

            bool mouse_clicked = Mouse::LeftClicked();
            bool mouse_down = Mouse::IsLeftDown();
            if (mouse_clicked || mouse_down) Graphics::MarkUIDirty();
            char kb_char = 0;
            if (Keyboard::HasChar()) { kb_char = Keyboard::GetChar(); Graphics::MarkUIDirty(); }
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my,
                                            mouse_down, mouse_clicked, kb_char);
            while (Keyboard::HasChar()) {
                kb_char = Keyboard::GetChar();
                Graphics::MarkUIDirty();
                // pass the REAL held button state, NOT false: HandleInput edge-
                // detects mouse_down to forward wl_pointer button press/release, so
                // a keyboard-drain call with a hardcoded false fired a spurious
                // RELEASE mid-hold, then the next frame re-fired a DOWN = a single
                // physical click delivered to firefox as a DOUBLE click (which on
                // the tab strip opens a new tab). (satoru)
                DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, Mouse::IsLeftDown(), false, kb_char);
            }

            if (scroll_delta != 0) {
                Graphics::MarkUIDirty();
                // scroll goes to the surface under the pointer (see the matching
                // note in KuronoShell::PumpUI). (satoru)
                WaylandServer::ForwardPointerAxis(Mouse::mx, Mouse::my, 0, scroll_delta * 32);
                Window* fw = WindowManager::GetFocusedWindow();
                if (fw && fw->input && !WaylandServer::IsWaylandWindow(fw->id)) {
                    { TaskMem::WindowScope _tms(fw->id); fw->input(fw, 3, scroll_delta, 0); }
                }
            }
        }

        if (g_gui_autorun_armed && (int32_t)(Timer::GetTicks() - autorun_target_ms) >= 0) {
            g_gui_autorun_armed = false;
            // sentinel "@taskmgr": open the task manager instead of the terminal
            // (headless ui-debug harness for the task manager overhaul). (satoru)
            const char* cmd = g_gui_autorun_cmd;
            bool open_taskmgr = (cmd[0]=='@' && cmd[1]=='t' && cmd[2]=='a' && cmd[3]=='s' &&
                                 cmd[4]=='k' && cmd[5]=='m' && cmd[6]=='g' && cmd[7]=='r' && cmd[8]==0);
            // sentinel "@firefox": launch through the desktop's detached path
            // (no terminal window) - the same flow the desktop icon uses. (satoru)
            bool open_firefox = (cmd[0]=='@' && cmd[1]=='f' && cmd[2]=='i' && cmd[3]=='r' &&
                                 cmd[4]=='e' && cmd[5]=='f' && cmd[6]=='o' && cmd[7]=='x' && cmd[8]==0);
            // sentinel "@settings" or "@settings:<page id>": open settings on a page
            // (headless ui harness for the settings redesign). (satoru)
            bool open_settings = (cmd[0]=='@' && cmd[1]=='s' && cmd[2]=='e' && cmd[3]=='t' &&
                                  cmd[4]=='t' && cmd[5]=='i' && cmd[6]=='n' && cmd[7]=='g' &&
                                  cmd[8]=='s' && (cmd[9]==0 || cmd[9]==':'));
            if (open_settings) {
                SerialLogger::Log("[gui-autorun] launching settings\r\n");
                SystemSettings::OpenPage(cmd[9]==':' ? cmd + 10 : nullptr);
            } else if (open_taskmgr) {
                SerialLogger::Log("[gui-autorun] launching task manager\r\n");
                DesktopEnvironment::LaunchTaskManager();
            } else if (open_firefox) {
                SerialLogger::Log("[gui-autorun] launching firefox (detached, no terminal)\r\n");
                DesktopEnvironment::LaunchFirefox();
            } else {
                SerialLogger::Log("[gui-autorun] launching terminal + queuing: ");
                SerialLogger::Log(g_gui_autorun_cmd);
                SerialLogger::Log("\r\n");
                TerminalApp::Open();
                TerminalApp::EnqueueCommand(g_gui_autorun_cmd);
            }
            // test hook: kurono.guiterms=N opens N more terminal windows (the last one focused),
            // in every autorun mode so a detached firefox run can still get a shell. (satoru)
            for (int ti = 0; ti < g_gui_extra_terms; ti++) TerminalApp::Open();
        }

        if (g_term_test && !g_gui_autorun_armed) {
            static uint32_t s_tt_at = 0;
            if (!s_tt_at) s_tt_at = Timer::GetTicks() + 8000;
            if ((int32_t)(Timer::GetTicks() - s_tt_at) >= 0) {
                g_term_test = false;
                SerialLogger::Log("[termtest] opening a second terminal\r\n");
                TerminalApp::Open();
                TerminalApp::EnqueueCommand("help");
            }
        }
        if (g_ffreopen_left > 0 && !g_gui_autorun_armed) {
            static uint64_t s_reopen_next = 0;
            uint64_t tnow = Timer::GetTicks();
            if (s_reopen_next == 0) s_reopen_next = tnow + 5000;
            if ((int64_t)(tnow - s_reopen_next) >= 0) {
                s_reopen_next = tnow + 3000;
                if (RunShellCommandDetached("firefox")) {
                    g_ffreopen_left--;
                    SerialLogger::Log("[ffreopen] relaunch, left ");
                    SerialLogger::LogDec(g_ffreopen_left);
                    SerialLogger::Log("\r\n");
                }
            }
        }
        DesktopEnvironment::Update();
        wp_test_tick();

        if (DesktopEnvironment::ConsumeLogoutRequest()) {
            SerialLogger::Log("[Session] Logout requested\r\n");
            WindowManager::CloseAll();
            UserManager::Logout();
            LockScreen::Show();
            Keyboard::FlushBuffers();
            Mouse::SetAutoDraw(false);
            // the lock ui owned the scanned buffer: repaint everything. (satoru)
            Graphics::MarkDirty(0, 0, Graphics::GetWidth(), Graphics::GetHeight());
            Graphics::MarkUIDirty();
            ks.leave();
            Scheduler::YieldNow();
            continue;
        }

        // one-shot interactive ksa prompt demo (kurono.ksa.prompt). fire once the
        // desktop has presented enough frames that the modal lands over a real
        // desktop. KSA::Prompt blocks this loop until the user (or synthetic
        // input) answers - that block IS the secure desktop: the gui compositor
        // and input process are starved while ksa owns the screen + input. when
        // it returns, the saved desktop is restored and the loop repaints. (satoru)
        if (g_ksa_prompt_demo_armed && frame_counter > 90) {
            g_ksa_prompt_demo_armed = false;
            SerialLogger::Log("[gui] firing interactive ksa prompt demo\r\n");
            ks.leave();   // the prompt waits on input (satoru)
            KSA::PromptDemo(g_ksa_prompt_demo_cred);
            Graphics::MarkUIDirty();
            continue;   // repaint the desktop on the next iteration (satoru)
        }

        // one-shot kj-scripted animation demo (kurono.kjdemo): once the desktop
        // has presented a few seconds of frames, run the shipped accent-animation
        // script. it drives kss.transition + kss.set on the "window" rule's accent
        // (which feeds the live window-border/titlebar accent) so the on-screen
        // accent eases - proving the js path drives real animation - then posts a
        // toast via ui.notify. prefer the shipped file; fall back to inline. (satoru)
        if (g_kj_demo_armed && frame_counter > 120) {
            g_kj_demo_armed = false;
            SerialLogger::Log("[gui] running kj-scripted animation demo\r\n");
            static char kj_out[512];
            int n = KJ::ExecFile("/kurono/scripts/accent_anim.kj", kj_out, sizeof(kj_out));
            if (n <= 0 || kj_out[0] == 0) {
                // fallback: same effect inline if the script file isn't present. (satoru)
                static const char* SRC =
                    "kss.transition('window','accent', 600, 'outcubic');\n"
                    "kss.set('window','accent', 4292032130);\n"   // 0xFFE74C42 warm red (satoru)
                    "ui.notify('KJ animation', 'accent eased by a kj script');\n"
                    "console.log('kjdemo: accent transition fired');\n";
                KJ::Execute(SRC, kj_out, sizeof(kj_out));
            }
            SerialLogger::Log("[kjdemo] ");
            SerialLogger::Log(kj_out);
            SerialLogger::Log("\r\n");
            Graphics::MarkUIDirty();
            continue;
        }

        // every ~5 s of wall time, not every 300 frames: at 240 hz a frame
        // count would refresh 4x as often (and cost 4x the cpu). (satoru)
        {
            static uint32_t s_tm_refresh_ms = 0;
            uint32_t tm_now = Timer::GetRealMs();
            if (s_tm_refresh_ms == 0 || (uint32_t)(tm_now - s_tm_refresh_ms) >= 5000u) {
                s_tm_refresh_ms = tm_now;
                TaskManagerApp::RefreshProcesses();
            }
        }
        if (frame_counter == 300) KernelHeap::LogMemReport("desktop");

        // Frame pacing to ~60fps. Timer::GetRealMs() is now sourced from the
        // PIT-IRQ scheduler clock (see timer.cpp GetRealMs), so it advances
        // reliably regardless of poll cadence - fixing the FPS-0 freeze where
        // a SleepMs(1) pacer starved the old *polled* clock (it lost whole PIT
        // periods between calls, so the 16ms threshold was never reached and
        // the loop slept forever). We sleep the WHOLE remaining budget in one
        // shot instead of 1ms spins, avoiding ~16 context switches per frame.
        // (satoru)
        // ── Damage + adaptive pace gate ───────────────────────────────
        // Decide whether anything needs drawing, THEN pace. Render only when
        // (a) the UI was marked dirty, (b) an animation/video/popup is active,
        // or (c) the ~250ms safety fallback is due (drives the clock/blink and
        // bounds any missed dirty source to a 250ms delay, never a permanent
        // freeze). (satoru)
        // the fallback is a safety net, not a refresh rate: keep it to at most
        // ~20% of the cpu by spacing it at 4x the last frame's cost. on real
        // hardware a frame is a few ms and this stays 250ms; under emulation a
        // full frame can take ~300ms, and a fixed 250ms fallback kept an idle
        // desktop drawing 100% of the time, starving apps on the same core.
        static uint32_t s_frame_cost_ms = 0;
        // per-phase cost counters for the [gui] line: tsc cycles spent in
        // Render() and in SwapBuffers()+present over the 60-frame window (satoru)
        static uint64_t s_prof_render = 0, s_prof_swap = 0, s_prof_tsc0 = 0;
        static uint32_t s_prof_frames = 0;
        const uint32_t FALLBACK_MS = s_frame_cost_ms * 4 > 250u ? s_frame_cost_ms * 4 : 250u;
        // advance the kss tween engine first so animated values progress and the
        // Active() check inside ui_activity_active() reflects in-flight tweens. then
        // advance the stylesheet keyframe layer against that same clock. (satoru)
        KSS::Anim::Tick(Timer::GetRealMs());
        KSS::Sheet::Tick();
        DenjiApp::BackgroundTick();   // media player: closed-window cleanup, audio feed, next frame (satoru)
        ff_main_ring_dump();   // firefox main-thread syscall ring, self-paced to 20s
        bool dirty    = Graphics::ConsumeUIDirty();
        if (Graphics::ConsumeFrameDue(Timer::GetRealMs())) dirty = true;
        bool active   = ui_activity_active();
        bool fallback = (Timer::GetRealMs() - last_render_ms) >= FALLBACK_MS;
        // the modal is composited every frame (hover/typed echo/mouse cursor)
        // and the desktop behind it keeps animating. (satoru)
        if (KSA::ModalActive()) { dirty = true; active = true; }
        // kurono.fpsbench=1: composite every iteration and skip the frame
        // pacer, so [fps] measures the RENDER+PRESENT PIPELINE instead of the
        // damage gate. without this an idle desktop legitimately reports ~4fps
        // (the 250ms fallback) and any rendering change looks like noise. bench
        // only - it deliberately pegs a core. (satoru)
        if (g_fps_bench) { dirty = true; active = true; }
        // every 60 rendered frames, say what drove them: how many were dirty,
        // animation-driven (and by what), or the 250ms fallback. a desktop that
        // should be idle but renders flat out shows up here.
        {
            static uint32_t n_dirty = 0, n_active = 0, n_fallback = 0, t0 = 0;
            if (dirty || active || fallback) {
                if (dirty) n_dirty++; else if (active) n_active++; else n_fallback++;
                // report once at least 60 frames AND ~1 s have gone by: at 60 hz
                // that is the old "60 frames in ~1000ms" line, at 240 hz it is one
                // line per second instead of four (each line costs ~3 ms of
                // busy-waiting uart, more than half a 240 hz frame). (satoru)
                uint32_t n_tot = n_dirty + n_active + n_fallback;
                uint32_t now = Timer::GetRealMs();
                if (n_tot >= 60 && ((uint32_t)(now - t0) >= 1000u || n_tot >= 2000u)) {
                    SerialLogger::Log("[gui] "); SerialLogger::LogDec((int)n_tot);
                    SerialLogger::Log(" frames in "); SerialLogger::LogDec((int)(now - t0));
                    SerialLogger::Log("ms last_cost="); SerialLogger::LogDec((int)s_frame_cost_ms);
                    {
                        uint64_t tsc_now = __builtin_ia32_rdtsc();
                        uint64_t span_ms = (uint64_t)(now - t0);
                        uint64_t cyc_us = (span_ms && s_prof_tsc0) ? (tsc_now - s_prof_tsc0) / (span_ms * 1000u) : 0;
                        uint32_t nf = s_prof_frames ? s_prof_frames : 1;
                        SerialLogger::Log("ms render_us="); SerialLogger::LogDec(cyc_us ? (int)(s_prof_render / cyc_us / nf) : 0);
                        SerialLogger::Log(" swap_us="); SerialLogger::LogDec(cyc_us ? (int)(s_prof_swap / cyc_us / nf) : 0);
                        s_prof_render = s_prof_swap = 0; s_prof_frames = 0; s_prof_tsc0 = tsc_now;
                        // hardware cursor frame accounting, only while the intel
                        // accel stage is enabled (kurono.igpu.accel=1), so the
                        // flag-off log line is byte for byte the old one. (satoru)
                        if (IntelAccel::Enabled()) {
                            uint32_t ahw = 0, asw = 0, afail = 0;
                            uint64_t acyc = IntelAccel::TakeGuiStats(&ahw, &asw, &afail);
                            SerialLogger::Log(" accel=cursor hw="); SerialLogger::LogDec((int)ahw);
                            SerialLogger::Log(" sw="); SerialLogger::LogDec((int)asw);
                            SerialLogger::Log(" fail="); SerialLogger::LogDec((int)afail);
                            SerialLogger::Log(" cur_us="); SerialLogger::LogDec(cyc_us ? (int)(acyc / cyc_us / nf) : 0);
                        }
                        // stage 2 counters, only while zero-copy is live, so
                        // every other log line stays byte identical. (satoru)
                        if (IntelAccel::ZeroCopyActive()) {
                            uint32_t zc_commits = 0, zc_fails = 0;
                            IntelAccel::TakeZeroCopyStats(&zc_commits, &zc_fails);
                            SerialLogger::Log(" zc=on commits="); SerialLogger::LogDec((int)zc_commits);
                            SerialLogger::Log(" zcfail="); SerialLogger::LogDec((int)zc_fails);
                        }
                    }
                    SerialLogger::Log(" dirty="); SerialLogger::LogDec((int)n_dirty);
                    SerialLogger::Log(" active="); SerialLogger::LogDec((int)n_active);
                    SerialLogger::Log(" why="); SerialLogger::LogDec(g_active_why);
                    SerialLogger::Log(" fallback="); SerialLogger::LogDec((int)n_fallback);
                    SerialLogger::Log(" hz="); SerialLogger::LogDec((int)FramePacer::CurrentHz());
                    SerialLogger::Log(" lastdirty=0x");
                    SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::LastUIDirtyCaller());
                    SerialLogger::Log("\r\n");
                    n_dirty = n_active = n_fallback = 0; t0 = now;
                }
            }
        }
        if (dirty || active) last_activity_ms = Timer::GetRealMs();
        if (!dirty && !active && !fallback) {
            // Nothing to draw. The old code BUSY-YIELDED here while "recently
            // active" to dodge WHPX/VMware timer coalescing (which made SleepMs
            // wake ~500ms late). but that spin pegged the vcpu at 100% the whole
            // time the desktop sat idle - the dev is on KVM now, where the timer
            // IRQ is NOT coalesced, so a 1ms sleep wakes on time AND frees the
            // core (fixes the "whole os is laggy / fan spins" feel). after ~2s of
            // true inactivity, sleep longer to idle the host core fully. if you
            // ever run under WHPX again, bump this back to YieldNow(). (satoru)
            ks.leave();
            if (Timer::GetRealMs() - last_activity_ms < 2000u) Scheduler::SleepMs(1u);
            else Scheduler::SleepMs(16u);
            g_pace_streak = false;   // an idle gap: the next frame is not "late" (satoru)
            continue;
        }
        // There IS work. Pace to ~60fps WITHOUT halting: SleepMs() HLTs the
        // cpu, and WHPX/VMware COALESCE the timer IRQ while the guest is
        // halted, so a 16ms sleep actually lasts ~55ms -> a choppy ~18fps
        // during interaction. Busy-yield instead - the loop wakes on the TSC
        // clock, not the coalesced IRQ, giving smooth 60fps while active. Idle
        // frames above still HLT, so host cpu stays low when nothing happens.
        // (satoru)
        // pace to the SELECTED refresh rate (display.refresh_hz -> Graphics
        // target fps), not a hardcoded 60; busy-yield on the tsc clock so
        // whpx/vmware timer coalescing can't stretch the frame. (satoru)
        // modes 1 and 3 are UNCAPPED (max throughput); mode 2 keeps the 60fps
        // pacer. mode 3 additionally forces a full-screen transfer. (satoru)
        // the period comes from FramePacer in exact nanoseconds on a fixed
        // deadline grid (deadline += period), not a whole-ms budget re-armed
        // from "now": 1000/240 truncated to 4 ms gave 250 fps, 144 gave 166,
        // and the cadence jittered between 4 and 5 ms. a frame that is more
        // than a period behind (or the first after an idle gap) re-anchors the
        // grid instead of bursting to catch up. (satoru)
        uint64_t period_ns = (g_fps_bench == 1 || g_fps_bench == 3)
                                 ? 0ull : FramePacer::PeriodNs();
        bool pace_continuous = g_pace_streak;
        {
            uint64_t now_ns = FramePacer::NowNs();
            // first frame after idle: anchor the grid here, but never closer
            // than one period after the previous frame started, so input that
            // trickles in between short idle sleeps cannot beat the cap. (satoru)
            if (g_pace_due_ns == 0 || !pace_continuous) {
                uint64_t earliest = g_pace_last_start_ns + period_ns;
                g_pace_due_ns = (g_pace_last_start_ns && earliest > now_ns) ? earliest : now_ns;
            }
            ks.leave();
            while (period_ns && FramePacer::NowNs() < g_pace_due_ns) Scheduler::YieldNow();
            ks.enter();
        }
        uint64_t frame_start_ns = FramePacer::NowNs();
        g_pace_last_start_ns = frame_start_ns;
        uint64_t frame_late_ns = (pace_continuous && frame_start_ns > g_pace_due_ns)
                                     ? frame_start_ns - g_pace_due_ns : 0;
        // next deadline: one period on from this one, or from now if this
        // frame was already more than a period late. (satoru)
        if (frame_late_ns > period_ns) g_pace_due_ns = frame_start_ns + period_ns;
        else g_pace_due_ns += period_ns;
        last_render_ms = Timer::GetRealMs();

        // (satoru) no full-screen clear here: the damage list already repaints
        // the moved window's old and new footprints, and with zero-copy active
        // this clear wrote over the buffer the panel was scanning, costing a
        // full 5120x1600 fill + present on every drag frame (the ~15 hz drag).
        // Framebuffer access - protected so any concurrent E1000 RX
        // path or audio mixer scratch doesn't interleave a SwapBuffers
        // mid-DrawPixel.
        {
            SpinLockCpuGuard guard(g_fb_lock);
            uint64_t prof_a = __builtin_ia32_rdtsc();
            // while the boot scene covers the screen the desktop is not
            // composed; it is back before the reveal needs it (satoru)
            if (!BootAnim::CoversScreen()) DesktopEnvironment::Render();
            // modal on top of the live desktop, before the cursor + swap. (satoru)
            if (KSA::ModalActive()) KSA::RenderPromptOverlay();
            s_prof_render += __builtin_ia32_rdtsc() - prof_a;

            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            static bool show_fps = false;
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
                show_fps = UIConfig::Bool("ui.fps", false);
                // the 1hz [fps] serial mirror is for the fpsbench harness
                // only, so a default boot stays quiet. (satoru)
                if (g_fps_bench) {
                    // mirror the framerate to serial. it was screen-only, so a
                    // headless boot (which is how every harness here measures)
                    // could not see the compositor's framerate at all - making any
                    // rendering/acceleration change unmeasurable. NOTE this counts
                    // COMPOSITED frames and the loop is damage-gated, so an idle
                    // desktop legitimately reports a low number: it is "frames
                    // actually painted", not a benchmark of the pipeline. throttled
                    // to 1hz by the enclosing branch. (satoru)
                    SerialLogger::Log("[fps] ");
                    SerialLogger::LogDec((int)displayed_fps);
                    SerialLogger::Log(" frames/s t=");
                    SerialLogger::LogDec((int)(now_ms / 1000u));
                    SerialLogger::Log("s\r\n");
                }
            }

            // fps pill, off unless ui.fps=on (f12 hud shows it too) (satoru)
            if (show_fps) {
                char fps_str[16] = "FPS ";
                char num_buf[8];
                int val = (int)displayed_fps;
                if (val == 0) { num_buf[0] = '0'; num_buf[1] = 0; }
                else {
                    char tmp[8]; int n = 0;
                    while (val > 0 && n < 7) { tmp[n++] = '0' + (val % 10); val /= 10; }
                    for (int i = 0; i < n; i++) num_buf[i] = tmp[n - 1 - i];
                    num_buf[n] = 0;
                }
                int si = 4;
                for (int i = 0; num_buf[i] && si < 14; i++) fps_str[si++] = num_buf[i];
                fps_str[si] = 0;
                int sw = Graphics::GetWidth();
                int pill_w = si * 8 + 16;
                int tx = sw - pill_w - 6;
                int py = 6 + Taskbar::TopInset();    // below the modern menu bar (satoru)
                Graphics::FillRoundedRect(tx, py, pill_w, 22, 11, 0xB0101020);
                Graphics::DrawString(tx + 8, py + 4, fps_str, 0xFF00E676, 0xFF000000);
            }

            // performance hud overlay (toggle with f12). (satoru)
            PerfHUD::Render(displayed_fps, displayed_fps ? 1000u / displayed_fps : 0u);
            // settings > displays: identify numbers + the keep-settings countdown (satoru)
            DisplaySettings::Overlay();

            uint64_t prof_b = __builtin_ia32_rdtsc();
            Mouse::DrawAt(Mouse::mx, Mouse::my);
            BootAnim::Composite();   // no-op once the boot animation is done (satoru)
            Graphics::SwapBuffers();
            Graphics::PresentVirtioIfActive();   // push the frame to the host gpu when accelerated (no-op otherwise) (satoru)
            UiDiag::NotePresent();
            s_prof_swap += __builtin_ia32_rdtsc() - prof_b;
            s_prof_frames++;
        }
        s_frame_cost_ms = Timer::GetRealMs() - last_render_ms;
        UiDiag::MaybeLog(WindowManager::IsDragging());
        if (period_ns) FramePacer::OnFrame(FramePacer::NowNs() - frame_start_ns, frame_late_ns, pace_continuous);
        g_pace_streak = true;
        // animation wall-clock probe: how long a window-manager animation
        // (open/close/minimize springs) stayed active and how many frames it
        // got. the springs run on the ms clock, so active_ms must match at 60
        // and 240 hz while frames scale with the rate. (satoru)
        {
            static bool s_anim_on = false;
            static uint32_t s_anim_t0 = 0, s_anim_frames = 0, s_anim_logs = 0;
            bool wm_anim = WindowManager::HasActiveAnimations();
            uint32_t an_now = Timer::GetRealMs();
            if (wm_anim && !s_anim_on) { s_anim_on = true; s_anim_t0 = an_now; s_anim_frames = 0; }
            if (s_anim_on) s_anim_frames++;
            if (!wm_anim && s_anim_on) {
                s_anim_on = false;
                if (s_anim_logs < 64) {
                    s_anim_logs++;
                    SerialLogger::Log("[anim] wm active_ms=");
                    SerialLogger::LogDec((int)(an_now - s_anim_t0));
                    SerialLogger::Log(" frames=");
                    SerialLogger::LogDec((int)s_anim_frames);
                    SerialLogger::Log(" hz=");
                    SerialLogger::LogDec((int)FramePacer::CurrentHz());
                    SerialLogger::Log("\r\n");
                }
            }
        }

        // Hand the CPU back so the other tiers progress.
        ks.leave();
        Scheduler::YieldNow();
    }
}

// watchdog: every 5 s, verify the graphics main loop ticked within the last
// 3 s. on a stall, log to serial and attempt to reinitialize the display
// backend. a healthy gui loop refreshes g_last_frame_ms every frame, so this
// never fires under normal operation. (satoru)
[[noreturn]] static void WatchdogProcessEntry() {
    SerialLogger::Log("[watchdog] online\r\n");
    int consecutive = 0;
    for (;;) {
        Scheduler::SleepMs(5000);
        uint32_t last = g_last_frame_ms;
        if (last == 0) { consecutive = 0; continue; } // gui loop not started yet (satoru)
        uint32_t now = Timer::GetRealMs();
        // only treat the gui as truly hung after a LONG (>8s) gap confirmed
        // across two consecutive checks (~13s total). the old 3s/one-shot rule
        // tripped on transient boot/heavy-frame slowness, and then reinit'd the
        // display WITHOUT g_fb_lock - racing the gui's render, tearing the
        // framebuffer state, and cascading into more stalls + reinits. (satoru)
        if (now > last && (now - last) > 8000) {
            if (++consecutive >= 2) {
                SerialLogger::Log("[watchdog] graphics loop hung >13s; reinitializing display (satoru)\r\n");
                // hold g_fb_lock so the reinit never runs concurrently with a
                // render in progress. (satoru)
                {
                    SpinLockCpuGuard guard(g_fb_lock);
                    DisplayManager::Init();
                }
                g_last_frame_ms = Timer::GetRealMs();
                consecutive = 0;
            }
        } else {
            consecutive = 0;
        }
    }
}

int SpawnAll() {
    // Order matters: REALTIME first so it wins ties on tier scan.
    int created = 0;
    if (Scheduler::SpawnKernelProcess("timekeeper", TimekeeperProcessEntry,
                                      PRIO_REALTIME, 64, 1024)) created++;
    if (Scheduler::SpawnKernelProcess("scheduler", SchedulerProcessEntry,
                                      PRIO_REALTIME, 64, 1024)) created++;
    if (Scheduler::SpawnKernelProcess("network",   NetworkProcessEntry,
                                      PRIO_HIGH,   64,  2048)) created++;
    if (Scheduler::SpawnKernelProcess("input",     InputProcessEntry,
                                      PRIO_HIGH,   64,  2048)) created++;
    if (Scheduler::SpawnKernelProcess("audio",     AudioProcessEntry,
                                      PRIO_HIGH,   128, 4096)) created++;
    if (Scheduler::SpawnKernelProcess("gui",       GUIProcessEntry,
                                      PRIO_NORMAL, 1024, 32 * 1024)) created++;
    if (Scheduler::SpawnKernelProcess("shell",     ShellProcessEntry,
                                      PRIO_NORMAL, 512, 8192)) created++;
    if (Scheduler::SpawnKernelProcess("logging",   LoggingProcessEntry,
                                      PRIO_LOW,    64,  2048)) created++;
    if (Scheduler::SpawnKernelProcess("watchdog",  WatchdogProcessEntry,
                                      PRIO_LOW,    64,  2048)) created++;
    if (Scheduler::DetachedLaunch()) {
        // same stack budget as the shell: these run KuronoShell::Execute. (satoru)
        if (Scheduler::SpawnKernelProcess("launcher0", LauncherProcessEntry<0>, PRIO_NORMAL, 512, 8192)) created++;
        if (Scheduler::SpawnKernelProcess("launcher1", LauncherProcessEntry<1>, PRIO_NORMAL, 512, 8192)) created++;
        if (Scheduler::SpawnKernelProcess("launcher2", LauncherProcessEntry<2>, PRIO_NORMAL, 512, 8192)) created++;
        if (Scheduler::SpawnKernelProcess("launcher3", LauncherProcessEntry<3>, PRIO_NORMAL, 512, 8192)) created++;
        if (Scheduler::SpawnKernelProcess("launcher4", LauncherProcessEntry<4>, PRIO_NORMAL, 512, 8192)) created++;
        if (Scheduler::SpawnKernelProcess("launcher5", LauncherProcessEntry<5>, PRIO_NORMAL, 512, 8192)) created++;
        if (g_kstack_test) Scheduler::SpawnKernelProcess("kt-overflow", KstackTestEntry, PRIO_LOW, 64, 64);
        if (g_term_workers) {
            if (Scheduler::SpawnKernelProcess("termworker0", TermWorkerEntry<0>, PRIO_NORMAL, 512, 8192)) created++;
            if (Scheduler::SpawnKernelProcess("termworker1", TermWorkerEntry<1>, PRIO_NORMAL, 512, 8192)) created++;
            if (Scheduler::SpawnKernelProcess("termworker2", TermWorkerEntry<2>, PRIO_NORMAL, 512, 8192)) created++;
            if (Scheduler::SpawnKernelProcess("termworker3", TermWorkerEntry<3>, PRIO_NORMAL, 512, 8192)) created++;
        }
    }
    return created;
}

} // namespace KernelProcesses
