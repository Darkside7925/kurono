#include "taskmem.h"
#include "heap.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"

// kurono.taskmem=1, see taskmem.h. three side tables, all mutated only with
// the kernel heap lock held (the heap hooks run inside it, the rest go through
// KernelHeap::LockedCall), so the counters need no atomics of their own:
//   recs  - owner records (pid or app), name, live heap bytes / blocks, frames
//   keys  - open-addressed key -> record index (key = pid or APP_KEY|app)
//   tags  - open-addressed payload address -> record index, one per tagged
//           live block. linear probing with backward-shift deletion, so there
//           are no tombstones and a lookup is bounded by the longest probe
//           any entry needed (normally a handful of slots).
// an untagged block is charged to "kernel": the free of a block that has no
// tag subtracts from kernel_bytes, so kernel_bytes stays the sum of the live
// untagged blocks (the enable-time baseline covers the ones from before). (satoru)

namespace TaskMem {

volatile bool g_on = false;

namespace {

bool g_req = false;
bool g_bench = false;
uint32_t g_bench_rec = 0;                       // boot microbenchmark: owner of every alloc (satoru)

constexpr uint32_t REC_CAP   = 4096;            // owner records (index 0 = kernel) (satoru)
constexpr uint32_t KEY_CAP   = 8192;            // key map slots, 2x records (satoru)
constexpr uint32_t PROBE_MAX = 32;              // insert probe bound before falling back to kernel (satoru)
constexpr uint32_t CPU_CAP   = SMP_MAX_CPUS;

struct Rec {
    uint32_t key;          // 0 = free slot (satoru)
    uint32_t pid;
    char     name[24];
    uint64_t heap_bytes;
    uint64_t heap_count;
    uint64_t frame_bytes;
    uint64_t peak_bytes;
};

Rec*      g_rec = nullptr;
uint32_t  g_rec_used = 1;                       // slots ever handed out incl. 0 (satoru)
uint64_t* g_keys = nullptr;                     // (key << 16) | rec, 0 = empty (satoru)

uint64_t* g_tag = nullptr;                      // ((payload >> 4) << 16) | rec, 0 = empty (satoru)
uint64_t  g_tag_cap = 0, g_tag_mask = 0;
uint32_t  g_tag_shift = 0;
uint64_t  g_tag_live = 0;
uint32_t  g_tag_dist = 0;                       // longest displacement of any live entry (satoru)
uint64_t  g_tag_max_cap = 0;

uint64_t  g_heap_live = 0, g_kernel_bytes = 0, g_kernel_frames = 0;
uint64_t  g_overflow = 0, g_stale = 0, g_sweeps = 0, g_grow_fail = 0;
uint64_t  g_grow_retry_at = 0;                  // after a failed grow, wait for more live tags (satoru)
uint32_t  g_gen = 1;                            // bumped when records are recycled (satoru)

// per-cpu: last resolved owner, interrupt mark, app scope (satoru)
uint32_t  g_cache_key[CPU_CAP], g_cache_rec[CPU_CAP], g_cache_gen[CPU_CAP];
volatile uint64_t g_irq_sp[CPU_CAP];
Process* volatile g_irq_proc[CPU_CAP];          // task interrupted when the mark went up (satoru)
volatile uint32_t g_scope_key[CPU_CAP], g_scope_pid[CPU_CAP];   // key: APP_KEY|app or the ui task pid (satoru)
volatile uint32_t g_ui_pid = 0;                 // the gui task (SetUiTask) (satoru)
char              g_ui_name[24];

// pmm blocks charged with ChargeFrames (few, long lived) (satoru)
constexpr int FR_CAP = 64;
struct FrameCharge { uint64_t block; uint64_t bytes; uint32_t rec; };
FrameCharge g_fr[FR_CAP];

// window id -> app (satoru)
constexpr int WIN_CAP = 128;
volatile uint64_t g_win[WIN_CAP];               // (id << 8) | app, 0 = empty (satoru)

const char* const k_app_names[APP_COUNT] = {
    "-", "Terminal", "Files", "Text Editor", "Settings", "Task Manager",
    "Calculator", "Media Player", "Browser", "Alpine", "System Settings", "App",
    "File cache (KVFS)"
};

inline uint32_t hash_tag(uint64_t k) {
    return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> g_tag_shift);
}
inline uint32_t hash_key(uint32_t k) {
    return (uint32_t)(((uint64_t)k * 0x9E3779B97F4A7C15ull) >> 51) & (KEY_CAP - 1);   // 13 bits (satoru)
}

void copy_name(char* dst, const char* src) {
    int i = 0;
    if (src) for (; i < 23 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ---- key map (satoru)
uint32_t key_find(uint32_t key) {
    uint32_t h = hash_key(key);
    for (uint32_t i = 0; i < KEY_CAP; i++) {
        uint64_t s = g_keys[(h + i) & (KEY_CAP - 1)];
        if (s == 0) return 0;
        if ((uint32_t)(s >> 16) == key) return (uint32_t)(s & 0xFFFF);
    }
    return 0;
}
void key_put(uint32_t key, uint32_t rec) {
    uint32_t h = hash_key(key);
    for (uint32_t i = 0; i < KEY_CAP; i++) {
        uint64_t& s = g_keys[(h + i) & (KEY_CAP - 1)];
        if (s == 0) { s = ((uint64_t)key << 16) | rec; return; }
    }
}

// recycle records that hold nothing; their tasks get a fresh record if they
// allocate again. the key map is rebuilt and every cpu cache invalidated. (satoru)
void sweep() {
    g_sweeps++;
    for (uint32_t i = 1; i < g_rec_used; i++) {
        Rec& r = g_rec[i];
        if (r.key && r.heap_count == 0 && r.frame_bytes == 0) r.key = 0;
    }
    for (uint32_t i = 0; i < KEY_CAP; i++) g_keys[i] = 0;
    for (uint32_t i = 1; i < g_rec_used; i++) if (g_rec[i].key) key_put(g_rec[i].key, i);
    g_gen++;
}

uint32_t rec_new(uint32_t key, uint32_t pid, const char* name) {
    uint32_t idx = 0;
    if (g_rec_used < REC_CAP) idx = g_rec_used++;
    else {
        for (int pass = 0; pass < 2 && !idx; pass++) {
            for (uint32_t i = 1; i < REC_CAP; i++) if (!g_rec[i].key) { idx = i; break; }
            if (!idx && pass == 0) sweep();
        }
        if (!idx) return 0;   // every record holds memory: charge to kernel (satoru)
    }
    Rec& r = g_rec[idx];
    r.key = key; r.pid = pid; copy_name(r.name, name);
    r.heap_bytes = r.heap_count = r.frame_bytes = r.peak_bytes = 0;
    key_put(key, idx);
    return idx;
}

// owner of an allocation made right now on `cpu` (heap lock held) (satoru)
inline Process* current_on(uint32_t cpu) {
    if (cpu != 0) { PerCpu* pc = SMP::ByIndex(cpu); return pc ? pc->current : nullptr; }
    return Scheduler::current_process;   // same rule as Scheduler::GetCurrentProcess (satoru)
}

uint32_t resolve(uint32_t cpu) {
    if (g_bench_rec) return g_bench_rec;
    if (cpu >= CPU_CAP) return 0;
    Process* p = current_on(cpu);
    uint64_t isp = g_irq_sp[cpu];
    if (isp && p == g_irq_proc[cpu]) {
        // interrupt work: still the interrupted task and still on the stack
        // below its interrupt frame. a handler that switched tasks leaves the
        // mark up, but the next task differs (or sits on another stack). (satoru)
        uint64_t sp; __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp));
        if (sp < isp && isp - sp < 128 * 1024) return 0;
    }
    if (!p) return 0;
    uint32_t pid = p->pid;
    if (pid == 0) return 0;
    uint32_t key = pid;
    uint32_t sk = g_scope_key[cpu];
    if (sk && g_scope_pid[cpu] == pid) key = sk;
    if (g_cache_key[cpu] == key && g_cache_gen[cpu] == g_gen) return g_cache_rec[cpu];
    uint32_t rec = key_find(key);
    if (!rec) {
        const char* nm = (key & APP_KEY) ? AppName(key & 0xFF) : (key == pid ? p->name : g_ui_name);
        rec = rec_new(key, (key & APP_KEY) ? pid : key, nm);
    }
    g_cache_key[cpu] = key; g_cache_rec[cpu] = rec; g_cache_gen[cpu] = g_gen;
    return rec;
}

// ---- tag table (satoru)
// 0 = no room within `limit` slots, 1 = new entry, 2 = replaced a stale entry
// for the same address (a block that left the heap without a free hook; its
// old owner keeps those bytes, counted in g_stale) (satoru)
int tag_insert_raw(uint64_t* tab, uint64_t mask, uint64_t k, uint32_t rec, uint32_t limit, uint32_t* dist) {
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> g_tag_shift);
    for (uint32_t i = 0; i < limit; i++) {
        uint64_t& s = tab[(h + i) & mask];
        if (s == 0) { s = (k << 16) | rec; if (i > *dist) *dist = i; return 1; }
        if ((s >> 16) == k) {
            uint32_t old = (uint32_t)(s & 0xFFFF);
            if (old < REC_CAP && g_rec[old].heap_count) g_rec[old].heap_count--;
            s = (k << 16) | rec; g_stale++;
            return 2;
        }
    }
    return 0;
}

// double the table (heap lock held; pmm under the heap lock is the existing
// order, see heap_grow). a failed allocation just leaves the table as it is. (satoru)
void tag_grow() {
    uint64_t ncap = g_tag_cap * 2;
    if (ncap > g_tag_max_cap) return;
    // set the retry point first: a failing pmm allocation logs, and the log
    // path can re-enter the heap (and this hook) on the same cpu (satoru)
    g_grow_retry_at = g_tag_live + g_tag_cap / 8;
    uint64_t* nt = (uint64_t*)PMM::AllocBytes((size_t)(ncap * 8));   // zeroed by AllocContiguous (satoru)
    if (!nt) { g_grow_fail++; return; }
    g_grow_retry_at = 0;
    uint32_t oshift = g_tag_shift;
    g_tag_shift = oshift - 1;
    uint32_t nd = 0;
    for (uint64_t i = 0; i < g_tag_cap; i++) {
        uint64_t s = g_tag[i];
        if (s) (void)tag_insert_raw(nt, ncap - 1, s >> 16, (uint32_t)(s & 0xFFFF), (uint32_t)ncap, &nd);
    }
    uint64_t* old = g_tag; uint64_t obytes = g_tag_cap * 8;
    g_tag = nt; g_tag_cap = ncap; g_tag_mask = ncap - 1; g_tag_dist = nd;
    PMM::FreeBytes(old, (size_t)obytes);
}

// remove the entry for k; returns its record + 1, or 0 when untagged (satoru)
uint32_t tag_take(uint64_t k) {
    uint32_t h = hash_tag(k);
    uint64_t pos = 0; bool found = false;
    for (uint32_t i = 0; i <= g_tag_dist; i++) {
        uint64_t p = (h + i) & g_tag_mask;
        uint64_t s = g_tag[p];
        if (s == 0) return 0;
        if ((s >> 16) == k) { pos = p; found = true; break; }
    }
    if (!found) return 0;
    uint32_t rec = (uint32_t)(g_tag[pos] & 0xFFFF);
    // backward-shift deletion: pull later entries of the cluster into the hole
    // when the hole lies between their home slot and where they sit. an entry
    // only ever moves toward its home, so g_tag_dist stays a valid bound. (satoru)
    uint64_t i = pos, j = pos;
    for (;;) {
        j = (j + 1) & g_tag_mask;
        uint64_t s = g_tag[j];
        if (s == 0) break;
        uint64_t home = hash_tag(s >> 16);
        bool stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
        if (stays) continue;
        g_tag[i] = s;
        i = j;
    }
    g_tag[i] = 0;
    g_tag_live--;
    return rec + 1;
}

// ---- locked bodies for LockedCall (satoru)
struct SnapArgs { Row* out; int max; int n; Totals* t; };
void snap_body(void* a) {
    SnapArgs* s = (SnapArgs*)a;
    for (uint32_t i = 1; i < g_rec_used && s->n < s->max; i++) {
        const Rec& r = g_rec[i];
        if (!r.key || (r.heap_count == 0 && r.frame_bytes == 0)) continue;
        Row& o = s->out[s->n++];
        o.key = r.key; o.pid = r.pid;
        for (int c = 0; c < 24; c++) o.name[c] = r.name[c];
        o.heap_bytes = r.heap_bytes; o.heap_count = r.heap_count;
        o.frame_bytes = r.frame_bytes; o.peak_bytes = r.peak_bytes;
    }
    if (s->t) {
        Totals& t = *s->t;
        t.heap_live = g_heap_live; t.kernel_bytes = g_kernel_bytes; t.kernel_frames = g_kernel_frames;
        uint32_t live = 0;
        for (uint32_t i = 1; i < g_rec_used; i++) if (g_rec[i].key) live++;
        t.records = live; t.tags = (uint32_t)g_tag_live; t.tag_cap = (uint32_t)g_tag_cap;
        t.tag_max_dist = g_tag_dist; t.overflow = g_overflow; t.stale = g_stale; t.sweeps = g_sweeps;
    }
}

struct FrameArgs { uint64_t block; uint64_t bytes; uint32_t cpu; bool charge; };
void frame_body(void* a) {
    FrameArgs* f = (FrameArgs*)a;
    if (f->charge) {
        uint32_t rec = resolve(f->cpu);
        for (int i = 0; i < FR_CAP; i++) {
            if (g_fr[i].block) continue;
            g_fr[i].block = f->block; g_fr[i].bytes = f->bytes; g_fr[i].rec = rec;
            if (rec) g_rec[rec].frame_bytes += f->bytes; else g_kernel_frames += f->bytes;
            return;
        }
        return;   // table full: this block is not tracked at all (satoru)
    }
    for (int i = 0; i < FR_CAP; i++) {
        if (g_fr[i].block != f->block) continue;
        uint32_t rec = g_fr[i].rec;
        if (rec) { Rec& r = g_rec[rec]; r.frame_bytes = r.frame_bytes >= g_fr[i].bytes ? r.frame_bytes - g_fr[i].bytes : 0; }
        else g_kernel_frames = g_kernel_frames >= g_fr[i].bytes ? g_kernel_frames - g_fr[i].bytes : 0;
        g_fr[i].block = 0; g_fr[i].bytes = 0; g_fr[i].rec = 0;
        return;
    }
}

struct EnableArgs { bool ok; };
void enable_body(void* a) {
    // baseline: everything live now is kernel's; from here on every alloc and
    // free goes through the hooks, so the counters stay exact. (satoru)
    uint64_t used = KernelHeap::GetUsed();   // the heap guard nests on this cpu (satoru)
    g_heap_live = used; g_kernel_bytes = used;
    g_on = true;
    ((EnableArgs*)a)->ok = true;
}

uint32_t cur_cpu() {
    uint32_t c = SMP::CpuIndex();
    return c < CPU_CAP ? c : 0;
}

uint32_t cur_pid() {
    Process* p = Scheduler::GetCurrentProcess();
    return p ? p->pid : 0;
}

void log_u(uint64_t v) {
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int n = 0;
    while (k) o[n++] = t[--k];
    o[n] = 0;
    SerialLogger::Log(o);
}

uint32_t g_log_s = 0;
[[noreturn]] void periodic_entry() {
    for (;;) {
        Scheduler::SleepMs(g_log_s * 1000);
        LogTop("periodic");
    }
}

}  // namespace (satoru)

void Request() { g_req = true; }
void RequestBench() { g_bench = true; }

namespace {
inline uint64_t rdtsc_now() { uint32_t lo, hi; __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

// kurono.taskmem.bench=1: alloc/free cost with accounting off vs on, at boot
// (one cpu, no scheduler). two patterns: a tight alloc+free pair, and 4096
// live blocks allocated then freed in a scrambled order (the tag table holds
// them all). sizes cycle 24..4 kb. cycles per alloc+free, best of 5. (satoru)
void bench_run(const char* tag) {
    static void* live[4096];
    static const uint32_t sizes[8] = {24, 48, 100, 256, 512, 1000, 2048, 4096};
    uint64_t best_pair = ~0ull, best_batch = ~0ull;
    for (int rep = 0; rep < 5; rep++) {
        uint64_t t0 = rdtsc_now();
        for (int i = 0; i < 20000; i++) { void* p = KernelHeap::Alloc(sizes[i & 7]); KernelHeap::Free(p); }
        uint64_t t1 = rdtsc_now();
        for (int i = 0; i < 4096; i++) live[i] = KernelHeap::Alloc(sizes[(i * 5) & 7]);
        for (int i = 0; i < 4096; i++) KernelHeap::Free(live[(i * 2654435761u) & 4095]);
        uint64_t t2 = rdtsc_now();
        if (t1 - t0 < best_pair) best_pair = t1 - t0;
        if (t2 - t1 < best_batch) best_batch = t2 - t1;
    }
    SerialLogger::Log("[taskmem] bench ");
    SerialLogger::Log(tag);
    SerialLogger::Log(" pair_cyc=");
    log_u(best_pair / 20000);
    SerialLogger::Log(" batch_cyc=");
    log_u(best_batch / 4096);
    SerialLogger::Log("\r\n");
}
}  // namespace (satoru)

void Init() {
    if (!g_req || g_on) return;
    uint64_t ram = PMM::GetTotalMemory();
    uint64_t heap = KernelHeap::GetTotal();
    // start at ~1 tag slot per 2 kb of heap arena (128 k slots minimum, 1 mb),
    // grow by doubling when half full, up to ram / 64 of table. (satoru)
    uint64_t cap = 1ull << 17;
    while (cap < heap / 2048 && cap < (1ull << 22)) cap <<= 1;
    // cap the table at ram / 256 and 8 m slots (64 mb): a rehash runs under the
    // heap lock with interrupts off, so it must stay a few ms at most (satoru)
    g_tag_max_cap = 1ull << 17;
    while (g_tag_max_cap * 8 * 2 <= ram / 256 && g_tag_max_cap < (1ull << 23)) g_tag_max_cap <<= 1;
    if (cap > g_tag_max_cap) cap = g_tag_max_cap;
    g_rec  = (Rec*)PMM::AllocBytes(sizeof(Rec) * REC_CAP);
    g_keys = (uint64_t*)PMM::AllocBytes(sizeof(uint64_t) * KEY_CAP);
    g_tag  = (uint64_t*)PMM::AllocBytes((size_t)(cap * 8));
    if (!g_rec || !g_keys || !g_tag) {
        if (g_rec)  PMM::FreeBytes(g_rec, sizeof(Rec) * REC_CAP);
        if (g_keys) PMM::FreeBytes(g_keys, sizeof(uint64_t) * KEY_CAP);
        if (g_tag)  PMM::FreeBytes(g_tag, (size_t)(cap * 8));
        g_rec = nullptr; g_keys = nullptr; g_tag = nullptr;
        SerialLogger::Log("[taskmem] no memory for the tables, off\r\n");
        return;
    }
    for (uint32_t i = 0; i < REC_CAP; i++) {
        g_rec[i].key = 0; g_rec[i].pid = 0; g_rec[i].name[0] = 0;
        g_rec[i].heap_bytes = g_rec[i].heap_count = g_rec[i].frame_bytes = g_rec[i].peak_bytes = 0;
    }
    copy_name(g_rec[0].name, "kernel");
    for (uint32_t i = 0; i < KEY_CAP; i++) g_keys[i] = 0;
    for (uint64_t i = 0; i < cap; i++) g_tag[i] = 0;
    g_tag_cap = cap; g_tag_mask = cap - 1;
    uint32_t bits = 0; while ((1ull << bits) < cap) bits++;
    g_tag_shift = 64 - bits;
    for (uint32_t c = 0; c < CPU_CAP; c++) {
        g_cache_key[c] = 0; g_cache_rec[c] = 0; g_cache_gen[c] = 0;
        g_irq_sp[c] = 0; g_irq_proc[c] = nullptr; g_scope_key[c] = 0; g_scope_pid[c] = 0;
    }
    for (int i = 0; i < WIN_CAP; i++) g_win[i] = 0;
    if (g_bench) bench_run("off");
    EnableArgs ea{false};
    KernelHeap::LockedCall(enable_body, &ea);
    if (g_bench) {
        g_bench_rec = rec_new(0x7FFFFFFFu, 0, "bench");
        bench_run("on");
        SerialLogger::Log("[taskmem] bench rec heap_bytes=");
        log_u(g_rec[g_bench_rec].heap_bytes);
        SerialLogger::Log(" blocks=");
        log_u(g_rec[g_bench_rec].heap_count);
        SerialLogger::Log(" tags=");
        log_u(g_tag_live);
        SerialLogger::Log("\r\n");
        g_bench_rec = 0;
    }
    SerialLogger::Log("[taskmem] on: tag slots=");
    log_u(g_tag_cap);
    SerialLogger::Log(" max=");
    log_u(g_tag_max_cap);
    SerialLogger::Log(" baseline_kb=");
    log_u(g_kernel_bytes >> 10);
    SerialLogger::Log("\r\n");
}

void OnHeapAlloc(void* payload, uint64_t bytes, uint32_t cpu) {
    g_heap_live += bytes;
    uint32_t rec = resolve(cpu);
    if (rec) {
        if (g_tag_live * 2 > g_tag_cap && g_tag_cap < g_tag_max_cap && g_tag_live >= g_grow_retry_at) tag_grow();
        uint64_t k = (uint64_t)(uintptr_t)payload >> 4;
        int ins = (g_tag_live * 4 < g_tag_cap * 3) ? tag_insert_raw(g_tag, g_tag_mask, k, rec, PROBE_MAX, &g_tag_dist) : 0;
        if (ins) {
            if (ins == 1) g_tag_live++;
            Rec& r = g_rec[rec];
            r.heap_bytes += bytes; r.heap_count++;
            if (r.heap_bytes > r.peak_bytes) r.peak_bytes = r.heap_bytes;
            return;
        }
        g_overflow++;
    }
    g_kernel_bytes += bytes;
}

void OnHeapFree(void* payload, uint64_t bytes) {
    g_heap_live = g_heap_live >= bytes ? g_heap_live - bytes : 0;
    uint32_t t = tag_take((uint64_t)(uintptr_t)payload >> 4);
    if (t) {
        Rec& r = g_rec[t - 1];
        r.heap_bytes = r.heap_bytes >= bytes ? r.heap_bytes - bytes : 0;
        if (r.heap_count) r.heap_count--;
        return;
    }
    g_kernel_bytes = g_kernel_bytes >= bytes ? g_kernel_bytes - bytes : 0;
}

uint64_t IrqEnter(const void* frame, uint32_t* cpu_out) {
    uint32_t c = cur_cpu();
    *cpu_out = c;
    uint64_t prev = g_irq_sp[c];
    if (!prev) {   // nested: keep the outer (higher) frame (satoru)
        g_irq_proc[c] = current_on(c);
        g_irq_sp[c] = (uint64_t)(uintptr_t)frame;
    }
    return prev;
}
void IrqLeave(uint32_t cpu, uint64_t prev) {
    // the cpu the mark went up on, even if the handler resumed elsewhere (satoru)
    if (cpu < CPU_CAP) g_irq_sp[cpu] = prev;
}

void ChargeFrames(const void* block, uint64_t bytes) {
    if (!g_on || !block || !bytes) return;
    FrameArgs f{(uint64_t)(uintptr_t)block, bytes, cur_cpu(), true};
    KernelHeap::LockedCall(frame_body, &f);
}
void UnchargeFrames(const void* block) {
    if (!g_on || !block) return;
    FrameArgs f{(uint64_t)(uintptr_t)block, 0, 0, false};
    KernelHeap::LockedCall(frame_body, &f);
}

namespace {
// set a per-cpu charge key for the calling task; returns false when off (satoru)
bool scope_push(uint32_t key, uint32_t* cpu, uint32_t* prev_key, uint32_t* prev_pid) {
    uint32_t pid = cur_pid();
    if (!pid) return false;
    *cpu = cur_cpu();
    *prev_key = g_scope_key[*cpu]; *prev_pid = g_scope_pid[*cpu];
    g_scope_pid[*cpu] = pid;
    g_scope_key[*cpu] = key;
    return true;
}
}  // namespace (satoru)

AppScope::AppScope(uint32_t app) : cpu(0), prev_app(0), prev_pid(0), armed(false) {
    if (!g_on || app == APP_NONE || app >= APP_COUNT) return;
    armed = scope_push(APP_KEY | app, &cpu, &prev_app, &prev_pid);
}
AppScope::~AppScope() {
    if (!armed) return;
    // restore on the cpu the scope was set on. a task that migrated mid-scope
    // (kernel tasks are pinned, so this is belt and braces) leaves a scope whose
    // pid check fails for everyone else on that cpu. (satoru)
    g_scope_key[cpu] = prev_app;
    g_scope_pid[cpu] = prev_pid;
}

void SetUiTask() {
    Process* p = Scheduler::GetCurrentProcess();
    if (!p) return;
    copy_name(g_ui_name, p->name);
    g_ui_pid = p->pid;
}

UiScope::UiScope() : cpu(0), prev_key(0), prev_pid(0), armed(false) {
    uint32_t ui = g_ui_pid;
    if (!g_on || !ui || cur_pid() == ui) return;
    armed = scope_push(ui, &cpu, &prev_key, &prev_pid);
}
UiScope::~UiScope() {
    if (!armed) return;
    g_scope_key[cpu] = prev_key;
    g_scope_pid[cpu] = prev_pid;
}

WindowScope::WindowScope(int window_id) : s(g_on ? WindowApp(window_id) : APP_NONE) {}

const char* AppName(uint32_t app) { return app < APP_COUNT ? k_app_names[app] : "App"; }

void BindWindow(int window_id) {
    if (!g_on || window_id <= 0) return;
    uint32_t c = cur_cpu();
    uint32_t sk = (g_scope_pid[c] == cur_pid()) ? g_scope_key[c] : 0;
    if (!(sk & APP_KEY)) return;
    uint32_t app = sk & 0xFF;
    uint64_t v = ((uint64_t)(uint32_t)window_id << 8) | app;
    int h = window_id & (WIN_CAP - 1);
    for (int i = 0; i < WIN_CAP; i++) {
        int s = (h + i) & (WIN_CAP - 1);
        if (g_win[s] == 0) { g_win[s] = v; return; }
    }
    g_win[h] = v;   // full of stale ids: take the home slot (satoru)
}
void UnbindWindow(int window_id) {
    if (!g_on || window_id <= 0) return;
    int h = window_id & (WIN_CAP - 1);
    for (int i = 0; i < WIN_CAP; i++) {
        int s = (h + i) & (WIN_CAP - 1);
        if ((g_win[s] >> 8) == (uint64_t)(uint32_t)window_id) { g_win[s] = 0; return; }
    }
}
uint32_t WindowApp(int window_id) {
    if (!g_on || window_id <= 0) return 0;
    int h = window_id & (WIN_CAP - 1);
    for (int i = 0; i < WIN_CAP; i++) {
        uint64_t v = g_win[(h + i) & (WIN_CAP - 1)];
        if ((v >> 8) == (uint64_t)(uint32_t)window_id) return (uint32_t)(v & 0xFF);
    }
    return 0;
}

int Snapshot(Row* out, int max, Totals* t) {
    if (!g_on || !out || max <= 0) return 0;
    SnapArgs s{out, max, 0, t};
    KernelHeap::LockedCall(snap_body, &s);
    return s.n;
}

struct OneArgs { uint32_t pid; Row* out; bool found; };
static void get_one_body(void* a) {
    OneArgs* s = (OneArgs*)a;
    s->found = false;
    for (uint32_t i = 1; i < g_rec_used; i++) {
        const Rec& r = g_rec[i];
        if (!r.key || (r.key & APP_KEY) || r.key != s->pid) continue;
        Row& o = *s->out;
        o.key = r.key; o.pid = r.pid;
        for (int c = 0; c < 24; c++) o.name[c] = r.name[c];
        o.heap_bytes = r.heap_bytes; o.heap_count = r.heap_count;
        o.frame_bytes = r.frame_bytes; o.peak_bytes = r.peak_bytes;
        s->found = true;
        return;
    }
}

bool GetForPid(uint32_t pid, Row* out) {
    if (!g_on || !out || pid == 0) return false;
    OneArgs s{pid, out, false};
    memset(out, 0, sizeof(*out));
    KernelHeap::LockedCall(get_one_body, &s);
    return s.found;
}

void GetTotals(Totals* t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    if (!g_on) return;
    SnapArgs s{nullptr, 0, 0, t};
    KernelHeap::LockedCall(snap_body, &s);
}

void LogTop(const char* why) {
    if (!g_on) return;
    static volatile uint32_t s_busy = 0;   // the report buffers are static: one report at a time (satoru)
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQUIRE)) return;
    static Row rows[1024];
    Totals t{};
    int n = Snapshot(rows, 1024, &t);
    SerialLogger::Log("[taskmem] ");
    SerialLogger::Log(why);
    SerialLogger::Log(" heap_kb="); log_u(t.heap_live >> 10);
    SerialLogger::Log(" kernel_kb="); log_u(t.kernel_bytes >> 10);
    SerialLogger::Log(" recs="); log_u(t.records);
    SerialLogger::Log(" tags="); log_u(t.tags);
    SerialLogger::Log("/"); log_u(t.tag_cap);
    SerialLogger::Log(" dist="); log_u(t.tag_max_dist);
    SerialLogger::Log(" ovf="); log_u(t.overflow);
    SerialLogger::Log(" stale="); log_u(t.stale);
    SerialLogger::Log(" sweeps="); log_u(t.sweeps);
    SerialLogger::Log("\r\n");
    // top 16 by bytes (satoru)
    static bool used[1024];
    for (int i = 0; i < 1024; i++) used[i] = false;
    for (int k = 0; k < 16; k++) {
        int best = -1; uint64_t bv = 0;
        for (int i = 0; i < n; i++) {
            uint64_t v = rows[i].heap_bytes + rows[i].frame_bytes;
            if (!used[i] && (best < 0 || v > bv)) { best = i; bv = v; }
        }
        if (best < 0) break;
        used[best] = true;
        const Row& r = rows[best];
        SerialLogger::Log("[taskmem]   ");
        SerialLogger::Log((r.key & APP_KEY) ? "app " : "pid ");
        log_u((r.key & APP_KEY) ? r.pid : r.key);
        SerialLogger::Log(" ");
        SerialLogger::Log(r.name);
        SerialLogger::Log(" heap_kb="); log_u(r.heap_bytes >> 10);
        SerialLogger::Log(" blocks="); log_u(r.heap_count);
        SerialLogger::Log(" frames_kb="); log_u(r.frame_bytes >> 10);
        SerialLogger::Log(" peak_kb="); log_u(r.peak_bytes >> 10);
        SerialLogger::Log("\r\n");
    }
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
}

void StartPeriodicLog(uint32_t secs) {
    if (!g_on || !secs) return;
    g_log_s = secs;
    Scheduler::SpawnKernelProcess("taskmem", periodic_entry, PRIO_LOW, 64, 256);
}

}  // namespace TaskMem (satoru)

// end (satoru)
