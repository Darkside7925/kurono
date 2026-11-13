// freeze: recovery R3. pause every linux thread group (apps, ring-3 kui apps,
// services) with SIGSTOP through the linux layer's job control, the same
// path as ctrl-z, and resume them with SIGCONT. KLD driver containers keep
// running (one of them is usually what is being restarted). the "paused by
// the error screen" marks live in a side table here; struct Process and the
// scheduler are not touched. audio is muted while frozen so no buffer loops.
// (satoru)
#pragma once
#include "../kernel/types.h"

namespace ProcessFreeze {

// returns the number of thread groups paused. calling it twice is harmless. (satoru)
int  FreezeAll();
// resume exactly the groups FreezeAll paused (a recycled pid is skipped). (satoru)
int  ThawAll();
bool Frozen();

}  // namespace ProcessFreeze

// end (satoru)
