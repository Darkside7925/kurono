// i915 register model seam (d4.1 step i). the i915 family is compiled with
// KLD_MMIO_SIM, so every mmio access of the driver calls lx_mmio_read or
// lx_mmio_write here. without "model:<variant>" in argv both are plain
// accessors. with it, an access inside the BAR0 mapping is a model access:
// the write is logged and stored into the BAR page (so a broker bar policy
// still faults the container), the read returns what the page holds,
// possibly replaced by the register behaviour in i915_model_regs.c. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"
#include "drm/i915_model.h"

extern int lx_pci_bar_range(int bar, unsigned long *va, unsigned long *len);
extern long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                           unsigned long a2, unsigned long a3);
extern char **lx_main_argv;
extern int lx_main_argc;
#define LX_KLD_OP_GT_WRITE 0x0029u

static int g_gtbroker;   // argv gtbroker: gt writes go through the broker (satoru)

// register behaviour hooks (i915_model_regs.c). a read hook returns 1 when it
// replaced *val; a write hook may change the value stored. (satoru)
int i915_model_reg_read(unsigned long off, u64 *val, int size);
void i915_model_reg_write(unsigned long off, u64 *val, int size);
void i915_model_regs_init(const char *variant);

static int g_on;
static unsigned long g_bar0, g_bar0_len;
static unsigned long g_wr_disp, g_wr_other, g_wr_gsm, g_wr_dmc;

#define GSM_OFF 0x800000ul

int i915_model_on(void) { return g_on; }

// the touchpad family's claimed-range seam (lx_mmio_sim.c) is not linked into
// i915: this model sits behind the broker's real bar mapping, so no physical
// range is ever claimed and ioremap goes to the broker as for hardware. (satoru)
int lx_mmio_sim_claimed(unsigned long start, unsigned long len)
{
    (void)start; (void)len;
    return 0;
}

void i915_model_enable(const char *variant)
{
    g_on = 1;
    for (int i = 1; i < lx_main_argc; i++)
        if (lx_main_argv[i] && !strcmp(lx_main_argv[i], "gtbroker"))
            g_gtbroker = 1;
    i915_model_regs_init(variant);
    lx_log("[i915m] register model on, variant=");
    lx_log_line(variant);
}

// the display block of the manifest's bar_window (survey section 9, dmc
// range widened to 0x9ffff by the measured dmc load). (satoru)
static int in_display(unsigned long off)
{
    return (off >= 0x40000 && off <= 0x9ffff) ||
           (off >= 0xc0000 && off <= 0xcffff) ||
           (off >= 0x160000 && off <= 0x16ffff);
}

static int model_off(const volatile void *addr, unsigned long *off)
{
    unsigned long a = (unsigned long)addr;
    if (!g_on)
        return 0;
    if (!g_bar0 && lx_pci_bar_range(0, &g_bar0, &g_bar0_len) != 0)
        return 0;
    if (a < g_bar0 || a >= g_bar0 + g_bar0_len)
        return 0;
    *off = a - g_bar0;
    return 1;
}

static u64 raw_read(const volatile void *a, int size)
{
    switch (size) {
    case 1: return *(const volatile u8 *)a;
    case 2: return *(const volatile u16 *)a;
    case 4: return *(const volatile u32 *)a;
    default: return *(const volatile u64 *)a;
    }
}

static void raw_write(volatile void *a, u64 v, int size)
{
    switch (size) {
    case 1: *(volatile u8 *)a = (u8)v; break;
    case 2: *(volatile u16 *)a = (u16)v; break;
    case 4: *(volatile u32 *)a = (u32)v; break;
    default: *(volatile u64 *)a = v; break;
    }
}

// one write() per line so lines of concurrent threads never interleave.
// (satoru)
static char *put_hex(char *p, unsigned long v)
{
    static const char hx[] = "0123456789abcdef";
    int top = 60;
    while (top > 0 && !((v >> top) & 0xF))
        top -= 4;
    *p++ = '0'; *p++ = 'x';
    for (; top >= 0; top -= 4)
        *p++ = hx[(v >> top) & 0xF];
    return p;
}

static void log_write(unsigned long off, u64 val, unsigned long ret, int d)
{
    char b[96], *p = b;
    const char *h = "[i915m] w ";
    while (*h) *p++ = *h++;
    p = put_hex(p, off); *p++ = ' ';
    p = put_hex(p, (unsigned long)val); *p++ = ' ';
    p = put_hex(p, ret); *p++ = ' ';
    *p++ = d ? 'D' : 'X'; *p++ = '\n'; *p = 0;
    lx_log(b);
}

// the stored value of a bar0 register (the model's own view, no logging).
// (satoru)
u32 i915m_peek(unsigned long off)
{
    return g_bar0 ? *(const volatile u32 *)(g_bar0 + off) : 0;
}

// the model's own store of a reply register (no logging, not a driver
// write). (satoru)
void i915m_poke(unsigned long off, u32 v)
{
    if (g_bar0)
        *(volatile u32 *)(g_bar0 + off) = v;
}

u64 i915m_peek64(unsigned long off)
{
    return g_bar0 ? *(const volatile u64 *)(g_bar0 + off) : 0;
}

unsigned long i915m_bar0(void) { return g_bar0; }

// one lock for the model state: a driver write (hook + store) and the
// model's interrupt thread never interleave. (satoru)
static volatile int g_mlock;
void i915m_lock(void)   { while (__atomic_test_and_set(&g_mlock, __ATOMIC_ACQUIRE)) ; }
void i915m_unlock(void) { __atomic_clear(&g_mlock, __ATOMIC_RELEASE); }

u64 lx_mmio_read(const volatile void __iomem *addr, int size)
{
    unsigned long off;
    u64 v = raw_read(addr, size);
    if (model_off(addr, &off))
        i915_model_reg_read(off, &v, size);
    return v;
}

void lx_mmio_write(u64 val, volatile void __iomem *addr, int size)
{
    unsigned long off;
    if (model_off(addr, &off)) {
        i915m_lock();
        if (off >= GSM_OFF) {
            g_wr_gsm++;
        } else if ((off >= 0x80000 && off <= 0x9ffff) ||
                   (off >= 0x52000 && off <= 0x52fff) ||
                   (off >= 0x59000 && off <= 0x59fff) ||
                   (off >= 0x5f000 && off <= 0x5ffff)) {
            // dmc program ram as mtl_dmc.bin v2.23 places it (measured:
            // main 0x90000/0x98000, pipe 0x52000/0x59000). (satoru)
            g_wr_dmc++;   // dmc program ram: counted, not logged (satoru)
            i915_model_reg_write(off, &val, size);
        } else {
            int d = in_display(off);
            if (d) g_wr_disp++; else g_wr_other++;
            log_write(off, val, (unsigned long)__builtin_return_address(0), d);
            i915_model_reg_write(off, &val, size);
            if (!d && g_gtbroker) {
                // the gt page is mapped read-only in the container; the
                // write goes through the broker allowlist op. (satoru)
                i915m_unlock();
                lx_broker_call(LX_KLD_OP_GT_WRITE, off, (unsigned long)size, (unsigned long)val, 0);
                return;
            }
        }
        raw_write(addr, val, size);
        i915m_unlock();
        return;
    }
    raw_write(addr, val, size);
}

void i915_model_summary(const char *when)
{
    if (!g_on)
        return;
    lx_log("[i915m] summary ");
    lx_log(when);
    lx_log(": display writes=");
    lx_log_u(g_wr_disp);
    lx_log(" other writes=");
    lx_log_u(g_wr_other);
    lx_log(" ggtt pte writes=");
    lx_log_u(g_wr_gsm);
    lx_log(" dmc ram writes=");
    lx_log_u(g_wr_dmc);
    lx_log("\n");
}

// end (satoru)
