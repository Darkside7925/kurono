// platform self-test entry (kurono.platformtest=1). (satoru)
#pragma once
#include "../kernel/types.h"

namespace PlatformTest {
int  Run();                                  // returns the number of failures (satoru)
void Register(const char* name, bool ok);    // one verdict line from another module's test (satoru)
// the other platform modules' offline checks, run after the core ones. (satoru)
typedef void (*TestFn)();
extern TestFn kExtra[];
extern const int kExtraCount;
}

// end (satoru)
