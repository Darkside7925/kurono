// lx_mmio_sim: claimed MMIO ranges for the simulated touchpad build.
//
// the model owns a zeroed BAR; the accessors route only addresses inside a
// claimed range to it. everything else falls back to the raw volatile access,
// so a forgotten register cannot silently disappear. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/io.h>

#include "lx_kshim.h"

struct lx_mmio_range {
    unsigned long start;
    unsigned long len;
    u64 (*read)(void *ctx, unsigned long off, int size);
    void (*write)(void *ctx, unsigned long off, int size, u64 val);
    void *ctx;
};

static struct lx_mmio_range g_ranges[8];
static int g_nranges;

void lx_mmio_sim_reset(void)
{
    for (int i = 0; i < 8; i++)
        g_ranges[i] = (struct lx_mmio_range){0};
    g_nranges = 0;
}

int lx_mmio_sim_claim(void *base, unsigned long len,
                      u64 (*read)(void *, unsigned long, int),
                      void (*write)(void *, unsigned long, int, u64),
                      void *ctx)
{
    if (!base || !len || g_nranges >= 8)
        return -ENOSPC;
    g_ranges[g_nranges].start = (unsigned long)base;
    g_ranges[g_nranges].len = len;
    g_ranges[g_nranges].read = read;
    g_ranges[g_nranges].write = write;
    g_ranges[g_nranges].ctx = ctx;
    g_nranges++;
    return 0;
}

int lx_mmio_sim_claimed(unsigned long start, unsigned long len)
{
    for (int i = 0; i < g_nranges; i++)
        if (start >= g_ranges[i].start &&
            start + len <= g_ranges[i].start + g_ranges[i].len)
            return 1;
    return 0;
}

static struct lx_mmio_range *lx_mmio_find(const volatile void __iomem *addr)
{
    unsigned long a = (unsigned long)addr;
    for (int i = 0; i < g_nranges; i++) {
        struct lx_mmio_range *r = &g_ranges[i];
        if (a >= r->start && a < r->start + r->len)
            return r;
    }
    return NULL;
}

u64 lx_mmio_read(const volatile void __iomem *addr, int size)
{
    struct lx_mmio_range *r = lx_mmio_find(addr);
    if (r && r->read)
        return r->read(r->ctx, (unsigned long)addr - r->start, size);
    switch (size) {
    case 1: return *(const volatile u8 __force *)addr;
    case 2: return *(const volatile u16 __force *)addr;
    case 4: return *(const volatile u32 __force *)addr;
    case 8: return *(const volatile u64 __force *)addr;
    default: return 0;
    }
}

void lx_mmio_write(u64 val, volatile void __iomem *addr, int size)
{
    struct lx_mmio_range *r = lx_mmio_find(addr);
    if (r && r->write) {
        r->write(r->ctx, (unsigned long)addr - r->start, size, val);
        return;
    }
    switch (size) {
    case 1: *(volatile u8 __force *)addr = (u8)val; break;
    case 2: *(volatile u16 __force *)addr = (u16)val; break;
    case 4: *(volatile u32 __force *)addr = (u32)val; break;
    case 8: *(volatile u64 __force *)addr = val; break;
    default: break;
    }
}

// end (satoru)
