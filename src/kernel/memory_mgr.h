#pragma once
#include "types.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"

//  well-known fixed physical addresses of the kernel layout. the kernel image is
//  linked in the canonical high half but loaded at KERNEL_LOAD_PHYS; the boot
//  stub sits at 1 mb and all physical ram is still identity-mapped, so every
//  frame the pmm and the heap hand out is reachable at its physical address.
//  these constants name the addresses that are otherwise scattered as magic
//  literals and let the static_asserts below guard the heap floor. (satoru)
static constexpr uint64_t VGA_TEXT_PHYS    = 0x000B8000ULL;  // vga text framebuffer (satoru)
static constexpr uint64_t LAPIC_PHYS       = 0xFEE00000ULL;  // local apic mmio window (satoru)
static constexpr uint64_t KERNEL_LOAD_PHYS = 0x01000000ULL;  // kernel image load base, per linker (satoru)
//  low bound for the pmm-backed heap: the boot stub and page tables occupy the
//  first few mb and the image starts at 16 mb, so in practice the first heap
//  block lands after the image and its metadata. (satoru)
static constexpr uint64_t KHEAP_PHYS_FLOOR = 0x01000000ULL;  // 16 mb (satoru)

static_assert(KHEAP_PHYS_FLOOR > VGA_TEXT_PHYS,
              "kernel heap floor must not overlap the vga text buffer (satoru)");
static_assert(KHEAP_PHYS_FLOOR >= KERNEL_LOAD_PHYS,
              "kernel heap floor must not overlap the kernel load base (satoru)");
static_assert(KHEAP_PHYS_FLOOR < LAPIC_PHYS,
              "kernel heap floor must lie below the local apic mmio window (satoru)");
static_assert(VGA_TEXT_PHYS < LAPIC_PHYS,
              "vga text buffer must lie below the local apic window (satoru)");
static_assert(PAGE_SIZE == 4096ULL, "kernel layout assumes 4 kb pages (satoru)");

//  memory manager - unified interface to pmm, vmm, and kernel heap
//  backward-compatible api: init(), allocpage(), freepage(), gettotalmemory()

class MemoryManager {
public:
    // initialize all memory subsystems. call with multiboot info pointer.
    static void Init(uint64_t mb_addr) {
        multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;
        KernelHeap::Init();            // phase 1: 64 kb bootstrap from bss
        PMM::Init(mbi);               // physical memory manager (detects all ram)
        KernelVMM::Init();             // pml4_phys (the boot root)
        // the direct map must exist before the heap expands, so the first heap
        // region is allocated with direct-map pointers. no-op unless requested.
        KernelVMM::InitDirectMap();
        KernelHeap::ExpandWithPMM();   // phase 2: expand heap to ~256 mb via pmm
    }

    // legacy no-arg init for backward compatibility
    static void Init() {
        KernelHeap::Init();
        // pmm/vmm not initialized without multiboot info - caller should
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
