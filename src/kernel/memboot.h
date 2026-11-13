#pragma once
#include "types.h"

//  boot-time ram census: one table of what physically occupies ram on an idle
//  desktop, plus the kvfs file data by top-level directory and the largest
//  files. read-only; every number comes from a subsystem's own accounting.
//  printed once at the end of boot ([memboot]) and on demand by the shell
//  command `memboot`. (satoru)
namespace MemBoot {
    // format the full census into out (shell) and, when to_serial, the same
    // lines to the serial log. returns bytes written to out. (satoru)
    int Report(char* out, int max, bool to_serial);

    // one-shot boot report: called when the desktop has settled. also drops
    // the kfs metadata caches the boot restore left mounted. (satoru)
    void OnDesktopIdle();
    void SetBootReport(bool on);   // kurono.memboot=1: print the report on the desktop-idle tick (satoru)
}

// end (satoru)
