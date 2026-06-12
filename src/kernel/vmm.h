#pragma once
#include "types.h"
#include "pmm.h"

//  virtual memory manager (x86_64 4-level paging)
//
//  works with the existing identity-mapped page tables set up by boot asm.
//  provides mappage / unmappage / querymapping for finer-grained control
//  when the kernel needs to map new physical memory (e.g., mmio, heap
//  expansion, framebuffer at different virtual addresses).
//
//  currently all physical memory 0..16gb is identity-mapped via 2mb pages
//  in boot. the vmm can overlay 4kb mappings on top of that for precise
//  control.

constexpr uint64_t USERSPACE_BASE = 0x0000000040000000ULL;  // 1 GB low-half userspace window

// top of the canonical lower-half user address space (last valid byte).
// the cpu sign-extends bit 47, so 0x0000_7FFF_FFFF_FFFF is the highest
// non-negative virtual address a user pointer can hold. mmap/brk/aslr are
// bounded by this rather than the old 4gb ceiling so pie binaries placed
// high by ld-kurono round-trip through the syscall abi intact (satoru)
constexpr uint64_t USER_SPACE_TOP = 0x00007FFFFFFFFFFFULL;
constexpr uint64_t TASK_SIZE      = USER_SPACE_TOP + 1;  // 0x0000_8000_0000_0000 (satoru)

// page table entry flags (common across all levels)
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PWT        (1ULL << 3)   // page write-through
#define PTE_PCD        (1ULL << 4)   // page cache disable
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)   // 2mb page (in pd), 1gb page (in pdpt)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_COW        (1ULL << 9)   // software-defined copy-on-write marker
#define PTE_NX         (1ULL << 63)  // no-execute (requires efer.nxe = 1)

// virtual address bit layout for 4-level paging:
//   [63:48]    sign extension
//   [47:39]    pml4 index  (9 bits, 512 entries)
//   [38:30]    pdpt index  (9 bits)
//   [29:21]    pd   index  (9 bits)
//   [20:12]    pt   index  (9 bits)
//   [11:0]     offset      (12 bits)

class KernelVMM {
public:
    // initialize: reads current cr3, makes it available for further mapping
    static void Init();

    // create a fresh address space that shares the kernel mappings but has
    // an independent user region starting at USERSPACE_BASE.
    static uint64_t CreateAddressSpace();

    // clone an existing address space, duplicating user-owned pages while
    // preserving shared kernel mappings.
    static uint64_t CloneAddressSpace(uint64_t source_root_pml4);

    // destroy an address space created by CreateAddressSpace(), freeing
    // user-owned page tables and frames.
    static void DestroyAddressSpace(uint64_t root_pml4);

    // map a single 4kb page: virt_addr → phys_addr with given flags.
    // allocates intermediate page tables from pmm as needed.
    // returns true on success.
    static bool MapPage(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
    static bool MapPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                      uint64_t phys_addr, uint64_t flags);

    // map a contiguous range with a single TLB shootdown at the end. Writes
    // PTEs back-to-back so the page-walker stays hot in cache, and only
    // invalidates the touched range when the root is the active CR3.
    static bool MapRange(uint64_t virt_base, uint64_t phys_base,
                         uint64_t bytes, uint64_t flags);
    static bool MapRangeInAddressSpace(uint64_t root_pml4, uint64_t virt_base,
                                       uint64_t phys_base, uint64_t bytes,
                                       uint64_t flags);

    // change the protection of an already-mapped 4kb page in place, keeping the
    // same physical frame but rewriting its pte to PTE_PRESENT|new_flags.
    // new_flags carries WRITABLE/USER/NX (and any pat/global bits). used by
    // mprotect to flip a page between rw and rx for w^x jits. returns false if
    // the page is not currently mapped (caller can skip it  -  demand-zero will
    // map it on first touch with the updated region flags). does NOT flush the
    // tlb; the caller must InvalidatePage when root is the active cr3. (satoru)
    static bool ProtectPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                          uint64_t new_flags);

    // unmap a single 4kb page. frees the physical frame if free_frame is true.
    static void UnmapPage(uint64_t virt_addr, bool free_frame = false);
    static void UnmapPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                        bool free_frame = false);
    static void UnmapRange(uint64_t virt_base, uint64_t bytes, bool free_frames = false);
    static void UnmapRangeInAddressSpace(uint64_t root_pml4, uint64_t virt_base,
                                         uint64_t bytes, bool free_frames = false);

    // query: returns the physical address mapped at virt_addr, or 0 if not mapped.
    static uint64_t QueryMapping(uint64_t virt_addr);
    static uint64_t QueryMappingInAddressSpace(uint64_t root_pml4, uint64_t virt_addr);
    static uint64_t QueryPageFlags(uint64_t virt_addr);
    static uint64_t QueryPageFlagsInAddressSpace(uint64_t root_pml4, uint64_t virt_addr);

    // switch the active CR3 to a different page-table root.
    static void ActivateAddressSpace(uint64_t root_pml4);

    // flush tlb for a single page
    static void InvalidatePage(uint64_t virt_addr);

    // flush entire tlb (reload cr3)
    static void FlushTLB();

    // get the current pml4 physical address (from cr3)
    static uint64_t GetPML4();
    static uint64_t GetCurrentAddressSpace();

private:
    static uint64_t pml4_phys;  // physical address of pml4 table
};
