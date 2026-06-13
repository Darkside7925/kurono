// Kurono OS  -  kernel process registry implementation
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
#include "../kernel/hrtimer.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/ac97.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../drivers/usb.h"
#include "../drivers/e1000.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../system/input_manager.h"
#include "../system/logging.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../ui/gui.h"   // GUI::UpdateBackbuffer for the resolution-change relayout (satoru)
#include "../ui/perf_hud.h"
#include "../ui/lockscreen.h"
#include "../system/user_mgmt.h"
#include "../apps/terminal.h"
#include "../apps/task_manager.h"
#include "../apps/settings.h"
#include "../ui/control_center.h"
#include "../ui/kss.h"
#include "../ui/notification.h"
#include "../apps/media_player.h"
#include "../apps/denji_app.h"
#include "../security/ksa.h"   // one-shot interactive prompt demo (kurono.ksa.prompt) (satoru)

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
}

void ArmKsaPromptDemo(bool want_cred) {
    g_ksa_prompt_demo_cred  = want_cred;
    g_ksa_prompt_demo_armed = true;
}

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
        {
            SpinLockCpuGuard guard(g_net_lock);
            if (TCPStack::IsUp()) {
                TCPStack::Tick();
            } else if (E1000::IsDetected()) {
                E1000::Poll();
            }
        }
        Scheduler::SleepMs(10);   // RX latency-tolerant; 1ms over-polled an idle NIC (satoru)
    }
}

// ── Input process: keyboard / mouse / input manager polling ───────────
[[noreturn]] static void InputProcessEntry() {
    SerialLogger::Log("[InputProcess] online\r\n");
    while (true) {
        {
            SpinLockCpuGuard guard(g_input_lock);
            Keyboard::Poll();
            Mouse::Poll();
            USB::PollHID();   // poll usb hid interrupt-in endpoints (satoru)
            InputManager::Poll();
        }
        Scheduler::SleepMs(8);   // 125Hz: ample for human input, 8x fewer 8042/backdoor VM-exits (satoru)
    }
}

// ── Audio process: AC97 / mixer pump ──────────────────────────────────
[[noreturn]] static void AudioProcessEntry() {
    SerialLogger::Log("[AudioProcess] online\r\n");
    while (true) {
        {
            SpinLockCpuGuard guard(g_audio_lock);
            // AudioServer::Tick() already pumps the ACTIVE backend (it calls
            // be->Tick() via the mixer), so the direct AC97::Tick()/Audio::Tick()
            // were redundant per-tick MMIO/PIO (extra VM-exits on VMware)  -  dropped.
            AudioServer::Tick();
        }
        // 10ms: the mixer period is ~21ms (1024 frames @ 48kHz) with a ~170ms
        // back-pressure buffer, so 4ms was ~4x over-polling  -  pure wakeups/exits
        // that kept the cpu from ever idling (HLT) on VMware. (satoru)
        Scheduler::SleepMs(10);
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
        HRTimer::Tick();
        Scheduler::ServiceSleepQueue();
        // 50ms: IRQ0 already calls wake_due_processes() every tick, so this is a
        // backup heartbeat. 1ms was 1000 redundant wakeups/sec that kept the cpu
        // from idling (HLT)  -  the main VMware host-CPU burn. (satoru)
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
        // Tickling MirrorSerial with an empty string is enough to flush
        // pending append paths without injecting noise into the log.
        {
            SpinLockCpuGuard guard(g_log_lock);
            RuntimeLog::MirrorSerial("");
        }
        Scheduler::SleepMs(500);
    }
}

// ── Shell process: drains the queued command from TerminalApp ─────────
//
//   `TerminalApp::Tick()` performs the actual KuronoShell::Execute call
//   when a command is pending.  Decoupling that from the GUI thread
//   means `kpkg sync` / `ping 8.8.8.8` no longer freezes the desktop  - 
//   the GUI keeps rendering at full FPS while we churn.
[[noreturn]] static void ShellProcessEntry() {
    SerialLogger::Log("[ShellProcess] online\r\n");
    while (true) {
        TerminalApp::Tick();
        Scheduler::SleepMs(25);   // drains an almost-always-empty command queue; 2ms was 500 idle wakeups/sec (satoru)
    }
}

namespace {
// Is anything that must keep rendering continuously currently live?
// While ANY of these holds, the damage-gated loop renders every frame so a
// missed MarkUIDirty() can never freeze an in-flight animation or video.
// Conservative by design: when unsure, return true (render). (satoru)
static inline bool ui_activity_active() {
    if (WindowManager::IsDragging())            return true;  // live drag/resize
    if (WindowManager::HasActiveAnimations())   return true;  // open/close/min/max
    if (Taskbar::IsAnimating())                 return true;  // menus/popups/cursor
    if (Desktop::IsAnimating())                 return true;  // icon hover-pop fade
    if (ControlCenter::IsAnimating())           return true;  // panel slide+fade
    if (KSS::Anim::Active())                     return true;  // kss tween engine in flight
    if (KSS::Sheet::Active())                    return true;  // kss keyframe track playing
    if (NotificationManager::ActiveCount() > 0) return true;  // toasts in/hold/out
    if (MediaPlayerApp::IsOpen())               return true;  // audio/video surface
    if (DenjiApp::IsOpen())                     return true;  // kvid playback frames
    if (TaskManagerApp::IsOpen())               return true;  // live perf graphs/list
    return false;
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

    Mouse::SetAutoDraw(false);

    uint32_t frame_counter = 0;
    uint32_t fps_counter = 0;
    uint32_t last_fps_ms = Timer::GetRealMs();
    uint32_t displayed_fps = 0;
    uint32_t last_render_ms = 0;
    uint32_t last_activity_ms = Timer::GetRealMs();   // last input/animation (satoru)
    uint32_t autorun_target_ms = Timer::GetTicks() + 4000u;

    while (true) {
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
        { static int __sw = -1, __sh = -1;
          int cw = Graphics::GetWidth(), ch = Graphics::GetHeight();
          if (cw != __sw || ch != __sh) {
              SerialLogger::Log("[res-sync] relayout to framebuffer mode\r\n");
              GUI::UpdateBackbuffer();
              DesktopEnvironment::Init(cw, ch);
              Graphics::MarkUIDirty();
          }
          __sw = cw; __sh = ch; }

        // Advance system wall-clock time once per frame.
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);

        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // Track cursor movement: any pointer motion marks the UI dirty so the
        // cursor stays responsive even on an otherwise static screen. (satoru)
        static int last_mx = -1, last_my = -1;
        if (Mouse::mx != last_mx || Mouse::my != last_my) {
            Graphics::MarkUIDirty();
            last_mx = Mouse::mx;
            last_my = Mouse::my;
        }

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
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, false, false, kb_char);
        }

        if (scroll_delta != 0) {
            Graphics::MarkUIDirty();
            Window* fw = WindowManager::GetFocusedWindow();
            if (fw && fw->input) {
                fw->input(fw, 3, scroll_delta, 0);
            }
        }

        if (g_gui_autorun_armed && (int32_t)(Timer::GetTicks() - autorun_target_ms) >= 0) {
            g_gui_autorun_armed = false;
            // sentinel "@taskmgr": open the task manager instead of the terminal
            // (headless ui-debug harness for the task manager overhaul). (satoru)
            const char* cmd = g_gui_autorun_cmd;
            bool open_taskmgr = (cmd[0]=='@' && cmd[1]=='t' && cmd[2]=='a' && cmd[3]=='s' &&
                                 cmd[4]=='k' && cmd[5]=='m' && cmd[6]=='g' && cmd[7]=='r' && cmd[8]==0);
            if (open_taskmgr) {
                SerialLogger::Log("[gui-autorun] launching task manager\r\n");
                DesktopEnvironment::LaunchTaskManager();
            } else {
                SerialLogger::Log("[gui-autorun] launching terminal + queuing: ");
                SerialLogger::Log(g_gui_autorun_cmd);
                SerialLogger::Log("\r\n");
                TerminalApp::Open();
                TerminalApp::EnqueueCommand(g_gui_autorun_cmd);
            }
        }

        DesktopEnvironment::Update();

        if (DesktopEnvironment::ConsumeLogoutRequest()) {
            SerialLogger::Log("[Session] Logout requested\r\n");
            WindowManager::CloseAll();
            UserManager::Logout();
            LockScreen::Show();
            Keyboard::FlushBuffers();
            Mouse::SetAutoDraw(false);
            Scheduler::YieldNow();
            continue;
        }

        // one-shot interactive ksa prompt demo (kurono.ksa.prompt). fire once the
        // desktop has presented enough frames that the modal lands over a real
        // desktop. KSA::Prompt blocks this loop until the user (or synthetic
        // input) answers  -  that block IS the secure desktop: the gui compositor
        // and input process are starved while ksa owns the screen + input. when
        // it returns, the saved desktop is restored and the loop repaints. (satoru)
        if (g_ksa_prompt_demo_armed && frame_counter > 90) {
            g_ksa_prompt_demo_armed = false;
            SerialLogger::Log("[gui] firing interactive ksa prompt demo\r\n");
            KSA::PromptDemo(g_ksa_prompt_demo_cred);
            Graphics::MarkUIDirty();
            continue;   // repaint the desktop on the next iteration (satoru)
        }

        if (frame_counter % 300 == 0) {
            TaskManagerApp::RefreshProcesses();
        }

        // Frame pacing to ~60fps. Timer::GetRealMs() is now sourced from the
        // PIT-IRQ scheduler clock (see timer.cpp GetRealMs), so it advances
        // reliably regardless of poll cadence  -  fixing the FPS-0 freeze where
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
        constexpr uint32_t FALLBACK_MS = 250u;
        // advance the kss tween engine first so animated values progress and the
        // Active() check inside ui_activity_active() reflects in-flight tweens. then
        // advance the stylesheet keyframe layer against that same clock. (satoru)
        KSS::Anim::Tick(Timer::GetRealMs());
        KSS::Sheet::Tick();
        bool dirty    = Graphics::ConsumeUIDirty();
        bool active   = ui_activity_active();
        bool fallback = (Timer::GetRealMs() - last_render_ms) >= FALLBACK_MS;
        if (dirty || active) last_activity_ms = Timer::GetRealMs();
        if (!dirty && !active && !fallback) {
            // Nothing to draw. The old code BUSY-YIELDED here while "recently
            // active" to dodge WHPX/VMware timer coalescing (which made SleepMs
            // wake ~500ms late). but that spin pegged the vcpu at 100% the whole
            // time the desktop sat idle  -  the dev is on KVM now, where the timer
            // IRQ is NOT coalesced, so a 1ms sleep wakes on time AND frees the
            // core (fixes the "whole os is laggy / fan spins" feel). after ~2s of
            // true inactivity, sleep longer to idle the host core fully. if you
            // ever run under WHPX again, bump this back to YieldNow(). (satoru)
            if (Timer::GetRealMs() - last_activity_ms < 2000u) Scheduler::SleepMs(1u);
            else Scheduler::SleepMs(16u);
            continue;
        }
        // There IS work. Pace to ~60fps WITHOUT halting: SleepMs() HLTs the
        // cpu, and WHPX/VMware COALESCE the timer IRQ while the guest is
        // halted, so a 16ms sleep actually lasts ~55ms -> a choppy ~18fps
        // during interaction. Busy-yield instead  -  the loop wakes on the TSC
        // clock, not the coalesced IRQ, giving smooth 60fps while active. Idle
        // frames above still HLT, so host cpu stays low when nothing happens.
        // (satoru)
        // pace to the SELECTED refresh rate (display.refresh_hz -> Graphics
        // target fps), not a hardcoded 60; busy-yield on the tsc clock so
        // whpx/vmware timer coalescing can't stretch the frame. (satoru)
        uint32_t frame_budget_ms = Graphics::GetTargetFrameTimeMs();
        while (Timer::GetRealMs() - last_render_ms < frame_budget_ms) Scheduler::YieldNow();
        last_render_ms = Timer::GetRealMs();

        if (WindowManager::IsDragging()) {
            Graphics::Clear(0xFF0C0C18);
        }

        // Framebuffer access  -  protected so any concurrent E1000 RX
        // path or audio mixer scratch doesn't interleave a SwapBuffers
        // mid-DrawPixel.
        {
            SpinLockCpuGuard guard(g_fb_lock);
            DesktopEnvironment::Render();

            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
            }

            // FPS pill overlay  -  preserved verbatim from kernel_main.
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
            Graphics::FillRoundedRect(tx, 6, pill_w, 22, 11, 0xB0101020);
            Graphics::DrawString(tx + 8, 10, fps_str, 0xFF00E676, 0xFF000000);

            // performance hud overlay (toggle with f12). (satoru)
            PerfHUD::Render(displayed_fps, displayed_fps ? 1000u / displayed_fps : 0u);

            Mouse::DrawAt(Mouse::mx, Mouse::my);
            Graphics::SwapBuffers();
            Graphics::PresentVirtioIfActive();   // push the frame to the host gpu when accelerated (no-op otherwise) (satoru)
        }

        // Hand the CPU back so the other tiers progress.
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
        // display WITHOUT g_fb_lock  -  racing the gui's render, tearing the
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
    return created;
}

} // namespace KernelProcesses
