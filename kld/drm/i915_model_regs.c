// i915 register model: register behaviour, added one group at a time (d4.1).
// group 2: gt bring-up (forcewake ack = request, masked registers, engine
// reset ready, gdrst and pcode complete at once, mcr semaphore free).
// group 3: display power (well state = any requester's request, aux power,
// dbuf slice state, pmdemand done, de pll lock, iir empty).
// group 1: registers read their table value (i915_model_tables.c) until the
// driver writes them; a never-written register outside the table reads 0 and
// its first read is logged once ("[i915m] r0 <off>"), so the probe log lists
// every register the model had to answer without knowing it. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>

#include "kimpl/lx_kshim.h"
#include "drm/i915_model_tables.h"

u32 i915m_peek(unsigned long off);
void i915_model_aux_init(const char *variant);
u32 i915_model_aux_xfer(unsigned long ctl, u32 v);
void i915_model_phy_init(const char *variant);
int i915_model_phy_read(unsigned long off, u32 *v);
int i915_model_phy_write(unsigned long off, u32 *v);

#define REGS_END 0x400000ul       // register half of bar0 (satoru)
#define R0_LOG_MAX 3000

static u8 *g_written;             // one bit per dword of the register half (satoru)
static u8 *g_read0;
static unsigned int g_r0_logged;

static int bit_get(const u8 *m, unsigned long off) { return m && (m[off >> 5] >> ((off >> 2) & 7)) & 1; }
static void bit_set(u8 *m, unsigned long off) { if (m) m[off >> 5] |= (u8)(1u << ((off >> 2) & 7)); }

void i915_model_regs_init(const char *variant)
{
    i915_model_aux_init(variant);
    i915_model_phy_init(variant);
    g_written = lx_os_malloc(REGS_END / 32);
    g_read0 = lx_os_malloc(REGS_END / 32);
    if (g_written) memset(g_written, 0, REGS_END / 32);
    if (g_read0) memset(g_read0, 0, REGS_END / 32);
}

static int init_value(unsigned long off, u32 *v)
{
    for (unsigned int i = 0; i < i915m_init_count; i++)
        if (i915m_init_table[i].off == off) {
            *v = i915m_init_table[i].val;
            return 1;
        }
    return 0;
}

// ── group 2: gt bring-up (survey section 1, 2.4) ──────────────────────────
static const unsigned long fw_req[] = {
    0xa188, 0xa278, 0x38a188, 0x38a540, 0x38a548, 0x38a560, 0x38a618 };
static const unsigned long fw_ack[] = {
    0x0dfc, 0x0d84, 0x380dfc, 0x380d50, 0x380d58, 0x380d70, 0x380df8 };
static const unsigned long ring_reset[] = {
    0x20d0, 0x220d0, 0x1a0d0, 0x1c00d0, 0x1d00d0, 0x1c80d0, 0x11a0d0 };
#define N(a) (sizeof(a) / sizeof((a)[0]))

static int in_list(const unsigned long *l, unsigned int n, unsigned long off)
{
    for (unsigned int i = 0; i < n; i++)
        if (l[i] == off)
            return (int)i + 1;
    return 0;
}

// masked register: bits 31:16 select which of 15:0 the write changes.
static u32 masked(u32 old, u32 v)
{
    u32 m = v >> 16;
    return (old & ~m) | (v & m & 0xffff);
}

static int gt_read(unsigned long off, u32 *v)
{
    int i;
    if ((i = in_list(fw_ack, N(fw_ack), off))) {
        *v = i915m_peek(fw_req[i - 1]) & 0x8001;   // ack mirrors req bits 0, 15
        return 1;
    }
    if (in_list(ring_reset, N(ring_reset), off)) {
        u32 s = i915m_peek(off);
        *v = (s & ~2u) | ((s & 1u) << 1);           // ready_to_reset = request
        return 1;
    }
    if (off == 0x941c || off == 0x38941c) { *v = 0; return 1; }       // gdrst self-clears
    if (off == 0xfd0 || off == 0x380fd0) { *v = 1; return 1; }        // steer semaphore free
    if (off == 0x138124) { *v = i915m_peek(off) & ~0x800000ffu; return 1; }  // pcode done, ok
    // guc_status: the guc never runs in the model, its mia core stays in reset
    // (gs_mia_in_reset), which is what i915 checks after resetting it.
    if (off == 0xc000 || off == 0x38c000) { *v = i915m_peek(off) | 0x1; return 1; }
    return 0;
}

static int disp_power_write(unsigned long off, u32 *v);

static int gt_write(unsigned long off, u32 *v)
{
    if (disp_power_write(off, v))
        return 1;
    if (in_list(fw_req, N(fw_req), off) || in_list(ring_reset, N(ring_reset), off)) {
        *v = masked(i915m_peek(off), *v);
        return 1;
    }
    return 0;
}

// ── group 3: display power (survey section 3) ─────────────────────────────
static const unsigned long aux_ctl[] = {
    0x64010, 0x64110, 0x16f210, 0x16f410, 0x16f610, 0x16f810 };
static const unsigned long dbuf_ctl[] = { 0x45008, 0x44fe8, 0x44300, 0x44304 };
static const unsigned long iir[] = {
    0x44408, 0x44418, 0x44428, 0x44438, 0x44448, 0x44468, 0x444e8, 0x444f8,
    0x16fe58, 0xc4008, 0x190018, 0x19001c,
    0x60818, 0x61818, 0x62818, 0x63818 };

static int disp_power_read(unsigned long off, u32 *v)
{
    if (off >= 0x45400 && off <= 0x4540c && !(off & 3)) {
        u32 req = i915m_peek(0x45400) | i915m_peek(0x45404) |
                  i915m_peek(0x45408) | i915m_peek(0x4540c);
        *v = i915m_peek(off) | ((req & 0xaaaaaaaau) >> 1);   // state = any requester
        return 1;
    }
    if (in_list(aux_ctl, N(aux_ctl), off)) {
        u32 s = i915m_peek(off);
        *v = (s & ~(1u << 18)) | ((s >> 1) & (1u << 18));   // power status = request
        return 1;
    }
    if (in_list(dbuf_ctl, N(dbuf_ctl), off)) {
        u32 s = i915m_peek(off);
        *v = (s & ~(1u << 30)) | ((s >> 1) & (1u << 30));   // slice state = request
        return 1;
    }
    if (off == 0x45234) { *v = i915m_peek(off) & ~(1u << 31); return 1; }   // pmdemand done
    if (off == 0x46070) {
        u32 s = i915m_peek(off);
        *v = (s & ~(1u << 30)) | ((s >> 1) & (1u << 30));   // de pll lock = enable
        return 1;
    }
    // iir: pending bits as stored (write-1-to-clear in disp_power_write).
    if (off == 0x44200) {                              // display int ctl
        u32 s = i915m_peek(off) & (1u << 31);
        for (int p = 0; p < 4; p++)
            if (i915m_peek(0x44408 + 0x10 * p)) s |= 1u << (16 + p);
        *v = s;
        return 1;
    }
    if (off == 0x190010) {                             // gfx master: display bit
        *v = (i915m_peek(0x44408) | i915m_peek(0x44418) | i915m_peek(0x44428) |
              i915m_peek(0x44438)) ? (1u << 16) : 0;
        return 1;
    }
    if (off == 0x190008) {                             // dg1 master tile: tile 0 pending
        u32 any = i915m_peek(0x44408) | i915m_peek(0x44418) | i915m_peek(0x44428) |
                  i915m_peek(0x44438);
        *v = (i915m_peek(off) & (1u << 31)) | (any ? 1u : 0);
        return 1;
    }
    return 0;
}

// write-1-to-clear iir; master registers keep only their enable bit, the
// pending bits are derived from the iirs. (satoru)
static int disp_power_write(unsigned long off, u32 *v)
{
    if (in_list(iir, N(iir), off)) {
        *v = i915m_peek(off) & ~*v;
        return 1;
    }
    if (off == 0x44200 || off == 0x190008) {
        *v &= 1u << 31;
        return 1;
    }
    if (off == 0x190010) {
        *v = 0;
        return 1;
    }
    return 0;
}

int i915_model_reg_read(unsigned long off, u64 *val, int size)
{
    u32 v;
    if (off >= REGS_END || (off & 3))
        return 0;
    if (size == 4 && (gt_read(off, &v) || disp_power_read(off, &v) ||
                      i915_model_phy_read(off, &v))) {
        *val = v;
        return 1;
    }
    if (!bit_get(g_written, off)) {
        if (init_value(off, &v)) {
            *val = (size == 8) ? (u64)v : v;
            return 1;
        }
        if (!bit_get(g_read0, off) && g_r0_logged < R0_LOG_MAX) {
            bit_set(g_read0, off);
            g_r0_logged++;
            lx_log("[i915m] r0 ");
            lx_log_hex(off);
            lx_log("\n");
        }
    }
    return 0;
}

void i915_model_reg_write(unsigned long off, u64 *val, int size)
{
    u32 v;
    if (off >= REGS_END)
        return;
    if (size == 4) {
        v = (u32)*val;
        if (gt_write(off, &v))
            *val = v;
        else if (in_list(aux_ctl, N(aux_ctl), off) && (v & (1u << 31)))
            *val = i915_model_aux_xfer(off, v);   // group 4 (i915_model_aux.c)
        else if (i915_model_phy_write(off, &v))
            *val = v;                             // group 5 (i915_model_phy.c)
    }
    bit_set(g_written, off & ~3ul);
    if (size == 8)
        bit_set(g_written, (off & ~3ul) + 4);
}

// end (satoru)
