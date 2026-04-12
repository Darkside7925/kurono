#include "vmm.h"
#include "../drivers/serial.h"

//  virtual memory manager implementation (x86_64 4-level paging)

uint64_t KernelVMM::pml4_phys = 0;

// since we're identity-mapped, phys == virt for anything below 16gb
static inline uint64_t* phys_to_virt(uint64_t phys) {
    return (uint64_t*)(uintptr_t)phys;
}

static inline uint64_t virt_to_phys(void* virt) {
    return (uint64_t)(uintptr_t)virt;
}

// extract 9-bit index at each paging level from a virtual address
static inline uint16_t pml4_index(uint64_t vaddr) { return (vaddr >> 39) & 0x1FF; }
static inline uint16_t pdpt_index(uint64_t vaddr) { return (vaddr >> 30) & 0x1FF; }
static inline uint16_t pd_index(uint64_t vaddr)   { return (vaddr >> 21) & 0x1FF; }
static inline uint16_t pt_index(uint64_t vaddr)   { return (vaddr >> 12) & 0x1FF; }

// allocate a zeroed 4kb page for a page table
static uint64_t alloc_table_page() {
    uint64_t frame = PMM::AllocFrame();
    if (frame == 0) {
        SerialLogger::Log("VMM: FATAL - cannot allocate page table frame!\r\n");
        return 0;
    }
    // zero it (identity-mapped, so we can just use the physical address)
    uint8_t* p = (uint8_t*)(uintptr_t)frame;
    for (int i = 0; i < 4096; i++) p[i] = 0;
    return frame;
}

void KernelVMM::Init() {
    // read current cr3  -  this is the pml4 set up by kurono_boot.asm
    asm volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    pml4_phys &= ~0xFFFULL;  // mask off flags (pcid etc)

    SerialLogger::Log("VMM: Initialized, PML4 at 0x");
    SerialLogger::LogHex(pml4_phys);
    SerialLogger::Log("\r\n");
}

uint64_t KernelVMM::GetPML4() {
    return pml4_phys;
}

bool KernelVMM::MapPage(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    virt_addr &= ~0xFFFULL;  // page-align
    phys_addr &= ~0xFFFULL;

    uint64_t* pml4 = phys_to_virt(pml4_phys);

    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        if (!new_table) return false;
        pml4[p4i] = new_table | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);

    uint16_t p3i = pdpt_index(virt_addr);
    if (pdpt[p3i] & PTE_HUGE) {
        // 1gb page  -  can't overlay a 4kb mapping without splitting
        SerialLogger::Log("VMM: WARNING - cannot map over 1GB huge page\r\n");
        return false;
    }
    if (!(pdpt[p3i] & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        if (!new_table) return false;
        pdpt[p3i] = new_table | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);

    uint16_t p2i = pd_index(virt_addr);
    if (pd[p2i] & PTE_HUGE) {
        // 2mb page  -  our boot setup uses these. to map a 4kb page inside,
        // we'd need to split the 2mb page into a page table. for now, we
        // allow overwriting if the caller explicitly wants to re-map.
        // a proper implementation would split, but that's complex and not
        // needed yet (we map into unmapped regions above the identity map).

        // if the 4kb page falls entirely within the 2mb page at the same
        // physical address, the identity map already covers it  -  success.
        uint64_t huge_base = pd[p2i] & ~0x1FFFFFULL;
        if (phys_addr >= huge_base && phys_addr < huge_base + 0x200000ULL) {
            // already identity-mapped by the 2mb page  -  no action needed
            return true;
        }

        // otherwise, we need a real split. allocate a pt and populate it
        // with 512 entries covering the same 2mb range, then replace one.
        uint64_t new_pt_phys = alloc_table_page();
        if (!new_pt_phys) return false;
        uint64_t* new_pt = phys_to_virt(new_pt_phys);
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (huge_base + (uint64_t)i * PAGE_SIZE)
                        | PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;
        }
        pd[p2i] = new_pt_phys | PTE_PRESENT | PTE_WRITABLE;
        // fall through to update the specific pt entry below
    }
    if (!(pd[p2i] & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        if (!new_table) return false;
        pd[p2i] = new_table | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);

    uint16_t p1i = pt_index(virt_addr);
    pt[p1i] = phys_addr | (flags & ~0xFFFULL ? flags : (PTE_PRESENT | PTE_WRITABLE | flags));
    // ensure pte_present is always set
    pt[p1i] = phys_addr | (flags | PTE_PRESENT);

    InvalidatePage(virt_addr);
    return true;
}

void KernelVMM::UnmapPage(uint64_t virt_addr, bool free_frame) {
    virt_addr &= ~0xFFFULL;

    uint64_t* pml4 = phys_to_virt(pml4_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return;

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return;

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    uint16_t p1i = pt_index(virt_addr);

    if (pt[p1i] & PTE_PRESENT) {
        if (free_frame) {
            PMM::FreeFrame(pt[p1i] & ~0xFFFULL);
        }
        pt[p1i] = 0;
        InvalidatePage(virt_addr);
    }
}

uint64_t KernelVMM::QueryMapping(uint64_t virt_addr) {
    uint64_t* pml4 = phys_to_virt(pml4_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return 0;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT)) return 0;
    if (pdpt[p3i] & PTE_HUGE) {
        // 1gb page
        return (pdpt[p3i] & ~0x3FFFFFFFULL) | (virt_addr & 0x3FFFFFFFULL);
    }

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return 0;
    if (pd[p2i] & PTE_HUGE) {
        // 2mb page
        return (pd[p2i] & ~0x1FFFFFULL) | (virt_addr & 0x1FFFFFULL);
    }

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return 0;

    return (pt[p1i] & ~0xFFFULL) | (virt_addr & 0xFFFULL);
}

void KernelVMM::InvalidatePage(uint64_t virt_addr) {
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

void KernelVMM::FlushTLB() {
    asm volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );
}
