// idle_sleep: the safe first step toward suspend on S0ix-only laptops like
// the Arrow Lake Legion (4d; the full design is in the platform report):
// screen off (backlight duty 0 when driven, the scanout painted black, no
// presents), every app paused (ProcessFreeze), audio muted, and the cpus left
// to the scheduler's hlt idle. any key, a mouse move or the power button
// wakes it. no device changes power state, so nothing has to be restored
// beyond the backlight. kurono.sleep=idle makes Sleep use it. (satoru)
#pragma once
#include "../kernel/types.h"

namespace IdleSleep {
void SetEnabled(bool on);
bool Enabled();
void Run();   // blocks until wake (satoru)
}

// end (satoru)
