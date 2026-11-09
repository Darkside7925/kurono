#pragma once
//  kurono os - kernel process registry
//
//  Owns the entry functions and spawn helper for the seven canonical
//  kernel processes: Network, Input, Audio, GUI, Shell, Logging,
//  Scheduler.  Called from kernel_main once all subsystems are
//  initialised.

#include "../kernel/types.h"

namespace KernelProcesses {
    // Spawn the seven kernel processes.  Returns the count actually
    // created (should always be 7 unless the PMM is exhausted).
    int  SpawnAll();

    // Optional hook the GUI process uses to honour the `kurono.gui.run`
    // autorun.  Called once early in the GUI loop.
    void SetGuiAutorun(const char* cmd);
    const char* GuiAutorun();

    // how long the gui process waits after coming up before it fires the
    // autorun. the default grace is pure dead time on the launch-to-paint
    // clock, so kurono.fastrun=1 shrinks it for the startup-latency work.
    // must be called before SpawnAll(). (satoru)
    void SetAutorunDelayMs(uint32_t ms);
    // sched=2: run terminal commands on worker kernel procs, concurrently per
    // window (default on); kurono.schedterm=0 keeps the single shell proc. (satoru)
    void SetTermWorkers(bool on);
    void SetTermTest(bool on);   // kurono.termtest=1 rig (satoru)
    void SetKstackTest(bool on);   // kurono.kstacktest=1 rig (satoru)

    // kurono.fpsbench=1: make the gui loop composite every iteration with no
    // frame pacing, so the [fps] serial line measures the render+present
    // pipeline rather than the damage gate. must be set before SpawnAll(). (satoru)
    void SetFpsBench(int mode);

    // arm a one-shot interactive ksa prompt demo (kurono.ksa.prompt boot gate).
    // once the desktop has presented a few frames the GUI process calls
    // KSA::PromptDemo() once, so a headless screendump can capture the real
    // on-screen modal and synthetic input can drive the verdict. want_cred
    // selects the credential-collecting variant (auth=kvault-only). (satoru)
    void ArmKsaPromptDemo(bool want_cred);

    // arm a one-shot kj-scripted animation demo (kurono.kjdemo boot gate). once
    // the desktop is up the GUI process runs the shipped accent-animation .kj
    // script once, so a real on-screen animation is driven through the kj host
    // bindings (kss.transition/kss.set/ui.notify). (satoru)
    void ArmKjDemo();

    // deterministic wallpaper damage test (kurono.wptest=1): open two plain
    // pattern windows and move/minimize/unminimize/close them on a fixed
    // schedule, logging [wptest] step=N so a headless harness can capture
    // pixel-identical screenshots with wallpaper damage-only on and off. (satoru)
    void SetWpTest(int mode);

    // queue a shell command to run on the Shell kernel-process WITHOUT a
    // terminal window (desktop launchers, e.g. the firefox icon). output goes
    // to serial only. single slot: returns false if a previous detached
    // command is still queued or running. (satoru)
    bool RunShellCommandDetached(const char* cmd);
    // the terminal exec bridge: run a kernel built-in for a program on a terminal's pty. the
    // output text and the program's stdio go to `pair`; returns the launcher slot (>= 0) or -1.
    // BridgeBusy: still running; BridgeCancel: kill its program group (ctrl+c). (satoru)
    int  RunShellCommandBridge(const char* cmd, int pair);
    bool BridgeBusy(int pair);      // a bridged command of this terminal pair is still running (satoru)
    void BridgeCancel(int pair);
    bool BridgeCancelRequested();

    // last Timer::GetRealMs() the gui process stamped at its loop top - the
    // graphics-loop heartbeat. PumpUI reads it to decide whether a live gui
    // process already owns input + rendering (fresh heartbeat -> PumpUI must
    // NOT run a second concurrent ui pipeline; that raced the wayland event
    // stream and the compositor from another cpu). (satoru)
    uint32_t LastGuiFrameMs();
}
