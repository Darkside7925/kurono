// hidsim: HID-over-I2C, HID parsing, hid-multitouch and input core, driven
// against a software model of this laptop's ELAN06FA touchpad.
//
// the i2c adapter and the i2c-hid transport driver are test code; every driver
// above them is the unmodified pinned tree. QEMU has no LPSS and no Intel GPIO
// community, so this proves everything except the two hardware ends. (satoru)

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/completion.h>
#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_broker.h"
#include "../input/tp_logic.h"
#include "elan06fa_model.h"
#include "elan06fa_report_descriptor.h"

#include "i2c-hid.h"

extern int kld_init_input(void);
extern int kld_init_i2c_core_base(void);
extern int kld_init_hid_core(void);
extern int kld_init_hid_generic(void);
extern int kld_init_hid_multitouch(void);
extern int kld_init_gpio_mockup(void);

static struct elan06fa_model g_model;
static unsigned int g_soft_irq;
static int g_gpio_irq = -1;
static int g_gpio_line = 1;
static int g_hold_low;
static struct tp_state g_tp;
static unsigned int g_tp_ms;
static struct hid_device *g_hdev;
static struct input_dev *g_touch;
static struct input_handle *g_touch_handle;
static struct completion g_frame;
static int g_pass, g_total;

struct ev {
    unsigned int type, code;
    int value;
};

#define MAX_EV 1024
static struct ev g_ev[MAX_EV];
static unsigned int g_evn;

static void tp_irq_raise(struct elan06fa_model *m)
{
    (void)m;
    if (g_gpio_irq < 0)
        return;
    g_gpio_line = 0;   // level-low asserted while the model has data. (satoru)
    irq_set_irqchip_state((unsigned int)g_gpio_irq, IRQCHIP_STATE_PENDING, true);
}

static void tp_irq_deassert(struct elan06fa_model *m)
{
    (void)m;
    if (!g_hold_low)
        g_gpio_line = 1;
}

static int gpio_label_cb(struct gpio_chip *gc, const void *d)
{
    (void)d;
    lx_log("[hidsim] gpiochip label=");
    lx_log_line(gc && gc->label ? gc->label : "?");
    return 0;
}

static const struct property_entry hidsim_gpio_props[] = {
    PROPERTY_ENTRY_U16("nr-gpios", 4),
    { }
};

static int hidsim_gpio_irq_setup(void)
{
    struct platform_device *pdev;
    struct gpio_device *gdev;
    struct gpio_desc *desc;
    int rc;

    rc = kld_init_gpio_mockup();
    lx_log("[hidsim] gpio-mockup init rc=");
    lx_log_u((unsigned long)(rc < 0 ? -rc : rc));
    lx_log("\n");
    if (rc)
        return rc;

    pdev = platform_device_alloc("gpio-mockup", 0);
    if (!pdev) {
        lx_log("[hidsim] gpio-mockup platform device failed\n");
        return -ENOMEM;
    }
    rc = device_create_managed_software_node(&pdev->dev, hidsim_gpio_props,
                                             NULL);
    if (rc) {
        platform_device_put(pdev);
        return rc;
    }
    rc = platform_device_add(pdev);
    if (rc) {
        platform_device_put(pdev);
        return rc;
    }
    gpio_device_find(NULL, gpio_label_cb);
    gdev = gpio_device_find_by_label("gpio-mockup.0");
    if (!gdev)
        gdev = gpio_device_find_by_label("gpio-mockup");
    if (!gdev) {
        lx_log("[hidsim] gpio-mockup chip not found\n");
        return -ENODEV;
    }
    desc = gpio_device_get_desc(gdev, 0);
    if (!desc) {
        lx_log("[hidsim] gpio-mockup line 0 not found\n");
        return -ENODEV;
    }
    g_gpio_irq = gpiod_to_irq(desc);
    lx_log("[hidsim] gpio line 0 -> irq ");
    lx_log_u((unsigned long)g_gpio_irq);
    lx_log(" via ");
    lx_log_line(gpio_device_get_label(gdev));
    return g_gpio_irq < 0 ? g_gpio_irq : 0;
}

static void result(const char *name, int ok, const char *detail)
{
    lx_log("[hidsim] ");
    lx_log(name);
    lx_log(ok ? ": PASS " : ": FAIL ");
    if (detail && detail[0])
        lx_log(detail);
    lx_log("\n");
    g_total++;
    if (ok)
        g_pass++;
}

// ── software i2c adapter ───────────────────────────────────────────────────
static u32 hidsim_func(struct i2c_adapter *adap)
{
    (void)adap;
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE;
}

static const struct i2c_algorithm hidsim_algo = {
    .master_xfer = elan06fa_master_xfer,
    .functionality = hidsim_func,
};

static struct i2c_adapter hidsim_adap = {
    .owner = THIS_MODULE,
    .name = "hidsim-adapter",
    .algo = &hidsim_algo,
};

// ── tiny i2c driver: the call i2c-hid-acpi makes ───────────────────────────
static const struct i2chid_ops hidsim_ops;

static int hidsim_probe(struct i2c_client *client)
{
    return i2c_hid_core_probe(client, (struct i2chid_ops *)&hidsim_ops,
                              0x0001, 0);
}

static const struct i2c_device_id hidsim_id[] = {
    { "ELAN06FA:00", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, hidsim_id);

static struct i2c_driver hidsim_driver = {
    .driver = { .name = "hidsim-i2c-hid" },
    .probe = hidsim_probe,
    .id_table = hidsim_id,
};

// ── input handler: log every event and catch the touchpad ──────────────────
static int hidsim_event(struct input_handle *handle, unsigned int type,
                        unsigned int code, int value)
{
    (void)handle;
    lx_log("[kld-input] ");
    lx_log_u(type);
    lx_log(" ");
    lx_log_u(code);
    lx_log(" ");
    lx_log_u((unsigned long)(value < 0 ? -value : value));
    lx_log(value < 0 ? " (neg)\n" : "\n");
    if (g_evn < MAX_EV) {
        g_ev[g_evn].type = type;
        g_ev[g_evn].code = code;
        g_ev[g_evn].value = value;
        g_evn++;
    }
    if (type == EV_SYN && code == SYN_REPORT)
        complete(&g_frame);
    return 0;
}

static int hidsim_connect(struct input_handler *handler, struct input_dev *dev,
                          const struct input_device_id *id)
{
    struct input_handle *handle;
    int err;

    (void)id;
    handle = kzalloc(sizeof(*handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;
    handle->dev = dev;
    handle->handler = handler;
    err = input_register_handle(handle);
    if (err) {
        kfree(handle);
        return err;
    }
    err = input_open_device(handle);
    if (err) {
        input_unregister_handle(handle);
        kfree(handle);
        return err;
    }
    lx_log("[hidsim] input device '");
    lx_log(dev->name);
    lx_log("'\n");
    if (test_bit(EV_ABS, dev->evbit) && dev->absinfo[ABS_MT_SLOT].maximum >= 0) {
        g_touch = dev;
        g_touch_handle = handle;
    }
    return 0;
}

static void hidsim_disconnect(struct input_handle *handle)
{
    if (handle == g_touch_handle) {
        g_touch = NULL;
        g_touch_handle = NULL;
    }
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

static const struct input_device_id hidsim_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_BUS | INPUT_DEVICE_ID_MATCH_VENDOR |
                 INPUT_DEVICE_ID_MATCH_PRODUCT,
        .bustype = BUS_I2C,
        .vendor = 0x04f3,
        .product = 0x327e,
    },
    { }
};
MODULE_DEVICE_TABLE(input, hidsim_ids);

static struct input_handler hidsim_handler = {
    .event = hidsim_event,
    .connect = hidsim_connect,
    .disconnect = hidsim_disconnect,
    .name = "hidsim",
    .id_table = hidsim_ids,
};

// ── helpers ────────────────────────────────────────────────────────────────
struct hid_find {
    u32 vendor, product;
    struct hid_device *hdev;
};

static int hid_find_fn(struct device *dev, void *data)
{
    struct hid_find *f = data;
    struct hid_device *hdev = to_hid_device(dev);

    if (hdev->vendor == f->vendor && hdev->product == f->product) {
        f->hdev = hdev;
        return 1;
    }
    return 0;
}

static struct hid_device *find_hid(u32 vendor, u32 product)
{
    struct hid_find f = { vendor, product, NULL };
    bus_for_each_dev(&hid_bus_type, NULL, &f, hid_find_fn);
    return f.hdev;
}

static struct hid_report *find_input_report(struct hid_device *hdev, int id)
{
    struct hid_report *report;
    list_for_each_entry(report, &hdev->report_enum[HID_INPUT_REPORT].report_list,
                        list) {
        if (report->id == id)
            return report;
    }
    return NULL;
}

static void set_bits(u8 *buf, unsigned int bit, unsigned int size, u32 val)
{
    unsigned int i;
    for (i = 0; i < size; i++) {
        unsigned int b = bit + i;
        if ((val >> i) & 1)
            buf[b / 8] |= (u8)(1u << (b % 8));
        else
            buf[b / 8] &= (u8)~(1u << (b % 8));
    }
}

static void set_usage(struct hid_report *rep, u8 *buf,
                      unsigned int usage, unsigned int usage_idx, u32 val)
{
    unsigned int i, j;
    for (i = 0; i < rep->maxfield; i++) {
        struct hid_field *f = rep->field[i];
        for (j = 0; j < f->maxusage; j++) {
            if (f->usage[j].hid != usage)
                continue;
            if (usage_idx != j)
                continue;
            set_bits(buf, 8 + f->report_offset + j * f->report_size,
                     f->report_size, val);
            return;
        }
    }
}

// find the next field whose usage array contains this hid, after field 'from'
static struct hid_field *field_with_usage(struct hid_report *rep,
                                          unsigned int usage, int *idx)
{
    unsigned int i, j;
    for (i = 0; i < rep->maxfield; i++) {
        struct hid_field *f = rep->field[i];
        for (j = 0; j < f->maxusage; j++) {
            if (f->usage[j].hid == usage) {
                *idx = (int)j;
                return f;
            }
        }
    }
    return NULL;
}

static unsigned int build_touch_report(struct hid_device *hdev, u8 *buf,
                                       unsigned int max,
                                       const int contacts[][2],
                                       unsigned int ncontacts,
                                       unsigned int lift, int button)
{
    struct hid_report *rep = find_input_report(hdev, 0x54);
    unsigned int len, i, ci = 0;
    int j;

    if (!rep)
        return 0;
    len = hid_report_len(rep);
    if (len > max)
        return 0;
    memset(buf, 0, len);
    buf[0] = 0x54;
    if (button)
        set_usage(rep, buf, 0x00090001, 0, 1);
    set_usage(rep, buf, HID_DG_CONTACTCOUNT, 0, ncontacts);
    for (i = 0; i < rep->maxfield; i++) {
        struct hid_field *f = rep->field[i];
        for (j = 0; j < (int)f->maxusage; j++) {
            unsigned int usage = f->usage[j].hid;
            if (usage == HID_DG_CONTACTID) {
                if (ci < ncontacts || ci < lift) {
                    set_bits(buf, 8 + f->report_offset + j * f->report_size,
                             f->report_size, ci + 1);
                }
                ci++;
            } else if (usage == HID_GD_X && ci > 0 && ci <= ncontacts) {
                set_bits(buf, 8 + f->report_offset + j * f->report_size,
                         f->report_size, (u32)contacts[ci - 1][0]);
            } else if (usage == HID_GD_Y && ci > 0 && ci <= ncontacts) {
                set_bits(buf, 8 + f->report_offset + j * f->report_size,
                         f->report_size, (u32)contacts[ci - 1][1]);
            } else if (usage == HID_DG_TIPSWITCH) {
                if (ci < ncontacts)
                    set_bits(buf, 8 + f->report_offset + j * f->report_size,
                             f->report_size, 1);
            } else if (usage == HID_DG_CONFIDENCE) {
                if (ci < ncontacts || ci < lift)
                    set_bits(buf, 8 + f->report_offset + j * f->report_size,
                             f->report_size, 1);
            }
        }
    }
    (void)field_with_usage;
    return len;
}

struct input_state {
    int next_id;
    int last_slot;
    int active[5];
    int x[5], y[5], have_pos[5];
    int touch, finger, doubletap, tripletap;
    int btn_left;
    int px, py, have_px;
};

static struct input_state g_st;
static struct ev g_exp[MAX_EV];
static unsigned int g_expn;

static void exp_event(unsigned int type, unsigned int code, int value)
{
    if (g_expn < MAX_EV) {
        g_exp[g_expn].type = type;
        g_exp[g_expn].code = code;
        g_exp[g_expn].value = value;
        g_expn++;
    }
}

static void expect_one_contact(unsigned int slot, int x, int y, int active)
{
    if ((int)slot != g_st.last_slot) {
        exp_event(EV_ABS, ABS_MT_SLOT, (int)slot);
        g_st.last_slot = (int)slot;
    }
    if (active && !g_st.active[slot]) {
        g_st.active[slot] = 1;
        exp_event(EV_ABS, ABS_MT_TRACKING_ID, g_st.next_id++);
    } else if (!active && g_st.active[slot]) {
        g_st.active[slot] = 0;
        exp_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    if (!active)
        return;
    if (!g_st.have_pos[slot] || g_st.x[slot] != x) {
        g_st.x[slot] = x;
        exp_event(EV_ABS, ABS_MT_POSITION_X, x);
    }
    if (!g_st.have_pos[slot] || g_st.y[slot] != y) {
        g_st.y[slot] = y;
        exp_event(EV_ABS, ABS_MT_POSITION_Y, y);
    }
    g_st.have_pos[slot] = 1;
}

static void expect_frame(const int contacts[][2], unsigned int ncontacts,
                         unsigned int lift, int button)
{
    unsigned int i;
    int count = 0, first = -1;
    int touch, finger, doubletap, tripletap;

    g_expn = 0;
    for (i = 0; i < ncontacts; i++)
        expect_one_contact(i, contacts[i][0], contacts[i][1], 1);
    for (i = 0; i < lift; i++)
        expect_one_contact(i, 0, 0, 0);

    for (i = 0; i < 5; i++) {
        if (g_st.active[i]) {
            count++;
            if (first < 0)
                first = (int)i;
        }
    }
    touch = count > 0;
    finger = count == 1;
    doubletap = count == 2;
    tripletap = count == 3;
    if (button != g_st.btn_left) {
        exp_event(EV_KEY, BTN_LEFT, button);
        g_st.btn_left = button;
    }
    if (touch != g_st.touch) {
        exp_event(EV_KEY, BTN_TOUCH, touch);
        g_st.touch = touch;
    }
    if (finger != g_st.finger) {
        exp_event(EV_KEY, BTN_TOOL_FINGER, finger);
        g_st.finger = finger;
    }
    if (doubletap != g_st.doubletap) {
        exp_event(EV_KEY, BTN_TOOL_DOUBLETAP, doubletap);
        g_st.doubletap = doubletap;
    }
    if (tripletap != g_st.tripletap) {
        exp_event(EV_KEY, BTN_TOOL_TRIPLETAP, tripletap);
        g_st.tripletap = tripletap;
    }
    if (first >= 0) {
        int x = g_st.x[first], y = g_st.y[first];
        if (!g_st.have_px || x != g_st.px) {
            exp_event(EV_ABS, ABS_X, x);
            g_st.px = x;
        }
        if (!g_st.have_px || y != g_st.py) {
            exp_event(EV_ABS, ABS_Y, y);
            g_st.py = y;
        }
        g_st.have_px = 1;
    }
    exp_event(EV_MSC, MSC_TIMESTAMP, 0);
    exp_event(EV_SYN, SYN_REPORT, 0);
}

static int frame_match(void)
{
    unsigned int i;
    if (g_evn != g_expn) {
        lx_log("[hidsim] frame event count actual=");
        lx_log_u(g_evn);
        lx_log(" expected=");
        lx_log_u(g_expn);
        lx_log("\n");
        return 0;
    }
    for (i = 0; i < g_evn; i++) {
        if (g_ev[i].type != g_exp[i].type || g_ev[i].code != g_exp[i].code ||
            g_ev[i].value != g_exp[i].value) {
            lx_log("[hidsim] frame mismatch at ");
            lx_log_u(i);
            lx_log(": actual ");
            lx_log_u(g_ev[i].type); lx_log("/");
            lx_log_u(g_ev[i].code); lx_log("/");
            lx_log_u((unsigned long)(g_ev[i].value < 0 ? -g_ev[i].value : g_ev[i].value));
            lx_log(" expected ");
            lx_log_u(g_exp[i].type); lx_log("/");
            lx_log_u(g_exp[i].code); lx_log("/");
            lx_log_u((unsigned long)(g_exp[i].value < 0 ? -g_exp[i].value : g_exp[i].value));
            lx_log("\n");
            return 0;
        }
    }
    return 1;
}

static void tp_emit(const int contacts[][2], unsigned int ncontacts,
                    unsigned int lift, int button)
{
    struct tp_frame f;
    struct tp_event ev[16];
    int ne;

    (void)lift;
    memset(&f, 0, sizeof(f));
    f.n = (int)ncontacts;
    f.buttons = button;
    f.now_ms = g_tp_ms += 20;
    for (unsigned int i = 0; i < ncontacts && i < TP_MAX_CONTACTS; i++) {
        f.c[i].active = 1;
        f.c[i].x = contacts[i][0];
        f.c[i].y = contacts[i][1];
        f.c[i].size = 10;
    }
    ne = tp_logic_frame(&g_tp, &f, ev, 16);
    for (int i = 0; i < ne; i++)
        (void)lx_broker_call(LX_KLD_OP_INPUT_POINTER,
                             (unsigned long)(long)ev[i].dx,
                             (unsigned long)(long)ev[i].dy,
                             (unsigned long)ev[i].buttons,
                             (unsigned long)(long)ev[i].wheel);
}

static int feed_touch(const int contacts[][2], unsigned int ncontacts,
                      unsigned int lift, int button, unsigned int timeout_ms)
{
    u8 report[64];
    unsigned int len = build_touch_report(g_hdev, report, sizeof(report),
                                          contacts, ncontacts, lift, button);
    if (!len) {
        lx_log("[hidsim] report build failed\n");
        return -1;
    }
    expect_frame(contacts, ncontacts, lift, button);
    lx_log("[hidsim] input report len=");
    lx_log_u(len);
    lx_log(" first=");
    for (unsigned int i = 0; i < len && i < 8; i++)
        lx_log_hex(report[i]);
    lx_log("\n");
    g_evn = 0;
    reinit_completion(&g_frame);
    elan06fa_queue_input(&g_model, report, len);
    if (!wait_for_completion_timeout(&g_frame, msecs_to_jiffies(timeout_ms))) {
        lx_log("[hidsim] frame timeout input_reads=");
        lx_log_u(g_model.input_reads);
        lx_log("\n");
        return -1;
    }
    if (!frame_match())
        return -1;
    tp_emit(contacts, ncontacts, lift, button);
    lx_log("[hidsim] frame events=");
    lx_log_u(g_evn);
    lx_log(":");
    for (unsigned int i = 0; i < g_evn; i++) {
        lx_log(" ");
        lx_log_u(g_ev[i].type);
        lx_log("/");
        lx_log_u(g_ev[i].code);
        lx_log("/");
        lx_log_u((unsigned long)(g_ev[i].value < 0 ? -g_ev[i].value : g_ev[i].value));
        if (g_ev[i].value < 0)
            lx_log("n");
    }
    lx_log("\n");
    return 0;
}

// ── checks ─────────────────────────────────────────────────────────────────
static int expect_abs(int min, int max, int res)
{
    (void)min; (void)max; (void)res;
    return 1;
}

static void test_descriptors(void)
{
    int ok = 1;
    char detail[128];

    if (!g_hdev)
        ok = 0;
    if (ok && (g_hdev->bus != BUS_I2C || g_hdev->vendor != 0x04f3 ||
               g_hdev->product != 0x327e || g_hdev->version != 0x0100))
        ok = 0;
    if (ok && (g_hdev->rsize != ELAN06FA_REPORT_DESC_LEN ||
               memcmp(g_hdev->rdesc, elan06fa_report_desc,
                      ELAN06FA_REPORT_DESC_LEN)))
        ok = 0;
    if (ok && (g_model.hid_desc_reads == 0 ||
               g_model.report_desc_reads == 0))
        ok = 0;
    snprintf(detail, sizeof(detail), "hid_reads=%u report_reads=%u rsize=%u",
             g_model.hid_desc_reads, g_model.report_desc_reads,
             g_hdev ? g_hdev->rsize : 0);
    result("H1_hid_descriptor", ok, detail);
}

static void test_caps(void)
{
    static const unsigned int ev_bits[] = { EV_SYN, EV_KEY, EV_ABS, EV_MSC };
    static const unsigned int key_bits[] = { 272, 325, 328, 330, 333, 334, 335 };
    static const unsigned int abs_bits[] = { 0, 1, 47, 53, 54, 55, 57 };
    static const unsigned int msc_bits[] = { 5 };
    static const unsigned int prop_bits[] = { 0, 2 };
    int ok = 1, i;
    char detail[192];

    if (!g_touch)
        ok = 0;
    if (ok && strcmp(g_touch->name, "ELAN06FA:00 04F3:327E Touchpad"))
        ok = 0;
    if (ok) {
        for (i = 0; i < EV_CNT; i++) {
            int want = 0;
            for (unsigned int k = 0; k < sizeof(ev_bits)/sizeof(ev_bits[0]); k++)
                if (ev_bits[k] == (unsigned int)i) want = 1;
            if (!!test_bit(i, g_touch->evbit) != want) ok = 0;
        }
        for (i = 0; i < KEY_CNT; i++) {
            int want = 0;
            for (unsigned int k = 0; k < sizeof(key_bits)/sizeof(key_bits[0]); k++)
                if (key_bits[k] == (unsigned int)i) want = 1;
            if (!!test_bit(i, g_touch->keybit) != want) ok = 0;
        }
        for (i = 0; i < ABS_CNT; i++) {
            int want = 0;
            for (unsigned int k = 0; k < sizeof(abs_bits)/sizeof(abs_bits[0]); k++)
                if (abs_bits[k] == (unsigned int)i) want = 1;
            if (!!test_bit(i, g_touch->absbit) != want) ok = 0;
        }
        for (i = 0; i < MSC_CNT; i++) {
            int want = 0;
            for (unsigned int k = 0; k < sizeof(msc_bits)/sizeof(msc_bits[0]); k++)
                if (msc_bits[k] == (unsigned int)i) want = 1;
            if (!!test_bit(i, g_touch->mscbit) != want) ok = 0;
        }
        for (i = 0; i < INPUT_PROP_CNT; i++) {
            int want = 0;
            for (unsigned int k = 0; k < sizeof(prop_bits)/sizeof(prop_bits[0]); k++)
                if (prop_bits[k] == (unsigned int)i) want = 1;
            if (!!test_bit(i, g_touch->propbit) != want) ok = 0;
        }
        if (g_touch->absinfo[ABS_X].minimum != 0 ||
            g_touch->absinfo[ABS_X].maximum != 3679 ||
            g_touch->absinfo[ABS_X].resolution != 31 ||
            g_touch->absinfo[ABS_Y].minimum != 0 ||
            g_touch->absinfo[ABS_Y].maximum != 2261 ||
            g_touch->absinfo[ABS_Y].resolution != 31 ||
            g_touch->absinfo[ABS_MT_SLOT].maximum != 4 ||
            g_touch->absinfo[ABS_MT_POSITION_X].maximum != 3679 ||
            g_touch->absinfo[ABS_MT_POSITION_Y].maximum != 2261 ||
            g_touch->absinfo[ABS_MT_TRACKING_ID].maximum != 65535 ||
            g_touch->absinfo[ABS_MT_TOOL_TYPE].maximum != 2)
            ok = 0;
    }
    if (ok && g_model.device_mode != 3)
        ok = 0;
    snprintf(detail, sizeof(detail),
             "name=%s mode=%d props=%lx props_ok=%d", g_touch ? g_touch->name : "-",
             g_model.device_mode, g_touch ? (unsigned long)g_touch->propbit[0] : 0,
             g_touch != NULL);
    result("H2_multitouch_probe", ok, detail);
}

static int feed_frame(const int contacts[][2], unsigned int n, int button)
{
    return feed_touch(contacts, n, 0, button, 2000);
}

static int feed_release(const int contacts[][2], unsigned int lift)
{
    return feed_touch(contacts, 0, lift, 0, 2000);
}

static void test_scenario_one(void)
{
    int contacts[3][2] = { { 1000, 800 }, { 0, 0 }, { 0, 0 } };
    int ok = 1, i;
    char detail[96];

    lx_log("[hidsim] --- one finger frame 0\n");
    if (!g_hdev || feed_frame((const int (*)[2])contacts, 1, 0))
        ok = 0;
    for (i = 1; ok && i <= 10; i++) {
        contacts[0][0] += 100;
        contacts[0][1] += 80;
        lx_log("[hidsim] --- one finger move ");
        lx_log_u((unsigned long)i);
        lx_log("\n");
        if (feed_frame((const int (*)[2])contacts, 1, 0))
            ok = 0;
    }
    lx_log("[hidsim] --- one finger up\n");
    if (ok && feed_release((const int (*)[2])contacts, 1))
        ok = 0;
    snprintf(detail, sizeof(detail), "events=%u", g_evn);
    result("H3_one_finger", ok, detail);
}

static void test_scenario_two(void)
{
    int contacts[5][2] = { { 1000, 1000 }, { 1200, 1000 }, { 0, 0 },
                           { 0, 0 }, { 0, 0 } };
    int ok = 1, i;
    char detail[96];

    lx_log("[hidsim] --- two finger frame 0\n");
    if (!g_hdev || feed_frame((const int (*)[2])contacts, 2, 0))
        ok = 0;
    for (i = 1; ok && i <= 8; i++) {
        contacts[0][1] += 50;
        contacts[1][1] += 50;
        lx_log("[hidsim] --- two finger scroll ");
        lx_log_u((unsigned long)i);
        lx_log("\n");
        if (feed_frame((const int (*)[2])contacts, 2, 0))
            ok = 0;
    }
    lx_log("[hidsim] --- two finger up\n");
    if (ok && feed_release((const int (*)[2])contacts, 2))
        ok = 0;
    snprintf(detail, sizeof(detail), "events=%u", g_evn);
    result("H3_two_finger_scroll", ok, detail);
}

static void test_scenario_click(void)
{
    int contacts[1][2] = { { 1500, 1000 } };
    int ok = 1;
    char detail[96];

    lx_log("[hidsim] --- click down\n");
    if (!g_hdev || feed_frame((const int (*)[2])contacts, 1, 1))
        ok = 0;
    lx_log("[hidsim] --- click release\n");
    if (ok && feed_frame((const int (*)[2])contacts, 1, 0))
        ok = 0;
    lx_log("[hidsim] --- click up\n");
    if (ok && feed_release((const int (*)[2])contacts, 1))
        ok = 0;
    snprintf(detail, sizeof(detail), "events=%u", g_evn);
    result("H3_click", ok, detail);
}

static void test_scenario_three(void)
{
    int contacts[5][2] = { { 800, 600 }, { 1200, 600 }, { 1600, 600 },
                           { 0, 0 }, { 0, 0 } };
    int ok = 1;
    char detail[96];

    lx_log("[hidsim] --- three finger down\n");
    if (!g_hdev || feed_frame((const int (*)[2])contacts, 3, 0))
        ok = 0;
    lx_log("[hidsim] --- three finger up\n");
    if (ok && feed_release((const int (*)[2])contacts, 3))
        ok = 0;
    snprintf(detail, sizeof(detail), "events=%u", g_evn);
    result("H3_three_finger", ok, detail);
}

static void test_level_low(void)
{
    int contacts[5][2] = { { 900, 700 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    unsigned int before = g_model.input_reads;
    unsigned int asserted, idle_before, idle_after;
    int ok = 1;
    char detail[128];

    // hold the line low across the first two reports: the handler must run
    // again for the second one even though the first read did not deassert it. (satoru)
    g_hold_low = 1;
    if (feed_frame((const int (*)[2])contacts, 1, 0))
        ok = 0;
    if (ok && feed_frame((const int (*)[2])contacts, 1, 0))
        ok = 0;
    asserted = g_model.input_reads;
    if (asserted < before + 2)
        ok = 0;

    // release the level and fire once more: with no input pending the line is
    // high, so the handler must not consume a report. (satoru)
    g_hold_low = 0;
    tp_irq_deassert(&g_model);
    idle_before = g_model.input_reads;
    irq_set_irqchip_state((unsigned int)g_gpio_irq, IRQCHIP_STATE_PENDING, true);
    lx_os_sleep_ns(50000000ull);
    idle_after = g_model.input_reads;
    if (idle_after != idle_before)
        ok = 0;
    snprintf(detail, sizeof(detail), "line=%d reads=%u/%u/%u",
             g_gpio_line, before, asserted, idle_after);
    result("H3_level_low", ok, detail);
}

int kld_test_main(int argc, char **argv)
{
    struct i2c_board_info info;
    struct i2c_client *client;
    int rc;

    (void)argc; (void)argv;
    lx_log("[hidsim] HID-over-I2C simulation starting\n");
    init_completion(&g_frame);
    tp_logic_init(&g_tp, NULL);

    rc = kld_init_input();
    lx_log("[hidsim] input init rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");
    rc = kld_init_i2c_core_base();
    lx_log("[hidsim] i2c init rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");
    rc = kld_init_hid_core();
    lx_log("[hidsim] hid init rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");
    rc = kld_init_hid_generic();
    lx_log("[hidsim] hid-generic init rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");
    rc = kld_init_hid_multitouch();
    lx_log("[hidsim] hid-multitouch init rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");

    rc = hidsim_gpio_irq_setup();
    if (rc) {
        lx_log("[hidsim] no gpiolib irq\n");
        return 1;
    }
    elan06fa_init(&g_model, g_gpio_irq);
    g_model.irq_raise = tp_irq_raise;
    g_model.irq_deassert = tp_irq_deassert;
    hidsim_adap.algo_data = &g_model;
    rc = i2c_add_adapter(&hidsim_adap);
    lx_log("[hidsim] i2c_add_adapter rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");
    if (rc < 0)
        return 1;

    rc = input_register_handler(&hidsim_handler);
    lx_log("[hidsim] input_register_handler rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");

    rc = i2c_add_driver(&hidsim_driver);
    lx_log("[hidsim] i2c_add_driver rc="); lx_log_u((unsigned long)(rc < 0 ? -rc : rc)); lx_log("\n");

    memset(&info, 0, sizeof(info));
    info.addr = ELAN06FA_I2C_ADDR;
    strscpy(info.type, "ELAN06FA:00", sizeof(info.type));
    info.irq = g_gpio_irq;
    client = i2c_new_client_device(&hidsim_adap, &info);
    if (IS_ERR(client)) {
        lx_log("[hidsim] client create failed\n");
        return 1;
    }
    g_hdev = find_hid(0x04f3, 0x327e);
    lx_log("[hidsim] hdev=");
    lx_log_hex((unsigned long)g_hdev);
    lx_log(" touch=");
    lx_log_hex((unsigned long)g_touch);
    lx_log("\n");

    test_descriptors();
    test_caps();
    test_scenario_one();
    test_scenario_two();
    test_scenario_click();
    test_scenario_three();
    test_level_low();

    lx_log("[hidsim] ");
    lx_log_u((unsigned long)g_pass);
    lx_log("/");
    lx_log_u((unsigned long)g_total);
    lx_log(g_pass == g_total ? " PASS\n" : " FAIL\n");
    return g_pass == g_total ? 0 : 1;
}

// end (satoru)
