#pragma once
#include "types.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"

//  memory manager  -  unified interface to pmm, vmm, and kernel heap
//  backward-compatible api: init(), allocpage(), freepage(), gettotalmemory()

class MemoryManager {
public:
    // initialize all memory subsystems. call with multiboot info pointer.
    static void Init(uint64_t mb_addr) {
        multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;
        KernelHeap::Init();            // phase 1: 64 kb bootstrap from bss
        PMM::Init(mbi);               // physical memory manager (detects all ram)
        KernelHeap::ExpandWithPMM();   // phase 2: expand heap to ~256 mb via pmm
        KernelVMM::Init();
    }

    // legacy no-arg init for backward compatibility
    static void Init() {
        KernelHeap::Init();
        // pmm/vmm not initialized without multiboot info  -  caller should
        // use init(mb_addr) instead.
    }

    // allocate a single 4kb-aligned page from the pmm
    static void* AllocPage() {
        uint64_t frame = PMM::AllocFrame();
        if (frame == 0) return nullptr;
        return (void*)(uintptr_t)frame;  // identity-mapped
    }

    // free a page back to the pmm
    static void FreePage(void* ptr) {
        if (!ptr) return;
        PMM::FreeFrame((uint64_t)(uintptr_t)ptr);
    }

    // allocate n contiguous 4kb pages
    static void* AllocPages(size_t count) {
        uint64_t base = PMM::AllocContiguous(count);
        if (base == 0) return nullptr;
        return (void*)(uintptr_t)base;
    }

    // total physical memory detected
    static size_t GetTotalMemory() {
        return (size_t)PMM::GetTotalMemory();
    }

    static size_t GetUsedMemory() {
        return (size_t)(PMM::GetUsedFrames() * PAGE_SIZE);
    }

    static size_t GetFreeMemory() {
        return (size_t)PMM::GetFreeMemory();
    }
};
