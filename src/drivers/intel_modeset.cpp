// intel display stage 2: light an external dp sink on a type-c port through
// pipe b. see intel_modeset.h for the contract. (satoru)
//
// linux sources (torvalds/linux master, drivers/gpu/drm/i915/display/
// unless noted), cited per step below:
//  - intel_display.c: hsw_crtc_enable, hsw_configure_cpu_transcoder,
//    intel_set_transcoder_timings, intel_set_pipe_src_size,
//    bdw_set_pipe_misc, hsw_set_transconf, intel_enable_transcoder,
//    intel_disable_transcoder, icl_set_pipe_chicken, hsw_set_linetime_wm,
//    intel_link_compute_m_n, intel_set_m_n.
//  - intel_ddi.c: mtl_ddi_pre_enable_dp, intel_ddi_init_dp_buf_reg,
//    mtl_port_buf_ctl_io_selection, mtl_port_buf_ctl_program,
//    mtl_ddi_prepare_link_retrain, mtl_ddi_enable_d2d / _disable_d2d,
//    intel_ddi_buf_enable / _disable, intel_ddi_enable,
//    intel_ddi_enable_transcoder_clock, intel_ddi_config_transcoder_func,
//    intel_ddi_transcoder_func_reg_val_get, intel_ddi_set_dp_msa,
//    intel_ddi_set_link_train, intel_ddi_set_idle_link_train,
//    intel_ddi_post_disable_dp, intel_ddi_disable_transcoder_func.
//  - intel_cx0_phy.c (+ _regs.h): intel_cx0pll_enable / _disable,
//    intel_program_port_clock_ctl, intel_cx0_phy_lane_reset,
//    intel_cx0_powerdown_change_sequence, intel_cx0_setup_powerdown,
//    intel_c20_pll_program, intel_c20_program_vdr_params,
//    intel_cx0_program_phy_lane, intel_cx0_phy_set_signal_levels, the
//    message bus (__intel_cx0_read_once / __intel_cx0_write_once,
//    intel_cx0_bus_reset, intel_cx0_wait_for_ack), mtl_c20_dp_tables.
//  - intel_tc.c: get_pin_assignment, mtl_get_max_lane_count,
//    tc_phy_load_fia_params (modular fia); intel_mg_phy_regs.h fia bases.
//  - intel_dp_link_training.c / intel_dp.c: see intel_dp_lt.cpp;
//    intel_dp_set_power, intel_dp_init_lttpr_and_dprx_caps.
//  - skl_universal_plane.c: icl_plane_update_noarm / _arm,
//    skl_write_plane_wm, skl_plane_ctl, glk_plane_color_ctl.
//  - skl_watermark.c: skl_compute_plane_wm, skl_crtc_allocate_plane_ddb,
//    adlp_allowed_dbufs, mbus_ddb_offset, pipe_mbus_dbox_ctl,
//    mtl_read_wm_latency, intel_sagv_block_time.
//  - intel_display_power.c: gen9_dbuf_slice_set, gen12_dbuf_slices_config.
//  - intel_pmdemand.c: intel_pmdemand_program_params (pre plane rule).
//  - intel_color.c: icl_color_commit_arm, icl_gamma_mode (no lut).
//  - gt/intel_ggtt.c: gen6_gttadr_offset, ggtt_probe_common,
//    mtl_ggtt_pte_encode; i915_utils.c i915_direct_stolen_access
//    (wa_22018444074); i915_pci.c MTL_CACHELEVEL; gem/i915_gem_domain.c
//    (scanout uses i915_cache_none without edram). (satoru)
#include "intel_modeset.h"
#include "intel_modeset_calc.h"
#include "intel_dp_lt.h"
#include "intel_dp_aux.h"
#include "intel_display_probe.h"
#include "dp_edid_decode.h"
#include "serial.h"
#include "cpu_detect.h"
#include "disklog.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"

namespace {

using DpEdid::Txt;
using IgpuCalc::Mode;
using IgpuCalc::LinkCfg;

// ---- global registers (intel_display_regs.h unless noted) (satoru) ----
constexpr uint32_t R_PWR_WELL_CTL2 = 0x45404;     // req 2*idx+1, state 2*idx (satoru)
constexpr int PW_1 = 0, PW_2 = 1, PW_A = 5, PW_B = 6;
constexpr uint32_t R_DC_STATE_EN = 0x45504;
constexpr uint32_t R_PICA_ISR = 0x16fe50;
constexpr uint32_t R_TCSS_DDI_STATUS = 0x161500;  // + 4 * tc_port (satoru)
constexpr uint32_t R_CDCLK_CTL = 0x46000;         // decimal 10:0 = (cdclk - 1000) / 500 (satoru)
constexpr uint32_t R_DISP_ARB_CTL2 = 0x45004;     // disp_ipc_enable bit 3 (satoru)
constexpr uint32_t R_PMDEMAND0 = 0x45230, R_PMDEMAND1 = 0x45234;
constexpr uint32_t PMDEMAND_REQ_ENABLE = 1u << 31;
constexpr uint32_t R_DCPR_STATUS_1 = 0x46440;     // pmdemand inflight bit 26 (satoru)
// skl_watermark_regs.h (satoru)
constexpr uint32_t R_MBUS_CTL = 0x4438C;          // mbus_join bit 31 (satoru)
constexpr uint32_t R_DBUF_CTL[4] = { 0x45008, 0x44FE8, 0x44300, 0x44304 };   // dbuf_ctl_s(0..3) (satoru)
constexpr uint32_t DBUF_POWER_REQUEST = 1u << 31, DBUF_POWER_STATE = 1u << 30;
constexpr uint32_t DBUF_TRACKERS_MASK = (0x1Fu << 19) | (0x7u << 16);   // tracker 23:19, min tracker 18:16 (satoru)
constexpr uint32_t R_MTL_LATENCY[3] = { 0x45780, 0x45784, 0x45788 };
constexpr uint32_t R_MTL_LATENCY_SAGV = 0x4578c;
// ggtt (i915_reg.h) (satoru)
constexpr uint32_t R_MTL_PCODE_STOLEN_ACCESS = 0x138914;   // == 1: firmware allows direct gsm access (satoru)
constexpr uint32_t R_GEN6_GSMBASE = 0x108100;              // 64 bit, gen11_bdsm_mask 63:20 (satoru)
constexpr uint64_t GGTT_BAR_OFFSET = 8ull << 20;           // gen6_gttadr_offset: 16 mb gttmmadr / 2 (satoru)

// ---- pipe a, read only (snapshot before / after) (satoru) ----
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

// ---- transcoder b / pipe b (trans offset 0x61000, pipe offset 0x71000) (satoru) ----
constexpr uint32_t TB = 0x61000, PB = 0x71000;
constexpr uint32_t T_HTOTAL = TB + 0x00, T_HBLANK = TB + 0x04, T_HSYNC = TB + 0x08, T_VTOTAL = TB + 0x0c;
constexpr uint32_t T_VBLANK = TB + 0x10, T_VSYNC = TB + 0x14, T_PIPESRC = TB + 0x1c, T_VSYNCSHIFT = TB + 0x28;
constexpr uint32_t T_MULT = TB + 0x2c, T_DATA_M1 = TB + 0x30, T_DATA_N1 = TB + 0x34, T_LINK_M1 = TB + 0x40;
constexpr uint32_t T_LINK_N1 = TB + 0x44, T_SCL = TB + 0x7c, T_DP2_CTL = TB + 0xa0;
constexpr uint32_t T_DDI_FUNC = TB + 0x400, T_DDI_FUNC2 = TB + 0x404, T_MSA_MISC = TB + 0x410;
constexpr uint32_t T_CHICKEN = TB + 0x4e0;        // _mtl_chicken_trans_b 0x614e0 (satoru)
constexpr uint32_t T_DP_TP_CTL = TB + 0x540, T_DP_TP_STATUS = TB + 0x544;   // tgl_dp_tp_ctl/status(b) (satoru)
constexpr uint32_t P_DSL = PB + 0x00, P_TRANSCONF = PB + 0x08, P_MISC = PB + 0x30, P_BOTTOM = PB + 0x34;
constexpr uint32_t P_CHICKEN = PB + 0x38, P_DBOX = PB + 0x3c, P_CUR_CTL = PB + 0x80;
constexpr uint32_t PL_CTL = PB + 0x180, PL_STRIDE = PB + 0x188, PL_POS = PB + 0x18c, PL_SIZE = PB + 0x190;
constexpr uint32_t PL_KEYVAL = PB + 0x194, PL_KEYMSK = PB + 0x198, PL_SURF = PB + 0x19c, PL_KEYMAX = PB + 0x1a0;
constexpr uint32_t PL_OFFSET = PB + 0x1a4, PL_SURFLIVE = PB + 0x1ac, PL_AUX_DIST = PB + 0x1c0;
constexpr uint32_t PL_CUS_CTL = PB + 0x1c8, PL_COLOR_CTL = PB + 0x1cc, PL_WM0 = PB + 0x240;
constexpr uint32_t PL_WM_SAGV = PB + 0x258, PL_WM_SAGV_TRANS = PB + 0x25c, PL_WM_TRANS = PB + 0x268;
constexpr uint32_t PL_BUF_CFG = PB + 0x27c;
constexpr uint32_t R_TRANS_CLK_SEL_B = 0x46144, R_WM_LINETIME_B = 0x45274;
constexpr uint32_t R_GAMMA_MODE_B = 0x4ac80, R_CSC_MODE_B = 0x49128;   // intel_color_regs.h (satoru)
constexpr uint32_t R_DSS_CTL1_B = 0x78200;        // intel_vdsc_regs.h _icl_pipe_dss_ctl1_pb (satoru)
constexpr uint32_t TRANSCONF_ENABLE = 1u << 31, TRANSCONF_STATE = 1u << 30;

// trans_ddi_func_ctl (satoru)
constexpr uint32_t DDI_FUNC_ENABLE = 1u << 31, DDI_FUNC_PORT_MASK = 0xFu << 27, DDI_FUNC_MODE_MASK = 7u << 24;
constexpr uint32_t DDI_MODE_DP_SST = 2u << 24, DDI_BPC_8 = 0, DDI_PVSYNC = 1u << 17, DDI_PHSYNC = 1u << 16;
// dp_tp_ctl (satoru)
constexpr uint32_t TP_ENABLE = 1u << 31, TP_ENHANCED = 1u << 18, TP_TRAIN_MASK = 7u << 8;
constexpr uint32_t TP_PAT1 = 0u << 8, TP_PAT2 = 1u << 8, TP_IDLE = 2u << 8, TP_NORMAL = 3u << 8;
constexpr uint32_t TP_PAT3 = 4u << 8, TP_PAT4 = 5u << 8;
constexpr uint32_t TP_STATUS_IDLE_DONE = 1u << 25;
// chicken_trans (satoru)
constexpr uint32_t CT_FRAME_START_DELAY_MASK = 3u << 27, CT_FECSTALL_DIS = 1u << 23;
constexpr uint32_t CT_DP_FEC_BS_JITTER_WA = 1u << 15, CT_DSC_INSERT_SF_EOL_WA = 1u << 4;
// pipe_chicken: per_pixel_alpha_bypass bit 7, pixel_rounding_trunc_fb_passthru 15,
// underrun_recovery_disable_adlp 30 (satoru)
constexpr uint32_t PIPE_CHICKEN_SET = (1u << 7) | (1u << 15) | (1u << 30);
// plane (skl_universal_plane_regs.h) (satoru)
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31, PLANE_FMT_XRGB8888 = 4u << 24;
constexpr uint32_t PLANE_COLOR_PLANE_GAMMA_DISABLE = 1u << 13;

// ---- type-c port registers (intel_cx0_phy_regs.h, display ver 14 index =
// enum port: tc1 = 3) (satoru) ----
struct Port {
    int tc;                 // 1..4 (satoru)
    int port;               // enum port, tc1 = 3 (satoru)
    uint32_t base;          // xelpdp usbc block 0x16f200 + 0x200 * (tc - 1) (satoru)
    uint32_t buf1, buf2, buf3, aux, m2p[2], p2m[2], timer[2], clk;
    uint32_t ddi_buf, valfreq;
    uint32_t fia_pa1; int fia_idx;
};

void make_port(int tc, Port* p) {
    p->tc = tc;
    p->port = 2 + tc;
    p->base = 0x16F200 + 0x200u * (uint32_t)(tc - 1);
    p->buf1 = p->base + 0x00; p->buf2 = p->base + 0x04; p->buf3 = p->base + 0x08;
    p->aux = p->base + 0x10;
    for (int l = 0; l < 2; l++) {
        p->m2p[l] = p->base + 0x40 + 4u * (uint32_t)l;     // xelpdp_port_m2p_msgbus_ctl (satoru)
        p->p2m[l] = p->base + 0x48 + 4u * (uint32_t)l;     // xelpdp_port_p2m_msgbus_status (satoru)
        p->timer[l] = p->base + 0x58 + 4u * (uint32_t)l;   // xelpdp_port_msgbus_timer (satoru)
    }
    p->clk = p->base + 0x60;                               // xelpdp_port_clock_ctl (satoru)
    p->ddi_buf = 0x64000 + 0x100u * (uint32_t)p->port;      // ddi_buf_ctl (ddi_ctl_de) (satoru)
    p->valfreq = 0x64030 + 0x100u * (uint32_t)p->port;      // ddi_clk_valfreq (satoru)
    // modular fia: fia = tc_port / 2, idx = tc_port % 2 (tc_phy_load_fia_params);
    // fia1 0x163000, fia2 0x16e000, fia3 0x16f000 (intel_mg_phy_regs.h);
    // port_tx_dflexpa1 = fia + 0x880 (satoru)
    int tcp = tc - 1, fia = tcp / 2;
    uint32_t fia_base = fia == 0 ? 0x163000u : (fia == 1 ? 0x16E000u : 0x16F000u);
    p->fia_pa1 = fia_base + 0x880;
    p->fia_idx = tcp % 2;
}

// port_buf_ctl1 bits (satoru)
constexpr uint32_t BUF1_D2D_ENABLE = 1u << 29, BUF1_D2D_STATE = 1u << 28, BUF1_SOC_PHY_READY = 1u << 24;
constexpr uint32_t BUF1_DATA_WIDTH_MASK = 3u << 18, BUF1_REVERSAL = 1u << 16, BUF1_IO_TBT = 1u << 11;
constexpr uint32_t BUF1_PHY_IDLE = 1u << 7, BUF1_TC_OWNED = 1u << 6, BUF1_TCSS_PWR_ST = 1u << 4;
constexpr uint32_t BUF1_WIDTH_MASK = 7u << 1;
// port_buf_ctl2 (satoru)
inline uint32_t lane_pipe_reset(int lane) { return lane ? 1u << 30 : 1u << 31; }
inline uint32_t lane_cur_status(int lane) { return lane ? 1u << 28 : 1u << 29; }
inline uint32_t lane_pd_update(int lane) { return lane ? 1u << 24 : 1u << 25; }
inline uint32_t lane_pd_state(int lane, uint32_t v) { return lane ? (v & 0xF) << 16 : (v & 0xF) << 20; }
constexpr uint32_t P2_STATE_READY = 0x2, P2_STATE_RESET = 0x2, P4PG_STATE_DISABLE = 0xC;
// port_clock_ctl (satoru)
inline uint32_t pclk_pll_req(int lane) { return 1u << (31 - lane * 4); }
inline uint32_t pclk_pll_ack(int lane) { return 1u << (30 - lane * 4); }
inline uint32_t pclk_refclk_req(int lane) { return 1u << (29 - lane * 4); }
inline uint32_t pclk_refclk_ack(int lane) { return 1u << (28 - lane * 4); }
constexpr uint32_t CLK_DDI_SEL_MASK = 0xFu << 12, CLK_DDI_SEL_MAXPCLK = 0x8u << 12;
constexpr uint32_t CLK_FWD_UNGATE = 1u << 10, CLK_LANE1_PHY_SEL = 1u << 8, CLK_SSC_A = 1u << 1, CLK_SSC_B = 1u << 0;
// message bus (satoru)
constexpr uint32_t M2P_PENDING = 1u << 31, M2P_RESET = 1u << 15;
constexpr uint32_t M2P_CMD_WRITE_UNCOMMITTED = 1u << 27, M2P_CMD_WRITE_COMMITTED = 2u << 27, M2P_CMD_READ = 3u << 27;
constexpr uint32_t P2M_RESPONSE_READY = 1u << 31, P2M_ERROR = 1u << 15;
constexpr uint32_t P2M_READ_ACK = 0x4, P2M_WRITE_ACK = 0x5;
constexpr uint8_t LANE0 = 1, LANE1 = 2, BOTH_LANES = 3;
// phy registers behind the bus (satoru)
constexpr uint16_t C20_WR_ADDR_L = 0xC02, C20_WR_ADDR_H = 0xC03, C20_WR_DATA_L = 0xC04, C20_WR_DATA_H = 0xC05;
constexpr uint16_t C20_VDR_SERDES = 0xD00, C20_VDR_WIDTH = 0xD02, C10_VDR_OVRD = 0xD71;
constexpr uint8_t C20_IS_HDMI_FRL = 1u << 7, C20_IS_DP = 1u << 6, C20_DP_RATE_MASK = 0xF << 1, C20_CONTEXT_TOGGLE = 1;

// ---- output (satoru) ----
char* g_out = nullptr;
int g_out_max = 0, g_out_pos = 0;

void emit(const char* line) {
    SerialLogger::Log("[igpu-mode] ");
    SerialLogger::Log(line);
    SerialLogger::Log("\r\n");
    if (g_out && g_out_pos < g_out_max - 2) {
        static const char pre[] = "[igpu-mode] ";
        for (int i = 0; pre[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = pre[i];
        for (int i = 0; line[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = line[i];
        g_out[g_out_pos++] = '\n';
        g_out[g_out_pos] = 0;
    }
}

struct Ln {
    char b[320];
    Txt t;
    Ln() : t(b, sizeof(b)) {}
    void go() { emit(b); }
};

void say(const char* s) { emit(s); }

// ---- time: same tsc / io-delay clock as stage 1 (satoru) ----
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

uint64_t now_us() { return g_tpu ? tsc() / g_tpu : 0; }

// the whole Light run gets this wall budget; steps check it between waits.
// a wait in progress is still bounded by its own timeout. (satoru)
constexpr uint64_t RUN_BUDGET_US = 4000000;
uint64_t g_run_deadline = 0;
bool over_budget() { return g_tpu && g_run_deadline && now_us() > g_run_deadline; }

// ---- mmio with the write allowlist (satoru) ----
volatile uint8_t* g_mmio = nullptr;
struct Range { uint32_t lo, hi; };   // inclusive (satoru)
Range g_allow[16];
int g_nallow = 0;
int g_writes = 0, g_blocked = 0;
bool g_log_writes = true;

void allow_reset() { g_nallow = 0; }
void allow_add(uint32_t lo, uint32_t hi) { if (g_nallow < 16) g_allow[g_nallow++] = Range{ lo, hi }; }
bool allowed(uint32_t off) {
    for (int i = 0; i < g_nallow; i++) if (off >= g_allow[i].lo && off <= g_allow[i].hi) return true;
    return false;
}

void build_allowlist(const Port& p) {
    allow_reset();
    allow_add(TB, TB + 0xFFF);                      // transcoder b (satoru)
    allow_add(PB, PB + 0xFFF);                      // pipe b, its planes and cursor (satoru)
    allow_add(p.base, p.base + 0xFF);               // the type-c port block (buf_ctl, aux, msgbus, clock) (satoru)
    allow_add(p.ddi_buf, p.ddi_buf);
    allow_add(p.valfreq, p.valfreq);
    allow_add(R_TRANS_CLK_SEL_B, R_TRANS_CLK_SEL_B);
    allow_add(R_WM_LINETIME_B, R_WM_LINETIME_B);
    allow_add(R_GAMMA_MODE_B, R_GAMMA_MODE_B);
    allow_add(R_CSC_MODE_B, R_CSC_MODE_B);
    allow_add(R_DSS_CTL1_B, R_DSS_CTL1_B);
    allow_add(R_DBUF_CTL[2], R_DBUF_CTL[2]);
    allow_add(R_DBUF_CTL[3], R_DBUF_CTL[3]);
    allow_add(R_PMDEMAND0, R_PMDEMAND1);
    allow_add(R_PWR_WELL_CTL2, R_PWR_WELL_CTL2);    // only the pw_b request bit is ever set, see power_wells (satoru)
}

inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_mmio + off); }

bool wr(uint32_t off, uint32_t v) {
    if (!allowed(off)) {
        g_blocked++;
        Ln l; l.t.s("BLOCKED write 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8); l.t.s(" (not on the stage 2 allowlist)"); l.go();
        return false;
    }
    uint32_t old = g_log_writes ? rd(off) : 0;
    *(volatile uint32_t*)(g_mmio + off) = v;
    __asm__ __volatile__("" ::: "memory");
    g_writes++;
    if (g_log_writes) {
        Ln l; l.t.s("wr 0x"); l.t.x(off, 6); l.t.s(" = 0x"); l.t.x(v, 8); l.t.s(" (was 0x"); l.t.x(old, 8); l.t.s(")"); l.go();
    }
    return true;
}

bool rmw(uint32_t off, uint32_t clear, uint32_t set) {
    uint32_t v = rd(off);
    uint32_t n = (v & ~clear) | set;
    return wr(off, n);
}

// quiet variants for the message bus and polling, still allowlisted (satoru)
bool wr_q(uint32_t off, uint32_t v) {
    bool save = g_log_writes;
    g_log_writes = false;
    bool ok = wr(off, v);
    g_log_writes = save;
    return ok;
}
bool rmw_q(uint32_t off, uint32_t clear, uint32_t set) { return wr_q(off, (rd(off) & ~clear) | set); }

// intel_de_wait: (val & mask) == want within timeout_us. bounded by the tsc
// and an iteration cap (each poll costs at least one mmio read). (satoru)
bool wait_reg(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t* last = nullptr) {
    uint64_t start = now_us();
    uint32_t v = rd(off);
    uint32_t polls = timeout_us + 16;
    while ((v & mask) != want && polls--) {
        if (g_tpu && now_us() - start > timeout_us) break;
        udelay(1);
        v = rd(off);
    }
    if (last) *last = v;
    return (v & mask) == want;
}

// ---- state (satoru) ----
enum Step : uint32_t {
    S_GGTT = 1u << 0, S_PMDEMAND = 1u << 1, S_DBUF = 1u << 2, S_AUXPWR = 1u << 3, S_PLL = 1u << 4,
    S_IOSEL = 1u << 5, S_TRANS_CLK = 1u << 6, S_FUNC_CFG = 1u << 7, S_SINK_D0 = 1u << 8, S_DDI_BUF = 1u << 9,
    S_PIPE_CFG = 1u << 10, S_FUNC_EN = 1u << 11, S_TRANS_EN = 1u << 12, S_PLANE = 1u << 13, S_DP_TP = 1u << 14,
};

struct State {
    bool lit;
    uint32_t steps;
    Port port;
    Mode mode;
    LinkCfg link;
    uint8_t owned;          // phy lane mask (satoru)
    int max_lanes;
    char pin;               // dp-alt pin assignment letter (satoru)
    // framebuffer (satoru)
    uint64_t fb_phys;
    uint64_t fb_bytes;      // 2 mb multiple (satoru)
    uint32_t pitch;
    int fb_w, fb_h;
    // ggtt (satoru)
    uint64_t gsm_phys;
    bool gsm_direct;
    uint32_t ggtt_off;      // window start (ggtt address) (satoru)
    uint32_t npte;
    uint64_t saved_pte;     // the uniform value the window held (satoru)
    // misc (satoru)
    bool aux_was_on;
    uint32_t pipe_a_before[N_PIPE_A_REGS];
    uint8_t dpcd[16];
    int lttpr_count;
    uint8_t lttpr_caps[8];
    uint8_t lttpr_phy[8][3];
    bool enhanced;
    uint8_t train_set[4];
    char last_result[96];
};

State g_s;
bool g_mapped_gsm_pages[2048];   // which 4 kb pages of the 8 mb gsm we remapped uc (satoru)

// ---- message bus (intel_cx0_phy.c) (satoru) ----
bool g_mb_err = false;
int g_mb_ops = 0;

void mb_clear_response(int lane) {
    // intel_cx0_clear_response_ready_flag: rmw set response_ready | error (w1c) (satoru)
    rmw_q(g_s.port.p2m[lane], 0, P2M_RESPONSE_READY | P2M_ERROR);
}

void mb_bus_reset(int lane) {
    // intel_cx0_bus_reset (satoru)
    wr_q(g_s.port.m2p[lane], M2P_RESET);
    if (!wait_reg(g_s.port.m2p[lane], M2P_RESET, 0, 1000)) {
        Ln l; l.t.s("msgbus lane "); l.t.d((uint64_t)lane); l.t.s(" failed to go idle after reset"); l.go();
        return;
    }
    mb_clear_response(lane);
}

int mb_wait_ack(int lane, uint32_t cmd, uint32_t* val) {
    // intel_cx0_wait_for_ack (xelpdp_msgbus_timeout_ms = 1) (satoru)
    if (!wait_reg(g_s.port.p2m[lane], P2M_RESPONSE_READY, P2M_RESPONSE_READY, 1000, val)) { mb_bus_reset(lane); return -1; }
    if (*val & P2M_ERROR) { mb_bus_reset(lane); return -2; }
    if (((*val >> 27) & 0xF) != cmd) { mb_bus_reset(lane); return -3; }
    return 0;
}

int mb_read_once(int lane, uint16_t addr) {
    // __intel_cx0_read_once (satoru)
    uint32_t val = 0;
    if (!wait_reg(g_s.port.m2p[lane], M2P_PENDING, 0, 1000)) { mb_bus_reset(lane); return -1; }
    mb_clear_response(lane);
    wr_q(g_s.port.m2p[lane], M2P_PENDING | M2P_CMD_READ | (addr & 0xFFF));
    int ack = mb_wait_ack(lane, P2M_READ_ACK, &val);
    if (ack < 0) return ack;
    mb_clear_response(lane);
    mb_bus_reset(lane);   // display ver < 30 settle workaround (satoru)
    return (int)((val >> 16) & 0xFF);
}

int mb_read(int lane, uint16_t addr) {
    g_mb_ops++;
    for (int i = 0; i < 3; i++) {
        int v = mb_read_once(lane, addr);
        if (v >= 0) return v;
    }
    g_mb_err = true;
    Ln l; l.t.s("phy lane "); l.t.d((uint64_t)lane); l.t.s(" read 0x"); l.t.x(addr, 4); l.t.s(" failed after 3 tries"); l.go();
    return -1;
}

bool mb_write_once(int lane, uint16_t addr, uint8_t data, bool committed) {
    // __intel_cx0_write_once (satoru)
    uint32_t val = 0;
    if (!wait_reg(g_s.port.m2p[lane], M2P_PENDING, 0, 1000)) { mb_bus_reset(lane); return false; }
    mb_clear_response(lane);
    wr_q(g_s.port.m2p[lane], M2P_PENDING | (committed ? M2P_CMD_WRITE_COMMITTED : M2P_CMD_WRITE_UNCOMMITTED) |
                             ((uint32_t)data << 16) | (addr & 0xFFF));
    if (!wait_reg(g_s.port.m2p[lane], M2P_PENDING, 0, 1000)) { mb_bus_reset(lane); return false; }
    if (committed) {
        if (mb_wait_ack(lane, P2M_WRITE_ACK, &val) < 0) return false;
    } else if (rd(g_s.port.p2m[lane]) & P2M_ERROR) {
        mb_bus_reset(lane);
        return false;
    }
    mb_clear_response(lane);
    mb_bus_reset(lane);
    return true;
}

bool mb_write(int lane, uint16_t addr, uint8_t data, bool committed) {
    g_mb_ops++;
    for (int i = 0; i < 3; i++) if (mb_write_once(lane, addr, data, committed)) return true;
    g_mb_err = true;
    Ln l; l.t.s("phy lane "); l.t.d((uint64_t)lane); l.t.s(" write 0x"); l.t.x(addr, 4); l.t.s(" failed after 3 tries"); l.go();
    return false;
}

void cx0_write(uint8_t mask, uint16_t addr, uint8_t data, bool committed) {
    for (int lane = 0; lane < 2; lane++) if (mask & (1u << lane)) mb_write(lane, addr, data, committed);
}

void cx0_rmw(uint8_t mask, uint16_t addr, uint8_t clear, uint8_t set, bool committed, bool log) {
    // intel_cx0_rmw -> __intel_cx0_rmw: read, write only when it changes (satoru)
    for (int lane = 0; lane < 2; lane++) {
        if (!(mask & (1u << lane))) continue;
        int old = mb_read(lane, addr);
        if (old < 0) continue;
        uint8_t v = (uint8_t)(((uint8_t)old & ~clear) | set);
        if (log) {
            Ln l; l.t.s("phy ln"); l.t.d((uint64_t)lane); l.t.s(" 0x"); l.t.x(addr, 4); l.t.s(": 0x"); l.t.x((uint32_t)old, 2);
            l.t.s(" -> 0x"); l.t.x(v, 2); l.go();
        }
        if (v != (uint8_t)old) mb_write(lane, addr, v, committed);
    }
}

void c20_sram_write(uint16_t addr, uint16_t data) {
    // intel_c20_sram_write on lane 0 (satoru)
    cx0_write(LANE0, C20_WR_ADDR_H, (uint8_t)(addr >> 8), false);
    cx0_write(LANE0, C20_WR_ADDR_L, (uint8_t)(addr & 0xFF), false);
    cx0_write(LANE0, C20_WR_DATA_H, (uint8_t)(data >> 8), false);
    cx0_write(LANE0, C20_WR_DATA_L, (uint8_t)(data & 0xFF), true);
}

void msgbus_timer() {
    // intel_cx0_program_msgbus_timer (in intel_cx0_phy_transaction_begin):
    // timer field 23:0 = 0xa000 on both lanes (satoru)
    for (int lane = 0; lane < 2; lane++) rmw_q(g_s.port.timer[lane], 0xFFFFFF, 0xA000);
}

// ---- c20 phy / pll (satoru) ----
void powerdown_change(uint8_t mask, uint32_t state) {
    // intel_cx0_powerdown_change_sequence (satoru)
    const Port& p = g_s.port;
    uint32_t clr = lane_pd_state(0, 0xF) | lane_pd_state(1, 0xF), set = 0, upd = 0;
    for (int lane = 0; lane < 2; lane++) if (mask & (1u << lane)) { set |= lane_pd_state(lane, state); upd |= lane_pd_update(lane); }
    rmw(p.buf2, clr, set);
    for (int lane = 0; lane < 2; lane++)
        if ((mask & (1u << lane)) && !wait_reg(p.m2p[lane], M2P_PENDING, 0, 1000)) mb_bus_reset(lane);
    rmw(p.buf2, lane_pd_update(0) | lane_pd_update(1), upd);
    uint32_t last = 0;
    if (!wait_reg(p.buf2, upd, 0, 2000, &last)) {
        Ln l; l.t.s("warn: powerdown change to 0x"); l.t.x(state, 1); l.t.s(" did not complete (buf_ctl2 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
}

void set_signal_levels(const uint8_t ts[4], int lanes) {
    // intel_cx0_phy_set_signal_levels, c20 branch: per dp lane ln, phy lane
    // ln / 2, tx ln % 2, vdrovrd ctl 0xd80 + ((lane ^ tx) * 0x10) + control
    // (0 pre cursor, 1 vswing, 2 post cursor), field 5:0, then the override
    // enables tx1 | tx2 in 0xd71 (satoru)
    msgbus_timer();
    for (int ln = 0; ln < lanes; ln++) {
        int level = IgpuCalc::TrainSetToLevel(ts[ln]);
        if (level < 0) level = 0;
        int lane = ln / 2, tx = ln % 2;
        uint8_t m = lane ? LANE1 : LANE0;
        if (!(m & g_s.owned)) continue;
        IgpuCalc::Snps s = IgpuCalc::C20Dp14Level(level);
        uint16_t base = (uint16_t)(0xD80 + ((lane ^ tx) * 0x10));
        cx0_rmw(m, (uint16_t)(base + 0), 0x3F, s.pre_cursor, true, false);
        cx0_rmw(m, (uint16_t)(base + 1), 0x3F, s.vswing, true, false);
        cx0_rmw(m, (uint16_t)(base + 2), 0x3F, s.post_cursor, true, false);
    }
    cx0_rmw(g_s.owned, C10_VDR_OVRD, 0, 0x01 | 0x04, true, false);
    Ln l; l.t.s("signal levels:");
    for (int ln = 0; ln < lanes; ln++) {
        int level = IgpuCalc::TrainSetToLevel(ts[ln]);
        IgpuCalc::Snps s = IgpuCalc::C20Dp14Level(level < 0 ? 0 : level);
        l.t.s(" ln"); l.t.d((uint64_t)ln); l.t.s(" ts 0x"); l.t.x(ts[ln], 2); l.t.s(" lvl "); l.t.d((uint64_t)(level < 0 ? 0 : level));
        l.t.s(" (v"); l.t.d(s.vswing); l.t.s(" pre"); l.t.d(s.pre_cursor); l.t.s(" post"); l.t.d(s.post_cursor); l.t.s(")");
    }
    l.go();
}

bool pll_enable(const IgpuCalc::C20Pll& pll, uint32_t rate, int lanes) {
    // intel_cx0pll_enable for a c20 phy in dp-alt (no lane reversal) (satoru)
    const Port& p = g_s.port;
    const uint8_t owned = g_s.owned;
    g_mb_err = false;
    {
        Ln l; l.t.s("pll: c20 "); l.t.s(pll.name); l.t.s(" port clock "); l.t.d(IgpuCalc::C20PortClock(pll));
        l.t.s(" khz (table "); l.t.d(pll.clock_khz); l.t.s("), lanes "); l.t.d((uint64_t)lanes);
        l.t.s(", owned phy lanes 0x"); l.t.x(owned, 1); l.t.s(", ssc off"); l.go();
    }
    msgbus_timer();
    {
        Ln l; l.t.s("pll: msgbus timer ln0 0x"); l.t.x(rd(p.timer[0]), 8); l.t.s(" ln1 0x"); l.t.x(rd(p.timer[1]), 8); l.go();
    }
    // 1. intel_program_port_clock_ctl: no reversal, forward clock ungate,
    // ddi clock select maxpclk, ssc off (no vbt says otherwise) (satoru)
    rmw(p.buf1, BUF1_REVERSAL, 0);
    rmw(p.clk, CLK_LANE1_PHY_SEL | CLK_FWD_UNGATE | CLK_DDI_SEL_MASK | CLK_SSC_A | CLK_SSC_B,
        CLK_FWD_UNGATE | CLK_DDI_SEL_MAXPCLK);
    // 2. intel_cx0_phy_lane_reset (satoru)
    uint32_t last = 0;
    if (!wait_reg(p.buf1, BUF1_SOC_PHY_READY, BUF1_SOC_PHY_READY, 1000, &last)) {
        Ln l; l.t.s("pll: phy not out of soc reset (buf_ctl1 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        return false;
    }
    uint32_t pipe_reset = lane_pipe_reset(0) | ((owned & LANE1) ? lane_pipe_reset(1) : 0);
    uint32_t cur_status = lane_cur_status(0) | ((owned & LANE1) ? lane_cur_status(1) : 0);
    rmw(p.buf2, pipe_reset, pipe_reset);
    if (!wait_reg(p.buf2, cur_status, cur_status, 100, &last)) {
        Ln l; l.t.s("warn: lane reset start not seen (buf_ctl2 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
    uint32_t ref_req_owned = pclk_refclk_req(0) | ((owned & LANE1) ? pclk_refclk_req(1) : 0);
    uint32_t ref_ack_owned = pclk_refclk_ack(0) | ((owned & LANE1) ? pclk_refclk_ack(1) : 0);
    rmw(p.clk, ref_req_owned, pclk_refclk_req(0));
    if (!wait_reg(p.clk, ref_ack_owned, pclk_refclk_ack(0), 100, &last)) {
        Ln l; l.t.s("warn: refclk request not acked (clock_ctl 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
    powerdown_change(BOTH_LANES, P2_STATE_RESET);
    // intel_cx0_setup_powerdown: power_state_ready 7:4 = p2, buf_ctl3
    // power_state_active 3:0 = p0, lane staggering delay 15:8 = 0 (satoru)
    rmw(p.buf2, 0xFu << 4, P2_STATE_READY << 4);
    rmw(p.buf3, 0xFu | (0xFFu << 8), 0);
    rmw(p.buf2, pipe_reset, 0);
    if (!wait_reg(p.buf2, cur_status, 0, 15000, &last)) {
        Ln l; l.t.s("pll: lanes did not leave reset (buf_ctl2 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        return false;
    }
    // 3. power state ready on both lanes (satoru)
    powerdown_change(BOTH_LANES, P2_STATE_READY);
    // 5. intel_c20_pll_program: write the context that is not in use,
    // vdr params, then toggle the context (satoru)
    int cntx_raw = mb_read(0, C20_VDR_SERDES);
    if (cntx_raw < 0) return false;
    bool cntx = cntx_raw & C20_CONTEXT_TOGGLE;
    {
        Ln l; l.t.s("pll: c20 serdes 0xd00 = 0x"); l.t.x((uint32_t)cntx_raw, 2); l.t.s(", context "); l.t.s(cntx ? "a in use, writing a" : "b selected, writing b");
        l.go();
    }
    // the linux tables are written to "A" when the toggle bit is set,
    // otherwise to "B" (intel_c20_pll_program step 3) (satoru)
    for (int i = 0; i < 3; i++) {
        uint16_t a = (uint16_t)((cntx ? 0xCF2E : 0xCF2A) - i);
        c20_sram_write(a, pll.tx[i]);
        Ln l; l.t.s("c20 sram tx"); l.t.d((uint64_t)i); l.t.s(" 0x"); l.t.x(a, 4); l.t.s(" = 0x"); l.t.x(pll.tx[i], 4); l.go();
    }
    for (int i = 0; i < 4; i++) {
        uint16_t a = (uint16_t)((cntx ? 0xCDAA : 0xCDA5) - i);
        c20_sram_write(a, pll.cmn[i]);
        Ln l; l.t.s("c20 sram cmn"); l.t.d((uint64_t)i); l.t.s(" 0x"); l.t.x(a, 4); l.t.s(" = 0x"); l.t.x(pll.cmn[i], 4); l.go();
    }
    for (int i = 0; i < 11; i++) {
        uint16_t a = (uint16_t)((cntx ? 0xCB5A : 0xCB4E) - i);
        c20_sram_write(a, pll.mpllb[i]);
        Ln l; l.t.s("c20 sram mpllb"); l.t.d((uint64_t)i); l.t.s(" 0x"); l.t.x(a, 4); l.t.s(" = 0x"); l.t.x(pll.mpllb[i], 4); l.go();
    }
    // 4. custom width 0 (8b/10b); serdes rate is_dp | dp_rate (satoru)
    cx0_rmw(owned, C20_VDR_WIDTH, 0x3, 0, true, true);
    cx0_rmw(owned, C20_VDR_SERDES, C20_IS_HDMI_FRL | C20_IS_DP | C20_DP_RATE_MASK,
            (uint8_t)(C20_IS_DP | (IgpuCalc::C20DpRate(rate) << 1)), true, true);
    // 7. toggle the context bit to load the programming (satoru)
    cx0_rmw(owned, C20_VDR_SERDES, C20_CONTEXT_TOGGLE, cntx ? 0 : C20_CONTEXT_TOGGLE, true, true);
    // 6. intel_cx0_program_phy_lane: disable the tx lanes above lane_count
    // (phy_cx0_tx_control(tx, 2) = 0x400 + (tx - 1) * 0x200 + 2, bit 6) (satoru)
    uint8_t disables = (uint8_t)((0xFu << lanes) & 0xF);
    if (lanes == 1) disables = (uint8_t)((disables & ~0x3u) | 0x1u);   // dp-alt with one lane (satoru)
    for (int i = 0; i < 4; i++) {
        int tx = i % 2 + 1;
        uint8_t m = i < 2 ? LANE0 : LANE1;
        if (!(owned & m)) continue;
        cx0_rmw(m, (uint16_t)(0x400 + (tx - 1) * 0x200 + 2), 0x40, (disables & (1u << i)) ? 0x40 : 0, true, true);
    }
    if (g_mb_err) { say("pll: message bus errors, aborting"); return false; }
    // 8. ddi_clk_valfreq = link symbol clock (satoru)
    wr(p.valfreq, IgpuCalc::LinkSymbolClock(rate));
    // 9. + 10. pclk pll request on lane 0 (maxpclk lane), wait the ack
    // (xelpdp_pclk_pll_enable_timeout_us 3200) (satoru)
    rmw(p.clk, pclk_pll_req(0) | pclk_pll_req(1), pclk_pll_req(0));
    if (!wait_reg(p.clk, pclk_pll_ack(0) | pclk_pll_ack(1), pclk_pll_ack(0), 3200, &last)) {
        Ln l; l.t.s("pll: not locked (clock_ctl 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        return false;
    }
    Ln l; l.t.s("pll: locked, clock_ctl 0x"); l.t.x(rd(p.clk), 8); l.t.s(" buf_ctl2 0x"); l.t.x(rd(p.buf2), 8); l.go();
    return true;
}

void pll_disable() {
    // intel_cx0pll_disable, c20 non-edp: p4pg (satoru)
    const Port& p = g_s.port;
    msgbus_timer();
    powerdown_change(BOTH_LANES, P4PG_STATE_DISABLE);
    rmw(p.clk, pclk_pll_req(0) | pclk_pll_req(1) | pclk_refclk_req(0) | pclk_refclk_req(1), 0);
    wr(p.valfreq, 0);
    uint32_t last = 0;
    if (!wait_reg(p.clk, pclk_pll_ack(0) | pclk_pll_ack(1) | pclk_refclk_ack(0) | pclk_refclk_ack(1), 0, 200, &last)) {
        Ln l; l.t.s("warn: pll not unlocked (clock_ctl 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
    rmw(p.clk, CLK_DDI_SEL_MASK, 0);
    rmw(p.clk, CLK_FWD_UNGATE, 0);
}

// ---- aux / dpcd glue (satoru) ----
int dpcd_rd(uint32_t a, uint8_t* b, int n) { return IntelDpAux::DpcdRead(g_s.port.tc, a, b, n); }
int dpcd_wr(uint32_t a, const uint8_t* b, int n) {
    int r = IntelDpAux::DpcdWrite(g_s.port.tc, a, b, n);
    Ln l; l.t.s("dpcd wr 0x"); l.t.x(a, 5); l.t.s(" ="); for (int i = 0; i < n && i < 8; i++) { l.t.s(" "); l.t.x(b[i], 2); }
    if (r != n) { l.t.s(" FAILED "); l.t.s(IntelDpAux::ErrName(r)); }
    l.go();
    return r;
}
int dpcd_wr1(uint32_t a, uint8_t v) { return dpcd_wr(a, &v, 1); }

// dp link training callbacks (satoru)
int lt_rd(void*, uint32_t a, uint8_t* b, int n) { return dpcd_rd(a, b, n); }
int lt_wr(void*, uint32_t a, const uint8_t* b, int n) { return dpcd_wr(a, b, n); }
void lt_pattern(void*, uint8_t pat) {
    // intel_ddi_set_link_train: intel_dp_training_pattern_symbol (satoru)
    uint32_t v;
    switch (pat & 0xF) {
        case DpLt::TP1: v = TP_PAT1; break;
        case DpLt::TP2: v = TP_PAT2; break;
        case DpLt::TP3: v = TP_PAT3; break;
        case DpLt::TP4: v = TP_PAT4; break;
        default: v = TP_NORMAL; break;
    }
    rmw(T_DP_TP_CTL, TP_TRAIN_MASK, v);
}
void lt_levels(void*, const uint8_t ts[4], int lanes) {
    for (int i = 0; i < 4; i++) g_s.train_set[i] = ts[i];
    set_signal_levels(ts, lanes);
}
void lt_idle(void*) {
    // intel_ddi_set_idle_link_train: pattern idle, wait idle_done 2 ms (satoru)
    rmw(T_DP_TP_CTL, TP_TRAIN_MASK, TP_IDLE);
    uint32_t last = 0;
    if (!wait_reg(T_DP_TP_STATUS, TP_STATUS_IDLE_DONE, TP_STATUS_IDLE_DONE, 2000, &last)) {
        Ln l; l.t.s("warn: dp idle patterns not done (dp_tp_status 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
}
void lt_delay(void*, uint32_t us) { udelay(us); }
void lt_log(void*, const char* s) { emit(s); }

// ---- pipe a snapshot (satoru) ----
void snapshot_pipe_a(uint32_t* v) { for (int i = 0; i < N_PIPE_A_REGS; i++) v[i] = rd(PIPE_A_REGS[i].off); }

void diff_pipe_a(const char* when) {
    uint32_t now[N_PIPE_A_REGS];
    snapshot_pipe_a(now);
    int diffs = 0;
    for (int i = 0; i < N_PIPE_A_REGS; i++) {
        if (now[i] == g_s.pipe_a_before[i]) continue;
        diffs++;
        Ln l; l.t.s("PIPE A CHANGED ("); l.t.s(when); l.t.s("): "); l.t.s(PIPE_A_REGS[i].name); l.t.s(" 0x");
        l.t.x(g_s.pipe_a_before[i], 8); l.t.s(" -> 0x"); l.t.x(now[i], 8); l.go();
    }
    Ln l; l.t.s("pipe a check ("); l.t.s(when); l.t.s("): "); l.t.d((uint64_t)N_PIPE_A_REGS); l.t.s(" registers, ");
    if (diffs) { l.t.d((uint64_t)diffs); l.t.s(" changed"); } else l.t.s("all unchanged");
    l.go();
}

// ---- framebuffer (satoru) ----
// 5x7 glyphs, bit 4 = left column (satoru)
struct Glyph { char c; uint8_t rows[7]; };
const Glyph FONT[] = {
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } }, { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '@', { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E } }, { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
};

const uint8_t* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const Glyph& g : FONT) if (g.c == c) return g.rows;
    return nullptr;
}

void fb_text(uint32_t* fb, uint32_t stride_px, int w, int h, int x0, int y0, int scale, const char* s, uint32_t color) {
    for (int i = 0; s[i]; i++) {
        const uint8_t* g = glyph(s[i]);
        int cx = x0 + i * 6 * scale;
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int y = y0 + r * scale + dy;
                    if (y < 0 || y >= h) continue;
                    uint32_t* row = fb + (uint64_t)y * stride_px;
                    for (int dx = 0; dx < scale; dx++) {
                        int x = cx + c * scale + dx;
                        if (x >= 0 && x < w) row[x] = color;
                    }
                }
            }
    }
}

void fb_pattern() {
    // xrgb8888 (byte order b, g, r, x): gradient, 8 colour bars, a white
    // border and a "KURONO" label with the mode (satoru)
    uint32_t* fb = (uint32_t*)(uintptr_t)g_s.fb_phys;
    uint32_t sp = g_s.pitch / 4;
    int w = g_s.fb_w, h = g_s.fb_h;
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (int y = 0; y < h; y++) {
        uint32_t* row = fb + (uint64_t)y * sp;
        for (int x = 0; x < w; x++) {
            uint32_t v;
            if (y < h / 3) v = bars[(x * 8) / w];
            else { uint32_t g = (uint32_t)((x * 255) / (w - 1)); uint32_t b = (uint32_t)((y * 255) / (h - 1)); v = (g << 8) | b | 0x200000; }
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) v = 0xFFFFFF;
            row[x] = v;
        }
    }
    fb_text(fb, sp, w, h, w / 2 - 6 * 24 * 6 / 2, h / 2 - 7 * 24 / 2, 24, "KURONO", 0xFFFFFF);
    char t[64]; Txt tx(t, sizeof(t));
    tx.d(g_s.mode.hdisplay); tx.c('X'); tx.d(g_s.mode.vdisplay); tx.c('@'); tx.d(g_s.mode.refresh_mhz / 1000);
    tx.s(" TC"); tx.d((uint64_t)g_s.port.tc); tx.s(" PIPE B");
    fb_text(fb, sp, w, h, 64, h - 7 * 8 - 64, 8, t, 0xFFFFFF);
    // the display reads memory uncached (pat uc), so push the pattern out
    // of the cpu caches (linux flushes scanout buffers the same way) (satoru)
    for (uint64_t o = 0; o < (uint64_t)g_s.pitch * (uint64_t)h; o += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"((uint8_t*)fb + o) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

bool fb_alloc(int w, int h) {
    if (g_s.fb_phys && g_s.fb_w == w && g_s.fb_h == h) return true;   // kept from an earlier light (satoru)
    if (g_s.fb_phys) { say("framebuffer exists with another size, refusing (a screen may use it)"); return false; }
    // linear xrgb8888: stride a multiple of 64 bytes (skl_plane_stride_mult
    // for linear is 64) (satoru)
    uint32_t pitch = ((uint32_t)w * 4 + 63) & ~63u;
    uint64_t bytes = (uint64_t)pitch * (uint64_t)h;
    uint64_t size2m = (bytes + 0x1FFFFF) & ~0x1FFFFFull;    // the wc remap works in 2 mb pages (satoru)
    uint64_t frames = size2m / 4096 + 512;                  // + 2 mb so we can align (satoru)
    uint64_t base = PMM::AllocContiguous(frames);
    if (!base) { say("framebuffer: no contiguous memory"); return false; }
    uint64_t aligned = (base + 0x1FFFFF) & ~0x1FFFFFull;
    uint64_t head = (aligned - base) / 4096, tail = frames - head - size2m / 4096;
    if (head) PMM::FreeContiguous(base, head);
    if (tail) PMM::FreeContiguous(aligned + size2m, tail);
    g_s.fb_phys = aligned;
    g_s.fb_bytes = size2m;
    g_s.pitch = pitch;
    g_s.fb_w = w;
    g_s.fb_h = h;
    Ln l; l.t.s("framebuffer: phys 0x"); l.t.x((uint32_t)(aligned >> 32), 8); l.t.x((uint32_t)aligned, 8);
    l.t.s(" "); l.t.d((uint64_t)w); l.t.c('x'); l.t.d((uint64_t)h); l.t.s(" pitch "); l.t.d(pitch); l.t.s(" bytes "); l.t.d(size2m); l.go();
    return true;
}

// ---- ggtt (satoru) ----
volatile uint64_t* gsm_pte(uint32_t idx) { return (volatile uint64_t*)(uintptr_t)(g_s.gsm_phys + (uint64_t)idx * 8); }

bool gsm_map(uint32_t first, uint32_t count) {
    // map the gsm pages holding ptes [first, first + count) uncached, the
    // way ggtt_probe_common ioremap()s the table (no wc on icl+) (satoru)
    uint32_t p0 = (first * 8) / 4096, p1 = ((first + count) * 8 + 4095) / 4096;
    for (uint32_t pg = p0; pg < p1 && pg < 2048; pg++) {
        if (g_mapped_gsm_pages[pg]) continue;
        uint64_t a = g_s.gsm_phys + (uint64_t)pg * 4096;
        if (!KernelVMM::MapPage(a, a, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) return false;
        g_mapped_gsm_pages[pg] = true;
    }
    return true;
}

bool ggtt_setup() {
    // wa_22018444074 (i915_direct_stolen_access): on meteor lake class parts
    // the bar path can hang; when the firmware has opened stolen memory
    // (0x138914 == 1) use gsmbase directly, else bar0 + 8 mb (satoru)
    uint32_t acc = rd(R_MTL_PCODE_STOLEN_ACCESS);
    uint64_t gsmbase = ((uint64_t)rd(R_GEN6_GSMBASE + 4) << 32 | rd(R_GEN6_GSMBASE)) & ~0xFFFFFull;
    uint64_t bar0 = (uint64_t)(uintptr_t)g_mmio;
    g_s.gsm_direct = acc == 1 && gsmbase;
    g_s.gsm_phys = g_s.gsm_direct ? gsmbase : bar0 + GGTT_BAR_OFFSET;
    {
        Ln l; l.t.s("ggtt: stolen_access 0x"); l.t.x(acc, 8); l.t.s(" gsmbase 0x"); l.t.x((uint32_t)(gsmbase >> 32), 8);
        l.t.x((uint32_t)gsmbase, 8); l.t.s(" -> ptes via "); l.t.s(g_s.gsm_direct ? "gsm (direct)" : "bar0 + 8 mb");
        l.t.s(" at 0x"); l.t.x((uint32_t)(g_s.gsm_phys >> 32), 8); l.t.x((uint32_t)g_s.gsm_phys, 8); l.go();
    }
    // the firmware's surface, for the log and the overlap check (satoru)
    uint32_t surf_a = rd(0x7019c) & 0xFFFFF000u;
    uint32_t surf_idx = surf_a >> 12;
    if (!gsm_map(surf_idx, 1)) { say("ggtt: mapping the gsm failed"); return false; }
    uint64_t gop_pte = *gsm_pte(surf_idx);
    {
        Ln l; l.t.s("ggtt: pipe a plane surf 0x"); l.t.x(surf_a, 8); l.t.s(" pte 0x"); l.t.x((uint32_t)(gop_pte >> 32), 8);
        l.t.x((uint32_t)gop_pte, 8); l.t.s(" (valid="); l.t.d(gop_pte & 1); l.t.s(" pat="); l.t.d((gop_pte >> 52) & 3); l.t.s(")"); l.go();
    }
    if (!(gop_pte & 1)) { say("ggtt: the firmware surface pte is not valid, gsm path looks wrong, refusing"); return false; }
    g_s.npte = (uint32_t)(g_s.fb_bytes / 4096);
    // window: 16 mb aligned (>= the 256 kb linear surface alignment of
    // intel_linear_alignment), below the firmware surface, where every pte
    // holds the same value (unused or scratch) (satoru)
    uint32_t win = (uint32_t)((g_s.fb_bytes + 0xFFFFFF) & ~0xFFFFFFull);
    for (int tries = 0; tries < 32; tries++) {
        uint64_t cand = ((uint64_t)surf_a & ~0xFFFFFFull) - (uint64_t)win * (uint64_t)(tries + 1);
        if (cand < 0x10000000ull || cand >= surf_a) break;
        uint32_t idx = (uint32_t)(cand >> 12);
        if (!gsm_map(idx, g_s.npte)) { say("ggtt: mapping the gsm failed"); return false; }
        uint64_t v0 = *gsm_pte(idx);
        // a window is free when none of its ptes is a live mapping. the firmware leaves the
        // unused part of the ggtt as stale ram, so "all ptes equal" never held and the aoc
        // never lit (found from the laptop log). a live mtl pte is
        // present | lm | addr 45:12 | pat 52:53 with the address inside ram (satoru)
        uint32_t live = 0;
        for (uint32_t i = 0; i < g_s.npte; i++) {
            uint64_t pte = *gsm_pte(idx + i);
            const uint64_t kAddr = 0x00003FFFFFFFF000ull;
            const uint64_t kOk = 1ull | 2ull | kAddr | (1ull << 52) | (1ull << 53);
            if (!(pte & 1) || (pte & ~kOk)) continue;
            uint64_t pa = pte & kAddr;
            if (pa != 0 && pa < PMM::GetPhysTop()) live++;
        }
        Ln l; l.t.s("ggtt: window 0x"); l.t.x((uint32_t)cand, 8); l.t.s(" ptes "); l.t.d(g_s.npte); l.t.s(" first 0x");
        l.t.x((uint32_t)(v0 >> 32), 8); l.t.x((uint32_t)v0, 8); l.t.s(" live "); l.t.d(live); l.t.s(live ? " in use" : " free"); l.go();
        if (live || (v0 & 1 && (v0 & 0x3FFFFFFFF000ull) == (gop_pte & 0x3FFFFFFFF000ull))) continue;
        g_s.ggtt_off = (uint32_t)cand;
        g_s.saved_pte = v0;
        // mtl_ggtt_pte_encode(addr, pat 2 = i915_cache_none) (satoru)
        for (uint32_t i = 0; i < g_s.npte; i++)
            *gsm_pte(idx + i) = IgpuCalc::MtlGgttPte(g_s.fb_phys + (uint64_t)i * 4096, IgpuCalc::MTL_PAT_UC);
        __asm__ __volatile__("mfence" ::: "memory");
        uint64_t chk0 = *gsm_pte(idx), chkn = *gsm_pte(idx + g_s.npte - 1);   // posting reads (satoru)
        uint64_t want0 = IgpuCalc::MtlGgttPte(g_s.fb_phys, IgpuCalc::MTL_PAT_UC);
        uint64_t wantn = IgpuCalc::MtlGgttPte(g_s.fb_phys + (uint64_t)(g_s.npte - 1) * 4096, IgpuCalc::MTL_PAT_UC);
        Ln m; m.t.s("ggtt: wrote "); m.t.d(g_s.npte); m.t.s(" ptes, first 0x"); m.t.x((uint32_t)(chk0 >> 32), 8); m.t.x((uint32_t)chk0, 8);
        m.t.s(" last 0x"); m.t.x((uint32_t)(chkn >> 32), 8); m.t.x((uint32_t)chkn, 8); m.t.s(chk0 == want0 && chkn == wantn ? " (read back ok)" : " (READ BACK MISMATCH)");
        m.go();
        g_s.steps |= S_GGTT;
        if (chk0 != want0 || chkn != wantn) return false;
        return true;
    }
    say("ggtt: no free window found below the firmware surface");
    return false;
}

void ggtt_restore() {
    uint32_t idx = g_s.ggtt_off >> 12;
    for (uint32_t i = 0; i < g_s.npte; i++) *gsm_pte(idx + i) = g_s.saved_pte;
    __asm__ __volatile__("mfence" ::: "memory");
    (void)*gsm_pte(idx);
    Ln l; l.t.s("ggtt: restored "); l.t.d(g_s.npte); l.t.s(" ptes to 0x"); l.t.x((uint32_t)(g_s.saved_pte >> 32), 8);
    l.t.x((uint32_t)g_s.saved_pte, 8); l.go();
}

// ---- watermarks + ddb for plane 1 of pipe b (satoru) ----
struct PlaneWms {
    IgpuCalc::WmLevel wm[6], trans, sagv, sagv_trans;
    uint16_t ddb_start, ddb_end;
};

void compute_wms(PlaneWms* w) {
    // mtl_read_wm_latency + adjust_wm_latency (satoru)
    uint16_t lat[6];
    for (int i = 0; i < 3; i++) {
        uint32_t v = rd(R_MTL_LATENCY[i]);
        lat[i * 2] = (uint16_t)(v & 0x1FFF);
        lat[i * 2 + 1] = (uint16_t)((v >> 16) & 0x1FFF);
    }
    uint16_t raw[6]; for (int i = 0; i < 6; i++) raw[i] = lat[i];
    IgpuCalc::AdjustLatency(lat);
    uint32_t sagv_block = rd(R_MTL_LATENCY_SAGV) & 0x1FFF;   // intel_sagv_block_time ver >= 14 (satoru)
    bool ipc = rd(R_DISP_ARB_CTL2) & (1u << 3);
    IgpuCalc::WmParams wp;
    IgpuCalc::WmParamsLinear(g_s.mode.clock_khz, g_s.mode.htotal, g_s.mode.hdisplay, 4, &wp);
    IgpuCalc::WmLevel prev = { false, 0, 0, 0 };
    for (int lv = 0; lv < 6; lv++) {
        IgpuCalc::PlaneWm(wp, lat[lv], lv == 0 ? IgpuCalc::WmLevel{ false, 0, 0, 0 } : prev, &w->wm[lv]);
        prev = w->wm[lv];
    }
    IgpuCalc::TransWm(w->wm[0], &w->trans);
    if (!ipc) w->trans = IgpuCalc::WmLevel{ false, 0, 0, IgpuCalc::WM_REJECT };
    // tgl_compute_sagv_wm: latency = sagv block time + wm0 latency (satoru)
    IgpuCalc::PlaneWm(wp, sagv_block ? sagv_block + lat[0] : 0, w->wm[0], &w->sagv);
    IgpuCalc::TransWm(w->sagv, &w->sagv_trans);
    if (!ipc) w->sagv_trans = IgpuCalc::WmLevel{ false, 0, 0, IgpuCalc::WM_REJECT };
    // ddb: pipe b alone in dbuf bank 1 (s3|s4, 2048 blocks, mbus offset
    // 2048), so its relative range is [0, 2048). keep a 32 block cursor
    // reserve at the end like skl_cursor_allocation (cursor stays off) (satoru)
    w->ddb_start = 0;
    w->ddb_end = 2048 - 32;
    uint16_t size = (uint16_t)(w->ddb_end - w->ddb_start);
    // skl_crtc_allocate_plane_ddb: levels whose min_ddb does not fit are off,
    // and their regs copy the level below (skl_need_wm_copy_wa) (satoru)
    for (int lv = 0; lv < 6; lv++) {
        if (w->wm[lv].min_ddb > size) {
            w->wm[lv] = IgpuCalc::WmLevel{ false, 0, 0, 0 };
            if (lv > 0) { w->wm[lv].blocks = w->wm[lv - 1].blocks; w->wm[lv].lines = w->wm[lv - 1].lines; }
        }
    }
    if (w->trans.min_ddb > size) w->trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    if (w->sagv.min_ddb > size) w->sagv = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    if (w->sagv_trans.min_ddb > size) w->sagv_trans = IgpuCalc::WmLevel{ false, 0, 0, 0 };
    Ln l; l.t.s("wm: latency raw");
    for (int i = 0; i < 6; i++) { l.t.c(' '); l.t.d(raw[i]); }
    l.t.s(" adj"); for (int i = 0; i < 6; i++) { l.t.c(' '); l.t.d(lat[i]); }
    l.t.s(" us, sagv block "); l.t.d(sagv_block); l.t.s(" us, ipc "); l.t.d(ipc); l.t.s(" | blocks/line "); l.t.d(wp.blocks_per_line_fx >> 16);
    l.t.s(" linetime "); l.t.d(wp.linetime_us); l.t.s(" us"); l.go();
    Ln m; m.t.s("wm: levels");
    for (int i = 0; i < 6; i++) { m.t.s(" ["); m.t.d(i); m.t.c(' '); m.t.d(w->wm[i].enable); m.t.c('/'); m.t.d(w->wm[i].blocks); m.t.c('/'); m.t.d(w->wm[i].lines); m.t.c(']'); }
    m.t.s(" trans "); m.t.d(w->trans.enable); m.t.c('/'); m.t.d(w->trans.blocks);
    m.t.s(" sagv "); m.t.d(w->sagv.enable); m.t.c('/'); m.t.d(w->sagv.blocks); m.t.c('/'); m.t.d(w->sagv.lines);
    m.t.s(" | ddb "); m.t.d(w->ddb_start); m.t.c('-'); m.t.d(w->ddb_end); m.go();
}

// ---- pmdemand + dbuf (satoru) ----
bool pmdemand_raise(uint32_t pipes, uint32_t dbufs, uint32_t ddiclk_mhz) {
    // intel_pmdemand_check_prev_transaction + intel_pmdemand_program_params
    // (pre plane: max of old, new and current) (satoru)
    if (!wait_reg(R_PMDEMAND1, PMDEMAND_REQ_ENABLE, 0, 10000) || !wait_reg(R_DCPR_STATUS_1, 1u << 26, 0, 10000)) {
        say("pmdemand: a previous request is still in flight, refusing");
        return false;
    }
    IgpuCalc::PmDemand p = { rd(R_PMDEMAND0), rd(R_PMDEMAND1) };
    IgpuCalc::PmDemand o = p;
    bool changed = IgpuCalc::PmDemandRaise(&p, pipes, dbufs, ddiclk_mhz);
    {
        Ln l; l.t.s("pmdemand: req0 0x"); l.t.x(o.reg0, 8); l.t.s(" req1 0x"); l.t.x(o.reg1, 8); l.t.s(" (qclk_gv_bw ");
        l.t.d(o.reg0 >> 16); l.t.s(" pipes "); l.t.d((o.reg0 >> 6) & 3); l.t.s(" dbufs "); l.t.d((o.reg0 >> 4) & 3);
        l.t.s(" phys "); l.t.d(o.reg0 & 7); l.t.s(" cdclk "); l.t.d((o.reg1 >> 20) & 0x7FF); l.t.s(" ddiclk "); l.t.d((o.reg1 >> 8) & 0x7FF);
        l.t.s(") -> 0x"); l.t.x(p.reg0, 8); l.t.s(" 0x"); l.t.x(p.reg1, 8); l.t.s(changed ? "" : " (no change)"); l.go();
    }
    if (!changed) return true;
    if (p.reg0 != o.reg0) wr(R_PMDEMAND0, p.reg0);
    if (p.reg1 != o.reg1) wr(R_PMDEMAND1, p.reg1 & ~PMDEMAND_REQ_ENABLE);
    rmw(R_PMDEMAND1, 0, PMDEMAND_REQ_ENABLE);
    uint32_t last = 0;
    if (!wait_reg(R_PMDEMAND1, PMDEMAND_REQ_ENABLE, 0, 10000, &last)) {
        Ln l; l.t.s("pmdemand: punit did not answer within 10 ms (req1 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        return false;
    }
    g_s.steps |= S_PMDEMAND;
    return true;
}

bool dbuf_bank1_on() {
    // gen9_dbuf_slice_set for s3/s4 (indices 2, 3): power request, posting
    // read, 10 us, check state. the tracker fields (gen12_dbuf_slices_config,
    // intel_dbuf_mdclk_cdclk_ratio_update) copy slice s1's values (satoru)
    uint32_t s0 = rd(R_DBUF_CTL[0]);
    for (int s = 2; s < 4; s++) {
        uint32_t v = rd(R_DBUF_CTL[s]);
        if (v & DBUF_POWER_STATE) continue;
        rmw(R_DBUF_CTL[s], DBUF_TRACKERS_MASK, s0 & DBUF_TRACKERS_MASK);
        rmw(R_DBUF_CTL[s], 0, DBUF_POWER_REQUEST);
        (void)rd(R_DBUF_CTL[s]);
        udelay(10);
        uint32_t st = 0;
        if (!wait_reg(R_DBUF_CTL[s], DBUF_POWER_STATE, DBUF_POWER_STATE, 100, &st)) {
            Ln l; l.t.s("dbuf: slice "); l.t.d((uint64_t)s); l.t.s(" did not power up (0x"); l.t.x(st, 8); l.t.s(")"); l.go();
            return false;
        }
        g_s.steps |= S_DBUF;
    }
    return true;
}

// ---- the port off path (intel_ddi_post_disable_dp, ver 14) (satoru) ----
void port_down() {
    const Port& p = g_s.port;
    if (g_s.steps & S_SINK_D0) { dpcd_wr1(0x600, 0x02); g_s.steps &= ~S_SINK_D0; }   // dp_set_power_d3 (satoru)
    if (g_s.steps & (S_DDI_BUF | S_DP_TP)) {
        // intel_ddi_buf_disable: buf off, wait idle 10 ms, d2d off, dp_tp off (satoru)
        rmw(p.ddi_buf, 1u << 31, 0);
        uint32_t last = 0;
        if (!wait_reg(p.buf1, BUF1_PHY_IDLE, BUF1_PHY_IDLE, 10000, &last)) {
            Ln l; l.t.s("warn: ddi buf did not go idle (buf_ctl1 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        }
        rmw(p.buf1, BUF1_D2D_ENABLE, 0);
        if (!wait_reg(p.buf1, BUF1_D2D_STATE, 0, 200, &last)) say("warn: d2d link did not go down");
        rmw(T_DP_TP_CTL, TP_ENABLE, 0);
        g_s.steps &= ~(S_DDI_BUF | S_DP_TP);
    }
    if (g_s.steps & S_FUNC_CFG) {
        wr(T_DP2_CTL, 0);                                    // intel_ddi_config_transcoder_dp2(false) (satoru)
        g_s.steps &= ~S_FUNC_CFG;
    }
    if (g_s.steps & S_TRANS_CLK) { wr(R_TRANS_CLK_SEL_B, 0); g_s.steps &= ~S_TRANS_CLK; }   // tgl_trans_clk_sel_disabled (satoru)
    if (g_s.steps & S_IOSEL) { rmw(p.buf1, BUF1_IO_TBT, 0); g_s.steps &= ~S_IOSEL; }        // de-select thunderbolt (satoru)
    // hsw_crtc_disable: intel_dpll_disable after the encoder post disable (satoru)
    if (g_s.steps & S_PLL) { pll_disable(); g_s.steps &= ~S_PLL; }
}

void plane_down() {
    if (!(g_s.steps & S_PLANE)) return;
    // icl_plane_disable_arm: cus_ctl 0, wms/ddb (now zero), plane_ctl 0,
    // plane_surf 0 (arms), then wait about a frame (satoru)
    wr(PL_CUS_CTL, 0);
    for (int lv = 0; lv < 6; lv++) wr(PL_WM0 + 4u * (uint32_t)lv, 0);
    wr(PL_WM_TRANS, 0); wr(PL_WM_SAGV, 0); wr(PL_WM_SAGV_TRANS, 0); wr(PL_BUF_CFG, 0);
    wr(PL_CTL, 0);
    wr(PL_SURF, 0);
    udelay(20000);
    g_s.steps &= ~S_PLANE;
}

void pipe_down() {
    if (g_s.steps & S_TRANS_EN) {
        // intel_disable_transcoder: clear enable, wait the state off 100 ms,
        // clear fecstall_dis_dptstream_dpttg (satoru)
        rmw(P_TRANSCONF, TRANSCONF_ENABLE, 0);
        uint32_t last = 0;
        if (!wait_reg(P_TRANSCONF, TRANSCONF_STATE, 0, 100000, &last)) say("warn: pipe b did not stop within 100 ms");
        rmw(T_CHICKEN, CT_FECSTALL_DIS, 0);
        g_s.steps &= ~S_TRANS_EN;
    }
    if (g_s.steps & S_FUNC_EN) {
        // intel_ddi_disable_transcoder_func (satoru)
        wr(T_DDI_FUNC2, 0);
        rmw(T_DDI_FUNC, DDI_FUNC_ENABLE | DDI_FUNC_PORT_MASK | DDI_FUNC_MODE_MASK, 0);
        g_s.steps &= ~S_FUNC_EN;
    }
    g_s.steps &= ~S_PIPE_CFG;
}

void rollback(const char* why) {
    {
        Ln l; l.t.s("rollback: "); l.t.s(why); l.t.s(" (steps 0x"); l.t.x(g_s.steps, 4); l.t.s(")"); l.go();
    }
    // a fresh small aux budget so the d3 write still goes out when the
    // failure was the aux budget itself (satoru)
    if (g_s.steps & S_SINK_D0) IntelDpAux::BeginSession(g_s.port.tc, 300000, 100);
    plane_down();
    pipe_down();
    if (g_s.steps & S_FUNC_CFG) rmw(T_DDI_FUNC, DDI_FUNC_ENABLE | DDI_FUNC_PORT_MASK | DDI_FUNC_MODE_MASK, 0);
    port_down();
    if (g_s.steps & S_GGTT) { ggtt_restore(); g_s.steps &= ~S_GGTT; }
    if (g_s.steps & S_AUXPWR) {
        if (!g_s.aux_was_on) IntelDpAux::AuxPower(g_s.port.tc, false, nullptr);
        g_s.steps &= ~S_AUXPWR;
    }
    // pmdemand, dbuf slices and power wells stay raised (never lowered) (satoru)
    g_s.steps &= ~(S_PMDEMAND | S_DBUF);
    g_s.lit = false;
    diff_pipe_a("after rollback");
}

// ---- lttpr + dprx caps (intel_dp_init_lttpr_and_dprx_caps) (satoru) ----
bool read_dprx_caps(uint8_t* d) {
    // drm_dp_read_dpcd_caps: 0x000.., and 0x2200.. when the extended field
    // bit (0x00e bit 7) is set and its revision is not older (satoru)
    if (dpcd_rd(0x000, d, 16) != 16) return false;
    if (d[0x0e] & 0x80) {
        uint8_t e[16];
        if (dpcd_rd(0x2200, e, 16) == 16 && e[0] >= d[0]) for (int i = 0; i < 16; i++) d[i] = e[i];
    }
    return true;
}

void init_lttpr() {
    g_s.lttpr_count = 0;
    uint8_t probe = 0;
    if (dpcd_rd(0xf0000, &probe, 1) != 1) say("lttpr: dpcd 0xf0000 probe failed, no lttpr handling");
    uint8_t d[16];
    if (!read_dprx_caps(d)) { say("lttpr: dprx caps read failed"); return; }
    // drm_dp_read_lttpr_common_caps (byte-wise below dpcd 1.4) (satoru)
    uint8_t c[8] = {};
    bool ok = true;
    if (d[0] < 0x14) { for (int i = 0; i < 8 && ok; i++) ok = dpcd_rd(0xf0000 + (uint32_t)i, &c[i], 1) == 1; }
    else ok = dpcd_rd(0xf0000, c, 8) == 8;
    for (int i = 0; i < 8; i++) g_s.lttpr_caps[i] = ok ? c[i] : 0;
    {
        Ln l; l.t.s("lttpr: common caps"); for (int i = 0; i < 8; i++) { l.t.c(' '); l.t.x(c[i], 2); }
        l.t.s(ok ? "" : " (read failed)"); l.go();
    }
    if (!ok || c[0] < 0x14) { say("lttpr: none (caps rev < 1.4), transparent"); return; }
    int count = IgpuCalc::LttprCount(c[2]);
    if (count == 0) { say("lttpr: count 0, mode left alone (linux: avoids breaking some docks)"); return; }
    // drm_dp_lttpr_init: transparent first, then non-transparent; on
    // failure back to transparent (satoru)
    if (dpcd_wr1(0xf0003, 0x55) != 1) { say("lttpr: set transparent failed"); return; }
    if (count < 0) { say("lttpr: bad repeater count, staying transparent"); return; }
    if (dpcd_wr1(0xf0003, 0xAA) != 1) { dpcd_wr1(0xf0003, 0x55); say("lttpr: non-transparent failed, back to transparent"); return; }
    g_s.lttpr_count = count;
    for (int i = 0; i < count; i++) {
        uint8_t pc[3] = {};
        // drm_dp_read_lttpr_phy_caps at dp_training_aux_rd_interval_phy_repeater(phy) (satoru)
        if (d[0] < 0x14) { for (int k = 0; k < 3; k++) dpcd_rd(0xf0020 + 0x50u * (uint32_t)i + (uint32_t)k, &pc[k], 1); }
        else dpcd_rd(0xf0020 + 0x50u * (uint32_t)i, pc, 3);
        for (int k = 0; k < 3; k++) g_s.lttpr_phy[i][k] = pc[k];
        Ln l; l.t.s("lttpr "); l.t.d((uint64_t)(i + 1)); l.t.s(" phy caps "); l.t.x(pc[0], 2); l.t.c(' '); l.t.x(pc[1], 2); l.t.c(' '); l.t.x(pc[2], 2); l.go();
    }
    Ln l; l.t.s("lttpr: "); l.t.d((uint64_t)count); l.t.s(" repeater(s) in non-transparent mode, max rate code 0x"); l.t.x(c[1], 2);
    l.t.s(" max lanes "); l.t.d(c[4] & 0x1F); l.go();
}

// ---- the enable path (satoru) ----
bool link_up(const LinkCfg& lc) {
    const Port& p = g_s.port;
    g_s.link = lc;
    const IgpuCalc::C20Pll* pll = IgpuCalc::C20DpTable(lc.rate_khz);
    if (!pll) { say("no c20 table for that rate"); return false; }
    {
        Ln l; l.t.s("---- link attempt: "); l.t.d(lc.rate_khz / 1000); l.t.s(" mhz x"); l.t.d((uint64_t)lc.lanes); l.t.s(" ----"); l.go();
    }
    // hsw_crtc_enable -> intel_dpll_enable -> intel_mtl_pll_enable (satoru)
    g_s.steps |= S_PLL;
    if (!pll_enable(*pll, lc.rate_khz, lc.lanes)) return false;
    if (over_budget()) { say("run budget exceeded"); return false; }
    // mtl_ddi_pre_enable_dp (satoru)
    // 3. mtl_port_buf_ctl_io_selection: tbt io off for dp-alt (satoru)
    rmw(p.buf1, BUF1_IO_TBT, 0);
    g_s.steps |= S_IOSEL;
    // 6.a intel_ddi_enable_transcoder_clock: tgl_trans_clk_sel_port(phy),
    // phy f + (tc - 1) for tc ports on display >= 13 -> (phy + 1) << 28 (satoru)
    wr(R_TRANS_CLK_SEL_B, (uint32_t)(5 + p.tc) << 28);
    g_s.steps |= S_TRANS_CLK;
    // 6.b/c intel_ddi_config_transcoder_func: trans_dp2_ctl 0 (8b/10b),
    // func ctl without the enable bit (satoru)
    wr(T_DP2_CTL, 0);
    uint32_t func = ((uint32_t)(p.port + 1) << 27) | DDI_MODE_DP_SST | DDI_BPC_8 |
                    (g_s.mode.pvsync ? DDI_PVSYNC : 0) | (g_s.mode.phsync ? DDI_PHSYNC : 0) |
                    ((uint32_t)(lc.lanes == 3 ? 4 : lc.lanes - 1) << 1);
    wr(T_DDI_FUNC, func);
    g_s.steps |= S_FUNC_CFG;
    // 6.e intel_ddi_mso_configure: no splitter (satoru)
    rmw(R_DSS_CTL1_B, (1u << 31) | (3u << 25) | (0xFu << 16), 0);
    // intel_dp_set_power(d0): 3 tries, 1 ms apart (satoru)
    int r = -1;
    for (int i = 0; i < 3 && r != 1; i++) { r = dpcd_wr1(0x600, 0x01); if (r != 1) udelay(1000); }
    g_s.steps |= S_SINK_D0;
    if (r != 1) { say("sink did not accept set_power d0"); return false; }
    // drm_dp_lttpr_wake_timeout_setup (satoru)
    if (g_s.lttpr_count == 0) {
        uint8_t req = 0;
        if (dpcd_rd(0x2211, &req, 1) == 1 && req >= 1 && req <= 5) dpcd_wr1(0x119, 0x01);
    } else {
        uint8_t req = 0;
        if (dpcd_rd(0xf0005, &req, 1) == 1 && (req & 0x7F)) dpcd_wr1(0xf0005, 0x80);
    }
    // intel_dp_start_link_train -> intel_dp_prepare_link_train ->
    // mtl_ddi_prepare_link_retrain (satoru)
    // 6.d dp_tp_ctl: enable, tps1, sst, enhanced framing (satoru)
    wr(T_DP_TP_CTL, TP_ENABLE | TP_PAT1 | (g_s.enhanced ? TP_ENHANCED : 0));
    (void)rd(T_DP_TP_CTL);
    g_s.steps |= S_DP_TP;
    // 6.f mtl_ddi_enable_d2d (satoru)
    rmw(p.buf1, 0, BUF1_D2D_ENABLE);
    uint32_t last = 0;
    if (!wait_reg(p.buf1, BUF1_D2D_STATE, BUF1_D2D_STATE, 100, &last)) {
        Ln l; l.t.s("warn: d2d link did not come up (buf_ctl1 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
    // 6.g set_signal_levels with the zeroed train_set (satoru)
    uint8_t ts0[4] = { 0, 0, 0, 0 };
    lt_levels(nullptr, ts0, lc.lanes);
    // 6.h mtl_port_buf_ctl_program: width, 10 bit data (satoru)
    rmw(p.buf1, BUF1_WIDTH_MASK | BUF1_DATA_WIDTH_MASK, (uint32_t)(lc.lanes == 3 ? 4 : lc.lanes - 1) << 1);
    // 6.i intel_ddi_buf_enable: ddi_port_width | trans select 0 | data 10 bit
    // (intel_ddi_init_dp_buf_reg) | enable, then wait active (phy idle clear,
    // 10 ms) (satoru)
    wr(p.ddi_buf, (1u << 31) | ((uint32_t)(lc.lanes == 3 ? 4 : lc.lanes - 1) << 1));
    (void)rd(p.ddi_buf);
    g_s.steps |= S_DDI_BUF;
    if (!wait_reg(p.buf1, BUF1_PHY_IDLE, 0, 10000, &last)) {
        Ln l; l.t.s("warn: ddi buf did not become active (buf_ctl1 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
    }
    // dpcd half + training (intel_dp_link_train_all_phys) (satoru)
    DpLt::Params lp = {};
    lp.rate_khz = lc.rate_khz;
    lp.lanes = lc.lanes;
    lp.enhanced_framing = g_s.enhanced;
    for (int i = 0; i < 16; i++) lp.dpcd[i] = g_s.dpcd[i];
    lp.lttpr_count = g_s.lttpr_count;
    for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) lp.lttpr_phy_caps[i][k] = g_s.lttpr_phy[i][k];
    lp.source_vswing_max = 3;          // intel_ddi_dp_voltage_max: 10 c20 entries -> level 3 (satoru)
    lp.source_preemph_max = 3 << 3;    // intel_ddi_dp_preemph_max: level 3 (satoru)
    DpLt::Ops ops = { nullptr, lt_rd, lt_wr, lt_pattern, lt_levels, lt_idle, lt_delay, lt_log };
    if (!DpLt::PrepareSink(ops, lp)) { say("link: dpcd link config write failed"); return false; }
    DpLt::Result res;
    bool ok = DpLt::TrainAllPhys(ops, lp, &res);
    {
        Txt t(g_s.last_result, sizeof(g_s.last_result));
        t.d(lc.rate_khz / 1000); t.s("x"); t.d((uint64_t)lc.lanes); t.s(ok ? " trained" : " failed: "); if (!ok) t.s(res.reason);
    }
    if (!ok) return false;
    // 6.n intel_dp_stop_link_train: dp_tp_ctl normal (satoru)
    rmw(T_DP_TP_CTL, TP_TRAIN_MASK, TP_NORMAL);
    // intel_ddi_set_dp_msa: sync clock | 8 bpc (drm_dp.h dp_msa_misc_*) (satoru)
    wr(T_MSA_MISC, 0x01 | (1u << 5));
    return true;
}

bool pipe_up(const PlaneWms& w) {
    const Mode& m = g_s.mode;
    IgpuCalc::TransRegs tr;
    IgpuCalc::TranscoderRegs(m, &tr);
    IgpuCalc::MN mn;
    IgpuCalc::ComputeMN(24, g_s.link.lanes, m.clock_khz, g_s.link.rate_khz, &mn);
    {
        Ln l; l.t.s("m/n: tu "); l.t.d(mn.tu); l.t.s(" data "); l.t.d(mn.data_m); l.t.c('/'); l.t.d(mn.data_n);
        l.t.s(" link "); l.t.d(mn.link_m); l.t.c('/'); l.t.d(mn.link_n); l.go();
    }
    g_s.steps |= S_PIPE_CFG;
    // intel_set_pipe_src_size, bdw_set_pipe_misc (8 bpc, pixel rounding trunc
    // on ver >= 12, no dither) (satoru)
    wr(T_PIPESRC, tr.pipesrc);
    wr(P_MISC, (0u << 5) | (1u << 8));
    // hsw_configure_cpu_transcoder: m1/n1 (link_n last, it arms), timings,
    // trans_mult, frame start delay, transconf (no enable yet) (satoru)
    wr(T_DATA_M1, IgpuCalc::DataMReg(mn));
    wr(T_DATA_N1, mn.data_n);
    wr(T_LINK_M1, mn.link_m);
    wr(T_LINK_N1, mn.link_n);
    wr(T_SCL, 0);
    wr(T_VSYNCSHIFT, 0);
    wr(T_HTOTAL, tr.htotal);
    wr(T_HBLANK, tr.hblank);
    wr(T_HSYNC, tr.hsync);
    wr(T_VTOTAL, tr.vtotal);
    wr(T_VBLANK, tr.vblank);
    wr(T_VSYNC, tr.vsync);
    wr(T_MULT, 0);
    rmw(T_CHICKEN, CT_FRAME_START_DELAY_MASK, 0);
    wr(P_TRANSCONF, 0);   // progressive (transconf_interlace_pf_pd_ilk = 0) (satoru)
    // intel_color_modeset with no luts: bottom color 0, gamma_mode 8bit
    // without pre/post csc gamma, csc off (icl_color_commit_arm) (satoru)
    wr(P_BOTTOM, 0);
    wr(R_GAMMA_MODE_B, 0);
    wr(R_CSC_MODE_B, 0);
    // hsw_set_linetime_wm, icl_set_pipe_chicken (satoru)
    wr(R_WM_LINETIME_B, IgpuCalc::LinetimeWm(m.htotal, m.clock_khz));
    rmw(P_CHICKEN, 0, PIPE_CHICKEN_SET);
    // pipe_mbus_dbox_ctl: mbus not joined, pipe b alone in its bank (satoru)
    wr(P_DBOX, IgpuCalc::MbusDboxCtl(false, true));
    // intel_ddi_enable: func ctl2 0, func ctl with enable (satoru)
    wr(T_DDI_FUNC2, 0);
    rmw(T_DDI_FUNC, 0, DDI_FUNC_ENABLE);
    g_s.steps |= S_FUNC_EN;
    // intel_enable_transcoder: chicken_trans clear dsc eol wa, set the ver 14
    // fec bs jitter wa, then transconf enable (satoru)
    rmw(T_CHICKEN, CT_DSC_INSERT_SF_EOL_WA, CT_DP_FEC_BS_JITTER_WA);
    rmw(P_TRANSCONF, 0, TRANSCONF_ENABLE);
    (void)rd(P_TRANSCONF);
    g_s.steps |= S_TRANS_EN;
    uint32_t last = 0;
    if (!wait_reg(P_TRANSCONF, TRANSCONF_STATE, TRANSCONF_STATE, 100000, &last)) {
        Ln l; l.t.s("pipe b did not start (transconf 0x"); l.t.x(last, 8); l.t.s(")"); l.go();
        return false;
    }
    // planes after the pipe runs (skl_commit_modeset_enables order):
    // icl_plane_update_noarm (satoru)
    g_s.steps |= S_PLANE;
    wr(PL_STRIDE, g_s.pitch / 64);
    wr(PL_POS, 0);
    wr(PL_SIZE, ((uint32_t)(g_s.fb_h - 1) << 16) | (uint32_t)(g_s.fb_w - 1));
    wr(PL_KEYVAL, 0);
    wr(PL_KEYMSK, 0);
    wr(PL_KEYMAX, 0xFFu << 24);        // plane_keymax_alpha(0xff) (satoru)
    wr(PL_OFFSET, 0);
    wr(PL_AUX_DIST, 0);                // has_aux_dist on meteor lake (satoru)
    wr(PL_CUS_CTL, 0);                 // hdr plane, no chroma upsampler (satoru)
    wr(PL_COLOR_CTL, PLANE_COLOR_PLANE_GAMMA_DISABLE);   // glk_plane_color_ctl, alpha disable (satoru)
    // skl_write_plane_wm (satoru)
    for (int lv = 0; lv < 6; lv++) wr(PL_WM0 + 4u * (uint32_t)lv, IgpuCalc::WmRegVal(w.wm[lv]));
    wr(PL_WM_TRANS, IgpuCalc::WmRegVal(w.trans));
    wr(PL_WM_SAGV, IgpuCalc::WmRegVal(w.sagv));
    wr(PL_WM_SAGV_TRANS, IgpuCalc::WmRegVal(w.sagv_trans));
    wr(PL_BUF_CFG, IgpuCalc::DdbRegVal(w.ddb_start, w.ddb_end));
    // icl_plane_update_arm: plane_ctl then plane_surf (arms) (satoru)
    wr(PL_CTL, PLANE_CTL_ENABLE | PLANE_FMT_XRGB8888);
    wr(PL_SURF, g_s.ggtt_off);
    // verify: surflive follows within ~3 frames, scanline moves (satoru)
    if (!wait_reg(PL_SURFLIVE, 0xFFFFF000u, g_s.ggtt_off, 60000, &last)) {
        Ln l; l.t.s("warn: plane_surflive 0x"); l.t.x(last, 8); l.t.s(" did not latch 0x"); l.t.x(g_s.ggtt_off, 8); l.go();
    }
    uint32_t dsl0 = rd(P_DSL) & 0xFFFFF;
    udelay(3000);
    uint32_t dsl1 = rd(P_DSL) & 0xFFFFF;
    Ln l; l.t.s("pipe b: transconf 0x"); l.t.x(rd(P_TRANSCONF), 8); l.t.s(" pipedsl "); l.t.d(dsl0); l.t.s(" -> "); l.t.d(dsl1);
    l.t.s(dsl0 != dsl1 ? " (scanning)" : " (NOT moving)"); l.t.s(" surflive 0x"); l.t.x(rd(PL_SURFLIVE), 8); l.go();
    return dsl0 != dsl1;
}

// ---- gates (satoru) ----
bool power_wells() {
    uint32_t pw = rd(R_PWR_WELL_CTL2);
    auto on = [&](int idx) { return (bool)((pw >> (idx * 2)) & 1); };
    Ln l; l.t.s("power wells: ctl2 0x"); l.t.x(pw, 8); l.t.s(" pw1="); l.t.d(on(PW_1)); l.t.s(" pw2="); l.t.d(on(PW_2));
    l.t.s(" pwA="); l.t.d(on(PW_A)); l.t.s(" pwB="); l.t.d(on(PW_B)); l.go();
    if (!on(PW_1) || !on(PW_2)) { say("pw1/pw2 off, refusing (not ours to bring up)"); return false; }
    if (!on(PW_B)) {
        // hsw_power_well_enable for pw_b only: set the request bit, wait the
        // state bit (1 ms, hsw_wait_for_power_well_enable). never cleared. (satoru)
        rmw(R_PWR_WELL_CTL2, 0, 1u << (PW_B * 2 + 1));
        uint32_t last = 0;
        if (!wait_reg(R_PWR_WELL_CTL2, 1u << (PW_B * 2), 1u << (PW_B * 2), 1000, &last)) {
            Ln m; m.t.s("pw_b did not come up (ctl2 0x"); m.t.x(last, 8); m.t.s(")"); m.go();
            return false;
        }
        say("pw_b requested and up");
    }
    return true;
}

bool pick_mode(const DpEdid::Edid& e, uint32_t target_mhz) {
    if (e.preferred < 0) { say("edid has no preferred timing"); return false; }
    const DpEdid::Timing& pt = e.t[e.preferred];
    int best = -1;
    uint32_t bestd = 0xFFFFFFFFu;
    for (int i = 0; i < e.nt; i++) {
        const DpEdid::Timing& t = e.t[i];
        if (t.hact != pt.hact || t.vact != pt.vact || t.interlaced) continue;
        uint32_t d = t.refresh_mhz > target_mhz ? t.refresh_mhz - target_mhz : target_mhz - t.refresh_mhz;
        if (d < bestd || (d == bestd && best >= 0 && t.pixclk_khz < e.t[best].pixclk_khz)) { bestd = d; best = i; }
    }
    if (best < 0) best = e.preferred;
    const DpEdid::Timing& t = e.t[best];
    if (!IgpuCalc::ModeFromDtd(t.pixclk_khz, t.hact, t.hblank, t.hfp, t.hsync, t.vact, t.vblank, t.vfp, t.vsync,
                               t.hpos, t.vpos, t.interlaced, &g_s.mode)) { say("edid timing is not usable"); return false; }
    char tb[160];
    DpEdid::FormatTiming(t, tb, sizeof(tb));
    Ln l; l.t.s("mode: "); l.t.s(tb); l.go();
    return true;
}

int finish(int code) {
    if (g_out && g_out_pos >= g_out_max - 2)
        SerialLogger::Log("[igpu-mode] terminal output truncated, full report is in the serial log\r\n");
    (void)code;
    return g_out_pos;
}

void begin_output(char* out, int maxo) {
    g_out = out;
    g_out_max = maxo;
    g_out_pos = 0;
    if (out && maxo > 0) out[0] = 0;
}

} // namespace

int IntelModeset::Light(char* out, int maxo, uint32_t target_hz) {
    begin_output(out, maxo);
    if (g_s.lit) { say("already lit, `gpumode off` first"); return finish(0); }
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) { IntelDisplayProbe::Run(nullptr, 0); g_mmio = IntelDisplayProbe::Mmio(); }
    if (!g_mmio) { say("no usable intel display ver 14 engine (see [igpu] lines), modeset skipped"); return finish(0); }
    clock_init();
    g_run_deadline = g_tpu ? now_us() + RUN_BUDGET_US : 0;
    uint64_t t0 = now_us();
    g_writes = g_blocked = g_mb_ops = 0;
    g_s.steps = 0;
    say("---- intel display stage 2: modeset of an external dp sink on pipe b ----");
    if (!g_tpu) say("warning: no tsc frequency, waits use the io-delay clock and the run budget is off");

    // ---- which type-c port: the first one stage 1 read a sink on (satoru) ----
    IntelDpAux::SinkInfo si = {};
    int tc = 0;
    for (int pass = 0; pass < 2 && !tc; pass++) {
        for (int i = 1; i <= 4; i++) if (IntelDpAux::LastSink(i, &si) && si.sink_count >= 1 && si.edid_len >= 128) { tc = i; break; }
        if (!tc && pass == 0) { say("no sink on record, rerunning the stage 1 aux probe"); IntelDpAux::Run(nullptr, 0); }
    }
    if (!tc) { say("no external dp sink with an edid on a type-c port, nothing to light"); return finish(0); }
    make_port(tc, &g_s.port);
    const Port& p = g_s.port;
    build_allowlist(p);
    snapshot_pipe_a(g_s.pipe_a_before);
    {
        Ln l; l.t.s("port TC"); l.t.d((uint64_t)tc); l.t.s(" (enum port "); l.t.d((uint64_t)p.port); l.t.s(") regs 0x"); l.t.x(p.base, 6);
        l.t.s(" ddi_buf 0x"); l.t.x(p.ddi_buf, 5); l.t.s(" | sink dpcd rev 0x"); l.t.x(si.dpcd[0], 2); l.t.s(" max rate 0x"); l.t.x(si.dpcd[1], 2);
        l.t.s(" lanes "); l.t.d(si.dpcd[2] & 0x1F); l.t.s(" sink_count "); l.t.d(si.sink_count); l.go();
    }

    // ---- gates (satoru) ----
    if (!power_wells()) return finish(0);
    uint32_t dc = rd(R_DC_STATE_EN);
    if (dc & 3) { Ln l; l.t.s("dc_state_en 0x"); l.t.x(dc, 8); l.t.s(" allows dc5/dc6, refusing (cx0 needs dc off)"); l.go(); return finish(0); }
    uint32_t b1 = rd(p.buf1), tcss = rd(R_TCSS_DDI_STATUS + 4u * (uint32_t)(tc - 1)), pica = rd(R_PICA_ISR);
    bool dpalt = (pica >> (16 + tc - 1)) & 1;
    {
        Ln l; l.t.s("tc: buf_ctl1 0x"); l.t.x(b1, 8); l.t.s(" owned="); l.t.d((b1 & BUF1_TC_OWNED) != 0); l.t.s(" tcss_pwr=");
        l.t.d((b1 & BUF1_TCSS_PWR_ST) != 0); l.t.s(" phy_rdy="); l.t.d((b1 & BUF1_SOC_PHY_READY) != 0); l.t.s(" | tcss_ddi_status 0x");
        l.t.x(tcss, 8); l.t.s(" | dp-alt live="); l.t.d(dpalt); l.go();
    }
    if (!(b1 & BUF1_TC_OWNED) || !(b1 & BUF1_TCSS_PWR_ST) || !(b1 & BUF1_SOC_PHY_READY) || !dpalt) {
        say("type-c phy is not owned / powered / ready in dp-alt mode, refusing (no tc connect sequence here)"); return finish(0);
    }
    // intel_tc.c get_pin_assignment / mtl_get_max_lane_count (satoru)
    uint32_t pa = rd(p.fia_pa1);
    uint32_t pin = (pa >> (4 * p.fia_idx)) & 0xF;
    g_s.pin = pin ? (char)('A' + pin - 1) : '-';
    g_s.max_lanes = (pin == 3 || pin == 5) ? 4 : (pin == 4 ? 2 : (pin == 0 ? 0 : 2));
    {
        Ln l; l.t.s("tc: port_tx_dflexpa1 0x"); l.t.x(pa, 8); l.t.s(" pin assignment "); l.t.c(g_s.pin); l.t.s(" -> max lanes ");
        l.t.d((uint64_t)g_s.max_lanes); l.go();
    }
    if (g_s.max_lanes == 0) { say("no dp pin assignment, refusing"); return finish(0); }
    g_s.owned = g_s.max_lanes > 2 ? BOTH_LANES : LANE0;   // intel_cx0_get_owned_lane_mask (satoru)
    // pipe b / the port must be idle and no other transcoder on this port (satoru)
    uint32_t tconf = rd(P_TRANSCONF), tfunc = rd(T_DDI_FUNC), dbuf = rd(p.ddi_buf), pctl = rd(PL_CTL);
    {
        Ln l; l.t.s("pipe b: transconf 0x"); l.t.x(tconf, 8); l.t.s(" ddi_func 0x"); l.t.x(tfunc, 8); l.t.s(" plane_ctl 0x"); l.t.x(pctl, 8);
        l.t.s(" | ddi_buf 0x"); l.t.x(dbuf, 8); l.t.s(" clock_ctl 0x"); l.t.x(rd(p.clk), 8); l.go();
    }
    if ((tconf & TRANSCONF_ENABLE) || (tfunc & DDI_FUNC_ENABLE) || (dbuf & (1u << 31)) || (pctl & PLANE_CTL_ENABLE)) {
        say("pipe b or the port is already in use, refusing"); return finish(0);
    }
    // only transcoders whose pipe well is up are read (pw_a idx 5, pw_c 7,
    // pw_d 8); an unpowered well's registers are not touched (satoru)
    static const uint32_t FUNC[4] = { 0x60400, 0x61400, 0x62400, 0x63400 };
    static const int PIPE_PW[4] = { 5, 6, 7, 8 };
    uint32_t pwctl = rd(R_PWR_WELL_CTL2);
    for (int t = 0; t < 4; t++) {
        if (t == 1 || !((pwctl >> (PIPE_PW[t] * 2)) & 1)) continue;
        uint32_t f = rd(FUNC[t]);
        if ((f & DDI_FUNC_ENABLE) && ((f >> 27) & 0xF) == (uint32_t)(p.port + 1)) {
            Ln l; l.t.s("transcoder "); l.t.c((char)('A' + t)); l.t.s(" already drives this port, refusing"); l.go(); return finish(0);
        }
    }
    // dbuf: mbus must not be joined (pipe a would own all four slices) and
    // pipe a's ddb must sit in bank 0 (adlp_allowed_dbufs: a -> s1|s2,
    // b -> s3|s4) (satoru)
    uint32_t mbus = rd(R_MBUS_CTL);
    uint32_t pa_ddb_max = 0;
    for (int pl = 0; pl < 5; pl++) {
        uint32_t v = rd(0x7027c + 0x100u * (uint32_t)pl);
        if (v) { uint32_t e = ((v >> 16) & 0x1FFF) + 1; if (e > pa_ddb_max) pa_ddb_max = e; }
    }
    uint32_t cur = rd(0x7017c);
    if (cur) { uint32_t e = ((cur >> 16) & 0x1FFF) + 1; if (e > pa_ddb_max) pa_ddb_max = e; }
    {
        Ln l; l.t.s("dbuf: mbus_ctl 0x"); l.t.x(mbus, 8); l.t.s(" joined="); l.t.d(mbus >> 31); l.t.s(" | slices");
        for (int s = 0; s < 4; s++) { l.t.s(" s"); l.t.d((uint64_t)(s + 1)); l.t.s("=0x"); l.t.x(rd(R_DBUF_CTL[s]), 8); }
        l.t.s(" | pipe a ddb end "); l.t.d(pa_ddb_max); l.go();
    }
    if (mbus & (1u << 31)) { say("mbus is joined: pipe a owns all dbuf slices; un-joining rewrites pipe a's ddb, refusing"); return finish(0); }
    if (pa_ddb_max > 2048) { say("pipe a ddb reaches past bank 0, refusing"); return finish(0); }

    // ---- mode + link (satoru) ----
    static DpEdid::Edid e;
    if (!DpEdid::DecodeEdid(si.edid, si.edid_len, &e)) { say("edid did not decode"); return finish(0); }
    if (!pick_mode(e, (target_hz ? target_hz : 60) * 1000)) return finish(0);
    uint32_t cd = rd(R_CDCLK_CTL);
    uint32_t cdclk = (cd & 0x7FF) * 500 + 1000;
    {
        Ln l; l.t.s("cdclk: ctl 0x"); l.t.x(cd, 8); l.t.s(" -> "); l.t.d(cdclk); l.t.s(" khz, mode needs "); l.t.d((g_s.mode.clock_khz + 1) / 2);
        l.t.s(" (2 px/clk)"); l.go();
    }
    if (cdclk < (g_s.mode.clock_khz + 1) / 2) { say("cdclk too low for this mode, refusing (a cdclk change would touch pipe a)"); return finish(0); }

    if (!IntelDpAux::BeginSession(tc, 2500000, 4000)) { say("aux session failed"); return finish(0); }
    // keep the aux power (tc-cold block) for as long as we are lit (satoru)
    if (!IntelDpAux::AuxPower(tc, true, &g_s.aux_was_on)) { say("aux power request failed"); return finish(0); }
    g_s.steps |= S_AUXPWR;
    init_lttpr();
    if (!read_dprx_caps(g_s.dpcd)) { rollback("dprx caps read failed"); return finish(0); }
    g_s.enhanced = g_s.dpcd[0] >= 0x11 && (g_s.dpcd[2] & 0x80);   // drm_dp_enhanced_frame_cap (satoru)
    uint32_t max_rate = IgpuCalc::BwCodeToRate(g_s.dpcd[1]);
    if (max_rate > 810000) max_rate = 810000;                     // c20 dp1.4 tables stop at hbr3 (satoru)
    int max_lanes = g_s.dpcd[2] & 0x1F;
    if (max_lanes > g_s.max_lanes) max_lanes = g_s.max_lanes;
    if (g_s.lttpr_count > 0) {
        uint32_t lr = IgpuCalc::BwCodeToRate(g_s.lttpr_caps[1]);
        if (lr && lr < max_rate) max_rate = lr;
        int ll = g_s.lttpr_caps[4] & 0x1F;
        if (ll && ll < max_lanes) max_lanes = ll;
    }
    LinkCfg cfgs[12];
    int ncfg = IgpuCalc::LinkConfigs(max_rate, max_lanes, g_s.mode.clock_khz, g_s.mode.hdisplay, 24, cfgs, 12);
    {
        Ln l; l.t.s("link: max "); l.t.d(max_rate / 1000); l.t.s(" mhz x"); l.t.d((uint64_t)max_lanes); l.t.s(", mode needs ");
        l.t.d(IgpuCalc::ModeDataRate(g_s.mode.clock_khz, 24)); l.t.s(" kB/s, candidates:");
        for (int i = 0; i < ncfg; i++) { l.t.c(' '); l.t.d(cfgs[i].rate_khz / 1000); l.t.c('x'); l.t.d((uint64_t)cfgs[i].lanes); }
        l.go();
    }
    if (!ncfg) { rollback("no link config carries this mode"); return finish(0); }

    // ---- framebuffer + ggtt (satoru) ----
    if (!fb_alloc(g_s.mode.hdisplay, g_s.mode.vdisplay)) { rollback("framebuffer"); return finish(0); }
    fb_pattern();
    if (!ggtt_setup()) { rollback("ggtt"); return finish(0); }

    // ---- pre plane: pmdemand, dbuf slices (intel_atomic_commit_tail
    // order) (satoru) ----
    uint32_t pipes = 1;
    static const uint32_t TCONF[4] = { 0x70008, 0x71008, 0x72008, 0x73008 };
    pwctl = rd(R_PWR_WELL_CTL2);
    for (int t = 0; t < 4; t++)
        if (t != 1 && ((pwctl >> (PIPE_PW[t] * 2)) & 1) && (rd(TCONF[t]) & TRANSCONF_ENABLE)) pipes++;
    // ddiclk_max covers the fastest config we may fall back to (satoru)
    uint32_t ddi_mhz = 0;
    for (int i = 0; i < ncfg && i < 3; i++) if ((cfgs[i].rate_khz + 999) / 1000 > ddi_mhz) ddi_mhz = (cfgs[i].rate_khz + 999) / 1000;
    if (!pmdemand_raise(pipes, 4, ddi_mhz)) { rollback("pmdemand"); return finish(0); }
    if (!dbuf_bank1_on()) { rollback("dbuf"); return finish(0); }

    PlaneWms wms;
    compute_wms(&wms);
    if (!wms.wm[0].enable) { rollback("plane wm0 does not fit"); return finish(0); }

    // ---- link, with linux style fallback to the next config (satoru) ----
    bool up = false;
    for (int i = 0; i < ncfg && i < 3 && !up; i++) {
        if (over_budget()) { say("run budget exceeded before a link attempt"); break; }
        if (link_up(cfgs[i])) { up = true; break; }
        Ln l; l.t.s("link attempt "); l.t.d(cfgs[i].rate_khz / 1000); l.t.c('x'); l.t.d((uint64_t)cfgs[i].lanes); l.t.s(" failed, port down"); l.go();
        if (g_s.steps & S_FUNC_CFG) rmw(T_DDI_FUNC, DDI_FUNC_ENABLE | DDI_FUNC_PORT_MASK | DDI_FUNC_MODE_MASK, 0);
        port_down();
    }
    if (!up) { rollback("link training failed on every attempted config"); return finish(0); }

    // ---- pipe, transcoder, plane (satoru) ----
    if (!pipe_up(wms)) { rollback("pipe b did not come up"); return finish(0); }

    // ---- verify the sink side (satoru) ----
    uint8_t st[6] = {};
    if (dpcd_rd(0x202, st, 6) == 6) {
        bool okst = DpLt::ChannelEqOk(st, g_s.link.lanes);
        Ln l; l.t.s("sink after enable: lane status"); for (int i = 0; i < 6; i++) { l.t.c(' '); l.t.x(st[i], 2); }
        l.t.s(okst ? " (locked)" : " (NOT locked)"); l.t.s(" sink_status in_sync="); l.t.d(st[3] & 1); l.go();
    }
    diff_pipe_a("after enable");
    g_s.lit = true;
    {
        Ln l; l.t.s("summary: LIT TC"); l.t.d((uint64_t)tc); l.t.s(" pipe B "); l.t.d(g_s.mode.hdisplay); l.t.c('x'); l.t.d(g_s.mode.vdisplay);
        l.t.c('@'); l.t.hz(g_s.mode.refresh_mhz); l.t.s(" link "); l.t.d(g_s.link.rate_khz / 1000); l.t.s("x"); l.t.d((uint64_t)g_s.link.lanes);
        l.t.s(" pin "); l.t.c(g_s.pin); l.t.s(" lttprs "); l.t.d((uint64_t)g_s.lttpr_count); l.t.s(" | fb 0x"); l.t.x((uint32_t)(g_s.fb_phys >> 32), 8);
        l.t.x((uint32_t)g_s.fb_phys, 8); l.t.s(" ggtt 0x"); l.t.x(g_s.ggtt_off, 8); l.t.s(" | mmio writes "); l.t.d((uint64_t)g_writes);
        l.t.s(" phy ops "); l.t.d((uint64_t)g_mb_ops); l.t.s(" blocked "); l.t.d((uint64_t)g_blocked); l.t.s(" | ");
        l.t.d(g_tpu ? (now_us() - t0) / 1000 : 0); l.t.s(" ms"); l.go();
    }
    return finish(1);
}

int IntelModeset::Off(char* out, int maxo) {
    begin_output(out, maxo);
    if (!g_s.lit && !g_s.steps) { say("nothing lit"); return finish(0); }
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) { say("no display engine mapped"); return finish(0); }
    clock_init();
    g_run_deadline = 0;
    IntelDpAux::BeginSession(g_s.port.tc, 500000, 200);
    rollback("gpumode off");
    say("summary: OFF (framebuffer memory kept)");
    return finish(0);
}

int IntelModeset::Status(char* out, int maxo) {
    begin_output(out, maxo);
    g_mmio = IntelDisplayProbe::Mmio();
    if (!g_mmio) { say("no usable intel display ver 14 engine"); return finish(0); }
    clock_init();
    Ln l; l.t.s("status: "); l.t.s(g_s.lit ? "lit" : "off"); l.t.s(" steps 0x"); l.t.x(g_s.steps, 4);
    if (g_s.last_result[0]) { l.t.s(" | last link: "); l.t.s(g_s.last_result); }
    l.go();
    if (!g_s.port.tc) return finish(0);
    uint32_t d0 = rd(P_DSL) & 0xFFFFF;
    udelay(2000);
    uint32_t d1 = rd(P_DSL) & 0xFFFFF;
    Ln m; m.t.s("pipe b: transconf 0x"); m.t.x(rd(P_TRANSCONF), 8); m.t.s(" pipedsl "); m.t.d(d0); m.t.s(" -> "); m.t.d(d1);
    m.t.s(" plane_ctl 0x"); m.t.x(rd(PL_CTL), 8); m.t.s(" surflive 0x"); m.t.x(rd(PL_SURFLIVE), 8); m.t.s(" | ddi_buf 0x");
    m.t.x(rd(g_s.port.ddi_buf), 8); m.t.s(" dp_tp_status 0x"); m.t.x(rd(T_DP_TP_STATUS), 8); m.t.s(" clock_ctl 0x"); m.t.x(rd(g_s.port.clk), 8);
    m.go();
    if (g_s.lit) {
        IntelDpAux::BeginSession(g_s.port.tc, 200000, 50);
        uint8_t st[6] = {};
        if (dpcd_rd(0x202, st, 6) == 6) {
            Ln n; n.t.s("sink: lane status"); for (int i = 0; i < 6; i++) { n.t.c(' '); n.t.x(st[i], 2); }
            n.t.s(DpLt::ChannelEqOk(st, g_s.link.lanes) ? " (locked)" : " (NOT locked)"); n.go();
        }
        Ln n; n.t.s("mode "); n.t.d(g_s.mode.hdisplay); n.t.c('x'); n.t.d(g_s.mode.vdisplay); n.t.c('@'); n.t.hz(g_s.mode.refresh_mhz);
        n.t.s(" link "); n.t.d(g_s.link.rate_khz / 1000); n.t.c('x'); n.t.d((uint64_t)g_s.link.lanes); n.go();
    }
    return finish(0);
}

int IntelModeset::Boot(bool disabled) {
    begin_output(nullptr, 0);
    if (disabled) { say("skipped (kurono.igpu.modeset=0)"); return 0; }
    if (!IntelDisplayProbe::Mmio()) { say("no usable intel display ver 14 engine, skipped"); return 0; }
    // file kill switch: KUROMODE.OFF in the KURONO fat root (8.3 name
    // "KUROMODEOFF"), so a fixed efi command line can still turn this off (satoru)
    if (DiskLog::Ready()) {
        uint64_t sz = 0;
        if (DiskLog::StatRootFile("KUROMODEOFF", &sz) == 0) { say("skipped (KUROMODE.OFF found on the KURONO partition)"); return 0; }
    } else {
        say("note: KURONO partition not open, the KUROMODE.OFF kill switch could not be checked");
    }
    IntelDpAux::SinkInfo si = {};
    bool any = false;
    for (int i = 1; i <= 4; i++) if (IntelDpAux::LastSink(i, &si) && si.sink_count >= 1) any = true;
    if (!any) { say("no external dp sink on a type-c port in the stage 1 probe, skipped"); return 0; }
    Light(nullptr, 0, 60);
    return 0;
}

bool IntelModeset::GetOutput(uint64_t* phys, int* w, int* h, uint32_t* pitch) {
    if (!g_s.lit || !g_s.fb_phys) return false;
    if (phys) *phys = g_s.fb_phys;
    if (w) *w = g_s.fb_w;
    if (h) *h = g_s.fb_h;
    if (pitch) *pitch = g_s.pitch;
    return true;
}

bool IntelModeset::GetOutputGgtt(uint32_t* ggtt) {
    if (!g_s.lit || !g_s.ggtt_off) return false;
    if (ggtt) *ggtt = g_s.ggtt_off;
    return true;
}

// end (satoru)
