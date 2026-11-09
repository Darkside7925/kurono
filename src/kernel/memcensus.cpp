#include "memcensus.h"
#include "../drivers/timer.h"   // lowmem sampling cadence (satoru)
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "kmemx_pool.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../linux/linux_syscall.h"

// see memcensus.h. one report walks every frame once (2M frames at 8 gb), so
// it runs from its own low-priority kernel process, never from a hot path. the
// walk is unlocked and racy by design: numbers can be off by the handful of
// frames that change during the walk. (satoru)
namespace MemCensus {
namespace {

struct SiteRow {
    uint32_t site;
    uint32_t count, unmapped;
    uint32_t prev, prev_unmapped;
    bool     used;
};
constexpr int SITE_CAP = 2048;
SiteRow  g_rows[SITE_CAP];
uint8_t* g_bits = nullptr;
uint64_t g_nframes = 0;
bool     g_on = false;
uint32_t g_overflow = 0;
uint32_t g_reports = 0;
uint32_t g_period_s = 30;
volatile uint32_t g_busy = 0;
uint64_t g_prev_free = 0, g_prev_heap = 0, g_prev_tables = 0, g_prev_leaves = 0;
SchedulerProcessSnapshot g_snap[512];

void log_u(uint64_t v);
// kernel heap rows: live bytes and allocation count per caller site. (satoru)
struct HeapRow { uint32_t site; uint32_t n; uint64_t bytes; uint64_t prev; bool used; };
constexpr int HROW_CAP = 1024;
HeapRow g_hrows[HROW_CAP];
HeapRow* hrow_for(uint32_t site) {
    uint32_t h = (site * 2654435761u) & (HROW_CAP - 1);
    for (int i = 0; i < HROW_CAP; i++) {
        HeapRow* r = &g_hrows[(h + i) & (HROW_CAP - 1)];
        if (r->used && r->site == site) return r;
        if (!r->used) { r->used = true; r->site = site; return r; }
    }
    return nullptr;
}
void print_hrow(const HeapRow* r) {
    SerialLogger::Log("[memcensus] heap site=0x");
    SerialLogger::LogHex64(r->site);
    SerialLogger::Log(" kb="); SerialLogger::LogDec((uint32_t)(r->bytes >> 10));
    SerialLogger::Log(" (");
    int64_t d = ((int64_t)r->bytes - (int64_t)r->prev) >> 10;
    if (d < 0) { SerialLogger::Log("-"); d = -d; } else SerialLogger::Log("+");
    SerialLogger::LogDec((uint32_t)d);
    SerialLogger::Log(") allocs="); SerialLogger::LogDec(r->n);
    SerialLogger::Log("\r\n");
}
// item 10: the user region table of the process that uses the most slots.
// regions live on the thread-group leader; a racy read of a live table. (satoru)
uint64_t g_rstart[PROCESS_MAX_USER_REGIONS];
uint32_t g_ridx[PROCESS_MAX_USER_REGIONS];
void region_profile(Process* best, int best_n);
void region_report() {
    Process* best = nullptr; int best_n = 0;
    // walk the linux process records: the scheduler snapshot does not list
    // tasks queued on the application processors. (satoru)
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* lp = LinuxSyscall::GetProcess(i);
        Process* p = lp ? lp->task : nullptr;
        if (!p || p->is_thread() || p == best) continue;
        int n = LinuxSyscall::ActiveRegionCount(p);   // inline + extension (satoru)
        if (n > best_n) { best_n = n; best = p; }
    }
    if (!best) return;
    region_profile(best, best_n);
}
void region_profile(Process* best, int best_n) {
    uint32_t stack = 0, heap = 0, dz = 0, file = 0, other = 0, s64k = 0, s1m = 0, s16m = 0, big = 0;
    int m = 0;
    for (int k = 0; k < PROCESS_MAX_USER_REGIONS; k++) {
        const UserMemoryRegion& r = best->regions[k];
        if (!r.active) continue;
        if (r.flags & USER_REGION_STACK) stack++;
        else if (r.flags & USER_REGION_HEAP) heap++;
        else if (r.file_node) file++;
        else if (r.flags & USER_REGION_DEMAND_ZERO) dz++;
        else other++;
        uint64_t sz = r.end - r.start;
        if (sz <= (64ull << 10)) s64k++; else if (sz <= (1ull << 20)) s1m++;
        else if (sz <= (16ull << 20)) s16m++; else big++;
        g_rstart[m] = r.start; g_ridx[m] = (uint32_t)k; m++;
    }
    // sort by start (insertion sort over indices; m <= 4096, census only). (satoru)
    for (int i = 1; i < m; i++) {
        uint64_t ks = g_rstart[i]; uint32_t ki = g_ridx[i]; int j = i - 1;
        while (j >= 0 && g_rstart[j] > ks) { g_rstart[j + 1] = g_rstart[j]; g_ridx[j + 1] = g_ridx[j]; j--; }
        g_rstart[j + 1] = ks; g_ridx[j + 1] = ki;
    }
    uint32_t mergeable = 0, gap1 = 0;
    for (int i = 1; i < m; i++) {
        const UserMemoryRegion& a = best->regions[g_ridx[i - 1]];
        const UserMemoryRegion& b = best->regions[g_ridx[i]];
        if (a.page_flags != b.page_flags || a.flags != b.flags) continue;
        if (a.end == b.start && !a.file_node && !b.file_node) mergeable++;
        else if (b.start > a.end && b.start - a.end <= 0x1000) gap1++;
    }
    SerialLogger::Log("[memcensus]   regions pid="); log_u(best->pid);
    SerialLogger::Log(" "); SerialLogger::Log(best->name);
    SerialLogger::Log(" active="); log_u((uint64_t)best_n);
    SerialLogger::Log(" (inline profile below covers the first ");
    log_u(PROCESS_MAX_USER_REGIONS); SerialLogger::Log(" slots)");
    SerialLogger::Log(" stack="); log_u(stack);
    SerialLogger::Log(" heap="); log_u(heap);
    SerialLogger::Log(" anon="); log_u(dz);
    SerialLogger::Log(" file="); log_u(file);
    SerialLogger::Log(" other="); log_u(other);
    SerialLogger::Log(" | <=64k="); log_u(s64k);
    SerialLogger::Log(" <=1m="); log_u(s1m);
    SerialLogger::Log(" <=16m="); log_u(s16m);
    SerialLogger::Log(" >16m="); log_u(big);
    SerialLogger::Log(" | adjacent_same="); log_u(mergeable);
    SerialLogger::Log(" one_page_gap_same="); log_u(gap1);
    SerialLogger::Log("\r\n");
}

void heap_report() {
    uint32_t cap = KernelHeap::CensusCapacity();
    if (!cap) return;
    for (int i = 0; i < HROW_CAP; i++) { g_hrows[i].n = 0; g_hrows[i].bytes = 0; }
    uint64_t total = 0, n = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t site, size;
        if (!KernelHeap::CensusEntry(i, &site, &size)) continue;
        HeapRow* r = hrow_for(site);
        if (!r) continue;
        r->n++; r->bytes += size; total += size; n++;
    }
    SerialLogger::Log("[memcensus]   heap tracked_kb="); SerialLogger::LogDec((uint32_t)(total >> 10));
    SerialLogger::Log(" allocs="); SerialLogger::LogDec((uint32_t)n);
    SerialLogger::Log(" table_overflow="); SerialLogger::LogDec(KernelHeap::CensusOverflow());
    SerialLogger::Log("\r\n");
    bool shown[HROW_CAP];
    for (int i = 0; i < HROW_CAP; i++) shown[i] = false;
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < HROW_CAP; i++) {
            if (!g_hrows[i].used || shown[i] || !g_hrows[i].bytes) continue;
            if (best < 0 || g_hrows[i].bytes > g_hrows[best].bytes) best = i;
        }
        if (best < 0) break;
        shown[best] = true;
        print_hrow(&g_hrows[best]);
    }
    for (int k = 0; k < 10; k++) {
        int best = -1; int64_t bd = 0;
        for (int i = 0; i < HROW_CAP; i++) {
            if (!g_hrows[i].used || shown[i]) continue;
            int64_t d = (int64_t)g_hrows[i].bytes - (int64_t)g_hrows[i].prev;
            if (d < 0) d = -d;
            if (d > bd) { bd = d; best = i; }
        }
        if (best < 0 || bd < 32 * 1024) break;
        shown[best] = true;
        SerialLogger::Log("[memcensus] (mover)");
        print_hrow(&g_hrows[best]);
    }
    for (int i = 0; i < HROW_CAP; i++) g_hrows[i].prev = g_hrows[i].bytes;
}

void log_s(int64_t v) {
    if (v < 0) { SerialLogger::Log("-"); v = -v; } else SerialLogger::Log("+");
    SerialLogger::LogDec((uint32_t)v);
}
void log_u(uint64_t v) { SerialLogger::LogDec((uint32_t)v); }

SiteRow* row_for(uint32_t site) {
    uint32_t h = (site * 2654435761u) & (SITE_CAP - 1);
    for (int i = 0; i < SITE_CAP; i++) {
        SiteRow* r = &g_rows[(h + i) & (SITE_CAP - 1)];
        if (r->used && r->site == site) return r;
        if (!r->used) { r->used = true; r->site = site; return r; }
    }
    return nullptr;
}

void print_row(const SiteRow* r) {
    SerialLogger::Log("[memcensus] site=0x");
    SerialLogger::LogHex64(r->site);
    SerialLogger::Log(" frames="); log_u(r->count);
    SerialLogger::Log(" ("); log_s((int64_t)r->count - (int64_t)r->prev);
    SerialLogger::Log(") unmapped="); log_u(r->unmapped);
    SerialLogger::Log(" ("); log_s((int64_t)r->unmapped - (int64_t)r->prev_unmapped);
    SerialLogger::Log(") mb="); log_u((uint64_t)r->count >> 8);
    SerialLogger::Log("\r\n");
}

// low-memory trigger: besides the periodic report, sample free memory every 10 ms and
// take ONE extra report when it first drops under the floor (kurono.memcensus.lowmb=N,
// default 24 MB) - a transient spike that drains the pmm between two periodic reports
// (the 512 mb churn oom) is otherwise never seen, and a report from the fault path
// itself is not safe. re-arms once free memory is back above twice the floor. (satoru)
uint32_t g_low_mb = 24;
volatile uint32_t g_low_armed = 1;
[[noreturn]] void periodic_entry() {
    uint64_t next = Timer::GetRealMs64() + (uint64_t)g_period_s * 1000;
    for (;;) {
        Scheduler::SleepMs(10);
        CheckLow();
        if (Timer::GetRealMs64() >= next) {
            next = Timer::GetRealMs64() + (uint64_t)g_period_s * 1000;
            Report("periodic");
        }
    }
}

}  // namespace

bool Enabled() { return g_on; }

void RegionTableFull(Process* owner) {
    if (!g_on || !owner) return;
    int n = LinuxSyscall::ActiveRegionCount(owner);
    SerialLogger::Log("[memcensus] region table full or extended:\r\n");
    region_profile(owner, n);
}

void Init() {
    if (g_on || !PMM::CensusOn()) return;
    g_nframes = PMM::GetPhysTop() / PAGE_SIZE;
    g_bits = (uint8_t*)PMM::AllocBytes((size_t)((g_nframes + 7) / 8));
    if (!g_bits) { SerialLogger::Log("[memcensus] no mark bitmap, census off\r\n"); return; }
    g_on = true;
    KernelHeap::CensusEnable();
    SerialLogger::Log("[memcensus] ready frames=");
    log_u(g_nframes);
    SerialLogger::Log("\r\n");
}

void Report(const char* why) {
    if (!g_on) return;
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)) return;   // one report at a time (satoru)
    g_reports++;

    uint64_t nb = (g_nframes + 7) / 8;
    for (uint64_t i = 0; i < nb; i++) g_bits[i] = 0;
    uint64_t leaves = 0, tables = 0, created = 0, destroyed = 0;
    uint32_t live_as = KernelVMM::CensusMarkUser(g_bits, g_nframes, &leaves, &tables,
                                                 &created, &destroyed);

    for (int i = 0; i < SITE_CAP; i++) { g_rows[i].count = 0; g_rows[i].unmapped = 0; }
    uint64_t used = 0, untagged = 0, untagged_unmapped = 0;
    for (uint64_t f = 0; f < g_nframes; f++) {
        if (!PMM::CensusFrameUsed(f)) continue;
        used++;
        bool mapped = (g_bits[f >> 3] >> (f & 7)) & 1;
        uint32_t site = PMM::CensusSite(f);
        if (!site) { untagged++; if (!mapped) untagged_unmapped++; continue; }
        SiteRow* r = row_for(site);
        if (!r) { g_overflow++; continue; }
        r->count++;
        if (!mapped) r->unmapped++;
    }

    int ra = 0, re = 0, shm_n = 0;
    uint64_t shm_b = 0;
    LinuxSyscall::CensusCounts(&ra, &re, &shm_n, &shm_b);
    int ns = Scheduler::GetProcessSnapshot(g_snap, 512);
    int user_procs = 0, kprocs = 0, dead = 0;
    for (int i = 0; i < ns; i++) {
        if (g_snap[i].is_kernel_proc) kprocs++; else user_procs++;
        if (g_snap[i].state == Process_Terminated) dead++;
    }

    uint64_t free_f = PMM::GetFreeFrames();
    uint64_t heap_used = (uint64_t)KernelHeap::GetUsed();
    SerialLogger::Log("[memcensus] report #"); log_u(g_reports);
    SerialLogger::Log(" "); SerialLogger::Log(why ? why : "");
    SerialLogger::Log(" up_s="); log_u(Scheduler::NowMs() / 1000);
    SerialLogger::Log(" free_mb="); log_u(free_f >> 8);
    SerialLogger::Log(" ("); log_s(((int64_t)free_f - (int64_t)g_prev_free) >> 8);
    SerialLogger::Log(") used_frames="); log_u(used);
    SerialLogger::Log(" untagged="); log_u(untagged);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[memcensus]   heap used_kb="); log_u(heap_used >> 10);
    SerialLogger::Log(" ("); log_s(((int64_t)heap_used - (int64_t)g_prev_heap) >> 10);
    SerialLogger::Log(") total_kb="); log_u((uint64_t)KernelHeap::GetTotal() >> 10);
    SerialLogger::Log(" kmemx_pool_kb="); log_u(KMemXPool::TotalBytes() >> 10);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[memcensus]   user as live="); log_u(live_as);
    SerialLogger::Log(" created="); log_u(created);
    SerialLogger::Log(" destroyed="); log_u(destroyed);
    SerialLogger::Log(" leaves="); log_u(leaves);
    SerialLogger::Log(" ("); log_s((int64_t)leaves - (int64_t)g_prev_leaves);
    SerialLogger::Log(") tables="); log_u(tables);
    SerialLogger::Log(" ("); log_s((int64_t)tables - (int64_t)g_prev_tables);
    SerialLogger::Log(")\r\n");
    SerialLogger::Log("[memcensus]   linux recs active="); log_u((uint64_t)ra);
    SerialLogger::Log(" exited="); log_u((uint64_t)re);
    SerialLogger::Log(" memfd slots="); log_u((uint64_t)shm_n);
    SerialLogger::Log(" memfd_kb="); log_u(shm_b >> 10);
    SerialLogger::Log(" | sched user="); log_u((uint64_t)user_procs);
    SerialLogger::Log(" kernel="); log_u((uint64_t)kprocs);
    SerialLogger::Log(" terminated="); log_u((uint64_t)dead);
    SerialLogger::Log(" site_overflow="); log_u(g_overflow);
    SerialLogger::Log("\r\n");

    // top sites by live frames, then the biggest growers not already shown. (satoru)
    bool shown[SITE_CAP];
    for (int i = 0; i < SITE_CAP; i++) shown[i] = false;
    for (int k = 0; k < 20; k++) {
        int best = -1;
        for (int i = 0; i < SITE_CAP; i++) {
            if (!g_rows[i].used || shown[i] || g_rows[i].count == 0) continue;
            if (best < 0 || g_rows[i].count > g_rows[best].count) best = i;
        }
        if (best < 0) break;
        shown[best] = true;
        print_row(&g_rows[best]);
    }
    for (int k = 0; k < 12; k++) {
        int best = -1; int64_t bd = 0;
        for (int i = 0; i < SITE_CAP; i++) {
            if (!g_rows[i].used || shown[i]) continue;
            int64_t d = (int64_t)g_rows[i].count - (int64_t)g_rows[i].prev;
            if (d < 0) d = -d;
            if (d > bd) { bd = d; best = i; }
        }
        if (best < 0 || bd < 16) break;
        shown[best] = true;
        SerialLogger::Log("[memcensus] (mover)");
        print_row(&g_rows[best]);
    }
    for (int i = 0; i < SITE_CAP; i++) {
        g_rows[i].prev = g_rows[i].count;
        g_rows[i].prev_unmapped = g_rows[i].unmapped;
    }
    heap_report();
    region_report();
    g_prev_free = free_f; g_prev_heap = heap_used;
    g_prev_tables = tables; g_prev_leaves = leaves;
    SerialLogger::Log("[memcensus] end #"); log_u(g_reports);
    SerialLogger::Log("\r\n");
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
}

void SetLowMemFloor(uint32_t mb) { g_low_mb = mb; }
// called from the census loop AND from the x64 syscall entry (process context, the same
// context a syscall body runs in): in default mode a user program that owns the bsp starves
// the census kernel process, so the syscall-side check is the one that catches a spike. (satoru)
void CheckLow() {
    if (!g_on || !g_low_mb) return;
    uint64_t free_mb = PMM::GetFreeMemory() >> 20;
    if (free_mb < g_low_mb) {
        if (__atomic_exchange_n(&g_low_armed, 0u, __ATOMIC_ACQ_REL)) Report("lowmem");
    } else if (free_mb >= 2ull * g_low_mb) {
        __atomic_store_n(&g_low_armed, 1u, __ATOMIC_RELEASE);
    }
}
void StartPeriodic(uint32_t secs) {
    if (!g_on) return;
    if (secs >= 1) g_period_s = secs;
    Scheduler::SpawnKernelProcess("memcensus", periodic_entry, PRIO_LOW, 64, 256);
}

}  // namespace MemCensus

// end (satoru)
