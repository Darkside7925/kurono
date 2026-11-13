// dw_i2c_model: the DesignWare I2C master at the mfd cell's memory range.
//
// register offsets and bit names follow drivers/i2c/busses/i2c-designware-core.h.
// the FIFO engine runs synchronously on the DATA_CMD write with STOP: no
// sleeps, no model thread. a bus callback supplies the byte transaction. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>

#include "kimpl/lx_kshim.h"
#include "dw_i2c_model.h"
#include "../elan06fa_model.h"

#define DW_IC_CON            0x00
#define DW_IC_TAR            0x04
#define DW_IC_DATA_CMD       0x10
#define DW_IC_SS_SCL_HCNT    0x14
#define DW_IC_SS_SCL_LCNT    0x18
#define DW_IC_FS_SCL_HCNT    0x1c
#define DW_IC_FS_SCL_LCNT    0x20
#define DW_IC_HS_SCL_HCNT    0x24
#define DW_IC_HS_SCL_LCNT    0x28
#define DW_IC_INTR_STAT      0x2c
#define DW_IC_INTR_MASK      0x30
#define DW_IC_RAW_INTR_STAT  0x34
#define DW_IC_RX_TL          0x38
#define DW_IC_TX_TL          0x3c
#define DW_IC_CLR_INTR       0x40
#define DW_IC_CLR_RX_UNDER   0x44
#define DW_IC_CLR_RX_OVER    0x48
#define DW_IC_CLR_TX_OVER    0x4c
#define DW_IC_CLR_RD_REQ     0x50
#define DW_IC_CLR_TX_ABRT    0x54
#define DW_IC_CLR_RX_DONE    0x58
#define DW_IC_CLR_ACTIVITY   0x5c
#define DW_IC_CLR_STOP_DET   0x60
#define DW_IC_CLR_START_DET  0x64
#define DW_IC_CLR_GEN_CALL   0x68
#define DW_IC_ENABLE         0x6c
#define DW_IC_STATUS         0x70
#define DW_IC_TXFLR          0x74
#define DW_IC_RXFLR          0x78
#define DW_IC_SDA_HOLD       0x7c
#define DW_IC_TX_ABRT_SOURCE 0x80
#define DW_IC_ENABLE_STATUS  0x9c
#define DW_IC_COMP_PARAM_1   0xf4
#define DW_IC_COMP_VERSION   0xf8
#define DW_IC_COMP_TYPE      0xfc

#define DW_IC_COMP_TYPE_VALUE 0x44570140u
#define DW_IC_STATUS_TFE      (1u << 2)
#define DW_IC_STATUS_RFNE     (1u << 3)
#define DW_IC_STATUS_ACTIVITY (1u << 0)
#define DW_IC_STATUS_MASTER_ACTIVITY (1u << 5)
#define DW_IC_INTR_RX_OVER    (1u << 1)
#define DW_IC_INTR_RX_FULL    (1u << 2)
#define DW_IC_INTR_TX_OVER    (1u << 3)
#define DW_IC_INTR_TX_EMPTY   (1u << 4)
#define DW_IC_INTR_TX_ABRT    (1u << 6)
#define DW_IC_INTR_ACTIVITY   (1u << 8)
#define DW_IC_INTR_STOP_DET   (1u << 9)
#define DW_IC_INTR_START_DET  (1u << 10)
#define DW_IC_ABRT_7B_ADDR_NOACK 0x1u

#define FIFO 64
#define RXMAX 64

static u32 g_reg[0x100];
static u8 g_tx[FIFO];
static int g_tx_len;
static u8 g_rx[RXMAX];
static int g_rx_len, g_rx_off;
static u8 g_w[RXMAX];
static int g_wn, g_rn;
static int g_addr;
static u32 g_raw;
static int g_irq = -1;
static dw_bus_xfer_t g_bus;
static void *g_bus_ctx;
static unsigned long g_irqs, g_aborts, g_tx_max, g_rx_max;
static unsigned long g_tx_empty_irqs, g_rx_full_irqs;
static int g_withhold_stop;
// one read burst per register write: the write bytes are the register address,
// the read bytes come from the ELAN model through the bus callback. (satoru)
static u8 g_rd[2048];
static int g_rd_len, g_rd_pos, g_rd_filled, g_had_read;

static u32 rd(unsigned long off)
{
    u32 v = (off / 4 < 0x100) ? g_reg[off / 4] : 0;
    switch (off) {
    case DW_IC_COMP_TYPE: return DW_IC_COMP_TYPE_VALUE;
    case DW_IC_COMP_VERSION: return 0x3132322au;
    case DW_IC_COMP_PARAM_1: return 0x000fff00u;
    case DW_IC_ENABLE_STATUS: return g_reg[DW_IC_ENABLE / 4] & 1;
    case DW_IC_STATUS:
        return DW_IC_STATUS_TFE |
               (g_rx_off < g_rx_len ? DW_IC_STATUS_RFNE : 0);
    case DW_IC_TXFLR: return 0;
    case DW_IC_RXFLR: return (u32)(g_rx_len - g_rx_off);
    case DW_IC_RAW_INTR_STAT: return g_raw;
    case DW_IC_INTR_STAT: return g_raw & g_reg[DW_IC_INTR_MASK / 4];
    case DW_IC_TX_ABRT_SOURCE: return v;
    default: return v;
    }
}

static void raise_irq(void)
{
    int irq = g_irq;

    // the controller's line is the PCI function's vector, allocated by the
    // lpss-pci probe before the adapter exists; follow it until the test sets
    // an explicit one. (satoru)
    if (irq < 0)
        irq = lx_pci_sim_irq();
    if (irq < 0)
        return;
    u32 stat = g_raw & g_reg[DW_IC_INTR_MASK / 4];
    if (stat) {
        g_irqs++;
        lx_irq_raise_soft((unsigned int)irq);
    }
}

static void clear_bits(u32 mask)
{
    g_raw &= ~mask;
}

static int execute(void)
{
    int i;
    if (g_addr != 0x15) {
        g_aborts++;
        g_reg[DW_IC_TX_ABRT_SOURCE / 4] = DW_IC_ABRT_7B_ADDR_NOACK;
        g_tx_len = 0;
        g_raw |= DW_IC_INTR_TX_ABRT;
        raise_irq();
        return -1;
    }
    if (!g_bus)
        return -1;
    // a read transfer already handed the write bytes and the read buffer to
    // the device model; a write transfer is delivered now, at STOP. (satoru)
    int rc = 0;
    if (!g_had_read)
        rc = g_bus(g_bus_ctx, (u8)g_addr, g_tx, g_tx_len, NULL, 0);
    if (rc < 0) {
        g_aborts++;
        g_reg[DW_IC_TX_ABRT_SOURCE / 4] = DW_IC_ABRT_7B_ADDR_NOACK;
        g_tx_len = 0;
        g_raw |= DW_IC_INTR_TX_ABRT;
        raise_irq();
        return -1;
    }
    g_rx_len = 0;
    g_rx_off = 0;
    if (g_rx_len > g_rx_max)
        g_rx_max = (unsigned long)g_rx_len;
    g_tx_len = 0;
    g_rd_filled = 0;
    g_rd_pos = g_rd_len = 0;
    g_had_read = 0;
    g_raw |= DW_IC_INTR_TX_EMPTY;
    if (!g_withhold_stop)
        g_raw |= DW_IC_INTR_STOP_DET;
    raise_irq();
    return 0;
}

static void data_cmd(u32 val)
{
    if (!(g_reg[DW_IC_ENABLE / 4] & 1))
        return;
    if (g_addr != 0x15) {
        g_aborts++;
        g_reg[DW_IC_TX_ABRT_SOURCE / 4] = DW_IC_ABRT_7B_ADDR_NOACK;
        g_tx_len = 0;
        g_raw |= DW_IC_INTR_TX_ABRT;
        raise_irq();
        return;
    }
    g_raw |= DW_IC_INTR_ACTIVITY;
    if (val & 0x100) {
        // the first read command of the burst is where the bus transfers the
        // write bytes (the register address) and the device fills the read
        // stream; later read commands only consume it. (satoru)
        u8 b = 0;

        g_had_read = 1;
        if (!g_rd_filled) {
            g_rd_filled = 1;
            g_rd_len = 0;
            g_rd_pos = 0;
            if (g_bus) {
                int rc = g_bus(g_bus_ctx, (u8)g_addr, g_tx, g_tx_len,
                               g_rd, sizeof(g_rd));

                if (rc >= 0)
                    g_rd_len = (int)sizeof(g_rd);
            }
        }
        if (g_rd_pos < g_rd_len)
            b = g_rd[g_rd_pos++];
        if (g_rx_off == g_rx_len) {
            g_rx_len = 0;
            g_rx_off = 0;
        }
        if (g_rx_len - g_rx_off < RXMAX)
            g_rx[g_rx_len++] = b;
        else
            g_raw |= DW_IC_INTR_RX_OVER;
        if (g_rx_len - g_rx_off > (int)g_rx_max)
            g_rx_max = (unsigned long)(g_rx_len - g_rx_off);
        g_raw |= DW_IC_INTR_RX_FULL;
        if ((val & 0x200) && !g_withhold_stop)
            g_raw |= DW_IC_INTR_STOP_DET;
        raise_irq();
        return;
    }
    if (g_tx_len < FIFO)
        g_tx[g_tx_len++] = (u8)(val & 0xff);
    else
        g_raw |= DW_IC_INTR_TX_OVER;
    if (g_tx_len > (int)g_tx_max)
        g_tx_max = (unsigned long)g_tx_len;
    if (g_tx_len < FIFO) {
        g_reg[DW_IC_TXFLR / 4] = (u32)g_tx_len;
    }
    g_raw |= DW_IC_INTR_TX_EMPTY;
    if (val & 0x200)
        (void)execute();
    else
        raise_irq();
}

static void wr(unsigned long off, u32 val)
{
    if (off / 4 >= 0x100)
        return;
    switch (off) {
    case DW_IC_DATA_CMD:
        data_cmd(val);
        return;
    case DW_IC_INTR_MASK:
        g_reg[DW_IC_INTR_MASK / 4] = val;
        if (val & DW_IC_INTR_TX_EMPTY) {
            g_raw |= DW_IC_INTR_TX_EMPTY;
            raise_irq();
        }
        return;
    case DW_IC_ENABLE:
        g_reg[DW_IC_ENABLE / 4] = val;
        g_reg[DW_IC_ENABLE_STATUS / 4] = val & 1;
        if (val & 1) {
            g_raw |= DW_IC_INTR_TX_EMPTY;
            raise_irq();
        }
        if (!(val & 1)) {
            g_tx_len = 0;
            g_rx_len = g_rx_off = 0;
            clear_bits(DW_IC_INTR_STOP_DET | DW_IC_INTR_RX_FULL | DW_IC_INTR_TX_EMPTY);
        }
        return;
    case DW_IC_TAR:
        g_reg[DW_IC_TAR / 4] = val;
        g_addr = (int)(val & 0x3ff);
        g_tx_len = 0;
        g_rd_filled = 0;
        g_rd_pos = g_rd_len = 0;
        g_had_read = 0;
        return;
    default:
        g_reg[off / 4] = val;
        return;
    }
}

static u64 model_read(void *ctx, unsigned long off, int size)
{
    (void)ctx; (void)size;
    if (off == DW_IC_CLR_INTR) { u32 v = g_raw; g_raw = 0; return v; }
    if (off == DW_IC_CLR_STOP_DET) { clear_bits(DW_IC_INTR_STOP_DET); return 0; }
    if (off == DW_IC_CLR_START_DET) { clear_bits(DW_IC_INTR_START_DET); return 0; }
    if (off == DW_IC_CLR_TX_ABRT) { clear_bits(DW_IC_INTR_TX_ABRT); return 0; }
    if (off == DW_IC_CLR_ACTIVITY) { clear_bits(DW_IC_INTR_ACTIVITY); return 0; }
    if (off == DW_IC_CLR_RX_UNDER) { return 0; }
    if (off == DW_IC_CLR_RX_OVER) { clear_bits(DW_IC_INTR_RX_OVER); return 0; }
    if (off == DW_IC_CLR_TX_OVER) { clear_bits(DW_IC_INTR_TX_OVER); return 0; }
    if (off == DW_IC_DATA_CMD) {
        if (g_rx_off < g_rx_len) {
            u8 b = g_rx[g_rx_off++];
            if (g_rx_off < g_rx_len) {
                g_raw |= DW_IC_INTR_RX_FULL;
                raise_irq();
            } else {
                g_raw &= ~DW_IC_INTR_RX_FULL;
            }
            return b;
        }
        return 0;
    }
    u32 v = rd(off);
    if (off == DW_IC_RAW_INTR_STAT || off == DW_IC_INTR_STAT) {
        if (v & DW_IC_INTR_TX_EMPTY) g_tx_empty_irqs++;
        if (v & DW_IC_INTR_RX_FULL) g_rx_full_irqs++;
        raise_irq();
    }
    return v;
}

static void model_write(void *ctx, unsigned long off, int size, u64 val)
{
    (void)ctx; (void)size;
    wr(off, (u32)val);
}

void dw_i2c_model_init(void *base, unsigned long len)
{
    for (unsigned i = 0; i < 0x100; i++)
        g_reg[i] = 0;
    g_reg[DW_IC_COMP_TYPE / 4] = DW_IC_COMP_TYPE_VALUE;
    g_reg[DW_IC_COMP_VERSION / 4] = 0x3132322au;
    g_reg[DW_IC_COMP_PARAM_1 / 4] = 0x003f3f00u;
    g_reg[DW_IC_INTR_MASK / 4] = 0;
    g_tx_len = g_rx_len = g_rx_off = 0;
    g_wn = g_rn = 0;
    g_raw = 0;
    g_irqs = g_aborts = g_tx_max = g_rx_max = 0;
    g_tx_empty_irqs = g_rx_full_irqs = 0;
    lx_mmio_sim_claim(base, 0x200, model_read, model_write, NULL);
    lx_log("[dw_i2c_model] claimed low BAR, comp_type=0x44570140, fifo=64/64 (assumed, LPSS typical)\n");
}

void dw_i2c_model_set_bus(dw_bus_xfer_t cb, void *ctx) { g_bus = cb; g_bus_ctx = ctx; }
void dw_i2c_model_set_irq(int irq) { g_irq = irq; }
unsigned long dw_i2c_irqs(void) { return g_irqs; }
unsigned long dw_i2c_aborts(void) { return g_aborts; }
unsigned long dw_i2c_tx_max(void) { return g_tx_max; }
unsigned long dw_i2c_rx_max(void) { return g_rx_max; }
unsigned long dw_i2c_tx_empty_irqs(void) { return g_tx_empty_irqs; }
unsigned long dw_i2c_rx_full_irqs(void) { return g_rx_full_irqs; }
void dw_i2c_withhold_stop(int on) { g_withhold_stop = on; }
// end (satoru)
