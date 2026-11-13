#include "pmm.h"
#include "kmap.h"   // KERNEL_DIRECT_BASE for kurono.directmap (satoru)
#include "../drivers/serial.h"
#include "../proc/smp.h"   // cpu index for the cross-core pmm lock (satoru)

//  physical memory manager - bitmap frame allocator implementation

// static member definitions
uint64_t* PMM::bitmap       = nullptr;
uint16_t* PMM::refs         = nullptr;
uint8_t*  PMM::mapped_bits  = nullptr;   // task 26: 1 bit/frame - a USER leaf pte points here (satoru)
uint64_t  PMM::bitmap_size  = 0;
// installed ram per the firmware map, before the 16 gb frame cap below; the task
// manager shows it next to the usable total so a 32 gb machine is not mis-stated. (satoru)
static uint64_t g_installed_ram_bytes = 0;
uint64_t pmm_installed_ram_bytes() { return g_installed_ram_bytes; }
uint64_t  PMM::total_frames = 0;
uint64_t  PMM::usable_frames = 0;
uint64_t  PMM::used_frames  = 0;
uint64_t  PMM::search_hint  = 0;
// kurono.pmmhigh=1 test knob (set in Init): keep the next-fit hint at or above
// the old 16 gb cap so the newly managed range is used first. (satoru)
static bool     g_pmm_high_first = false;
static uint64_t g_pmm_high_floor = 0;   // frame index of the floor while the knob is on (satoru)
// frames at or above the old 16 gb cap currently handed out, and the peak since
// the last ResetHighPeak. counted only when ram reaches past 16 gb (Init sets the
// boundary; it stays at ~0 otherwise). test reporting only, under the pmm lock. (satoru)
static uint64_t g_hi_first = ~0ULL;
static uint64_t g_hi_used  = 0;
static uint64_t g_hi_peak  = 0;
static inline uint64_t hi_count(uint64_t start, uint64_t count) {
    uint64_t end = start + count;
    if (end <= g_hi_first) return 0;
    return end - (start > g_hi_first ? start : g_hi_first);
}
static inline void hi_add(uint64_t n) { if (n) { g_hi_used += n; if (g_hi_used > g_hi_peak) g_hi_peak = g_hi_used; } }
static inline void hi_sub(uint64_t n) { g_hi_used = g_hi_used > n ? g_hi_used - n : 0; }

// kurono.memcensus=1: one 32-bit allocation-site tag per frame (the caller's
// return address; the kernel links below 4 gb). null when off, so the only
// cost on a normal boot is one branch per allocation. a freed frame keeps its
// stale tag; the census only counts frames whose used bit is set. (satoru)
static uint32_t* g_site = nullptr;
static bool      g_census_req = false;

// kurono.directmap=1: AllocBytes returns KERNEL_DIRECT_BASE + phys once the
// high-half map is installed by KernelVMM::InitDirectMap. AllocFrame and
// AllocContiguous keep returning physical frame numbers. (satoru)
static bool g_direct_req = false;
static bool g_direct_on  = false;
// kurono.ptaudit=1: paging-structure lifetime audit, see kernel/vmm.cpp. (satoru)
static bool g_ptaudit = false;
static bool g_pttrace = false;
bool PMM::PtAudit() { return g_ptaudit; }
bool PMM::PtTrace() { return g_pttrace; }
static bool (*g_free_guard)(uint64_t phys, uint64_t ra) = nullptr;
void PMM::SetFreeGuard(bool (*fn)(uint64_t phys, uint64_t ra)) { g_free_guard = fn; }
static bool (*g_bulk_guard)(uint64_t phys) = nullptr;
void PMM::SetBulkFreeGuard(bool (*fn)(uint64_t phys)) { g_bulk_guard = fn; }
bool     PMM::DirectMapRequested() { return g_direct_req; }
bool     PMM::DirectMapOn()        { return g_direct_on; }
void     PMM::EnableDirectMap()    { if (g_direct_req) g_direct_on = true; }
void*    PMM::PhysToVirt(uint64_t phys) {
    return (void*)(uintptr_t)(g_direct_on ? KERNEL_DIRECT_BASE + phys : phys);
}
uint64_t PMM::VirtToPhys(uint64_t addr) {
    if (g_direct_on && KernelDirectAddr(addr)) return addr - KERNEL_DIRECT_BASE;
    return addr;
}
// the frame-ownership api takes a physical frame address, but kernel blocks
// are handed out as direct-map pointers under kurono.directmap=1 and callers
// routinely feed such a pointer (cast to u64) into RetainFrame/FreeFrame. an
// address past ram top used to be ignored by RetainFrame, so a user mapping of
// a kernel block never held its own reference and the unmap/quarantine path
// dropped the block owner's. normalize once at every entry point. (satoru)
static inline uint64_t frame_addr(uint64_t a) {
    if (g_direct_on && KernelDirectAddr(a)) return a - KERNEL_DIRECT_BASE;
    return a;
}
static inline void site_tag(uint64_t start, uint64_t count, void* ra) {
    if (!g_site) return;
    uint32_t v = (uint32_t)(uintptr_t)ra;
    for (uint64_t j = 0; j < count; j++) g_site[start + j] = v;
}

extern "C" uint8_t kernel_start;       // link (virtual) range of the kernel (satoru)
extern "C" uint8_t kernel_end;
extern "C" uint8_t kernel_phys_start;  // where the loader put it (satoru)
extern "C" uint8_t kernel_phys_end;
extern "C" uint8_t kernel_boot_start;  // low stub + boot page tables + boot stack (satoru)
extern "C" uint8_t kernel_boot_end;
extern "C" uint8_t user_image_ceiling; // top of the fixed low user image window (satoru)

static uint64_t g_image_lo = 0, g_image_hi = 0;
static uint64_t g_image_full_hi = 0;       // virtual window top, fixed for the session (satoru)
static bool     g_image_soft    = false;   // window released: allocators prefer other frames (satoru)
static bool     g_window_keep   = false;   // kurono.window.keep=1: diagnose by keeping the boot reservation (satoru)
static int      g_image_force   = 0;       // kurono.imgwin: 0 auto, 1 reserve, 2 demand (satoru)
static uint64_t g_dump_lo = 0, g_dump_hi = 0;  // reserved panic minidump page (satoru)
static uint64_t g_meta_lo = 0, g_meta_hi = 0;  // frame metadata (also inside the window on low ram) (satoru)
static uint64_t g_fb_reserved_bytes = 0;   // mbi framebuffer span pinned in Init (accounting) (satoru)

// the kernel ranges below are resident for the whole session and are read
// through their identity addresses, so a user mapping over one of them would
// redirect those reads. everything else inside the low window is fair game for
// a fixed image once the window has been released. (satoru)
static bool image_safe_phys(uint64_t pa) {
    uint64_t bs = (uint64_t)&kernel_boot_start, be = (uint64_t)&kernel_boot_end;
    uint64_t ks = (uint64_t)&kernel_phys_start, ke = (uint64_t)&kernel_phys_end;
    if (pa >= bs && pa < be) return true;
    if (pa >= ks && pa < ke) return true;
    if (g_dump_hi && pa >= g_dump_lo && pa < g_dump_hi) return true;
    return false;
}

// frames the boot window release must keep: the fixed kernel ranges above plus
// the pmm's own metadata, which can land inside the window on a machine too
// small to ever run a 300 mb image anyway. (satoru)
static bool image_release_keep(uint64_t pa) {
    if (image_safe_phys(pa)) return true;
    if (g_meta_hi && pa >= g_meta_lo && pa < g_meta_hi) return true;
    return false;
}

static inline bool frame_window_protected(uint64_t idx) {
    if (!g_image_soft || !g_image_full_hi) return false;
    uint64_t a = idx * PAGE_SIZE;
    return a >= g_image_lo && a < g_image_full_hi;
}


// interrupt guard for the frame allocator. these mutators are re-entered
// from the #pf stack-grow handler (Scheduler::TryGrowGuardPage -> AllocBytes),
// so a fault landing mid-allocation would corrupt bitmap/refs/used_frames.
// cli/sti makes each mutation atomic vs the fault path, exactly like the
// kernel heap's HeapIrqGuard. save/restore so it nests harmlessly. (satoru)
// since smp thread dispatch, cli alone is not enough: an ap's syscall (mmap,
// demand-zero fault) mutates bitmap/refs concurrently with the bsp - the guard
// is now cli + a cross-core lock, cpu-owner-recursive because the #pf
// stack-grow re-entry above still happens on the SAME core and cli can't stop
// exceptions (a plain spinlock would self-deadlock there). (satoru)
namespace {
volatile uint32_t g_pmm_lock_word  = 0;
volatile int      g_pmm_lock_owner = -1;
struct PmmIrqGuard {
    uint64_t f;
    bool nested;
    PmmIrqGuard() {
        __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        int cpu = (int)SMP::CpuIndex();
        if (g_pmm_lock_owner == cpu) { nested = true; return; }
        nested = false;
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&g_pmm_lock_word, &expected, 1u, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            do { __asm__ __volatile__("pause" ::: "memory"); }
            while (__atomic_load_n(&g_pmm_lock_word, __ATOMIC_RELAXED) != 0);
        }
        g_pmm_lock_owner = cpu;
    }
    ~PmmIrqGuard() {
        if (!nested) {
            g_pmm_lock_owner = -1;
            __atomic_store_n(&g_pmm_lock_word, 0u, __ATOMIC_RELEASE);
        }
        if (f & 0x200ULL) __asm__ __volatile__("sti" ::: "memory");
    }
};
}

void PMM::SetFrame(uint64_t idx) {
    bitmap[idx / 64] |= (1ULL << (idx % 64));
}

void PMM::ClearFrame(uint64_t idx) {
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

bool PMM::TestFrame(uint64_t idx) {
    return (bitmap[idx / 64] & (1ULL << (idx % 64))) != 0;
}

bool PMM::IsFrameUsed(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return true;
    return TestFrame(idx);
}

// count trailing zeros (find first set bit). returns 64 for v==0.
static inline int ctz64(uint64_t v) {
    if (v == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int n = 0;
    while (!(v & 1ULL)) { v >>= 1; n++; }
    return n;
#endif
}

// qword range that covers the released image window (frame / 64). qwords fully
// inside it can be skipped in one step instead of walking 64 protected bits
// with interrupts off. (satoru)
static inline uint64_t window_qword_lo() { return g_image_lo / (PAGE_SIZE * 64); }
static inline uint64_t window_qword_hi() {
    return (g_image_full_hi + PAGE_SIZE * 64 - 1) / (PAGE_SIZE * 64);
}

uint64_t PMM::FindFreeFrame() {
    uint64_t qwords = bitmap_size;
    if (qwords == 0) return ~0ULL;

    uint64_t start_q = search_hint / 64;
    if (start_q >= qwords) start_q = 0;
    uint64_t win_qlo = 0, win_qhi = 0;
    if (g_image_soft && g_image_full_hi) {
        win_qlo = window_qword_lo();
        win_qhi = window_qword_hi();
        // do not start the search inside the reserved-for-images window: scan
        // the range after it first and wrap; the window is still visited last
        // as the fallback. (satoru)
        if (start_q >= win_qlo && start_q < win_qhi) start_q = win_qhi % qwords;
    }

    // after the image window is released, ordinary kernel allocations prefer
    // frames outside it so the low range stays available for a fixed-address
    // image; if nothing outside is free the window is used as a fallback. the
    // search order itself (next-fit from search_hint, wrapping once) is the
    // same as before. (satoru)
    uint64_t fallback = ~0ULL;
    for (uint64_t pass = 0; pass < qwords; pass++) {
        uint64_t qi = (start_q + pass) % qwords;
        uint64_t val = bitmap[qi];
        if (val == ~0ULL) continue;

        if (g_image_soft && qi >= win_qlo && qi < win_qhi) {
            // whole qword inside the window: remember its first free frame and
            // move on without walking the other bits. (satoru)
            if (fallback == ~0ULL) {
                uint64_t f = qi * 64 + (uint64_t)ctz64(~val);
                if (f < total_frames) fallback = f;
            }
            continue;
        }

        uint64_t inv = ~val;
        while (inv) {
            int bit = ctz64(inv);
            uint64_t frame = qi * 64 + (uint64_t)bit;
            if (frame >= total_frames) break;
            if (!frame_window_protected(frame)) {
                search_hint = frame + 1;
                return frame;
            }
            if (fallback == ~0ULL) fallback = frame;
            inv &= inv - 1;
        }
    }
    if (fallback != ~0ULL) { search_hint = fallback + 1; return fallback; }
    return ~0ULL;
}

// task 26 free-while-mapped guard: a frame with a live USER leaf pte must never
// be re-handed by the allocator - that is the proven crossbeam stack corruptor
// (a live parked stack's frame freed-while-mapped, recycled as another thread's
// stack; both push through their own ptes into one page). set/cleared by the
// vmm leaf writers; checked at alloc. (satoru)
void PMM::MarkFrameMapped(uint64_t phys_addr, bool m) {
    phys_addr = frame_addr(phys_addr);
    if (!mapped_bits) return;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (m) __atomic_fetch_or (&mapped_bits[idx >> 3], (uint8_t)(1u << (idx & 7)), __ATOMIC_RELAXED);
    else   __atomic_fetch_and(&mapped_bits[idx >> 3], (uint8_t)~(1u << (idx & 7)), __ATOMIC_RELAXED);
}
bool PMM::IsFrameMapped(uint64_t phys_addr) {
    phys_addr = frame_addr(phys_addr);
    if (!mapped_bits) return false;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return false;
    return (mapped_bits[idx >> 3] >> (idx & 7)) & 1;
}

uint64_t PMM::AllocFrame() {
    PmmIrqGuard _g;
    // task 26: refuse to hand out a frame that still has a live USER leaf pte
    // pointing at it (a free-while-mapped escaped into the free pool). log the
    // event - it names the guilty free path - leak the flagged frame, and pick
    // another. bounded retries; the leak is 4k per real event. (satoru)
    uint64_t idx;
    for (int tries = 0; ; tries++) {
        idx = FindFreeFrame();
        if (idx == ~0ULL) {
            SerialLogger::Log("PMM: OUT OF MEMORY!\r\n");
            return 0;
        }
        if (!mapped_bits || tries >= 8 ||
            !((mapped_bits[idx >> 3] >> (idx & 7)) & 1)) break;
        static uint32_t s_dbl_logs = 0;
        if (s_dbl_logs < 8) {
            s_dbl_logs++;
            SerialLogger::Log("[DBLMAP] phys=");
            SerialLogger::LogHex64(idx * PAGE_SIZE);
            SerialLogger::Log(" refct=");
            SerialLogger::LogDec((int)refs[idx]);
            SerialLogger::Log(" free-while-mapped frame quarantined\r\n");
        }
        SetFrame(idx);   // leak it: mark used, never hand out (satoru)
        used_frames++;
    }
    SetFrame(idx);
    refs[idx] = 1;
    used_frames++;
    hi_add(hi_count(idx, 1));
    site_tag(idx, 1, __builtin_return_address(0));
    uint64_t phys = idx * PAGE_SIZE;
    // zero-on-alloc: scrub the frame so we never hand out stale data.
    uint64_t* p = (uint64_t*)PhysToVirt(phys);
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i++) p[i] = 0;
    return phys;
}

void PMM::RetainFrame(uint64_t phys_addr) {
    PmmIrqGuard _g;
    phys_addr = frame_addr(phys_addr);
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;
    if (refs[idx] == 0xFFFF) return;  // saturate to prevent wraparound
    refs[idx]++;
}

uint32_t PMM::GetFrameRefCount(uint64_t phys_addr) {
    phys_addr = frame_addr(phys_addr);
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return 0;
    return refs[idx];
}

bool PMM::AllocFrameAt(uint64_t phys_addr) {
    if (phys_addr & (PAGE_SIZE - 1)) return false;
    PmmIrqGuard _g;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return false;
    if (TestFrame(idx)) {
        // in reserved mode the frame at the image va is already ours: it is a
        // window reservation, not a real owner. hand it to the image page (the
        // identity alias then IS the page) instead of failing the load. a frame
        // with a live user pte belongs to someone else and is refused. (satoru)
        bool window_owned = g_image_full_hi && !g_image_soft &&
                            phys_addr >= g_image_lo && phys_addr < g_image_full_hi &&
                            !image_release_keep(phys_addr) &&
                            !(mapped_bits && ((mapped_bits[idx >> 3] >> (idx & 7)) & 1));
        if (!window_owned) return false;
        refs[idx] = 1;
        uint64_t* p = (uint64_t*)PhysToVirt(phys_addr);
        for (int i = 0; i < (int)(PAGE_SIZE / 8); i++) p[i] = 0;
        return true;
    }
    // a frame with a live user leaf pte is not free even if the bitmap says so
    // (see AllocFrame's task 26 guard). (satoru)
    if (mapped_bits && ((mapped_bits[idx >> 3] >> (idx & 7)) & 1)) return false;
    SetFrame(idx);
    refs[idx] = 1;
    used_frames++;
    hi_add(hi_count(idx, 1));
    site_tag(idx, 1, __builtin_return_address(0));
    uint64_t* p = (uint64_t*)PhysToVirt(phys_addr);
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i++) p[i] = 0;
    return true;
}

// [ovfree] the frames behind the fixed user overlay windows (main stack,
// ld-kurono trampoline; same table as g_overlay below) are reserved in Init and
// never handed out, so nothing may ever free one: a freed one re-enters the
// pool and can become a page table that every user address space shadows with
// its main stack (the kernel then walks the stack page instead of the table).
// logged with the caller; the free itself is unchanged. (satoru)
static inline bool ov_reserved_frame(uint64_t phys) {
    return (phys >= 0x3FA00000ULL && phys < 0x40200000ULL) ||
           (phys >= 0x600000000ULL && phys < 0x600001000ULL);
}
static void ov_free_note(uint64_t phys, uint16_t r, uint64_t ra, int path) {
    static volatile uint32_t s_ov = 0;
    if (__atomic_fetch_add(&s_ov, 1, __ATOMIC_RELAXED) >= 16) return;
    SerialLogger::Log("[ovfree] overlay frame freed phys=0x"); SerialLogger::LogHex64(phys);
    SerialLogger::Log(" refs="); SerialLogger::LogDec((int)r);
    SerialLogger::Log(path == 2 ? " bulk-imgwin" : path ? " bulk" : " single");
    SerialLogger::Log(" ra=0x"); SerialLogger::LogHex64(ra);
    SerialLogger::Log("\r\n");
}

void PMM::FreeFrame(uint64_t phys_addr) {
    PmmIrqGuard _g;
    phys_addr = frame_addr(phys_addr);
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;  // double-free guard

    if (refs[idx] > 1) {
        refs[idx]--;
        return;
    }

    // reserved mode: a fixed-image page comes out of the window reservation, so
    // dropping it returns it to the reservation, not to the general pool. this
    // keeps the next claude launch working after firefox grew across the range.
    // (satoru)
    if (g_image_full_hi && !g_image_soft && phys_addr >= g_image_lo &&
        phys_addr < g_image_full_hi && !image_release_keep(phys_addr)) {
        refs[idx] = 1;
        return;
    }

    if (g_free_guard && g_free_guard(phys_addr,
                                     (uint64_t)(uintptr_t)__builtin_return_address(0)))
        return;   // audit refused: still a live paging structure. (satoru)
    if (ov_reserved_frame(phys_addr))
        ov_free_note(phys_addr, refs[idx], (uint64_t)(uintptr_t)__builtin_return_address(0), 0);   // [ovfree] (satoru)
    refs[idx] = 0;
    ClearFrame(idx);
    used_frames--;
    hi_sub(hi_count(idx, 1));

    if (idx < search_hint && idx >= g_pmm_high_floor) search_hint = idx;
}

static void bulk_set_frames(uint64_t start, uint64_t count,
                            uint64_t* bmp, uint64_t& used, uint64_t total) {
    uint64_t end = start + count;
    if (end > total) end = total;
    uint64_t i = start;

    while (i < end && (i % 64) != 0) {
        bmp[i / 64] |= (1ULL << (i % 64));
        used++;
        i++;
    }
    while (i + 64 <= end) {
        bmp[i / 64] = ~0ULL;
        used += 64;
        i += 64;
    }
    while (i < end) {
        bmp[i / 64] |= (1ULL << (i % 64));
        used++;
        i++;
    }
}

static void bulk_clear_frames(uint64_t start, uint64_t count,
                              uint64_t* bmp, uint64_t& used, uint64_t total) {
    uint64_t end = start + count;
    if (end > total) end = total;
    uint64_t i = start;

    while (i < end && (i % 64) != 0) {
        uint64_t mask = (1ULL << (i % 64));
        if (bmp[i / 64] & mask) {
            bmp[i / 64] &= ~mask;
            used--;
        }
        i++;
    }
    while (i + 64 <= end) {
        // count bits set, then clear whole qword
        uint64_t v = bmp[i / 64];
#if defined(__GNUC__) || defined(__clang__)
        used -= (uint64_t)__builtin_popcountll(v);
#else
        for (int b = 0; b < 64; b++) if (v & (1ULL << b)) used--;
#endif
        bmp[i / 64] = 0;
        i += 64;
    }
    while (i < end) {
        uint64_t mask = (1ULL << (i % 64));
        if (bmp[i / 64] & mask) {
            bmp[i / 64] &= ~mask;
            used--;
        }
        i++;
    }
}

// one first-fit run scan. protect=true treats the released image window as
// busy so the caller's block lands outside it; the algorithm and the search
// order are unchanged, the window is just a reservation the scan honours.
// (satoru)
static uint64_t alloc_contig_scan(uint64_t count, bool protect) {
    PmmIrqGuard _g;   // atomic vs the #pf stack-grow path (the run-scan below mutates the bitmap directly) (satoru)
    uint64_t qwords = PMM::GetBitmapQwords();
    uint64_t win_qlo = 0, win_qhi = 0;
    if (protect) { win_qlo = window_qword_lo(); win_qhi = window_qword_hi(); }

    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (uint64_t qi = 0; qi < qwords; qi++) {
        uint64_t val = PMM::GetBitmapQword(qi);
        if (val == ~0ULL) {
            run_start = (qi + 1) * 64;
            run_len = 0;
            continue;
        }
        if (protect && qi >= win_qlo && qi < win_qhi) {
            // the whole qword is image-window reservation: skip it without
            // walking its 64 protected frames. (satoru)
            run_start = (qi + 1) * 64;
            run_len = 0;
            continue;
        }
        if (val == 0ULL && !protect) {
            uint64_t frame_base = qi * 64;
            if (run_len == 0) run_start = frame_base;
            // cap the run to total_frames
            uint64_t add = 64;
            if (frame_base + add > PMM::GetTotalFrameCount()) add = PMM::GetTotalFrameCount() - frame_base;
            run_len += add;
            if (run_len >= count) {
                PMM::CommitContiguous(run_start, count);
                uint64_t phys = run_start * PAGE_SIZE;
                uint64_t* zp = (uint64_t*)PMM::PhysToVirt(phys);
                uint64_t qcount = (count * PAGE_SIZE) / 8;
                for (uint64_t z = 0; z < qcount; z++) zp[z] = 0;
                return phys;
            }
            continue;
        }
        for (int bit = 0; bit < 64; bit++) {
            uint64_t fi = qi * 64 + (uint64_t)bit;
            if (fi >= PMM::GetTotalFrameCount()) break;
            bool busy = (val & (1ULL << bit)) != 0;
            if (!busy && protect && frame_window_protected(fi)) busy = true;
            if (busy) {
                run_start = fi + 1;
                run_len = 0;
            } else {
                if (run_len == 0) run_start = fi;
                run_len++;
                if (run_len >= count) {
                    PMM::CommitContiguous(run_start, count);
                    uint64_t phys = run_start * PAGE_SIZE;
                    uint64_t* zp = (uint64_t*)PMM::PhysToVirt(phys);
                    uint64_t qcount = (count * PAGE_SIZE) / 8;
                    for (uint64_t z = 0; z < qcount; z++) zp[z] = 0;
                    return phys;
                }
            }
        }
    }
    return 0;
}

uint64_t PMM::AllocContiguous(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) {
        uint64_t f = AllocFrame();
        if (f) site_tag(f / PAGE_SIZE, 1, __builtin_return_address(0));
        return f;
    }
    if (count > total_frames) return 0;
    bool protect = g_image_soft && g_image_full_hi != 0;
    uint64_t phys = alloc_contig_scan(count, protect);
    if (!phys && protect) phys = alloc_contig_scan(count, false);
    if (!phys) {
        SerialLogger::Log("PMM: AllocContiguous failed (");
        SerialLogger::LogDec((uint32_t)count);
        SerialLogger::Log(" frames requested)\r\n");
    }
    return phys;
}

void PMM::CommitContiguous(uint64_t run_start, uint64_t count) {
    bulk_set_frames(run_start, count, bitmap, used_frames, total_frames);
    for (uint64_t j = 0; j < count; j++) refs[run_start + j] = 1;
    if (run_start + count >= g_pmm_high_floor) search_hint = run_start + count;
    hi_add(hi_count(run_start, count));
    site_tag(run_start, count, __builtin_return_address(0));
}

uint64_t PMM::GetBitmapQwords() { return bitmap_size; }
uint64_t PMM::GetBitmapQword(uint64_t i) { return bitmap[i]; }
uint64_t PMM::GetTotalFrameCount() { return total_frames; }

void PMM::FreeContiguous(uint64_t phys_addr, uint64_t count) {
    if (!phys_addr || count == 0) return;
    PmmIrqGuard _g;
    phys_addr = frame_addr(phys_addr);
    uint64_t start = phys_addr / PAGE_SIZE;
    if (start >= total_frames) return;
    if (count > total_frames - start) count = total_frames - start;

    // fast path: if all frames have refcount == 1, do bulk clear
    bool fast = true;
    for (uint64_t j = 0; j < count; j++) {
        if (g_bulk_guard && g_bulk_guard((start + j) * PAGE_SIZE)) { fast = false; break; }
        if (refs[start + j] != 1) { fast = false; break; }
        if (!TestFrame(start + j))  { fast = false; break; }
    }
    if (fast) {
        if (ov_reserved_frame(start * PAGE_SIZE) || ov_reserved_frame((start + count - 1) * PAGE_SIZE) ||
            (start * PAGE_SIZE < 0x3FA00000ULL && (start + count) * PAGE_SIZE > 0x40200000ULL))
            ov_free_note(start * PAGE_SIZE, refs[start], (uint64_t)(uintptr_t)__builtin_return_address(0), 1);   // [ovfree] (satoru)
        if (g_image_full_hi && !g_image_soft && start * PAGE_SIZE < g_image_full_hi &&
            (start + count) * PAGE_SIZE > g_image_lo)
            ov_free_note(start * PAGE_SIZE, refs[start], (uint64_t)(uintptr_t)__builtin_return_address(0), 2);   // [ovfree] reserved image window, bulk (satoru)
        for (uint64_t j = 0; j < count; j++) refs[start + j] = 0;
        bulk_clear_frames(start, count, bitmap, used_frames, total_frames);
        hi_sub(hi_count(start, count));
        if (start < search_hint && start >= g_pmm_high_floor) search_hint = start;
        return;
    }
    for (uint64_t j = 0; j < count; j++) {
        FreeFrame((start + j) * PAGE_SIZE);
    }
}

void* PMM::AllocBytes(size_t bytes) {
    if (bytes == 0) return nullptr;
    // overflow guard on round-up
    if (bytes > (size_t)-1 - (PAGE_SIZE - 1)) return nullptr;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = AllocContiguous(pages);
    if (!phys) return nullptr;
    site_tag(phys / PAGE_SIZE, pages, __builtin_return_address(0));
    // AllocContiguous already zeroed the region
    return PhysToVirt(phys);
}

void PMM::FreeBytes(void* addr, size_t bytes) {
    if (!addr || bytes == 0) return;
    uint64_t phys = VirtToPhys(addr);
    if (bytes > (size_t)-1 - (PAGE_SIZE - 1)) return;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    FreeContiguous(phys, pages);
}

// ── boot-time helpers for Init ──────────────────────────────────────────────
// a private copy of the firmware memory map, taken before the frame metadata is
// written. the metadata can now be large (refs are 2 bytes a frame: 64 mb at
// 128 gb), and on an mb1 boot the map lives in loader memory that the metadata
// write could reach. (satoru)
namespace {
struct PmmRange { uint64_t base, end; uint32_t type; };
constexpr int PMM_MAX_RANGES = 512;
PmmRange g_ranges[PMM_MAX_RANGES];
int      g_range_count = 0;
bool     g_ranges_truncated = false;   // more entries than the copy holds (satoru)

// fixed user virtual windows that overlay the kernel identity map in every user
// address space. the pmm never hands out the frames behind them, because a
// kernel write to such a frame through the identity map lands in the user page
// instead. (satoru)
struct OverlayWin { uint64_t lo, hi; };
const OverlayWin g_overlay[] = {
    // the main stack of every user process, 8 mb below 0x40200000 (see
    // main-stack note in Init). (satoru)
    { 0x3FA00000ULL, 0x40200000ULL },
    // ld-kurono's init trampoline page, mapped at 24 gb in every dynamic
    // process (linux/ld_kurono.cpp TRAMP_VA). harmless below the old 16 gb cap,
    // a live alias once ram above 24 gb is managed. (satoru)
    { 0x600000000ULL, 0x600001000ULL },
};
constexpr int OVERLAY_COUNT = (int)(sizeof(g_overlay) / sizeof(g_overlay[0]));

bool g_pmm_selftest = false;   // kurono.pmmtest=1 (satoru)

inline bool span_overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

// whole-token cmdline scan, same rules as kurono_kernel's boot_has_token. returns
// a pointer just past "key" when the token starts with it, else nullptr. (satoru)
const char* pmm_find_token(const char* cl, const char* key) {
    if (!cl || !key) return nullptr;
    int kl = 0; while (key[kl]) kl++;
    for (const char* p = cl; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        bool match = true;
        for (int i = 0; i < kl; i++) if (p[i] != key[i]) { match = false; break; }
        if (match) return p + kl;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return nullptr;
}

// true when every page of [lo, hi) is usable ram per the firmware map and no
// reserved entry overlaps it. (satoru)
bool pmm_span_is_ram(uint64_t lo, uint64_t hi) {
    if (hi <= lo || g_ranges_truncated) return false;
    for (int i = 0; i < g_range_count; i++) {
        if (g_ranges[i].type != 1 && span_overlaps(lo, hi, g_ranges[i].base, g_ranges[i].end))
            return false;
    }
    uint64_t cur = lo;
    for (int guard = 0; cur < hi && guard < PMM_MAX_RANGES + 1; guard++) {
        bool advanced = false;
        for (int i = 0; i < g_range_count; i++) {
            const PmmRange& r = g_ranges[i];
            if (r.type != 1) continue;
            uint64_t rb = (r.base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t re = r.end & ~(PAGE_SIZE - 1);
            if (rb <= cur && cur < re) { cur = re; advanced = true; break; }
        }
        if (!advanced) return false;
    }
    return cur >= hi;
}
}  // namespace

// item 8: the 1 mb .. kernel_start window a high-linked kernel leaves for a
// fixed-address user image (claude at 0x200000, static musl at 0x400000). its
// frames are reserved in Init, so a user page there shadows nothing the kernel
// owns. empty with the classic low link (the kernel itself sits there). (satoru)
bool PMM::ImageSafePhys(uint64_t pa) { return image_safe_phys(pa); }
bool PMM::IsInImageWindow(uint64_t va) {
    return g_image_full_hi && va >= g_image_lo && va < g_image_full_hi;
}
uint64_t PMM::ImageWindowHi() { return g_image_full_hi; }


uint64_t PMM::GetReservedFramebufferBytes() { return g_fb_reserved_bytes; }
// physical frames the low window currently costs: boot keeps the whole window
// reserved, once released only the pages backing a running fixed image (and
// the fixed kernel ranges inside it) are used. (satoru)
uint64_t PMM::GetImageWindowBytes() {
    if (!g_image_full_hi) return 0;
    uint64_t n = 0;
    PmmIrqGuard _g;
    for (uint64_t addr = g_image_lo; addr < g_image_full_hi; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx >= total_frames) break;
        if (TestFrame(idx) && !image_release_keep(addr)) n++;
    }
    return n * PAGE_SIZE;
}
uint64_t PMM::GetOverlayBytes() {
    uint64_t n = 0;
    for (int i = 0; i < OVERLAY_COUNT; i++) n += g_overlay[i].hi - g_overlay[i].lo;
    return n;
}

bool PMM::ImageWindowFullyReserved() {
    return g_image_hi != 0 && g_image_hi >= g_image_full_hi;
}

// hand the whole fixed-image window back to the allocator once boot is done.
// every frame in it is either the window's own reservation or one of the fixed
// kernel ranges above, so nothing a live kernel object owns can be freed here.
// after this the window costs no physical memory: pages become image backing
// one at a time, when a fixed-address program is actually mapped. (satoru)
uint64_t PMM::MinidumpSlot() {
    if (g_dump_lo) return g_dump_lo;
    // before Init ran: same cut-off Init uses for the slot.
    return (total_frames * PAGE_SIZE) < (2ULL << 30) ? 0xF00000ULL : 0x14000000ULL;
}

bool PMM::ReleaseImageWindow() {
    if (g_window_keep) return false;
    if (!g_image_full_hi || g_image_soft) return true;
    PmmIrqGuard _g;
    for (uint64_t addr = g_image_lo; addr < g_image_full_hi; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx >= total_frames) break;
        if (!TestFrame(idx)) continue;
        if (image_release_keep(addr)) continue;   // kernel image / boot stub / dump / metadata (satoru)
        ClearFrame(idx);
        if (used_frames) used_frames--;
        if (idx < search_hint) search_hint = idx;
    }
    g_image_hi = g_image_lo;
    g_image_soft = true;
    return true;
}

// kept for the callers that only know the old keep_bytes form. the window is
// no longer a fixed reservation, so the entire window is released. (satoru)
bool PMM::ReleaseImageWindowAbove(uint64_t keep_bytes) {
    (void)keep_bytes;
    return ReleaseImageWindow();
}

// end-of-boot policy. a fixed-address image (claude) must always start on a big
// machine, and by then firefox's heap may have grown across the low window. so
// above ~1.75 gb of installed ram, when a CLAUDE image is present, keep the
// whole window reserved exactly like the old layout (300 mb of 32 gb is
// nothing) and let image pages take frames out of that reservation. below that,
// or with no image installed, release it: pages are then claimed from the free
// pool one at a time and a frame someone else owns is refused cleanly.
// kurono.imgwin=reserve|demand forces either policy for testing. (satoru)
// claude and opencode load at a fixed address and this window is the only reason they dont stomp the kernel. if claude stops launching after u changed this, put it back
bool PMM::FinalizeImageWindow(bool claude_present) {
    (void)claude_present;
    if (g_window_keep) return true;
    // default is RESERVED at every ram size. the window is pinned during boot
    // anyway, and keeping it pinned means low images (static ET_EXECs, claude)
    // can fork/COW and run several copies at once exactly like main. demand
    // mode is only entered when explicitly requested (kurono.imgwin=demand)
    // until the high-half direct map makes arbitrary-frame backing safe.
    // below ~1.75 gb init only offers the first 64 mb, so the pinned cost is
    // small there. (satoru)
    if (g_image_force != 2) {
        g_image_soft = false;
        SerialLogger::Log("[pmm] user image window: reserved\r\n");
        return true;
    }
    ReleaseImageWindow();
    SerialLogger::Log("[pmm] user image window: demand-backed (forced)\r\n");
    return false;
}

// a fixed-address image maps its pages one at a time now (see AllocFrameAt), so
// there is no whole-window reservation to take back. report success as long as
// the window was configured at all; a page whose identity frame is taken will
// fail its own AllocFrameAt with a clear oom instead of corrupting anything. (satoru)
bool PMM::TryReserveImageWindow() {
    return true;
}

bool PMM::IsUserOverlayRange(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return true;
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        if (lo >= g_overlay[i].lo && hi <= g_overlay[i].hi) return true;
    }
    // the whole virtual window stays legal for a fixed image even after the
    // physical reservation was released. (satoru)
    if (g_image_full_hi && lo >= g_image_lo && hi <= g_image_full_hi) return true;
    return false;
}

uint64_t PMM::CountUsedFramesFrom(uint64_t phys) {
    PmmIrqGuard _g;
    uint64_t idx = phys / PAGE_SIZE;
    uint64_t n = 0;
    while (idx < total_frames && (idx % 64) != 0) { if (TestFrame(idx)) n++; idx++; }
    while (idx + 64 <= total_frames) {
        n += (uint64_t)__builtin_popcountll(bitmap[idx / 64]);
        idx += 64;
    }
    while (idx < total_frames) { if (TestFrame(idx)) n++; idx++; }
    return n;
}

uint64_t PMM::HighFramesInUse() { PmmIrqGuard _g; return g_hi_used; }
uint64_t PMM::HighFramesPeak()  { PmmIrqGuard _g; return g_hi_peak; }
void     PMM::ResetHighPeak()   { PmmIrqGuard _g; g_hi_peak = g_hi_used; }

bool PMM::CensusOn() { return g_site != nullptr; }
uint32_t PMM::CensusSite(uint64_t idx) {
    return (g_site && idx < total_frames) ? g_site[idx] : 0;
}
bool PMM::CensusFrameUsed(uint64_t idx) {
    return idx < total_frames && TestFrame(idx);
}

static uint64_t alloc_contig_below_scan(uint64_t count, uint64_t limit, bool protect) {
    PmmIrqGuard _g;
    uint64_t run_start = 0, run_len = 0;
    for (uint64_t f = 0; f < limit; f++) {
        if ((f % 64) == 0 && PMM::GetBitmapQword(f / 64) == ~0ULL) { run_len = 0; f += 63; continue; }
        // a free frame that still has a live user pte is treated as used, same
        // rule as AllocFrame's task 26 guard (it is not leaked here). (satoru)
        bool busy = PMM::IsFrameUsed((uint64_t)f * PAGE_SIZE) || PMM::IsFrameMapped((uint64_t)f * PAGE_SIZE);
        if (!busy && protect && frame_window_protected(f)) busy = true;
        if (busy) { run_len = 0; continue; }
        if (run_len == 0) run_start = f;
        if (++run_len < count) continue;
        PMM::CommitContiguous(run_start, count);
        uint64_t phys = run_start * PAGE_SIZE;
        uint64_t* zp = (uint64_t*)PMM::PhysToVirt(phys);
        uint64_t qcount = (count * PAGE_SIZE) / 8;
        for (uint64_t z = 0; z < qcount; z++) zp[z] = 0;
        return phys;
    }
    return 0;
}

uint64_t PMM::AllocContiguousBelow(uint64_t count, uint64_t max_phys) {
    if (count == 0) return 0;
    uint64_t limit = max_phys / PAGE_SIZE;
    if (limit > total_frames) limit = total_frames;
    if (count > limit) return 0;
    bool protect = g_image_soft && g_image_full_hi != 0;
    uint64_t phys = alloc_contig_below_scan(count, limit, protect);
    if (!phys && protect) phys = alloc_contig_below_scan(count, limit, false);
    if (!phys) {
        SerialLogger::Log("PMM: AllocContiguousBelow failed (");
        SerialLogger::LogDec((uint32_t)count);
        SerialLogger::Log(" frames below 0x");
        SerialLogger::LogHex64(max_phys);
        SerialLogger::Log(")\r\n");
    }
    return phys;
}

void* PMM::AllocBytesBelow(size_t bytes, uint64_t max_phys) {
    if (bytes == 0) return nullptr;
    if (bytes > (size_t)-1 - (PAGE_SIZE - 1)) return nullptr;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = AllocContiguousBelow(pages, max_phys);
    if (phys) site_tag(phys / PAGE_SIZE, pages, __builtin_return_address(0));
    return phys ? PhysToVirt(phys) : nullptr;
}

// kurono.pmmtest=1: hand out every free frame through AllocFrame, chain them
// through their own first qword, fill the rest with a phys-derived pattern,
// verify the whole chain, then free it all. proves the identity map, the bitmap
// and the refcounts reach the new top of ram before anything else runs. a frame
// handed out twice breaks the chain count. (satoru)
static void pmm_self_test() {
    const uint64_t K = 0x9E3779B97F4A7C15ULL;
    uint64_t free0 = PMM::GetFreeFrames();
    uint64_t hi0   = PMM::CountUsedFramesFrom(PMM::LEGACY_CAP_BYTES);
    SerialLogger::Log("[pmmtest] begin free_frames=");
    SerialLogger::LogDec((uint32_t)free0);
    SerialLogger::Log(" phys_top=0x");
    SerialLogger::LogHex64(PMM::GetPhysTop());
    SerialLogger::Log("\r\n");
    uint64_t head = 0, n = 0, above = 0, maxp = 0, bad = 0;
    for (uint64_t i = 0; i < free0; i++) {
        uint64_t f = PMM::AllocFrame();
        if (!f) { bad++; break; }
        uint64_t* p = (uint64_t*)PMM::PhysToVirt(f);
        p[0] = head;
        for (int w = 1; w < 512; w++) p[w] = f ^ ((uint64_t)w * K);
        head = f; n++;
        if (f >= PMM::LEGACY_CAP_BYTES) above++;
        if (f > maxp) maxp = f;
    }
    uint64_t hi_peak = PMM::CountUsedFramesFrom(PMM::LEGACY_CAP_BYTES) - hi0;
    uint64_t hi_ctr  = PMM::HighFramesInUse();
    uint64_t walked = 0, mism = 0;
    for (uint64_t cur = head; cur && walked <= n; walked++) {
        const uint64_t* p = (const uint64_t*)PMM::PhysToVirt(cur);
        for (int w = 1; w < 512; w++) {
            if (p[w] != (cur ^ ((uint64_t)w * K))) {
                if (mism < 4) {
                    SerialLogger::Log("[pmmtest] MISMATCH phys=0x");
                    SerialLogger::LogHex64(cur);
                    SerialLogger::Log(" word=");
                    SerialLogger::LogDec(w);
                    SerialLogger::Log("\r\n");
                }
                mism++;
                break;
            }
        }
        cur = p[0];
    }
    for (uint64_t cur = head, k = 0; cur && k < n; k++) {
        uint64_t next = *(const uint64_t*)(uintptr_t)cur;
        PMM::FreeFrame(cur);
        cur = next;
    }
    uint64_t free1 = PMM::GetFreeFrames();
    uint64_t hi1   = PMM::CountUsedFramesFrom(PMM::LEGACY_CAP_BYTES);
    bool pass = bad == 0 && mism == 0 && walked == n && free1 == free0 && hi1 == hi0 && n == free0 &&
                hi_ctr == above && PMM::HighFramesInUse() == 0;
    SerialLogger::Log("[pmmtest] frames=");
    SerialLogger::LogDec((uint32_t)n);
    SerialLogger::Log(" above16g=");
    SerialLogger::LogDec((uint32_t)above);
    SerialLogger::Log(" used_above16g_delta=");
    SerialLogger::LogDec((uint32_t)hi_peak);
    SerialLogger::Log(" hi_counter=");
    SerialLogger::LogDec((uint32_t)hi_ctr);
    SerialLogger::Log(" max_phys=0x");
    SerialLogger::LogHex64(maxp);
    SerialLogger::Log(" walked=");
    SerialLogger::LogDec((uint32_t)walked);
    SerialLogger::Log(" mismatches=");
    SerialLogger::LogDec((uint32_t)mism);
    SerialLogger::Log(" free_after=");
    SerialLogger::LogDec((uint32_t)free1);
    SerialLogger::Log(pass ? " PASS\r\n" : " FAIL\r\n");
}

// if u touch this and something crashes in firefox a week later, its this
void PMM::Init(multiboot_info_t* mbi) {
    SerialLogger::Log("PMM: Initializing physical memory manager...\r\n");

    // everything Init needs from the boot info is copied out first, before the
    // frame metadata is written anywhere. (satoru)
    const char* cmdline = (mbi->flags & (1 << 2)) && mbi->cmdline
                          ? (const char*)(uintptr_t)mbi->cmdline : nullptr;
    uint64_t memcap_gb = 0;
    if (const char* v = pmm_find_token(cmdline, "kurono.memcap=")) {
        while (*v >= '0' && *v <= '9') { memcap_gb = memcap_gb * 10 + (uint64_t)(*v - '0'); v++; if (memcap_gb > 1000000) break; }
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.pmmtest=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_pmm_selftest = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.memcensus=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_census_req = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.pmmhigh=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_pmm_high_first = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.directmap=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_direct_req = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.ptaudit=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_ptaudit = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.pttrace=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_pttrace = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.window.keep=1")) {
        if (*v == 0 || *v == ' ' || *v == '\t') g_window_keep = true;
    }
    if (const char* v = pmm_find_token(cmdline, "kurono.imgwin=")) {
        if (v[0] == 'r' && v[1] == 'e' && v[2] == 's') g_image_force = 1;
        else if (v[0] == 'd' && v[1] == 'e' && v[2] == 'm') g_image_force = 2;
    }
    bool have_fb = (mbi->flags & (1 << 12)) && mbi->framebuffer_addr != 0;
    uint64_t fb_base = 0, fb_end = 0;
    if (have_fb) {
        uint64_t fb_size = (uint64_t)mbi->framebuffer_pitch * mbi->framebuffer_height;
        fb_base = mbi->framebuffer_addr & ~(PAGE_SIZE - 1);
        fb_end  = mbi->framebuffer_addr + fb_size;
    }

    uint64_t max_addr = 0;
    bool have_mmap = (mbi->flags & (1 << 6)) != 0;

    if (have_mmap) {
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        uint64_t offset = mbi->mmap_addr;

        while (offset < mmap_end) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;
            // size the bitmap from usable ram only. reserved entries (bios,
            // pci hole, qemu's ~1tb hypertransport range on amd hosts) used to
            // push max_addr straight to the 16gb cap on every machine, so the
            // os thought it had 16gb and paid 8mb of refcounts for it. (satoru)
            uint64_t region_end = entry->base_addr + entry->length;
            if (entry->type == 1 && region_end > max_addr) max_addr = region_end;
            if (g_range_count < PMM_MAX_RANGES) {
                g_ranges[g_range_count].base = entry->base_addr;
                g_ranges[g_range_count].end  = region_end;
                g_ranges[g_range_count].type = entry->type;
                g_range_count++;
            } else {
                static bool s_logged = false;
                if (!s_logged) { s_logged = true; SerialLogger::Log("PMM: WARNING - memory map has more than 512 entries, using the loader map directly\r\n"); }
                g_ranges_truncated = true;
            }
            offset += entry->size + 4;
        }
    } else if (mbi->flags & (1 << 0)) {
        max_addr = ((uint64_t)mbi->mem_upper + 1024) * 1024;
    } else {
        max_addr = 128ULL * 1024 * 1024;
    }

    // the managed range used to stop at a fixed 16 gb. it now follows the ram,
    // bounded by the boot identity map: kurono_boot.asm maps 0..512 gb with 2 mb
    // pages (NUM_PDS) on both boot paths, and every frame is reached through
    // that map (zero-on-alloc, page tables, the heap). the efi path's 1 gb pages
    // above 512 gb cannot be demoted by the vmm, so ram there stays unmanaged.
    // kurono.memcap=N (gb) lowers the bound; kurono.memcap=16 is the old cap. (satoru)
    uint64_t ram_top = max_addr;
    uint64_t ceiling = IDENTITY_CEIL_BYTES;
    if (memcap_gb >= 1 && (memcap_gb << 30) < ceiling) ceiling = memcap_gb << 30;
    if (max_addr > ceiling) max_addr = ceiling;

    total_frames = max_addr / PAGE_SIZE;
    used_frames  = total_frames;

    uint64_t bitmap_bytes = (total_frames + 63) / 64 * 8;
    bitmap_size = bitmap_bytes / 8;
    uint64_t refs_bytes = total_frames * sizeof(uint16_t);
    uint64_t mapped_bytes = (total_frames + 7) / 8;
    uint64_t meta_bytes = bitmap_bytes + refs_bytes + mapped_bytes;

    // metadata placement. it has always sat right after the kernel image; that
    // stays the first choice, but it now has to be checked: the arrays scale with
    // ram (bitmap + refs + mapped bits = 2.25 bytes per 4k frame, ~18 mb at 32 gb,
    // ~72 mb at 128 gb), and on real firmware a reserved range can sit a few mb
    // past the kernel. the span must be ram per the firmware map, inside the
    // managed range, and clear of the kernel, the low 1 mb, the below-kernel user
    // image window, the overlay windows and the framebuffer. otherwise the
    // lowest usable span that is clear of all of those is used. (satoru)
    // the kernel image no longer lives at its link address: it is loaded low
    // and the boot stub maps the link range onto it. reservations, metadata
    // placement and the zero-on-alloc identity pointers all follow the PHYSICAL
    // load range now; the link symbols are only used to describe the user
    // visible image window. (satoru)
    uint64_t boot_lo = (uint64_t)&kernel_boot_start;
    uint64_t boot_hi = (uint64_t)&kernel_boot_end;
    uint64_t kern_start_phys = (uint64_t)&kernel_phys_start;
    uint64_t kern_end_phys   = (uint64_t)&kernel_phys_end;

    // fixed user image window: everything below the old kernel link base is a
    // legal va for a low et_exec. below ~1.75 gb only the first 64 mb is
    // offered: a 300 mb fixed image cannot run there (its buffer plus its
    // pages do not fit), and the smaller window keeps boot allocations out of
    // the low static-binary range without reserving half of a small machine.
    // from 1.75 gb up the whole range is offered, still virtual only. (satoru)
    uint64_t win_hi = (uint64_t)&user_image_ceiling;
    if (ram_top < (1792ULL << 20) && win_hi > 0x4000000ULL) win_hi = 0x4000000ULL;
    if (win_hi <= 0x100000ULL) win_hi = 0;

    // panic minidump slots: below 2 gb the image starts at 16 mb, so use 15 mb;
    // above 2 gb use the top of the user image window, which is never mapped
    // by a process and so a panic write there is safe under any cr3. both are
    // reserved here so the allocator never hands them out. (satoru)
    uint64_t dump_lo = (ram_top < (2ULL << 30)) ? 0xF00000ULL : win_hi;
    uint64_t dump_hi = dump_lo ? dump_lo + 4 * PAGE_SIZE : 0;   // struct is ~4.6 kb (satoru)
    g_dump_lo = dump_lo;
    g_dump_hi = dump_hi;

    uint64_t first = (kern_end_phys + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    auto meta_ok = [&](uint64_t lo) -> bool {
        uint64_t hi = lo + meta_bytes;
        if (lo < 0x100000ULL || hi > max_addr || hi < lo) return false;
        if (span_overlaps(lo, hi, kern_start_phys & ~(PAGE_SIZE - 1), kern_end_phys)) return false;
        if (span_overlaps(lo, hi, boot_lo, boot_hi)) return false;
        if (dump_hi && span_overlaps(lo, hi, dump_lo, dump_hi)) return false;
        if (win_hi && span_overlaps(lo, hi, 0x100000ULL, win_hi)) return false;
        for (int i = 0; i < OVERLAY_COUNT; i++)
            if (span_overlaps(lo, hi, g_overlay[i].lo, g_overlay[i].hi)) return false;
        if (have_fb && span_overlaps(lo, hi, fb_base, fb_end)) return false;
        return !have_mmap || pmm_span_is_ram(lo, hi);
    };
    uint64_t meta_base = first;
    bool relocated = false;
    if (!meta_ok(first)) {
        // candidates: the start of every usable range, and the end of every
        // excluded span. lowest valid one at or above the kernel wins, else the
        // lowest valid one below it. (satoru)
        uint64_t best_hi = ~0ULL, best_lo = ~0ULL;
        auto consider = [&](uint64_t c) {
            c = (c + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (!meta_ok(c)) return;
            if (c >= first) { if (c < best_hi) best_hi = c; }
            else            { if (c < best_lo) best_lo = c; }
        };
        for (int i = 0; i < g_range_count; i++) {
            if (g_ranges[i].type != 1) continue;
            consider(g_ranges[i].base < 0x100000ULL ? 0x100000ULL : g_ranges[i].base);
        }
        for (int i = 0; i < OVERLAY_COUNT; i++) consider(g_overlay[i].hi);
        if (have_fb) consider(fb_end);
        if (dump_hi) consider(dump_hi);
        if (win_hi) consider(win_hi);
        consider(first);
        consider(boot_hi);
        uint64_t pick = best_hi != ~0ULL ? best_hi : best_lo;
        if (pick != ~0ULL) {
            meta_base = pick;
            relocated = true;
        } else {
            // nothing fits: keep the old placement so the boot is no worse than
            // before, and say so. (satoru)
            SerialLogger::Log("PMM: WARNING - no verified span for frame metadata, using kernel_end\r\n");
        }
    }

    g_meta_lo = meta_base;
    g_meta_hi = meta_base + meta_bytes;
    bitmap = (uint64_t*)(uintptr_t)meta_base;
    refs = (uint16_t*)(uintptr_t)(bitmap + bitmap_size);

    // qword-fill the bitmap (all 1s) and refs (all 0s) instead of byte-by-
    // byte; on multi-GB RAM these byte loops were a measurable chunk of boot
    // time. handle the sub-8-byte tail with a byte loop. (satoru)
    uint64_t* bp64 = (uint64_t*)bitmap;
    uint64_t  bq   = bitmap_bytes >> 3;
    for (uint64_t i = 0; i < bq; i++) bp64[i] = 0xFFFFFFFFFFFFFFFFULL;
    uint8_t*  bp   = (uint8_t*)bitmap;
    for (uint64_t i = bq << 3; i < bitmap_bytes; i++) bp[i] = 0xFF;

    uint64_t* rp64 = (uint64_t*)refs;
    uint64_t  rq   = refs_bytes >> 3;
    for (uint64_t i = 0; i < rq; i++) rp64[i] = 0;
    uint8_t*  rp   = (uint8_t*)refs;
    for (uint64_t i = rq << 3; i < refs_bytes; i++) rp[i] = 0;

    // task 26 free-while-mapped guard: the mapped bit array lives right after
    // refs; its frames are reserved with the refs reservation below (the loop
    // rounds refs_end up to mapped_end). (satoru)
    mapped_bits = (uint8_t*)refs + refs_bytes;
    for (uint64_t i = 0; i < mapped_bytes; i++) mapped_bits[i] = 0;

    SerialLogger::Log("PMM: Bitmap at 0x");
    SerialLogger::LogHex((uint64_t)bitmap);
    SerialLogger::Log(", ");
    SerialLogger::LogHex(bitmap_bytes);
    SerialLogger::Log(" bytes for ");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" frames\r\n");
    SerialLogger::Log("[pmm] ram_top=0x");
    SerialLogger::LogHex64(ram_top);
    SerialLogger::Log(" managed_top=0x");
    SerialLogger::LogHex64(max_addr);
    SerialLogger::Log(" memcap=");
    if (memcap_gb >= 1) { SerialLogger::LogDec((uint32_t)memcap_gb); SerialLogger::Log("gb"); }
    else SerialLogger::Log("none");
    SerialLogger::Log(" meta_kb=");
    SerialLogger::LogDec((uint32_t)(meta_bytes >> 10));
    SerialLogger::Log(relocated ? " meta=relocated@0x" : " meta=after-kernel@0x");
    SerialLogger::LogHex64(meta_base);
    SerialLogger::Log("\r\n");
    if (ram_top > max_addr) {
        SerialLogger::Log("[pmm] ram above 0x");
        SerialLogger::LogHex64(max_addr);
        SerialLogger::Log(memcap_gb >= 1 && (memcap_gb << 30) < IDENTITY_CEIL_BYTES
                          ? " left unmanaged (kurono.memcap)\r\n"
                          : " left unmanaged (beyond the 512 gb identity map)\r\n");
    }

    if (have_mmap) {
        auto free_ram = [&](uint64_t rbase, uint64_t rend) {
            uint64_t base = (rbase + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t end  = rend & ~(PAGE_SIZE - 1);
            if (end > base) g_installed_ram_bytes += end - base;   // counts ram above the frame cap too (satoru)

            uint64_t lim = end < max_addr ? end : (max_addr & ~(PAGE_SIZE - 1));
            for (uint64_t addr = base; addr < lim; addr += PAGE_SIZE) {
                uint64_t idx = addr / PAGE_SIZE;
                if (idx < total_frames && TestFrame(idx)) {
                    ClearFrame(idx);
                    used_frames--;
                    usable_frames++;
                }
            }
        };
        if (!g_ranges_truncated) {
            for (int ri = 0; ri < g_range_count; ri++)
                if (g_ranges[ri].type == 1) free_ram(g_ranges[ri].base, g_ranges[ri].end);
        } else {
            // the copy overflowed: walk the loader's map like the old code did
            // (the metadata then sits at kernel_end, also like before). (satoru)
            uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
            for (uint64_t offset = mbi->mmap_addr; offset < mmap_end; ) {
                MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;
                if (entry->type == 1) free_ram(entry->base_addr, entry->base_addr + entry->length);
                offset += entry->size + 4;
            }
        }
    } else {
        SerialLogger::Log("PMM: No mmap - using fallback (1MB..max_addr free)\r\n");
        for (uint64_t addr = 0x100000; addr < max_addr; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && TestFrame(idx)) {
                ClearFrame(idx);
                used_frames--;
                usable_frames++;
            }
        }
    }

    for (uint64_t f = 0; f < (1024 * 1024) / PAGE_SIZE; f++) {
        if (f < total_frames && !TestFrame(f)) {
            SetFrame(f);
            used_frames++;
        }
    }

    // the low boot stub, its page tables and the boot stack are live for the
    // whole session (the vmm walks the boot-built tables through their identity
    // addresses). (satoru)
    for (uint64_t addr = boot_lo & ~(PAGE_SIZE - 1); addr < boot_hi; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }
    if (dump_hi && dump_lo < max_addr) {
        for (uint64_t addr = dump_lo; addr < dump_hi && addr < max_addr; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && !TestFrame(idx)) {
                SetFrame(idx);
                used_frames++;
            }
        }
    }

    for (uint64_t addr = kern_start_phys & ~(PAGE_SIZE - 1);
         addr < kern_end_phys; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }

    // a kernel linked high (KURONO_KERNEL_BASE) leaves 1 MiB .. kernel_start free
    // for a fixed-address user image (claude is an ET_EXEC at 0x200000). the kernel
    // reaches every frame through the identity map, which that user image overlays
    // in its own page tables, so the pmm must never hand those frames out. (satoru)
    // item 9 (512 mb halted at boot, "Start() with no kernel processes"): the
    // full window is 319 mb with the 0x14000000 link, and on a 512 mb machine it
    // left ~135 mb, which the heap (a quarter of ram) took nearly all of, so no
    // kernel stack could be allocated. the window is only that large for claude's
    // 300 mb fixed image, which cannot run in under 2 gb anyway; there keep 64 mb,
    // enough for the static musl tools (busybox, recovery tools, ffmpeg at
    // 0x400000). item 8's map guard follows g_image_hi, so a user image past it is
    // refused instead of shadowing a frame the pmm hands out. (satoru)
    if (win_hi) {
        SerialLogger::Log("[pmm] user image window 1 mb..0x");
        SerialLogger::LogHex64(win_hi);
        SerialLogger::Log(" virtual only, demand-backed after boot\r\n");
        g_image_lo = 0x100000;
        g_image_hi = win_hi;
        g_image_full_hi = win_hi;   // virtual capacity for Release/TryReserve (satoru)
        for (uint64_t addr = 0x100000; addr < win_hi; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && !TestFrame(idx)) {
                SetFrame(idx);
                used_frames++;
            }
        }
    }

    // same trap for the fixed main stack of every user process (8 mb below
    // 0x40200000): a kernel write to these frames through the identity map lands on
    // the running process's stack instead. firefox's stack was zeroed this way, and
    // once the kernel's own data reached this range every new process died at start.
    // the overlay table also covers ld-kurono's trampoline page at 24 gb. (satoru)
    for (int w = 0; w < OVERLAY_COUNT; w++) {
        for (uint64_t addr = g_overlay[w].lo; addr < g_overlay[w].hi; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && !TestFrame(idx)) {
                SetFrame(idx);
                used_frames++;
            }
        }
    }

    uint64_t bm_start = (uint64_t)bitmap;
    uint64_t bm_end   = bm_start + bitmap_bytes;
    uint64_t refs_start = (uint64_t)refs;
    uint64_t refs_end = refs_start + refs_bytes;
    for (uint64_t addr = bm_start & ~(PAGE_SIZE - 1);
         addr < bm_end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }
    // reservation extended past refs to cover the mapped bit array too. (satoru)
    uint64_t meta_end = (uint64_t)mapped_bits + mapped_bytes;
    for (uint64_t addr = refs_start & ~(PAGE_SIZE - 1);
         addr < meta_end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }

    if (have_fb) {
        g_fb_reserved_bytes = (fb_end - fb_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t addr = fb_base; addr < fb_end; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && !TestFrame(idx)) {
                SetFrame(idx);
                used_frames++;
            }
        }
    }

    for (uint64_t idx = 0; idx < total_frames; idx++) {
        refs[idx] = TestFrame(idx) ? 1 : 0;
    }

    // next-fit starts after the metadata when it sits after the kernel (as
    // always); a relocated metadata block starts the search after the kernel
    // instead, so early single-frame allocations stay low like before. (satoru)
    uint64_t allocator_end = refs_end > bm_end ? refs_end : bm_end;
    search_hint = relocated ? first / PAGE_SIZE
                            : (allocator_end + PAGE_SIZE - 1) / PAGE_SIZE;

    SerialLogger::Log("PMM: Total=");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" Used=");
    SerialLogger::LogHex(used_frames);
    SerialLogger::Log(" Free=");
    SerialLogger::LogHex(total_frames - used_frames);
    SerialLogger::Log(" Usable=");
    SerialLogger::LogDec((uint32_t)(usable_frames * PAGE_SIZE / (1024 * 1024)));
    SerialLogger::Log("MB\r\n");

    if (total_frames > LEGACY_CAP_BYTES / PAGE_SIZE) g_hi_first = LEGACY_CAP_BYTES / PAGE_SIZE;

    // the census tag array (4 bytes a frame) comes from the pmm itself, after the
    // reservations above. frames handed out before this point stay untagged. (satoru)
    if (g_census_req) {
        uint64_t bytes = total_frames * sizeof(uint32_t);
        uint64_t phys = AllocContiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (phys) {
            g_site = (uint32_t*)PMM::PhysToVirt(phys);   // zeroed by AllocContiguous (satoru)
            SerialLogger::Log("[memcensus] site tags on, kb=");
            SerialLogger::LogDec((uint32_t)(bytes >> 10));
            SerialLogger::Log("\r\n");
        } else {
            SerialLogger::Log("[memcensus] site tags: allocation failed, census off\r\n");
        }
    }

    // the self-test patterns every free frame. on an mb2 boot (grub multiboot2,
    // the efi loader, every test rig) the boot info was copied into the kernel
    // first; on an mb1 boot loader modules sit in unreserved ram and would be
    // overwritten, so use it with multiboot2 only. (satoru)
    if (g_pmm_selftest) pmm_self_test();

    // test knob: start the single-frame next-fit search at the old 16 gb cap so
    // page tables, user pages and small kernel allocations come from the newly
    // managed range first. contiguous allocations stay first-fit from 0. (satoru)
    if (g_pmm_high_first && total_frames > LEGACY_CAP_BYTES / PAGE_SIZE) {
        search_hint = LEGACY_CAP_BYTES / PAGE_SIZE;
        g_pmm_high_floor = search_hint;
        SerialLogger::Log("[pmm] kurono.pmmhigh=1: single-frame allocations start at 16 gb\r\n");
    }
}
