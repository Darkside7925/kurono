// recovery: Kurono Recovery, a boot mode of the same kernel
// (kurono.mode=recovery, chosen by the EFI loader: asked once, two boots in a
// row that never reached the desktop, or R / F8 held at power-on). no
// desktop, no Linux drivers, software drawing on the firmware framebuffer,
// keyboard only. home: fix last issue, drivers, restore, files & disks,
// terminal, export diagnostics, start normally, start with drivers disabled.
// it writes only the KURONO partition files (KBOOT.CFG, KDRIVERS.CFG,
// KDIAGnnn.TXT) and anything else only from its terminal. (satoru)
#pragma once
#include "../kernel/types.h"

namespace Recovery {
// reason: 0 unknown, 1 asked, 2 two failed boots, 3 key held. (satoru)
[[noreturn]] void Run(int reason);
}

// end (satoru)
