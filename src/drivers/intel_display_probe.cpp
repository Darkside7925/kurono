// intel display stage 0: read-only probe of the firmware-programmed display
// state on display version 14 (meteor lake / arrow lake). see the header for
// the contract. (satoru)
//
// safety rules this file follows (satoru):
//  - mmio is only ever read. there is no write helper in this file at all.
//  - no gtt, no forcewake (display registers do not need it, linux reads
//    gmd_id_display with a bare ioread32), no power well requests.
//  - pci config space is only read (no bar sizing, no command register edit).
//  - registers inside a power well are only read when that well's state bit
//    says it is on; otherwise the block is reported as skipped.
//  - mmio is skipped entirely unless the device is in d0 with memory decode on.
//
// every offset below comes from the linux kernel tree, torvalds/linux master,
// drivers/gpu/drm/i915/display/<file> (cited per block). (satoru)
#include "intel_display_probe.h"
#include "serial.h"
#include "../kernel/vmm.h"

namespace {

// intel_display_regs.h: gmd_id_display, arch 31:22, release 21:14, step 5:0 (satoru)
constexpr uint32_t R_GMD_ID_DISPLAY = 0x510a0;

// intel_display_regs.h: hsw_pwr_well_ctl1 (bios) / ctl2 (driver), req bit
// 2*idx+1, state bit 2*idx. idx: pw_1=0, pw_2=1, pw_a=5, pw_b=6, pw_c=7,
// pw_d=8 (icl_pw_ctl_idx_* / xelpd_pw_ctl_idx_*). (satoru)
constexpr uint32_t R_PWR_WELL_CTL1 = 0x45400;
constexpr uint32_t R_PWR_WELL_CTL2 = 0x45404;
constexpr int PW_1 = 0, PW_2 = 1, PW_A = 5, PW_B = 6, PW_C = 7, PW_D = 8;
// intel_display_regs.h: dc_state_en, bits 1:0 = 0 off, 1 dc5, 2 dc6, 3 dc3co (satoru)
constexpr uint32_t R_DC_STATE_EN = 0x45504;

// intel_display_regs.h: transcoder timing block at 0x60000 + n*0x1000
// (transcoder_*_offset in intel_display_device.c). htotal 0x00, hblank 0x04,
// hsync 0x08, vtotal 0x0c, vblank 0x10, vsync 0x14, pipesrc 0x1c, each field
// stored minus one. data_m1 0x30, data_n1 0x34, link_m1 0x40, link_n1 0x44
// (_pipea_*_m1/n1, m/n in bits 23:0). trans_ddi_func_ctl 0x400. (satoru)
constexpr uint32_t TRANS_BASE = 0x60000, TRANS_STRIDE = 0x1000;
constexpr uint32_t T_HTOTAL = 0x00, T_HBLANK = 0x04, T_HSYNC = 0x08;
constexpr uint32_t T_VTOTAL = 0x0c, T_VBLANK = 0x10, T_VSYNC = 0x14;
constexpr uint32_t T_PIPESRC = 0x1c;
constexpr uint32_t T_DATA_M1 = 0x30, T_DATA_N1 = 0x34;
constexpr uint32_t T_LINK_M1 = 0x40, T_LINK_N1 = 0x44;
constexpr uint32_t T_DDI_FUNC_CTL = 0x400;

// intel_display_regs.h: pipe block at 0x70000 + n*0x1000 (pipe_*_offset in
// intel_display_device.c). pipedsl 0x00 (line 19:0), transconf 0x08 (bit 31
// enable, bit 30 state). (satoru)
// skl_universal_plane_regs.h: plane 1 of the pipe: plane_ctl 0x180, stride
// 0x188, pos 0x18c, size 0x190, surf 0x19c, offset 0x1a4, surflive 0x1ac. (satoru)
constexpr uint32_t PIPE_BASE = 0x70000, PIPE_STRIDE = 0x1000;
constexpr uint32_t P_PIPEDSL = 0x00, P_TRANSCONF = 0x08;
constexpr uint32_t P_PLANE_CTL = 0x180, P_PLANE_STRIDE = 0x188, P_PLANE_POS = 0x18c;
constexpr uint32_t P_PLANE_SIZE = 0x190, P_PLANE_SURF = 0x19c, P_PLANE_OFFSET = 0x1a4;
constexpr uint32_t P_PLANE_SURFLIVE = 0x1ac;

// intel_display_regs.h: trans_clk_sel 0x46140 + 4*trans, tgl+ field 31:28 =
// phy + 1 (intel_ddi_enable_transcoder_clock in intel_ddi.c; phy a=0, b=1,
// tc1..4 = phy_f.. per intel_port_to_phy in intel_display.c). (satoru)
constexpr uint32_t R_TRANS_CLK_SEL = 0x46140;

// intel_display_regs.h: ddi_buf_ctl 0x64000 + port*0x100 (_mmio_port, port
// a=0, b=1, tc1=3 .. tc4=6 per intel_display_limits.h). (satoru)
// intel_cx0_phy_regs.h: ddi_clk_valfreq 0x64030 + port*0x100 (link symbol
// clock in khz, see intel_ddi_link_symbol_clock), xelpdp_port_buf_ctl1
// a 0x64004 b 0x64104 usbc1 0x16f200 (+0x200 per tc), xelpdp_port_clock_ctl
// a 0x640e0 b 0x641e0 usbc1 0x16f260 (+0x200 per tc). (satoru)
// intel_dp_aux_regs.h: dp_aux_ch_ctl a 0x64010 b 0x64110, xelpdp usbc1
// 0x16f210 (+0x200 per tc), bit 18 power status, bit 19 power request. (satoru)
constexpr uint32_t R_DDI_BUF_CTL = 0x64000, R_DDI_CLK_VALFREQ = 0x64030, DDI_STRIDE = 0x100;

// intel_display_regs.h: sdeisr 0xc4000 (ddi hpd bit 16+pin, tc legacy bit
// 24+pin, hpd_mtp table in intel_hotplug_irq.c), shotplug_ctl_ddi 0xc4030 and
// shotplug_ctl_tc 0xc4034 (enable 0x8 << 4*pin, status 0x3 << 4*pin),
// picainterrupt_isr 0x16fe50 (tbt bit pin, dp-alt bit 16+pin, hpd_xelpdp),
// xelpdp_port_hotplug_ctl 0x16f270 + 0x200*pin (bit 6 tbt en, bit 2 dp-alt
// en, bits 5:4 tbt long/short, bits 1:0 dp-alt long/short). (satoru)
constexpr uint32_t R_SDEISR = 0xc4000, R_SHOTPLUG_CTL_DDI = 0xc4030, R_SHOTPLUG_CTL_TC = 0xc4034;
constexpr uint32_t R_PICA_ISR = 0x16fe50, R_XELPDP_PORT_HOTPLUG_CTL = 0x16f270;

// intel_pps_regs.h + intel_pps.c: pch_pps_base 0xc7200, pp_status +0,
// pp_control +4, pp_on_delays +8, second pps instance at +0x100. (satoru)
constexpr uint32_t R_PCH_PPS_BASE = 0xc7200;

// only the first 2 mb of bar0 is mapped; the highest register read above is
// 0x16fe50. (satoru)
constexpr uint64_t MMIO_WINDOW = 0x200000;

struct PortDesc {
    const char* name;
    int idx;          // linux enum port value (a=0, b=1, tc1=3 ..) (satoru)
    int tc;           // 0 for combo ports, 1..4 for type-c ports (satoru)
    uint32_t buf1, clk, aux;
};

// display version 14 port_mask: a, b, tc1..tc4 (xe_lpdp_features in
// intel_display_device.c). (satoru)
const PortDesc PORTS[] = {
    { "A",   0, 0, 0x64004,  0x640e0,  0x64010  },
    { "B",   1, 0, 0x64104,  0x641e0,  0x64110  },
    { "TC1", 3, 1, 0x16f200, 0x16f260, 0x16f210 },
    { "TC2", 4, 2, 0x16f400, 0x16f460, 0x16f410 },
    { "TC3", 5, 3, 0x16f600, 0x16f660, 0x16f610 },
    { "TC4", 6, 4, 0x16f800, 0x16f860, 0x16f810 },
};
constexpr int NPORTS = (int)(sizeof(PORTS) / sizeof(PORTS[0]));

volatile uint8_t* g_mmio = nullptr;
bool g_mmio_ok = false;   // last Run reached ver 14 with pw_1 up, see Mmio() (satoru)
uint64_t g_mapped_base = 0;
char* g_out = nullptr;
int g_out_max = 0;
int g_out_pos = 0;

inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t*)(g_mmio + off);
}

// one output line. emits to serial, and to the shell buffer if any. (satoru)
struct Ln {
    char b[256];
    int n;
    Ln() : n(0) { b[0] = 0; s("[igpu] "); }
    Ln& s(const char* t) { while (t && *t && n < 254) b[n++] = *t++; b[n] = 0; return *this; }
    Ln& c(char ch) { if (n < 254) { b[n++] = ch; b[n] = 0; } return *this; }
    Ln& x(uint32_t v, int digits = 8) {
        s("0x");
        for (int i = digits - 1; i >= 0; i--) c("0123456789abcdef"[(v >> (i * 4)) & 0xF]);
        return *this;
    }
    Ln& x64(uint64_t v) {
        s("0x");
        for (int i = 15; i >= 0; i--) c("0123456789abcdef"[(v >> (i * 4)) & 0xF]);
        return *this;
    }
    Ln& d(uint64_t v) {
        char t[24]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) c(t[--k]);
        return *this;
    }
    Ln& b01(bool v) { return c(v ? '1' : '0'); }
    void emit() {
        SerialLogger::Log(b);
        SerialLogger::Log("\r\n");
        if (g_out && g_out_pos < g_out_max - 1) {
            for (int i = 0; b[i] && g_out_pos < g_out_max - 2; i++) g_out[g_out_pos++] = b[i];
            g_out[g_out_pos++] = '\n';
            g_out[g_out_pos] = 0;
        }
    }
};

void say(const char* t) { Ln l; l.s(t); l.emit(); }

// pci config reads through 0xcf8/0xcfc with interrupts held off so another
// path on this cpu can not interleave an address write. read only. (satoru)
uint32_t cfg_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (off & 0xFC);
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
    __asm__ __volatile__("outl %0, %w1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    if (fl & 0x200) __asm__ __volatile__("sti" : : : "memory");
    return v;
}

const char* mode_name(uint32_t m) {
    switch (m) {
        case 0: return "hdmi";
        case 1: return "dvi";
        case 2: return "dp-sst";
        case 3: return "dp-mst";
        case 4: return "128b132b";
        default: return "?";
    }
}

const char* bpc_name(uint32_t b) {
    switch (b) {
        case 0: return "8";
        case 1: return "10";
        case 2: return "6";
        case 3: return "12";
        default: return "?";
    }
}

// trans_ddi_func_ctl tgl+ port select, bits 30:27 = port + 1 (satoru)
const char* port_from_sel(uint32_t sel) {
    for (int i = 0; i < NPORTS; i++) if ((uint32_t)PORTS[i].idx + 1 == sel) return PORTS[i].name;
    return sel ? "?" : "none";
}

int port_index_from_sel(uint32_t sel) {
    for (int i = 0; i < NPORTS; i++) if ((uint32_t)PORTS[i].idx + 1 == sel) return i;
    return -1;
}

// trans_clk_sel value = phy + 1; phy a=0, b=1, tc1..tc4 = 5..8 (satoru)
const char* phy_from_clksel(uint32_t v) {
    switch (v) {
        case 0: return "disabled";
        case 1: return "A";
        case 2: return "B";
        case 6: return "TC1";
        case 7: return "TC2";
        case 8: return "TC3";
        case 9: return "TC4";
        default: return "?";
    }
}

// xelpdp_port_clock_ctl ddi clock select, bits 15:12 (intel_cx0_phy_regs.h) (satoru)
const char* clksel_name(uint32_t v) {
    switch (v) {
        case 0x0: return "none";
        case 0x8: return "maxpclk(phy pll)";
        case 0x9: return "div18clk";
        case 0xc: return "tbt-162";
        case 0xd: return "tbt-270";
        case 0xe: return "tbt-540";
        case 0xf: return "tbt-810";
        default: return "?";
    }
}

// skl_universal_plane_regs.h plane_ctl format, icl+ field 27:23 (satoru)
const char* fmt_name(uint32_t f) {
    switch (f) {
        case 0x4: return "xrgb2101010";
        case 0x8: return "xrgb8888";
        case 0xc: return "xrgb16161616f";
        case 0x18: return "indexed8";
        case 0x1c: return "rgb565";
        default: return "other";
    }
}

const char* tiling_name(uint32_t t) {
    switch (t) {
        case 0: return "linear";
        case 1: return "x";
        case 4: return "y";
        case 5: return "4";
        default: return "?";
    }
}

// ddi_port_width_decode: field 4 means 3 lanes, else field + 1 (satoru)
uint32_t ddi_lanes(uint32_t field) { return field == 4 ? 3 : field + 1; }

struct PipeState {
    bool active;          // transconf enable and state (satoru)
    int port;             // index into PORTS or -1 (satoru)
    uint32_t mode;
    uint32_t hact, vact, htot, vtot;
    uint32_t link_m, link_n;
    uint32_t mhz;         // refresh in millihertz from the timings, 0 = unknown (satoru)
};

// the last run's live pipes, kept for RefreshMilliHz (values only, no
// register is read again). (satoru)
struct PipeRefresh { uint32_t hact, vact, mhz; };
PipeRefresh g_refresh[4];
int g_refresh_n = 0;

} // namespace

uint32_t IntelDisplayProbe::RefreshMilliHz(uint32_t w, uint32_t h) {
    // the pipe scanning out a w x h mode first, else the first live pipe
    // with a computed refresh. (satoru)
    for (int i = 0; i < g_refresh_n; i++)
        if (g_refresh[i].mhz && g_refresh[i].hact == w && g_refresh[i].vact == h) return g_refresh[i].mhz;
    for (int i = 0; i < g_refresh_n; i++)
        if (g_refresh[i].mhz) return g_refresh[i].mhz;
    return 0;
}

volatile uint8_t* IntelDisplayProbe::Mmio() {
    return g_mmio_ok ? g_mmio : nullptr;
}

int IntelDisplayProbe::Run(char* out, int maxo) {
    g_mmio_ok = false;
    g_out = out;
    g_out_max = maxo;
    g_out_pos = 0;
    if (out && maxo > 0) out[0] = 0;

    // find the intel display function on bus 0 (the igpu is always 00:02.0,
    // but scan the whole bus rather than assume). (satoru)
    int fdev = -1, ffn = -1;
    for (int dev = 0; dev < 32 && fdev < 0; dev++) {
        uint32_t id0 = cfg_rd(0, (uint8_t)dev, 0, 0x00);
        if ((id0 & 0xFFFF) == 0xFFFF) continue;
        bool multi = (cfg_rd(0, (uint8_t)dev, 0, 0x0C) >> 16) & 0x80;
        for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
            uint32_t id = cfg_rd(0, (uint8_t)dev, (uint8_t)fn, 0x00);
            if ((id & 0xFFFF) != 0x8086) continue;
            uint32_t cls = cfg_rd(0, (uint8_t)dev, (uint8_t)fn, 0x08);
            if ((cls >> 24) == 0x03) { fdev = dev; ffn = fn; break; }
        }
    }
    if (fdev < 0) {
        say("no intel display device (vendor 8086, class 03) on pci bus 0, probe skipped");
        return g_out_pos;
    }

    uint8_t dv = (uint8_t)fdev, fn = (uint8_t)ffn;
    uint32_t id   = cfg_rd(0, dv, fn, 0x00);
    uint32_t cls  = cfg_rd(0, dv, fn, 0x08);
    uint32_t cmd  = cfg_rd(0, dv, fn, 0x04);
    uint32_t b0lo = cfg_rd(0, dv, fn, 0x10), b0hi = cfg_rd(0, dv, fn, 0x14);
    uint32_t b2lo = cfg_rd(0, dv, fn, 0x18), b2hi = cfg_rd(0, dv, fn, 0x1C);
    uint64_t bar0 = (uint64_t)(b0lo & ~0xFu) | (((b0lo >> 1) & 3) == 2 ? ((uint64_t)b0hi << 32) : 0);
    uint64_t bar2 = (uint64_t)(b2lo & ~0xFu) | (((b2lo >> 1) & 3) == 2 ? ((uint64_t)b2hi << 32) : 0);

    // power management capability: pmcsr bits 1:0 give the d-state (satoru)
    int dstate = -1;
    if ((cmd >> 16) & 0x10) {
        uint8_t ptr = (uint8_t)(cfg_rd(0, dv, fn, 0x34) & 0xFC);
        for (int guard = 0; ptr && guard < 48; guard++) {
            uint32_t cap = cfg_rd(0, dv, fn, ptr);
            if ((cap & 0xFF) == 0x01) { dstate = (int)(cfg_rd(0, dv, fn, (uint8_t)(ptr + 4)) & 3); break; }
            ptr = (uint8_t)((cap >> 8) & 0xFC);
        }
    }

    say("---- intel display probe, stage 0, read-only ----");
    {
        Ln l; l.s("pci 00:"); l.d((uint64_t)fdev); l.s("."); l.d((uint64_t)ffn);
        l.s(" id "); l.x(id & 0xFFFF, 4); l.s(":"); l.x(id >> 16, 4);
        l.s(" class "); l.x(cls >> 8, 6);
        l.s(" cmd "); l.x(cmd & 0xFFFF, 4);
        l.s(" mem="); l.b01(cmd & 2); l.s(" bm="); l.b01(cmd & 4);
        l.s(" pm=");
        if (dstate < 0) l.s("?"); else { l.c('D'); l.d((uint64_t)dstate); }
        l.emit();
    }
    { Ln l; l.s("bar0(gttmmadr) "); l.x64(bar0); l.s(" bar2(gmadr) "); l.x64(bar2); l.emit(); }

    if (bar0 == 0) { say("bar0 is unassigned, mmio not read"); return g_out_pos; }
    if (!(cmd & 2)) { say("memory decode is off in the pci command register, mmio not read"); return g_out_pos; }
    if (dstate > 0) { say("device is not in d0, mmio not read"); return g_out_pos; }

    // identity-map the register window uncached, same flags as gpu_probe.cpp
    // and intel_gpu.cpp use for this bar, so it never changes their mapping.
    // done once per bar address. (satoru)
    if (g_mapped_base != bar0) {
        for (uint64_t p = bar0; p < bar0 + MMIO_WINDOW; p += 0x1000) {
            if (!KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) {
                say("mapping bar0 failed, mmio not read");
                return g_out_pos;
            }
        }
        g_mapped_base = bar0;
    }
    g_mmio = (volatile uint8_t*)(uintptr_t)bar0;

    uint32_t gmd = rd(R_GMD_ID_DISPLAY);
    uint32_t ver = (gmd >> 22) & 0x3FF, rel = (gmd >> 14) & 0xFF, step = gmd & 0x3F;
    {
        Ln l; l.s("gmd_id_display "); l.x(gmd); l.s(" -> display ver "); l.d(ver); l.c('.'); l.d(rel);
        l.s(" step "); l.d(step);
        l.emit();
    }
    if (gmd == 0 || gmd == 0xFFFFFFFF) {
        say("gmd_id reads 0/ffffffff: no display ip or pre-meteor-lake part, decode skipped");
        return g_out_pos;
    }
    if (ver != 14) {
        say("decode tables here are for display ver 14 only, stopping after the id");
        return g_out_pos;
    }

    // power wells: only read a block when its well state bit is set (satoru)
    uint32_t pw_bios = rd(R_PWR_WELL_CTL1), pw_drv = rd(R_PWR_WELL_CTL2);
    auto pw_on = [&](int idx) -> bool { return (pw_drv >> (idx * 2)) & 1; };
    uint32_t dc = rd(R_DC_STATE_EN);
    {
        Ln l; l.s("pwr_well bios "); l.x(pw_bios); l.s(" drv "); l.x(pw_drv);
        l.s(" state pw1="); l.b01(pw_on(PW_1)); l.s(" pw2="); l.b01(pw_on(PW_2));
        l.s(" pwA="); l.b01(pw_on(PW_A)); l.s(" pwB="); l.b01(pw_on(PW_B));
        l.s(" pwC="); l.b01(pw_on(PW_C)); l.s(" pwD="); l.b01(pw_on(PW_D));
        l.s(" | dc_state_en "); l.x(dc); l.s(" (");
        static const char* dcn[4] = { "dc off", "upto dc5", "upto dc6", "dc3co" };
        l.s(dcn[dc & 3]); l.s(")");
        l.emit();
    }
    if (!pw_on(PW_1)) {
        say("pw1 reports off: display core unpowered, nothing else read");
        return g_out_pos;
    }
    g_mmio_ok = true;

    // ---- ports first, so pipe lines can quote the port clock (satoru) ----
    uint32_t sdeisr = rd(R_SDEISR);
    uint32_t shp_ddi = rd(R_SHOTPLUG_CTL_DDI), shp_tc = rd(R_SHOTPLUG_CTL_TC);
    uint32_t pica_isr = rd(R_PICA_ISR);
    {
        Ln l; l.s("hpd regs sdeisr "); l.x(sdeisr); l.s(" shotplug_ddi "); l.x(shp_ddi);
        l.s(" shotplug_tc "); l.x(shp_tc); l.s(" pica_isr "); l.x(pica_isr);
        l.emit();
    }

    uint32_t valfreq[NPORTS] = {};
    uint32_t clkctl[NPORTS] = {};
    int hpd_state[NPORTS] = {};   // 1 live, 0 not live, -1 unknown (detect off) (satoru)
    for (int i = 0; i < NPORTS; i++) {
        const PortDesc& pd = PORTS[i];
        // tc ddi lanes sit in pw_2 (xelpdp_pw_2_power_domains); the pica
        // registers (buf_ctl1, clock_ctl, aux, hotplug) are always on for
        // usbc1-4 per the mtl note in intel_display_power_map.c. (satoru)
        bool ddi_pw = pd.tc ? pw_on(PW_2) : true;
        uint32_t buf = 0;
        if (ddi_pw) {
            buf = rd(R_DDI_BUF_CTL + (uint32_t)pd.idx * DDI_STRIDE);
            valfreq[i] = rd(R_DDI_CLK_VALFREQ + (uint32_t)pd.idx * DDI_STRIDE);
        }
        uint32_t b1 = rd(pd.buf1);
        uint32_t ck = rd(pd.clk);
        uint32_t aux = rd(pd.aux);
        clkctl[i] = ck;

        Ln l; l.s("port "); l.s(pd.name); l.s(pd.tc ? " (c20)" : " (c10)");
        if (ddi_pw) {
            l.s(" ddi_buf "); l.x(buf); l.s(" en="); l.b01(buf >> 31);
            l.s(" idle="); l.b01((buf >> 7) & 1);
            l.s(" lanes="); l.d(ddi_lanes((buf >> 1) & 7));
            l.s(" init_det="); l.b01(buf & 1);
            l.s(" valfreq="); l.d(valfreq[i]); l.s("khz");
        } else {
            l.s(" ddi_buf skipped (pw2 off)");
        }
        l.emit();

        Ln m; m.s("  "); m.s(pd.name); m.s(" buf_ctl1 "); m.x(b1);
        m.s(" phy_rdy="); m.b01((b1 >> 24) & 1); m.s(" tbt_io="); m.b01((b1 >> 11) & 1);
        m.s(" phy_idle="); m.b01((b1 >> 7) & 1);
        if (pd.tc) {
            m.s(" tc_own="); m.b01((b1 >> 6) & 1);
            m.s(" tcss_pwr req/st="); m.b01((b1 >> 5) & 1); m.c('/'); m.b01((b1 >> 4) & 1);
        }
        m.s(" | clock_ctl "); m.x(ck);
        m.s(" pll req/ack l0="); m.b01(ck >> 31); m.c('/'); m.b01((ck >> 30) & 1);
        m.s(" l1="); m.b01((ck >> 27) & 1); m.c('/'); m.b01((ck >> 26) & 1);
        m.s(" tbtclk="); m.b01((ck >> 19) & 1); m.c('/'); m.b01((ck >> 18) & 1);
        m.s(" sel="); m.s(clksel_name((ck >> 12) & 0xF));
        m.emit();

        Ln h; h.s("  "); h.s(pd.name); h.s(" hpd");
        if (!pd.tc) {
            int pin = pd.idx;  // hpd_port_a/b map to ddi pins 0/1 (xelpd_hpd_pin) (satoru)
            bool en = (shp_ddi >> (pin * 4)) & 0x8;
            bool live = (sdeisr >> (16 + pin)) & 1;
            h.s(" detect_en="); h.b01(en); h.s(" live="); h.b01(live);
            h.s(" latched="); h.d((shp_ddi >> (pin * 4)) & 3);
            hpd_state[i] = en ? (live ? 1 : 0) : (live ? 1 : -1);
        } else {
            int pin = pd.tc - 1;
            uint32_t pctl = rd(R_XELPDP_PORT_HOTPLUG_CTL + (uint32_t)pin * 0x200);
            bool leg_en = (shp_tc >> (pin * 4)) & 0x8;
            bool leg = (sdeisr >> (24 + pin)) & 1;
            bool dpalt_en = (pctl >> 2) & 1, tbt_en = (pctl >> 6) & 1;
            bool dpalt = (pica_isr >> (16 + pin)) & 1, tbt = (pica_isr >> pin) & 1;
            h.s(" legacy en/live="); h.b01(leg_en); h.c('/'); h.b01(leg);
            h.s(" dp-alt en/live="); h.b01(dpalt_en); h.c('/'); h.b01(dpalt);
            h.s(" tbt en/live="); h.b01(tbt_en); h.c('/'); h.b01(tbt);
            h.s(" ctl "); h.x(pctl);
            bool any_en = leg_en || dpalt_en || tbt_en;
            bool any_live = leg || dpalt || tbt;
            hpd_state[i] = any_live ? 1 : (any_en ? 0 : -1);
        }
        h.s(" | aux_ctl "); h.x(aux); h.s(" pwr req/st="); h.b01((aux >> 19) & 1); h.c('/');
        h.b01((aux >> 18) & 1); h.s(" busy="); h.b01(aux >> 31);
        h.emit();
    }

    // ---- panel power sequencer (edp) (satoru) ----
    for (int pps = 0; pps < 2; pps++) {
        uint32_t base = R_PCH_PPS_BASE + (uint32_t)pps * 0x100;
        uint32_t st = rd(base + 0), ctl = rd(base + 4), on = rd(base + 8);
        Ln l; l.s("pps"); l.d((uint64_t)pps); l.s(" pp_status "); l.x(st);
        l.s(" on="); l.b01(st >> 31); l.s(" ready="); l.b01((st >> 30) & 1);
        l.s(" seq="); l.d((st >> 28) & 3); l.s(" state="); l.x(st & 0xF, 1);
        l.s(" | pp_control "); l.x(ctl); l.s(" power_on="); l.b01(ctl & 1);
        l.s(" blc="); l.b01((ctl >> 2) & 1); l.s(" vdd="); l.b01((ctl >> 3) & 1);
        l.s(" | on_delays "); l.x(on);
        l.emit();
    }

    // ---- transcoders / pipes a-d (satoru) ----
    PipeState ps[4] = {};
    static const char PN[4] = { 'A', 'B', 'C', 'D' };
    // transcoder a sits in pw_1. linux also lists pipe a under pw_a, but the
    // firmware-lit pipe a is the one we must see, so pipe a is read whenever
    // pw_1 is up (pw_1 is the dmc-managed always-on well). a pw_a-gated
    // register reads back 0 rather than faulting. (satoru)
    static const int TW[4] = { PW_1, PW_B, PW_C, PW_D };   // transcoder well (satoru)
    static const int PW[4] = { PW_1, PW_B, PW_C, PW_D };   // pipe + plane well (satoru)
    for (int i = 0; i < 4; i++) {
        ps[i].port = -1;
        bool tw = pw_on(TW[i]), pw = pw_on(PW[i]);
        if (i > 0 && !pw_on(PW_2)) { tw = false; pw = false; }  // pw_b..d hang off pw_2 (satoru)
        if (!tw && !pw) {
            Ln l; l.s("pipe "); l.c(PN[i]); l.s(": power well off, not read"); l.emit();
            continue;
        }
        uint32_t tb = TRANS_BASE + (uint32_t)i * TRANS_STRIDE;
        uint32_t pb = PIPE_BASE + (uint32_t)i * PIPE_STRIDE;
        uint32_t conf = pw ? rd(pb + P_TRANSCONF) : 0;
        uint32_t func = tw ? rd(tb + T_DDI_FUNC_CTL) : 0;
        uint32_t pctl = pw ? rd(pb + P_PLANE_CTL) : 0;
        uint32_t clks = rd(R_TRANS_CLK_SEL + (uint32_t)i * 4);
        bool c_en = conf >> 31, c_st = (conf >> 30) & 1;
        bool f_en = func >> 31, p_en = pctl >> 31;

        if (!c_en && !c_st && !f_en && !p_en) {
            Ln l; l.s("pipe "); l.c(PN[i]); l.s(": off (transconf "); l.x(conf);
            l.s(" ddi_func "); l.x(func); l.s(" plane_ctl "); l.x(pctl);
            l.s(" clk_sel "); l.x(clks); l.s(")");
            if (!tw) l.s(" [trans well off]");
            if (!pw) l.s(" [pipe well off]");
            l.emit();
            continue;
        }

        uint32_t ht = rd(tb + T_HTOTAL), hb = rd(tb + T_HBLANK), hs = rd(tb + T_HSYNC);
        uint32_t vt = rd(tb + T_VTOTAL), vb = rd(tb + T_VBLANK), vs = rd(tb + T_VSYNC);
        uint32_t src = rd(tb + T_PIPESRC);
        uint32_t dm = rd(tb + T_DATA_M1), dn = rd(tb + T_DATA_N1);
        uint32_t lm = rd(tb + T_LINK_M1), lnk = rd(tb + T_LINK_N1);

        ps[i].active = c_en && c_st;
        ps[i].hact = (ht & 0xFFFF) + 1; ps[i].htot = (ht >> 16) + 1;
        ps[i].vact = (vt & 0xFFFF) + 1; ps[i].vtot = (vt >> 16) + 1;
        ps[i].link_m = lm & 0xFFFFFF; ps[i].link_n = lnk & 0xFFFFFF;
        ps[i].mode = (func >> 24) & 7;
        if (f_en) ps[i].port = port_index_from_sel((func >> 27) & 0xF);

        {
            Ln l; l.s("pipe "); l.c(PN[i]); l.s(": transconf "); l.x(conf);
            l.s(" enable="); l.b01(c_en); l.s(" active="); l.b01(c_st);
            l.s(" interlace="); l.d((conf >> 21) & 3);
            l.s(" | src "); l.d((src >> 16) + 1); l.c('x'); l.d((src & 0xFFFF) + 1);
            l.emit();
        }
        {
            Ln l; l.s("  h active "); l.d(ps[i].hact); l.s(" blank "); l.d((hb & 0xFFFF) + 1);
            l.c('-'); l.d((hb >> 16) + 1); l.s(" sync "); l.d((hs & 0xFFFF) + 1); l.c('-');
            l.d((hs >> 16) + 1); l.s(" total "); l.d(ps[i].htot);
            l.s(" | raw "); l.x(ht); l.c(' '); l.x(hb); l.c(' '); l.x(hs);
            l.emit();
        }
        {
            Ln l; l.s("  v active "); l.d(ps[i].vact); l.s(" blank "); l.d((vb & 0xFFFF) + 1);
            l.c('-'); l.d((vb >> 16) + 1); l.s(" sync "); l.d((vs & 0xFFFF) + 1); l.c('-');
            l.d((vs >> 16) + 1); l.s(" total "); l.d(ps[i].vtot);
            l.s(" | raw "); l.x(vt); l.c(' '); l.x(vb); l.c(' '); l.x(vs);
            l.emit();
        }
        {
            Ln l; l.s("  ddi_func "); l.x(func); l.s(" en="); l.b01(f_en);
            l.s(" port="); l.s(port_from_sel((func >> 27) & 0xF));
            l.s(" mode="); l.s(mode_name(ps[i].mode));
            l.s(" bpc="); l.s(bpc_name((func >> 20) & 7));
            l.s(" lanes="); l.d(((func >> 1) & 7) + 1);
            l.s(" hsync+="); l.b01((func >> 16) & 1); l.s(" vsync+="); l.b01((func >> 17) & 1);
            l.s(" | clk_sel "); l.x(clks); l.s(" -> phy "); l.s(phy_from_clksel(clks >> 28));
            l.emit();
        }
        {
            Ln l; l.s("  m/n data "); l.x(dm); l.c('/'); l.x(dn); l.s(" link "); l.x(lm); l.c('/'); l.x(lnk);
            int pi = ps[i].port;
            if (pi >= 0 && valfreq[pi] && ps[i].link_n && ps[i].mode >= 2 && ps[i].mode <= 4) {
                // dotclock = link_m * link symbol clock / link_n
                // (intel_dotclock_calculate in intel_display.c) (satoru)
                uint64_t dot = (uint64_t)ps[i].link_m * valfreq[pi] / ps[i].link_n;
                l.s(" -> pixclk "); l.d(dot); l.s("khz");
                uint64_t frame = (uint64_t)ps[i].htot * ps[i].vtot;
                if (frame) {
                    uint64_t mhz = dot * 1000000ull / frame;  // millihertz (satoru)
                    if (ps[i].active && mhz <= 1000000ull) ps[i].mhz = (uint32_t)mhz;
                    l.s(" refresh "); l.d(mhz / 1000); l.c('.');
                    uint64_t fr = mhz % 1000;
                    l.c((char)('0' + fr / 100)); l.c((char)('0' + (fr / 10) % 10)); l.c((char)('0' + fr % 10));
                    l.s("hz");
                }
            }
            l.emit();
        }
        if (pw) {
            uint32_t st = rd(pb + P_PLANE_STRIDE), pos = rd(pb + P_PLANE_POS), sz = rd(pb + P_PLANE_SIZE);
            uint32_t surf = rd(pb + P_PLANE_SURF), off = rd(pb + P_PLANE_OFFSET);
            uint32_t live = rd(pb + P_PLANE_SURFLIVE);
            uint32_t tiling = (pctl >> 10) & 7;
            Ln l; l.s("  plane1 ctl "); l.x(pctl); l.s(" en="); l.b01(p_en);
            l.s(" fmt="); l.s(fmt_name((pctl >> 23) & 0x1F));
            l.s(" tiling="); l.s(tiling_name(tiling));
            l.s(" stride "); l.d(st & 0xFFF);
            if (tiling == 0) { l.s(" ("); l.d((uint64_t)(st & 0xFFF) * 64); l.s("B)"); }
            l.s(" size "); l.d((sz & 0xFFFF) + 1); l.c('x'); l.d((sz >> 16) + 1);
            l.emit();
            Ln m; m.s("  plane1 surf "); m.x(surf); m.s(" (ggtt) surflive "); m.x(live);
            m.s(" pos "); m.x(pos); m.s(" offset "); m.x(off);
            // pipedsl: sample the scanline counter to prove the pipe runs (satoru)
            uint32_t d0 = rd(pb + P_PIPEDSL) & 0xFFFFF, d1 = d0;
            for (int k = 0; k < 20000 && d1 == d0; k++) d1 = rd(pb + P_PIPEDSL) & 0xFFFFF;
            m.s(" | pipedsl "); m.d(d0); m.s("->"); m.d(d1); m.s(d1 != d0 ? " (scanning)" : " (frozen)");
            m.emit();
        }
    }

    // ---- summary (satoru) ----
    g_refresh_n = 0;
    for (int i = 0; i < 4; i++) {
        if (!ps[i].active) continue;
        g_refresh[g_refresh_n].hact = ps[i].hact;
        g_refresh[g_refresh_n].vact = ps[i].vact;
        g_refresh[g_refresh_n].mhz  = ps[i].mhz;
        g_refresh_n++;
    }
    int lit = 0;
    for (int i = 0; i < 4; i++) {
        if (!ps[i].active) continue;
        lit++;
        Ln l; l.s("summary: pipe "); l.c(PN[i]); l.s(" live ");
        l.d(ps[i].hact); l.c('x'); l.d(ps[i].vact); l.s(" (total "); l.d(ps[i].htot); l.c('x'); l.d(ps[i].vtot); l.s(")");
        int pi = ps[i].port;
        if (pi >= 0) {
            l.s(" -> port "); l.s(PORTS[pi].name); l.s(" "); l.s(mode_name(ps[i].mode));
            if (PORTS[pi].idx == 0 && ps[i].mode == 2) l.s("(edp if pps0 on)");
            uint32_t ck = clkctl[pi];
            uint32_t sel = (ck >> 12) & 0xF;
            l.s(" pll=");
            if (sel == 0x8) l.s(PORTS[pi].tc ? "c20 phy pll of " : "c10 phy pll of ");
            else if (sel >= 0xc) l.s("tbt pll via ");
            l.s(sel == 0x8 || sel >= 0xc ? PORTS[pi].name : clksel_name(sel));
            l.s(" ack="); l.b01((ck >> 30) & 1);
        } else {
            l.s(" -> no ddi port selected");
        }
        l.emit();
    }
    {
        Ln l; l.s("summary: "); l.d((uint64_t)lit); l.s(" pipe(s) lit by firmware; sink detect per port:");
        for (int i = 0; i < NPORTS; i++) {
            l.c(' '); l.s(PORTS[i].name); l.c('=');
            l.s(hpd_state[i] > 0 ? "yes" : (hpd_state[i] == 0 ? "no" : "unknown(detect off)"));
        }
        l.emit();
    }
    say("---- end of intel display probe ----");
    if (out && g_out_pos >= maxo - 2) {
        // the terminal buffer filled up; the serial log always has it all (satoru)
        SerialLogger::Log("[igpu] terminal output truncated, full report is in the serial log\r\n");
    }
    return g_out_pos;
}

// end (satoru)
