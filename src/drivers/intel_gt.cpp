// intel gt bring-up and 2d blits on xe-lpg without guc firmware. see
// intel_gt.h for the contract and intel_gt_calc.h for the data layouts.
//
// linux sources (v7.1-rc3), cited next to the steps below: (satoru)
//  - xe/xe_force_wake.c: a domain is woken by writing FORCEWAKE_MT_MASK(0) |
//    FORCEWAKE_MT(0) (0x00010001) to its control register and waiting up to
//    50 ms for bit 0 of the ack register; 0x00010000 lets it sleep again.
//    FORCEWAKE_GT 0xa188 / FORCEWAKE_ACK_GT_MTL 0xdfc, FORCEWAKE_RENDER 0xa278
//    / FORCEWAKE_ACK_RENDER 0xd84; the blitter is in the render domain
//    (xe_hw_engine.c engine_infos). (satoru)
//  - xe/xe_gt_mcr.c, i915/gt/intel_gt_mcr.c: pat and mocs are multicast
//    registers. on 12.70 the steering lock is a hardware semaphore at 0xfd0: a
//    read that returns 1 takes it, writing 1 releases it, and
//    intel_gt_mcr_lock_sanitize writes 1 once at load because boot firmware
//    may have left it taken. (satoru)
//  - xe/xe_pat.c xelpg_pat_table, xe/xe_mocs.c mtl_mocs_desc: the values. (satoru)
//  - xe/xe_hw_engine.c xe_hw_engine_enable_ring, xe/xe_execlist.c __start_lrc:
//    HWSTAM, HWS_PGA, RING_MODE (legacy mode off = execlists), MI_MODE, then
//    the descriptor into the submit queue and EL_CTRL_LOAD. (satoru)
//  - xe/xe_lrc.c, xe/xe_ring_ops.c, xe/xe_vm.c, i915/gt/intel_migrate.c: the
//    context image, the job, the ppgtt and the blits (intel_gt_calc.cpp). (satoru)
//  - i915/gt/intel_reset.c gen8_engine_reset_prepare / gen11_reset_engines:
//    the engine reset used only after a job timed out. (satoru)
//  - xe/xe_guc_tlb_inval.c: with the guc not in use linux invalidates the ggtt
//    tlb with GUC_TLB_INV_CR (0xcee8) bit 0. (satoru)
#include "intel_gt.h"
#include "intel_gt_calc.h"
#include "intel_modeset_calc.h"   // MtlGgttPte for the standalone ggtt (satoru)
#include "intel_accel.h"
#include "intel_display_probe.h"
#include "dp_edid_decode.h"
#include "serial.h"
#include "cpu_detect.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../proc/spinlock.h"
#include "../proc/smp.h"
#include "../kernel/kld_auto.h"   // KldAuto::TakesDevice: a device taken for a kld container is left alone (satoru)

namespace {

using DpEdid::Txt;
using namespace IgpuGt;

// ---- registers outside the engine block (satoru) ----
constexpr uint32_t R_GMD_ID = 0xd8c;               // arch 31:22, release 21:14 (satoru)
constexpr uint32_t R_FW_GT = 0xa188, R_FW_GT_ACK = 0xdfc;
constexpr uint32_t R_FW_RENDER = 0xa278, R_FW_RENDER_ACK = 0xd84;
constexpr uint32_t R_STEER_SEMAPHORE = 0xfd0, R_MCR_SELECTOR = 0xfd4;
constexpr uint32_t R_PAT0 = 0x4800, R_MOCS0 = 0x4000;
constexpr uint32_t R_GUC_STATUS = 0xc000;
constexpr uint32_t R_GUC_TLB_INV_CR = 0xcee8;
constexpr uint32_t R_GDRST = 0x941c;
constexpr uint32_t GRDOM_BLT = 1u << 2;            // i915 GEN11_GRDOM_BLT (satoru)
constexpr uint32_t FW_WAKE = 0x00010001u, FW_SLEEP = 0x00010000u;
constexpr uint32_t BASE = BLT_RING_BASE;

// ---- memory layout of the one block the ggtt maps (satoru) ----
constexpr uint32_t PG_HWSP = 0, PG_PPHWSP = 1, PG_STATE = 2, PG_RING = 3, PG_SEQNO = 7;
constexpr uint32_t BLK_PAGES = 8;
constexpr uint32_t RING_BYTES = 4 * 4096;
constexpr uint32_t BATCH_PAGES = 4;
constexpr int BATCH_DWORDS = (int)(BATCH_PAGES * 1024) - 4;
constexpr int MAX_TABLES = 512;
constexpr uint32_t SIM_GGTT = 0x00100000u;         // where the qemu model "maps" the block (satoru)
constexpr uint32_t JOB_TIMEOUT_US = 100000;
constexpr int TEST_W = 256, TEST_H = 64;
constexpr uint32_t TEST_PITCH = TEST_W * 4;
constexpr uint32_t TEST_PAGES = (TEST_PITCH * TEST_H + 4095) / 4096;

// ---- logging (satoru) ----
char* g_out = nullptr;
int g_out_max = 0, g_out_pos = 0;
void begin_out(char* out, int maxo) {
    g_out = out; g_out_max = maxo; g_out_pos = 0;
    if (out && maxo > 0) out[0] = 0;
}
void emit(const char* line) {
    SerialLogger::Log("[igpu-gt] ");
    SerialLogger::Log(line);
    SerialLogger::Log("\r\n");
    if (g_out && g_out_pos < g_out_max - 2) {
        static const char pre[] = "[igpu-gt] ";
        for (int i = 0; pre[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = pre[i];
        for (int i = 0; line[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = line[i];
        g_out[g_out_pos++] = '\n';
        g_out[g_out_pos] = 0;
    }
}
struct Ln {
    char b[256];
    Txt t;
    Ln() : t(b, sizeof(b)) {}
    void go() { emit(b); }
};

// ---- clock: differences of rdtsc on one cpu only (see intel_accel.cpp) (satoru) ----
uint64_t g_tpu = 0;
inline uint64_t tsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
void clock_init() {
    if (g_tpu) return;
    CpuInfo ci = CPUDetect::GetInfo();
    uint64_t f = ci.frequency.tsc_frequency;
    if (!f && ci.frequency.base_mhz > 0) f = (uint64_t)ci.frequency.base_mhz * 1000000ull;
    uint64_t t = f / 1000000ull;
    g_tpu = (t >= 100 && t <= 10000) ? t : 3000;
}
void udelay(uint32_t us) {
    uint64_t end = tsc() + (uint64_t)us * g_tpu;
    for (uint64_t guard = (uint64_t)us * 4096 + 64; guard && tsc() < end; guard--) __asm__ __volatile__("pause");
}
inline uint64_t us_since(uint64_t t0) { return (tsc() - t0) / g_tpu; }

inline void clflush_range(const void* p, uint64_t bytes) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + bytes;
    for (; a < e; a += 64) __asm__ __volatile__("clflush (%0)" : : "r"((const void*)a) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

// ---- mmio with a write allowlist (satoru) ----
volatile uint8_t* g_mmio = nullptr;
int g_blocked = 0;
bool g_log_writes = true;
inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }
bool allowed(uint32_t off) {
    if (off == R_FW_GT || off == R_FW_RENDER || off == R_STEER_SEMAPHORE || off == R_MCR_SELECTOR) return true;
    if (off >= R_PAT0 && off < R_PAT0 + 4u * PAT_ENTRIES) return true;
    if (off >= R_MOCS0 && off < R_MOCS0 + 4u * MOCS_ENTRIES) return true;
    if (off == R_GUC_TLB_INV_CR || off == R_GDRST) return true;
    if (off == BASE + RING_HWS_PGA || off == BASE + RING_HWSTAM || off == BASE + RING_MI_MODE ||
        off == BASE + RING_MODE || off == BASE + RING_RESET_CTL || off == BASE + RING_EXECLIST_SQ_LO ||
        off == BASE + RING_EXECLIST_SQ_HI || off == BASE + RING_EXECLIST_CONTROL) return true;
    return false;
}
bool wr(uint32_t off, uint32_t v) {
    if (!allowed(off)) {
        g_blocked++;
        Ln l; l.t.s("BLOCKED write 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8); l.t.s(" (not on the gt allowlist)"); l.go();
        return false;
    }
    *(volatile uint32_t*)(g_mmio + off) = v;
    __asm__ __volatile__("" ::: "memory");
    if (g_log_writes) {
        Ln l; l.t.s("wr 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8); l.go();
    }
    return true;
}
// ---- standalone gt access: no display probe needed (satoru) ----
// the gt does not depend on the display. in a vm with the igpu passed through
// (vfio) no firmware ran, every display power well is off and the display
// probe stops, but the gt answers. then bar0 is mapped here on its own and the
// ggtt is written through the gttmmadr half of bar0: bar0 + 8 mb holds one 64
// bit pte per 4 kb ggtt page (i915 gen6_gttadr_offset, xe_ggtt gsm). the
// stolen memory base (BDSM) is never read or written on this path: in a vm it
// is the host's address. on the laptop, where the probe passes, nothing here
// runs and the ggtt keeps going through IntelAccel::GgttReserve. (satoru)
constexpr uint64_t SA_BAR_BYTES = 16ull << 20;
constexpr uint64_t SA_GTT_OFF = 8ull << 20;
constexpr uint32_t SA_GTT_ENTRIES = (uint32_t)((SA_BAR_BYTES - SA_GTT_OFF) / 8u);   // 4 gb of ggtt (satoru)
bool g_sa = false;
uint64_t g_sa_bar0 = 0;
volatile uint64_t* g_sa_gtt = nullptr;
uint8_t g_sa_dev = 0, g_sa_fn = 0;

uint32_t sa_cfg_rd(uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
    __asm__ __volatile__("outl %0, %w1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    if (fl & 0x200) __asm__ __volatile__("sti" : : : "memory");
    return v;
}
void sa_cfg_wr16(uint8_t dev, uint8_t fn, uint8_t off, uint16_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
    __asm__ __volatile__("outl %0, %w1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("outw %0, %w1" : : "a"(val), "Nd"((uint16_t)(0xCFC + (off & 2))));
    if (fl & 0x200) __asm__ __volatile__("sti" : : : "memory");
}

bool sa_map() {
    int fdev = -1, ffn = -1;
    for (int dev = 0; dev < 32 && fdev < 0; dev++) {
        uint32_t id0 = sa_cfg_rd((uint8_t)dev, 0, 0x00);
        if ((id0 & 0xFFFF) == 0xFFFF) continue;
        bool multi = (sa_cfg_rd((uint8_t)dev, 0, 0x0C) >> 16) & 0x80;
        for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
            uint32_t id = sa_cfg_rd((uint8_t)dev, (uint8_t)fn, 0x00);
            if ((id & 0xFFFF) != 0x8086) continue;
            if (KldAuto::TakesDevice(0, (uint8_t)dev, (uint8_t)fn, 0x8086, (uint16_t)(id >> 16))) continue;   // kld owns it (satoru)
            if ((sa_cfg_rd((uint8_t)dev, (uint8_t)fn, 0x08) >> 24) == 0x03) { fdev = dev; ffn = fn; break; }
        }
    }
    if (fdev < 0) { emit("standalone: no intel display-class device on bus 0"); return false; }
    g_sa_dev = (uint8_t)fdev; g_sa_fn = (uint8_t)ffn;
    uint32_t b0lo = sa_cfg_rd(g_sa_dev, g_sa_fn, 0x10), b0hi = sa_cfg_rd(g_sa_dev, g_sa_fn, 0x14);
    uint64_t bar0 = (uint64_t)(b0lo & ~0xFu) | (((b0lo >> 1) & 3) == 2 ? ((uint64_t)b0hi << 32) : 0);
    uint32_t cmd = sa_cfg_rd(g_sa_dev, g_sa_fn, 0x04) & 0xFFFF;
    if (!bar0) { emit("standalone: bar0 is unassigned"); return false; }
    // the engine fetches its context, ring and batches by dma: bus mastering
    // has to be on. the gop turns it on; after a vfio reset it is off. (satoru)
    if ((cmd & 6) != 6) {
        sa_cfg_wr16(g_sa_dev, g_sa_fn, 0x04, (uint16_t)(cmd | 6));
        uint32_t now = sa_cfg_rd(g_sa_dev, g_sa_fn, 0x04) & 0xFFFF;
        Ln l; l.t.s("standalone: pci command 0x"); l.t.x(cmd, 4); l.t.s(" -> 0x"); l.t.x(now, 4);
        l.t.s(" (memory decode and bus master on)"); l.go();
        if ((now & 6) != 6) { emit("FAIL standalone: bus master did not stick"); return false; }
    }
    for (uint64_t p = bar0; p < bar0 + SA_BAR_BYTES; p += 0x1000) {
        if (!KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) {
            emit("FAIL standalone: mapping bar0 failed");
            return false;
        }
    }
    g_sa_bar0 = bar0;
    g_mmio = (volatile uint8_t*)(uintptr_t)bar0;
    g_sa_gtt = (volatile uint64_t*)(uintptr_t)(bar0 + SA_GTT_OFF);
    g_sa = true;
    Ln l; l.t.s("standalone: gt at pci 00:"); l.t.d((uint64_t)fdev); l.t.c('.'); l.t.d((uint64_t)ffn);
    l.t.s(" bar0 0x"); l.t.x((uint32_t)(bar0 >> 32), 8); l.t.x((uint32_t)bar0, 8);
    l.t.s(" mapped 16 mb, ggtt ptes through bar0 + 8 mb (no display probe, no stolen memory access)"); l.go();
    return true;
}

// a ggtt window for npte pages: highest first below 3 gb, every entry it
// covers must be invalid, old values saved, new ones read back. (satoru)
bool sa_ggtt_reserve(uint64_t phys, uint32_t npte, int pat, uint32_t* ggtt_out, uint64_t* saved) {
    if (!g_sa_gtt || !npte) return false;
    for (uint32_t cand = 0xBF000000u; cand >= 0x01000000u; cand -= 0x00100000u) {
        uint32_t idx = cand >> 12;
        if (idx + npte > SA_GTT_ENTRIES) continue;
        bool busy = false;
        for (uint32_t i = 0; i < npte && !busy; i++) busy = (g_sa_gtt[idx + i] & 1) != 0;
        if (busy) continue;
        for (uint32_t i = 0; i < npte; i++) saved[i] = g_sa_gtt[idx + i];
        for (uint32_t i = 0; i < npte; i++) g_sa_gtt[idx + i] = IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096u, pat);
        __asm__ __volatile__("mfence" ::: "memory");
        bool ok = true;
        for (uint32_t i = 0; i < npte && ok; i++)
            ok = g_sa_gtt[idx + i] == IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096u, pat);
        if (!ok) {
            for (uint32_t i = 0; i < npte; i++) g_sa_gtt[idx + i] = saved[i];
            __asm__ __volatile__("mfence" ::: "memory");
            Ln l; l.t.s("FAIL standalone ggtt: pte read back mismatch at 0x"); l.t.x(cand, 8); l.go();
            return false;
        }
        *ggtt_out = cand;
        Ln l; l.t.s("standalone ggtt: "); l.t.d((uint64_t)npte); l.t.s(" entries at 0x"); l.t.x(cand, 8);
        l.t.s(" -> phys 0x"); l.t.x((uint32_t)(phys >> 32), 8); l.t.x((uint32_t)phys, 8);
        l.t.s(" pat "); l.t.d((uint64_t)pat); l.t.s(", written and read back through bar0"); l.go();
        return true;
    }
    emit("FAIL standalone ggtt: no free window");
    return false;
}

void sa_ggtt_release(uint32_t ggtt, uint32_t npte, const uint64_t* saved) {
    if (!g_sa_gtt) return;
    uint32_t idx = ggtt >> 12;
    for (uint32_t i = 0; i < npte; i++) g_sa_gtt[idx + i] = saved[i];
    __asm__ __volatile__("mfence" ::: "memory");
}

bool wait_bits(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t* last) {
    uint64_t t0 = tsc();
    uint32_t v = rd(off);
    for (uint32_t guard = timeout_us + 16; (v & mask) != want && guard; guard--) {
        if (us_since(t0) > timeout_us) break;
        udelay(1);
        v = rd(off);
    }
    if (last) *last = v;
    return (v & mask) == want;
}

// ---- state (satoru) ----
struct Gt {
    bool sim;
    bool fw_gt, fw_render;
    bool pat_ok;
    bool up, blit_ok, dead;
    bool coherent_read, coherent_write;
    uint64_t blk;                 // 8 contiguous pages: hwsp, pphwsp, state, ring x4, seqno (satoru)
    uint32_t ggtt;                // ggtt address of that block (satoru)
    bool ggtt_mapped;
    uint64_t saved_pte[BLK_PAGES];
    uint64_t batch;               // batch pages, ppgtt only (satoru)
    uint64_t test_a, test_b;      // self-test surfaces (satoru)
    Ppgtt pg;
    uint64_t tables[MAX_TABLES];
    int ntables;
    bool tables_dirty;            // a table changed since the last flush (satoru)
    uint32_t tail;                // ring tail in bytes (satoru)
    uint32_t seq;                 // last sequence number handed out (satoru)
    uint32_t inflight;            // 0 = idle (satoru)
    uint64_t inflight_t0;
    // the engine saves the context image after the job's last command. the
    // job only counts as finished when that save is in ram (saved head ==
    // tail): the next submit rewrites the tail in the same cache line as the
    // saved head, and writing a line the engine is about to write could put
    // the old head back. bounded: after 500 us on one cpu, or 4 looks from
    // changing cpus, the job is taken as finished anyway. (satoru)
    bool seen;
    uint64_t seen_tsc;
    uint32_t seen_cpu, seen_moves;
    uint32_t save_us_max, save_misses;
    int blen;
    bool bopen;
    uint64_t bbytes;
    uint32_t st_jobs, st_fails;
    uint64_t st_bytes, st_us;
    uint32_t total_jobs;
    uint32_t last_job_us;
};
Gt g = {};
Spinlock g_gt_lock;

inline uint32_t* blk_dw(uint32_t page) { return (uint32_t*)(uintptr_t)(g.blk + (uint64_t)page * 4096u); }

// ---- ppgtt callbacks and the model's view of memory (satoru) ----
uint64_t pg_alloc(void*) {
    if (g.ntables >= MAX_TABLES) return 0;
    uint64_t p = PMM::AllocContiguous(1);
    if (!p) return 0;
    __builtin_memset((void*)(uintptr_t)p, 0, 4096);
    g.tables[g.ntables++] = p;
    g.tables_dirty = true;
    return p;
}
uint64_t* pg_table(void*, uint64_t phys) { return (uint64_t*)(uintptr_t)phys; }
uint8_t* sim_ggtt_page(void*, uint32_t a) {
    if (a < SIM_GGTT || a >= SIM_GGTT + BLK_PAGES * 4096u) return nullptr;
    return (uint8_t*)(uintptr_t)(g.blk + (a - SIM_GGTT));
}
uint8_t* sim_phys_page(void*, uint64_t phys) {
    if (!phys || phys + 4096 > PMM::GetPhysTop()) return nullptr;
    return (uint8_t*)(uintptr_t)phys;
}

void flush_tables() {
    // the engine walks these from ram; nothing may stay only in a cpu cache (satoru)
    if (!g.tables_dirty) return;
    for (int i = 0; i < g.ntables; i++) clflush_range((void*)(uintptr_t)g.tables[i], 4096);
    if (g.pg.pml4) clflush_range((void*)(uintptr_t)g.pg.pml4, 4096);
    g.tables_dirty = false;
}

// ---- forcewake (xe_force_wake.c) (satoru) ----
bool fw_get(uint32_t ctl, uint32_t ack, const char* name) {
    uint32_t before_ctl = rd(ctl), before_ack = rd(ack), last = 0;
    wr(ctl, FW_WAKE);
    bool ok = wait_bits(ack, 1, 1, 50000, &last);
    Ln l; l.t.s(ok ? "PASS forcewake " : "FAIL forcewake "); l.t.s(name); l.t.s(": ctl 0x"); l.t.x(before_ctl, 8);
    l.t.s(" ack 0x"); l.t.x(before_ack, 8); l.t.s(" -> ack 0x"); l.t.x(last, 8);
    if (last == 0xFFFFFFFFu) { l.t.s(" (mmio reads all ones)"); ok = false; }
    l.go();
    return ok;
}
void fw_put(uint32_t ctl) { wr(ctl, FW_SLEEP); }

// ---- pat and mocs, multicast under the steering semaphore (satoru) ----
bool program_pat_mocs() {
    // intel_gt_mcr_lock_sanitize: boot firmware may have left it taken (satoru)
    wr(R_STEER_SEMAPHORE, 1);
    uint32_t last = 0;
    if (!wait_bits(R_STEER_SEMAPHORE, 1, 1, 10000, &last)) {   // a read of 1 takes the lock (satoru)
        Ln l; l.t.s("FAIL pat/mocs: steering semaphore not granted (0x"); l.t.x(last, 8);
        l.t.s("), the firmware defaults stay"); l.go();
        return false;
    }
    uint32_t sel = rd(R_MCR_SELECTOR);
    if (!(sel & (1u << 31))) wr(R_MCR_SELECTOR, 1u << 31);     // multicast, as linux leaves it (satoru)
    uint32_t pat_before[PAT_ENTRIES], mocs_before = rd(R_MOCS0 + 4u * MOCS_UC_INDEX);
    for (int i = 0; i < PAT_ENTRIES; i++) pat_before[i] = rd(R_PAT0 + 4u * (uint32_t)i);
    g_log_writes = false;
    for (int i = 0; i < PAT_ENTRIES; i++) wr(R_PAT0 + 4u * (uint32_t)i, PAT_VALUES[i]);
    for (int i = 0; i < MOCS_ENTRIES; i++) wr(R_MOCS0 + 4u * (uint32_t)i, MocsControl(i));
    g_log_writes = true;
    wr(R_STEER_SEMAPHORE, 1);                                  // release (satoru)
    bool ok = true;
    for (int i = 0; i < PAT_ENTRIES; i++) if (rd(R_PAT0 + 4u * (uint32_t)i) != PAT_VALUES[i]) ok = false;
    for (int i = 0; i < MOCS_ENTRIES; i++) if (rd(R_MOCS0 + 4u * (uint32_t)i) != MocsControl(i)) ok = false;
    Ln l; l.t.s(ok ? "PASS pat/mocs: " : "FAIL pat/mocs readback: ");
    l.t.s("selector 0x"); l.t.x(sel, 8); l.t.s(" pat was");
    for (int i = 0; i < PAT_ENTRIES; i++) { l.t.s(" 0x"); l.t.x(pat_before[i], 2); }
    l.t.s(" now");
    for (int i = 0; i < PAT_ENTRIES; i++) { l.t.s(" 0x"); l.t.x(rd(R_PAT0 + 4u * (uint32_t)i), 2); }
    l.t.s(" | mocs["); l.t.d((uint64_t)MOCS_UC_INDEX); l.t.s("] 0x"); l.t.x(mocs_before, 4);
    l.t.s(" -> 0x"); l.t.x(rd(R_MOCS0 + 4u * MOCS_UC_INDEX), 4); l.go();
    return ok;
}

void dump_engine(const char* when) {
    if (g.sim || !g_mmio) return;
    {
        Ln l; l.t.s("engine ("); l.t.s(when); l.t.s("): mode 0x"); l.t.x(rd(BASE + RING_MODE), 8);
        l.t.s(" mi_mode 0x"); l.t.x(rd(BASE + RING_MI_MODE), 8);
        l.t.s(" execlist_status 0x"); l.t.x(rd(BASE + RING_EXECLIST_STATUS_HI), 8); l.t.c(':'); l.t.x(rd(BASE + RING_EXECLIST_STATUS_LO), 8);
        l.t.s(" hws 0x"); l.t.x(rd(BASE + RING_HWS_PGA), 8);
        l.t.s(" id 0x"); l.t.x(rd(BASE + ENGINE_ID), 8);
        l.t.s(" gdrst 0x"); l.t.x(rd(R_GDRST), 8);
        l.t.s(" reset_ctl 0x"); l.t.x(rd(BASE + RING_RESET_CTL), 8); l.go();
    }
    {
        Ln l; l.t.s("engine ("); l.t.s(when); l.t.s("): start 0x"); l.t.x(rd(BASE + RING_START), 8);
        l.t.s(" head 0x"); l.t.x(rd(BASE + RING_HEAD), 8); l.t.s(" tail 0x"); l.t.x(rd(BASE + RING_TAIL), 8);
        l.t.s(" ctl 0x"); l.t.x(rd(BASE + RING_CTL), 8); l.t.s(" acthd 0x"); l.t.x(rd(BASE + RING_ACTHD), 8);
        l.t.s(" ipehr 0x"); l.t.x(rd(BASE + RING_IPEHR), 8); l.t.s(" instdone 0x"); l.t.x(rd(BASE + RING_INSTDONE), 8);
        l.t.s(" eir 0x"); l.t.x(rd(BASE + RING_EIR), 8); l.t.s(" esr 0x"); l.t.x(rd(BASE + RING_ESR), 8);
        l.t.s(" ctx_ctl 0x"); l.t.x(rd(BASE + RING_CONTEXT_CONTROL), 8); l.go();
    }
}

// ---- memory (satoru) ----
void release_memory() {
    if (g.ggtt_mapped && !g.sim) {
        if (g_sa) sa_ggtt_release(g.ggtt, BLK_PAGES, g.saved_pte);
        else IntelAccel::GgttRelease(g.ggtt, BLK_PAGES, g.saved_pte);
        g.ggtt_mapped = false;
    }
    for (int i = 0; i < g.ntables; i++) PMM::FreeContiguous(g.tables[i], 1);
    g.ntables = 0;
    if (g.pg.pml4) PMM::FreeContiguous(g.pg.pml4, 1);
    g.pg.pml4 = 0;
    if (g.blk) PMM::FreeContiguous(g.blk, BLK_PAGES);
    if (g.batch) PMM::FreeContiguous(g.batch, BATCH_PAGES);
    if (g.test_a) PMM::FreeContiguous(g.test_a, TEST_PAGES);
    if (g.test_b) PMM::FreeContiguous(g.test_b, TEST_PAGES);
    g.blk = g.batch = g.test_a = g.test_b = 0;
}

bool setup_memory() {
    g.blk = PMM::AllocContiguous(BLK_PAGES);
    g.batch = PMM::AllocContiguous(BATCH_PAGES);
    g.pg = Ppgtt{};
    g.pg.pml4 = PMM::AllocContiguous(1);
    uint64_t top = PMM::GetPhysTop();
    if (!g.blk || !g.batch || !g.pg.pml4 || g.blk + BLK_PAGES * 4096ull > top || g.batch + BATCH_PAGES * 4096ull > top) {
        emit("FAIL memory: no pages for the context, ring and batch");
        return false;
    }
    __builtin_memset((void*)(uintptr_t)g.blk, 0, BLK_PAGES * 4096u);
    __builtin_memset((void*)(uintptr_t)g.batch, 0, BATCH_PAGES * 4096u);
    __builtin_memset((void*)(uintptr_t)g.pg.pml4, 0, 4096);
    g.pg.alloc_table = pg_alloc;
    g.pg.table = pg_table;
    g.ntables = 0;
    if (g.sim) {
        g.ggtt = SIM_GGTT;
    } else {
        // write-back, one way coherent, like every kernel object linux puts in
        // the ggtt on this part (xe_ggtt.c, pat index of XE_CACHE_WB) (satoru)
        bool got = g_sa ? sa_ggtt_reserve(g.blk, BLK_PAGES, PAT_WB_1WAY, &g.ggtt, g.saved_pte)
                        : IntelAccel::GgttReserve(g.blk, BLK_PAGES, PAT_WB_1WAY, &g.ggtt, g.saved_pte);
        if (!got) {
            emit("FAIL ggtt: no reserved window for the context block (needs the kurono.igpu.accel=1 probe)");
            return false;
        }
        g.ggtt_mapped = true;
    }
    if (!PpgttMap(g.pg, g.batch, g.batch, BATCH_PAGES, PAT_WB_1WAY)) {
        emit("FAIL ppgtt: no table pages for the batch mapping");
        return false;
    }
    LrcParams lp = { BASE, g.ggtt + PG_RING * 4096u, RING_BYTES, g.pg.pml4 };
    LrcBuild(blk_dw(PG_STATE), lp);
    // xe_lrc_ctx_init: the ring starts with arbitration on (satoru)
    uint32_t* ring = blk_dw(PG_RING);
    ring[0] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    ring[1] = MI_NOOP;
    g.tail = 8;
    blk_dw(PG_STATE)[CTX_RING_TAIL] = 0;       // set by the first submit (satoru)
    clflush_range((void*)(uintptr_t)g.blk, BLK_PAGES * 4096u);
    g.tables_dirty = true;
    flush_tables();
    Ln l; l.t.s("context: block phys 0x"); l.t.x((uint32_t)(g.blk >> 32), 8); l.t.x((uint32_t)g.blk, 8);
    l.t.s(" ggtt 0x"); l.t.x(g.ggtt, 8); l.t.s(" (hwsp +0, lrc +0x1000, ring +0x3000 16k, seqno +0x7000), ppgtt root 0x");
    l.t.x((uint32_t)(g.pg.pml4 >> 32), 8); l.t.x((uint32_t)g.pg.pml4, 8);
    l.t.s(", batch 0x"); l.t.x((uint32_t)(g.batch >> 32), 8); l.t.x((uint32_t)g.batch, 8); l.go();
    return true;
}

// ---- failure: stop the engine, reset it, give everything back (satoru) ----
void gt_dead(const char* why) {
    if (g.dead) return;
    g.dead = true;
    g.up = g.blit_ok = false;
    g.inflight = 0;
    g.st_fails++;
    {
        Ln l; l.t.s("FAIL "); l.t.s(why); l.t.s(": the gt is off for this session, every user falls back to the cpu"); l.go();
    }
    bool quiet = true;
    if (!g.sim && g_mmio && g.fw_render) {
        dump_engine("at the failure");
        // i915 intel_engine_stop_cs + gen8_engine_reset_prepare +
        // gen11_reset_engines: stop the ring, ask the engine to get ready,
        // reset the blitter domain alone, withdraw the request. (satoru)
        wr(BASE + RING_MI_MODE, MaskedEnable(STOP_RING));
        uint32_t last = 0;
        wr(BASE + RING_RESET_CTL, MaskedEnable(1u));
        bool ready = wait_bits(BASE + RING_RESET_CTL, 2u, 2u, 1000, &last);
        wr(R_GDRST, GRDOM_BLT);
        bool done = wait_bits(R_GDRST, GRDOM_BLT, 0, 5000, &last);
        wr(BASE + RING_RESET_CTL, MaskedDisable(1u));
        Ln l; l.t.s("engine reset: ready "); l.t.d(ready); l.t.s(" done "); l.t.d(done);
        l.t.s(" gdrst 0x"); l.t.x(last, 8); l.go();
        dump_engine("after the reset");
        quiet = done;
    }
    if (!g.sim && g_mmio) {
        if (g.fw_render) fw_put(R_FW_RENDER);
        if (g.fw_gt) fw_put(R_FW_GT);
        g.fw_render = g.fw_gt = false;
    }
    // memory the engine might still touch is never handed back: if the reset
    // was not confirmed, the pages and the ggtt entries stay where they are. (satoru)
    if (quiet) release_memory();
    else emit("the engine reset was not confirmed: the context memory and its ggtt entries are left in place");
}

// ---- submission (xe_execlist.c __start_lrc) (satoru) ----
uint32_t submit_locked() {
    if (!g.bopen || g.inflight || g.dead) return 0;
    uint32_t* batch = (uint32_t*)(uintptr_t)g.batch;
    g.blen = EmitBatchEnd(batch, g.blen);
    g.bopen = false;
    uint32_t seq = ++g.seq;
    if (!seq) seq = ++g.seq;
    uint32_t dw[JOB_DWORDS];
    int n = EmitJob(dw, BASE, g.ggtt + PG_SEQNO * 4096u, g.ggtt + PG_SEQNO * 4096u + 8u, seq, g.batch);
    uint32_t* ring = blk_dw(PG_RING);
    for (int i = 0; i < n; i++) {
        ring[g.tail / 4] = dw[i];
        g.tail = (g.tail + 4u) % RING_BYTES;
    }
    uint32_t* state = blk_dw(PG_STATE);
    // drop any cached copy of the line first: it also holds the head the
    // engine saved, and that must come from ram, not from an old cache line. (satoru)
    clflush_range(state, 64);
    state[CTX_RING_TAIL] = g.tail;
    // nothing the engine fetches may exist only in a cpu cache (satoru)
    clflush_range(batch, (uint64_t)g.blen * 4u);
    clflush_range(ring, RING_BYTES);
    clflush_range(state, 4096);
    flush_tables();
    uint64_t desc = CtxDescriptor(g.ggtt + PG_PPHWSP * 4096u, 1);
    g.inflight = seq;
    g.seen = false;
    g.inflight_t0 = tsc();
    g.total_jobs++;
    if (g.sim) {
        SimEnv env = { nullptr, sim_ggtt_page, sim_phys_page };
        SimResult r;
        if (!SimSubmit(env, BASE, desc, &r)) {
            Ln l; l.t.s("model: "); l.t.s(r.err ? r.err : "?"); l.t.s(" (0x"); l.t.x(r.err_arg, 8); l.t.s(")"); l.go();
            gt_dead("the software model refused the job");
            return 0;
        }
        if (r.blit_cctl != BLIT_CCTL_VALUE || !r.tlb_invalidated) {
            gt_dead("the software model saw a job without the mocs load or the tlb invalidate");
            return 0;
        }
        return seq;
    }
    bool lw = g_log_writes;
    g_log_writes = g.total_jobs <= 2;          // the first two submits are shown in full (satoru)
    wr(BASE + RING_HWS_PGA, g.ggtt + PG_HWSP * 4096u);
    (void)rd(BASE + RING_HWS_PGA);
    wr(BASE + RING_MODE, MaskedEnable(GFX_DISABLE_LEGACY_MODE));
    wr(BASE + RING_EXECLIST_SQ_LO, (uint32_t)desc);
    wr(BASE + RING_EXECLIST_SQ_HI, (uint32_t)(desc >> 32));
    wr(BASE + RING_EXECLIST_CONTROL, EL_CTRL_LOAD);
    g_log_writes = lw;
    return seq;
}

// 1 done, 0 running, -1 failed. the seqno line is flushed before the read so a
// stale cached copy can never hide the engine's write. (satoru)
int poll_locked(uint32_t seq) {
    if (g.dead) return -1;
    if (!seq || g.inflight != seq) return 1;      // an older job: long finished (satoru)
    volatile uint32_t* sq = (volatile uint32_t*)blk_dw(PG_SEQNO);
    __asm__ __volatile__("clflush (%0)" : : "r"((const void*)sq) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
    if (sq[2] == seq) {
        volatile uint32_t* st = (volatile uint32_t*)blk_dw(PG_STATE);
        __asm__ __volatile__("clflush (%0)" : : "r"((const void*)st) : "memory");
        __asm__ __volatile__("mfence" ::: "memory");
        bool saved = (st[CTX_RING_HEAD] & 0x1FFFFCu) == g.tail;
        uint32_t cpu = SMP::CpuIndex();
        uint64_t now = tsc();
        if (!g.seen) { g.seen = true; g.seen_tsc = now; g.seen_cpu = cpu; g.seen_moves = 0; }
        if (!saved) {
            if (cpu != g.seen_cpu) { g.seen_cpu = cpu; g.seen_tsc = now; g.seen_moves++; }
            if (g.seen_moves < 4 && (now - g.seen_tsc) / g_tpu < 500) return 0;
            g.save_misses++;
        } else if (cpu == g.seen_cpu) {
            uint32_t su = (uint32_t)((now - g.seen_tsc) / g_tpu);
            if (su > g.save_us_max) g.save_us_max = su;
        }
        g.seen = false;
        uint64_t us = us_since(g.inflight_t0);
        if (us > 10000000ull) us = 0;          // polled from another cpu with a different tsc: no figure (satoru)
        g.last_job_us = (uint32_t)us;
        g.st_us += us;
        g.st_jobs++;
        g.st_bytes += g.bbytes;
        g.inflight = 0;
        return 1;
    }
    return 0;
}

bool wait_locked(uint32_t seq, uint32_t timeout_us) {
    uint64_t t0 = tsc();
    for (uint64_t guard = (uint64_t)timeout_us * 64 + 4096; guard; guard--) {
        int p = poll_locked(seq);
        if (p > 0) return true;
        if (p < 0) return false;
        if (us_since(t0) > timeout_us) break;
        __asm__ __volatile__("pause");
    }
    {
        volatile uint32_t* sq = (volatile uint32_t*)blk_dw(PG_SEQNO);
        Ln l; l.t.s("job "); l.t.d((uint64_t)seq); l.t.s(" not finished after "); l.t.d(us_since(t0));
        l.t.s(" us: start seqno "); l.t.d((uint64_t)sq[0]); l.t.s(" end seqno "); l.t.d((uint64_t)sq[2]);
        l.t.s(sq[0] == seq ? " (the engine started it)" : " (the engine never started it)"); l.go();
    }
    gt_dead("a job timed out");
    return false;
}

// blits of one batch that touch the same pixels are separated by MI_FLUSH_DW (upstream
// i915 emits one between dependent blits, intel_migrate.c emit_flush before each
// emit_copy): without it the blitter write cache is not drained and a later blit
// reads or overwrites stale memory (measured on the rig, selftest). g_flush_mode:
// 0 never (the old builder), 1 when a blit touches pixels an earlier blit of the
// batch touched (default), 2 after every blit. (satoru)
int g_flush_mode = 1;
struct BRect { uint64_t base; int x, y, w, h; };
BRect g_br[48];
int g_br_n = 0;
uint32_t g_flushes = 0;
inline bool br_hit(const BRect& a, uint64_t base, int x, int y, int w, int h) {
    return a.base == base && x < a.x + a.w && a.x < x + w && y < a.y + a.h && a.y < y + h;
}
void br_flush() {
    uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
    g.blen = EmitFlushDw(b, g.blen, g.ggtt + PG_SEQNO * 4096u + 16u);
    g.bbytes += 0;
    g_br_n = 0;
    g_flushes++;
}
// before a blit: flush if it touches what an earlier one of this batch touched (satoru)
void br_before(uint64_t dbase, int dx, int dy, int w, int h, bool is_copy, uint64_t sbase, int sx, int sy) {
    if (g_flush_mode == 0) return;
    bool hit = g_flush_mode == 2 && g_br_n > 0;
    for (int i = 0; i < g_br_n && !hit; i++) {
        hit = br_hit(g_br[i], dbase, dx, dy, w, h) || (is_copy && br_hit(g_br[i], sbase, sx, sy, w, h));
    }
    if (hit || g_br_n + 2 > 48) br_flush();
    g_br[g_br_n++] = BRect{ dbase, dx, dy, w, h };
    if (is_copy) g_br[g_br_n++] = BRect{ sbase, sx, sy, w, h };
}
void emit_fill_t(uint64_t dst, uint32_t pitch, int x, int y, int w, int h, uint32_t color) {
    br_before(dst, x, y, w, h, false, 0, 0, 0);
    g.blen = EmitFill((uint32_t*)(uintptr_t)g.batch, g.blen, dst, pitch, x, y, w, h, color);
}
void emit_copy_t(uint64_t dst, uint32_t dpitch, int dx, int dy, uint64_t src, uint32_t spitch, int sx, int sy, int w, int h) {
    br_before(dst, dx, dy, w, h, true, src, sx, sy);
    g.blen = EmitCopy((uint32_t*)(uintptr_t)g.batch, g.blen, dst, dpitch, dx, dy, src, spitch, sx, sy, w, h);
}

bool begin_locked() {
    if (g.dead || !g.blk || g.inflight) return false;
    g_br_n = 0;
    g.blen = 0;
    g.bbytes = 0;
    g.bopen = true;
    return true;
}

bool map_locked(uint64_t phys, uint64_t bytes, bool scanout) {
    if (g.dead || !g.pg.pml4 || (phys & 0xFFF) || !bytes || phys + bytes > PMM::GetPhysTop()) return false;
    uint64_t pages = (bytes + 4095) >> 12;
    g.tables_dirty = true;
    if (!PpgttMap(g.pg, phys, phys, pages, scanout ? PAT_UC : PAT_WB_1WAY)) return false;
    flush_tables();
    return true;
}

// ---- self-tests (satoru) ----
inline uint32_t pat_px(int x, int y, uint32_t salt) { return 0xFF000000u | ((uint32_t)(x * 7 + y * 131 + (int)salt) * 2654435761u >> 8); }

void fill_pattern(uint64_t phys, uint32_t salt, bool flush) {
    uint32_t* p = (uint32_t*)(uintptr_t)phys;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) p[y * TEST_W + x] = pat_px(x, y, salt);
    if (flush) clflush_range(p, TEST_PITCH * TEST_H);
}

bool run_job(const char* what) {
    uint32_t seq = submit_locked();
    if (!seq) { Ln l; l.t.s("FAIL "); l.t.s(what); l.t.s(": nothing submitted"); l.go(); return false; }
    return wait_locked(seq, JOB_TIMEOUT_US);
}

bool test_noop() {
    if (!begin_locked()) return false;
    uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
    b[g.blen++] = MI_NOOP;
    b[g.blen++] = MI_NOOP;
    volatile uint32_t* sq = (volatile uint32_t*)blk_dw(PG_SEQNO);
    if (!run_job("no-op batch")) return false;
    Ln l; l.t.s("PASS no-op batch: the engine wrote start seqno "); l.t.d((uint64_t)sq[0]); l.t.s(" and end seqno ");
    l.t.d((uint64_t)sq[2]); l.t.s(" in "); l.t.d((uint64_t)g.last_job_us); l.t.s(" us");
    l.t.s(g.sim ? " (software model)" : " (execlist submission without guc works)"); l.go();
    return true;
}

bool test_fill() {
    const int fx = 16, fy = 8, fw = 100, fh = 40;
    const uint32_t color = 0xFF3366CCu;
    fill_pattern(g.test_b, 1, true);
    if (!begin_locked()) return false;
    uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
    g.blen = EmitFill(b, g.blen, g.test_b, TEST_PITCH, fx, fy, fw, fh, color);
    if (!run_job("fill")) return false;
    uint32_t* p = (uint32_t*)(uintptr_t)g.test_b;
    clflush_range(p, TEST_PITCH * TEST_H);
    int bad = 0, first_x = -1, first_y = -1;
    uint32_t first_v = 0;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) {
            bool in = x >= fx && x < fx + fw && y >= fy && y < fy + fh;
            uint32_t want = in ? color : pat_px(x, y, 1);
            if (p[y * TEST_W + x] != want) { if (!bad) { first_x = x; first_y = y; first_v = p[y * TEST_W + x]; } bad++; }
        }
    Ln l; l.t.s(bad ? "FAIL fill: " : "PASS fill: "); l.t.d((uint64_t)fw); l.t.c('x'); l.t.d((uint64_t)fh);
    l.t.s(" at ("); l.t.d((uint64_t)fx); l.t.c(','); l.t.d((uint64_t)fy); l.t.s(") of a 256x64 surface, cpu readback ");
    if (bad) {
        l.t.d((uint64_t)bad); l.t.s(" wrong pixels, first at ("); l.t.d((uint64_t)first_x); l.t.c(','); l.t.d((uint64_t)first_y);
        l.t.s(") = 0x"); l.t.x(first_v, 8);
    } else {
        l.t.s("matches inside and outside the rectangle, "); l.t.d((uint64_t)g.last_job_us); l.t.s(" us");
    }
    l.go();
    return bad == 0;
}

// copy a rectangle of a to b and compare. cached = the source is written with
// ordinary stores and NOT flushed, after ram was given a different pattern:
// the destination then tells whether the engine saw the cpu caches. (satoru)
int copy_and_compare(bool cached, uint32_t salt) {
    const int sx = 24, sy = 4, dx = 40, dy = 12, w = 180, h = 44;
    if (cached) fill_pattern(g.test_a, salt + 100, true);
    fill_pattern(g.test_a, salt, !cached);
    fill_pattern(g.test_b, salt + 7, true);
    if (!begin_locked()) return -1;
    uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
    g.blen = EmitCopy(b, g.blen, g.test_b, TEST_PITCH, dx, dy, g.test_a, TEST_PITCH, sx, sy, w, h);
    if (!run_job("copy")) return -1;
    uint32_t* p = (uint32_t*)(uintptr_t)g.test_b;
    clflush_range(p, TEST_PITCH * TEST_H);
    int bad = 0;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) {
            bool in = x >= dx && x < dx + w && y >= dy && y < dy + h;
            uint32_t want = in ? pat_px(x - dx + sx, y - dy + sy, salt) : pat_px(x, y, salt + 7);
            if (p[y * TEST_W + x] != want) bad++;
        }
    return bad;
}

bool test_copy() {
    int bad = copy_and_compare(false, 3);
    if (bad < 0) return false;
    Ln l; l.t.s(bad ? "FAIL copy: " : "PASS copy: "); l.t.s("180x44 from (24,4) to (40,12) between two surfaces, cpu readback ");
    if (bad) { l.t.d((uint64_t)bad); l.t.s(" wrong pixels"); }
    else { l.t.s("matches, "); l.t.d((uint64_t)g.last_job_us); l.t.s(" us"); }
    l.go();
    return bad == 0;
}

void test_coherency() {
    int bad = copy_and_compare(true, 11);
    if (bad < 0) return;
    g.coherent_read = bad == 0;
    {
        Ln l; l.t.s(bad ? "NOTE coherency: " : "PASS coherency: ");
        if (bad) { l.t.s("a cpu-cached, unflushed source came out with "); l.t.d((uint64_t)bad); l.t.s(" stale pixels: sources are clflushed before a job"); }
        else l.t.s("the engine read a cpu-cached, unflushed source correctly (pat 3, one way coherent): no clflush needed");
        l.go();
    }
    // the other direction: lines of the destination sit in the cpu cache, the
    // engine overwrites them, the cpu reads without a flush. (satoru)
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)g.test_b;
    uint32_t sum = 0;
    for (int i = 0; i < TEST_W * TEST_H; i++) sum += p[i];
    (void)sum;
    if (!begin_locked()) return;
    uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
    g.blen = EmitFill(b, g.blen, g.test_b, TEST_PITCH, 0, 0, TEST_W, TEST_H, 0xFF00AA55u);
    if (!run_job("fill for the coherency check")) return;
    int stale = 0;
    for (int i = 0; i < TEST_W * TEST_H; i++) if (p[i] != 0xFF00AA55u) stale++;
    g.coherent_write = stale == 0;
    Ln l; l.t.s("coherency: after an engine fill the cpu, without a clflush, read ");
    if (stale) { l.t.d((uint64_t)stale); l.t.s(" stale pixels of 16384 (readers of engine output flush first)"); }
    else l.t.s("all 16384 pixels new");
    l.go();
    clflush_range((void*)(uintptr_t)g.test_b, TEST_PITCH * TEST_H);
}

bool test_resubmit() {
    // back to back jobs with no pause, enough of them to wrap the 16k ring:
    // the context is resubmitted right after each save (xe_execlist.c
    // resubmits the same context the same way). (satoru)
    uint32_t worst = 0;
    for (int i = 0; i < 256; i++) {
        if (!begin_locked()) return false;
        uint32_t* b = (uint32_t*)(uintptr_t)g.batch;
        g.blen = EmitFill(b, g.blen, g.test_b, TEST_PITCH, i % 48, i % 48, 8, 8, 0xFF000000u | (uint32_t)i);
        if (!run_job("resubmit")) return false;
        if (g.last_job_us > worst) worst = g.last_job_us;
    }
    uint32_t* p = (uint32_t*)(uintptr_t)g.test_b;
    clflush_range(p, TEST_PITCH * TEST_H);
    // the last job (255) filled (15,15) to (22,22); job 254 left (14,14) (satoru)
    bool ok = p[15 * TEST_W + 15] == (0xFF000000u | 255u) && p[22 * TEST_W + 22] == (0xFF000000u | 255u) &&
              p[14 * TEST_W + 14] == (0xFF000000u | 254u);
    Ln l; l.t.s(ok ? "PASS resubmit: " : "FAIL resubmit: "); l.t.s("256 jobs back to back, the ring wrapped ");
    l.t.d((uint64_t)((g.total_jobs * (uint32_t)JOB_DWORDS * 4u) / RING_BYTES)); l.t.s(" time(s), worst job ");
    l.t.d((uint64_t)worst); l.t.s(" us, context save seen at most "); l.t.d((uint64_t)g.save_us_max);
    l.t.s(" us after the seqno ("); l.t.d((uint64_t)g.save_misses); l.t.s(" not seen within 500 us)"); l.go();
    return ok;
}

void test_throughput() {
    // a real number for the report: 4 mb through the engine against the same
    // 4 mb with the cpu's non-temporal copy the flip driver uses. (satoru)
    const uint32_t W = 1024, H = 1024, PITCH = W * 4, PAGES = PITCH * H / 4096;
    uint64_t a = PMM::AllocContiguous(PAGES), b2 = PMM::AllocContiguous(PAGES);
    if (!a || !b2 || !map_locked(a, (uint64_t)PAGES * 4096, false) || !map_locked(b2, (uint64_t)PAGES * 4096, true)) {
        emit("throughput: skipped, no memory");
        if (a) PMM::FreeContiguous(a, PAGES);
        if (b2) PMM::FreeContiguous(b2, PAGES);
        return;
    }
    uint32_t* s = (uint32_t*)(uintptr_t)a;
    for (uint32_t i = 0; i < W * H; i++) s[i] = i * 2654435761u;
    clflush_range(s, (uint64_t)PITCH * H);
    clflush_range((void*)(uintptr_t)b2, (uint64_t)PITCH * H);
    uint32_t gpu_us = 0;
    bool ok = false;
    if (begin_locked()) {
        uint32_t* bb = (uint32_t*)(uintptr_t)g.batch;
        g.blen = EmitCopy(bb, g.blen, b2, PITCH, 0, 0, a, PITCH, 0, 0, (int)W, (int)H);
        g.bbytes = (uint64_t)PITCH * H;
        ok = run_job("throughput copy");
        gpu_us = g.last_job_us;
    }
    if (ok) {
        const uint32_t* d = (const uint32_t*)(uintptr_t)b2;
        clflush_range(d, (uint64_t)PITCH * H);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < W * H; i += 97) if (d[i] != i * 2654435761u) bad++;
        uint64_t t0 = tsc();
        uint8_t* dp = (uint8_t*)(uintptr_t)b2;
        const uint8_t* sp = (const uint8_t*)(uintptr_t)a;
        for (uint64_t o = 0; o < (uint64_t)PITCH * H; o += 64)
            __asm__ __volatile__(
                "movdqu   (%0), %%xmm0;  movdqu 16(%0), %%xmm1;"
                "movdqu 32(%0), %%xmm2;  movdqu 48(%0), %%xmm3;"
                "movntdq %%xmm0,   (%1); movntdq %%xmm1, 16(%1);"
                "movntdq %%xmm2, 32(%1); movntdq %%xmm3, 48(%1);"
                :: "r"(sp + o), "r"(dp + o) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
        __asm__ __volatile__("sfence" ::: "memory");
        uint64_t cpu_us = us_since(t0);
        Ln l; l.t.s(bad ? "FAIL throughput: " : "throughput: "); l.t.s("4096 kb copy, engine "); l.t.d((uint64_t)gpu_us);
        l.t.s(" us ("); l.t.d(gpu_us ? 4096000ull / gpu_us : 0); l.t.s(" mb/s, submit to seqno), cpu non-temporal ");
        l.t.d(cpu_us); l.t.s(" us ("); l.t.d(cpu_us ? 4096000ull / cpu_us : 0); l.t.s(" mb/s, source uncached)");
        if (bad) { l.t.s(", "); l.t.d((uint64_t)bad); l.t.s(" sampled pixels wrong"); }
        if (g.sim) l.t.s(" [software model: the engine number is cpu time]");
        l.go();
    }
    if (!g.dead) {
        PMM::FreeContiguous(a, PAGES);
        PMM::FreeContiguous(b2, PAGES);
    }
}

// kurono.igpu.selftest=N: N random fills and copies between two 256x64 surfaces,
// jobops ops to a job, both surfaces read back and compared with a cpu model after
// each job. seeded, so a failure replays. modes (kurono.igpu.selftest.mode): 0 random
// (dependent), 1 independent (A is only read, every op writes its own cell of B),
// 2 dependent pairs (fill a rect of A, copy it to B), 3 fills only, 4 copies only,
// 5 copy then copy back (a copy reads what a copy wrote).
// the first 5 wrong jobs are described op by op. (satoru)
int g_selftest_n = 0;
int g_selftest_jobops = 16;   // ops per job, kurono.igpu.selftest.jobops=N (satoru)
int g_selftest_mode = 0;
uint32_t g_rng = 0x2545F491u;
uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
uint32_t g_sh[2][TEST_W * TEST_H];
uint32_t g_pre[2][TEST_W * TEST_H];
struct StOp { int type, d, x, y, w, h, sx, sy; uint32_t c; };   // type 0 fill, 1 copy (satoru)
StOp g_stops[32];

bool selftest_random() {
    if (!g.up || g.dead || !g.test_a || !g.test_b) return false;
    uint64_t surf[2] = { g.test_a, g.test_b };
    uint64_t ops = 0, jobs = 0, bad_px = 0, bad_jobs = 0, job_us = 0, timeouts = 0, bad_px_late = 0, late_fixed_jobs = 0;
    uint32_t worst_us = 0, flushes0 = g_flushes;
    int first_x = -1, first_y = -1, first_s = -1, described = 0;
    int jobops = (g_selftest_mode == 2 || g_selftest_mode == 5) ? 2 : g_selftest_jobops;
    if (jobops > 32) jobops = 32;
    if (g_selftest_mode == 1 && jobops > 16) jobops = 16;
    {
        Ln l; l.t.s("selftest: mode "); l.t.d((uint64_t)g_selftest_mode); l.t.s(", "); l.t.d((uint64_t)jobops);
        l.t.s(" ops per job, flush mode "); l.t.d((uint64_t)g_flush_mode); l.go();
    }
    while ((int)ops < g_selftest_n && !g.dead) {
        for (int k = 0; k < 2; k++) {
            fill_pattern(surf[k], 100u + (uint32_t)jobs * 2u + (uint32_t)k, true);
            uint32_t* pp = (uint32_t*)(uintptr_t)surf[k];
            for (int i = 0; i < TEST_W * TEST_H; i++) g_sh[k][i] = g_pre[k][i] = pp[i];
        }
        if (!begin_locked()) { timeouts++; break; }
        int in_job = 0;
        while (in_job < jobops && (int)ops < g_selftest_n) {
            StOp o = {};
            int m = g_selftest_mode;
            if (m == 1) {
                o.d = 1; o.w = 1 + (int)(rnd() % 64u); o.h = 1 + (int)(rnd() % 16u);
                o.x = (in_job % 4) * 64; o.y = (in_job / 4) * 16;
                o.type = (int)(rnd() & 1u);
                o.sx = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.sy = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
            } else if (m == 5) {
                // copy then copy back of the same rect: a copy reads what a copy wrote (satoru)
                if ((in_job & 1) == 0) {
                    o.type = 1; o.d = 1; o.w = 1 + (int)(rnd() % 64u); o.h = 1 + (int)(rnd() % 32u);
                    o.x = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.y = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
                    o.sx = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.sy = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
                } else {
                    StOp& pv = g_stops[in_job - 1];
                    o.type = 1; o.d = 0; o.w = pv.w; o.h = pv.h; o.x = pv.sx; o.y = pv.sy; o.sx = pv.x; o.sy = pv.y;
                }
            } else if (m == 2) {
                if ((in_job & 1) == 0) {
                    o.type = 0; o.d = 0; o.w = 1 + (int)(rnd() % 64u); o.h = 1 + (int)(rnd() % 32u);
                    o.x = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.y = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
                } else {
                    StOp& pv = g_stops[in_job - 1];
                    o.type = 1; o.d = 1; o.w = pv.w; o.h = pv.h; o.x = pv.x; o.y = pv.y; o.sx = pv.x; o.sy = pv.y;
                }
            } else {
                o.w = 1 + (int)(rnd() % 64u); o.h = 1 + (int)(rnd() % 32u);
                o.x = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.y = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
                o.d = (int)(rnd() & 1u);
                o.type = m == 3 ? 0 : m == 4 ? 1 : (int)(rnd() & 1u);
                o.sx = (int)(rnd() % (uint32_t)(TEST_W - o.w + 1)); o.sy = (int)(rnd() % (uint32_t)(TEST_H - o.h + 1));
            }
            if (o.type == 0) {
                o.c = 0xFF000000u | (rnd() & 0xFFFFFFu);
                emit_fill_t(surf[o.d], TEST_PITCH, o.x, o.y, o.w, o.h, o.c);
                for (int yy = o.y; yy < o.y + o.h; yy++) for (int xx = o.x; xx < o.x + o.w; xx++) g_sh[o.d][yy * TEST_W + xx] = o.c;
            } else {
                emit_copy_t(surf[o.d], TEST_PITCH, o.x, o.y, surf[1 - o.d], TEST_PITCH, o.sx, o.sy, o.w, o.h);
                for (int yy = 0; yy < o.h; yy++) for (int xx = 0; xx < o.w; xx++)
                    g_sh[o.d][(o.y + yy) * TEST_W + o.x + xx] = g_sh[1 - o.d][(o.sy + yy) * TEST_W + o.sx + xx];
            }
            g_stops[in_job] = o;
            ops++; in_job++;
        }
        uint32_t seq = submit_locked();
        if (!seq || !wait_locked(seq, JOB_TIMEOUT_US)) { timeouts++; break; }
        jobs++; job_us += g.last_job_us;
        if (g.last_job_us > worst_us) worst_us = g.last_job_us;
        uint64_t bad_here = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t* pp = (uint32_t*)(uintptr_t)surf[k];
            clflush_range(pp, TEST_PITCH * TEST_H);
            for (int i = 0; i < TEST_W * TEST_H; i++)
                if (pp[i] != g_sh[k][i]) {
                    if (first_s < 0) { first_s = k; first_x = i % TEST_W; first_y = i / TEST_W; }
                    bad_px++; bad_here++;
                    if (described < 5 && bad_here == 1) {
                        // the last op of the job that wrote this pixel (satoru)
                        int px = i % TEST_W, py = i / TEST_W, wop = -1;
                        for (int q = 0; q < in_job; q++)
                            if (g_stops[q].d == k && px >= g_stops[q].x && px < g_stops[q].x + g_stops[q].w && py >= g_stops[q].y && py < g_stops[q].y + g_stops[q].h) wop = q;
                        Ln l; l.t.s("selftest bad job "); l.t.d(jobs); l.t.s(": surface "); l.t.d((uint64_t)k); l.t.s(" pixel ("); l.t.d((uint64_t)px); l.t.c(','); l.t.d((uint64_t)py);
                        l.t.s(") got 0x"); l.t.x(pp[i], 8); l.t.s(" model 0x"); l.t.x(g_sh[k][i], 8); l.t.s(" before the job 0x"); l.t.x(g_pre[k][i], 8);
                        if (wop >= 0) {
                            const StOp& o = g_stops[wop];
                            l.t.s(", written by op "); l.t.d((uint64_t)wop); l.t.s(" of "); l.t.d((uint64_t)in_job); l.t.s(o.type ? " copy to (" : " fill at (");
                            l.t.d((uint64_t)o.x); l.t.c(','); l.t.d((uint64_t)o.y); l.t.c(' '); l.t.d((uint64_t)o.w); l.t.c('x'); l.t.d((uint64_t)o.h); l.t.c(')');
                            if (o.type) {
                                int ox = px - o.x, oy = py - o.y;
                                l.t.s(" from surface "); l.t.d((uint64_t)(1 - o.d)); l.t.s(" ("); l.t.d((uint64_t)o.sx); l.t.c(','); l.t.d((uint64_t)o.sy);
                                l.t.s("), that source pixel was 0x"); l.t.x(g_pre[1 - o.d][(o.sy + oy) * TEST_W + o.sx + ox], 8); l.t.s(" before the job");
                            }
                        } else l.t.s(", no op of the job writes it");
                        l.go();
                    }
                }
        }
        if (bad_here) {
            bad_jobs++; described++;
            // readback race? read again after 10 ms (satoru)
            udelay(10000);
            uint64_t late = 0;
            for (int k = 0; k < 2; k++) {
                uint32_t* pp = (uint32_t*)(uintptr_t)surf[k];
                clflush_range(pp, TEST_PITCH * TEST_H);
                for (int i = 0; i < TEST_W * TEST_H; i++) if (pp[i] != g_sh[k][i]) late++;
            }
            bad_px_late += late;
            if (late == 0) late_fixed_jobs++;
        }
    }
    Ln l; l.t.s("selftest: "); l.t.d(ops); l.t.s(" ops in "); l.t.d(jobs); l.t.s(" jobs, "); l.t.d(bad_px);
    l.t.s(" mismatching pixels in "); l.t.d(bad_jobs); l.t.s(" jobs ("); l.t.d(bad_px_late); l.t.s(" still wrong 10 ms later, "); l.t.d(late_fixed_jobs);
    l.t.s(" jobs fully right on the second read), "); l.t.d(timeouts); l.t.s(" timeouts, "); l.t.d((uint64_t)(g_flushes - flushes0));
    l.t.s(" flushes, job time "); l.t.d(ops ? job_us * 1000u / ops : 0); l.t.s(" ns per op (worst job "); l.t.d((uint64_t)worst_us); l.t.s(" us)");
    if (first_s >= 0) { l.t.s(", first bad pixel surface "); l.t.d((uint64_t)first_s); l.t.s(" ("); l.t.d((uint64_t)first_x); l.t.c(','); l.t.d((uint64_t)first_y); l.t.c(')'); }
    l.go();
    bool ok = ops >= (uint64_t)g_selftest_n && !bad_px && !timeouts;
    emit(ok ? "SELFTEST PASS" : "SELFTEST FAIL");
    return ok;
}

bool blit_ladder() {
    if (!g.up || g.dead) return false;
    if (!g.test_a) {
        g.test_a = PMM::AllocContiguous(TEST_PAGES);
        g.test_b = PMM::AllocContiguous(TEST_PAGES);
        if (!g.test_a || !g.test_b || !map_locked(g.test_a, TEST_PAGES * 4096ull, false) ||
            !map_locked(g.test_b, TEST_PAGES * 4096ull, false)) {
            emit("FAIL blit tests: no memory for the test surfaces");
            return false;
        }
    }
    g.blit_ok = false;
    if (!test_fill() || g.dead) return false;
    if (!test_copy() || g.dead) return false;
    test_coherency();
    if (g.dead) return false;
    if (!test_resubmit() || g.dead) return false;
    test_throughput();
    if (g.dead) return false;
    g.blit_ok = true;
    emit("ENABLED blitter: fills and copies verified by cpu readback");
    return true;
}

bool bring_up() {
    emit(g.sim ? "---- intel gt: blitter engine against the software model (kurono.igpu.gttest=1) ----"
               : "---- intel gt: forcewake, pat/mocs, execlist context on the blitter engine (no guc) ----");
    if (!g.sim) {
        g_mmio = IntelDisplayProbe::Mmio();
        if (!g_mmio) {
            emit("no usable intel display engine: the gt comes up on its own (standalone bar0 + ggtt)");
            if (!sa_map()) return false;
        }
    }
    // memory and the ggtt entries first: if they cannot be had, no gt register
    // has been written. (satoru)
    if (!setup_memory()) return false;
    if (!g.sim) {
        g.fw_gt = fw_get(R_FW_GT, R_FW_GT_ACK, "gt");
        if (!g.fw_gt) return false;
        g.fw_render = fw_get(R_FW_RENDER, R_FW_RENDER_ACK, "render");
        if (!g.fw_render) return false;
        uint32_t gmd = rd(R_GMD_ID);
        uint32_t arch = gmd >> 22, rel = (gmd >> 14) & 0xFF;
        {
            Ln l; l.t.s((arch == 12 && rel >= 70 && rel <= 74) ? "PASS graphics ip " : "FAIL graphics ip ");
            l.t.d((uint64_t)arch); l.t.c('.'); l.t.d((uint64_t)rel); l.t.s(" (gmd_id 0x"); l.t.x(gmd, 8);
            l.t.s(arch == 12 && rel >= 70 && rel <= 74 ? "): xe-lpg" : "): not xe-lpg 12.70 to 12.74, refusing"); l.go();
            if (!(arch == 12 && rel >= 70 && rel <= 74)) return false;
        }
        uint32_t gs = rd(R_GUC_STATUS);
        {
            bool running = ((gs >> 8) & 0xFF) == 0xF0;     // XE_GUC_LOAD_STATUS_READY (satoru)
            Ln l; l.t.s(running ? "FAIL guc: status 0x" : "PASS guc: status 0x"); l.t.x(gs, 8);
            l.t.s(running ? ", firmware is running and may own the submit ports, refusing" : ", no firmware running, the execlist ports are ours"); l.go();
            if (running) return false;
        }
        dump_engine("as the firmware left it");
        uint32_t id = rd(BASE + ENGINE_ID);
        if ((id & 7) != 3) {
            Ln l; l.t.s("NOTE engine id 0x"); l.t.x(id, 8); l.t.s(": class is not 3 (copy); continuing, the no-op job decides"); l.go();
        }
        g.pat_ok = program_pat_mocs();
        wr(R_GUC_TLB_INV_CR, 1);                   // the ggtt entries of the context block are new (satoru)
        // xe_hw_engine_enable_ring (satoru)
        wr(BASE + RING_HWSTAM, 0xFFFFFFFFu);
        wr(BASE + RING_HWS_PGA, g.ggtt + PG_HWSP * 4096u);
        wr(BASE + RING_MODE, MaskedEnable(GFX_DISABLE_LEGACY_MODE));
        wr(BASE + RING_MI_MODE, MaskedDisable(STOP_RING));
        uint32_t mi = rd(BASE + RING_MI_MODE), mode = rd(BASE + RING_MODE);
        bool ok = (mode & GFX_DISABLE_LEGACY_MODE) && !(mi & STOP_RING);
        Ln l; l.t.s(ok ? "PASS engine enable: " : "FAIL engine enable: "); l.t.s("ring mode 0x"); l.t.x(mode, 8);
        l.t.s(" (execlists "); l.t.s((mode & GFX_DISABLE_LEGACY_MODE) ? "on" : "NOT accepted"); l.t.s("), mi_mode 0x"); l.t.x(mi, 8);
        l.t.s(", hws 0x"); l.t.x(rd(BASE + RING_HWS_PGA), 8); l.go();
        if (!ok) return false;
    }
    if (!test_noop()) return false;
    g.up = true;
    emit("ENABLED gt: blitter engine up");
    return true;
}

} // namespace

// ---- public api (satoru) ----
void IntelGt::SetSelftest(int n, int jobops, int mode, int flush) {
    g_selftest_n = n; g_selftest_mode = mode; g_flush_mode = flush;
    if (jobops > 0 && jobops <= 32) g_selftest_jobops = jobops;
}

int IntelGt::Boot(bool gt, bool blt, bool sim) {
    if (!gt && !blt && !sim) return 0;             // default: nothing runs, nothing logs (satoru)
    SpinLockCpuGuard guard(g_gt_lock);
    if (g.up || g.dead) return 0;
    begin_out(nullptr, 0);
    clock_init();
    g.sim = sim;
    if (!bring_up()) {
        if (!g.dead) {
            // nothing was submitted yet: just give back what was taken (satoru)
            if (!g.sim && g_mmio) {
                if (g.fw_render) fw_put(R_FW_RENDER);
                if (g.fw_gt) fw_put(R_FW_GT);
                g.fw_render = g.fw_gt = false;
            }
            release_memory();
            g.dead = true;
            emit("gt: not enabled, the cpu paths stay");
        }
        return 0;
    }
    if (blt || sim) blit_ladder();
    if (g_selftest_n > 0 && g.up && g.blit_ok && !g.dead) selftest_random();
    {
        Ln l; l.t.s("summary: engine "); l.t.s(g.up ? "up" : "off"); l.t.s(", blitter "); l.t.s(g.blit_ok ? "verified" : "not enabled");
        l.t.s(", "); l.t.d((uint64_t)g.total_jobs); l.t.s(" jobs, "); l.t.d((uint64_t)g_blocked); l.t.s(" blocked writes"); l.go();
    }
    return 0;
}

bool IntelGt::Ready() { return g.up && !g.dead; }
bool IntelGt::BlitReady() { return g.up && g.blit_ok && !g.dead; }
bool IntelGt::Simulated() { return g.sim; }
bool IntelGt::CoherentRead() { return g.coherent_read; }

bool IntelGt::MapBuffer(uint64_t phys, uint64_t bytes, bool scanout) {
    SpinLockCpuGuard guard(g_gt_lock);
    if (!g.up) return false;
    return map_locked(phys, bytes, scanout);
}

bool IntelGt::Busy() { return g.inflight != 0; }

bool IntelGt::Begin() {
    SpinLockCpuGuard guard(g_gt_lock);
    if (!g.blit_ok) return false;
    return begin_locked();
}

bool IntelGt::AddCopy(uint64_t dst_phys, uint32_t dst_pitch, int dx, int dy,
                      uint64_t src_phys, uint32_t src_pitch, int sx, int sy, int w, int h) {
    SpinLockCpuGuard guard(g_gt_lock);
    if (!g.bopen || g.blen + XY_FAST_COPY_BLT_DW + 4 > BATCH_DWORDS) return false;
    if (!BlitRectOk(dst_pitch, dx, dy, w, h) || !BlitRectOk(src_pitch, sx, sy, w, h)) return false;
    emit_copy_t(dst_phys, dst_pitch, dx, dy, src_phys, src_pitch, sx, sy, w, h);
    g.bbytes += (uint64_t)w * 4u * (uint64_t)h;
    return true;
}

bool IntelGt::AddFill(uint64_t dst_phys, uint32_t dst_pitch, int x, int y, int w, int h, uint32_t color) {
    SpinLockCpuGuard guard(g_gt_lock);
    if (!g.bopen || g.blen + XY_FAST_COLOR_BLT_DW + 4 > BATCH_DWORDS) return false;
    if (!BlitRectOk(dst_pitch, x, y, w, h)) return false;
    emit_fill_t(dst_phys, dst_pitch, x, y, w, h, color);
    g.bbytes += (uint64_t)w * 4u * (uint64_t)h;
    return true;
}

uint32_t IntelGt::Submit() {
    SpinLockCpuGuard guard(g_gt_lock);
    return submit_locked();
}

int IntelGt::Poll(uint32_t seq) {
    SpinLockCpuGuard guard(g_gt_lock);
    return poll_locked(seq);
}

bool IntelGt::Wait(uint32_t seq, uint32_t timeout_us) {
    SpinLockCpuGuard guard(g_gt_lock);
    return wait_locked(seq, timeout_us);
}

void IntelGt::Off(const char* why) {
    SpinLockCpuGuard guard(g_gt_lock);
    if (!g.up || g.dead) return;
    begin_out(nullptr, 0);
    if (g.inflight) wait_locked(g.inflight, JOB_TIMEOUT_US);
    if (g.dead) return;
    g.up = g.blit_ok = false;
    g.dead = true;
    if (!g.sim && g_mmio) {
        wr(BASE + RING_MI_MODE, MaskedEnable(STOP_RING));
        fw_put(R_FW_RENDER);
        fw_put(R_FW_GT);
        g.fw_render = g.fw_gt = false;
    }
    release_memory();
    Ln l; l.t.s("off ("); l.t.s(why ? why : "requested"); l.t.s("): ring stopped, forcewake released, ggtt entries restored"); l.go();
}

int IntelGt::Status(char* out, int maxo) {
    begin_out(out, maxo);
    Ln l; l.t.s("gt: "); l.t.s(g.dead ? "off" : (g.up ? "up" : "not started"));
    if (g.sim) l.t.s(" (software model)");
    l.t.s(", blitter "); l.t.s(g.blit_ok ? "verified" : "not enabled");
    l.t.s(", pat/mocs "); l.t.s(g.pat_ok ? "programmed" : "firmware defaults");
    l.t.s(", coherent read "); l.t.d(g.coherent_read); l.t.s(", jobs "); l.t.d((uint64_t)g.total_jobs);
    l.t.s(", last job "); l.t.d((uint64_t)g.last_job_us); l.t.s(" us"); l.go();
    int n = g_out_pos;
    begin_out(nullptr, 0);
    return n;
}

int IntelGt::SelfTest(char* out, int maxo) {
    SpinLockCpuGuard guard(g_gt_lock);
    begin_out(out, maxo);
    if (!g.up || g.dead) emit("gt is not up (boot with kurono.igpu.gt=1 kurono.igpu.blt=1)");
    else if (g.inflight) emit("a job is in flight, try again");
    else blit_ladder();
    int n = g_out_pos;
    begin_out(nullptr, 0);
    return n;
}

void IntelGt::TakeStats(uint32_t* jobs, uint64_t* kb, uint64_t* job_us, uint32_t* fails) {
    SpinLockCpuGuard guard(g_gt_lock);
    if (jobs) *jobs = g.st_jobs;
    if (kb) *kb = g.st_bytes / 1024u;
    if (job_us) *job_us = g.st_us;
    if (fails) *fails = g.st_fails;
    g.st_jobs = 0;
    g.st_bytes = g.st_us = 0;
}
// end (satoru)
