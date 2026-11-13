#include "memacct.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "kmemx.h"
#include "../drivers/timer.h"

extern uint64_t pmm_installed_ram_bytes();      // pmm.cpp (satoru)
extern uint64_t linux_shm_total_bytes();        // linux_syscall.cpp: live memfd object bytes (satoru)

namespace MemAcct {
namespace {

// side table: the last walk per address space. lock-free; a slot is claimed by
// zeroing root, filling the fields, then publishing root last. a reader copies
// the fields and re-checks root, so a slot rewritten under it is just a miss.
// the values are display-only, so a lost race between two writers is harmless. (satoru)
struct Slot {
    volatile uint64_t root;
    uint32_t rss_kb, shared_kb, pss_kb, pt_kb;
    uint64_t stamp_ms;
};
constexpr int SLOTS = 256;
Slot g_slots[SLOTS];

inline const uint64_t* tbl(uint64_t phys) { return (const uint64_t*)PMM::PhysToVirt(phys); }
inline uint64_t ephys(uint64_t e) { return e & 0x000FFFFFFFFFF000ULL; }

// a table pointer is only followed if it is inside ram and the pmm says the
// frame is allocated: a table freed and reused mid-walk then reads as garbage
// ram, and a stray value can never point the walk at the pci hole. (satoru)
inline bool table_ok(uint64_t phys, uint64_t top) {
    return phys && phys < top && PMM::IsFrameUsed(phys);
}

bool walk_space(uint64_t root, SpaceMem* o) {
    const uint64_t top = PMM::GetPhysTop();
    if (!table_ok(root, top)) return false;
    const uint64_t UP = PTE_PRESENT | PTE_USER;
    uint64_t rss = 0, shr = 0, pss_bytes = 0, tables = 1;

    const uint64_t* l4 = tbl(root);
    for (int i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = l4[i4];
        if ((e4 & UP) != UP) continue;       // kernel entries (no PTE_USER) are not this process's memory
        uint64_t p3 = ephys(e4);
        if (!table_ok(p3, top)) continue;
        tables++;
        const uint64_t* l3 = tbl(p3);
        for (int i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = l3[i3];
            if ((e3 & UP) != UP || (e3 & PTE_HUGE)) continue;
            uint64_t p2 = ephys(e3);
            if (!table_ok(p2, top)) continue;
            tables++;
            const uint64_t* l2 = tbl(p2);
            for (int i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = l2[i2];
                if ((e2 & UP) != UP) continue;
                uint64_t p1 = ephys(e2);
                if (e2 & PTE_HUGE) {         // a user 2 mb page: not created today, counted if it ever is (satoru)
                    if (p1 < top) {
                        rss += 512;
                        if (e2 & PTE_SHARED) shr += 512;
                        pss_bytes += 512ULL * PAGE_SIZE;
                    }
                    continue;
                }
                if (!table_ok(p1, top)) continue;
                tables++;
                const uint64_t* l1 = tbl(p1);
                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = l1[i1];
                    if ((e1 & UP) != UP) continue;
                    uint64_t ph = ephys(e1);
                    if (ph >= top) continue;
                    uint32_t ref = PMM::GetFrameRefCount(ph);
                    if (ref == 0) continue;  // frame not owned by the allocator (device window): not ram
                    rss++;
                    if (e1 & PTE_SHARED) shr++;
                    pss_bytes += (ref > 1) ? PAGE_SIZE / ref : PAGE_SIZE;
                }
            }
        }
    }
    o->rss_kb    = (uint32_t)(rss * 4);
    o->shared_kb = (uint32_t)(shr * 4);
    o->pss_kb    = (uint32_t)(pss_bytes / 1024);
    o->pt_kb     = (uint32_t)(tables * 4);
    return true;
}

}   // namespace

bool QuerySpace(uint64_t root, uint32_t max_age_ms, SpaceMem* out, bool allow_walk) {
    if (!root || !out) return false;
    const uint64_t now = Timer::GetRealMs64();
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < SLOTS; i++) {
        Slot& s = g_slots[i];
        uint64_t r = s.root;
        if (r == root) {
            SpaceMem m = { s.rss_kb, s.shared_kb, s.pss_kb, s.pt_kb };
            uint64_t st = s.stamp_ms;
            if (s.root == root && (!allow_walk || now - st <= max_age_ms)) { *out = m; return true; }
            free_slot = i;
            break;
        }
        if (r == 0) { if (free_slot < 0) free_slot = i; }
        else if (g_slots[i].stamp_ms < g_slots[oldest].stamp_ms) oldest = i;
    }
    if (!allow_walk) return false;
    SpaceMem m;
    if (!walk_space(root, &m)) return false;
    int w = free_slot >= 0 ? free_slot : oldest;
    Slot& s = g_slots[w];
    s.root = 0;
    s.rss_kb = m.rss_kb; s.shared_kb = m.shared_kb; s.pss_kb = m.pss_kb; s.pt_kb = m.pt_kb;
    s.stamp_ms = now;
    s.root = root;
    *out = m;
    return true;
}

uint32_t SumUserPssKb(uint32_t max_age_ms) {
    const uint64_t now = Timer::GetRealMs64();
    uint64_t sum = 0;
    for (int i = 0; i < SLOTS; i++) {
        const Slot& s = g_slots[i];
        if (s.root && now - s.stamp_ms <= max_age_ms) sum += s.pss_kb;
    }
    return sum > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)sum;
}

void QuerySystem(SysMem* o) {
    uint64_t total = PMM::GetTotalMemory();
    uint64_t freeb = PMM::GetFreeMemory();
    if (freeb > total) freeb = total;
    o->total_kb = total / 1024;
    o->free_kb  = freeb / 1024;
    o->used_kb  = o->total_kb - o->free_kb;
    o->heap_arena_kb = (uint64_t)KernelHeap::GetTotal() / 1024;
    o->kmemx_pool_kb = KMemX::IsInitialized() ? KMemX::GetStats().pool_bytes / 1024 : 0;
    o->shm_kb = linux_shm_total_bytes() / 1024;
    o->installed_kb = pmm_installed_ram_bytes() / 1024;
}

uint64_t HeapUsedKbCached() {
    static uint64_t s_kb = 0, s_at = 0;
    uint64_t now = Timer::GetRealMs64();
    if (s_at == 0 || now - s_at > 5000) {
        s_kb = (uint64_t)KernelHeap::GetUsed() / 1024;
        s_at = now ? now : 1;
    }
    return s_kb;
}

namespace {
int put(char* buf, int p, int mx, const char* s) {
    while (*s && p < mx - 1) buf[p++] = *s++;
    buf[p] = 0;
    return p;
}
int put_kb(char* buf, int p, int mx, const char* key, uint64_t kb) {
    char num[24]; int n = 0;
    if (kb == 0) num[n++] = '0';
    while (kb) { num[n++] = (char)('0' + kb % 10); kb /= 10; }
    p = put(buf, p, mx, key);
    p = put(buf, p, mx, ":");
    for (int pad = (int)__builtin_strlen(key) + 1; pad < 16 && p < mx - 1; pad++) buf[p++] = ' ';
    for (int i = 0; i < 8 - n && p < mx - 1; i++) buf[p++] = ' ';
    while (n && p < mx - 1) buf[p++] = num[--n];
    buf[p] = 0;
    return put(buf, p, mx, " kB\n");
}
}   // namespace

int FormatMeminfo(char* buf, int mx) {
    SysMem m; QuerySystem(&m);
    int p = 0;
    buf[0] = 0;
    p = put_kb(buf, p, mx, "MemTotal",     m.total_kb);
    p = put_kb(buf, p, mx, "MemFree",      m.free_kb);
    // nothing in kurono is reclaimable on demand (kvfs lives in the kernel heap, no page cache): available == free. (satoru)
    p = put_kb(buf, p, mx, "MemAvailable", m.free_kb);
    p = put_kb(buf, p, mx, "Buffers",      0);
    p = put_kb(buf, p, mx, "Cached",       0);
    p = put_kb(buf, p, mx, "SwapCached",   0);
    p = put_kb(buf, p, mx, "SwapTotal",    0);
    p = put_kb(buf, p, mx, "SwapFree",     0);
    p = put_kb(buf, p, mx, "Shmem",        m.shm_kb);
    p = put_kb(buf, p, mx, "Slab",         HeapUsedKbCached());
    return p;
}

}   // namespace MemAcct

// end (satoru)
