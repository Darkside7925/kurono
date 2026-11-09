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
#include "intel_display_probe.h"
#include "intel_modeset.h"
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
constexpr uint32_t P_DCY = 0x70000;
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
constexpr uint32_t CURSOR_POS_X_MASK = 0x7FFFu;
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

// ---- mmio with the write allowlist (satoru) ----
volatile uint8_t* g_mmio = nullptr;
struct Range { uint32_t lo, hi; };
Range g_allow[32];
int g_nallow = 0;
int g_blocked = 0;
bool g_log_writes = true;

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

uint32_t cursor_pos_val(int x, int y) {
    uint32_t v = 0;
    if (x < 0) v |= 1u << 15; else v |= (uint32_t)x & CURSOR_POS_X_MASK;
    if (y < 0) v |= 1u << 31; else v |= ((uint32_t)y & 0x7FFFu) << 16;
    return v;
}

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

void cursor_disable_pipe(Pipe* p) {
    if (!p->live) return;
    wr(0x70000 + p->id * 0x1000 + 0x080, 0);
    p->live = false;
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
        WmSet pl;
        if (!newsize || newsize < 64 || !compute_wms(p->pixclk_khz, p->htotal, p->w, newsize, &pl)) {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary plane wm does not fit the smaller ddb, refusing"); l.go();
            restore_cursor_regs(p);
            return false;
        }
        {
            Ln l; l.t.s("pipe "); l.t.c((char)('A' + p->id)); l.t.s(": primary ddb "); l.t.d((uint64_t)pstart);
            l.t.c('-'); l.t.d((uint64_t)pend); l.t.s(" -> "); l.t.d((uint64_t)pstart); l.t.c('-'); l.t.d((uint64_t)cstart);
            l.t.s(", wm0 blocks "); l.t.d((uint64_t)pl.wm[0].blocks); l.go();
        }
        write_plane_wms(b, pl);
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
    if (Outputs::Total() > 0) {
        int oi = Outputs::At(x, y);
        if (oi < 0) return -1;
        const Output* o = Outputs::Get(oi);
        int pi = pipe_for_output(oi, x, y);
        if (pi < 0) return -1;
        *lx = x - o->x;
        *ly = y - o->y;
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
    uint64_t target = (uint64_t)(uintptr_t)Graphics::GetBuffer();
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
    bool had_out = (out != nullptr);
    if (!had_out) begin_out(nullptr, 0); else begin_out(out, maxo);
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    clock_init();
    // zero-copy redirected the primary planes: restore them before probing the
    // original scanout ptes again (this is not a permanent off). (satoru)
    zc_stop(false);
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
    return g_out_pos;
}

int IntelAccel::Off(char* out, int maxo) {
    begin_out(out, maxo);
    if (!g_mmio) { emit("no usable intel display engine"); return g_out_pos; }
    clock_init();
    // restore the scanned surfaces before the cursor registers: zero-copy owns
    // its own saved plane state and must run outside the cursor lock. (satoru)
    ZeroCopyDisable();
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
