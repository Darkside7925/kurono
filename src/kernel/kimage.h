#pragma once
#include "types.h"
#include "kmap.h"   // KERNEL_DIRECT_BASE (satoru)

// the kernel image is linked in the high canonical half but loaded low, so a
// pointer into .text/.rodata/.data/.bss is no longer its own physical address.
// devices (and page-table entries) need the physical one; everything else,
// including every pmm frame and the kernel heap, is still identity mapped.
// (satoru)
extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;
extern "C" uint8_t kernel_phys_start;

static inline uint64_t KernelPhysOf(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    uint64_t ks = (uint64_t)(uintptr_t)&kernel_start;
    uint64_t ke = (uint64_t)(uintptr_t)&kernel_end;
    if (a >= ks && a < ke)
        return (uint64_t)(uintptr_t)&kernel_phys_start + (a - ks);
    if (KernelDirectAddr(a)) return a - KERNEL_DIRECT_BASE;
    return a;
}

// true when the address belongs to the kernel image (and therefore needs the
// conversion above before it is handed to hardware). (satoru)
static inline bool KernelImageAddr(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return a >= (uint64_t)(uintptr_t)&kernel_start &&
           a <  (uint64_t)(uintptr_t)&kernel_end;
}

// end (satoru)
