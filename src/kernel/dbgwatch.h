#pragma once
#include "../kernel/types.h"
#include "../hal/hal.h"

// kurono.dbgwatch=<hex va>[,<hex va2>]: a hardware write watchpoint (DR0/DR1,
// 4 bytes, every cpu) on a user virtual address. every write to it traps (#DB,
// after the store) and is recorded in a 256-entry ring: cpu, task pid + name,
// rip, the value now in the word and the address space. a value with bit 31
// set or a small integer is also logged at once (rate limited). the ring is
// dumped by kurono.usnap and by DbgWatch::Dump. made to catch who leaves
// 0x80000000 in claude's .bss lock word (the GC freeze). (satoru)
namespace DbgWatch {
    void Arm(uint64_t va0, uint64_t va1);
    void ApplyThisCpu();                     // load DR0/DR1/DR7 on this cpu (tlb ipi path) (satoru)
    bool OnDebugTrap(InterruptFrame* f);     // vector 1; true if it was ours (satoru)
    void Dump(const char* why);
    bool Armed();
}

// end (satoru)
