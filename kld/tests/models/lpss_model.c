// lpss_model: behavioural model of the LPSS private space at BAR+0x200.
//
// the register names and offsets are the driver's own LPSS_PRIV_* definitions
// in drivers/mfd/intel-lpss.c. caps type=I2C (0), NO_IDMA set, so the probe
// creates the i2c_designware cell only. the lower BAR (DesignWare) is left at
// zero on purpose in this step and counted. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>

#include "kimpl/lx_kshim.h"
#include "lpss_model.h"

#define LPSS_BAR_SIZE 0x1000
#define LPSS_PRIV 0x200
#define LPSS_CAPS (LPSS_PRIV + 0xfc)

static u8 g_regs[LPSS_BAR_SIZE];
static unsigned long g_reads, g_writes;

static u64 model_read(void *ctx, unsigned long off, int size)
{
    (void)ctx;
    g_reads++;
    u64 v = 0;
    if (off + (unsigned long)size <= LPSS_BAR_SIZE) {
        for (int i = 0; i < size; i++)
            v |= (u64)g_regs[off + i] << (8 * i);
    }
    return v;
}

static void model_write(void *ctx, unsigned long off, int size, u64 val)
{
    (void)ctx;
    g_writes++;
    if (off + (unsigned long)size > LPSS_BAR_SIZE)
        return;
    for (int i = 0; i < size; i++)
        g_regs[off + i] = (u8)(val >> (8 * i));
}

void lpss_model_init(void *bar, unsigned long len)
{
    (void)len;
    for (unsigned long i = 0; i < LPSS_BAR_SIZE; i++)
        g_regs[i] = 0;
    // caps read from THIS laptop's debugfs (sudo -n cat
    // /sys/kernel/debug/intel_lpss/0000:80:15.1/capabilities) = 0x00000201:
    // type bits 7:4 = 0 (I2C), BIT(9) and BIT(0) set, NO_IDMA clear, so the
    // upstream probe creates the idma64 cell in addition to i2c_designware. (satoru)
    g_regs[LPSS_CAPS + 0] = 0x01;
    g_regs[LPSS_CAPS + 1] = 0x02;
    g_reads = g_writes = 0;
    lx_mmio_sim_claim((char *)bar + LPSS_PRIV, LPSS_BAR_SIZE - LPSS_PRIV, model_read, model_write, NULL);
    lx_log("[lpss_model] claimed priv 0x200..0x2ff, caps=0x201 (I2C, idma, host debugfs)\n");
}

unsigned long lpss_model_reads(void) { return g_reads; }
unsigned long lpss_model_writes(void) { return g_writes; }

// end (satoru)
