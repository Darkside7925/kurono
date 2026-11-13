#include "vmm.h"
#include "kmap.h"   // high-half direct map (satoru)
#include "../drivers/serial.h"
#include "../proc/cgroup.h"     // memory.max charge on user page maps (satoru)
#include "../proc/scheduler.h"  // current task -> charging cgroup (satoru)
#include "pmm.h"                // ram top - the hardened walks bound every table deref (satoru)
#include "../proc/smp.h"        // fork tlb shootdown (satoru)

//  virtual memory manager implementation (x86_64 4-level paging)

uint64_t KernelVMM::pml4_phys = 0;

// kernel image link range (a high-half address, outside the identity map).
extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;

// kmemx's compressed-leaf lifecycle callbacks (see vmm.h). null when the
// engine never initialized, so every hook site is a single pointer test.
// (satoru)
static KernelVMM::KmemxForgetHook   g_kmemx_forget   = nullptr;
static KernelVMM::KmemxProtHook     g_kmemx_prot     = nullptr;
static KernelVMM::KmemxPreCloneHook g_kmemx_preclone = nullptr;

void KernelVMM::SetKmemxHooks(KmemxForgetHook forget, KmemxProtHook prot,
                              KmemxPreCloneHook preclone) {
    g_kmemx_forget   = forget;
    g_kmemx_prot     = prot;
    g_kmemx_preclone = preclone;
}

// cgroup memory.max chokepoint. every user page (PTE_USER) mapped into a
// per-process address space passes through MapPage*/MapRange*InAddressSpace,
// so charging here covers the mmap / brk / demand-zero / stack paths without
// touching the syscall layer. the charge lands on the CURRENT task's cgroup
// (linux memcg charges the allocating task) but is recorded in a ledger keyed
// by the address-space root, so frees and teardown always uncharge the right
// account no matter which task performs them. tasks outside any cgroup
// (cgroup_id 0 - everything except kinit service units) short-circuit inside
// ChargeUserPages at zero cost. (satoru)
volatile uint64_t g_cpu_cr3[SMP_MAX_CPUS];   // [asdead], see NoteCr3 (satoru)
static inline uint32_t current_cgroup_id() {
    Process* cur = Scheduler::GetCurrentProcess();
    return cur ? cur->cgroup_id : 0;
}

// forwarded: the non-demoting leaf walk is defined with the kmemx primitives
// below, but the unmap path uses it for its hardened root/table bounds. (satoru)
static uint64_t* leaf_pte_nodemote(uint64_t root_phys, uint64_t virt_addr);

// every managed frame is reached through the high-half direct map when
// kurono.directmap=1, else through the boot identity map. (satoru)
static inline uint64_t* phys_to_virt(uint64_t phys) {
    return (uint64_t*)PMM::PhysToVirt(phys);
}

static inline uint64_t virt_to_phys(void* virt) {
    return PMM::VirtToPhys(virt);
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
    // the kernel image and the direct map live in the canonical high half,
    // outside the identity map. nothing user mode maps may land on them. (satoru)
    {
        uint64_t klo = (uint64_t)&kernel_start, khi = (uint64_t)&kernel_end;
        uint64_t end = va + bytes;
        if (end < va) end = ~0ULL;
        if (va < khi && klo < end) {
            SerialLogger::Log("[idalias] refused user map over the kernel link range\r\n");
            return true;
        }
        if (va < KERNEL_DIRECT_BASE + KERNEL_DIRECT_SIZE && KERNEL_DIRECT_BASE < end) {
            SerialLogger::Log("[idalias] refused user map over the kernel direct map\r\n");
            return true;
        }
    }
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

// kurono.ptaudit=1: debug guards for paging-structure frame lifetime. each
// 4kb frame used as a pml4/pdpt/pd/pt carries a flag plus the number of parent
// entries that point at it. the assertions are: a user leaf never maps a frame
// that is still a paging structure; a paging structure is never freed or
// quarantined while a parent still points at it; and a root that is torn down
// does not contain garbage entries. the state is a 16-bit word per frame,
// allocated only when the flag is on. (satoru)
static constexpr int AS_REG_MAX = 2048;
static uint64_t g_as_reg[AS_REG_MAX];
static uint64_t g_as_created = 0, g_as_destroyed = 0;

static constexpr uint16_t TAB_FLAG      = 0x8000;
static constexpr uint16_t TAB_LINK_MASK = 0x7FFF;
static uint16_t* g_tab = nullptr;
static uint64_t  g_tab_frames = 0;
static uint64_t  g_audit_ft[SMP_MAX_CPUS];   // table being freed through the tree path (satoru)

static inline uint64_t entry_phys_mask(uint64_t entry) {
    return entry & 0x000FFFFFFFFFF000ULL;
}
static inline uint16_t tab_get(uint64_t phys) {
    uint64_t idx = phys >> 12;
    return (g_tab && idx < g_tab_frames) ? g_tab[idx] : 0;
}
static inline bool tab_is(uint64_t phys) { return (tab_get(phys) & TAB_FLAG) != 0; }
static inline uint16_t tab_links(uint64_t phys) { return tab_get(phys) & TAB_LINK_MASK; }
static inline void tab_note_alloc(uint64_t phys) {
    uint64_t idx = phys >> 12;
    if (g_tab && idx < g_tab_frames) g_tab[idx] = TAB_FLAG;
}
static inline void tab_note_link(uint64_t phys) {
    uint64_t idx = phys >> 12;
    if (!g_tab || idx >= g_tab_frames) return;
    uint16_t n = g_tab[idx] & TAB_LINK_MASK;
    if (n < TAB_LINK_MASK) n++;
    g_tab[idx] = TAB_FLAG | n;
}
static inline void tab_note_free(uint64_t phys) {
    uint64_t idx = phys >> 12;
    if (g_tab && idx < g_tab_frames) g_tab[idx] = 0;
}
// kurono.pttrace=1: last leaf write and table event per frame, in a small
// event ring. when the audit catches a bad free it dumps the frame's history,
// naming the caller that first mapped it as a user leaf and the table alloc
// that reused it. only armed with ptaudit; in-memory, no serial spam. (satoru)
enum { PT_LEAF = 1, PT_CLONE, PT_PROT, PT_KMEMX, PT_TABALLOC, PT_COPY, PT_QUAR };
struct PtEvent {
    uint64_t seq, phys, va, root, ra0, ra1;
    uint32_t flags, kind;
};
static constexpr int PT_RING = 4096;
static PtEvent  g_pt_ev[PT_RING];
static uint64_t g_pt_seq = 0;
static uint32_t* g_leaf_seq = nullptr;
static uint32_t* g_tab_seq = nullptr;

static uint64_t ptw_event(uint32_t kind, uint64_t phys, uint64_t va,
                          uint64_t root, uint64_t flags) {
    if (!g_leaf_seq) return 0;
    uint64_t i = phys >> 12;
    if (i >= g_tab_frames) return 0;
    uint64_t q = __atomic_add_fetch(&g_pt_seq, 1, __ATOMIC_RELAXED);
    if (kind == PT_TABALLOC) g_tab_seq[i] = (uint32_t)q;
    else                     g_leaf_seq[i] = (uint32_t)q;
    PtEvent& e = g_pt_ev[q & (PT_RING - 1)];
    e.seq = q; e.phys = phys; e.va = va; e.root = root;
    e.ra0 = (uint64_t)(uintptr_t)__builtin_return_address(0);
    e.ra1 = 0;   // ra(1) is not reliable without frame pointers (satoru)
    e.flags = (uint32_t)flags; e.kind = kind;
    return q;
}

static uint32_t* g_quar_seq = nullptr;      // last enqueue seq per frame (satoru)
static uint32_t* g_quar_ra  = nullptr;      // enqueue caller per frame (satoru)
static uint8_t*  g_quar_live = nullptr;     // frame is sitting in the quarantine (satoru)
static uint64_t  g_audit_fq[SMP_MAX_CPUS];  // queue release in progress (satoru)

void KernelVMM::PtQuarantineEvent(uint64_t phys, uint64_t caller) {
    uint64_t q = ptw_event(PT_QUAR, phys, caller, 0, 0);
    uint64_t i = phys >> 12;
    if (g_quar_seq && i < g_tab_frames && q) {
        g_quar_seq[i] = (uint32_t)q;
        g_quar_ra[i]  = (uint32_t)caller;
        g_quar_live[i] = 1;
    }
}
void KernelVMM::PtQuarantineRelease(uint64_t phys) {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    g_audit_fq[cpu] = phys;
}
void KernelVMM::PtQuarantineDone(uint64_t phys) {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    g_audit_fq[cpu] = 0;
    uint64_t i = phys >> 12;
    if (g_quar_live && i < g_tab_frames) g_quar_live[i] = 0;
}

void KernelVMM::PtHistory(uint64_t phys) {
    if (!g_leaf_seq) return;
    uint64_t i = phys >> 12;
    if (i >= g_tab_frames) return;
    uint32_t ls = g_leaf_seq[i], ts = g_tab_seq[i];
    SerialLogger::Log("[pthist] phys=0x"); SerialLogger::LogHex64(phys);
    SerialLogger::Log(" leaf_seq="); SerialLogger::LogDec(ls);
    SerialLogger::Log(" tab_seq="); SerialLogger::LogDec(ts);
    if (g_quar_seq) {
        SerialLogger::Log(" quar_seq="); SerialLogger::LogDec(g_quar_seq[i]);
        SerialLogger::Log(" quar_ra=0x"); SerialLogger::LogHex(g_quar_ra[i]);
        SerialLogger::Log(" quar_live="); SerialLogger::LogDec(g_quar_live[i]);
    }
    SerialLogger::Log("\r\n");
    int shown = 0;
    for (int n = 0; n < PT_RING && shown < 10; n++) {
        const PtEvent& e = g_pt_ev[n];
        if (!e.seq) continue;
        bool hit = (e.phys == phys) || (ls && (uint32_t)e.seq == ls) ||
                   (ts && (uint32_t)e.seq == ts);
        if (!hit) continue;
        const char* k = e.kind == PT_LEAF ? "leaf" : e.kind == PT_CLONE ? "clone" :
                        e.kind == PT_PROT ? "prot" : e.kind == PT_KMEMX ? "kmemx" :
                        e.kind == PT_TABALLOC ? "taballoc" : e.kind == PT_QUAR ? "quar" : "copy";
        SerialLogger::Log("[pthist]  "); SerialLogger::Log(k);
        SerialLogger::Log(" seq="); SerialLogger::LogDec((uint32_t)e.seq);
        SerialLogger::Log(" va=0x"); SerialLogger::LogHex64(e.va);
        SerialLogger::Log(" root=0x"); SerialLogger::LogHex64(e.root);
        SerialLogger::Log(" fl=0x"); SerialLogger::LogHex(e.flags);
        SerialLogger::Log(" ra0=0x"); SerialLogger::LogHex64(e.ra0);
        SerialLogger::Log(" ra1=0x"); SerialLogger::LogHex64(e.ra1);
        SerialLogger::Log("\r\n");
        shown++;
    }
}

// L1 page-table growth trace (kurono.vmatrace=1): a per-cpu map origin is read
// when a user paging structure is allocated, so the census can tell whether the
// 2 MiB VAs march upward or scatter and which mapping path asked. no behaviour
// change when off; the state is a bounded side table. (satoru)
bool g_kvmm_vmtrace = false;
static uint8_t g_vm_origin[SMP_MAX_CPUS];
static uint64_t g_vm_events = 0, g_vm_lvl[5] = {}, g_vm_org[KernelVMM::VMO_KIND] = {};
static uint64_t g_vm_min2m = ~0ull, g_vm_max2m = 0, g_vm_back = 0, g_vm_forward = 0;
static constexpr int VM_SEEN = 8192;
static uint64_t g_vm_seen[VM_SEEN];
static uint64_t g_vm_newva = 0, g_vm_reuseva = 0;
struct VmTabEv { uint64_t va2m; uint8_t lvl, org; };
static constexpr int VM_RING = 4096;
static VmTabEv g_vm_ring[VM_RING];
static uint32_t g_vm_ring_n = 0, g_vm_ring_i = 0;

void KernelVMM::SetVmTrace(bool on) { g_kvmm_vmtrace = on; }
void KernelVMM::VmOriginSet(uint8_t o) {
    uint32_t c = SMP::CpuIndex();
    if (c >= SMP_MAX_CPUS) c = 0;
    g_vm_origin[c] = o;
}
uint8_t KernelVMM::VmOriginGet() {
    uint32_t c = SMP::CpuIndex();
    if (c >= SMP_MAX_CPUS) c = 0;
    return g_vm_origin[c];
}
void KernelVMM::VmTableNote(uint64_t virt_addr, int level) {
    if (!g_kvmm_vmtrace) return;
    uint64_t va2m = virt_addr & ~0x1FFFFFULL;
    uint8_t org = VmOriginGet();
    if (org >= VMO_KIND) org = VMO_OTHER;
    g_vm_events++;
    if (level >= 1 && level <= 4) g_vm_lvl[level]++;
    g_vm_org[org]++;
    if (va2m < g_vm_min2m) g_vm_min2m = va2m;
    if (va2m >= g_vm_max2m) { g_vm_max2m = va2m; g_vm_forward++; }
    else g_vm_back++;
    // fresh 2 MiB VA vs a VA seen before (open addressing, trace only). (satoru)
    {
        uint64_t h = (va2m >> 21) & (VM_SEEN - 1);
        bool found = false;
        for (int k = 0; k < 16; k++) {
            uint64_t slot = g_vm_seen[(h + k) & (VM_SEEN - 1)];
            if (slot == va2m) { found = true; break; }
            if (slot == 0) { g_vm_seen[(h + k) & (VM_SEEN - 1)] = va2m; break; }
        }
        if (found) g_vm_reuseva++; else g_vm_newva++;
    }
    g_vm_ring[g_vm_ring_i].va2m = va2m;
    g_vm_ring[g_vm_ring_i].lvl = (uint8_t)level;
    g_vm_ring[g_vm_ring_i].org = org;
    g_vm_ring_i = (g_vm_ring_i + 1) % VM_RING;
    if (g_vm_ring_n < VM_RING) g_vm_ring_n++;
}
void KernelVMM::VmTraceDump() {
    if (!g_kvmm_vmtrace) return;
    SerialLogger::Log("[vmtrace] tabs pml4="); SerialLogger::LogDec((int)g_vm_lvl[1]);
    SerialLogger::Log(" pdpt="); SerialLogger::LogDec((int)g_vm_lvl[2]);
    SerialLogger::Log(" pd="); SerialLogger::LogDec((int)g_vm_lvl[3]);
    SerialLogger::Log(" pt="); SerialLogger::LogDec((int)g_vm_lvl[4]);
    SerialLogger::Log(" events="); SerialLogger::LogDec((int)g_vm_events);
    SerialLogger::Log(" min2m="); SerialLogger::LogHex64(g_vm_min2m == ~0ull ? 0 : g_vm_min2m);
    SerialLogger::Log(" max2m="); SerialLogger::LogHex64(g_vm_max2m);
    SerialLogger::Log(" newva="); SerialLogger::LogDec((int)g_vm_newva);
    SerialLogger::Log(" reuseva="); SerialLogger::LogDec((int)g_vm_reuseva);
    SerialLogger::Log(" back="); SerialLogger::LogDec((int)g_vm_back);
    SerialLogger::Log(" forward="); SerialLogger::LogDec((int)g_vm_forward);
    uint64_t bins[8] = {};
    if (g_vm_max2m > g_vm_min2m) {
        uint64_t n2m = (g_vm_max2m - g_vm_min2m) / 0x200000ull + 1ull;
        for (uint32_t k = 0; k < g_vm_ring_n; k++) {
            uint64_t idx = (g_vm_ring[k].va2m - g_vm_min2m) / 0x200000ull;
            uint64_t b = (idx * 8ull) / n2m;
            bins[b > 7 ? 7 : b]++;
        }
    } else if (g_vm_ring_n) bins[0] = g_vm_ring_n;
    SerialLogger::Log(" hist=");
    for (int k = 0; k < 8; k++) { SerialLogger::LogDec((int)bins[k]); SerialLogger::Log(k == 7 ? "\r\n" : ","); }
    static const char* kOrg[VMO_KIND] = {"other","anon","fd","fault","brk","mremap","stack"};
    SerialLogger::Log("[vmtrace] origin");
    for (int k = 0; k < VMO_KIND; k++) {
        SerialLogger::Log(" "); SerialLogger::Log(kOrg[k]);
        SerialLogger::Log("="); SerialLogger::LogDec((int)g_vm_org[k]);
    }
    SerialLogger::Log(" last=");
    for (int k = 0; k < 4 && k < (int)g_vm_ring_n; k++) {
        const VmTabEv& e = g_vm_ring[(g_vm_ring_i + VM_RING - 1 - k) % VM_RING];
        SerialLogger::LogHex64(e.va2m);
        SerialLogger::Log("/"); SerialLogger::LogDec(e.lvl);
        SerialLogger::Log(k == 3 ? "\r\n" : ",");
    }
}
static void audit_hit(const char* what, uint64_t va, uint64_t phys,
                      uint16_t links, uint64_t ra) {
    static uint32_t s_hits = 0;
    s_hits++;
    if (s_hits <= 32) {
        SerialLogger::Log("[PTAUDIT] "); SerialLogger::Log(what);
        SerialLogger::Log(" va=0x"); SerialLogger::LogHex64(va);
        SerialLogger::Log(" phys=0x"); SerialLogger::LogHex64(phys);
        SerialLogger::Log(" links="); SerialLogger::LogDec(links);
        SerialLogger::Log(" ra=0x"); SerialLogger::LogHex64(ra);
        SerialLogger::Log("\r\n");
    } else if (s_hits == 33) {
        SerialLogger::Log("[PTAUDIT] further hits suppressed\r\n");
    }
}

// a table free done by free_user_tree/destroy sets the per-cpu marker; any
// other free of a frame that is still a paging structure is an alias. (satoru)
static bool audit_free_guard(uint64_t phys, uint64_t ra) {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    {
        uint64_t i = phys >> 12;
        if (g_quar_live && i < g_tab_frames && g_quar_live[i] &&
            g_audit_fq[cpu] != phys) {
            audit_hit("free-queued", 0, phys, 0, ra);
            // report only: refusing the free used to mask the double owner this
            // audit exists to catch, so it is left to the real accounting. (satoru)
            return false;
        }
    }
    uint16_t v = tab_get(phys);
    if (!(v & TAB_FLAG)) return false;
    uint16_t links = v & TAB_LINK_MASK;
    if (g_audit_ft[cpu] == phys) {
        if (links > 1) {
            audit_hit("free-shared", 0, phys, links, ra);
            return true;   // a parent in another root still points here (satoru)
        }
        tab_note_free(phys);
        return false;
    }
    audit_hit("free-nontable", 0, phys, links, ra);
    return true;   // leaf/quarantine path trying to free a live paging structure (satoru)
}

bool KernelVMM::IsTableFrame(uint64_t phys) { return tab_is(phys); }

// a frame still owned by the quarantine must not be bulk-cleared: the queue's
// deferred FreeFrame is the reference that keeps it alive. (satoru)
[[maybe_unused]] static bool audit_bulk_free_guard(uint64_t phys) {
    uint64_t i = phys >> 12;
    if (!g_quar_live || i >= g_tab_frames || !g_quar_live[i]) return false;
    audit_hit("bulk-free-queued", 0, phys, 0,
              (uint64_t)(uintptr_t)__builtin_return_address(0));
    return true;
}

// does any live user pt still have a 4kb leaf pointing at phys? the reverse of
// the map-time assert: a frame that a user can still write must never become a
// paging structure. racy read-only walk, bounded. (satoru)
static constexpr uint64_t AUDIT_SCAN_BUDGET = 1ULL << 20;
static bool audit_leaf_walk(uint64_t table_phys, int level, uint64_t target,
                            uint64_t va_base, uint64_t* out_va, uint64_t* budget) {
    if (--*budget == 0) return false;
    const uint64_t* t = phys_to_virt(table_phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
        uint64_t c = entry_phys_mask(e);
        if (c / PAGE_SIZE >= PMM::GetPhysTop() / PAGE_SIZE) continue;
        uint64_t va = va_base | ((uint64_t)i << (12 + 9 * (level - 1)));
        if (level > 1) {
            if (e & PTE_HUGE) continue;
            if (audit_leaf_walk(c, level - 1, target, va, out_va, budget)) return true;
        } else if (!(e & PTE_HUGE) && c == target) {
            *out_va = va;
            return true;
        }
    }
    return false;
}
static bool audit_user_leaf(uint64_t phys, uint64_t* out_root, uint64_t* out_va) {
    uint64_t budget = AUDIT_SCAN_BUDGET;
    uint64_t nframes = PMM::GetPhysTop() / PAGE_SIZE;
    uint64_t kroot = KernelVMM::GetPML4();
    if (kroot && kroot / PAGE_SIZE < nframes) {
        if (audit_leaf_walk(kroot, 4, phys, 0, out_va, &budget)) {
            *out_root = kroot;
            return true;
        }
    }
    for (int i = 0; i < AS_REG_MAX; i++) {
        uint64_t root = __atomic_load_n(&g_as_reg[i], __ATOMIC_RELAXED);
        if (!root || root / PAGE_SIZE >= nframes) continue;
        if (audit_leaf_walk(root, 4, phys, 0, out_va, &budget)) {
            *out_root = root;
            return true;
        }
        if (budget == 0) break;
    }
    return false;
}

// allocate a zeroed 4kb page for a page table
static uint64_t alloc_table_page() {
    uint64_t frame = 0;
    for (int tries = 0; tries < 4; tries++) {
        frame = PMM::AllocFrame();
        if (frame == 0) {
            SerialLogger::Log("VMM: FATAL - cannot allocate page table frame!\r\n");
            return 0;
        }
        if (g_tab) {
            uint16_t v = tab_get(frame);
            if ((v & TAB_FLAG) && (v & TAB_LINK_MASK)) {
                audit_hit("alloc-linked", 0, frame, v & TAB_LINK_MASK,
                          (uint64_t)(uintptr_t)__builtin_return_address(0));
                KernelVMM::PtHistory(frame);
                continue;   // leak it: still linked from a root we did not free (satoru)
            }
            if (v & TAB_FLAG) tab_note_free(frame);   // stale marker, safe to reuse (satoru)
            uint64_t ur = 0, uva = 0;
            if (audit_user_leaf(frame, &ur, &uva)) {
                audit_hit("alloc-userleaf", uva, frame, 0,
                          (uint64_t)(uintptr_t)__builtin_return_address(0));
                KernelVMM::PtHistory(frame);
                continue;   // a user leaf can still write it: never a table (satoru)
            }
        }
        break;
    }
    if (frame == 0) return 0;
    // zero it (identity-mapped, so we can just use the physical address)
    uint8_t* p = (uint8_t*)phys_to_virt(frame);
    for (int i = 0; i < 4096; i++) p[i] = 0;
    if (g_tab) tab_note_alloc(frame);
    ptw_event(PT_TABALLOC, frame, 0, 0, 0);
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
    ptw_event(PT_COPY, new_table, src_phys, 0, 0);

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
    {
        uint32_t cpu = SMP::CpuIndex();
        if (cpu >= SMP_MAX_CPUS) cpu = 0;
        uint64_t prev = g_audit_ft[cpu];
        g_audit_ft[cpu] = table_phys;
        PMM::FreeFrame(table_phys);
        g_audit_ft[cpu] = prev;
    }
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
            tab_note_link(cloned_child);
            continue;
        }

        if (entry & PTE_HUGE) {
            free_user_tree(new_table_phys, level);
            return 0;
        }

        uint64_t phys = entry_phys(entry);
        uint64_t flags = entry_flags(entry);
        PMM::RetainFrame(phys);
        if (flags & PTE_USER) ptw_event(PT_CLONE, phys, 0, table_phys, flags);

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

static uint64_t ensure_table_in_root(uint64_t* parent, uint16_t index, uint64_t flags,
                                     bool* out_new) {
    if (out_new) *out_new = false;
    uint64_t entry = parent[index];
    uint64_t required_flags = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER) required_flags |= PTE_USER;

    if (!(entry & PTE_PRESENT)) {
        uint64_t new_table = alloc_table_page();
        if (!new_table) return 0;
        parent[index] = new_table | required_flags;
        if (flags & PTE_USER) tab_note_link(new_table);
        if (out_new) *out_new = true;
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
        tab_note_link(copied_table);
        if (out_new) *out_new = true;
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
    bool vm_new = false;
    uint64_t pdpt_phys = ensure_table_in_root(pml4, p4i, flags, &vm_new);
    if (vm_new && (flags & PTE_USER)) KernelVMM::VmTableNote(virt_addr, 2);
    if (!pdpt_phys) return false;
    uint64_t* pdpt = phys_to_virt(pdpt_phys);

    uint16_t p3i = pdpt_index(virt_addr);
    if (pdpt[p3i] & PTE_HUGE) {
        SerialLogger::Log("VMM: WARNING - cannot map over 1GB huge page\r\n");
        return false;
    }
    uint64_t pd_phys = ensure_table_in_root(pdpt, p3i, flags, &vm_new);
    if (vm_new && (flags & PTE_USER)) KernelVMM::VmTableNote(virt_addr, 3);
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
        if (flags & PTE_USER) KernelVMM::VmTableNote(virt_addr, 4);
        uint64_t* new_pt = phys_to_virt(new_pt_phys);
        uint64_t leaf_flags = (huge_flags & ~PTE_HUGE) | PTE_PRESENT;
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (huge_base + (uint64_t)i * PAGE_SIZE) | leaf_flags;
        }
        pd[p2i] = new_pt_phys | PTE_PRESENT | PTE_WRITABLE
                              | ((flags & PTE_USER) ? PTE_USER : 0);
        if (flags & PTE_USER) tab_note_link(new_pt_phys);
        if (out_demoted) *out_demoted = true;
    }
    uint64_t pt_phys = ensure_table_in_root(pd, p2i, flags, &vm_new);
    if (vm_new && (flags & PTE_USER)) KernelVMM::VmTableNote(virt_addr, 4);
    if (!pt_phys) return false;
    uint64_t* pt = phys_to_virt(pt_phys);

    uint16_t p1i = pt_index(virt_addr);
    // mapping over a kmemx-compressed leaf: the old page's bytes are in the
    // pool but the caller is replacing this va, so drop the engine's copy
    // now. otherwise a later fork/drain could restore stale bytes here.
    // (satoru)
    if (g_kmemx_forget && !(pt[p1i] & PTE_PRESENT) &&
        (pt[p1i] & PTE_KMEMX_COMPRESSED)) {
        g_kmemx_forget(root_phys, virt_addr);
    }
    if (flags & PTE_USER)
        ptw_event(PT_LEAF, phys_addr, virt_addr, root_phys, flags);
    if ((flags & PTE_USER) && tab_is(phys_addr)) {
        audit_hit("map-table", virt_addr & ~0xFFFULL, phys_addr & ~0xFFFULL,
                  tab_links(phys_addr), (uint64_t)(uintptr_t)__builtin_return_address(0));
        KernelVMM::PtHistory(phys_addr);
        return false;   // a user leaf may never alias a live paging structure (satoru)
    }
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

    // hardened walk (see leaf_pte_nodemote): a stale/recycled root must not
    // deref a wild table pointer from an unmap issued with a racy root. (satoru)
    uint64_t* pte = leaf_pte_nodemote(root_phys, virt_addr);
    if (!pte) return false;
    if (!(*pte & PTE_PRESENT)) {
        // a kmemx-compressed leaf is not-present but logically still owns a
        // pool blob. an unmap/discard drops the mapping, so the blob goes too;
        // do not decompress it back over a va the caller is retiring. (satoru)
        if ((*pte & PTE_KMEMX_COMPRESSED) && g_kmemx_forget)
            g_kmemx_forget(root_phys, virt_addr);
        if (*pte & PTE_KMEMX_COMPRESSED) *pte = 0;
        return false;
    }

    bool user_freed = false;
    if (free_frame) {
        // entry_phys: an NX leaf freed nothing with the old ~0xFFF mask (the
        // single-cpu munmap / madvise path). (satoru)
        PMM::FreeFrame(entry_phys(*pte));
        user_freed = (*pte & PTE_USER) != 0;
    }
    *pte = 0;
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
    if (!(pt[p1i] & PTE_PRESENT)) {
        // a compressed leaf has no live pte to rewrite, but mprotect still has
        // to reach the page: hand the new bits to the engine's metadata so the
        // decompression fault restores them. report success so the caller does
        // not treat the page as unmapped. (satoru)
        if ((pt[p1i] & PTE_KMEMX_COMPRESSED) && g_kmemx_prot) {
            g_kmemx_prot(root_phys, virt_addr, new_flags);
            return true;
        }
        return false;
    }

    // keep the physical frame, replace every permission/attr bit except the
    // shared marker: mprotect on a MAP_SHARED view must not make the next fork
    // turn it copy-on-write. (satoru)
    uint64_t phys = entry_phys(pt[p1i]);
    uint64_t keep = pt[p1i] & PTE_SHARED;
    if (new_flags & PTE_USER) ptw_event(PT_PROT, phys, virt_addr, root_phys, new_flags);
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

    if (PMM::PtAudit()) {
        uint64_t frames = PMM::GetPhysTop() / PAGE_SIZE;
        uint64_t bytes  = frames * sizeof(uint16_t);
        g_tab = (uint16_t*)PMM::AllocBytes(bytes);
        if (g_tab) {
            g_tab_frames = frames;
            memset(g_tab, 0, bytes);
            PMM::SetFreeGuard(audit_free_guard);
            // the bulk-free guard stays uninstalled: it masked the double owner
            // instead of reporting it (see section 10 of the lowram report). (satoru)
            SerialLogger::Log("[PTAUDIT] armed frames=");
            SerialLogger::LogDec((uint32_t)(frames > 0xFFFFFFFFull ? 0xFFFFFFFFu : frames));
            SerialLogger::Log(" kb=");
            SerialLogger::LogDec((uint32_t)(bytes >> 10));
            SerialLogger::Log("\r\n");
            if (PMM::PtTrace()) {
                uint64_t qbytes = frames * sizeof(uint32_t);
                g_leaf_seq = (uint32_t*)PMM::AllocBytes(qbytes);
                g_tab_seq  = (uint32_t*)PMM::AllocBytes(qbytes);
                g_quar_seq = (uint32_t*)PMM::AllocBytes(qbytes);
                g_quar_ra  = (uint32_t*)PMM::AllocBytes(qbytes);
                g_quar_live = (uint8_t*)PMM::AllocBytes(frames);
                if (g_leaf_seq && g_tab_seq && g_quar_seq && g_quar_ra && g_quar_live) {
                    memset(g_leaf_seq, 0, qbytes);
                    memset(g_tab_seq, 0, qbytes);
                    memset(g_quar_seq, 0, qbytes);
                    memset(g_quar_ra, 0, qbytes);
                    memset(g_quar_live, 0, frames);
                    SerialLogger::Log("[PTTRACE] armed per-frame history\r\n");
                } else {
                    g_leaf_seq = nullptr;
                    g_tab_seq = nullptr;
                    g_quar_seq = nullptr;
                    g_quar_ra = nullptr;
                    g_quar_live = nullptr;
                    SerialLogger::Log("[PTTRACE] arm FAILED\r\n");
                }
            }
        } else {
            SerialLogger::Log("[PTAUDIT] arm FAILED (no memory for the frame map)\r\n");
        }
    }
}

// map [0, pmm phys top) at DIRECT_BASE + phys with 2 mb pages. the tables are
// allocated with AllocFrame (physical frame numbers) and written through the
// identity map, which is still what phys_to_virt uses until the last line
// enables the direct map for the allocator. (satoru)
bool KernelVMM::InitDirectMap() {
    if (!PMM::DirectMapRequested()) return false;
    uint64_t top = PMM::GetPhysTop();
    if (!top) return false;
    uint64_t* pml4 = phys_to_virt(pml4_phys);
    uint16_t idx = pml4_index(KERNEL_DIRECT_BASE);
    if (pml4[idx] & PTE_PRESENT) {
        SerialLogger::Log("VMM: direct map slot already in use, disabled\r\n");
        return false;
    }
    uint64_t pdpt_phys = PMM::AllocFrame();
    if (!pdpt_phys) { SerialLogger::Log("VMM: direct map pdpt alloc failed\r\n"); return false; }
    uint64_t* pdpt = phys_to_virt(pdpt_phys);
    for (int i = 0; i < 512; i++) pdpt[i] = 0;
    pml4[idx] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    uint64_t gbs = (top + ((1ULL << 30) - 1)) >> 30;
    if (gbs > 512) gbs = 512;
    for (uint64_t g = 0; g < gbs; g++) {
        uint64_t pd_phys = PMM::AllocFrame();
        if (!pd_phys) { SerialLogger::Log("VMM: direct map pd alloc failed\r\n"); return false; }
        uint64_t* pd = phys_to_virt(pd_phys);
        for (int j = 0; j < 512; j++) {
            uint64_t pa = (g << 30) + ((uint64_t)j << 21);
            pd[j] = (pa < top) ? (pa | 0x83ULL) : 0;
        }
        pdpt[g] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
    }
    PMM::EnableDirectMap();
    SerialLogger::Log("VMM: direct map 0x");
    SerialLogger::LogHex64(KERNEL_DIRECT_BASE);
    SerialLogger::Log(" covers 0x");
    SerialLogger::LogHex64(top);
    SerialLogger::Log(" (");
    SerialLogger::LogDec((uint32_t)gbs);
    SerialLogger::Log(" x 1gb)\r\n");
    return true;
}

// kurono.memcensus=1: registry of live user address spaces, so the census can
// tell which user frames no address space maps any more. lock-free slots; only
// filled while the census is on. created/destroyed counts are always kept. (satoru)
static void as_reg_add(uint64_t root) {
    __atomic_add_fetch(&g_as_created, 1, __ATOMIC_RELAXED);
    if (!PMM::CensusOn() && !PMM::PtAudit()) return;
    for (int i = 0; i < AS_REG_MAX; i++) {
        uint64_t z = 0;
        if (__atomic_compare_exchange_n(&g_as_reg[i], &z, root, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return;
    }
}
static void as_reg_del(uint64_t root) {
    __atomic_add_fetch(&g_as_destroyed, 1, __ATOMIC_RELAXED);
    if (!PMM::CensusOn() && !PMM::PtAudit()) return;
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

// copy one level of a kernel page-table tree into fresh pmm frames. level 4 is
// the pml4, 1 is a leaf page table. huge entries and all leaves are copied
// verbatim; everything else is cloned one level down. (satoru)
static uint64_t clone_kernel_table(uint64_t src_phys, int level) {
    uint64_t dst = alloc_table_page();
    if (!dst) return 0;
    const uint64_t* s = phys_to_virt(src_phys);
    uint64_t* d = phys_to_virt(dst);
    for (int i = 0; i < 512; i++) {
        uint64_t e = s[i];
        if (!(e & PTE_PRESENT) || level == 1 || (e & PTE_HUGE)) { d[i] = e; continue; }
        uint64_t child = clone_kernel_table(entry_phys(e), level - 1);
        if (!child) {
            SerialLogger::Log("VMM: kernel root copy ran out of frames\r\n");
            return 0;
        }
        d[i] = child | entry_flags(e);
    }
    return dst;
}

void KernelVMM::RelocateKernelRoot() {
    if (!pml4_phys) return;
    uint64_t old = pml4_phys;
    uint64_t fresh = clone_kernel_table(old, 4);
    if (!fresh) {
        SerialLogger::Log("VMM: kernel root relocation failed, keeping the boot tables\r\n");
        return;
    }
    pml4_phys = fresh;
    asm volatile("mov %0, %%cr3" : : "r"(fresh) : "memory");
    SerialLogger::Log("VMM: kernel root relocated to 0x");
    SerialLogger::LogHex(fresh);
    SerialLogger::Log(" (boot tables at 0x");
    SerialLogger::LogHex(old);
    SerialLogger::Log(")\r\n");
}

// the ap trampoline does `mov eax,[P_CR3]; mov cr3,eax` in 32-bit mode before
// entering long mode, so the root frame must be addressable with 32 bits. the
// boot tables always were; the relocated copy is not guaranteed to be. copy
// again with a below-4gb root frame (the table below it may stay anywhere).
uint64_t KernelVMM::EnsureRootBelow4GB() {
    if (!pml4_phys || (pml4_phys >> 32) == 0) return pml4_phys;
    uint64_t old = pml4_phys;
    uint64_t fresh = PMM::AllocContiguousBelow(1, 0x100000000ULL);
    if (!fresh) {
        SerialLogger::Log("VMM: no root frame below 4 gb for the AP trampoline\r\n");
        return old;
    }
    // copy the old root contents into the low frame, then deep-copy every
    // present child so the two roots share no tables (the old ones may live
    // above 4 gb, which is fine for the cpu but not for a later teardown).
    uint64_t* dst = phys_to_virt(fresh);
    const uint64_t* src = phys_to_virt(old);
    for (int i = 0; i < 512; i++) dst[i] = 0;
    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_PRESENT)) continue;
        if (e & PTE_HUGE) { dst[i] = e; continue; }
        uint64_t child = clone_kernel_table(entry_phys(e), 3);
        if (!child) {
            SerialLogger::Log("VMM: below-4gb root copy failed, keeping the old root\r\n");
            return old;
        }
        dst[i] = child | entry_flags(e);
    }
    pml4_phys = fresh;
    asm volatile("mov %0, %%cr3" : : "r"(fresh) : "memory");
    SerialLogger::Log("VMM: kernel root moved below 4 gb: 0x");
    SerialLogger::LogHex(fresh);
    SerialLogger::Log("\r\n");
    return fresh;
}

// live-table write-combining. pat entry 1 is pwt=1,pcd=0,pat=0; for a 2mb/1gb
// entry the pat bit is bit 12, for a 4kb leaf it is bit 7. creates a 2mb
// identity leaf in the live root when the range was not mapped (this replaces
// the old extra_pd path, which wrote the abandoned boot pdpt). (satoru)
uint64_t KernelVMM::SetIdentityWriteCombining(uint64_t phys, size_t bytes) {
    if (!pml4_phys || bytes == 0) return 0;
    uint64_t start = phys & ~(uint64_t)0x1FFFFF;
    uint64_t end   = (phys + bytes + 0x1FFFFF) & ~(uint64_t)0x1FFFFF;
    if (end < start) return 0;
    uint64_t chunks = 0;
    uint64_t* pml4 = phys_to_virt(pml4_phys);
    for (uint64_t addr = start; addr < end; addr += 0x200000) {
        uint16_t p4i = pml4_index(addr);
        uint16_t p3i = pdpt_index(addr);
        uint16_t p2i = pd_index(addr);
        if (!(pml4[p4i] & PTE_PRESENT)) {
            uint64_t t = alloc_table_page();
            if (!t) break;
            pml4[p4i] = t | PTE_PRESENT | PTE_WRITABLE;
        }
        uint64_t* pdpt = phys_to_virt(entry_phys(pml4[p4i]));
        if (!(pdpt[p3i] & PTE_PRESENT)) {
            uint64_t t = alloc_table_page();
            if (!t) break;
            pdpt[p3i] = t | PTE_PRESENT | PTE_WRITABLE;
        }
        if (pdpt[p3i] & PTE_HUGE) {
            // 1 gib page covers this whole gb; one edit is enough
            pdpt[p3i] &= ~((1ULL << 4) | (1ULL << 12));
            pdpt[p3i] |=  (1ULL << 3);
            chunks += 1;
            addr = (addr & ~(uint64_t)((1ULL << 30) - 1)) + (1ULL << 30) - 0x200000;
            continue;
        }
        uint64_t* pd = phys_to_virt(entry_phys(pdpt[p3i]));
        uint64_t e = pd[p2i];
        if (!(e & PTE_PRESENT)) {
            pd[p2i] = (addr & ~(uint64_t)0x1FFFFF) | 0x83ULL | (1ULL << 3);
            chunks++;
            continue;
        }
        if (e & PTE_HUGE) {
            pd[p2i] = (e & ~((1ULL << 4) | (1ULL << 12))) | (1ULL << 3);
            chunks++;
            continue;
        }
        uint64_t* pt = phys_to_virt(entry_phys(e));
        for (int i = 0; i < 512; i++) {
            uint64_t le = pt[i];
            if (!(le & PTE_PRESENT)) continue;
            le &= ~((1ULL << 3) | (1ULL << 4) | (1ULL << 7));
            le |= (1ULL << 3);
            pt[i] = le;
        }
        chunks++;
    }
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    // the framebuffer was written write-back before this (early boot text): write
    // those dirty lines back now, or a later eviction overwrites newer wc data;
    // and drop the old translation on every online cpu, not only this one. (satoru)
    asm volatile("wbinvd" ::: "memory");
    SMP::BroadcastTlbFlush();
    asm volatile("sfence" ::: "memory");
    return chunks;
}

uint64_t KernelVMM::QueryRawEntry(uint64_t va, bool* huge) {
    if (huge) *huge = false;
    if (!pml4_phys) return 0;
    const uint64_t* pml4 = phys_to_virt(pml4_phys);
    uint64_t e = pml4[pml4_index(va)];
    if (!(e & PTE_PRESENT)) return 0;
    const uint64_t* pdpt = phys_to_virt(entry_phys(e));
    e = pdpt[pdpt_index(va)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) { if (huge) *huge = true; return e; }
    const uint64_t* pd = phys_to_virt(entry_phys(e));
    e = pd[pd_index(va)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) { if (huge) *huge = true; return e; }
    const uint64_t* pt = phys_to_virt(entry_phys(e));
    e = pt[pt_index(va)];
    if (!(e & PTE_PRESENT)) return 0;
    return e;
}

uint64_t KernelVMM::CreateAddressSpace() {
    uint64_t new_root = alloc_table_page();
    if (!new_root) return 0;
    as_reg_add(new_root);

    uint64_t* src = phys_to_virt(pml4_phys);
    uint64_t* dst = phys_to_virt(new_root);
    for (int index = 0; index < 512; index++) {
        dst[index] = src[index];
        if ((src[index] & PTE_PRESENT) && !(src[index] & PTE_HUGE))
            tab_note_link(entry_phys(src[index]));
    }

    return new_root;
}

uint64_t KernelVMM::CloneAddressSpace(uint64_t source_root_pml4) {
    if (!source_root_pml4) return 0;

    // fork copies present leaves. a kmemx-compressed leaf is not present, so
    // the child would see an empty page and lose the parent's contents. let
    // the engine fault every compressed page of this space back in first; if
    // it cannot (out of frames), refuse the fork rather than hand the child a
    // silently truncated address space. (satoru)
    if (g_kmemx_preclone && !g_kmemx_preclone(source_root_pml4)) {
        SerialLogger::Log("[kmemx] fork refused: could not restore compressed pages\r\n");
        return 0;
    }

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
        tab_note_link(cloned_subtree);
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
// the kld display bridge uses this to set its instance's as_dead fact before
// the pml4 (and with it the mapping refs) is freed. (satoru)
static void (*g_as_destroy_hook2)(uint64_t root) = nullptr;
void KernelVMM::SetAddressSpaceDestroyHook2(void (*hook)(uint64_t root)) { g_as_destroy_hook2 = hook; }

void KernelVMM::DestroyAddressSpace(uint64_t root_pml4) {
    if (!root_pml4 || root_pml4 == pml4_phys) return;
    {   // [asdead] a cpu whose loaded cr3 is this root walks freed tables once
        // they recycle. log it (and who), the teardown itself is unchanged. (satoru)
        static volatile uint32_t s_asdead = 0;
        uint32_t me = SMP::CpuIndex();
        for (uint32_t c = 0; c < SMP::CpuCount() && c < SMP_MAX_CPUS; c++) {
            if (g_cpu_cr3[c] != (root_pml4 & ~0xFFFULL)) continue;
            if (__atomic_fetch_add(&s_asdead, 1, __ATOMIC_RELAXED) >= 24) break;
            PerCpu* pcx = SMP::ByIndex(c);
            Process* cp = c == 0 ? Scheduler::current_process : (pcx ? __atomic_load_n(&pcx->current, __ATOMIC_ACQUIRE) : nullptr);
            SerialLogger::Log("[asdead] destroying root=0x"); SerialLogger::LogHex64(root_pml4);
            SerialLogger::Log(" still loaded on cpu"); SerialLogger::LogDec((int)c);
            SerialLogger::Log(c == me ? " (self)" : "");
            SerialLogger::Log(" cur=");
            if (cp) { SerialLogger::LogDec((int)cp->pid); SerialLogger::Log(" "); SerialLogger::Log(cp->name);
                      SerialLogger::Log(cp->is_kernel_proc ? " kproc" : " user"); }
            else SerialLogger::Log("none");
            SerialLogger::Log(" ra=0x"); SerialLogger::LogHex64((uint64_t)(uintptr_t)__builtin_return_address(0));
            SerialLogger::Log("\r\n");
        }
    }
    as_reg_del(root_pml4);
    if (g_as_destroy_hook) g_as_destroy_hook(root_pml4);
    if (g_as_destroy_hook2) g_as_destroy_hook2(root_pml4);
    if (g_cc_noasfree) return;

    if (g_tab) {
        uint64_t ra = (uint64_t)(uintptr_t)__builtin_return_address(0);
        if (!(tab_get(root_pml4) & TAB_FLAG))
            audit_hit("root-not-table", 0, root_pml4, 0, ra);
        const uint64_t* p4 = phys_to_virt(root_pml4);
        uint64_t top = PMM::GetPhysTop();
        for (int k = 0; k < 512; k++) {
            uint64_t e = p4[k];
            if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) continue;
            uint64_t c = entry_phys(e);
            if (c >= top) {
                audit_hit("badroot", (uint64_t)k << 39, c, tab_links(c), ra);
                break;
            }
        }
    }

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

    // a recycled root frame can be handed to a new address space while a cpu
    // still has stale TLB entries tagged with the old one. until the switch
    // path flushes unconditionally, leak the root under the direct map. (satoru)
    if (!PMM::DirectMapOn()) {
        uint32_t cpu = SMP::CpuIndex();
        if (cpu >= SMP_MAX_CPUS) cpu = 0;
        uint64_t prev = g_audit_ft[cpu];
        g_audit_ft[cpu] = root_pml4;
        PMM::FreeFrame(root_pml4);
        g_audit_ft[cpu] = prev;
    }
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
        if (pd[p2i] & PTE_USER) tab_note_link(new_pt_phys);
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
    // same hardening as query_mapping_in_root: the scanner walks address
    // spaces it only knows are live by a scheduler check, so a root or table
    // frame recycled between that check and this walk must read as "not a
    // leaf", never as a wild pointer deref. (satoru)
    uint64_t ram_top = PMM::GetPhysTop();
    if (!root_phys || (root_phys & 0xFFFULL) || root_phys >= ram_top) return nullptr;
    uint64_t* pml4 = phys_to_virt(root_phys);
    uint16_t p4i = pml4_index(virt_addr);
    if (!(pml4[p4i] & PTE_PRESENT)) return nullptr;
    uint64_t pdpt_phys = entry_phys(pml4[p4i]);
    if (pdpt_phys >= ram_top) return nullptr;
    uint64_t* pdpt = phys_to_virt(pdpt_phys);
    uint16_t p3i = pdpt_index(virt_addr);
    if (!(pdpt[p3i] & PTE_PRESENT) || (pdpt[p3i] & PTE_HUGE)) return nullptr;
    uint64_t pd_phys = entry_phys(pdpt[p3i]);
    if (pd_phys >= ram_top) return nullptr;
    uint64_t* pd = phys_to_virt(pd_phys);
    uint16_t p2i = pd_index(virt_addr);
    if (!(pd[p2i] & PTE_PRESENT) || (pd[p2i] & PTE_HUGE)) return nullptr;
    uint64_t pt_phys = entry_phys(pd[p2i]);
    if (pt_phys >= ram_top) return nullptr;
    uint64_t* pt = phys_to_virt(pt_phys);
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
    // global) plus the copy-on-write marker: a fork child's private page is
    // read-only + COW, and restoring it read-only without COW turns the next
    // write into a protection fault instead of a copy. (satoru)
    if (out_flags) *out_flags = e & (PTE_WRITABLE | PTE_USER | PTE_PWT | PTE_PCD |
                                     PTE_GLOBAL | PTE_NX | PTE_COW);
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
    if (flags & PTE_USER) ptw_event(PT_KMEMX, phys, virt_addr, root_pml4, flags);
    *pte = (phys & 0x000FFFFFFFFFF000ULL) | PTE_PRESENT | (flags & (PTE_WRITABLE |
            PTE_USER | PTE_PWT | PTE_PCD | PTE_GLOBAL | PTE_NX | PTE_COW));
    PMM::MarkFrameMapped(phys & 0x000FFFFFFFFFF000ULL, true);   // task 26 guard (satoru)
    if (root_pml4 == current_cr3()) InvalidatePage(virt_addr);
    return true;
}

bool KernelVMM::MapPage(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    phys_addr = PMM::VirtToPhys(phys_addr);
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
    // callers may pass a physical frame or a direct-map pointer for it. (satoru)
    phys_addr = PMM::VirtToPhys(phys_addr);
    // cgroup memory.max: charge a user page before it lands. a failed charge
    // fails the map, which the mmap/demand-zero callers surface as OOM - the
    // enforcement linux delivers via memcg. kernel-root maps are exempt. (satoru)
    bool charged = false;
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        user_va_shadows_managed_ram(virt_addr & ~0xFFFULL, PAGE_SIZE))
        return false;   // see user_va_shadows_managed_ram (satoru)
    // in demand mode (window released, frames reusable by the kernel) a user
    // page inside the window must own the frame at its own physical address:
    // the kernel reaches ordinary frames there through the identity map. only
    // the fixed resident ranges may be shadowed. in reserved mode the whole
    // window is still pinned, exactly like the old layout, so an arbitrary
    // frame (brk, a low mmap hint) can be mapped there safely - that is what
    // main always did and firefox's brk depends on. (satoru)
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        PMM::IsInImageWindow(virt_addr & ~0xFFFULL) &&
        !PMM::ImageWindowFullyReserved() && !PMM::DirectMapOn() &&
        (phys_addr & ~0xFFFULL) != (virt_addr & ~0xFFFULL) &&
        !PMM::ImageSafePhys(virt_addr & ~0xFFFULL)) {
        static uint32_t s_winmap = 0;
        if (s_winmap < 16) {
            s_winmap++;
            SerialLogger::Log("[winmap] refused va=0x");
            SerialLogger::LogHex64(virt_addr & ~0xFFFULL);
            SerialLogger::Log(" phys=0x");
            SerialLogger::LogHex64(phys_addr & ~0xFFFULL);
            SerialLogger::Log("\r\n");
        }
        return false;
    }
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
    return MapRangeInAddressSpace(pml4_phys, virt_base, PMM::VirtToPhys(phys_base), bytes, flags);
}

bool KernelVMM::MapRangeInAddressSpace(uint64_t root_pml4, uint64_t virt_base,
                                       uint64_t phys_base, uint64_t bytes,
                                       uint64_t flags) {
    if (!bytes) return true;
    virt_base &= ~0xFFFULL;
    phys_base = PMM::VirtToPhys(phys_base & ~0xFFFULL);
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    // overflow guard
    if (pages > (1ULL << 40)) return false;

    // cgroup memory.max: charge the whole user range up front (one ledger
    // round-trip) and refund it wholesale if the map fails part-way. (satoru)
    bool charged = false;
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        user_va_shadows_managed_ram(virt_base, pages * PAGE_SIZE))
        return false;   // see user_va_shadows_managed_ram (satoru)
    if ((flags & PTE_USER) && root_pml4 != pml4_phys &&
        PMM::IsInImageWindow(virt_base) && !PMM::ImageWindowFullyReserved() &&
        !PMM::DirectMapOn() && phys_base != virt_base) {
        // demand mode: a non-identity view inside the window is only legal when
        // every page shadows one of the fixed resident kernel ranges. (satoru)
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t va = virt_base + i * PAGE_SIZE;
            if (PMM::IsInImageWindow(va) && !PMM::ImageSafePhys(va)) return false;
        }
    }
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

// [pfloop] diagnostics: the four paging entries for va in root, read once through
// the identity map as the current cr3 sees it and once with the kernel root loaded
// (no user overlay can shadow a table frame there). a difference means the kernel's
// view of a table frame is not the physical page the cpu walks. caller holds kls,
// interrupts off. (satoru)
static void walk_log_once(uint64_t root, uint64_t va) {
    uint64_t ram_top = PMM::GetPhysTop();
    uint64_t t = root;
    static const char* nm[4] = { " l4=", " l3=", " l2=", " l1=" };
    int sh[4] = { 39, 30, 21, 12 };
    for (int lv = 0; lv < 4; lv++) {
        if (!t || t >= ram_top) { SerialLogger::Log(" (stop)"); return; }
        uint64_t e = phys_to_virt(t)[(va >> sh[lv]) & 0x1FF];
        SerialLogger::Log(nm[lv]); SerialLogger::LogHex64(e);
        if (!(e & PTE_PRESENT)) return;
        if (lv > 0 && lv < 3 && (e & PTE_HUGE)) { SerialLogger::Log(" (huge)"); return; }
        t = e & 0x000FFFFFFFFFF000ULL;
    }
}
void KernelVMM::DebugWalkDump(uint64_t root, uint64_t va) {
    uint64_t cr3 = current_cr3();
    SerialLogger::Log("[pfloop] walk cr3=0x"); SerialLogger::LogHex64(cr3);
    SerialLogger::Log(" root=0x"); SerialLogger::LogHex64(root);
    SerialLogger::Log(" cur:"); walk_log_once(root, va);
    SerialLogger::Log("\r\n[pfloop] walk kroot:");
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    walk_log_once(root, va);
    // the tree the cpu itself walks: its cr3, read under the kernel root. it
    // differs from the lines above only when cr3 is not the task's root. (satoru)
    SerialLogger::Log("\r\n[pfloop] walk hw(cr3) kroot:");
    walk_log_once(cr3, va);
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    SerialLogger::Log("\r\n");
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

// [asdead] the root each cpu last loaded (ActivateAddressSpace and the kernel
// root loads), so a teardown can tell whether some cpu still runs on the
// tables it is about to free. diagnostic only. (satoru)
void KernelVMM::NoteCr3(uint64_t root) {
    uint32_t c = SMP::CpuIndex();
    if (c < SMP_MAX_CPUS) g_cpu_cr3[c] = root & ~0xFFFULL;
}
void KernelVMM::ActivateAddressSpace(uint64_t root_pml4) {
    if (!root_pml4) return;
    asm volatile("mov %0, %%cr3" : : "r"(root_pml4) : "memory");
    NoteCr3(root_pml4);
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
