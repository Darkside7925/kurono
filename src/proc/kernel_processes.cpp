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
#include "../ui/perf_hud.h"
#include "../ui/lockscreen.h"
#include "../system/user_mgmt.h"
#include "../apps/terminal.h"
#include "../apps/task_manager.h"
#include "../apps/settings.h"

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
        Scheduler::SleepMs(1);
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
        Scheduler::SleepMs(1);
    }
}

// ── Audio process: AC97 / mixer pump ──────────────────────────────────
[[noreturn]] static void AudioProcessEntry() {
    SerialLogger::Log("[AudioProcess] online\r\n");
    while (true) {
        {
            SpinLockCpuGuard guard(g_audio_lock);
            AudioServer::Tick();
            AC97::Tick();
            Audio::Tick();
        }
        Scheduler::SleepMs(4);
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
        Scheduler::SleepMs(1);
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
        Scheduler::SleepMs(2);
    }
}

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
    uint32_t autorun_target_ms = Timer::GetTicks() + 4000u;

    while (true) {
        // stamp the graphics-loop heartbeat for the watchdog before any work,
        // so a hang anywhere in the body is detectable as a stalled tick. (satoru)
        g_last_frame_ms = Timer::GetRealMs();

        // Advance system wall-clock time once per frame.
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);

        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // Drain mouse events.
        int scroll_delta = 0;
        while (Mouse::HasEvent()) {
            Mouse::Event mevt = Mouse::GetEvent();
            if (mevt.type == 3) { scroll_delta += mevt.dz; continue; }
            if (mevt.type == 1 || mevt.type == 2) {
                WindowManager::HandlePointerButton(mevt.x, mevt.y,
                    (int)mevt.button, mevt.type == 1);
            }
        }

        bool mouse_clicked = Mouse::LeftClicked();
        bool mouse_down = Mouse::IsLeftDown();
        char kb_char = 0;
        if (Keyboard::HasChar()) kb_char = Keyboard::GetChar();
        DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my,
                                        mouse_down, mouse_clicked, kb_char);
        while (Keyboard::HasChar()) {
            kb_char = Keyboard::GetChar();
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, false, false, kb_char);
        }

        if (scroll_delta != 0) {
            Window* fw = WindowManager::GetFocusedWindow();
            if (fw && fw->input) {
                fw->input(fw, 3, scroll_delta, 0);
            }
        }

        if (g_gui_autorun_armed && (int32_t)(Timer::GetTicks() - autorun_target_ms) >= 0) {
            g_gui_autorun_armed = false;
            SerialLogger::Log("[gui-autorun] launching terminal + queuing: ");
            SerialLogger::Log(g_gui_autorun_cmd);
            SerialLogger::Log("\r\n");
            TerminalApp::Open();
            TerminalApp::EnqueueCommand(g_gui_autorun_cmd);
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

        if (frame_counter % 300 == 0) {
            TaskManagerApp::RefreshProcesses();
        }

        // Frame pacing: when the renderer says we still have budget we
        // sleep briefly so the CPU can HLT instead of busy-yielding.  1 ms
        // is well below human perception (~16 ms frame budget) and lets
        // every other tier progress.
        if (!Graphics::ShouldRender()) {
            Scheduler::SleepMs(1);
            continue;
        }

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
    for (;;) {
        Scheduler::SleepMs(5000);
        uint32_t last = g_last_frame_ms;
        if (last == 0) continue;                  // gui loop not started yet (satoru)
        uint32_t now = Timer::GetRealMs();
        if (now > last && (now - last) > 3000) {
            SerialLogger::Log("[watchdog] graphics main loop stalled >3s; reinitializing display backend (satoru)\r\n");
            DisplayManager::Init();
            g_last_frame_ms = Timer::GetRealMs(); // avoid immediate re-trigger after recovery (satoru)
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
