#pragma once
#include "types.h"

// high-half linear map of physical ram, enabled by kurono.directmap=1. it lives
// in pml4[0x140] (0xffffa00000000000), 512 gb aligned, clear of the kernel
// image at 0xffff800000000000 and the kdf window at 0xffffc00000000000. the
// kernel reaches ordinary frames through direct_base + phys instead of the low
// identity map when this map is active. (satoru)
static constexpr uint64_t KERNEL_DIRECT_BASE = 0xFFFFA00000000000ULL;
static constexpr uint64_t KERNEL_DIRECT_SIZE = 512ULL << 30;

static inline bool KernelDirectAddr(uint64_t a) {
    return a >= KERNEL_DIRECT_BASE && (a - KERNEL_DIRECT_BASE) < KERNEL_DIRECT_SIZE;
}

// end (satoru)
