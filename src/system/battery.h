// battery: charge level, charging state and the time estimate (4c). (satoru)
//
// kurono has no aml interpreter in the kernel, so _BST/_BIX (and the
// embedded controller fields they read) cannot be evaluated here. the KLD
// ACPI container (kld/acpi, unmodified ACPICA in ring 3, kurono.kld.acpi=1)
// can: the contract for it is one RAM-only file, /system/run/kld/battery
// (/run/kld/battery inside the container), rewritten every few seconds:
//     <percent 0..100>\t<charging|discharging|full|unknown>\t<minutes or -1>\n
// from _BST (state bits, remaining capacity, present rate) and _BIX/_BIF
// (last full charge capacity): pct = remaining * 100 / last_full, minutes =
// remaining * 60 / rate when discharging, (last_full - remaining) * 60 /
// rate when charging. Tick() reads it; without the file there is no battery
// and the menu bar shows none (it used to draw a full battery always). (satoru)
#pragma once
#include "../kernel/types.h"

namespace Battery {
enum State : uint8_t { S_UNKNOWN = 0, S_CHARGING, S_DISCHARGING, S_FULL };
void  Tick();            // cheap; rereads the source every 5 s (satoru)
bool  Present();
int   Percent();
State GetState();
int   Minutes();         // -1 unknown (satoru)
// "72%, 2 h 10 min left" / "Charging, 40 min until full" (satoru)
void  Describe(char* out, int cap);
}

// end (satoru)
