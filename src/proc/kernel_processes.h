#pragma once
//  kurono os  -  kernel process registry
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
}
