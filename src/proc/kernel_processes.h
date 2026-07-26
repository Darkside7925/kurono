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

    // queue a shell command to run on the Shell kernel-process WITHOUT a
    // terminal window (desktop launchers, e.g. the firefox icon). output goes
    // to serial only. single slot: returns false if a previous detached
    // command is still queued or running. (satoru)
    bool RunShellCommandDetached(const char* cmd);

    // last Timer::GetRealMs() the gui process stamped at its loop top - the
    // graphics-loop heartbeat. PumpUI reads it to decide whether a live gui
    // process already owns input + rendering (fresh heartbeat -> PumpUI must
    // NOT run a second concurrent ui pipeline; that raced the wayland event
    // stream and the compositor from another cpu). (satoru)
    uint32_t LastGuiFrameMs();
}
