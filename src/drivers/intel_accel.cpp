// intel display acceleration, stage 3a: hardware cursor plane on display
// version 14 (meteor lake / arrow lake). see intel_accel.h for the contract
// and scratchpad/oc_gpuaccel_design.md for the analysis and the later stages.
//
// linux sources (torvalds/linux v6.12, drivers/gpu/drm/i915/display/ unless
// noted), cited next to the steps below: (satoru)
//  - intel_cursor_regs.h: CURCNTR 0x70080, CURBASE 0x70084, CURPOS 0x70088,
//    CUR_FBC_CTL 0x700a0, CUR_SURFLIVE 0x700ac, CUR_WM 0x70140 + 4n,
//    CUR_WM_TRANS 0x70168, CUR_WM_SAGV 0x70158, CUR_WM_SAGV_TRANS 0x7015c,
//    CUR_BUF_CFG 0x7017c, MCURSOR_MODE_64_ARGB_AX 0x27. (satoru)
//  - intel_cursor.c: i9xx_cursor_update_arm (write fbc_ctl, cntr, pos, base;
//    base arms), skl_write_cursor_wm, i9xx_cursor_size_ok (64/128/256 argb,
//    square unless CUR_FBC_CTL), intel_cursor_position. (satoru)
//  - skl_watermark.c: skl_cursor_allocation (max(32, min_ddb) for one active
//    pipe), skl_compute_plane_wm, skl_crtc_allocate_plane_ddb (a level that
//    does not fit is disabled), skl_write_plane_wm. (satoru)
//  - skl_universal_plane_regs.h: PLANE_CTL 0x70180, PLANE_STRIDE 0x70188,
//    PLANE_POS 0x7018c, PLANE_SIZE 0x70190, PLANE_SURF 0x7019c,
//    PLANE_SURFLIVE 0x701ac, PLANE_WM 0x70240, PLANE_WM_TRANS 0x70268,
//    PLANE_WM_SAGV 0x70258, PLANE_WM_SAGV_TRANS 0x7025c,
//    PLANE_BUF_CFG 0x7027c. (satoru)
//  - skl_universal_plane.c: icl_plane_update_arm (plane_surf write arms a
//    double buffered plane update). (satoru)
//  - gt/intel_ggtt.c / intel_gtt.h: GEN6_GSMBASE 0x108100, GEN11_BDSM_MASK
//    63:20, MTL_PCODE_STOLEN_ACCESS 0x138914, mtl_ggtt_pte_encode (present
//    bit 0, lm bit 1, addr 45:12, pat 52:53). (satoru)
//  - i915_reg.h: GEN6_GSMBASE, MTL_PCODE_STOLEN_ACCESS. (satoru)
//  - intel_psr_regs.h: EDP_PSR_ENABLE 0x60800 bit 31, EDP_PSR2_ENABLE
//    0x60900 bit 31 (a hardware cursor update can be swallowed by psr). (satoru)
#include "intel_accel.h"
#include "../kernel/kimage.h"   // scanout target may be a direct-map buffer (satoru)
#include "intel_display_probe.h"
#include "intel_modeset.h"
#include "intel_gt.h"
#include "intel_modeset_calc.h"
#include "dp_edid_decode.h"
#include "outputs.h"
#include "graphics.h"
#include "serial.h"
#include "cpu_detect.h"
#include "disklog.h"
#include "../kernel/panic.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../proc/spinlock.h"
#include "../proc/smp.h"

namespace {

using DpEdid::Txt;

// ---- shared registers (satoru) ----
constexpr uint32_t R_PWR_WELL_CTL2 = 0x45404;   // state bit 2*idx, PW_1 idx 0, PW_2 idx 1 (satoru)
constexpr uint32_t R_DC_STATE_EN = 0x45504;
constexpr uint32_t R_DISP_ARB_CTL2 = 0x45004;   // disp_ipc_enable bit 3 (satoru)
constexpr uint32_t R_MTL_LATENCY[3] = { 0x45780, 0x45784, 0x45788 };
constexpr uint32_t R_MTL_LATENCY_SAGV = 0x4578c;
constexpr uint32_t R_STOLEN_ACCESS = 0x138914;  // == 1: direct gsm access allowed (satoru)
constexpr uint32_t R_GSMBASE = 0x108100;        // 64 bit, gen11_bdsm_mask 63:20 (satoru)
constexpr uint32_t R_SRD_CTL_A = 0x60800, R_PSR2_CTL_A = 0x60900;
constexpr uint32_t R_PP_STATUS0 = 0xc7200;

// ---- per pipe (pipe offset 0x1000, transcoder offset 0x1000) (satoru) ----
constexpr uint32_t P_DCY = 0x70000;              // pipedsl: scanline in bits 19:0 (satoru)
constexpr uint32_t P_FRMCNT = 0x70040;           // PIPE_FRMCNT_G4X: frames since the pipe came up (satoru)
constexpr uint32_t P_FLIPCNT = 0x70044;          // PIPE_FLIPCOUNT_G4X: primary plane flips latched (satoru)
constexpr uint32_t R_TIMESTAMP_CTR = 0x44070;    // IVB_TIMESTAMP_CTR: free running microsecond counter (satoru)
constexpr uint32_t P_TRANSCONF = 0x70008;
constexpr uint32_t P_CUR_CTL = 0x70080, P_CUR_BASE = 0x70084, P_CUR_POS = 0x70088;
constexpr uint32_t P_CUR_FBC = 0x700a0, P_CUR_SURFLIVE = 0x700ac;
constexpr uint32_t P_CUR_WM0 = 0x70140, P_CUR_WM_SAGV = 0x70158, P_CUR_WM_SAGV_TRANS = 0x7015c;
constexpr uint32_t P_CUR_WM_TRANS = 0x70168, P_CUR_BUF_CFG = 0x7017c;
constexpr uint32_t P_PL_CTL = 0x70180, P_PL_STRIDE = 0x70188, P_PL_POS = 0x7018c;
constexpr uint32_t P_PL_SIZE = 0x70190, P_PL_SURF = 0x7019c, P_PL_OFFSET = 0x701a4;
constexpr uint32_t P_PL_SURFLIVE = 0x701ac;
constexpr uint32_t P_PL_WM0 = 0x70240, P_PL_WM_SAGV = 0x70258, P_PL_WM_SAGV_TRANS = 0x7025c;
constexpr uint32_t P_PL_WM_TRANS = 0x70268, P_PL_BUF_CFG = 0x7027c;
constexpr uint32_t P_PL_BUF_CFG_STRIDE = 0x100;  // plane 2..5 buf cfg + 0x100 each (satoru)
constexpr uint32_t T_HTOTAL = 0x60000, T_LINK_M1 = 0x60040, T_LINK_N1 = 0x60044;
constexpr uint32_t T_DDI_FUNC = 0x60400;
constexpr uint32_t R_DDI_CLK_VALFREQ = 0x64030;  // + port * 0x100 (satoru)

constexpr uint32_t TRANSCONF_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_FORMAT_MASK = 0xFu << 24;
constexpr uint32_t PLANE_CTL_TILING_MASK = 7u << 10;
constexpr uint32_t MCURSOR_MODE_64_ARGB = 0x27;   // argb 64x64 (satoru)
constexpr uint32_t CURSOR_POS_Y_MASK = 0x7FFFu << 16;
constexpr uint32_t PLANE_WM_EN = 1u << 31;
constexpr uint32_t PLANE_WM_LINES_MASK = 0x1FFFu << 14;
constexpr uint32_t PLANE_WM_BLOCKS_MASK = 0x1FFFu;
constexpr uint32_t CUR_WM_BLOCKS_MASK = 0xFFFu;

constexpr uint32_t PIPE_N = 2;
constexpr uint32_t CURSOR_W = 64, CURSOR_H = 64;
constexpr uint32_t CURSOR_BYTES = CURSOR_W * CURSOR_H * 4;
constexpr uint32_t CURSOR_PAGES = (CURSOR_BYTES + 0xFFFu) / 0x1000u;
constexpr uint32_t SLOT_SLACK = 0x1000;          // leave the page after the fb untouched (satoru)
constexpr uint32_t DDB_BANK = 2048;              // blocks per pipe bank on mtl (satoru)
constexpr uint32_t CURSOR_MIN_BLOCKS = 32;       // skl_cursor_allocation, one active pipe (satoru)
constexpr uint64_t PTE_ADDR = 0x00003FFFFFFFF000ull;
constexpr uint64_t GSM_MASK = 0xFFFFFFFFFFFFF000ull & ~0xFFFFFull;  // bits 63:20 (satoru)

// ---- logging (satoru) ----
char* g_out = nullptr;
int g_out_max = 0, g_out_pos = 0;

void begin_out(char* out, int maxo) {
    g_out = out; g_out_max = maxo; g_out_pos = 0;
    if (out && maxo > 0) out[0] = 0;
}

void emit(const char* line) {
    SerialLogger::Log("[igpu-accel] ");
    SerialLogger::Log(line);
    SerialLogger::Log("\r\n");
    if (g_out && g_out_pos < g_out_max - 2) {
        static const char pre[] = "[igpu-accel] ";
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

// ---- tsc clock, same shape as stage 1/2 (satoru) ----
uint64_t g_tpu = 0;
bool g_clock_init = false;

inline uint64_t tsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void clock_init() {
    if (g_clock_init) return;
    g_clock_init = true;
    CpuInfo ci = CPUDetect::GetInfo();
    uint64_t f = ci.frequency.tsc_frequency;
    if (!f && ci.frequency.base_mhz > 0) f = (uint64_t)ci.frequency.base_mhz * 1000000ull;
    uint64_t t = f / 1000000ull;
    g_tpu = (t >= 100 && t <= 10000) ? t : 0;
}

void udelay(uint32_t us) {
    if (g_tpu) {
        uint64_t end = tsc() + (uint64_t)us * g_tpu;
        for (uint64_t guard = (uint64_t)us * 4096 + 64; guard && tsc() < end; guard--)
            __asm__ __volatile__("pause");
    } else {
        for (uint32_t i = 0; i < us; i++) __asm__ __volatile__("outb %%al, $0x80" : : "a"((uint8_t)0));
    }
}

// ---- cpu-safe elapsed time (satoru) ----
// rdtsc values are only comparable on the cpu that took them. the laptop's
// firmware hands over with a different IA32_TSC_ADJUST per cpu (linux logs
// "[Firmware Bug]: TSC ADJUST: CPU0: -1797404919 force to 0" there on every
// boot), so the bsp's rdtsc was 0.6 to 0.9 s behind the aps'. the page flip
// latch timeout did tsc() - pending_tsc with the arm taken on the boot
// animation's ap and the check made by the gui on the bsp: the difference
// wrapped, and a flip a few ms old was declared "not latched within 250 ms".
// the kernel now zeroes the msr at boot (SMP::TscAdjustBoot), but nothing
// below depends on that. a Lapse measures one interval and only ever adds the
// difference of two rdtsc values taken on the same cpu by consecutive looks;
// a look from another cpu adds nothing and becomes the new reference. the
// result is a lower bound of the real time, exact while one cpu does the
// looking (the gui loop), and it can never run ahead. (satoru)
struct Lapse { uint32_t cpu; uint64_t tsc; uint64_t us; };

inline void cpu_tsc_now(uint32_t* cpu, uint64_t* t) {
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) :: "memory");   // the index and the count belong together (satoru)
    *cpu = SMP::CpuIndex();
    *t = tsc();
    if (fl & 0x200ull) __asm__ __volatile__("sti" ::: "memory");
}

void lapse_start(Lapse& l) {
    cpu_tsc_now(&l.cpu, &l.tsc);
    l.us = 0;
}

uint64_t lapse_us(Lapse& l, uint32_t* cpu_out = nullptr) {
    uint32_t c; uint64_t t;
    cpu_tsc_now(&c, &t);
    uint64_t tpu = g_tpu ? g_tpu : 3000;
    if (c == l.cpu && t >= l.tsc) {
        uint64_t adv = (t - l.tsc) / tpu;
        l.us += adv;
        l.tsc += adv * tpu;          // keeps the sub-microsecond remainder (satoru)
    } else {
        l.cpu = c;
        l.tsc = t;
    }
    if (cpu_out) *cpu_out = c;
    return l.us;
}

// ---- mmio with the write allowlist (satoru) ----
volatile uint8_t* g_mmio = nullptr;
struct Range { uint32_t lo, hi; };
Range g_allow[32];
int g_nallow = 0;
int g_blocked = 0;
// register writes are logged only inside the self-test, enable and off paths
// (WrLog below). it used to be always on, so every runtime cursor move logged
// two lines: at a 1000 hz mouse that is thousands of log lines a second on
// the input path with the cursor lock held. (satoru)
bool g_log_writes = false;
struct WrLog {
    bool s;
    WrLog() : s(g_log_writes) { g_log_writes = true; }
    ~WrLog() { g_log_writes = s; }
};
uint32_t g_rt_writes = 0;       // silent runtime writes, for the status line (satoru)

void allow_reset() { g_nallow = 0; }
void allow_add(uint32_t lo, uint32_t hi) { if (g_nallow < 32) g_allow[g_nallow++] = Range{ lo, hi }; }
bool allowed(uint32_t off) {
    for (int i = 0; i < g_nallow; i++) if (off >= g_allow[i].lo && off <= g_allow[i].hi) return true;
    return false;
}

inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }

bool wr(uint32_t off, uint32_t v) {
    if (!allowed(off)) {
        g_blocked++;
        Ln l; l.t.s("BLOCKED write 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8);
        l.t.s(" (not on the cursor allowlist)"); l.go();
        return false;
    }
    uint32_t old = g_log_writes ? rd(off) : 0;
    *(volatile uint32_t*)(g_mmio + off) = v;
    __asm__ __volatile__("" ::: "memory");
    if (g_log_writes) {
        Ln l; l.t.s("wr 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8);
        l.t.s(" (was 0x"); l.t.x(old, 8); l.t.s(")"); l.go();
    } else {
        g_rt_writes++;
    }
    return true;
}

bool wait_reg(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t* last = nullptr) {
    uint64_t start = tsc();
    uint32_t v = rd(off);
    uint32_t polls = timeout_us + 16;
    while ((v & mask) != want && polls--) {
        if (g_tpu && (tsc() - start) / g_tpu > timeout_us) break;
        udelay(1);
        v = rd(off);
    }
    if (last) *last = v;
    return (v & mask) == want;
}

// ---- gsm / ggtt pte access, read and validate only (satoru) ----
struct Gsm {
    bool ok;
    bool direct;
    uint64_t base;
};
Gsm g_gsm = {};
bool g_gsm_pages[2048] = {};   // ggtt pte pages we mapped uncached (satoru)

bool gsm_open() {
    if (g_gsm.ok) return true;
    uint32_t acc = rd(R_STOLEN_ACCESS);
    uint64_t lo = (uint64_t)rd(R_GSMBASE), hi = (uint64_t)rd(R_GSMBASE + 4);
    uint64_t base = ((hi << 32) | lo) & GSM_MASK;
    g_gsm.direct = (acc == 1) && base != 0;
    // i915 direct_stolen_access else bar0 + 8 mb (gen6_gttadr_offset) (satoru)
    g_gsm.base = g_gsm.direct ? base : (uint64_t)(uintptr_t)g_mmio + (8ull << 20);
    g_gsm.ok = g_gsm.base != 0;
    {
        Ln l; l.t.s("gsm: stolen_access 0x"); l.t.x(acc, 8); l.t.s(" gsmbase 0x");
        l.t.x((uint32_t)(base >> 32), 8); l.t.x((uint32_t)base, 8);
        l.t.s(" -> ptes via "); l.t.s(g_gsm.direct ? "gsm (direct)" : "bar0 + 8 mb");
        l.t.s(" at 0x"); l.t.x((uint32_t)(g_gsm.base >> 32), 8); l.t.x((uint32_t)g_gsm.base, 8); l.go();
    }
    return g_gsm.ok;
}

bool gsm_map(uint32_t idx) {
    uint32_t pg = (uint32_t)(((uint64_t)idx * 8) / 4096);
    if (pg >= 2048) return false;
    if (g_gsm_pages[pg]) return true;
    uint64_t a = g_gsm.base + (uint64_t)pg * 4096;
    if (!KernelVMM::MapPage(a, a, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) return false;
    g_gsm_pages[pg] = true;
    return true;
}

uint64_t gsm_pte(uint32_t idx) {
    if (!g_gsm.ok || !gsm_map(idx)) return 0;
    return *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)idx * 8);
}

// ---- state (satoru) ----
struct Pipe {
    uint32_t id;
    bool lit;
    bool probed;            // surface and tail ptes validated (satoru)
    bool ready;             // the self-test passed and the pipe can be used
    bool verified;          // surflive latched during the self-test (satoru)
    bool failed;            // runtime failure, never try this pipe again (satoru)
    bool live;              // cur_ctl currently enabled (satoru)
    bool saved;             // cursor/ddb registers saved for restore (satoru)
    bool pl_saved;          // primary plane regs saved because we carved (satoru)
    uint32_t ggtt;          // plane surface ggtt offset (satoru)
    uint64_t phys;          // cpu alias of that surface (satoru)
    uint32_t stride, w, h;
    uint64_t vis_bytes;
    uint32_t slot_ggtt;     // cursor image ggtt offset (satoru)
    uint64_t slot_phys;     // cpu alias of the cursor image (satoru)
    bool own_ptes;          // slot backed by a pte window we wrote (satoru)
    uint64_t own_phys;      // pmm pages behind that window (satoru)
    uint32_t own_frames;
    uint64_t saved_pte[CURSOR_PAGES + 1];
    uint32_t pte_idx;       // first pte index of that window (satoru)
    uint32_t cur_ddb_start, cur_ddb_end;
    uint32_t pixclk_khz, htotal;
    // saved cursor registers (satoru)
    uint32_t s_ctl, s_base, s_pos, s_fbc;
    uint32_t s_wm[6], s_wm_trans, s_wm_sagv, s_wm_sagv_trans, s_buf;
    // saved primary plane registers when carved (satoru)
    uint32_t s_pl_wm[6], s_pl_wm_trans, s_pl_wm_sagv, s_pl_wm_sagv_trans, s_pl_buf, s_pl_surf;
    int cur_x, cur_y;
};

Pipe g_pipes[PIPE_N];
bool g_enabled = false;
bool g_boot_once = false;
uint64_t g_fb_phys = 0;
uint32_t g_fb_w = 0, g_fb_h = 0, g_fb_pitch = 0;
bool g_pte_fallback = false;
bool g_wm_calc = false;          // kurono.igpu.accel.wmcalc=1: old computed primary wm (satoru)
Spinlock g_lock;
char g_last_err[96];
void set_last_error(const char* s) {
    int i = 0;
    if (!s) { g_last_err[0] = 0; return; }
    while (s[i] && i < 95) { g_last_err[i] = s[i]; i++; }
    g_last_err[i] = 0;
}
uint32_t g_hw_updates = 0, g_sw_fallbacks = 0, g_failures = 0;
uint64_t g_gui_cycles = 0;
bool g_visible = true;

// pipe a registers we never write, diffed to prove that (satoru)
struct ARef { const char* name; uint32_t off; };
const ARef PIPE_A_REGS[] = {
    { "transconf_a", 0x70008 }, { "plane_ctl_a1", 0x70180 }, { "plane_surf_a1", 0x7019c },
    { "plane_stride_a1", 0x70188 }, { "plane_buf_cfg_a1", 0x7027c }, { "plane_wm_a1_0", 0x70240 },
    { "cur_buf_cfg_a", 0x7017c }, { "dbox_a", 0x7003c }, { "trans_ddi_func_a", 0x60400 },
    { "trans_clk_sel_a", 0x46140 }, { "ddi_buf_ctl_a", 0x64000 }, { "port_clock_ctl_a", 0x640e0 },
    { "pp_status", 0xc7200 }, { "pp_control", 0xc7204 }, { "htotal_a", 0x60000 }, { "vtotal_a", 0x6000c },
    { "cdclk_ctl", 0x46000 }, { "wm_linetime_a", 0x45270 }, { "gamma_mode_a", 0x4a480 },
};
constexpr int N_PIPE_A_REGS = (int)(sizeof(PIPE_A_REGS) / sizeof(PIPE_A_REGS[0]));
uint32_t g_pipe_a_before[N_PIPE_A_REGS];

void snapshot_pipe_a(uint32_t* v) { for (int i = 0; i < N_PIPE_A_REGS; i++) v[i] = rd(PIPE_A_REGS[i].off); }

void diff_pipe_a(const char* when, bool allow_ddb, int* out_of_spec) {
    uint32_t now[N_PIPE_A_REGS];
    snapshot_pipe_a(now);
    int diffs = 0;
    for (int i = 0; i < N_PIPE_A_REGS; i++) {
        if (now[i] == g_pipe_a_before[i]) continue;
        // the cursor ddb/wm carve deliberately rewrites pipe a's primary ddb
        // and watermarks; they are logged but are not an out-of-spec change.
        // cur_buf_cfg is the cursor's own ddb, always ours to set. (satoru)
        bool expected = allow_ddb &&
                        (PIPE_A_REGS[i].off == 0x7027c || PIPE_A_REGS[i].off == 0x70240 ||
                         PIPE_A_REGS[i].off == 0x7017c);
        if (!expected) diffs++;
        Ln l; l.t.s(expected ? "pipe a expected change (" : "PIPE A CHANGED (");
        l.t.s(when); l.t.s("): "); l.t.s(PIPE_A_REGS[i].name);
        l.t.s(" 0x"); l.t.x(g_pipe_a_before[i], 8); l.t.s(" -> 0x"); l.t.x(now[i], 8); l.go();
    }
    Ln l; l.t.s("pipe a check ("); l.t.s(when); l.t.s("): "); l.t.d((uint64_t)N_PIPE_A_REGS);
    l.t.s(" registers, ");
    if (diffs) { l.t.d((uint64_t)diffs); l.t.s(" changed"); } else l.t.s("all unchanged");
    l.go();
    if (out_of_spec) *out_of_spec = diffs;
}

void allow_pipe(uint32_t p) {
    uint32_t b = 0x70000 + p * 0x1000;
    allow_add(b + 0x080, b + 0x088);            // cur ctl/base/pos (satoru)
    allow_add(b + 0x0a0, b + 0x0a4);            // cur fbc, chicken (satoru)
    allow_add(b + 0x0ac, b + 0x0ac);            // cur surflive (satoru)
    allow_add(b + 0x140, b + 0x17c);            // cur wm0..5/trans/sagv/buf_cfg (satoru)
    allow_add(b + 0x180, b + 0x19c);            // primary plane, ddb carve + zero-copy (satoru)
    allow_add(b + 0x1a0, b + 0x1a4);            // keymax + plane_offset, zero-copy slice (satoru)
    allow_add(b + 0x1ac, b + 0x1ac);            // plane surflive (satoru)
    allow_add(b + 0x240, b + 0x27c);            // plane wm0..5/trans/sagv/buf_cfg (satoru)
}

// ---- cursor image (satoru) ----
uint32_t g_cursor_img[CURSOR_W * CURSOR_H];

void build_cursor_image(bool checker) {
    static const uint8_t bitmap[16][12] = {
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,1,1,1,1,0,0},
        {1,2,2,1,2,2,1,0,0,0,0,0},
        {1,2,1,0,1,2,2,1,0,0,0,0},
        {1,1,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,0,1,1,0,0,0,0},
    };
    for (uint32_t i = 0; i < CURSOR_W * CURSOR_H; i++) g_cursor_img[i] = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 12; x++) {
            uint8_t p = bitmap[y][x];
            if (p == 1) g_cursor_img[y * CURSOR_W + x] = 0xFF000000u;
            else if (p == 2) g_cursor_img[y * CURSOR_W + x] = 0xFFFFFFFFu;
        }
    if (checker) {
        // visible self-test pattern in the top right of the fetched square,
        // so a user can confirm the plane really is scanning our buffer (satoru)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                g_cursor_img[y * CURSOR_W + 32 + x] =
                    ((x ^ y) & 1) ? 0xFFFF00FFu : 0xFF00FFFFu;
    }
}

bool write_cursor_image(Pipe* p, bool checker) {
    build_cursor_image(checker);
    // map the slot pages uncached before writing: the fb tail may be wc
    // mapped by graphics or not mapped at all (satoru)
    for (uint32_t i = 0; i < CURSOR_PAGES + 1; i++) {
        uint64_t a = p->slot_phys + (uint64_t)i * 4096;
        if (!KernelVMM::MapPage(a, a, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) {
            Ln l; l.t.s("cursor image: map failed at 0x"); l.t.x((uint32_t)(a >> 32), 8);
            l.t.x((uint32_t)a, 8); l.go();
            return false;
        }
    }
    volatile uint32_t* dst = (volatile uint32_t*)(uintptr_t)p->slot_phys;
    for (uint32_t i = 0; i < CURSOR_W * CURSOR_H; i++) dst[i] = g_cursor_img[i];
    for (uint32_t o = 0; o < CURSOR_BYTES; o += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"((uint8_t*)(uintptr_t)p->slot_phys + o) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
    // read back a few words through the same alias (satoru)
    bool ok = dst[0] == g_cursor_img[0] && dst[CURSOR_W * CURSOR_H - 1] == g_cursor_img[CURSOR_W * CURSOR_H - 1];
    Ln l; l.t.s("cursor image: 0x"); l.t.x((uint32_t)(p->slot_phys >> 32), 8); l.t.x((uint32_t)p->slot_phys, 8);
    l.t.s(" ggtt 0x"); l.t.x(p->slot_ggtt, 8); l.t.s(ok ? " written" : " READ BACK MISMATCH"); l.go();
    return ok;
}

// ---- watermarks (satoru) ----
void read_latencies(uint16_t lat[6], uint16_t* sagv_block, bool* ipc) {
    uint16_t raw[6];
    for (int i = 0; i < 3; i++) {
        uint32_t v = rd(R_MTL_LATENCY[i]);
        raw[i * 2] = (uint16_t)(v & 0x1FFF);
        raw[i * 2 + 1] = (uint16_t)((v >> 16) & 0x1FFF);
    }
    for (int i = 0; i < 6; i++) lat[i] = raw[i];
    IgpuCalc::AdjustLatency(lat);
    *sagv_block = (uint16_t)(rd(R_MTL_LATENCY_SAGV) & 0x1FFF);
    *ipc = (rd(R_DISP_ARB_CTL2) & (1u << 3)) != 0;
}

struct WmSet {
    IgpuCalc::WmLevel wm[6], trans, sagv, sagv_trans;
};

bool compute_wms(uint32_t pixclk_khz, uint32_t htotal, uint32_t width, uint32_t ddb_size, WmSet* out) {
    uint16_t lat[6]; uint16_t sagv_block = 0; bool ipc = false;
    read_latencies(lat, &sagv_block, &ipc);
    IgpuCalc::WmParams wp;
    IgpuCalc::WmParamsLinear(pixclk_khz, htotal, width, 4, &wp);
    IgpuCalc::WmLevel prev = { false, 0, 0, 0 };
    for (int lv = 0; lv < 6; lv++) {
        IgpuCalc::PlaneWm(wp, lat[lv], lv == 0 ? IgpuCalc::WmLevel{ false, 0, 0, 0 } : prev, &out->wm[lv]);
        prev = out->wm[lv];
        if (out->wm[lv].min_ddb > ddb_size) {
            // skl_crtc_allocate_plane_ddb: a level that does not fit is off,
            // and its regs copy the level below (skl_need_wm_copy_wa) (satoru)
            IgpuCalc::WmLevel off = { false, 0, 0, 0 };
            if (lv > 0) { off.blocks = out->wm[lv - 1].blocks; off.lines = out->wm[lv - 1].lines; }
            out->wm[lv] = off;
        }
    }
    IgpuCalc::TransWm(out->wm[0], &out->trans);
    if (!ipc || out->trans.min_ddb > ddb_size) out->trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    IgpuCalc::PlaneWm(wp, sagv_block ? sagv_block + lat[0] : 0, out->wm[0], &out->sagv);
    if (out->sagv.min_ddb > ddb_size) out->sagv = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    IgpuCalc::TransWm(out->sagv, &out->sagv_trans);
    if (!ipc || out->sagv_trans.min_ddb > ddb_size) out->sagv_trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    return out->wm[0].enable;
}

uint32_t cursor_wm_reg_val(const IgpuCalc::WmLevel& l) {
    uint32_t v = 0;
    if (l.enable) v |= PLANE_WM_EN;
    v |= ((uint32_t)l.lines & 0x1FFF) << 14;
    v |= (uint32_t)l.blocks & CUR_WM_BLOCKS_MASK;
    return v;
}

void write_cursor_wms(uint32_t b, const WmSet& w) {
    for (int lv = 0; lv < 6; lv++) wr(b + P_CUR_WM0 - 0x70000 + 4u * (uint32_t)lv, cursor_wm_reg_val(w.wm[lv]));
    wr(b + P_CUR_WM_TRANS - 0x70000, cursor_wm_reg_val(w.trans));
    wr(b + P_CUR_WM_SAGV - 0x70000, cursor_wm_reg_val(w.sagv));
    wr(b + P_CUR_WM_SAGV_TRANS - 0x70000, cursor_wm_reg_val(w.sagv_trans));
}

void write_plane_wms(uint32_t b, const WmSet& w) {
    for (int lv = 0; lv < 6; lv++) wr(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv, IgpuCalc::WmRegVal(w.wm[lv]));
    wr(b + P_PL_WM_TRANS - 0x70000, IgpuCalc::WmRegVal(w.trans));
    wr(b + P_PL_WM_SAGV - 0x70000, IgpuCalc::WmRegVal(w.sagv));
    wr(b + P_PL_WM_SAGV_TRANS - 0x70000, IgpuCalc::WmRegVal(w.sagv_trans));
}

// ---- pipe discovery and validation (satoru) ----
bool pipe_lit(uint32_t id) {
    uint32_t b = 0x70000 + id * 0x1000;
    if (!(rd(b + 0x008) & TRANSCONF_ENABLE)) return false;
    return (rd(b + 0x180) & PLANE_CTL_ENABLE) != 0;
}

bool pte_ok(uint64_t pte) {
    return (pte & 1) != 0;   // present (satoru)
}

void restore_ptes(Pipe* p);   // forward, probe_surface rolls back its own window (satoru)

// validate the surface pte against the known cpu framebuffer address, then
// walk the tail ptes and prove they are present, contiguous and the same kind
// of mapping. fills slot_ggtt/slot_phys on success. (satoru)
bool probe_surface(Pipe* p, uint64_t hint_phys) {
    if (!p->ggtt) return false;
    uint32_t idx = p->ggtt >> 12;
    uint64_t pte0 = gsm_pte(idx);
    if (!pte_ok(pte0)) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": surface pte not present (0x");
        l.t.x((uint32_t)(pte0 >> 32), 8); l.t.x((uint32_t)pte0, 8); l.t.s("), refusing"); l.go();
        return false;
    }
    bool lm = (pte0 & 2) != 0;
    uint64_t want = 0;
    if (lm) {
        if (!g_gsm.ok || hint_phys < g_gsm.base) {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": lm pte but gsm base/alias unknown, refusing"); l.go();
            return false;
        }
        want = hint_phys - g_gsm.base;
    } else {
        want = hint_phys;
    }
    if ((pte0 & PTE_ADDR) != want) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": pte 0x"); l.t.x((uint32_t)(pte0 >> 32), 8);
        l.t.x((uint32_t)pte0, 8); l.t.s(" does not match fb 0x"); l.t.x((uint32_t)(hint_phys >> 32), 8);
        l.t.x((uint32_t)hint_phys, 8); l.t.s(" (gsm 0x"); l.t.x((uint32_t)g_gsm.base, 8);
        l.t.s("), refusing"); l.go();
        return false;
    }
    // the cursor image goes after the visible bytes, one page of slack, and
    // needs CURSOR_PAGES + 1 contiguous ptes (one extra for prefetch) (satoru)
    uint64_t tail = (p->vis_bytes + 0xFFFu) & ~0xFFFull;
    uint64_t slot_off = tail + SLOT_SLACK;
    uint32_t first = (uint32_t)(slot_off >> 12);
    uint64_t base_addr = pte0 & PTE_ADDR;
    bool tail_ok = true;
    for (uint32_t i = 0; i <= CURSOR_PAGES; i++) {
        uint64_t pte = gsm_pte(idx + first + i);
        if (!pte_ok(pte) || (pte & 2) != (lm ? 2ull : 0ull) || (pte & PTE_ADDR) != base_addr + (uint64_t)(first + i) * 0x1000) {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": tail pte "); l.t.d((uint64_t)i);
            l.t.s(" not usable (0x"); l.t.x((uint32_t)(pte >> 32), 8); l.t.x((uint32_t)pte, 8); l.t.s(")"); l.go();
            tail_ok = false;
            break;
        }
    }
    p->slot_ggtt = p->ggtt + (uint32_t)slot_off;
    if (tail_ok) {
        // contiguous ptes prove the cpu alias continues at the same offset (satoru)
        p->slot_phys = hint_phys + slot_off;
    } else {
        // safe fallback: back the cursor slot with our own pmm pages and a 5
        // entry pte window in the padding of the same ggtt window. it lives
        // past the visible framebuffer, inside the window the pipe already
        // scans, and every old pte is saved and restored on off. enabled by
        // default: a gop framebuffer frequently maps only its visible bytes,
        // which used to reject the whole pipe and leave the software cursor
        // in charge. kurono.igpu.accel.noptefb=1 turns it off. (satoru)
        if (!g_pte_fallback) {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id));
            l.t.s(": firmware did not map the fb tail and the pte fallback is off (kurono.igpu.accel.noptefb=1)"); l.go();
            return false;
        }
        uint32_t frames = CURSOR_PAGES + 1;
        uint64_t own = PMM::AllocContiguous(frames);
        if (!own) {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": no contiguous memory for the cursor pte window, refusing"); l.go();
            return false;
        }
        p->own_phys = own;
        p->own_frames = frames;
        p->pte_idx = idx + first;
        for (uint32_t i = 0; i <= CURSOR_PAGES; i++) {
            uint32_t pi = idx + first + i;
            if (!gsm_map(pi)) { PMM::FreeContiguous(own, frames); p->own_phys = 0; return false; }
            volatile uint64_t* slot = (volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)pi * 8);
            p->saved_pte[i] = *slot;
            *slot = IgpuCalc::MtlGgttPte(own + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC);
        }
        __asm__ __volatile__("mfence" ::: "memory");
        bool rb = true;
        for (uint32_t i = 0; i <= CURSOR_PAGES; i++) {
            volatile uint64_t* slot = (volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + first + i) * 8);
            if (*slot != IgpuCalc::MtlGgttPte(own + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC)) rb = false;
        }
        p->own_ptes = true;
        p->slot_phys = own;
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor pte window "); l.t.d((uint64_t)(CURSOR_PAGES + 1));
        l.t.s(" entries at ggtt 0x"); l.t.x(p->slot_ggtt, 8); l.t.s(" pmm 0x");
        l.t.x((uint32_t)(own >> 32), 8); l.t.x((uint32_t)own, 8);
        l.t.s(rb ? " (read back ok)" : " (READ BACK MISMATCH)"); l.go();
        if (!rb) { restore_ptes(p); return false; }
    }
    Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": fb ggtt 0x"); l.t.x(p->ggtt, 8);
    l.t.s(" phys 0x"); l.t.x((uint32_t)(hint_phys >> 32), 8); l.t.x((uint32_t)hint_phys, 8);
    l.t.s(" "); l.t.d((uint64_t)p->w); l.t.c('x'); l.t.d((uint64_t)p->h);
    l.t.s(" stride "); l.t.d((uint64_t)p->stride); l.t.s(" | cursor slot ggtt 0x"); l.t.x(p->slot_ggtt, 8);
    l.t.s(" phys 0x"); l.t.x((uint32_t)(p->slot_phys >> 32), 8); l.t.x((uint32_t)p->slot_phys, 8);
    l.t.s(lm ? " (lm)" : " (system)"); l.t.s(p->own_ptes ? " (own ptes)" : ""); l.go();
    return true;
}

void restore_ptes(Pipe* p) {
    if (!p->own_ptes) return;
    for (uint32_t i = 0; i <= CURSOR_PAGES; i++) {
        volatile uint64_t* slot = (volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(p->pte_idx + i) * 8);
        *slot = p->saved_pte[i];
    }
    __asm__ __volatile__("mfence" ::: "memory");
    if (p->own_phys && p->own_frames) PMM::FreeContiguous(p->own_phys, p->own_frames);
    p->own_phys = 0; p->own_frames = 0; p->own_ptes = false;
    Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor pte window restored"); l.go();
}

bool probe_pipe(uint32_t id, uint64_t hint_phys) {
    Pipe* p = &g_pipes[id];
    restore_ptes(p);
    p->id = id;
    p->lit = false; p->probed = false; p->ready = false; p->verified = false; p->failed = false;
    p->live = false; p->saved = false; p->pl_saved = false;
    p->ggtt = 0; p->phys = 0; p->stride = p->w = p->h = 0; p->vis_bytes = 0;
    p->slot_ggtt = 0; p->slot_phys = 0;
    p->own_ptes = false; p->own_phys = 0; p->own_frames = 0; p->pte_idx = 0;
    if (!pipe_lit(id)) return false;
    p->lit = true;
    uint32_t b = 0x70000 + id * 0x1000, tb = 0x60000 + id * 0x1000;
    uint32_t ctl = rd(b + 0x180);
    if (ctl & PLANE_CTL_TILING_MASK) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": plane is tiled, refusing"); l.go();
        return false;
    }
    (void)PLANE_CTL_FORMAT_MASK;
    if (rd(b + 0x1a4) != 0) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": plane offset is not 0, refusing"); l.go();
        return false;
    }
    uint32_t st = rd(b + 0x188) & 0xFFF;   // plane_stride mask 11:0 (satoru)
    p->stride = st * 64;
    uint32_t sz = rd(b + 0x190);
    p->w = (sz & 0xFFFF) + 1;
    p->h = ((sz >> 16) & 0xFFFF) + 1;
    // plane_surf is armed from a 4k-aligned ggtt offset, so check the raw
    // register for the low bits before masking it: masking first made the
    // probe_surface alignment test impossible to pass (it always saw zero). (satoru)
    uint32_t surf = rd(b + 0x19c);
    if (surf & 0xFFFu) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": plane surf 0x");
        l.t.x(surf, 8); l.t.s(" is not 4k aligned, refusing"); l.go();
        return false;
    }
    p->ggtt = surf & 0xFFFFF000u;
    if (!p->stride || !p->w || !p->h) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": plane geometry is zero, refusing"); l.go();
        return false;
    }
    p->vis_bytes = (uint64_t)p->stride * p->h;
    // the cursor slot lives in the tail of the surface's 16 mb ggtt window,
    // so the surface must start on a 16 mb boundary and the visible bytes plus
    // the slot must fit inside that window. (satoru)
    if (p->ggtt & 0xFFFFFF) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": plane ggtt is not 16 mb aligned, refusing"); l.go();
        return false;
    }
    if (p->vis_bytes + SLOT_SLACK + (uint64_t)(CURSOR_PAGES + 1) * 4096 > 0x1000000ull) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": framebuffer leaves no tail for the cursor, refusing"); l.go();
        return false;
    }
    // pixel clock from the transcoder m/n and the ddi valfreq, the same
    // intel_dotclock_calculate the stage 0 probe decodes (satoru)
    uint32_t func = rd(tb + 0x400);
    uint32_t psel = (func >> 27) & 0xF;
    uint32_t lm = rd(tb + 0x40) & 0xFFFFFF;
    uint32_t ln = rd(tb + 0x44) & 0xFFFFFF;
    uint32_t valfreq = (psel >= 1 && psel <= 16) ? rd(R_DDI_CLK_VALFREQ + (psel - 1) * 0x100) : 0;
    p->htotal = (rd(tb + 0x00) >> 16) + 1;
    p->pixclk_khz = (lm && ln && valfreq) ? (uint32_t)((uint64_t)lm * valfreq / ln) : 0;
    if (!p->pixclk_khz || !p->htotal) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + id)); l.t.s(": no pixel clock, refusing"); l.go();
        return false;
    }
    if (!probe_surface(p, hint_phys)) return false;
    p->phys = hint_phys;
    p->lit = true;
    p->probed = true;
    return true;
}

// ---- cursor ddb (satoru) ----
// pick a cursor ddb range: an existing sane CUR_BUF_CFG, a free tail of the
// pipe bank, or (pipe a) 32 blocks carved off the end of the primary plane.
// returns true and fills start/end, or false. (satoru)
bool find_cursor_ddb(Pipe* p, uint32_t need, uint32_t* start, uint32_t* end, bool* carve) {
    uint32_t b = 0x70000 + p->id * 0x1000;
    uint32_t pcfg = rd(b + P_PL_BUF_CFG - 0x70000);
    uint32_t pstart = pcfg & 0xFFF;
    uint32_t pend = pcfg ? ((pcfg >> 16) & 0x1FFF) + 1 : 0;
    uint32_t ccfg = rd(b + P_CUR_BUF_CFG - 0x70000);
    uint32_t cstart = ccfg & 0xFFF;
    uint32_t cend = ccfg ? ((ccfg >> 16) & 0x1FFF) + 1 : 0;
    {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": ddb pl_cfg 0x");
        l.t.x(pcfg, 8); l.t.s(" cur_cfg 0x"); l.t.x(ccfg, 8); l.go();
    }
    // an enabled primary with no buf_cfg has not had a ddb range assigned at
    // all (some firmware leaves it zero and relies on the hardware default).
    // assume it owns the whole pipe bank so the carve below can still reserve
    // the cursor range and program both ranges consistently. (satoru)
    if (!pcfg) { pstart = 0; pend = DDB_BANK; }
    // planes 2..4, i.e. everything except the primary and the cursor (satoru)
    uint32_t used[4][2]; int nu = 0;
    auto add_used = [&](uint32_t s, uint32_t e) { if (e > s && nu < 4) { used[nu][0] = s; used[nu][1] = e; nu++; } };
    for (int pl = 1; pl < 4; pl++) {   // planes 2..4, plane 1 is the primary (satoru)
        uint32_t v = rd(b + P_PL_BUF_CFG - 0x70000 + (uint32_t)pl * P_PL_BUF_CFG_STRIDE);
        if (v) add_used(v & 0xFFF, ((v >> 16) & 0x1FFF) + 1);
    }
    auto overlaps_other = [&](uint32_t s, uint32_t e) {
        for (int i = 0; i < nu; i++) if (s < used[i][1] && e > used[i][0]) return true;
        return false;
    };
    auto overlaps_primary = [&](uint32_t s, uint32_t e) { return pend > pstart && s < pend && e > pstart; };
    auto overlaps_cursor = [&](uint32_t s, uint32_t e) { return cend > cstart && s < cend && e > cstart; };
    auto overlaps = [&](uint32_t s, uint32_t e) {
        return overlaps_other(s, e) || overlaps_primary(s, e) || overlaps_cursor(s, e);
    };
    if (cend > cstart && (cend - cstart) >= need && !overlaps_primary(cstart, cend) && !overlaps_other(cstart, cend)) {
        *start = cstart; *end = cend; *carve = false;
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor ddb already reserved ");
        l.t.d((uint64_t)cstart); l.t.c('-'); l.t.d((uint64_t)cend); l.go();
        return true;
    }
    if (pend && pend < DDB_BANK && pend + need <= DDB_BANK && !overlaps(pend, pend + need)) {
        *start = pend; *end = pend + need; *carve = false;
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor ddb from the free tail ");
        l.t.d((uint64_t)pend); l.t.c('-'); l.t.d((uint64_t)(pend + need)); l.go();
        return true;
    }
    // carve off the primary's end. the carved range is inside the primary, so
    // only the other planes and the existing cursor range can conflict. (satoru)
    if (!pend || pend <= pstart + 64 || pend < need ||
        overlaps_other(pend - need, pend) || overlaps_cursor(pend - need, pend)) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": no room for a cursor ddb, refusing"); l.go();
        return false;
    }
    *start = pend - need; *end = pend; *carve = true;
    Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor ddb carved from the primary plane ");
    l.t.d((uint64_t)*start); l.t.c('-'); l.t.d((uint64_t)*end); l.go();
    return true;
}

void save_plane_regs(Pipe* p) {
    uint32_t b = 0x70000 + p->id * 0x1000;
    for (int lv = 0; lv < 6; lv++) p->s_pl_wm[lv] = rd(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv);
    p->s_pl_wm_trans = rd(b + P_PL_WM_TRANS - 0x70000);
    p->s_pl_wm_sagv = rd(b + P_PL_WM_SAGV - 0x70000);
    p->s_pl_wm_sagv_trans = rd(b + P_PL_WM_SAGV_TRANS - 0x70000);
    p->s_pl_buf = rd(b + P_PL_BUF_CFG - 0x70000);
    p->s_pl_surf = rd(b + P_PL_SURF - 0x70000);
    p->pl_saved = true;
}

void restore_plane_regs(Pipe* p) {
    if (!p->pl_saved) return;
    uint32_t b = 0x70000 + p->id * 0x1000;
    for (int lv = 0; lv < 6; lv++) wr(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv, p->s_pl_wm[lv]);
    wr(b + P_PL_WM_TRANS - 0x70000, p->s_pl_wm_trans);
    wr(b + P_PL_WM_SAGV - 0x70000, p->s_pl_wm_sagv);
    wr(b + P_PL_WM_SAGV_TRANS - 0x70000, p->s_pl_wm_sagv_trans);
    wr(b + P_PL_BUF_CFG - 0x70000, p->s_pl_buf);
    wr(b + P_PL_SURF - 0x70000, p->s_pl_surf);   // arms the restore (satoru)
    p->pl_saved = false;
    Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary plane ddb/wm restored"); l.go();
}

void save_cursor_regs(Pipe* p) {
    uint32_t b = 0x70000 + p->id * 0x1000;
    p->s_ctl = rd(b + 0x080);
    p->s_base = rd(b + 0x084);
    p->s_pos = rd(b + 0x088);
    p->s_fbc = rd(b + 0x0a0);
    for (int lv = 0; lv < 6; lv++) p->s_wm[lv] = rd(b + P_CUR_WM0 - 0x70000 + 4u * (uint32_t)lv);
    p->s_wm_trans = rd(b + P_CUR_WM_TRANS - 0x70000);
    p->s_wm_sagv = rd(b + P_CUR_WM_SAGV - 0x70000);
    p->s_wm_sagv_trans = rd(b + P_CUR_WM_SAGV_TRANS - 0x70000);
    p->s_buf = rd(b + P_CUR_BUF_CFG - 0x70000);
    p->saved = true;
}

void restore_cursor_regs(Pipe* p) {
    if (!p->saved) return;
    uint32_t b = 0x70000 + p->id * 0x1000;
    for (int lv = 0; lv < 6; lv++) wr(b + P_CUR_WM0 - 0x70000 + 4u * (uint32_t)lv, p->s_wm[lv]);
    wr(b + P_CUR_WM_TRANS - 0x70000, p->s_wm_trans);
    wr(b + P_CUR_WM_SAGV - 0x70000, p->s_wm_sagv);
    wr(b + P_CUR_WM_SAGV_TRANS - 0x70000, p->s_wm_sagv_trans);
    wr(b + P_CUR_BUF_CFG - 0x70000, p->s_buf);
    wr(b + 0x0a0, p->s_fbc);
    wr(b + 0x080, p->s_ctl);    // off or whatever the firmware had (satoru)
    wr(b + 0x088, p->s_pos);
    wr(b + 0x084, p->s_base);
    p->saved = false;
    p->live = false;
}

uint32_t cursor_pos_val(int x, int y) { return IgpuCalc::CursorPosVal(x, y); }   // tested on the host (satoru)

bool cursor_enable_pipe(Pipe* p, int x, int y) {
    uint32_t b = 0x70000 + p->id * 0x1000;
    uint32_t pos = cursor_pos_val(x, y);
    wr(b + 0x0a0, 0);                                   // fbc off, 64x64 is square (satoru)
    wr(b + 0x080, MCURSOR_MODE_64_ARGB);                // cntl arms an enable (satoru)
    wr(b + 0x088, pos);
    wr(b + 0x084, p->slot_ggtt);                        // base arms the update (satoru)
    p->live = true;
    p->cur_x = x; p->cur_y = y;
    g_hw_updates++;
    return true;
}

// CUR_CTL is double buffered like the rest of the cursor: a write to it only
// takes effect once CUR_BASE is written (i9xx_cursor_update_arm writes base
// last for the disable too). writing the control word alone left the plane
// scanning its last image: the frozen second cursor on the monitor the pointer
// left, and the arrow the boot self-test left in the middle of each screen. (satoru)
void cursor_disable_pipe(Pipe* p) {
    if (!p->live) return;
    uint32_t b = 0x70000 + p->id * 0x1000;
    wr(b + 0x080, 0);
    wr(b + 0x084, p->slot_ggtt);                        // base arms the disable (satoru)
    p->live = false;
}

// every probed pipe's cursor plane off, whatever this driver thinks is live:
// raw mmio only, for the boot hand-off, reset, shutdown and panic paths. (satoru)
void cursor_hide_all_raw() {
    if (!g_mmio) return;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.ready && !p.live) continue;
        volatile uint32_t* r = (volatile uint32_t*)(g_mmio + 0x70000 + p.id * 0x1000);
        r[0x080 / 4] = 0;
        r[0x084 / 4] = p.slot_ggtt;
        p.live = false;
    }
    __asm__ __volatile__("mfence" ::: "memory");
}

bool cursor_move_pipe(Pipe* p, int x, int y) {
    if (x == p->cur_x && y == p->cur_y) return true;
    uint32_t b = 0x70000 + p->id * 0x1000;
    uint32_t pos = cursor_pos_val(x, y);
    wr(b + 0x088, pos);
    wr(b + 0x084, p->slot_ggtt);                        // base arms the pos (satoru)
    uint32_t rb = rd(b + 0x088);
    if (rb != pos) {
        p->failed = true;
        g_failures++;
        cursor_disable_pipe(p);
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor pos readback 0x");
        l.t.x(rb, 8); l.t.s(" != 0x"); l.t.x(pos, 8); l.t.s(", hardware cursor off (software fallback)"); l.go();
        return false;
    }
    p->cur_x = x; p->cur_y = y;
    g_hw_updates++;
    return true;
}

// ---- firmware watermark keep (satoru) ----
// the default for the cursor ddb carve: keep the levels the firmware enabled
// (the laptop's gop enables level 0 only, wm1..5 off) and only switch off a
// level whose blocks no longer fit the smaller primary ddb. the old code wrote
// freshly computed wm1..5 and turned them on. an enabled level lets the
// display ride out a deeper memory power state on its fifo; if our computed
// budget is short of the real latency, the pipe underruns, which on a panel
// looks like torn or broken horizontal lines. kurono.igpu.accel.wmcalc=1 keeps
// the computed values for comparison. (satoru)
uint32_t wm_blocks(uint32_t v) { return v & 0x1FFF; }

uint32_t wm_fit(uint32_t v, uint32_t ddb, uint32_t below) {
    if (!(v & PLANE_WM_EN)) return v;
    if (wm_blocks(v) < ddb) return v;
    return below & ~PLANE_WM_EN;   // skl_need_wm_copy_wa: copy the level below, disabled (satoru)
}

bool keep_fw_plane_wms(Pipe* p, uint32_t b, uint32_t newsize, uint32_t pstart, uint32_t pend, uint32_t cstart) {
    uint32_t v[6];
    for (int lv = 0; lv < 6; lv++) v[lv] = rd(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv);
    if (!(v[0] & PLANE_WM_EN) || wm_blocks(v[0]) >= newsize) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": firmware wm0 0x"); l.t.x(v[0], 8);
        l.t.s(" does not fit a "); l.t.d((uint64_t)newsize); l.t.s(" block primary ddb, refusing"); l.go();
        return false;
    }
    uint32_t out[6];
    int kept = 0;
    out[0] = v[0];
    for (int lv = 1; lv < 6; lv++) {
        out[lv] = wm_fit(v[lv], newsize, out[lv - 1]);
        if (out[lv] & PLANE_WM_EN) kept++;
    }
    uint32_t trans = rd(b + P_PL_WM_TRANS - 0x70000);
    uint32_t sagv = rd(b + P_PL_WM_SAGV - 0x70000);
    uint32_t sagv_trans = rd(b + P_PL_WM_SAGV_TRANS - 0x70000);
    if ((trans & PLANE_WM_EN) && wm_blocks(trans) >= newsize) trans &= ~PLANE_WM_EN;
    if ((sagv & PLANE_WM_EN) && wm_blocks(sagv) >= newsize) sagv &= ~PLANE_WM_EN;
    if ((sagv_trans & PLANE_WM_EN) && wm_blocks(sagv_trans) >= newsize) sagv_trans &= ~PLANE_WM_EN;
    {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary ddb "); l.t.d((uint64_t)pstart);
        l.t.c('-'); l.t.d((uint64_t)pend); l.t.s(" -> "); l.t.d((uint64_t)pstart); l.t.c('-'); l.t.d((uint64_t)cstart);
        l.t.s(", firmware wm kept (wm0 blocks "); l.t.d((uint64_t)wm_blocks(v[0]));
        l.t.s(", "); l.t.d((uint64_t)kept); l.t.s(" higher levels on)"); l.go();
    }
    for (int lv = 0; lv < 6; lv++)
        if (out[lv] != v[lv]) wr(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv, out[lv]);
    if (trans != rd(b + P_PL_WM_TRANS - 0x70000)) wr(b + P_PL_WM_TRANS - 0x70000, trans);
    if (sagv != rd(b + P_PL_WM_SAGV - 0x70000)) wr(b + P_PL_WM_SAGV - 0x70000, sagv);
    if (sagv_trans != rd(b + P_PL_WM_SAGV_TRANS - 0x70000)) wr(b + P_PL_WM_SAGV_TRANS - 0x70000, sagv_trans);
    return true;
}

// a cursor level the primary plane of the same pipe does not have enabled
// buys nothing (the pipe can only use a level every enabled plane meets) and
// is one more value to get wrong: keep the cursor's enabled set inside the
// primary's. skipped with kurono.igpu.accel.wmcalc=1. (satoru)
void cap_cursor_to_primary(uint32_t b, WmSet* cur) {
    for (int lv = 0; lv < 6; lv++) {
        uint32_t pv = rd(b + P_PL_WM0 - 0x70000 + 4u * (uint32_t)lv);
        if (!(pv & PLANE_WM_EN) && cur->wm[lv].enable) {
            if (lv == 0) continue;   // level 0 is required, never dropped (satoru)
            cur->wm[lv].enable = false;
            cur->wm[lv].blocks = cur->wm[lv - 1].blocks;
            cur->wm[lv].lines = cur->wm[lv - 1].lines;
        }
    }
    if (!(rd(b + P_PL_WM_TRANS - 0x70000) & PLANE_WM_EN)) cur->trans.enable = false;
    if (!(rd(b + P_PL_WM_SAGV - 0x70000) & PLANE_WM_EN)) cur->sagv.enable = false;
    if (!(rd(b + P_PL_WM_SAGV_TRANS - 0x70000) & PLANE_WM_EN)) cur->sagv_trans.enable = false;
}

// ---- one pipe enable + self-test (satoru) ----
bool enable_pipe(Pipe* p, bool visible_test) {
    uint32_t b = 0x70000 + p->id * 0x1000;
    if (!p->lit || !g_gsm.ok) return false;
    // cursor ddb size this pipe needs: a 64 wide argb cursor, level 0 must
    // fit. linux uses max(32, min_ddb) for one active pipe. (satoru)
    WmSet cur;
    if (!compute_wms(p->pixclk_khz, p->htotal, CURSOR_W, 0xFFFF, &cur) || !cur.wm[0].enable) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor wm0 does not compute, refusing"); l.go();
        return false;
    }
    uint32_t need = cur.wm[0].min_ddb;
    if (need < CURSOR_MIN_BLOCKS) need = CURSOR_MIN_BLOCKS;
    if (need > 256) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor ddb need "); l.t.d((uint64_t)need);
        l.t.s(" is too large, refusing"); l.go();
        return false;
    }
    uint32_t cstart = 0, cend = 0;
    bool carve = false;
    if (!find_cursor_ddb(p, need, &cstart, &cend, &carve)) return false;
    p->cur_ddb_start = cstart;
    p->cur_ddb_end = cend;
    save_cursor_regs(p);
    if (carve) {
        save_plane_regs(p);
        uint32_t pcfg = rd(b + P_PL_BUF_CFG - 0x70000);
        uint32_t pstart = pcfg & 0xFFF;
        uint32_t pend = pcfg ? ((pcfg >> 16) & 0x1FFF) + 1 : 0;
        if (!pcfg) { pstart = 0; pend = DDB_BANK; }   // see find_cursor_ddb (satoru)
        uint32_t newsize = (pend > cstart) ? (cstart - pstart) : 0;
        if (g_wm_calc) {
            WmSet pl;
            if (!newsize || newsize < 64 || !compute_wms(p->pixclk_khz, p->htotal, p->w, newsize, &pl)) {
                Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary plane wm does not fit the smaller ddb, refusing"); l.go();
                restore_cursor_regs(p);
                return false;
            }
            {
                Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary ddb "); l.t.d((uint64_t)pstart);
                l.t.c('-'); l.t.d((uint64_t)pend); l.t.s(" -> "); l.t.d((uint64_t)pstart); l.t.c('-'); l.t.d((uint64_t)cstart);
                l.t.s(", wm0 blocks "); l.t.d((uint64_t)pl.wm[0].blocks); l.t.s(" (computed wm, kurono.igpu.accel.wmcalc=1)"); l.go();
            }
            write_plane_wms(b, pl);
        } else if (!keep_fw_plane_wms(p, b, newsize, pstart, pend, cstart)) {
            restore_cursor_regs(p);
            return false;
        }
        wr(b + P_PL_BUF_CFG - 0x70000, IgpuCalc::DdbRegVal((uint16_t)pstart, (uint16_t)cstart));
        wr(b + P_PL_SURF - 0x70000, rd(b + P_PL_SURF - 0x70000));   // arm the plane update (satoru)
    }
    // cursor watermarks sized to the reserved range (satoru)
    uint32_t ddb_size = cend - cstart;
    for (int lv = 0; lv < 6; lv++) if (cur.wm[lv].min_ddb > ddb_size) {
        IgpuCalc::WmLevel off = { false, 0, 0, 0 };
        if (lv > 0) { off.blocks = cur.wm[lv - 1].blocks; off.lines = cur.wm[lv - 1].lines; }
        cur.wm[lv] = off;
    }
    if (cur.trans.min_ddb > ddb_size) cur.trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    if (cur.sagv.min_ddb > ddb_size) cur.sagv = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    if (cur.sagv_trans.min_ddb > ddb_size) cur.sagv_trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    if (!g_wm_calc) cap_cursor_to_primary(b, &cur);
    write_cursor_wms(b, cur);
    wr(b + P_CUR_BUF_CFG - 0x70000, IgpuCalc::DdbRegVal((uint16_t)cstart, (uint16_t)cend));
    {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor ddb "); l.t.d((uint64_t)cstart);
        l.t.c('-'); l.t.d((uint64_t)cend); l.t.s(" wm0 "); l.t.d(cur.wm[0].enable);
        l.t.c('/'); l.t.d((uint64_t)cur.wm[0].blocks); l.go();
    }
    if (!write_cursor_image(p, visible_test)) {
        restore_cursor_regs(p);
        if (carve) restore_plane_regs(p);
        return false;
    }
    // program and verify: base arms, surflive follows (intel_cursor.c) (satoru)
    int tx = (int)p->w / 2, ty = (int)p->h / 2;
    cursor_enable_pipe(p, tx, ty);
    uint32_t last = 0;
    if (!wait_reg(b + 0x0ac, 0xFFFFF000u, p->slot_ggtt, 100000, &last)) {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor surflive 0x"); l.t.x(last, 8);
        l.t.s(" did not latch 0x"); l.t.x(p->slot_ggtt, 8); l.go();
        cursor_disable_pipe(p);
        restore_cursor_regs(p);
        if (carve) restore_plane_regs(p);
        return false;
    }
    p->verified = true;
    {
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": cursor surflive 0x"); l.t.x(last, 8);
        l.t.s(" ok, test move "); l.go();
    }
    if (visible_test) udelay(2000000);
    if (!cursor_move_pipe(p, 64, 64) || !cursor_move_pipe(p, (int)p->w - 80, (int)p->h - 80) ||
        !cursor_move_pipe(p, tx, ty)) {
        cursor_disable_pipe(p);
        restore_cursor_regs(p);
        if (carve) restore_plane_regs(p);
        return false;
    }
    // hidden until the first real pointer update, ddb/wm stay programmed (satoru)
    cursor_disable_pipe(p);
    p->ready = true;
    Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": PASS hardware cursor ready"); l.go();
    return true;
}

void disable_pipe(Pipe* p) {
    if (!p->ready && !p->saved && !p->own_ptes) return;
    cursor_disable_pipe(p);
    restore_cursor_regs(p);
    if (p->pl_saved) restore_plane_regs(p);
    restore_ptes(p);
    p->ready = false;
}

// ---- output mapping (satoru) ----
int pipe_for_output(int oi, int x, int y) {
    const Output* o = Outputs::Get(oi);
    if (!o || !o->connected) return -1;
    // ui scale or a logical/physical size mismatch: the cursor plane works in
    // physical pixels, so refuse rather than put it at a wrong place (satoru)
    if (o->w != o->pw || o->h != o->ph) return -1;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.ready || p.failed || !p.lit) continue;
        if (o->phys != p.phys) continue;
        if (o->pw != (int)p.w || o->ph != (int)p.h) continue;
        if (x < o->x || x >= o->x + o->w || y < o->y || y >= o->y + o->h) return -1;
        return (int)i;
    }
    return -1;
}

int pipe_for_point(int x, int y, int* lx, int* ly) {
    // ui scale on a single output: SetUIScale shrinks the logical framebuffer but leaves
    // the output at its physical size, so pipe_for_output saw 1:1 and the cursor plane got
    // logical coordinates as physical pixels (the arrow sat at 2/3 of the pointer at 150%).
    // the software cursor is drawn in logical space and scaled with the frame. (satoru)
    if (Graphics::GetUIScale() != 100 && !Graphics::MultiActive()) return -1;
    if (Outputs::Total() > 0) {
        int oi = Outputs::At(x, y);
        if (oi < 0) return -1;
        const Output* o = Outputs::Get(oi);
        int pi = pipe_for_output(oi, x, y);
        if (pi < 0) return -1;
        if (!IgpuCalc::CursorLocal(Graphics::GetUIScale(), Graphics::MultiActive(), o->x, o->y, o->w, o->h, o->pw, o->ph, x, y, lx, ly)) return -1;
        return pi;
    }
    // before outputs exist (early boot): pipe a covers the boot framebuffer (satoru)
    Pipe& p = g_pipes[0];
    if (p.ready && !p.failed && x >= 0 && y >= 0 && x < (int)p.w && y < (int)p.h) {
        *lx = x; *ly = y;
        return 0;
    }
    return -1;
}

bool cursor_update_locked(int x, int y) {
    if (!g_enabled || !g_visible) return false;
    int lx = 0, ly = 0;
    int want = (x >= 0) ? pipe_for_point(x, y, &lx, &ly) : -1;
    int live = -1;
    for (uint32_t i = 0; i < PIPE_N; i++) if (g_pipes[i].live) live = (int)i;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.ready || p.failed) continue;
        if ((int)i == want) {
            if (p.live) return cursor_move_pipe(&p, lx, ly);
            if (live >= 0) cursor_disable_pipe(&g_pipes[live]);
            return cursor_enable_pipe(&p, lx, ly);
        }
        if (p.live) cursor_disable_pipe(&p);
    }
    return false;
}

// ---- stage 2: zero-copy present (satoru) ----
// the compositor renders into a write-back ram buffer that the display engine
// scans through a validated ggtt window. every damaged rect is clflushed
// before the display can fetch it, and there is no present copy. the wc-mapped
// gop framebuffer is never a render target: blending reads it, and uncached
// reads were what made the first attempt look frozen. single output allocates
// its own ram buffer; multi output scans slices of the compositor virtual
// desktop (see skl_plane_update_noarm: PLANE_STRIDE/POS/SIZE/OFFSET/SURF). (satoru)
constexpr uint32_t ZC_MAX_PTES = 32768;           // 128 mb window cap (satoru)

struct ZcPipe {
    int id;
    bool used;
    uint64_t phys;
    int ox, oy, ow, oh;                           // slice in the shared buffer (satoru)
    uint32_t s_ctl, s_stride, s_pos, s_size, s_offset, s_surf;
};

struct ZcState {
    bool active;
    bool committed;
    bool multi;
    uint32_t win_ggtt, win_npte;
    uint64_t win_bytes;
    uint64_t saved_phys;                          // pmm copy of the old ptes (satoru)
    uint32_t saved_frames;
    uint64_t target_phys;
    uint32_t pitch, w, h;
    ZcPipe pipe[PIPE_N];
    uint32_t commits, fails;
};

ZcState g_zc;
bool g_zc_flag = false;
bool g_zc_runtime_off = false;

void zc_restore_planes(bool raw);
void zc_unmap_window();
void zc_suspend_hook();
void zc_present_hook();
void zc_panic_restore();

// true when [lo, hi) overlaps a surface the display engine can be scanning
// right now: an enabled primary/overlay plane, its latched surface, or a
// cursor image. gated register reads return all ones and are ignored. stale
// ggtt entries left by the firmware are not referenced by any enabled plane,
// so a window that passes this test is safe to repoint. (satoru)
bool window_used_by_display(uint64_t lo, uint64_t hi) {
    if (!g_mmio) return true;
    auto span_hits = [&](uint32_t surf, uint64_t span) {
        if (!surf || surf == 0xFFFFFFFFu || !span) return false;
        uint64_t s0 = surf & 0xFFFFF000u;
        return lo < s0 + span && hi > s0;
    };
    for (uint32_t id = 0; id < 4; id++) {
        uint32_t b = 0x70000 + id * 0x1000;
        uint32_t tc = rd(b + 0x008);
        if (tc == 0xFFFFFFFFu || !(tc & TRANSCONF_ENABLE)) continue;
        for (uint32_t pl = 0; pl < 4; pl++) {
            uint32_t pb = b + 0x180 + pl * 0x100;
            uint32_t ctl = rd(pb);
            if (ctl == 0xFFFFFFFFu || !(ctl & PLANE_CTL_ENABLE)) continue;
            uint64_t span = (uint64_t)(rd(pb + 0x08) & 0xFFFu) * 64ull *
                            (uint64_t)(((rd(pb + 0x10) >> 16) & 0xFFFFu) + 1u);
            if (span_hits(rd(pb + 0x1c), span)) return true;   // plane_surf
            if (span_hits(rd(pb + 0x2c), span)) return true;   // plane_surflive
        }
        if (span_hits(rd(b + 0x084), 0x10000ull)) return true; // cur_base
        if (span_hits(rd(b + 0x0ac), 0x10000ull)) return true; // cur_surflive
    }
    return false;
}

// find a free window below the firmware surface, save its old ptes, map the
// contiguous write-back buffer into it and verify every entry read back. the
// unused ggtt is stale stolen-memory contents, so IgpuCalc::GgttPteLive is the
// only safe test (the same rule ggtt_setup uses). stale entries that no
// enabled display plane references (window_used_by_display) are older firmware
// leftovers, not scanout mappings, so the window is still usable and every
// overwritten pte is saved for the restore. (satoru)
bool zc_map_window(uint64_t phys, uint64_t bytes) {
    if (!g_gsm.ok || !bytes || !phys) return false;
    uint64_t win = (bytes + 0xFFFFFFull) & ~0xFFFFFFull;   // 16 mb search step (satoru)
    uint32_t npte = (uint32_t)((bytes + 0xFFFull) >> 12);  // only the pages we own (satoru)
    if (!npte || npte > ZC_MAX_PTES) {
        Ln l; l.t.s("zerocopy: buffer "); l.t.d(bytes); l.t.s(" bytes needs "); l.t.d((uint64_t)npte);
        l.t.s(" ptes, too large for one window, refusing"); l.go();
        return false;
    }
    uint32_t surf_a = rd(P_PL_SURF) & 0xFFFFF000u;
    if (!surf_a) { emit("zerocopy: pipe A surface is not programmed, refusing"); return false; }
    uint32_t frames = (npte * 8u + 4095u) / 4096u;
    uint64_t saved = PMM::AllocContiguous(frames);
    if (!saved) { emit("zerocopy: no memory to save the old ptes, refusing"); return false; }
    uint64_t phys_top = PMM::GetPhysTop();
    for (uint32_t tries = 0; tries < 64; tries++) {
        uint64_t cand = ((uint64_t)surf_a & ~0xFFFFFFull) - win * (uint64_t)(tries + 1);
        if (cand < 0x10000000ull || cand >= surf_a) break;
        uint32_t idx = (uint32_t)(cand >> 12);
        bool mapped = true;
        for (uint32_t pi = idx; pi < idx + npte; pi += 512)
            if (!gsm_map(pi)) { mapped = false; break; }
        if (!mapped) { Ln l; l.t.s("zerocopy: cannot map the gsm pte pages, refusing"); l.go(); PMM::FreeContiguous(saved, frames); return false; }
        uint64_t* save = (uint64_t*)(uintptr_t)saved;
        uint32_t live = 0;
        for (uint32_t i = 0; i < npte; i++) {
            uint64_t pte = *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8);
            save[i] = pte;
            if (IgpuCalc::GgttPteLive(pte, phys_top)) live++;
        }
        bool display_uses = window_used_by_display(cand, cand + bytes);
        {
            Ln l; l.t.s("zerocopy: ggtt window 0x"); l.t.x((uint32_t)cand, 8); l.t.s(" ptes ");
            l.t.d((uint64_t)npte); l.t.s(" live "); l.t.d((uint64_t)live);
            l.t.s(display_uses ? " referenced by an enabled plane, skipping" :
                  live ? " stale (no enabled plane uses them), taking" : " free"); l.go();
        }
        if (display_uses) continue;
        bool ok = true;
        for (uint32_t i = 0; i < npte; i++) {
            volatile uint64_t* slot = (volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8);
            *slot = IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC);
        }
        __asm__ __volatile__("mfence" ::: "memory");
        for (uint32_t i = 0; i < npte; i++) {   // every pte we touched reads back (satoru)
            volatile uint64_t* slot = (volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8);
            if (*slot != IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC)) { ok = false; break; }
        }
        if (!ok) {
            for (uint32_t i = 0; i < npte; i++)
                *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) = save[i];
            __asm__ __volatile__("mfence" ::: "memory");
            Ln l; l.t.s("zerocopy: ggtt window 0x"); l.t.x((uint32_t)cand, 8);
            l.t.s(" read back mismatch, restored, refusing"); l.go();
            PMM::FreeContiguous(saved, frames);
            return false;
        }
        g_zc.win_ggtt = (uint32_t)cand;
        g_zc.win_npte = npte;
        g_zc.win_bytes = win;
        g_zc.saved_phys = saved;
        g_zc.saved_frames = frames;
        {
            Ln l; l.t.s("zerocopy: mapped "); l.t.d((uint64_t)npte); l.t.s(" ptes at ggtt 0x");
            l.t.x((uint32_t)cand, 8); l.t.s(" -> phys 0x"); l.t.x((uint32_t)(phys >> 32), 8);
            l.t.x((uint32_t)phys, 8); l.t.s(" (all read back)"); l.go();
        }
        return true;
    }
    PMM::FreeContiguous(saved, frames);
    emit("zerocopy: no free ggtt window below the firmware surface, refusing");
    return false;
}

void zc_unmap_window() {
    if (!g_zc.win_npte || !g_zc.saved_phys) return;
    uint32_t idx = g_zc.win_ggtt >> 12;
    const uint64_t* save = (const uint64_t*)(uintptr_t)g_zc.saved_phys;
    for (uint32_t i = 0; i < g_zc.win_npte; i++)
        *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) = save[i];
    __asm__ __volatile__("mfence" ::: "memory");
    PMM::FreeContiguous(g_zc.saved_phys, g_zc.saved_frames);
    Ln l; l.t.s("zerocopy: ggtt window 0x"); l.t.x(g_zc.win_ggtt, 8); l.t.s(" ptes restored"); l.go();
    g_zc.win_ggtt = 0; g_zc.win_npte = 0; g_zc.saved_phys = 0; g_zc.saved_frames = 0;
}

void zc_save_planes() {
    for (uint32_t i = 0; i < PIPE_N; i++) {
        ZcPipe& z = g_zc.pipe[i];
        if (!z.used) continue;
        uint32_t b = 0x70000 + (uint32_t)z.id * 0x1000;
        z.s_ctl    = rd(b + P_PL_CTL - 0x70000);
        z.s_stride = rd(b + P_PL_STRIDE - 0x70000);
        z.s_pos    = rd(b + P_PL_POS - 0x70000);
        z.s_size   = rd(b + P_PL_SIZE - 0x70000);
        z.s_offset = rd(b + P_PL_OFFSET - 0x70000);
        z.s_surf   = rd(b + P_PL_SURF - 0x70000);
    }
}

// put the primary plane of every used pipe back on its original surface. raw
// skips the allowlist so the panic path stays independent of module state; the
// write order is i915's: geometry, control, then surf last to arm it. (satoru)
void zc_restore_planes(bool raw) {
    for (uint32_t i = 0; i < PIPE_N; i++) {
        ZcPipe& z = g_zc.pipe[i];
        if (!z.used) continue;
        uint32_t b = 0x70000 + (uint32_t)z.id * 0x1000;
        if (raw) {
            volatile uint32_t* m = (volatile uint32_t*)g_mmio;
            m[(b + P_PL_STRIDE - 0x70000) / 4] = z.s_stride;
            m[(b + P_PL_POS - 0x70000) / 4]    = z.s_pos;
            m[(b + P_PL_SIZE - 0x70000) / 4]   = z.s_size;
            m[(b + P_PL_OFFSET - 0x70000) / 4] = z.s_offset;
            m[(b + P_PL_CTL - 0x70000) / 4]    = z.s_ctl;
            m[(b + P_PL_SURF - 0x70000) / 4]   = z.s_surf;
        } else {
            wr(b + P_PL_STRIDE - 0x70000, z.s_stride);
            wr(b + P_PL_POS - 0x70000, z.s_pos);
            wr(b + P_PL_SIZE - 0x70000, z.s_size);
            wr(b + P_PL_OFFSET - 0x70000, z.s_offset);
            wr(b + P_PL_CTL - 0x70000, z.s_ctl);
            wr(b + P_PL_SURF - 0x70000, z.s_surf);
        }
    }
    __asm__ __volatile__("mfence" ::: "memory");
}

// first complete zero-copy frame: point the planes at the shared buffer. runs
// from the compositor's swap after the damage was flushed, so the frame the
// display starts fetching is already in memory. (satoru)
void zc_commit_planes() {
    WrLog wl;
    bool ok = true;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        ZcPipe& z = g_zc.pipe[i];
        if (!z.used) continue;
        uint32_t b = 0x70000 + (uint32_t)z.id * 0x1000;
        wr(b + P_PL_STRIDE - 0x70000, g_zc.pitch / 64);
        wr(b + P_PL_POS - 0x70000, 0);
        wr(b + P_PL_SIZE - 0x70000, ((uint32_t)(z.oh - 1) << 16) | (uint32_t)(z.ow - 1));
        wr(b + P_PL_OFFSET - 0x70000, ((uint32_t)z.oy << 16) | (uint32_t)z.ox);
        wr(b + P_PL_SURF - 0x70000, g_zc.win_ggtt);    // arms the update (satoru)
    }
    for (uint32_t i = 0; i < PIPE_N; i++) {
        ZcPipe& z = g_zc.pipe[i];
        if (!z.used) continue;
        uint32_t b = 0x70000 + (uint32_t)z.id * 0x1000;
        uint32_t last = 0;
        if (!wait_reg(b + P_PL_SURFLIVE - 0x70000, 0xFFFFF000u, g_zc.win_ggtt, 60000, &last)) {
            Ln l; l.t.s("zerocopy: pipe "); l.t.c((char)('A' + z.id)); l.t.s(" surflive 0x");
            l.t.x(last, 8); l.t.s(" did not latch 0x"); l.t.x(g_zc.win_ggtt, 8); l.go();
            ok = false;
        } else {
            Ln l; l.t.s("zerocopy: pipe "); l.t.c((char)('A' + z.id)); l.t.s(" scanning slice (");
            l.t.d((uint64_t)z.ox); l.t.c(','); l.t.d((uint64_t)z.oy); l.t.s(") ");
            l.t.d((uint64_t)z.ow); l.t.c('x'); l.t.d((uint64_t)z.oh); l.t.s(" surflive 0x");
            l.t.x(last, 8); l.go();
        }
    }
    if (!ok) {
        g_zc.fails++;
        g_zc.active = false;
        g_zc.committed = false;
        g_zc_runtime_off = true;
        zc_restore_planes(true);
        zc_unmap_window();
        emit("FAILED zero-copy plane commit, surfaces restored, cpu present stays");
        Graphics::ZeroCopyLeave();
        return;
    }
    g_zc.committed = true;
    g_zc.commits++;
    {
        Ln l; l.t.s("ENABLED zero-copy present: "); l.t.d((uint64_t)g_zc.w); l.t.c('x');
        l.t.d((uint64_t)g_zc.h); l.t.s(" buffer in ggtt 0x"); l.t.x(g_zc.win_ggtt, 8);
        l.t.s(", clflush damage, no present copy"); l.go();
    }
}

void zc_present_hook() {
    if (!g_zc.active || g_zc.committed) return;
    zc_commit_planes();
}

// the compositor is about to reallocate or repoint its buffer: restore the
// scanned surfaces and release the window before that memory goes away. a
// re-arm needs a fresh map, so the request stays off until `gpuaccel on`. (satoru)
void zc_suspend_hook() {
    WrLog wl;
    if (!g_zc.active && !g_zc.committed) return;
    g_zc.active = false;
    g_zc.committed = false;
    g_zc_runtime_off = true;
    zc_restore_planes(false);
    zc_unmap_window();
    emit("zerocopy: suspended (compositor buffer change), surfaces restored");
}

// panic: no logging, no allocation, only the saved scanout registers. (satoru)
void zc_panic_restore() {
    if (!g_zc.active && !g_zc.committed) return;
    if (g_mmio) zc_restore_planes(true);
    g_zc.committed = false;
    g_zc.active = false;
}

void zc_stop(bool announce) {
    WrLog wl;
    bool was = g_zc.active || g_zc.committed;
    if (was) {
        g_zc.active = false;
        g_zc.committed = false;
        zc_restore_planes(false);
        zc_unmap_window();
    }
    Graphics::SetZeroCopyHooks(nullptr, nullptr);
    if (Graphics::ZeroCopyActive()) Graphics::ZeroCopyLeave();
    if (announce && was) emit("zerocopy: off, scanned surfaces and ggtt window restored");
}

int zc_pipe_for(uint64_t phys, int w, int h) {
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.ready || p.failed || !p.lit) continue;
        if (p.phys != phys) continue;
        if ((int)p.w != w || (int)p.h != h) continue;
        return (int)i;
    }
    return -1;
}

int zc_enable_impl() {
    WrLog wl;
    if (!g_mmio) { emit("zerocopy: no usable intel display engine, skipped"); return g_out_pos; }
    if (!g_enabled) { emit("zerocopy: requires kurono.igpu.accel=1 (stage 1 cursor not ready)"); return g_out_pos; }
    if (g_zc.active) { emit("zerocopy: already active"); return g_out_pos; }
    clock_init();
    emit("---- intel display acceleration stage 2: zero-copy present ----");
    if (!g_gsm.ok) { emit("FAIL zerocopy: ggtt is not available"); return g_out_pos; }
    if (Graphics::GetBpp() != 32 || Graphics::GetUIScale() != 100 || Graphics::GetWidth() <= 0) {
        emit("FAIL zerocopy: needs a 32 bpp 100% scale framebuffer");
        return g_out_pos;
    }
    // without damage-only wallpaper every frame re-blits the whole cache and
    // marks full damage, so zero-copy would flush the whole buffer every frame
    // (slower than the copy it removes). require the damage path. (satoru)
    if (!Graphics::WallpaperDamageOnly()) {
        emit("FAIL zerocopy: needs kurono.wpdamage=1 (full-frame damage would be flushed every frame)");
        return g_out_pos;
    }
    g_zc.multi = Graphics::MultiActive();
    for (uint32_t i = 0; i < PIPE_N; i++) g_zc.pipe[i] = {};
    uint32_t used = 0;
    uint32_t nout = (uint32_t)Outputs::Total();
    for (uint32_t oi = 0; oi < nout; oi++) {
        const Output* o = Outputs::Get((int)oi);
        if (!o || !o->connected) continue;
        if (o->w != o->pw || o->h != o->ph) {
            Ln l; l.t.s("FAIL zerocopy: output "); l.t.d((uint64_t)oi);
            l.t.s(" is not 1:1 (ui scale), refusing"); l.go();
            return g_out_pos;
        }
        int pi = zc_pipe_for(o->phys, o->pw, o->ph);
        if (pi < 0) {
            Ln l; l.t.s("FAIL zerocopy: output "); l.t.d((uint64_t)oi);
            l.t.s(" is not on a validated intel pipe, refusing"); l.go();
            return g_out_pos;
        }
        ZcPipe& z = g_zc.pipe[pi];
        if (z.used) { emit("FAIL zerocopy: two outputs map to one pipe, refusing"); return g_out_pos; }
        z.used = true;
        z.id = pi;
        z.phys = o->phys;
        z.ox = g_zc.multi ? o->x : 0;
        z.oy = g_zc.multi ? o->y : 0;
        z.ow = o->w;
        z.oh = o->h;
        used++;
    }
    if (!used) { emit("FAIL zerocopy: no connected output"); return g_out_pos; }
    // the render target must be owned write-back ram, never a wc-mapped gop
    // framebuffer: blending does read-modify-write and uncached reads are the
    // reason the first attempt was unusable. graphics allocates it. (satoru)
    char why[64];
    if (!Graphics::ZeroCopyEnter(why, sizeof(why))) {
        Ln l; l.t.s("FAIL zerocopy: compositor refused: "); l.t.s(why); l.go();
        return g_out_pos;
    }
    uint64_t target = KernelPhysOf(Graphics::GetBuffer());
    uint64_t bytes = Graphics::GetBufferBytes();
    int tw = Graphics::GetWidth(), th = Graphics::GetHeight();
    uint32_t tpitch = Graphics::GetPitch();
    if (!target || !bytes || tw <= 0 || th <= 0 || !tpitch ||
        target + bytes > PMM::GetPhysTop() || (tpitch & 63) || (tpitch / 64) > 0xFFF) {
        emit("FAIL zerocopy: render target is not a usable write-back buffer");
        Graphics::ZeroCopyLeave();
        return g_out_pos;
    }
    g_zc.target_phys = target;
    g_zc.pitch = tpitch;
    g_zc.w = (uint32_t)tw;
    g_zc.h = (uint32_t)th;
    zc_save_planes();
    if (!zc_map_window(target, bytes)) {
        Graphics::ZeroCopyLeave();
        return g_out_pos;
    }
    Graphics::SetZeroCopyHooks(zc_suspend_hook, zc_present_hook);
    KernelPanic::SetDisplayRestoreHook(zc_panic_restore);
    g_zc.active = true;
    g_zc.committed = false;
    g_zc.commits = 0;
    g_zc.fails = 0;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        ZcPipe& z = g_zc.pipe[i];
        if (!z.used) continue;
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + z.id)); l.t.s(": slice (");
        l.t.d((uint64_t)z.ox); l.t.c(','); l.t.d((uint64_t)z.oy); l.t.s(") ");
        l.t.d((uint64_t)z.ow); l.t.c('x'); l.t.d((uint64_t)z.oh);
        l.t.s(" saved surf 0x"); l.t.x(z.s_surf, 8); l.go();
    }
    emit("ARMED zero-copy present: planes switch on the next complete frame");
    g_zc_runtime_off = false;
    return g_out_pos;
}

// ---- stage 2b: double-buffered page flip (kurono.igpu.flip=1) (satoru) ----
// zero-copy (direct) renders straight into the buffer the display scans. the
// compositor paints the wallpaper under a damaged rect and then the windows
// over it, and the cache writes those lines back in whatever order it evicts
// them, so the panel fetches half-drawn frames (wallpaper bands through the
// taskbar and apps) until the end-of-frame flush. the flip path never touches
// scanned memory: the compositor keeps its own back buffer, the damage is
// copied with non-temporal stores (no dirty cache line is left behind) into
// the idle one of two buffers per pipe, and PLANE_SURF is rewritten, which the
// hardware latches at the next vblank. the old buffer is reused only after
// PLANE_SURFLIVE shows the latch, so a slower pipe (the 60 hz monitor) defers
// its update instead of tearing or stalling the faster one. (satoru)
//
// each buffer has exactly the layout of the surface the firmware or stage 2
// programmed (same stride and size, offset 0), so PLANE_SURF is the only
// register a flip writes. the ggtt windows come from a fixed region (1 gb up to
// 3 gb, far below the firmware's top-of-ggtt surfaces); a window is refused if
// an enabled plane or cursor references it or another kurono user owns it, and
// every pte it overwrites is saved and put back on off. (satoru)
constexpr int FL_RECTS = 32;
constexpr uint32_t FL_REGION_LO = 0x40000000u;
constexpr uint32_t FL_REGION_HI = 0xC0000000u;
constexpr uint32_t FL_WIN = 0x1000000u;               // one 16 mb window per buffer (satoru)
constexpr uint64_t FL_LATCH_TIMEOUT_US = 250000;      // never latched: flip goes off (satoru)
constexpr int FL_CHECKS = 6;                          // crc checks per pipe (satoru)

struct FlRect { int x, y, w, h; };
struct FlList { FlRect r[FL_RECTS]; int n; bool full; };

struct FlPipe {
    bool used;
    int id;                       // pipe (hardware) or output slot (qemu test) (satoru)
    int out;                      // the Outputs slot, for its tone (dim, night light) (satoru)
    int ox, oy, ow, oh;           // slice of the compositor frame (satoru)
    uint32_t pitch;
    uint64_t bytes;
    uint32_t frames;
    uint64_t phys[2];
    uint32_t ggtt[2];
    uint32_t npte;
    uint64_t saved[2];            // pmm pages holding each window's old ptes (satoru)
    uint32_t saved_frames;
    bool mapped[2];
    uint32_t s_surf;              // the firmware / stage 2 surface, restored on off (satoru)
    uint8_t* test_fb;             // qemu: stand-in scanout (satoru)
    uint32_t test_pitch;
    uint32_t test_lat_us;
    int test_req, test_live;
    Lapse test_req_lapse;
    uint64_t test_req_tsc;        // qemu stand-in: raw tsc of the request, vm tscs agree (satoru)
    int front;
    bool pending;
    Lapse pend;                   // cpu-safe time since the pending flip was armed (satoru)
    // the pipe as it was at that arm, for the failure line. the frame and flip
    // counters and the display clock are hardware, so they say how long the
    // flip really waited no matter which cpu asks. (satoru)
    uint32_t arm_cpu, arm_hw_us, arm_frm, arm_flipcnt, arm_dsl, arm_live;
    uint64_t arm_tsc;             // raw rdtsc at the arm: only to show a cross-cpu offset (satoru)
    FlList acc;                   // damage the front still misses (satoru)
    FlList prev;                  // damage the back misses: what the last flip wrote (satoru)
    uint32_t flips, defers;
    uint32_t latch_us_max;
    int checks_left, checks_ok, mismatches;
    Lapse check_lapse;            // spacing of the selfchecks (satoru)
    bool check_spaced;
};

struct FlState {
    bool active;
    bool seeded;
    int test;
    FlPipe pipe[PIPE_N];
    uint32_t flips, defers, fails;
    uint64_t copy_bytes, copy_cycles;
    uint32_t latch_us_max;
    bool rearm_pending;
    Lapse rearm_lapse;
    uint32_t rearms;
    bool hw_clock_ok;             // the display microsecond counter advances at the tsc's rate (satoru)
    bool skew_logged;
    uint32_t tone_sig;            // Graphics::ToneSignature() the buffers were filled with (satoru)
};

FlState g_fl;
bool g_fl_flag = false;
int g_fl_test_flag = 0;
Spinlock g_fl_lock;
// ggtt entries other drivers took out of the flip region (IntelAccel::GgttReserve) (satoru)
struct XOwn { uint32_t ggtt, npte; };
XOwn g_xown[4] = {};

// how long f's pending flip has really been waiting. the old code subtracted
// two raw rdtsc values that could come from different cpus (see Lapse). on
// hardware the answer is the display engine's own microsecond counter, read
// at the arm and now: it is the same register for every cpu. the cpu-safe
// lapse is a lower bound of the real time, so it can only raise the answer,
// which covers a display counter that stopped. qemu has the lapse alone. (satoru)
uint64_t fl_waited_us(FlPipe& f, uint32_t* hw_us = nullptr, uint32_t* cpu = nullptr, uint64_t* sw_us = nullptr) {
    uint64_t sw = lapse_us(f.pend, cpu);
    uint64_t w = sw;
    uint32_t hw = 0;
    if (!g_fl.test && g_fl.hw_clock_ok) {
        hw = rd(R_TIMESTAMP_CTR) - f.arm_hw_us;
        if ((uint64_t)hw > w) w = hw;
    }
    if (hw_us) *hw_us = hw;
    if (sw_us) *sw_us = sw;
    return w;
}

void fl_log_signed(Txt& t, int64_t v) {
    if (v < 0) { t.c('-'); t.d((uint64_t)(-v)); } else t.d((uint64_t)v);
}

// everything the next laptop log needs to tell a display problem from a
// bookkeeping one: the plane registers, the pipe's frame and flip counters and
// scanline at the arm and now, who armed and who checks, and what each clock
// says. (satoru)
void fl_fail_detail(FlPipe& f) {
    uint32_t cpu = 0, hw = 0;
    uint64_t sw = 0;
    fl_waited_us(f, &hw, &cpu, &sw);
    int64_t raw = (int64_t)(tsc() - f.arm_tsc) / (int64_t)(g_tpu ? g_tpu : 3000);
    {
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" detail: waited "); l.t.d(sw);
        l.t.s(" us (same-cpu tsc, a lower bound), "); l.t.d((uint64_t)hw); l.t.s(" us (display clock");
        l.t.s(g_fl.test ? ", none in qemu" : (g_fl.hw_clock_ok ? "" : ", not usable")); l.t.s("), raw rdtsc says ");
        fl_log_signed(l.t, raw);
        l.t.s(" us | armed on cpu "); l.t.d((uint64_t)f.arm_cpu); l.t.s(", checked on cpu "); l.t.d((uint64_t)cpu);
        l.t.s(" | front "); l.t.d((uint64_t)f.front); l.t.s(" flips "); l.t.d((uint64_t)f.flips); l.go();
    }
    if (g_fl.test) return;
    uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
    {
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" detail: plane_ctl 0x"); l.t.x(rd(b + P_PL_CTL - 0x70000), 8);
        l.t.s(" plane_surf 0x"); l.t.x(rd(b + P_PL_SURF - 0x70000), 8);
        l.t.s(" surflive 0x"); l.t.x(rd(b + P_PL_SURFLIVE - 0x70000), 8);
        l.t.s(" stride 0x"); l.t.x(rd(b + P_PL_STRIDE - 0x70000), 8);
        l.t.s(" transconf 0x"); l.t.x(rd(b + P_TRANSCONF - 0x70000), 8);
        l.t.s(" psr 0x"); l.t.x(rd(R_SRD_CTL_A + (uint32_t)f.id * 0x1000), 8); l.go();
    }
    {
        uint32_t frm = rd(b + P_FRMCNT - 0x70000);
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" detail: at the arm frame "); l.t.d((uint64_t)f.arm_frm);
        l.t.s(" flipcount "); l.t.d((uint64_t)f.arm_flipcnt); l.t.s(" line "); l.t.d((uint64_t)f.arm_dsl);
        l.t.s(" surflive 0x"); l.t.x(f.arm_live, 8);
        l.t.s(" | now frame "); l.t.d((uint64_t)frm);
        l.t.s(" flipcount "); l.t.d((uint64_t)rd(b + P_FLIPCNT - 0x70000));
        l.t.s(" line "); l.t.d((uint64_t)(rd(b + P_DCY - 0x70000) & 0xFFFFFu));
        l.t.s(" (frames that passed: "); l.t.d((uint64_t)(uint32_t)(frm - f.arm_frm)); l.t.s(")"); l.go();
    }
}

// copy n bytes with non-temporal stores. dst and the length are 64 byte
// multiples on the fast path (16 pixel aligned columns in a 64 byte aligned
// pitch); anything else is written normally and its lines clflushed, so
// nothing the display reads is left only in a cpu cache. (satoru)
void fl_nt_copy(uint8_t* d, const uint8_t* s, size_t n) {
    while (n >= 64 && !((uintptr_t)d & 63)) {
        __asm__ __volatile__(
            "movdqu   (%0), %%xmm0;  movdqu 16(%0), %%xmm1;"
            "movdqu 32(%0), %%xmm2;  movdqu 48(%0), %%xmm3;"
            "movntdq %%xmm0,   (%1); movntdq %%xmm1, 16(%1);"
            "movntdq %%xmm2, 32(%1); movntdq %%xmm3, 48(%1);"
            :: "r"(s), "r"(d) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
        d += 64; s += 64; n -= 64;
    }
    if (!n) return;
    uint8_t* d0 = d;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    for (uintptr_t p = (uintptr_t)d0 & ~(uintptr_t)63; p < (uintptr_t)d0 + n; p += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"((const void*)p) : "memory");
}

// one row of the output's tone (filter, dim, night light), converted before
// the non-temporal copy. 8192 px covers any single output. (satoru)
alignas(64) uint32_t g_fl_row[8192];

// the source pixels of a slice row exactly as the cpu present would show
// them on this output: the back buffer itself, or a toned copy of it. (satoru)
const uint8_t* fl_src_row(FlPipe& f, const uint8_t* src, uint32_t spitch, int x0, int y, int n, bool tone) {
    const uint8_t* s = src + (size_t)(f.oy + y) * spitch + (size_t)(f.ox + x0) * 4u;
    if (!tone) return s;
    if (n > 8192) n = 8192;
    Graphics::ToneRow(g_fl_row, (const uint32_t*)s, n, f.out);
    return (const uint8_t*)g_fl_row;
}

// cumulative flip counters for the gfx oracle, never reset (satoru)
IntelAccel::FlipTotals g_fl_tot = {};

void fl_copy_rect(FlPipe& f, int k, const uint8_t* src, uint32_t spitch, int x, int y, int w, int h) {
    int x0 = x & ~15;
    int x1 = (x + w + 15) & ~15;
    if (x1 > f.ow) x1 = f.ow;
    if (x0 >= x1 || h <= 0) return;
    uint8_t* dbase = (uint8_t*)(uintptr_t)f.phys[k];
    size_t n = (size_t)(x1 - x0) * 4u;
    bool tone = Graphics::ToneActive(f.out);
    if (tone && x1 - x0 > 8192) { x1 = x0 + 8192; n = (size_t)(x1 - x0) * 4u; }   // g_fl_row (satoru)
    for (int row = 0; row < h; row++) {
        uint8_t* d = dbase + (size_t)(y + row) * f.pitch + (size_t)x0 * 4u;
        fl_nt_copy(d, fl_src_row(f, src, spitch, x0, y + row, x1 - x0, tone), n);
    }
    g_fl.copy_bytes += (uint64_t)n * (uint64_t)h;
    g_fl_tot.bytes += (uint64_t)n * (uint64_t)h;
}

void fl_list_clear(FlList& l) { l.n = 0; l.full = false; }

// [flipwhy]: why flips copied the whole slice, once a second while any did. (satoru)
uint32_t g_flw_part = 0, g_flw_null = 0, g_flw_retone = 0, g_flw_area = 0, g_flw_prev = 0, g_flw_merge = 0;
Lapse g_flw_lapse; bool g_flw_started = false;

// add a compositor-frame rect to a pipe's list in slice coordinates. the
// list collapses to "the whole slice" when it overflows or covers most of it. (satoru)
void fl_list_add(FlPipe& f, FlList& l, int x, int y, int w, int h) {
    if (l.full) return;
    int x0 = x - f.ox, y0 = y - f.oy, x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > f.ow) x1 = f.ow;
    if (y1 > f.oh) y1 = f.oh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < l.n; i++) {
        FlRect& r = l.r[i];
        if (x0 >= r.x && y0 >= r.y && x1 <= r.x + r.w && y1 <= r.y + r.h) return;   // already covered (satoru)
    }
    // damage scissor: overlapping rects fold into their union, so the area rule
    // below measures pixels and not the same pixels several times (a drag frame
    // listed ~30 overlapping rects and went to the full slice 59 times of 182). (satoru)
    if (Graphics::ScissorEnabled()) {
        for (int i = 0; i < l.n; i++) {
            FlRect& r = l.r[i];
            if (x0 >= r.x + r.w || r.x >= x1 || y0 >= r.y + r.h || r.y >= y1) continue;
            int ux0 = r.x < x0 ? r.x : x0, uy0 = r.y < y0 ? r.y : y0;
            int ux1 = r.x + r.w > x1 ? r.x + r.w : x1, uy1 = r.y + r.h > y1 ? r.y + r.h : y1;
            l.r[i] = l.r[--l.n];     // take it out, add the union again (satoru)
            fl_list_add(f, l, ux0 + f.ox, uy0 + f.oy, ux1 - ux0, uy1 - uy0);
            return;
        }
    }
    if (l.n >= FL_RECTS) {
        // fold into the rect whose box grows least instead of copying the whole
        // slice (16 mb at 2560x1600); the area rule below still goes full. (satoru)
        int best = 0; uint64_t bg = ~0ull;
        for (int i = 0; i < l.n; i++) {
            FlRect& r = l.r[i];
            int ux0 = r.x < x0 ? r.x : x0, uy0 = r.y < y0 ? r.y : y0;
            int ux1 = r.x + r.w > x1 ? r.x + r.w : x1, uy1 = r.y + r.h > y1 ? r.y + r.h : y1;
            uint64_t g = (uint64_t)(ux1 - ux0) * (uint64_t)(uy1 - uy0) - (uint64_t)r.w * (uint64_t)r.h;
            if (g < bg) { bg = g; best = i; }
        }
        FlRect& r = l.r[best];
        int ux0 = r.x < x0 ? r.x : x0, uy0 = r.y < y0 ? r.y : y0;
        int ux1 = r.x + r.w > x1 ? r.x + r.w : x1, uy1 = r.y + r.h > y1 ? r.y + r.h : y1;
        r = FlRect{ ux0, uy0, ux1 - ux0, uy1 - uy0 };
        g_flw_merge++;
        g_fl_tot.merges++;
    } else {
        l.r[l.n++] = FlRect{ x0, y0, x1 - x0, y1 - y0 };
    }
    uint64_t area = 0;
    for (int i = 0; i < l.n; i++) area += (uint64_t)l.r[i].w * (uint64_t)l.r[i].h;
    if (area * 5 >= (uint64_t)f.ow * (uint64_t)f.oh * 3) l.full = true;
}

void fl_copy_list(FlPipe& f, int k, const FlList& l, const uint8_t* src, uint32_t spitch) {
    for (int i = 0; i < l.n; i++) fl_copy_rect(f, k, src, spitch, l.r[i].x, l.r[i].y, l.r[i].w, l.r[i].h);
}

void fl_copy_full(FlPipe& f, int k, const uint8_t* src, uint32_t spitch) {
    fl_copy_rect(f, k, src, spitch, 0, 0, f.ow, f.oh);
}

// what the pipe is scanning now (hardware), or the qemu stand-in: a request
// latches after its simulated vblank delay, and the latch copies the chosen
// buffer onto the real framebuffer like a scanout would show it. (satoru)
uint32_t fl_live(FlPipe& f) {
    if (!g_fl.test) {
        uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
        return rd(b + P_PL_SURFLIVE - 0x70000) & 0xFFFFF000u;
    }
    // the stand-in "vblank" times on the raw tsc: a vm's tscs agree, and the
    // same-cpu lapse never advanced while the gui loop hopped cpus, which
    // held stand-in flips for seconds. hardware never takes this path. (satoru)
    if (f.test_req >= 0 && (tsc() - f.test_req_tsc) / (g_tpu ? g_tpu : 3000) >= f.test_lat_us) {
        int k = f.test_req;
        f.test_req = -1;
        const uint8_t* s = (const uint8_t*)(uintptr_t)f.phys[k];
        if (f.test_fb) {
            for (int y = 0; y < f.oh; y++)
                memcpy(f.test_fb + (size_t)y * f.test_pitch, s + (size_t)y * f.pitch, (size_t)f.ow * 4u);
        }
        f.test_live = k;
    }
    return f.test_live >= 0 ? f.ggtt[f.test_live] : 0;
}

void fl_arm(FlPipe& f, int k) {
    if (!g_fl.test) {
        uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
        f.arm_frm = rd(b + P_FRMCNT - 0x70000);
        f.arm_flipcnt = rd(b + P_FLIPCNT - 0x70000);
        f.arm_dsl = rd(b + P_DCY - 0x70000) & 0xFFFFFu;
        f.arm_live = rd(b + P_PL_SURFLIVE - 0x70000);
        f.arm_hw_us = rd(R_TIMESTAMP_CTR);
        wr(b + P_PL_SURF - 0x70000, f.ggtt[k]);   // armed at the next vblank (satoru)
    } else {
        f.test_req = k;
        lapse_start(f.test_req_lapse);
        f.test_req_tsc = tsc();
    }
    f.pending = true;
    lapse_start(f.pend);
    f.arm_cpu = f.pend.cpu;
    f.arm_tsc = f.pend.tsc;
}

bool fl_window_owned(uint32_t lo, uint32_t hi) {
    auto hit = [&](uint32_t s, uint32_t e) { return lo < e && hi > s; };
    for (uint32_t i = 0; i < PIPE_N; i++) {
        uint32_t g = g_pipes[i].ggtt & ~0xFFFFFFu;
        if (g_pipes[i].ggtt && hit(g, g + 0x1000000u)) return true;
        const FlPipe& f = g_fl.pipe[i];
        for (int k = 0; k < 2; k++) if (f.mapped[k] && hit(f.ggtt[k], f.ggtt[k] + FL_WIN)) return true;
    }
    uint32_t mg = 0;
    if (IntelModeset::GetOutputGgtt(&mg) && mg) {
        uint32_t s = mg & ~0xFFFFFFu;
        if (hit(s, s + 0x1000000u)) return true;
    }
    if (g_zc.win_npte && hit(g_zc.win_ggtt, g_zc.win_ggtt + g_zc.win_npte * 0x1000u)) return true;
    for (const XOwn& x : g_xown)
        if (x.npte && hit(x.ggtt & ~0xFFFFFFu, (x.ggtt & ~0xFFFFFFu) + FL_WIN)) return true;
    return false;
}

// pick and map one 16 mb window for buffer k of pipe f. (satoru)
bool fl_map_window(FlPipe& f, int k) {
    for (uint32_t cand = FL_REGION_LO; cand < FL_REGION_HI; cand += FL_WIN) {
        if (fl_window_owned(cand, cand + FL_WIN)) continue;
        if (window_used_by_display(cand, cand + FL_WIN)) {
            Ln l; l.t.s("flip: ggtt window 0x"); l.t.x(cand, 8); l.t.s(" is referenced by an enabled plane, skipping"); l.go();
            continue;
        }
        uint32_t idx = cand >> 12;
        for (uint32_t pi = idx; pi < idx + f.npte; pi += 512)
            if (!gsm_map(pi)) { emit("FAIL flip: cannot map the gsm pte pages"); return false; }
        if (!gsm_map(idx + f.npte - 1)) { emit("FAIL flip: cannot map the gsm pte pages"); return false; }
        uint64_t* save = (uint64_t*)(uintptr_t)f.saved[k];
        uint64_t phys_top = PMM::GetPhysTop();
        uint32_t live = 0;
        for (uint32_t i = 0; i < f.npte; i++) {
            uint64_t pte = *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8);
            save[i] = pte;
            if (IgpuCalc::GgttPteLive(pte, phys_top)) live++;
        }
        for (uint32_t i = 0; i < f.npte; i++)
            *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) =
                IgpuCalc::MtlGgttPte(f.phys[k] + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC);
        __asm__ __volatile__("mfence" ::: "memory");
        bool ok = true;
        for (uint32_t i = 0; i < f.npte && ok; i++)
            if (*(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) !=
                IgpuCalc::MtlGgttPte(f.phys[k] + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC)) ok = false;
        if (!ok) {
            for (uint32_t i = 0; i < f.npte; i++)
                *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) = save[i];
            __asm__ __volatile__("mfence" ::: "memory");
            Ln l; l.t.s("FAIL flip: ggtt window 0x"); l.t.x(cand, 8); l.t.s(" pte read back mismatch, restored"); l.go();
            return false;
        }
        f.ggtt[k] = cand;
        f.mapped[k] = true;
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" buffer "); l.t.d((uint64_t)k);
        l.t.s(": ggtt 0x"); l.t.x(cand, 8); l.t.s(" -> phys 0x"); l.t.x((uint32_t)(f.phys[k] >> 32), 8);
        l.t.x((uint32_t)f.phys[k], 8); l.t.s(", "); l.t.d((uint64_t)f.npte); l.t.s(" ptes written and read back (");
        l.t.d((uint64_t)live); l.t.s(" stale entries saved)"); l.go();
        return true;
    }
    emit("FAIL flip: no ggtt window in the reserved 1-3 gb region is free of display references");
    return false;
}

void fl_unmap_window(FlPipe& f, int k) {
    if (!f.mapped[k]) return;
    uint32_t idx = f.ggtt[k] >> 12;
    const uint64_t* save = (const uint64_t*)(uintptr_t)f.saved[k];
    for (uint32_t i = 0; i < f.npte; i++)
        *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) = save[i];
    __asm__ __volatile__("mfence" ::: "memory");
    f.mapped[k] = false;
}

void fl_release_pipe(FlPipe& f) {
    for (int k = 0; k < 2; k++) {
        if (!g_fl.test) fl_unmap_window(f, k);
        if (f.saved[k] && f.saved_frames) PMM::FreeContiguous(f.saved[k], f.saved_frames);
        if (f.phys[k] && f.frames) PMM::FreeContiguous(f.phys[k], f.frames);
        f.saved[k] = 0;
        f.phys[k] = 0;
    }
    f.used = false;
}

// put every pipe back on its original surface, wait (bounded) for the latch
// so the buffers are no longer fetched, then release ptes and memory. (satoru)
void fl_stop_locked(const char* why) {
    if (!g_fl.active) return;
    WrLog wl;
    g_fl.active = false;
    Graphics::SetFlipHooks(nullptr, nullptr);
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used || g_fl.test || !g_fl.seeded) continue;
        uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
        wr(b + P_PL_SURF - 0x70000, f.s_surf);
    }
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used || g_fl.test || !g_fl.seeded) continue;
        uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
        uint32_t last = 0;
        if (!wait_reg(b + P_PL_SURFLIVE - 0x70000, 0xFFFFF000u, f.s_surf & 0xFFFFF000u, 60000, &last)) {
            Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" restore surflive 0x"); l.t.x(last, 8);
            l.t.s(" not yet 0x"); l.t.x(f.s_surf, 8); l.go();
        }
    }
    for (uint32_t i = 0; i < PIPE_N; i++) if (g_fl.pipe[i].used) fl_release_pipe(g_fl.pipe[i]);
    g_fl.seeded = false;
    Ln l; l.t.s("flip: off ("); l.t.s(why); l.t.s("), original surfaces and ptes restored, cpu present back"); l.go();
}

// panic: raw mmio only, the surfaces the panic screen draws into. (satoru)
void fl_panic_restore() {
    if (!g_fl.active || !g_fl.seeded || g_fl.test || !g_mmio) return;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
        *(volatile uint32_t*)(g_mmio + b + P_PL_SURF - 0x70000) = f.s_surf;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    g_fl.active = false;
}

void accel_panic_restore() {
    cursor_hide_all_raw();
    zc_panic_restore();
    fl_panic_restore();
}

uint64_t fl_hash_slice(const uint8_t* base, uint32_t pitch, int w, int h) {
    uint64_t v = 0xcbf29ce484222325ull;
    for (int y = 0; y < h; y++) {
        const uint64_t* p = (const uint64_t*)(base + (size_t)y * pitch);
        for (int x = 0; x < w / 2; x++) v = (v ^ p[x]) * 0x100000001b3ull;
    }
    return v;
}

// the same hash over what the slice should show: the back buffer through the
// output's tone. (satoru)
uint64_t fl_hash_src(FlPipe& f, const uint8_t* src, uint32_t spitch) {
    bool tone = Graphics::ToneActive(f.out);
    uint64_t v = 0xcbf29ce484222325ull;
    for (int y = 0; y < f.oh; y++) {
        const uint64_t* p = (const uint64_t*)fl_src_row(f, src, spitch, 0, y, f.ow, tone);
        for (int x = 0; x < f.ow / 2; x++) v = (v ^ p[x]) * 0x100000001b3ull;
    }
    return v;
}

// where the front buffer and the frame disagree: pixel count and bounding
// box in slice coordinates, for the mismatch line. (satoru)
uint32_t fl_diff_box(FlPipe& f, const uint8_t* src, uint32_t spitch, int* bx0, int* by0, int* bx1, int* by1) {
    bool tone = Graphics::ToneActive(f.out);
    const uint8_t* fb = (const uint8_t*)(uintptr_t)f.phys[f.front];
    uint32_t n = 0;
    int x0 = f.ow, y0 = f.oh, x1 = -1, y1 = -1;
    for (int y = 0; y < f.oh; y++) {
        const uint32_t* a = (const uint32_t*)(fb + (size_t)y * f.pitch);
        const uint32_t* b = (const uint32_t*)fl_src_row(f, src, spitch, 0, y, f.ow, tone);
        for (int x = 0; x < f.ow; x++) {
            if (a[x] == b[x]) continue;
            n++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    *bx0 = x0; *by0 = y0; *bx1 = x1 + 1; *by1 = y1 + 1;
    return n;
}

// the front buffer just latched and nothing is queued for it: it must hold
// exactly the compositor frame. a mismatch means the damage bookkeeping lost
// a rect, so the flip path turns itself off. the read lines are flushed again
// so later non-temporal stores never meet a cached copy. (satoru)
bool fl_check(FlPipe& f, const uint8_t* src, uint32_t spitch) {
    if (f.checks_left <= 0 || (f.check_spaced && lapse_us(f.check_lapse) < 1000000ull)) return true;
    const uint8_t* fb = (const uint8_t*)(uintptr_t)f.phys[f.front];
    uint64_t a = fl_hash_slice(fb, f.pitch, f.ow, f.oh);
    uint64_t c = fl_hash_src(f, src, spitch);
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    uint32_t dn = 0;
    if (a != c) dn = fl_diff_box(f, src, spitch, &dx0, &dy0, &dx1, &dy1);
    for (size_t o = 0; o < (size_t)f.pitch * (size_t)f.oh; o += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"(fb + o) : "memory");
    f.checks_left--;
    lapse_start(f.check_lapse);
    f.check_spaced = true;
    if (a != c) {
        // one mismatch can be a draw that never marked damage (the cpu path
        // would hide it just the same); repaint the whole slice on the next
        // flip and only give up when it keeps happening. (satoru)
        f.mismatches++;
        Ln l; l.t.s(f.mismatches >= 3 ? "FAIL flip: pipe " : "flip: pipe "); l.t.c((char)('A' + f.id));
        l.t.s(" selfcheck MISMATCH "); l.t.d((uint64_t)f.mismatches); l.t.s("/3 after ");
        l.t.d((uint64_t)f.flips); l.t.s(" flips (front 0x"); l.t.x((uint32_t)(a >> 32), 8); l.t.x((uint32_t)a, 8);
        l.t.s(" frame 0x"); l.t.x((uint32_t)(c >> 32), 8); l.t.x((uint32_t)c, 8); l.t.s(")");
        l.t.s(" "); l.t.d((uint64_t)dn); l.t.s(" px differ in (");
        l.t.d((uint64_t)dx0); l.t.c(','); l.t.d((uint64_t)dy0); l.t.s(")-(");
        l.t.d((uint64_t)dx1); l.t.c(','); l.t.d((uint64_t)dy1); l.t.s(") of the slice");
        if (f.mismatches < 3) l.t.s(", full re-render queued");
        l.go();
        if (f.mismatches >= 3) return false;
        // repair from a full re-render, not a copy of a back buffer that may
        // itself hold the stale pixels. (satoru)
        Graphics::RequestFullRepaint();
        f.acc.full = true;
        f.checks_left++;   // the repaired frame gets checked too (satoru)
        return true;
    }
    f.checks_ok++;
    if (f.checks_left == 0) {
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" selfcheck PASS, front buffer == compositor frame ");
        l.t.d((uint64_t)f.checks_ok); l.t.c('/'); l.t.d((uint64_t)FL_CHECKS); l.t.s(" times, ");
        l.t.d((uint64_t)f.flips); l.t.s(" flips, max latch "); l.t.d((uint64_t)f.latch_us_max); l.t.s(" us"); l.go();
    }
    return true;
}

// one pass over the pipes: notice latches, then copy and flip every pipe
// whose previous flip has landed. false when the path had to turn off. (satoru)
bool fl_service_locked(const uint8_t* src, uint32_t spitch) {
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        if (f.pending) {
            uint32_t hw = 0, cpu = 0;
            uint64_t waited = fl_waited_us(f, &hw, &cpu);
            // one line of evidence, once: a flip armed on one cpu and looked at
            // on another, where the raw rdtsc difference disagrees with the
            // cpu-independent clocks. that is the firmware tsc_adjust offset
            // that used to turn this path off. (satoru)
            if (!g_fl.skew_logged && cpu != f.arm_cpu) {
                int64_t raw = (int64_t)(tsc() - f.arm_tsc) / (int64_t)(g_tpu ? g_tpu : 3000);
                int64_t off = raw - (int64_t)hw;
                bool proven = (!g_fl.test && g_fl.hw_clock_ok) ? (off > 20000 || off < -20000) : (raw < -1000);
                if (proven) {
                    g_fl.skew_logged = true;
                    Ln l; l.t.s("flip: note: cpu "); l.t.d((uint64_t)cpu); l.t.s(" checks a flip armed on cpu ");
                    l.t.d((uint64_t)f.arm_cpu); l.t.s(": it waited "); l.t.d(waited); l.t.s(" us (");
                    l.t.s((!g_fl.test && g_fl.hw_clock_ok) ? "display clock" : "lower bound"); l.t.s("), raw rdtsc says ");
                    fl_log_signed(l.t, raw);
                    l.t.s(" us (rdtsc differs across cpus; the flip timeout no longer uses it that way)"); l.go();
                }
            }
            if (fl_live(f) == f.ggtt[f.front]) {
                f.pending = false;
                uint32_t us = (uint32_t)waited;
                if (us > f.latch_us_max) f.latch_us_max = us;
                if (us > g_fl.latch_us_max) g_fl.latch_us_max = us;
                if (!f.acc.n && !f.acc.full && !fl_check(f, src, spitch)) {
                    g_fl.fails++;
                    fl_stop_locked("selfcheck mismatch");
                    return false;
                }
            } else if (waited > FL_LATCH_TIMEOUT_US) {
                Ln l; l.t.s("FAIL flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" surflive 0x");
                l.t.x(fl_live(f), 8); l.t.s(" did not latch 0x"); l.t.x(f.ggtt[f.front], 8);
                l.t.s(" after "); l.t.d(waited / 1000); l.t.s(" ms of real waiting"); l.go();
                fl_fail_detail(f);
                g_fl.fails++;
                fl_stop_locked("latch timeout");
                return false;
            }
        }
        if (!f.acc.n && !f.acc.full) continue;
        if (f.pending) { f.defers++; g_fl.defers++; g_fl_tot.defers++; continue; }
        int back = 1 - f.front;
        uint64_t t0 = tsc();
        g_fl_tot.flips++;
        if (f.prev.full || f.acc.full) {
            g_fl_tot.full_flips++;
            fl_copy_full(f, back, src, spitch);
        } else {
            fl_copy_list(f, back, f.prev, src, spitch);
            fl_copy_list(f, back, f.acc, src, spitch);
        }
        __asm__ __volatile__("sfence" ::: "memory");
        fl_arm(f, back);
        f.front = back;
        f.prev = f.acc;
        fl_list_clear(f.acc);
        f.flips++;
        g_fl.flips++;
        g_fl.copy_cycles += tsc() - t0;
    }
    return true;
}

// first complete frame: both buffers get the whole slice, buffer 0 is armed
// and the latch is awaited (bounded) before anything is reported enabled. (satoru)
bool fl_seed_locked(const uint8_t* src, uint32_t spitch) {
    WrLog wl;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        fl_copy_full(f, 0, src, spitch);
        fl_copy_full(f, 1, src, spitch);
    }
    __asm__ __volatile__("sfence" ::: "memory");
    g_fl.seeded = true;   // from here a stop restores the original surfaces (satoru)
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        f.front = 0;
        fl_arm(f, 0);
    }
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        while (fl_live(f) != f.ggtt[0]) {
            if (fl_waited_us(f) > FL_LATCH_TIMEOUT_US) {
                Ln l; l.t.s("FAIL flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" surflive 0x");
                l.t.x(fl_live(f), 8); l.t.s(" did not latch the first flip buffer 0x"); l.t.x(f.ggtt[0], 8); l.go();
                fl_fail_detail(f);
                g_fl.fails++;
                fl_stop_locked("first latch timeout");
                return false;
            }
            udelay(50);
        }
        f.pending = false;
        uint32_t us = (uint32_t)fl_waited_us(f);
        f.latch_us_max = us;
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" latched flip buffer 0 (ggtt 0x");
        l.t.x(f.ggtt[0], 8); l.t.s(") in "); l.t.d((uint64_t)us); l.t.s(" us (armed on cpu ");
        l.t.d((uint64_t)f.arm_cpu); l.t.s(")"); l.go();
    }
    int n = 0;
    for (uint32_t i = 0; i < PIPE_N; i++) if (g_fl.pipe[i].used) n++;
    Ln l; l.t.s("ENABLED page flip present: "); l.t.d((uint64_t)n);
    l.t.s(" pipe(s), two buffers each, vblank-latched PLANE_SURF flips, no drawing into scanned memory");
    if (g_fl.test) l.t.s(" (qemu stand-in scanout)");
    l.go();
    return true;
}

bool fl_present_hook(const Graphics::Rect* rects, int count) {
    SpinLockCpuGuard g(g_fl_lock);
    if (!g_fl.active) return false;
    const uint8_t* src = Graphics::GetBackBuffer();
    uint32_t spitch = Graphics::GetPitch();
    if (!src || !spitch) { fl_stop_locked("no back buffer"); return false; }
    if (!g_fl.seeded) { g_fl.tone_sig = Graphics::ToneSignature(); return fl_seed_locked(src, spitch); }
    // a filter, dim or night light change repaints every slice through the
    // new tone. (satoru)
    uint32_t tsig = Graphics::ToneSignature();
    bool retone = tsig != g_fl.tone_sig;
    g_fl.tone_sig = tsig;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        if (retone || !rects || count <= 0) {
            if (retone) g_flw_retone++; else g_flw_null++;
            f.acc.full = true; continue;
        }
        bool was_full = f.acc.full;
        for (int r = 0; r < count; r++) fl_list_add(f, f.acc, rects[r].x, rects[r].y, rects[r].w, rects[r].h);
        if (f.acc.full && !was_full) g_flw_area++;
        else if (!f.acc.full && f.prev.full) g_flw_prev++;
        else if (!f.acc.full) g_flw_part++;
    }
    {
        if (!g_flw_started) { g_flw_started = true; lapse_start(g_flw_lapse); }
        if (lapse_us(g_flw_lapse) >= 1000000ull) {
            uint32_t full = g_flw_null + g_flw_retone + g_flw_area + g_flw_prev;
            uint32_t ov = 0, col = 0;
            Graphics::TakeDirtyOverflowStats(&ov, &col);
            uint32_t eskip = Graphics::TakeFlipEmptySkips();
            if (full || g_flw_merge || ov || eskip) {
                Ln l; l.t.s("[flipwhy] partial="); l.t.d(g_flw_part); l.t.s(" full: no-rects="); l.t.d(g_flw_null);
                l.t.s(" retone="); l.t.d(g_flw_retone); l.t.s(" area60="); l.t.d(g_flw_area); l.t.s(" prev-full="); l.t.d(g_flw_prev);
                l.t.s(" | flip-list merges="); l.t.d(g_flw_merge); l.t.s(" | dirty-list overflows="); l.t.d(ov);
                l.t.s(" collapsed-full="); l.t.d(col);
                l.t.s(" | empty frames skipped="); l.t.d(eskip); l.go();
            }
            lapse_start(g_flw_lapse);
            g_flw_part = g_flw_null = g_flw_retone = g_flw_area = g_flw_prev = g_flw_merge = 0;
        }
    }
    return fl_service_locked(src, spitch);
}

// relayout / scale / mode change: everything goes back and is released; the
// gui loop re-arms the flip once the new layout has settled. (satoru)
void fl_suspend_hook() {
    SpinLockCpuGuard g(g_fl_lock);
    if (!g_fl.active) return;
    fl_stop_locked("compositor buffer change");
    if (g_fl_flag || g_fl_test_flag) {
        g_fl.rearm_pending = true;
        lapse_start(g_fl.rearm_lapse);
    }
}

int fl_pipe_for(uint64_t phys, int w, int h) {
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.probed || p.failed || !p.lit) continue;
        if (p.phys != phys) continue;
        if ((int)p.w != w || (int)p.h != h) continue;
        return (int)i;
    }
    return -1;
}

bool fl_alloc_pipe(FlPipe& f) {
    f.bytes = (uint64_t)f.pitch * (uint64_t)f.oh;
    f.npte = (uint32_t)((f.bytes + 0xFFFu) >> 12);
    f.frames = f.npte;
    if (!f.npte || f.npte > FL_WIN / 0x1000u) {
        Ln l; l.t.s("FAIL flip: slot "); l.t.d((uint64_t)f.id); l.t.s(" needs "); l.t.d((uint64_t)f.npte);
        l.t.s(" pages, more than one 16 mb window"); l.go();
        return false;
    }
    f.saved_frames = (f.npte * 8u + 4095u) / 4096u;
    for (int k = 0; k < 2; k++) {
        f.phys[k] = PMM::AllocContiguous(f.frames);
        f.saved[k] = PMM::AllocContiguous(f.saved_frames);
        if (!f.phys[k] || !f.saved[k] || f.phys[k] + f.bytes > PMM::GetPhysTop()) {
            emit("FAIL flip: no contiguous memory for the flip buffers");
            return false;
        }
        // the allocator zeroed the pages through the cache. write those lines
        // back now so no dirty zero line can land on top of a later
        // non-temporal store. (satoru)
        const uint8_t* b = (const uint8_t*)(uintptr_t)f.phys[k];
        for (uint64_t o = 0; o < (uint64_t)f.frames * 4096u; o += 64)
            __asm__ __volatile__("clflush (%0)" : : "r"(b + o) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}

int fl_enable_impl() {
    WrLog wl;
    SpinLockCpuGuard g(g_fl_lock);
    g_fl.rearm_pending = false;
    if (g_fl.active) { emit("flip: already active"); return g_out_pos; }
    int test = g_fl_test_flag;
    emit(test ? "---- intel display acceleration stage 2b: page flip (qemu stand-in, kurono.igpu.fliptest=1) ----"
              : "---- intel display acceleration stage 2b: double-buffered page flip ----");
    if (!test) {
        if (!g_mmio) { emit("flip: no usable intel display engine, skipped"); return g_out_pos; }
        if (!g_gsm.ok) { emit("FAIL flip: ggtt is not available (needs the kurono.igpu.accel=1 probe)"); return g_out_pos; }
        if (g_zc.active) { emit("FAIL flip: zero-copy (direct) is active, use one of the two"); return g_out_pos; }
    }
    char why[64];
    if (!Graphics::FlipUsable(why, sizeof(why))) {
        Ln l; l.t.s("FAIL flip: compositor refused: "); l.t.s(why); l.go();
        return g_out_pos;
    }
    for (uint32_t i = 0; i < PIPE_N; i++) g_fl.pipe[i] = {};
    g_fl.flips = g_fl.defers = 0;
    g_fl.copy_bytes = g_fl.copy_cycles = 0;
    g_fl.latch_us_max = 0;
    g_fl.test = test;
    g_fl.hw_clock_ok = false;
    g_fl.skew_logged = false;
    if (!test) {
        // the display engine's microsecond counter is the second clock of the
        // latch timeout. it only counts when it moves at the tsc's rate. (satoru)
        uint32_t a = rd(R_TIMESTAMP_CTR);
        udelay(400);
        uint32_t d = rd(R_TIMESTAMP_CTR) - a;
        g_fl.hw_clock_ok = d >= 300 && d <= 2000;
        Ln l; l.t.s("flip: display clock (0x44070) moved "); l.t.d((uint64_t)d); l.t.s(" us across a 400 us tsc delay: ");
        l.t.s(g_fl.hw_clock_ok ? "it times the latch (the same on every cpu)"
                               : "NOT usable, same-cpu tsc lapses alone time the latch"); l.go();
    }
    bool multi = Graphics::MultiActive();
    int used = 0;
    int nout = Outputs::Total();
    int setup_out = 0;
    auto setup = [&](FlPipe& f, int id, int ox, int oy, int ow, int oh, uint32_t pitch) {
        f.used = true; f.id = id; f.out = setup_out;
        f.ox = ox; f.oy = oy; f.ow = ow; f.oh = oh;
        f.pitch = pitch;
        f.test_req = -1; f.test_live = -1;
        f.checks_left = FL_CHECKS; f.check_spaced = false;
        fl_list_clear(f.acc); fl_list_clear(f.prev);
    };
    bool bad = false;
    if (nout > 0 && (multi || nout == 1)) {
        for (int oi = 0; oi < nout && !bad; oi++) {
            const Output* o = Outputs::Get(oi);
            if (!o || !o->connected) continue;
            setup_out = oi;
            if (o->w != o->pw || o->h != o->ph) { emit("FAIL flip: an output is not 1:1 (ui scale)"); bad = true; break; }
            int ox = multi ? o->x : 0, oy = multi ? o->y : 0;
            if (ox + o->w > Graphics::GetWidth() || oy + o->h > Graphics::GetHeight()) {
                emit("FAIL flip: an output lies outside the compositor frame"); bad = true; break;
            }
            if (test) {
                if (used >= (int)PIPE_N) { emit("flip: qemu test drives the first two outputs only"); break; }
                FlPipe& f = g_fl.pipe[used];
                setup(f, used, ox, oy, o->w, o->h, ((uint32_t)o->w * 4u + 63u) & ~63u);
                f.test_fb = multi ? o->fb : Graphics::GetFrontBuffer();
                f.test_pitch = multi ? o->pitch : Graphics::GetPitch();
                f.test_lat_us = used == 0 ? 4000 : 16700;   // a 240 hz and a 60 hz pipe (satoru)
                f.ggtt[0] = 0x1000u * (uint32_t)(used * 2 + 1);
                f.ggtt[1] = 0x1000u * (uint32_t)(used * 2 + 2);
                used++;
                continue;
            }
            int pi = fl_pipe_for(o->phys, o->pw, o->ph);
            if (pi < 0) {
                Ln l; l.t.s("FAIL flip: output "); l.t.d((uint64_t)oi); l.t.s(" is not on a probed intel pipe"); l.go();
                bad = true; break;
            }
            FlPipe& f = g_fl.pipe[pi];
            if (f.used) { emit("FAIL flip: two outputs map to one pipe"); bad = true; break; }
            setup(f, pi, ox, oy, o->w, o->h, g_pipes[pi].stride);
            used++;
        }
    } else if (!test && g_pipes[0].probed && !g_pipes[0].failed &&
               (int)g_pipes[0].w == Graphics::GetWidth() && (int)g_pipes[0].h == Graphics::GetHeight()) {
        setup(g_fl.pipe[0], 0, 0, 0, (int)g_pipes[0].w, (int)g_pipes[0].h, g_pipes[0].stride);
        used = 1;
    } else if (test) {
        FlPipe& f = g_fl.pipe[0];
        setup(f, 0, 0, 0, Graphics::GetWidth(), Graphics::GetHeight(), ((uint32_t)Graphics::GetWidth() * 4u + 63u) & ~63u);
        f.test_fb = Graphics::GetFrontBuffer();
        f.test_pitch = Graphics::GetPitch();
        f.test_lat_us = 4000;
        f.ggtt[0] = 0x1000u; f.ggtt[1] = 0x2000u;
        used = 1;
    }
    if (bad || !used) {
        if (!bad) emit("FAIL flip: no output on a probed pipe");
        for (uint32_t i = 0; i < PIPE_N; i++) g_fl.pipe[i] = {};
        return g_out_pos;
    }
    for (uint32_t i = 0; i < PIPE_N && !bad; i++) {
        FlPipe& f = g_fl.pipe[i];
        if (!f.used) continue;
        if ((f.pitch & 63) || f.pitch < (uint32_t)f.ow * 4u) {
            Ln l; l.t.s("FAIL flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" stride "); l.t.d((uint64_t)f.pitch);
            l.t.s(" is not a 64 byte multiple of the width"); l.go();
            bad = true; break;
        }
        if (!fl_alloc_pipe(f)) { bad = true; break; }
        if (!test) {
            uint32_t b = 0x70000 + (uint32_t)f.id * 0x1000;
            f.s_surf = rd(b + P_PL_SURF - 0x70000);
            if (!fl_map_window(f, 0) || !fl_map_window(f, 1)) { bad = true; break; }
        }
        Ln l; l.t.s("flip: pipe "); l.t.c((char)('A' + f.id)); l.t.s(" slice ("); l.t.d((uint64_t)f.ox); l.t.c(',');
        l.t.d((uint64_t)f.oy); l.t.s(") "); l.t.d((uint64_t)f.ow); l.t.c('x'); l.t.d((uint64_t)f.oh);
        l.t.s(" stride "); l.t.d((uint64_t)f.pitch); l.t.s(" (plane stride/size/offset unchanged), surf 0x");
        l.t.x(f.s_surf, 8); l.t.s(" saved"); l.go();
    }
    if (bad) {
        for (uint32_t i = 0; i < PIPE_N; i++) if (g_fl.pipe[i].used) fl_release_pipe(g_fl.pipe[i]);
        return g_out_pos;
    }
    g_fl.active = true;
    g_fl.seeded = false;
    if (!test) KernelPanic::SetDisplayRestoreHook(accel_panic_restore);
    Graphics::SetFlipHooks(fl_present_hook, fl_suspend_hook);
    emit("ARMED page flip: planes switch to the flip buffers on the next complete frame");
    return g_out_pos;
}

} // namespace

// ---- public api (satoru) ----

int IntelAccel::Boot(bool enabled, bool pte_fallback, uint64_t fb_phys, uint32_t fb_w, uint32_t fb_h, uint32_t fb_pitch) {
    begin_out(nullptr, 0);
    g_fb_phys = fb_phys; g_fb_w = fb_w; g_fb_h = fb_h; g_fb_pitch = fb_pitch;
    g_pte_fallback = pte_fallback;
    if (!enabled) return 0;                       // default: no output, no write (satoru)
    if (g_boot_once) return 0;
    g_boot_once = true;
    clock_init();
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) {
        IntelDisplayProbe::Run(nullptr, 0);
        g_mmio = IntelDisplayProbe::Mmio();
    }
    if (!g_mmio) { emit("no usable intel display engine, skipped"); return 0; }
    // fixed command line kill switch: KUROACC.OFF in the KURONO fat root (satoru)
    if (DiskLog::Ready()) {
        uint64_t sz = 0;
        if (DiskLog::StatRootFile("KUROACCOFF", &sz) == 0) { emit("skipped (KUROACC.OFF found on the KURONO partition)"); return 0; }
    }
    return SelfTest(nullptr, 0);
}

int IntelAccel::Enable(char* out, int maxo) {
    begin_out(out, maxo);
    g_zc_runtime_off = false;   // `on` re-arms the zero-copy boot request (satoru)
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) { IntelDisplayProbe::Run(nullptr, 0); g_mmio = IntelDisplayProbe::Mmio(); }
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    return SelfTest(out, maxo);
}

int IntelAccel::SelfTest(char* out, int maxo) {
    WrLog wl;
    bool had_out = (out != nullptr);
    if (!had_out) begin_out(nullptr, 0); else begin_out(out, maxo);
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    clock_init();
    // zero-copy redirected the primary planes: restore them before probing the
    // original scanout ptes again (this is not a permanent off). (satoru)
    zc_stop(false);
    FlipDisable("self-test re-probes the surfaces");
    // park the runtime updater while registers are reprogrammed; the long
    // waits below must not run with interrupts disabled, so the lock is only
    // taken around this flag flip and around the final enable. (satoru)
    { SpinLockCpuGuard g(g_lock); g_enabled = false; }
    g_blocked = 0;
    emit("---- intel display acceleration stage 3a: hardware cursor self-test ----");
    if (!g_tpu) emit("warning: no tsc frequency, waits use the io-delay clock");
    uint32_t pw = rd(R_PWR_WELL_CTL2);
    bool pw1 = (pw & 1) != 0, pw2 = (pw & 2) != 0;
    uint32_t dc = rd(R_DC_STATE_EN);
    {
        Ln l; l.t.s("gates: pw1="); l.t.d((uint64_t)pw1); l.t.s(" pw2="); l.t.d((uint64_t)pw2);
        l.t.s(" dc_state_en 0x"); l.t.x(dc, 8); l.go();
    }
    if (!pw1 || !pw2) { emit("FAIL power wells: pw1/pw2 are not both up, refusing"); return g_out_pos; }
    if (dc & 3) { emit("FAIL dc states: dc5/dc6 enabled, display registers are not trustworthy"); return g_out_pos; }
    emit("PASS display engine and power: ver 14 mmio, pw1/pw2 up, dc off");
    if (!gsm_open()) { emit("FAIL ggtt: gsm base is not available, cannot validate scanout ptes"); return g_out_pos; }
    emit("PASS ggtt: gsm base read, pte reads are bounded and read only");
    bool psr = (rd(R_SRD_CTL_A) & (1u << 31)) || (rd(R_PSR2_CTL_A) & (1u << 31));
    if (psr) { emit("FAIL psr: psr/psr2 is enabled on pipe a, cursor updates could be swallowed"); return g_out_pos; }
    emit("PASS psr: psr and psr2 are off");

    allow_reset();
    for (uint32_t i = 0; i < PIPE_N; i++) allow_pipe(i);
    snapshot_pipe_a(g_pipe_a_before);

    // pipe a: the gop framebuffer passed at boot. pipe b: stage 2, if lit. (satoru)
    bool any = false;
    uint64_t b_phys = 0; int bw = 0, bh = 0; uint32_t bpitch = 0;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        if (i == 0) {
            if (!g_fb_phys) { emit("pipe A: no boot framebuffer address, skipped"); continue; }
            if (probe_pipe(0, g_fb_phys)) {
                emit("PASS pipe A: lit, plane ptes match the gop framebuffer, cursor slot validated");
                any = true;
            } else emit("FAIL pipe A: framebuffer not usable for a hardware cursor");
        } else {
            if (!IntelModeset::GetOutput(&b_phys, &bw, &bh, &bpitch)) { emit("pipe B: not lit by stage 2, skipped"); continue; }
            {
                Ln l; l.t.s("pipe B stage 2 output: "); l.t.d((uint64_t)bw); l.t.c('x'); l.t.d((uint64_t)bh);
                l.t.s(" pitch "); l.t.d((uint64_t)bpitch); l.t.s(" phys 0x");
                l.t.x((uint32_t)(b_phys >> 32), 8); l.t.x((uint32_t)b_phys, 8); l.go();
            }
            if (probe_pipe(1, b_phys)) {
                emit("PASS pipe B: lit, plane ptes match the stage 2 framebuffer, cursor slot validated");
                any = true;
            } else emit("FAIL pipe B: framebuffer not usable for a hardware cursor");
        }
    }
    if (!any) { emit("summary: FAILED no pipe has a usable hardware cursor, software path stays"); return g_out_pos; }

    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        if (!p.probed) continue;
        if (p.ready) { disable_pipe(&p); }
        if (enable_pipe(&p, false)) { any = true; }
        else { p.ready = false; }
    }
    int changed = 0;
    diff_pipe_a("after self-test", true, &changed);
    int ready = 0;
    for (uint32_t i = 0; i < PIPE_N; i++) if (g_pipes[i].ready) ready++;
    {
        Ln l; l.t.s("summary: "); l.t.d((uint64_t)ready); l.t.s(" pipe(s) ready, ");
        l.t.d((uint64_t)g_blocked); l.t.s(" blocked writes, pipe a changed "); l.t.d((uint64_t)changed);
        l.go();
    }
    if (!ready || changed) {
        for (uint32_t i = 0; i < PIPE_N; i++) disable_pipe(&g_pipes[i]);
        emit("FAILED hardware cursor disabled, software path stays");
        set_last_error("self-test failed");
        return g_out_pos;
    }
    { SpinLockCpuGuard g(g_lock); g_enabled = true; g_visible = true; }
    emit("ENABLED hardware cursor (hidden until the pointer moves)");
    // re-arm the stage 2 request that `on` (or the boot flag) asked for. (satoru)
    if (g_zc_flag && !g_zc_runtime_off) ZeroCopyEnable(nullptr, 0);
    if (g_fl_flag && !g_zc.active) fl_enable_impl();
    return g_out_pos;
}

int IntelAccel::Off(char* out, int maxo) {
    WrLog wl;
    begin_out(out, maxo);
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    clock_init();
    // restore the scanned surfaces before the cursor registers: zero-copy owns
    // its own saved plane state and must run outside the cursor lock. (satoru)
    ZeroCopyDisable();
    FlipDisable("gpuaccel off");
    IntelGt::Off("gpuaccel off");   // gives its ggtt entries and forcewake back (satoru)
    SpinLockCpuGuard guard(g_lock);
    g_enabled = false;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        if (g_pipes[i].ready || g_pipes[i].saved || g_pipes[i].own_ptes) {
            Ln l; l.t.s("off: pipe "); l.t.c((char)('A' + i)); l.go();
            disable_pipe(&g_pipes[i]);
        }
    }
    int changed = 0;
    if (g_mmio) diff_pipe_a("after off", false, &changed);
    emit("summary: hardware cursor off, software path back in charge");
    return g_out_pos;
}

int IntelAccel::Status(char* out, int maxo) {
    begin_out(out, maxo);
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    {
        Ln l; l.t.s("status: "); l.t.s(g_enabled ? "enabled" : "off");
        l.t.s(" boot fb 0x"); l.t.x((uint32_t)(g_fb_phys >> 32), 8); l.t.x((uint32_t)g_fb_phys, 8);
        l.t.s(" "); l.t.d((uint64_t)g_fb_w); l.t.c('x'); l.t.d((uint64_t)g_fb_h);
        l.t.s(" | updates "); l.t.d((uint64_t)g_hw_updates); l.t.s(" sw "); l.t.d((uint64_t)g_sw_fallbacks);
        l.t.s(" fail "); l.t.d((uint64_t)g_failures); l.t.s(" blocked "); l.t.d((uint64_t)g_blocked);
        l.go();
    }
    {
        Ln l; l.t.s("zerocopy: ");
        l.t.s(g_zc.active ? (g_zc.committed ? "active" : "armed") : "off");
        if (g_zc.active) {
            l.t.s(" multi="); l.t.d((uint64_t)g_zc.multi);
            l.t.s(" commits "); l.t.d((uint64_t)g_zc.commits);
            l.t.s(" fails "); l.t.d((uint64_t)g_zc.fails);
            if (g_zc.win_npte) { l.t.s(" ggtt 0x"); l.t.x(g_zc.win_ggtt, 8); }
        }
        if (g_zc_runtime_off) l.t.s(" (runtime off)");
        l.go();
    }
    {
        Ln l; l.t.s("flip: ");
        l.t.s(g_fl.active ? (g_fl.seeded ? "active" : "armed") : "off");
        if (g_fl.test) l.t.s(" (qemu stand-in)");
        for (uint32_t i = 0; i < PIPE_N; i++) {
            const FlPipe& f = g_fl.pipe[i];
            if (!g_fl.active || !f.used) continue;
            l.t.s(" | pipe "); l.t.c((char)('A' + f.id)); l.t.s(" front "); l.t.d((uint64_t)f.front);
            l.t.s(" ggtt 0x"); l.t.x(f.ggtt[f.front], 8); l.t.s(" flips "); l.t.d((uint64_t)f.flips);
            l.t.s(" defers "); l.t.d((uint64_t)f.defers); l.t.s(" latch_max "); l.t.d((uint64_t)f.latch_us_max);
            l.t.s(" us checks "); l.t.d((uint64_t)f.checks_ok);
        }
        l.t.s(" | runtime mmio writes "); l.t.d((uint64_t)g_rt_writes);
        l.go();
    }
    for (uint32_t i = 0; i < PIPE_N; i++) {
        Pipe& p = g_pipes[i];
        Ln l; l.t.s("pipe "); l.t.c((char)('A' + i));
        l.t.s(p.lit ? " lit" : " off"); l.t.s(p.ready ? " ready" : " not-ready");
        l.t.s(p.live ? " live" : " hidden"); l.t.s(p.failed ? " failed" : "");
        if (p.ready) {
            l.t.s(" fb ggtt 0x"); l.t.x(p.ggtt, 8); l.t.s(" slot 0x"); l.t.x(p.slot_ggtt, 8);
            l.t.s(" ddb "); l.t.d((uint64_t)p.cur_ddb_start); l.t.c('-'); l.t.d((uint64_t)p.cur_ddb_end);
            l.t.s(" at "); l.t.d((uint64_t)(p.cur_x < 0 ? 0 : p.cur_x)); l.t.c(',');
            l.t.d((uint64_t)(p.cur_y < 0 ? 0 : p.cur_y));
        }
        l.go();
    }
    if (g_last_err[0]) { Ln l; l.t.s("last error: "); l.t.s(g_last_err); l.go(); }
    return g_out_pos;
}

bool IntelAccel::Enabled() { return g_enabled; }

// ---- stage 2 zero-copy public api (satoru) ----
int IntelAccel::ZeroCopyBoot(bool requested) {
    g_zc_flag = requested;
    if (!requested) return 0;                 // default: nothing runs, nothing logs (satoru)
    if (!g_enabled) {
        SerialLogger::Log("[igpu-zc] skipped: intel display acceleration is not active (kurono.igpu.accel=1 must pass its self-test)\r\n");
        return 0;
    }
    return zc_enable_impl();
}

int IntelAccel::ZeroCopyEnable(char* out, int maxo) {
    // append to a running command's output buffer (gpuaccel on -> self-test ->
    // zero-copy) instead of resetting it. (satoru)
    if (out) begin_out(out, maxo);
    else if (!g_out) begin_out(nullptr, 0);
    return zc_enable_impl();
}

void IntelAccel::ZeroCopyDisable() {
    g_zc_runtime_off = true;
    zc_stop(true);
}

bool IntelAccel::ZeroCopyActive() { return g_zc.active; }

void IntelAccel::TakeZeroCopyStats(uint32_t* commits, uint32_t* fails) {
    if (commits) *commits = g_zc.commits;
    if (fails) *fails = g_zc.fails;
    g_zc.commits = 0;
    g_zc.fails = 0;
}

// ---- stage 2b page flip public api (satoru) ----
void IntelAccel::SetWmCalc(bool on) { g_wm_calc = on; }

int IntelAccel::FlipBoot(bool requested, bool qemu_test) {
    g_fl_flag = requested;
    g_fl_test_flag = qemu_test ? 1 : 0;
    if (!requested && !qemu_test) return 0;   // default: nothing runs, nothing logs (satoru)
    clock_init();
    if (!qemu_test && !g_gsm.ok) {
        SerialLogger::Log("[igpu-accel] flip: skipped, the intel pipe probe did not run or failed (needs kurono.igpu.accel=1)\r\n");
        return 0;
    }
    begin_out(nullptr, 0);
    return fl_enable_impl();
}

int IntelAccel::FlipEnable(char* out, int maxo) {
    if (out) begin_out(out, maxo);
    else if (!g_out) begin_out(nullptr, 0);
    return fl_enable_impl();
}

void IntelAccel::FlipDisable(const char* why) {
    SpinLockCpuGuard g(g_fl_lock);
    g_fl.rearm_pending = false;
    fl_stop_locked(why ? why : "requested");
}

bool IntelAccel::FlipActive() { return g_fl.active; }

// ---- ggtt entries for the gt driver, out of the flip region (satoru) ----
// the same rules as a flip window: a 16 mb window nobody owns and no enabled
// plane or cursor references, every overwritten pte saved, every new pte read
// back. searched from the top of the region down, so the flip buffers keep the
// addresses they always had. (satoru)
bool IntelAccel::GgttReserve(uint64_t phys, uint32_t npte, int pat_index, uint32_t* ggtt_out, uint64_t* saved) {
    if (!g_mmio || !g_gsm.ok || !npte || npte > 64 || !ggtt_out || !saved) return false;
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!g_xown[i].npte) { slot = i; break; }
    if (slot < 0) return false;
    for (uint32_t cand = FL_REGION_HI - FL_WIN; cand >= FL_REGION_LO; cand -= FL_WIN) {
        if (fl_window_owned(cand, cand + FL_WIN)) continue;
        if (window_used_by_display(cand, cand + FL_WIN)) continue;
        uint32_t idx = cand >> 12;
        if (!gsm_map(idx) || !gsm_map(idx + npte - 1)) return false;
        uint64_t phys_top = PMM::GetPhysTop();
        uint32_t live = 0;
        for (uint32_t i = 0; i < npte; i++) {
            saved[i] = *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8);
            if (IgpuCalc::GgttPteLive(saved[i], phys_top)) live++;
        }
        for (uint32_t i = 0; i < npte; i++)
            *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) =
                IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096, pat_index);
        __asm__ __volatile__("mfence" ::: "memory");
        bool ok = true;
        for (uint32_t i = 0; i < npte && ok; i++)
            if (*(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) !=
                IgpuCalc::MtlGgttPte(phys + (uint64_t)i * 4096, pat_index)) ok = false;
        if (!ok) {
            for (uint32_t i = 0; i < npte; i++)
                *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + i) * 8) = saved[i];
            __asm__ __volatile__("mfence" ::: "memory");
            Ln l; l.t.s("FAIL ggtt reserve: window 0x"); l.t.x(cand, 8); l.t.s(" pte read back mismatch, restored"); l.go();
            return false;
        }
        g_xown[slot] = XOwn{ cand, npte };
        *ggtt_out = cand;
        Ln l; l.t.s("ggtt reserve: "); l.t.d((uint64_t)npte); l.t.s(" entries at 0x"); l.t.x(cand, 8); l.t.s(" -> phys 0x");
        l.t.x((uint32_t)(phys >> 32), 8); l.t.x((uint32_t)phys, 8); l.t.s(" pat "); l.t.d((uint64_t)pat_index);
        l.t.s(", written and read back ("); l.t.d((uint64_t)live); l.t.s(" stale entries saved)"); l.go();
        return true;
    }
    emit("FAIL ggtt reserve: no window in the reserved 1-3 gb region is free of display references");
    return false;
}

void IntelAccel::GgttRelease(uint32_t ggtt, uint32_t npte, const uint64_t* saved) {
    if (!g_gsm.ok || !saved) return;
    for (int i = 0; i < 4; i++) {
        if (g_xown[i].npte != npte || g_xown[i].ggtt != ggtt) continue;
        uint32_t idx = ggtt >> 12;
        for (uint32_t k = 0; k < npte; k++)
            *(volatile uint64_t*)(uintptr_t)(g_gsm.base + (uint64_t)(idx + k) * 8) = saved[k];
        __asm__ __volatile__("mfence" ::: "memory");
        g_xown[i] = XOwn{};
        return;
    }
}

bool IntelAccel::FlipBusy() {
    if (g_fl.rearm_pending) return true;
    if (!g_fl.active || !g_fl.seeded) return false;
    for (uint32_t i = 0; i < PIPE_N; i++) {
        const FlPipe& f = g_fl.pipe[i];
        if (f.used && (f.pending || f.acc.n || f.acc.full)) return true;
    }
    return false;
}

// gui loop, between frames, with the framebuffer lock held: finish deferred
// flips (a pipe whose last flip had not latched yet) and re-arm after a
// relayout once the layout has been stable for a moment. (satoru)
void IntelAccel::FlipService() {
    if (g_fl.rearm_pending) {
        if (lapse_us(g_fl.rearm_lapse) < 1500000ull) return;
        if (g_fl.rearms >= 4) {
            g_fl.rearm_pending = false;
            SerialLogger::Log("[igpu-accel] flip: not re-armed (4 relayouts already), cpu present stays\r\n");
            return;
        }
        g_fl.rearms++;
        begin_out(nullptr, 0);
        fl_enable_impl();
        return;
    }
    SpinLockCpuGuard g(g_fl_lock);
    if (!g_fl.active || !g_fl.seeded) return;
    const uint8_t* src = Graphics::GetBackBuffer();
    uint32_t spitch = Graphics::GetPitch();
    if (!src || !spitch) return;
    fl_service_locked(src, spitch);
}

void IntelAccel::GetFlipTotals(FlipTotals* out) { if (out) *out = g_fl_tot; }

void IntelAccel::TakeFlipStats(uint32_t* flips, uint32_t* defers, uint32_t* fails, uint64_t* copy_kb,
                               uint64_t* copy_cycles, uint32_t* latch_max_us) {
    SpinLockCpuGuard g(g_fl_lock);
    if (flips) *flips = g_fl.flips;
    if (defers) *defers = g_fl.defers;
    if (fails) *fails = g_fl.fails;
    if (copy_kb) *copy_kb = g_fl.copy_bytes / 1024u;
    if (copy_cycles) *copy_cycles = g_fl.copy_cycles;
    if (latch_max_us) *latch_max_us = g_fl.latch_us_max;
    g_fl.flips = g_fl.defers = 0;
    g_fl.copy_bytes = g_fl.copy_cycles = 0;
    g_fl.latch_us_max = 0;
}

bool IntelAccel::CursorUpdate(int x, int y) {
    if (!g_enabled) return false;
    uint64_t t0 = tsc();
    bool r;
    {
        SpinLockCpuGuard guard(g_lock);
        r = cursor_update_locked(x, y);
        if (!r && x >= 0) g_sw_fallbacks++;
    }
    g_gui_cycles += tsc() - t0;
    return r;
}

void IntelAccel::CursorHideAll() {
    // never spin here: a reset or shutdown path may run while the lock is
    // held (or on a wedged cpu). the raw writes are idempotent. (satoru)
    bool locked = g_lock.TryLock();
    cursor_hide_all_raw();
    if (locked) g_lock.Unlock();
}

void IntelAccel::CursorVisible(bool on) {
    if (!g_enabled) return;
    SpinLockCpuGuard guard(g_lock);
    g_visible = on;
    if (!on) for (uint32_t i = 0; i < PIPE_N; i++) cursor_disable_pipe(&g_pipes[i]);
}

uint64_t IntelAccel::TakeGuiStats(uint32_t* hw, uint32_t* sw, uint32_t* fail) {
    SpinLockCpuGuard guard(g_lock);
    if (hw) *hw = g_hw_updates;
    if (sw) *sw = g_sw_fallbacks;
    if (fail) *fail = g_failures;
    uint64_t c = g_gui_cycles;
    g_hw_updates = g_sw_fallbacks = g_failures = 0;
    g_gui_cycles = 0;
    return c;
}

const char* IntelAccel::LastError() { return g_last_err; }

// end (satoru)
