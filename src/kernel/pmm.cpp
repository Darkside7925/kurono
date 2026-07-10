#include "pmm.h"
#include "../drivers/serial.h"
#include "../proc/smp.h"   // cpu index for the cross-core pmm lock (satoru)

//  physical memory manager - bitmap frame allocator implementation

// static member definitions
uint64_t* PMM::bitmap       = nullptr;
uint16_t* PMM::refs         = nullptr;
uint64_t  PMM::bitmap_size  = 0;
uint64_t  PMM::total_frames = 0;
uint64_t  PMM::used_frames  = 0;
uint64_t  PMM::search_hint  = 0;

extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;       // after .boot_tables - safe to place bitmap here

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

uint64_t PMM::FindFreeFrame() {
    uint64_t qwords = bitmap_size;
    if (qwords == 0) return ~0ULL;

    uint64_t start_q = search_hint / 64;
    if (start_q >= qwords) start_q = 0;

    for (uint64_t pass = 0; pass < qwords; pass++) {
        uint64_t qi = (start_q + pass) % qwords;
        uint64_t val = bitmap[qi];
        if (val == ~0ULL) continue;

        // ~val: 1 = free. find lowest set bit.
        uint64_t inv = ~val;
        int bit = ctz64(inv);
        uint64_t frame = qi * 64 + (uint64_t)bit;
        if (frame >= total_frames) continue;
        search_hint = frame + 1;
        return frame;
    }
    return ~0ULL;
}

uint64_t PMM::AllocFrame() {
    PmmIrqGuard _g;
    uint64_t idx = FindFreeFrame();
    if (idx == ~0ULL) {
        SerialLogger::Log("PMM: OUT OF MEMORY!\r\n");
        return 0;
    }
    SetFrame(idx);
    refs[idx] = 1;
    used_frames++;
    uint64_t phys = idx * PAGE_SIZE;
    // zero-on-alloc: scrub the frame so we never hand out stale data.
    uint64_t* p = (uint64_t*)(uintptr_t)phys;
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i++) p[i] = 0;
    return phys;
}

void PMM::RetainFrame(uint64_t phys_addr) {
    PmmIrqGuard _g;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;
    if (refs[idx] == 0xFFFF) return;  // saturate to prevent wraparound
    refs[idx]++;
}

uint32_t PMM::GetFrameRefCount(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return 0;
    return refs[idx];
}

void PMM::FreeFrame(uint64_t phys_addr) {
    PmmIrqGuard _g;
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;  // double-free guard

    if (refs[idx] > 1) {
        refs[idx]--;
        return;
    }

    refs[idx] = 0;
    ClearFrame(idx);
    used_frames--;

    if (idx < search_hint) search_hint = idx;
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

uint64_t PMM::AllocContiguous(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return AllocFrame();
    PmmIrqGuard _g;   // atomic vs the #pf stack-grow path (the run-scan below mutates the bitmap directly) (satoru)
    // overflow guard
    if (count > total_frames) return 0;

    uint64_t qwords = bitmap_size;

    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (uint64_t qi = 0; qi < qwords; qi++) {
        uint64_t val = bitmap[qi];
        if (val == ~0ULL) {
            run_start = (qi + 1) * 64;
            run_len = 0;
            continue;
        }
        if (val == 0ULL) {
            uint64_t frame_base = qi * 64;
            if (run_len == 0) run_start = frame_base;
            // cap the run to total_frames
            uint64_t add = 64;
            if (frame_base + add > total_frames) add = total_frames - frame_base;
            run_len += add;
            if (run_len >= count) {
                bulk_set_frames(run_start, count, bitmap, used_frames, total_frames);
                for (uint64_t j = 0; j < count; j++) refs[run_start + j] = 1;
                search_hint = run_start + count;
                uint64_t phys = run_start * PAGE_SIZE;
                // zero the entire region
                uint64_t* zp = (uint64_t*)(uintptr_t)phys;
                uint64_t qcount = (count * PAGE_SIZE) / 8;
                for (uint64_t z = 0; z < qcount; z++) zp[z] = 0;
                return phys;
            }
            continue;
        }
        for (int bit = 0; bit < 64; bit++) {
            uint64_t fi = qi * 64 + (uint64_t)bit;
            if (fi >= total_frames) break;
            if (val & (1ULL << bit)) {
                run_start = fi + 1;
                run_len = 0;
            } else {
                if (run_len == 0) run_start = fi;
                run_len++;
                if (run_len >= count) {
                    bulk_set_frames(run_start, count, bitmap, used_frames, total_frames);
                    for (uint64_t j = 0; j < count; j++) refs[run_start + j] = 1;
                    search_hint = run_start + count;
                    uint64_t phys = run_start * PAGE_SIZE;
                    uint64_t* zp = (uint64_t*)(uintptr_t)phys;
                    uint64_t qcount = (count * PAGE_SIZE) / 8;
                    for (uint64_t z = 0; z < qcount; z++) zp[z] = 0;
                    return phys;
                }
            }
        }
    }
    SerialLogger::Log("PMM: AllocContiguous failed (");
    SerialLogger::LogDec((uint32_t)count);
    SerialLogger::Log(" frames requested)\r\n");
    return 0;
}

void PMM::FreeContiguous(uint64_t phys_addr, uint64_t count) {
    if (!phys_addr || count == 0) return;
    PmmIrqGuard _g;
    uint64_t start = phys_addr / PAGE_SIZE;
    if (start >= total_frames) return;
    if (count > total_frames - start) count = total_frames - start;

    // fast path: if all frames have refcount == 1, do bulk clear
    bool fast = true;
    for (uint64_t j = 0; j < count; j++) {
        if (refs[start + j] != 1) { fast = false; break; }
        if (!TestFrame(start + j))  { fast = false; break; }
    }
    if (fast) {
        for (uint64_t j = 0; j < count; j++) refs[start + j] = 0;
        bulk_clear_frames(start, count, bitmap, used_frames, total_frames);
        if (start < search_hint) search_hint = start;
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
    // AllocContiguous already zeroed the region
    return (void*)(uintptr_t)phys;
}

void PMM::FreeBytes(void* addr, size_t bytes) {
    if (!addr || bytes == 0) return;
    uint64_t phys = (uint64_t)(uintptr_t)addr;
    if (bytes > (size_t)-1 - (PAGE_SIZE - 1)) return;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    FreeContiguous(phys, pages);
}

void PMM::Init(multiboot_info_t* mbi) {
    SerialLogger::Log("PMM: Initializing physical memory manager...\r\n");

    uint64_t max_addr = 0;

    if (mbi->flags & (1 << 6)) {
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        uint64_t offset = mbi->mmap_addr;

        while (offset < mmap_end) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;
            uint64_t region_end = entry->base_addr + entry->length;
            if (region_end > max_addr) max_addr = region_end;
            offset += entry->size + 4;
        }
    } else if (mbi->flags & (1 << 0)) {
        max_addr = ((uint64_t)mbi->mem_upper + 1024) * 1024;
    } else {
        max_addr = 128ULL * 1024 * 1024;
    }

    if (max_addr > 16ULL * 1024 * 1024 * 1024) {
        max_addr = 16ULL * 1024 * 1024 * 1024;
    }

    total_frames = max_addr / PAGE_SIZE;
    used_frames  = total_frames;

    uint64_t bitmap_bytes = (total_frames + 63) / 64 * 8;
    bitmap_size = bitmap_bytes / 8;
    bitmap = (uint64_t*)(uintptr_t)( ((uint64_t)&kernel_end + 4095) & ~4095ULL );
    refs = (uint16_t*)(uintptr_t)(bitmap + bitmap_size);
    uint64_t refs_bytes = total_frames * sizeof(uint16_t);

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

    SerialLogger::Log("PMM: Bitmap at 0x");
    SerialLogger::LogHex((uint64_t)bitmap);
    SerialLogger::Log(", ");
    SerialLogger::LogHex(bitmap_bytes);
    SerialLogger::Log(" bytes for ");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" frames\r\n");

    if (mbi->flags & (1 << 6)) {
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        uint64_t offset = mbi->mmap_addr;

        while (offset < mmap_end) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;

            if (entry->type == 1) {
                uint64_t base = (entry->base_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                uint64_t end  = (entry->base_addr + entry->length) & ~(PAGE_SIZE - 1);

                for (uint64_t addr = base; addr < end; addr += PAGE_SIZE) {
                    uint64_t idx = addr / PAGE_SIZE;
                    if (idx < total_frames && TestFrame(idx)) {
                        ClearFrame(idx);
                        used_frames--;
                    }
                }
            }
            offset += entry->size + 4;
        }
    } else {
        SerialLogger::Log("PMM: No mmap - using fallback (1MB..max_addr free)\r\n");
        for (uint64_t addr = 0x100000; addr < max_addr; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && TestFrame(idx)) {
                ClearFrame(idx);
                used_frames--;
            }
        }
    }

    for (uint64_t f = 0; f < (1024 * 1024) / PAGE_SIZE; f++) {
        if (f < total_frames && !TestFrame(f)) {
            SetFrame(f);
            used_frames++;
        }
    }

    uint64_t kern_start_phys = (uint64_t)&kernel_start;
    uint64_t kern_end_phys   = (uint64_t)&kernel_end;
    for (uint64_t addr = kern_start_phys & ~(PAGE_SIZE - 1);
         addr < kern_end_phys; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
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
    for (uint64_t addr = refs_start & ~(PAGE_SIZE - 1);
         addr < refs_end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }

    if ((mbi->flags & (1 << 12)) && mbi->framebuffer_addr != 0) {
        uint64_t fb_size = (uint64_t)mbi->framebuffer_pitch * mbi->framebuffer_height;
        uint64_t fb_base = mbi->framebuffer_addr & ~(PAGE_SIZE - 1);
        uint64_t fb_end  = mbi->framebuffer_addr + fb_size;
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

    uint64_t allocator_end = refs_end > bm_end ? refs_end : bm_end;
    search_hint = (allocator_end + PAGE_SIZE - 1) / PAGE_SIZE;

    SerialLogger::Log("PMM: Total=");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" Used=");
    SerialLogger::LogHex(used_frames);
    SerialLogger::Log(" Free=");
    SerialLogger::LogHex(total_frames - used_frames);
    SerialLogger::Log("\r\n");
}
