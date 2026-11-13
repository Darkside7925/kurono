// KLD model device: see kld_model.h. everything here is test fixture data for
// a rig without hardware; nothing runs unless kurono.kld.model= is on the
// command line. the vbt layout follows upstream intel_vbt_defs.h (6.12.99),
// survey in scratch/kld/gfx/survey_vbt.md. (satoru)

#include "kld_model.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../proc/spinlock.h"

namespace KldModel {

namespace {

bool g_on;
const char* g_variant = "edp";

constexpr uint32_t MODEL_BDF = (0xFEu << 16) | (0u << 8) | 0u;
constexpr uint16_t VENDOR = 0x8086, DEVICE = 0x7D67;
constexpr uint64_t BAR0_SIZE = 16ull << 20;    // gttmmadr: regs + gsm (satoru)
constexpr uint64_t BAR2_SIZE = 256ull << 20;   // lmembar: mtl needs exactly 256 MiB (satoru)
// fake bus addresses far above guest ram; only the container's ioremap
// arithmetic sees them, map_bar resolves pages through BarPagePhys. (satoru)
constexpr uint64_t BAR0_BASE = 0x4000000000ull;
constexpr uint64_t BAR2_BASE = 0x4010000000ull;
// ggc gms=0xf3 -> 16 MiB dsm at lmembar + 8 MiB: only that window of bar2 is
// backed. (satoru)
constexpr uint64_t DSM_OFF = 8ull << 20;
constexpr uint64_t DSM_SIZE = 16ull << 20;
constexpr uint64_t BAR0_PAGES = BAR0_SIZE / PAGE_SIZE;
constexpr uint64_t DSM_PAGES = DSM_SIZE / PAGE_SIZE;

uint8_t  g_cfg[256];
uint64_t g_bar0[BAR0_PAGES];
uint64_t g_dsm[DSM_PAGES];
uint64_t g_opregion;      // 2 contiguous frames, 3 with an rvda vbt (satoru)
// opregion form (review f1): 0 vbt in mailbox 4 (2.1), 21 = 2.1 with the vbt
// behind rvda/rvds relative to asls, 20 = 2.0 with an absolute rvda. (satoru)
int g_opr_form;
uint64_t g_opr_pages = 2;
Spinlock g_lock;

bool tok(const char* s, const char* t, const char** after)
{
    for (const char* p = s; p && *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        const char* a = p;
        const char* b = t;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) { if (after) *after = a; return true; }
    }
    return false;
}

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

void cfg_init()
{
    for (int i = 0; i < 256; i++) g_cfg[i] = 0;
    put16(g_cfg + 0x00, VENDOR);
    put16(g_cfg + 0x02, DEVICE);
    put16(g_cfg + 0x04, 0x0000);
    put16(g_cfg + 0x06, 0x0010);          // capability list: msi only (satoru)
    g_cfg[0x34] = 0x90;
    g_cfg[0x90] = 0x05;                   // msi, 64-bit address, 1 vector (satoru)
    g_cfg[0x91] = 0x00;
    put16(g_cfg + 0x92, 0x0080);
    put32(g_cfg + 0x08, 0x03000004u);     // vga class, revision 4 (mtl needs rev != 0) (satoru)
    put32(g_cfg + 0x10, (uint32_t)BAR0_BASE | 0x4u);          // 64-bit mem (satoru)
    put32(g_cfg + 0x14, (uint32_t)(BAR0_BASE >> 32));
    put32(g_cfg + 0x18, (uint32_t)BAR2_BASE | 0xCu);          // 64-bit prefetchable (satoru)
    put32(g_cfg + 0x1C, (uint32_t)(BAR2_BASE >> 32));
    put16(g_cfg + 0x2C, VENDOR);
    put16(g_cfg + 0x2E, 0x2212);
    put16(g_cfg + 0x50, 0x00C0);          // snb_gmch_ctrl ggms=3 (8 MiB gsm) (satoru)
    put32(g_cfg + 0xFC, (uint32_t)g_opregion);
}

// ── synthetic opregion + vbt (bdb 251) ──────────────────────────────────────
constexpr int CHILD_SIZE = 39;

void child(uint8_t* c, uint16_t handle, uint16_t type, uint8_t dvo, uint8_t aux,
           uint8_t typec)
{
    for (int i = 0; i < CHILD_SIZE; i++) c[i] = 0;
    put16(c + 0, handle);
    put16(c + 2, type);
    c[16] = dvo;
    c[23] = 0xC0;     // dp_max_lane_count 3 = 4 lanes (bdb >= 244 stores n-1) (satoru)
    c[24] = 0x02;
    c[25] = aux;
    c[33] = typec;
}

// appends one block (id, data) at *pos; returns the data pointer. (satoru)
uint8_t* block(uint8_t* vbt, uint32_t* pos, uint8_t id, uint16_t size)
{
    uint8_t* b = vbt + *pos;
    b[0] = id;
    put16(b + 1, size);
    for (uint16_t i = 0; i < size; i++) b[3 + i] = 0;
    *pos += 3u + size;
    return b + 3;
}

uint32_t build_vbt(uint8_t* vbt, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) vbt[i] = 0;
    const char* sig = "$VBT KURONO-MODEL   ";
    for (int i = 0; i < 20; i++) vbt[i] = (uint8_t)sig[i];
    put16(vbt + 20, 0x0100);
    put16(vbt + 22, 48);
    put32(vbt + 28, 48);
    const char* bsig = "BIOS_DATA_BLOCK ";
    for (int i = 0; i < 16; i++) vbt[48 + i] = (uint8_t)bsig[i];
    put16(vbt + 64, 251);
    put16(vbt + 66, 22);
    uint32_t pos = 70;

    bool edp = g_variant[0] == 'e';
    bool tc = g_variant[0] == 'e' || g_variant[0] == 't';

    uint8_t* gf = block(vbt, &pos, 1, 7);        // general features (satoru)
    gf[4] = 0x04;

    int nchild = (edp ? 1 : 0) + (tc ? 1 : 0);
    uint8_t* gd = block(vbt, &pos, 2, (uint16_t)(5 + nchild * CHILD_SIZE));
    gd[4] = CHILD_SIZE;
    int ci = 0;
    if (edp)   // edp on ddi a, aux a (satoru)
        child(gd + 5 + CHILD_SIZE * ci++, 0x0008, 0x78C6, 10, 0x40, 0x00);
    if (tc)    // dp alt mode on tc1 (dvo dpf), aux usbc1 (satoru)
        child(gd + 5 + CHILD_SIZE * ci++, 0x0004, 0x68C6, 13, 0x60, 0x01);

    if (edp) {
        uint8_t* e = block(vbt, &pos, 27, 848);  // bdb_edp (satoru)
        put16(e + 0, 2000); put16(e + 2, 50); put16(e + 4, 50);
        put16(e + 6, 500);  put16(e + 8, 500);
        put32(e + 160, 1);                        // panel 0: 24 bpp (satoru)
        e[164] = 0x32;
        uint8_t* lo = block(vbt, &pos, 40, 34);  // lfp options (satoru)
        put32(lo + 16, 1);
        uint8_t* bl = block(vbt, &pos, 43, 305); // lfp backlight: vesa edp aux (satoru)
        bl[0] = 6;
        bl[1] = 2;
        put16(bl + 2, 200);
        bl[113] = 5;
        put16(bl + 129, 255);
        bl[257] = 8;
        put16(bl + 273, 3000);
        uint8_t* dt = block(vbt, &pos, 58, 30);  // generic dtd 2560x1600 (satoru)
        put16(dt + 0, 28);
        uint8_t* d = dt + 2;
        put32(d + 0, 268500);
        put16(d + 4, 2560); put16(d + 6, 160); put16(d + 8, 48); put16(d + 10, 32);
        put16(d + 12, 1600); put16(d + 14, 46); put16(d + 16, 3); put16(d + 18, 6);
        put16(d + 20, 344); put16(d + 22, 215);
        d[24] = 0x80;
    }
    put16(vbt + 24, (uint16_t)pos);
    put16(vbt + 68, (uint16_t)(pos - 48));
    uint8_t sum = 0;
    for (uint32_t i = 0; i < pos; i++) sum = (uint8_t)(sum + vbt[i]);
    vbt[26] = (uint8_t)(0x100 - sum);
    return pos;
}

void build_opregion()
{
    uint8_t* o = (uint8_t*)PMM::PhysToVirt(g_opregion);
    for (int i = 0; i < 8192; i++) o[i] = 0;
    const char* sig = "IntelGraphicsMem";
    for (int i = 0; i < 16; i++) o[i] = (uint8_t)sig[i];
    put32(o + 0x10, 8);
    put32(o + 0x14, g_opr_form == 20 ? 0x02000000u : 0x02010000u);
    uint32_t n;
    if (g_opr_form) {
        // the vbt in the page after the region, mailbox 4 left empty so the
        // driver must take the rvda path; asle mailbox present (mboxes bit 2)
        // as the rvda rule requires. (satoru)
        uint8_t* vb = o + 8192;
        for (int i = 0; i < 4096; i++) vb[i] = 0;
        put32(o + 0x58, 0x4);
        n = build_vbt(vb, 4096);
        uint64_t rvda = g_opr_form == 21 ? 8192ull : g_opregion + 8192;
        for (int i = 0; i < 8; i++) o[0x3BA + i] = (uint8_t)(rvda >> (8 * i));
        put32(o + 0x3C2, n);
    } else {
        put32(o + 0x58, 0);         // no mailboxes (satoru)
        n = build_vbt(o + 0x400, 0x1C00);
    }
    SerialLogger::Log("[kld-model] opregion phys=0x");
    SerialLogger::LogHex64(g_opregion);
    SerialLogger::Log(" vbt bytes=");
    SerialLogger::LogDec((int)n);
    SerialLogger::Log(" variant=");
    SerialLogger::Log(g_variant);
    SerialLogger::Log(g_opr_form == 21 ? " vbt=rvda(2.1 relative)" : g_opr_form == 20 ? " vbt=rvda(2.0 absolute)" : " vbt=mailbox4");
    SerialLogger::Log("\r\n");
}

// first use from a container op: allocate and build the opregion, then
// point asls at it. (satoru)
// ready is published only after asls points at a built table, so a
// concurrent CfgRead either builds it or sees the finished value. (satoru)
bool g_tables_tried;
volatile bool g_tables_ready;
void ensure_tables()
{
    SpinLockGuard g(g_lock);
    if (g_tables_tried) return;
    g_tables_tried = true;
    g_opr_pages = g_opr_form ? 3 : 2;
    g_opregion = PMM::AllocContiguousBelow(g_opr_pages, 0xFFFFF000ull);   // asls is 32 bit (satoru)
    if (!g_opregion) {
        SerialLogger::Log("[kld-model] no frames for the opregion: asls stays 0\r\n");
        return;
    }
    build_opregion();
    put32(g_cfg + 0xFC, (uint32_t)g_opregion);
    __atomic_store_n(&g_tables_ready, true, __ATOMIC_RELEASE);
}

uint64_t zeroed_frame()
{
    uint64_t p = PMM::AllocContiguous(1);
    if (!p) return 0;
    uint64_t* v = (uint64_t*)PMM::PhysToVirt(p);
    for (int i = 0; i < 512; i++) v[i] = 0;
    return p;
}

}  // namespace

void BootInit(const char* cmdline)
{
    const char* after = nullptr;
    if (!cmdline || !tok(cmdline, "kurono.kld.model=i915", &after))
        return;
    if (after && *after == ':') {
        if (after[1] == 't') g_variant = "tc";
        else if (after[1] == 'n') g_variant = "none";
        else g_variant = "edp";
    }
    if (tok(cmdline, "kurono.kld.model.opregion=rvda21", nullptr)) g_opr_form = 21;
    else if (tok(cmdline, "kurono.kld.model.opregion=rvda20", nullptr)) g_opr_form = 20;
    // the opregion frames come later (ensure_tables): this runs before the
    // pmm hands out contiguous runs. (satoru)
    cfg_init();
    g_on = true;
    SerialLogger::Log("[kld-model] i915 model device 8086:7d67 at fe:00.0 (no hardware)\r\n");
}

bool Enabled() { return g_on; }
const char* Variant() { return g_variant; }
uint32_t Bdf() { return MODEL_BDF; }
bool IsModel(uint32_t bdf) { return g_on && bdf == MODEL_BDF; }

void Identity(uint16_t* vendor, uint16_t* device, uint16_t* subvendor,
              uint16_t* subdevice, uint32_t* class_code)
{
    *vendor = VENDOR;
    *device = DEVICE;
    *subvendor = (uint16_t)(g_cfg[0x2C] | (g_cfg[0x2D] << 8));
    *subdevice = (uint16_t)(g_cfg[0x2E] | (g_cfg[0x2F] << 8));
    *class_code = 0x030000u;
}

uint32_t CfgRead(uint32_t off, uint32_t width)
{
    if (!__atomic_load_n(&g_tables_ready, __ATOMIC_ACQUIRE)) ensure_tables();
    if (off > 0xFF || off + width > 0x100) return 0xFFFFFFFFu;
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++) v |= (uint32_t)g_cfg[off + i] << (8 * i);
    return v;
}

void CfgWrite(uint32_t off, uint32_t width, uint32_t val)
{
    if (off > 0xFF || off + width > 0x100) return;
    // read-only identity and bars (as the device has them); asls and the
    // swsci/asle range are refused for containers by the broker's
    // cfg_write_refused, the same rule as for a real device. (satoru)
    for (uint32_t i = 0; i < width; i++) {
        uint32_t o = off + i;
        if (o < 0x04 || (o >= 0x08 && o < 0x30) || o == 0x34 || o == 0x90 || o == 0x91) continue;
        g_cfg[o] = (uint8_t)(val >> (8 * i));
    }
}

uint32_t HostCfgRead(uint32_t off, uint32_t width)
{
    // a host bridge with mchbar already enabled by firmware (cfg 0x48 bit0),
    // which is what i915's intel_gmch_bar_setup expects on a booted machine.
    // (satoru)
    uint8_t h[256];
    for (int i = 0; i < 256; i++) h[i] = 0;
    put16(h + 0x00, 0x8086);
    put16(h + 0x02, 0x7D1A);
    put32(h + 0x08, 0x06000001u);
    put32(h + 0x48, 0xFEDC0001u);
    if (off > 0xFF || off + width > 0x100) return 0xFFFFFFFFu;
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++) v |= (uint32_t)h[off + i] << (8 * i);
    return v;
}

uint64_t BarSize(uint32_t idx)
{
    if (idx == 0) return BAR0_SIZE;
    if (idx == 2) return BAR2_SIZE;
    return 0;
}

uint64_t BarPagePhys(uint32_t idx, uint64_t page)
{
    SpinLockGuard g(g_lock);
    uint64_t* slot = nullptr;
    if (idx == 0 && page < BAR0_PAGES) slot = &g_bar0[page];
    else if (idx == 2 && page >= DSM_OFF / PAGE_SIZE &&
             page < (DSM_OFF + DSM_SIZE) / PAGE_SIZE)
        slot = &g_dsm[page - DSM_OFF / PAGE_SIZE];
    if (!slot) return 0;
    if (!*slot) *slot = zeroed_frame();
    return *slot;
}

// a fresh device for every container start (review f6): every backed bar
// page zeroed (no stale ggtt, dsm or register state from the previous run)
// and the config space back to reset values (asls kept). (satoru)
void ResetForContainer()
{
    if (!g_on) return;
    SpinLockGuard g(g_lock);
    uint64_t n = 0;
    for (uint64_t i = 0; i < BAR0_PAGES; i++)
        if (g_bar0[i]) { uint64_t* v = (uint64_t*)PMM::PhysToVirt(g_bar0[i]); for (int k = 0; k < 512; k++) v[k] = 0; n++; }
    for (uint64_t i = 0; i < DSM_PAGES; i++)
        if (g_dsm[i]) { uint64_t* v = (uint64_t*)PMM::PhysToVirt(g_dsm[i]); for (int k = 0; k < 512; k++) v[k] = 0; n++; }
    uint32_t asls = (uint32_t)(g_cfg[0xFC] | (g_cfg[0xFD] << 8) | (g_cfg[0xFE] << 16) | ((uint32_t)g_cfg[0xFF] << 24));
    cfg_init();
    put32(g_cfg + 0xFC, asls);
    SerialLogger::Log("[kld-model] device reset for a new container, pages zeroed=");
    SerialLogger::LogDec((int)n);
    SerialLogger::Log("\r\n");
}

bool OwnsOpRegion(uint64_t phys, uint64_t bytes)
{
    uint64_t end = phys + bytes;
    return g_on && g_opregion && bytes && end > phys && phys >= g_opregion &&
           end <= g_opregion + g_opr_pages * PAGE_SIZE;
}

}  // namespace KldModel

// end (satoru)
