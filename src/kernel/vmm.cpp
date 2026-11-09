#include "vmm.h"
#include "../drivers/serial.h"
#include "../proc/cgroup.h"     // memory.max charge on user page maps (satoru)
#include "../proc/scheduler.h"  // current task -> charging cgroup (satoru)
#include "pmm.h"                // ram top - the hardened walks bound every table deref (satoru)
#include "../proc/smp.h"        // fork tlb shootdown (satoru)

//  virtual memory manager implementation (x86_64 4-level paging)

uint64_t KernelVMM::pml4_phys = 0;

// cgroup memory.max chokepoint. every user page (PTE_USER) mapped into a
// per-process address space passes through MapPage*/MapRange*InAddressSpace,
// so charging here covers the mmap / brk / demand-zero / stack paths without
// touching the syscall layer. the charge lands on the CURRENT task's cgroup
// (linux memcg charges the allocating task) but is recorded in a ledger keyed
// by the address-space root, so frees and teardown always uncharge the right
// account no matter which task performs them. tasks outside any cgroup
// (cgroup_id 0 - everything except kinit service units) short-circuit inside
// ChargeUserPages at zero cost. (satoru)
static inline uint32_t current_cgroup_id() {
    Process* cur = Scheduler::GetCurrentProcess();
    return cur ? cur->cgroup_id : 0;
}

// since we're identity-mapped, phys == virt for every managed frame (the boot
// map covers 0..512 gb with 2 mb pages; the pmm never manages ram above it). (satoru)
static inline uint64_t* phys_to_virt(uint64_t phys) {
    return (uint64_t*)(uintptr_t)phys;
}

static inline uint64_t virt_to_phys(void* virt) {
    return (uint64_t)(uintptr_t)virt;
}

// the pmm now manages ram above the old 16 gb cap. every user address space
// copies the kernel pml4, identity map included, so a USER leaf at a va inside
// [16 gb, ram top) shadows the frame at that same physical address in that
// address space: kernel identity writes to the frame (zero-on-alloc, page
// tables, heap) would land in the user page instead - the main-stack overlay
// bug, generalised. such a map is refused unless the va is one of the pmm's
// fixed overlay windows, whose frames are never handed out. this catches
// MAP_FIXED, elf segments and fixed kernel-chosen va. (satoru)
// item 8 (the remaining shadow channels below 16 gb): the same holds for every
// va the boot identity map covers, not only the newly managed range. a user
// MAP_FIXED (or an elf segment, or a stack grow) over unreserved low ram hid
// that frame from the kernel in that address space; low 1 mb (the ac97 bdl, the
// ap trampoline), the kernel image and mmio identity pages above ram are just
// as live. so the whole identity range [0, 512 gb) is refused except the
// windows the pmm never hands out: the overlay table (main stack, ld-kurono
// trampoline) and, with a high-linked kernel, the user image window below it.
// auto-placed mmaps were never affected (choose_mmap_base already skips every
// present identity va). (satoru)
static bool user_va_shadows_managed_ram(uint64_t va, uint64_t bytes) {
    const uint64_t lo  = 0;
    const uint64_t top = PMM::IDENTITY_CEIL_BYTES;
    if (bytes == 0) return false;
    uint64_t end = va + bytes;
    if (end < va) end = ~0ULL;
    if (end <= lo || va >= top) return false;
    uint64_t a = va < lo ? lo : va;
    uint64_t b = end > top ? top : end;
    if (PMM::IsUserOverlayRange(a & ~0xFFFULL, (b + 0xFFFULL) & ~0xFFFULL)) return false;
    static uint32_t s_logs = 0;
    if (s_logs < 16) {
        s_logs++;
        SerialLogger::Log("[idalias] refused user map va=0x");
        SerialLogger::LogHex64(va);
        SerialLogger::Log(" bytes=0x");
        SerialLogger::LogHex64(bytes);
        SerialLogger::Log(" (inside the kernel identity map, outside the user windows)\r\n");
    }
    return true;
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

// physical frame address of a page-table entry: bits 12..51. masking only the
// low 12 bits kept NX (bit 63), so clone_user_tree retained "phys | NX" - an
// out-of-range frame, a silent no-op - and every NX page (all stack and heap)
// stayed at refcount 1 after fork. the cow handler then saw a sole owner and
// made the page writable without copying, so parent and child wrote the same
// stack: firefox's main process returned to 0 out of _Fork.
static inline uint64_t entry_phys(uint64_t entry) {
    return entry & 0x000FFFFFFFFFF000ULL;
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

// item 7 (frames leaked on every exit): the child address was `entry & ~0xFFF`,
// which keeps bit 63. every NX leaf (anon heap/stack/data, library data and
// rodata - most of a process) became an address past ram top, so FreeFrame and
// MarkFrameMapped silently ignored it and the frame was lost. measured 4.6 mb a
// round of fork/exec churn. entry_phys keeps only bits 12..51. (satoru)
static void free_user_tree(uint64_t table_phys, int level) {
    uint64_t* table = phys_to_virt(table_phys);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = table[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;

        uint64_t child_phys = entry_phys(entry);
        if (level > 1 && !(entry & PTE_HUGE)) {
            free_user_tree(child_phys, level - 1);
        } else if (!(entry & PTE_HUGE)) {
            // task 26 guard: the leaf pte dies with this tree - clear the
            // mapped bit so the freed frame is re-handable. (satoru)
            PMM::MarkFrameMapped(child_phys, false);
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

        // shared mappings stay shared and writable in both: posix fork
        // semantics for MAP_SHARED. (satoru)
        if (flags & PTE_SHARED) {
            dst[index] = phys | flags;
            continue;
        }

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
        // always demote the covering 2mb huge page to a 4kb pt - even when
        // phys_addr already falls inside this huge page's range. previously we
        // early-returned here, which silently discarded the caller's requested
        // flags (pte_user / pte_nx / pte_pcd|pwt cache bits): every identity
        // MapPage(p,p,flags) for p<512gb hits the boot supervisor-wb huge map
        // and would have kept write-back/supervisor rather than the requested
        // attrs (nvme/virtio-gpu mmio asked for pte_pcd but got write-back; user
        // mappings over kernel identity pages stayed supervisor-only). demote so
        // the leaf write below (pt[p1i] = phys|present|flags) lands the real
        // bits. mirrors leaf_pte_demoting. (satoru)
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

// returns true when a PRESENT user (PTE_USER) frame was actually freed, so
// the callers can uncharge the cgroup memory ledger by exactly the pages that
// really went back to the pmm. (satoru)
static bool unmap_page_in_root(uint64_t root_phys, uint64_t virt_addr, bool free_frame) {
    virt_addr &= ~0xFFFULL;

    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return false;

    uint64_t* pdpt = phys_to_virt(entry_phys(pml4[p4i]));
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return false;

    uint64_t* pd = phys_to_virt(entry_phys(pdpt[p3i]));
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return false;

    uint64_t* pt = phys_to_virt(entry_phys(pd[p2i]));
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return false;

    bool user_freed = false;
    if (free_frame) {
        // entry_phys: an NX leaf freed nothing with the old ~0xFFF mask (the
        // single-cpu munmap / madvise path). (satoru)
        PMM::FreeFrame(entry_phys(pt[p1i]));
        user_freed = (pt[p1i] & PTE_USER) != 0;
    }
    pt[p1i] = 0;
    return user_freed;
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

    uint64_t* pdpt = phys_to_virt(entry_phys(pml4[p4i]));
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return false;

    uint64_t* pd = phys_to_virt(entry_phys(pdpt[p3i]));
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return false;

    uint64_t* pt = phys_to_virt(entry_phys(pd[p2i]));
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return false;

    // keep the physical frame, replace every permission/attr bit except the
    // shared marker: mprotect on a MAP_SHARED view must not make the next fork
    // turn it copy-on-write. (satoru)
    uint64_t phys = entry_phys(pt[p1i]);
    uint64_t keep = pt[p1i] & PTE_SHARED;
    pt[p1i] = phys | PTE_PRESENT | new_flags | keep;
    return true;
}

static uint64_t query_mapping_in_root(uint64_t root_phys, uint64_t virt_addr) {
    // HARDENED walk: the futex sweep re-walks PARKED waiters' address spaces
    // every 8ms, and a stale root (task struct recycled or address space
    // destroyed while a stale waiter slot survived) hands this walk freed,
    // reused table frames whose garbage "entries" decode to wild pointers -
    // dereferencing one #GP'd the kernel from the sweep's irq context (the
    // p4 boot3 panic; the dump stack was full of ftxgraph strings). bound the
    // root and every next-level table to real, identity-mapped ram before
    // touching it - a garbage entry then reads as "not mapped", never as a
    // wild deref. (satoru)
    uint64_t ram_top = PMM::GetPhysTop();
    if (!root_phys || (root_phys & 0xFFFULL) || root_phys >= ram_top) return 0;
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return 0;

    uint64_t pdpt_phys = pml4[p4i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pdpt_phys >= ram_top) return 0;
    uint64_t* pdpt = phys_to_virt(pdpt_phys);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT)) return 0;
    // mask to the 52-bit physical frame (bits 12-51), stripping the NX bit (63)
    // and any other high flag bits - otherwise an NX (rw data) page returns a
    // non-canonical "phys" and dereferencing it #GPs. exposed by the threading
    // path writing ptid/ctid to nx thread-stack pages via write_user_u32. (satoru)
    if (pdpt[p3i] & PTE_HUGE) {
        return (pdpt[p3i] & 0x000FFFFFC0000000ULL) | (virt_addr & 0x3FFFFFFFULL);
    }

    uint64_t pd_phys = pdpt[p3i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pd_phys >= ram_top) return 0;
    uint64_t* pd = phys_to_virt(pd_phys);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return 0;
    if (pd[p2i] & PTE_HUGE) {
        return (pd[p2i] & 0x000FFFFFFFE00000ULL) | (virt_addr & 0x1FFFFFULL);
    }

    uint64_t pt_phys = pd[p2i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pt_phys >= ram_top) return 0;
    uint64_t* pt = phys_to_virt(pt_phys);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return 0;

    return (pt[p1i] & 0x000FFFFFFFFFF000ULL) | (virt_addr & 0xFFFULL);
}

static uint64_t query_page_flags_in_root(uint64_t root_phys, uint64_t virt_addr) {
    // same hardening as query_mapping_in_root: never deref a table pointer
    // outside real ram (stale roots / recycled frames feed garbage here). (satoru)
    uint64_t ram_top = PMM::GetPhysTop();
    if (!root_phys || (root_phys & 0xFFFULL) || root_phys >= ram_top) return 0;
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return 0;

    uint64_t pdpt_phys = pml4[p4i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pdpt_phys >= ram_top) return 0;
    uint64_t* pdpt = phys_to_virt(pdpt_phys);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT)) return 0;
    if (pdpt[p3i] & PTE_HUGE) {
        return pdpt[p3i] & (0xFFFULL | PTE_NX);
    }

    uint64_t pd_phys = pdpt[p3i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pd_phys >= ram_top) return 0;
    uint64_t* pd = phys_to_virt(pd_phys);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT)) return 0;
    if (pd[p2i] & PTE_HUGE) {
        return pd[p2i] & (0xFFFULL | PTE_NX);
    }

    uint64_t pt_phys = pd[p2i] & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    if (pt_phys >= ram_top) return 0;
    uint64_t* pt = phys_to_virt(pt_phys);
    uint16_t p1i = pt_index(virt_addr);
    if (!(pt[p1i] & PTE_PRESENT)) return 0;

    return pt[p1i] & (0xFFFULL | PTE_NX);
}

void KernelVMM::Init() {
    // read current cr3 - this is the pml4 set up by kurono_boot.asm
    pml4_phys = current_cr3();

    SerialLogger::Log("VMM: Initialized, PML4 at 0x");
    SerialLogger::LogHex(pml4_phys);
    SerialLogger::Log("\r\n");
}

// kurono.memcensus=1: registry of live user address spaces, so the census can
// tell which user frames no address space maps any more. lock-free slots; only
// filled while the census is on. created/destroyed counts are always kept. (satoru)
static constexpr int AS_REG_MAX = 2048;
static uint64_t g_as_reg[AS_REG_MAX];
static uint64_t g_as_created = 0, g_as_destroyed = 0;
static void as_reg_add(uint64_t root) {
    __atomic_add_fetch(&g_as_created, 1, __ATOMIC_RELAXED);
    if (!PMM::CensusOn()) return;
    for (int i = 0; i < AS_REG_MAX; i++) {
        uint64_t z = 0;
        if (__atomic_compare_exchange_n(&g_as_reg[i], &z, root, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return;
    }
}
static void as_reg_del(uint64_t root) {
    __atomic_add_fetch(&g_as_destroyed, 1, __ATOMIC_RELAXED);
    if (!PMM::CensusOn()) return;
    for (int i = 0; i < AS_REG_MAX; i++) {
        uint64_t r = root;
        if (__atomic_compare_exchange_n(&g_as_reg[i], &r, 0, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return;
    }
}
static void census_mark(uint8_t* bits, uint64_t nframes, uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (f < nframes) bits[f >> 3] |= (uint8_t)(1u << (f & 7));
}
// racy read-only walk of one user subtree (same shape as free_user_tree). every
// table pointer is bounded by ram top, so a table freed mid-walk reads garbage
// instead of faulting. (satoru)
static void census_walk(uint64_t table_phys, int level, uint8_t* bits, uint64_t nframes,
                        uint64_t* leaves, uint64_t* tables) {
    census_mark(bits, nframes, table_phys);
    (*tables)++;
    const uint64_t* t = phys_to_virt(table_phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER) || (e & PTE_HUGE)) continue;
        uint64_t c = entry_phys(e);
        if (c / PAGE_SIZE >= nframes) continue;
        if (level > 1) census_walk(c, level - 1, bits, nframes, leaves, tables);
        else { census_mark(bits, nframes, c); (*leaves)++; }
    }
}
uint32_t KernelVMM::CensusMarkUser(uint8_t* bits, uint64_t nframes,
                                   uint64_t* leaves, uint64_t* tables,
                                   uint64_t* created, uint64_t* destroyed) {
    *leaves = 0; *tables = 0;
    *created = __atomic_load_n(&g_as_created, __ATOMIC_RELAXED);
    *destroyed = __atomic_load_n(&g_as_destroyed, __ATOMIC_RELAXED);
    uint32_t n = 0;
    for (int i = 0; i < AS_REG_MAX; i++) {
        uint64_t root = __atomic_load_n(&g_as_reg[i], __ATOMIC_RELAXED);
        if (!root || root / PAGE_SIZE >= nframes) continue;
        n++;
        census_mark(bits, nframes, root);
        (*tables)++;
        const uint64_t* pml4 = phys_to_virt(root);
        for (int k = 0; k < 512; k++) {
            uint64_t e = pml4[k];
            if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
            uint64_t c = entry_phys(e);
            if (c / PAGE_SIZE >= nframes) continue;
            census_walk(c, 3, bits, nframes, leaves, tables);
        }
    }
    return n;
}

uint64_t KernelVMM::CreateAddressSpace() {
    uint64_t new_root = alloc_table_page();
    if (!new_root) return 0;
    as_reg_add(new_root);

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
    // parent ptes are now cow+ro; flush siblings' stale writable tlbs on other cores. (satoru)
    SMP::BroadcastTlbFlush();

    return new_root;
}

bool g_cc_noasfree = false;   // kurono.cc.noasfree=1: leak address spaces instead of freeing (a/b diagnostic) (satoru)
// item 10: the linux layer's region-table extension chunks are keyed by address
// space; this tells it when one dies. (satoru)
static void (*g_as_destroy_hook)(uint64_t root) = nullptr;
void KernelVMM::SetAddressSpaceDestroyHook(void (*hook)(uint64_t root)) { g_as_destroy_hook = hook; }

void KernelVMM::DestroyAddressSpace(uint64_t root_pml4) {
    if (!root_pml4 || root_pml4 == pml4_phys) return;
    as_reg_del(root_pml4);
    if (g_as_destroy_hook) g_as_destroy_hook(root_pml4);
    if (g_cc_noasfree) return;

    uint64_t* pml4 = phys_to_virt(root_pml4);
    for (int index = 0; index < 512; index++) {
        uint64_t entry = pml4[index];
        if (!(entry & PTE_PRESENT) || !(entry & PTE_USER)) continue;
        free_user_tree(entry_phys(entry), 3);
        pml4[index] = 0;
    }

    // settle the cgroup memory ledger: everything this space still had
    // charged is uncharged in one shot, so a dying service unit's memcg
    // balance returns to zero. (satoru)
    Cgroup::ReleaseAddressSpace(root_pml4);

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

    uint64_t* pdpt = phys_to_virt(entry_phys(pml4[p4i]));
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return nullptr;

    uint64_t* pd = phys_to_virt(entry_phys(pdpt[p3i]));
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

    uint64_t* pt = phys_to_virt(entry_phys(pd[p2i]));
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
        *pte = 0;   // zero the leaf - frame is now unmapped in the main os (satoru)
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

// walk to the 4kb LEAF pte for virt_addr in root_phys WITHOUT demoting: a
// covering huge page (1gb or 2mb) returns nullptr, because kmemx only ages /
// compresses individual 4kb leaves and must never split a huge identity map.
// returns a host-virtual pointer to the leaf uint64_t, or nullptr. (satoru)
static uint64_t* leaf_pte_nodemote(uint64_t root_phys, uint64_t virt_addr) {
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return nullptr;
    uint64_t* pdpt = phys_to_virt(entry_phys(pml4[p4i]));
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return nullptr;
    uint64_t* pd = phys_to_virt(entry_phys(pdpt[p3i]));
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return nullptr;
    uint64_t* pt = phys_to_virt(entry_phys(pd[p2i]));
    return &pt[pt_index(virt_addr)];
}

int KernelVMM::KmemxAgeLeaf(uint64_t root_pml4, uint64_t virt_addr) {
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr & ~0xFFFULL);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    uint64_t e = *pte;
    if (e & PTE_ACCESSED) {
        // touched recently: clear A so we can observe the NEXT interval, reset gen. (satoru)
        e &= ~PTE_ACCESSED;
        e &= ~PTE_KMEMX_GEN_MASK;
        *pte = e;
        return 0;
    }
    // untouched this interval: bump the generation (saturating at 15). (satoru)
    uint64_t gen = (e & PTE_KMEMX_GEN_MASK) >> PTE_KMEMX_GEN_SHIFT;
    if (gen < 15) gen++;
    e = (e & ~PTE_KMEMX_GEN_MASK) | (gen << PTE_KMEMX_GEN_SHIFT);
    *pte = e;
    return (int)gen;
}

int KernelVMM::KmemxGetGeneration(uint64_t root_pml4, uint64_t virt_addr) {
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr & ~0xFFFULL);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    return (int)((*pte & PTE_KMEMX_GEN_MASK) >> PTE_KMEMX_GEN_SHIFT);
}

bool KernelVMM::KmemxIsLeafPresent(uint64_t root_pml4, uint64_t virt_addr) {
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr & ~0xFFFULL);
    return pte && (*pte & PTE_PRESENT);
}

bool KernelVMM::KmemxMarkCompressed(uint64_t root_pml4, uint64_t virt_addr,
                                    uint64_t* out_phys, uint64_t* out_flags) {
    virt_addr &= ~0xFFFULL;
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr);
    if (!pte || !(*pte & PTE_PRESENT)) return false;
    uint64_t e = *pte;
    if (out_phys)  *out_phys  = e & 0x000FFFFFFFFFF000ULL;
    // preserve the permission/attr bits the page had (writable/user/nx/pcd/pwt/
    // global) so the restored page is byte-for-byte the same mapping. (satoru)
    if (out_flags) *out_flags = e & (PTE_WRITABLE | PTE_USER | PTE_PWT | PTE_PCD |
                                     PTE_GLOBAL | PTE_NX);
    // rewrite as NOT-present + the kmemx marker. the frame is the caller's to
    // free once the bytes are safely in the pool. (satoru)
    *pte = PTE_KMEMX_COMPRESSED;
    PMM::MarkFrameMapped(e & 0x000FFFFFFFFFF000ULL, false);   // task 26 guard (satoru)
    return true;
}

bool KernelVMM::KmemxIsCompressed(uint64_t root_pml4, uint64_t virt_addr) {
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr & ~0xFFFULL);
    // a compressed leaf is NOT-present but carries the marker. (satoru)
    return pte && !(*pte & PTE_PRESENT) && (*pte & PTE_KMEMX_COMPRESSED);
}

bool KernelVMM::KmemxRestoreLeaf(uint64_t root_pml4, uint64_t virt_addr,
                                 uint64_t phys, uint64_t flags) {
    virt_addr &= ~0xFFFULL;
    uint64_t* pte = leaf_pte_nodemote(root_pml4, virt_addr);
    if (!pte) return false;   // the leaf table must still exist (it does - only the leaf changed) (satoru)
    // clear any stale generation/marker; install the fresh frame + perms. (satoru)
    *pte = (phys & 0x000FFFFFFFFFF000ULL) | PTE_PRESENT | (flags & (PTE_WRITABLE |
            PTE_USER | PTE_PWT | PTE_PCD | PTE_GLOBAL | PTE_NX));
    PMM::MarkFrameMapped(phys & 0x000FFFFFFFFFF000ULL, true);   // task 26 guard (satoru)
    if (root_pml4 == current_cr3()) InvalidatePage(virt_addr);
    return true;
}

bool KernelVMM::MapPage(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    bool demoted = false;
    bool mapped = map_page_in_root_ex(pml4_phys, virt_addr, phys_addr, flags, &demoted);
    if (!mapped) return false;
    // demoting a 2MB huge page invalidates 512 4KB translations - a single
    // INVLPG can leave stale large-page TLB entries behind on some CPUs.
    if (demoted) FlushTLB();
    else         InvalidatePage(virt_addr);
    return true;
}

bool KernelVMM::MapPageInAddressSpace(uint64_t root_pml4, uint64_t virt_addr,
                                      uint64_t phys_addr, uint64_t flags) {
    // cgroup memory.max: charge a user page before it lands. a failed charge
    // fails the map, which the mmap/demand-zero callers surface as OOM - the
    // enforcement linux delivers via memcg. kernel-root maps are exempt. (satoru)
    bool charged = false;
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        user_va_shadows_managed_ram(virt_addr & ~0xFFFULL, PAGE_SIZE))
        return false;   // see user_va_shadows_managed_ram (satoru)
    if ((flags & PTE_USER) && root_pml4 != pml4_phys) {
        if (!Cgroup::ChargeUserPages(root_pml4, current_cgroup_id(), PAGE_SIZE))
            return false;
        charged = true;
    }
    // task 26 free-while-mapped guard: replacing a present user leaf must clear
    // the OLD frame's mapped bit before the new frame's is set. (satoru)
    uint64_t old_phys = (flags & PTE_USER)
        ? QueryMappingInAddressSpace(root_pml4, virt_addr & ~0xFFFULL) : 0;
    bool demoted = false;
    bool mapped = map_page_in_root_ex(root_pml4, virt_addr, phys_addr, flags, &demoted);
    if (!mapped) {
        if (charged) Cgroup::UnchargeUserPages(root_pml4, PAGE_SIZE);
        return false;
    }
    if (flags & PTE_USER) {
        if (old_phys) PMM::MarkFrameMapped(old_phys & ~0xFFFULL, false);
        PMM::MarkFrameMapped(phys_addr & ~0xFFFULL, true);
    }
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
    // task 26 free-while-mapped guard: clear the frame's mapped bit as the pte
    // goes away. read BEFORE the unmap (the leaf is gone after). (satoru)
    uint64_t old_phys = QueryMappingInAddressSpace(root_pml4, virt_addr & ~0xFFFULL);
    bool user_freed = unmap_page_in_root(root_pml4, virt_addr, free_frame);
    if (old_phys) PMM::MarkFrameMapped(old_phys & ~0xFFFULL, false);
    if (user_freed && root_pml4 != pml4_phys)
        Cgroup::UnchargeUserPages(root_pml4, PAGE_SIZE);   // memory.max ledger (satoru)
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

    // cgroup memory.max: charge the whole user range up front (one ledger
    // round-trip) and refund it wholesale if the map fails part-way. (satoru)
    bool charged = false;
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        user_va_shadows_managed_ram(virt_base, pages * PAGE_SIZE))
        return false;   // see user_va_shadows_managed_ram (satoru)
    if ((flags & PTE_USER) && root_pml4 != pml4_phys) {
        if (!Cgroup::ChargeUserPages(root_pml4, current_cgroup_id(),
                                     pages * PAGE_SIZE))
            return false;
        charged = true;
    }

    for (uint64_t i = 0; i < pages; i++) {
        if (!map_page_in_root(root_pml4, virt_base + i * PAGE_SIZE,
                              phys_base + i * PAGE_SIZE, flags)) {
            // rollback prior mappings to avoid leaks
            for (uint64_t j = 0; j < i; j++) {
                unmap_page_in_root(root_pml4, virt_base + j * PAGE_SIZE, false);
            }
            invalidate_range_if_current(root_pml4, virt_base, i);
            if (charged) Cgroup::UnchargeUserPages(root_pml4, pages * PAGE_SIZE);
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
    uint64_t user_freed_pages = 0;
    for (uint64_t i = 0; i < pages; i++) {
        if (unmap_page_in_root(root_pml4, virt_base + i * PAGE_SIZE, free_frames))
            user_freed_pages++;
    }
    // one ledger uncharge for the whole range (one lock take, not per page) (satoru)
    if (user_freed_pages && root_pml4 != pml4_phys)
        Cgroup::UnchargeUserPages(root_pml4, user_freed_pages * PAGE_SIZE);
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
