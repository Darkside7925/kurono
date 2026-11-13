// dw_i2c_model: behavioural Synopsys DesignWare I2C master.
// end (satoru)
#ifndef KLD_DW_I2C_MODEL_H
#define KLD_DW_I2C_MODEL_H
#include <linux/types.h>

typedef int (*dw_bus_xfer_t)(void *ctx, u8 addr, const u8 *w, int wn,
                             u8 *r, int rn);

void dw_i2c_model_init(void *base, unsigned long len);
void dw_i2c_model_set_bus(dw_bus_xfer_t cb, void *ctx);
void dw_i2c_model_set_irq(int irq);
unsigned long dw_i2c_irqs(void);
unsigned long dw_i2c_aborts(void);
unsigned long dw_i2c_tx_max(void);
unsigned long dw_i2c_rx_max(void);
unsigned long dw_i2c_tx_empty_irqs(void);
unsigned long dw_i2c_rx_full_irqs(void);
void dw_i2c_withhold_stop(int on);
#endif
// end (satoru)
