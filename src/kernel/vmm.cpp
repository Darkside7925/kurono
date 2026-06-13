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

static uint64_t current_cr3() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~0xFFFULL;
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

static inline uint64_t entry_phys(uint64_t entry) {
    return entry & ~0xFFFULL;
}

static inline uint64_t entry_flags(uint64_t entry) {
    return entry & (0xFFFULL | PTE_NX);
}

static uint64_t copy_table_page(uint64_t src_phys) {
    uint64_t new_table = alloc_table_page();
    if (!new_table) return 0;

    memcpy(phys_to_virt(new_table), phys_to_virt(src_phys), PAGE_SIZE);
    return new_table;
}

static void free_user_tree(uint64_t table_phys, int level) {
    uint64_t* table = phys_to_virt(table_phys);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = table[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;

        uint64_t child_phys = entry & ~0xFFFULL;
        if (level > 1 && !(entry & PTE_HUGE)) {
            free_user_tree(child_phys, level - 1);
        } else if (!(entry & PTE_HUGE)) {
            PMM::FreeFrame(child_phys);
        }
    }
    PMM::FreeFrame(table_phys);
}

static uint64_t clone_user_tree(uint64_t table_phys, int level) {
    uint64_t new_table_phys = copy_table_page(table_phys);
    if (!new_table_phys) return 0;

    uint64_t* src = phys_to_virt(table_phys);
    uint64_t* dst = phys_to_virt(new_table_phys);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = src[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;

        if (level > 1) {
            if (entry & PTE_HUGE) {
                free_user_tree(new_table_phys, level);
                return 0;
            }

            uint64_t cloned_child = clone_user_tree(entry_phys(entry), level - 1);
            if (!cloned_child) {
                free_user_tree(new_table_phys, level);
                return 0;
            }

            dst[index] = cloned_child | entry_flags(entry);
            continue;
        }

        if (entry & PTE_HUGE) {
            free_user_tree(new_table_phys, level);
            return 0;
        }

        uint64_t phys = entry_phys(entry);
        uint64_t flags = entry_flags(entry);
        PMM::RetainFrame(phys);

        if (flags & PTE_WRITABLE) {
            flags &= ~PTE_WRITABLE;
            flags |= PTE_COW;
            src[index] = phys | flags;
        }

        dst[index] = phys | flags;
    }

    return new_table_phys;
}

static uint64_t ensure_table_in_root(uint64_t* parent, uint16_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    uint64_t required_flags = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER) required_flags |= PTE_USER;

    if (!(entry & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        if (!new_table) return 0;
        parent[index] = new_table | required_flags;
        return new_table;
    }

    if (!(flags & PTE_USER)) {
        if (!(entry & PTE_WRITABLE)) {
            parent[index] |= PTE_WRITABLE;
        }
        return entry_phys(parent[index]);
    }

    if (entry & PTE_HUGE) return 0;

    if (!(entry & PTE_USER)) {
        uint64_t copied_table = copy_table_page(entry_phys(entry));
        if (!copied_table) return 0;

        parent[index] = copied_table | (entry_flags(entry) | PTE_USER | PTE_WRITABLE);
        return copied_table;
    }

    if (!(entry & PTE_WRITABLE)) {
        parent[index] |= PTE_WRITABLE;
    }

    return entry_phys(parent[index]);
}

static bool map_page_in_root_ex(uint64_t root_phys, uint64_t virt_addr,
                                uint64_t phys_addr, uint64_t flags,
                                bool* out_demoted) {
    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;
    if (out_demoted) *out_demoted = false;

    uint64_t* pml4 = phys_to_virt(root_phys);

    uint16_t p4i = pml4_index(virt_addr);
    uint64_t pdpt_phys = ensure_table_in_root(pml4, p4i, flags);
    if (!pdpt_phys) return false;
    uint64_t* pdpt = phys_to_virt(pdpt_phys);

    uint16_t p3i = pdpt_index(virt_addr);
    if (pdpt[p3i] & PTE_HUGE) {
        SerialLogger::Log("VMM: WARNING - cannot map over 1GB huge page\r\n");
        return false;
    }
    uint64_t pd_phys = ensure_table_in_root(pdpt, p3i, flags);
    if (!pd_phys) return false;
    uint64_t* pd = phys_to_virt(pd_phys);

    uint16_t p2i = pd_index(virt_addr);
    if (pd[p2i] & PTE_HUGE) {
        uint64_t huge_base = pd[p2i] & ~0x1FFFFFULL;
        uint64_t huge_flags = pd[p2i] & 0xFFFULL;
        if (phys_addr >= huge_base && phys_addr < huge_base + 0x200000ULL) {
            return true;
        }

        uint64_t new_pt_phys = alloc_table_page();
        if (!new_pt_phys) return false;
        uint64_t* new_pt = phys_to_virt(new_pt_phys);
        uint64_t leaf_flags = (huge_flags & ~PTE_HUGE) | PTE_PRESENT;
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (huge_base + (uint64_t)i * PAGE_SIZE) | leaf_flags;
        }
        pd[p2i] = new_pt_phys | PTE_PRESENT | PTE_WRITABLE
                              | ((flags & PTE_USER) ? PTE_USER : 0);
        if (out_demoted) *out_demoted = true;
    }
    uint64_t pt_phys = ensure_table_in_root(pd, p2i, flags);
    if (!pt_phys) return false;
    uint64_t* pt = phys_to_virt(pt_phys);

    uint16_t p1i = pt_index(virt_addr);
    pt[p1i] = phys_addr | PTE_PRESENT | flags;
    return true;
}

static bool map_page_in_root(uint64_t root_phys, uint64_t virt_addr,
                             uint64_t phys_addr, uint64_t flags) {
    return map_page_in_root_ex(root_phys, virt_addr, phys_addr, flags, nullptr);
}

static void unmap_page_in_root(uint64_t root_phys, uint64_t virt_addr, bool free_frame) {
    virt_addr &= ~0xFFFULL;

    uint64_t* pml4 = phys_to_virt(root_phys);
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
    if (!(pt[p1i] & PTE_PRESENT)) return;

    if (free_frame) {
        PMM::FreeFrame(pt[p1i] & ~0xFFFULL);
    }
    pt[p1i] = 0;
}

// rewrite the protection bits of an existing 4kb leaf pte in place, keeping the
// mapped physical frame but swapping the flags for PTE_PRESENT|new_flags. walks
// the four levels exactly like unmap_page_in_root; if any level is missing, or
// the translation is served by a huge page, or the leaf isn't present, there is
// nothing to reprotect so we return false (the caller skips it). (satoru)
static bool protect_page_in_root(uint64_t root_phys, uint64_t virt_addr,
                                 uint64_t new_flags) {
    virt_addr &= ~0xFFFULL;

    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return false;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return false;

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return false;

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return false;

    // keep the physical frame, replace every permission/attr bit (satoru)
    uint64_t phys = pt[p1i] & ~0xFFFULL & ~PTE_NX;
    pt[p1i] = phys | PTE_PRESENT | new_flags;
    return true;
}

static uint64_t query_mapping_in_root(uint64_t root_phys, uint64_t virt_addr) {
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return 0;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT)) return 0;
    // mask to the 52-bit physical frame (bits 12-51), stripping the NX bit (63)
    // and any other high flag bits  -  otherwise an NX (rw data) page returns a
    // non-canonical "phys" and dereferencing it #GPs. exposed by the threading
    // path writing ptid/ctid to nx thread-stack pages via write_user_u32. (satoru)
    if (pdpt[p3i] & PTE_HUGE) {
        return (pdpt[p3i] & 0x000FFFFFC0000000ULL) | (virt_addr & 0x3FFFFFFFULL);
    }

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return 0;
    if (pd[p2i] & PTE_HUGE) {
        return (pd[p2i] & 0x000FFFFFFFE00000ULL) | (virt_addr & 0x1FFFFFULL);
    }

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return 0;

    return (pt[p1i] & 0x000FFFFFFFFFF000ULL) | (virt_addr & 0xFFFULL);
}

static uint64_t query_page_flags_in_root(uint64_t root_phys, uint64_t virt_addr) {
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return 0;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT)) return 0;
    if (pdpt[p3i] & PTE_HUGE) {
        return pdpt[p3i] & (0xFFFULL | PTE_NX);
    }

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return 0;
    if (pd[p2i] & PTE_HUGE) {
        return pd[p2i] & (0xFFFULL | PTE_NX);
    }

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return 0;

    return pt[p1i] & (0xFFFULL | PTE_NX);
}

void KernelVMM::Init() {
    // read current cr3  -  this is the pml4 set up by kurono_boot.asm
    pml4_phys = current_cr3();

    SerialLogger::Log("VMM: Initialized, PML4 at 0x");
    SerialLogger::LogHex(pml4_phys);
    SerialLogger::Log("\r\n");
}

uint64_t KernelVMM::CreateAddressSpace() {
    uint64_t new_root = alloc_table_page();
    if (!new_root) return 0;

    uint64_t* src = phys_to_virt(pml4_phys);
    uint64_t* dst = phys_to_virt(new_root);
    for (int index = 0; index < 512; index++) {
        dst[index] = src[index];
    }

    return new_root;
}

uint64_t KernelVMM::CloneAddressSpace(uint64_t source_root_pml4) {
    if (!source_root_pml4) return 0;

    uint64_t new_root = CreateAddressSpace();
    if (!new_root) return 0;

    uint64_t* src = phys_to_virt(source_root_pml4);
    uint64_t* dst = phys_to_virt(new_root);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = src[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;

        uint64_t cloned_subtree = clone_user_tree(entry_phys(entry), 3);
        if (!cloned_subtree) {
            DestroyAddressSpace(new_root);
            return 0;
        }

        dst[index] = cloned_subtree | entry_flags(entry);
    }

    if (source_root_pml4 == current_cr3()) {
        FlushTLB();
    }

    return new_root;
}

void KernelVMM::DestroyAddressSpace(uint64_t root_pml4) {
    if (!root_pml4 || root_pml4 == pml4_phys) return;

    uint64_t* pml4 = phys_to_virt(root_pml4);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = pml4[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;
        free_user_tree(entry & ~0xFFFULL, 3);
        pml4[index] = 0;
    }

    PMM::FreeFrame(root_pml4);
}

uint64_t KernelVMM::GetPML4() {
    return pml4_phys;
}

uint64_t KernelVMM::GetCurrentAddressSpace() {
    return current_cr3();
}

// walk to the leaf PT entry for virt_addr in root_phys, demoting a covering
// 2mb huge page into a fresh 4kb page table first if necessary. returns a
// pointer to the leaf uint64_t pte (host-virtual, identity-mapped) or nullptr
// if the upper levels are absent / a 1gb huge page blocks the split. (satoru)
static uint64_t* leaf_pte_demoting(uint64_t root_phys, uint64_t virt_addr,
                                   bool* out_demoted) {
    if (out_demoted) *out_demoted = false;
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return nullptr;

    uint64_t* pdpt = phys_to_virt(pml4[p4i] & ~0xFFFULL);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return nullptr;

    uint64_t* pd = phys_to_virt(pdpt[p3i] & ~0xFFFULL);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return nullptr;

    if (pd[p2i] & PTE_HUGE) {
        // demote the 2mb huge page to 512 identity 4kb leaves so we can
        // surgically unmap individual frames. (satoru)
        uint64_t huge_base  = pd[p2i] & ~0x1FFFFFULL;
        uint64_t huge_flags = pd[p2i] & 0xFFFULL;
        uint64_t new_pt_phys = alloc_table_page();
        if (!new_pt_phys) return nullptr;
        uint64_t* new_pt = phys_to_virt(new_pt_phys);
        uint64_t leaf_flags = (huge_flags & ~PTE_HUGE) | PTE_PRESENT;
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (huge_base + (uint64_t)i * PAGE_SIZE) | leaf_flags;
        }
        pd[p2i] = new_pt_phys | PTE_PRESENT | PTE_WRITABLE;
        if (out_demoted) *out_demoted = true;
    }

    uint64_t* pt = phys_to_virt(pd[p2i] & ~0xFFFULL);
    return &pt[pt_index(virt_addr)];
}

bool KernelVMM::IsolateFrames(uint64_t phys_base, uint64_t count) {
    phys_base &= ~0xFFFULL;
    bool any_demoted = false;
    bool all_ok = true;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t va = phys_base + i * PAGE_SIZE;   // identity: va == pa (satoru)
        bool demoted = false;
        uint64_t* pte = leaf_pte_demoting(pml4_phys, va, &demoted);
        any_demoted = any_demoted || demoted;
        if (!pte) { all_ok = false; continue; }
        *pte = 0;   // zero the leaf  -  frame is now unmapped in the main os (satoru)
    }
    // demoting a huge page invalidates 512 large-page translations; a full
    // flush is the safe way to drop any stale large-page tlb entries. (satoru)
    if (any_demoted) FlushTLB();
    else {
        for (uint64_t i = 0; i < count; i++) InvalidatePage(phys_base + i * PAGE_SIZE);
    }
    return all_ok;
}

bool KernelVMM::RevealFrames(uint64_t phys_base, uint64_t count, uint64_t flags) {
    phys_base &= ~0xFFFULL;
    bool ok = true;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t pa = phys_base + i * PAGE_SIZE;
        if (!MapPage(pa, pa, flags)) ok = false;   // identity remap (satoru)
    }
    return ok;
}

bool KernelVMM::MapPage(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    bool demoted = false;
    bool mapped = map_page_in_root_ex(pml4_phys, virt_addr, phys_addr, flags, &demoted);
    if (!mapped) return false;
    // demoting a 2MB huge page invalidates 512 4KB translations  -  a single
    // INVLPG can leave stale large-page TLB entries behind on some CPUs.
    if (demoted) FlushTLB();
    else         InvalidatePage(virt_addr);
    return true;
}

bool KernelVMM::MapPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                      uint64_t phys_addr, uint64_t flags) {
    bool demoted = false;
    bool mapped = map_page_in_root_ex(root_pml4, virt_addr, phys_addr, flags, &demoted);
    if (!mapped) return false;
    if (root_pml4 == current_cr3()) {
        if (demoted) FlushTLB();
        else         InvalidatePage(virt_addr);
    }
    return true;
}

void KernelVMM::UnmapPage(uint64_t virt_addr, bool free_frame) {
    unmap_page_in_root(pml4_phys, virt_addr, free_frame);
    InvalidatePage(virt_addr);
}

void KernelVMM::UnmapPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                        bool free_frame) {
    unmap_page_in_root(root_pml4, virt_addr, free_frame);
    if (root_pml4 == current_cr3()) InvalidatePage(virt_addr);
}

bool KernelVMM::ProtectPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                          uint64_t new_flags) {
    // leave the tlb to the caller: mprotect reprotects many pages in a loop and
    // flushes the whole touched range once, rather than per-page here. (satoru)
    return protect_page_in_root(root_pml4, virt_addr, new_flags);
}

// invalidate a contiguous virtual range. for small ranges (< 32 pages) we
// issue per-page INVLPG which is cheaper than a full CR3 reload; otherwise
// fall back to a full TLB flush which avoids 4096 INVLPG instructions.
static void invalidate_range_if_current(uint64_t root, uint64_t virt_base,
                                        uint64_t pages) {
    if (root != current_cr3()) return;
    if (pages == 0) return;
    if (pages > 32) {
        KernelVMM::FlushTLB();
        return;
    }
    for (uint64_t i = 0; i < pages; i++) {
        KernelVMM::InvalidatePage(virt_base + i * PAGE_SIZE);
    }
}

bool KernelVMM::MapRange(uint64_t virt_base, uint64_t phys_base,
                         uint64_t bytes, uint64_t flags) {
    return MapRangeInAddressSpace(pml4_phys, virt_base, phys_base, bytes, flags);
}

bool KernelVMM::MapRangeInAddressSpace(uint64_t root_pml4, uint64_t virt_base,
                                       uint64_t phys_base, uint64_t bytes,
                                       uint64_t flags) {
    if (!bytes) return true;
    virt_base &= ~0xFFFULL;
    phys_base &= ~0xFFFULL;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    // overflow guard
    if (pages > (1ULL << 40)) return false;

    for (uint64_t i = 0; i < pages; i++) {
        if (!map_page_in_root(root_pml4, virt_base + i * PAGE_SIZE,
                              phys_base + i * PAGE_SIZE, flags)) {
            // rollback prior mappings to avoid leaks
            for (uint64_t j = 0; j < i; j++) {
                unmap_page_in_root(root_pml4, virt_base + j * PAGE_SIZE, false);
            }
            invalidate_range_if_current(root_pml4, virt_base, i);
            return false;
        }
    }
    invalidate_range_if_current(root_pml4, virt_base, pages);
    return true;
}

void KernelVMM::UnmapRange(uint64_t virt_base, uint64_t bytes, bool free_frames) {
    UnmapRangeInAddressSpace(pml4_phys, virt_base, bytes, free_frames);
}

void KernelVMM::UnmapRangeInAddressSpace(uint64_t root_pml4, uint64_t virt_base,
                                         uint64_t bytes, bool free_frames) {
    if (!bytes) return;
    virt_base &= ~0xFFFULL;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > (1ULL << 40)) return;
    for (uint64_t i = 0; i < pages; i++) {
        unmap_page_in_root(root_pml4, virt_base + i * PAGE_SIZE, free_frames);
    }
    invalidate_range_if_current(root_pml4, virt_base, pages);
}

uint64_t KernelVMM::QueryMapping(uint64_t virt_addr) {
    return query_mapping_in_root(pml4_phys, virt_addr);
}

uint64_t KernelVMM::QueryMappingInAddressSpace(uint64_t root_pml4, uint64_t virt_addr) {
    return query_mapping_in_root(root_pml4, virt_addr);
}

uint64_t KernelVMM::QueryPageFlags(uint64_t virt_addr) {
    return query_page_flags_in_root(pml4_phys, virt_addr);
}

uint64_t KernelVMM::QueryPageFlagsInAddressSpace(uint64_t root_pml4, uint64_t virt_addr) {
    return query_page_flags_in_root(root_pml4, virt_addr);
}

void KernelVMM::ActivateAddressSpace(uint64_t root_pml4) {
    if (!root_pml4) return;
    asm volatile("mov %0, %%cr3" : : "r"(root_pml4) : "memory");
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
