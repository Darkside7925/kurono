// kernel_slots: A/B kernel updates (3a). a new Kurono EFI is written next to
// the running one as a slot file in the KURONO root (KSLOTA.EFI or
// KSLOTB.EFI, whichever is not the default) and marked in KBOOT.CFG as a
// trial for one boot. the EFI loader starts it once; the boot watch makes it
// the default after 60 s on the desktop, and a trial that does not reach
// the desktop is dropped: the loader goes back to the previous kernel on the
// next start (a hang is ended by the watch's 180 s restart). (satoru)
//
// slot files are sized to the image rounded up plus 16 MiB headroom when
// created, and later updates overwrite them in place (no fat change) when
// the new image fits. (satoru)
#pragma once
#include "../kernel/types.h"

namespace KernelSlots {
// stage an EFI image (a KVFS path, or an 8.3 KURONO root name such as
// KNEW.EFI). writes a one line result into msg. (satoru)
bool Stage(const char* source, char* msg, int cap);
// one line on what runs now and what is staged. (satoru)
void Describe(char* out, int cap);
// shell `kslot status|stage <src>|previous|recovery`. (satoru)
int  Command(int argc, const char** argv, char* out, int mx);
}

// end (satoru)
