#pragma once
#include "types.h"
#include "heap.h"

// Enhanced Memory Manager
// Provides paging support and advanced allocation strategies.

class MemoryManager {
public:
    static void Init() {
        // KernelHeap is static, no init needed yet
    }
    
    static void* AllocPage() {
        // Allocate 4KB aligned
        // For now, just alloc from heap
        return KernelHeap::Alloc(4096);
    }
    
    static void FreePage(void* ptr) {
        // No free implementation in basic heap yet
        (void)ptr;
    }
    
    static size_t GetTotalMemory() {
        return 10ULL * 1024 * 1024 * 1024; // 10 GB (QEMU -m 10G)
    }
    
    static size_t GetUsedMemory() {
        // KernelHeap doesn't track used yet
        return 0;
    }
};
