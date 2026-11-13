// elan06fa_model: a software HID-over-I2C device that speaks the register
// protocol of the real ELAN06FA touchpad (i2c 0x15). it only models the
// device; the i2c adapter is in hidsim.c and the drivers are upstream. (satoru)

#ifndef KLD_ELAN06FA_MODEL_H
#define KLD_ELAN06FA_MODEL_H

#include <linux/types.h>

#define ELAN06FA_I2C_ADDR 0x15

struct elan06fa_model {
    int irq;
    void (*irq_raise)(struct elan06fa_model *m);
    void (*irq_deassert)(struct elan06fa_model *m);
    const unsigned char *report_desc;
    unsigned int report_desc_len;
    u8 hid_desc[30];
    u8 get[2048];
    unsigned int get_len;
    int have_get;
    u8 input[2048];
    unsigned int input_len;
    int have_input;
    u8 last_set[2048];
    unsigned int last_set_len;
    int last_set_type;
    int last_set_id;
    int device_mode;
    unsigned int hid_desc_reads;
    unsigned int report_desc_reads;
    unsigned int resets;
    unsigned int power_ons;
    unsigned int set_reports;
    unsigned int input_reads;
};

void elan06fa_init(struct elan06fa_model *m, int irq);
int elan06fa_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
void elan06fa_queue_input(struct elan06fa_model *m, const u8 *report,
                          unsigned int len);
void elan06fa_queue_reset(struct elan06fa_model *m);

#endif
// end (satoru)
