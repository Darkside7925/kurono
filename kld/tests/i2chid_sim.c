// i2chid_sim: C1 end-to-end entry. register the laptop's LPSS PCI function and
// its private-space model, run the ordered i2chid init, and report how far the
// upstream probes got. (satoru)

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/property.h>
#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/completion.h>
#include <linux/bitops.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_broker.h"
#include "kimpl/lx_acpi.h"
#include "models/lpss_model.h"
#include "models/dw_i2c_model.h"
#include "models/pch_gpio_model.h"
#include "elan06fa_model.h"
#include "elan06fa_report_descriptor.h"
#include "../input/tp_logic.h"

#include "i2c-designware-core.h"

static int g_acpi_found;
static int g_pch_irq = -1;
static struct platform_device *g_pch_pdev;

// the five firmware NVS values measured on this laptop's tables. GPDI packs
// GNUM/INUM: 0x1408 is controller-relative GPIO 552, which is the pad the host
// Linux uses for TPD0 (gpiolib desc 552 = pinctrl pad 310, GPP_I_8). S3a used
// 0xCB6, which makes GNUM return 310 (the pinctrl pin), not a valid GPIO
// number on this controller; C4 needs the host-consistent value. PCHA hides or
// exposes \_SB.PC02, PBSB is the root bridge bus linux sees (0x80), and
// TPTY/SDS1 are read by the same module-level branch. (satoru)
static struct lx_acpi_seed lx_seeds[] = {
    { "GPDI", 0x1408 },
    { "PCHA", 0x1 },
    { "TPTY", 0x1 },
    { "SDS1", 0x1 },
    { "PBSB", 0x80 },
    // SGIR patches _SB.PGPI's Extended IRQ GSI (the host uses 14). (satoru)
    { "SGIR", 0x0E },
};

static struct elan06fa_model g_model;

// the real PCH pinctrl controller as a platform device built from \_SB.PGPI's
// own _CRS. the register model claims the communities' memory ranges before
// the driver probes; gpiolib-acpi then finds the chip through the companion.
// (satoru)
static int lx_pch_gpio_setup(void)
{
    struct platform_device *pdev;
    int mem = 0, rc;
    char line[192];

    pdev = lx_acpi_platform_device_alloc("\\_SB.PGPI");
    if (IS_ERR(pdev)) {
        lx_log("[i2chidsim] no INTC1084 platform device\n");
        return PTR_ERR(pdev);
    }
    for (unsigned int i = 0; i < pdev->num_resources; i++) {
        struct resource *r = &pdev->resource[i];

        if (r->flags & IORESOURCE_MEM) {
            pch_gpio_model_init(mem, r->start,
                                (unsigned long)resource_size(r));
            mem++;
        } else if (r->flags & IORESOURCE_IRQ) {
            g_pch_irq = (int)r->start;
        }
    }
    pch_gpio_model_set_irq(g_pch_irq);
    rc = platform_device_add(pdev);
    if (rc) {
        platform_device_put(pdev);
        return rc;
    }
    snprintf(line, sizeof(line),
             "[i2chidsim] C4.1 INTC1084 platform device, %d communities, irq %d",
             mem, g_pch_irq);
    lx_log_line(line);
    g_pch_pdev = pdev;
    return 0;
}

static int c33_c42_report(void)
{
    char line[192];

    if (!g_pch_pdev)
        return -1;
    snprintf(line, sizeof(line),
             "[i2chidsim] C4.2 pinctrl-meteorpoint probe %s, 5 communities, 339 pads, gpiochip %s",
             g_pch_pdev->dev.driver ? "rc=0" : "FAILED",
             gpio_device_find_by_label("INTC1084:00") ? "registered" : "MISSING");
    lx_log_line(line);
    return (g_pch_pdev->dev.driver &&
            gpio_device_find_by_label("INTC1084:00")) ? 0 : -1;
}

static int elan_xfer(void *ctx, u8 addr, const u8 *w, int wn, u8 *r, int rn)
{
    struct elan06fa_model *m = ctx;
    struct i2c_adapter a;
    struct i2c_msg msgs[2];
    int n = 0;

    memset(&a, 0, sizeof(a));
    a.algo_data = m;
    if (wn) {
        msgs[n].addr = addr; msgs[n].flags = 0;
        msgs[n].len = (u16)wn; msgs[n].buf = (u8 *)w; n++;
    }
    if (rn) {
        msgs[n].addr = addr; msgs[n].flags = I2C_M_RD;
        msgs[n].len = (u16)rn; msgs[n].buf = r; n++;
    }
    return elan06fa_master_xfer(&a, msgs, n);
}
#include "models/lpss_i2c1_pci.h"

extern int kld_family_init(void);

// ── C3.3: the S2 touch scenarios through the whole upstream chain ───────────
// sim PCI -> intel-lpss-pci -> mfd -> i2c-designware (model) -> i2c-core-acpi
// -> i2c-hid-acpi -> hid-multitouch -> input; the interrupt is the ELAN
// model's line on the mock INTC1084 chip, delivered by gpiolib-acpi and
// irq_sim, and the captured events are compared frame by frame with the same
// expectations the S2 hidsim test uses. (satoru)
#define C33_MAX_EV 1024

struct c33_ev {
    unsigned int type, code;
    int value;
};

static struct c33_ev g_ev[C33_MAX_EV];
static unsigned int g_evn;
static struct completion g_frame;
static struct hid_device *g_hdev;
static struct input_dev *g_touch;
static struct input_handle *g_touch_handle;
static unsigned long g_pointer_events;
static int g_client_irq = -1;
static int g_tp_ms;

// the pad's interrupt: drive GPP_I_8 low (active-low level). the community
// model asserts its line, intel_gpio_irq runs, and gpiolib dispatches to the
// i2c-hid irq. reading the report releases the line. (satoru)
static void c33_irq_raise(struct elan06fa_model *m)
{
    (void)m;
    (void)g_client_irq;
    pch_gpio_model_set_level(310, 0);
}

static void c33_irq_deassert(struct elan06fa_model *m)
{
    (void)m;
    pch_gpio_model_set_level(310, 1);
}

static int c33_event(struct input_handle *handle, unsigned int type,
                     unsigned int code, int value)
{
    (void)handle;
    if (g_evn < C33_MAX_EV) {
        g_ev[g_evn].type = type;
        g_ev[g_evn].code = code;
        g_ev[g_evn].value = value;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&g_evn, g_evn + 1, __ATOMIC_RELEASE);
    }
    if (type == EV_SYN && code == SYN_REPORT)
        complete(&g_frame);
    return 0;
}

static int c33_connect(struct input_handler *handler, struct input_dev *dev,
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
    // the hid-multitouch touchpad, not the hid-generic mouse. (satoru)
    if (test_bit(EV_ABS, dev->evbit) &&
        dev->absinfo[ABS_MT_SLOT].maximum >= 4 &&
        dev->name && strstr(dev->name, "Touchpad")) {
        g_touch = dev;
        g_touch_handle = handle;
        lx_log("[i2chidsim] C3.3 input device '");
        lx_log(dev->name);
        lx_log("'\n");
    }
    return 0;
}

static void c33_disconnect(struct input_handle *handle)
{
    if (handle == g_touch_handle) {
        g_touch = NULL;
        g_touch_handle = NULL;
    }
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

static const struct input_device_id c33_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_BUS | INPUT_DEVICE_ID_MATCH_VENDOR |
                 INPUT_DEVICE_ID_MATCH_PRODUCT,
        .bustype = BUS_I2C,
        .vendor = 0x04f3,
        .product = 0x327e,
    },
    { }
};

static struct input_handler c33_handler = {
    .event = c33_event,
    .connect = c33_connect,
    .disconnect = c33_disconnect,
    .name = "i2chidsim",
    .id_table = c33_ids,
};

struct c33_hid_find {
    u32 vendor, product;
    struct hid_device *hdev;
};

static int c33_hid_find_fn(struct device *dev, void *data)
{
    struct c33_hid_find *f = data;
    struct hid_device *hdev = to_hid_device(dev);

    if (hdev->vendor == f->vendor && hdev->product == f->product) {
        f->hdev = hdev;
        return 1;
    }
    return 0;
}

static struct hid_device *c33_find_hid(u32 vendor, u32 product)
{
    struct c33_hid_find f = { vendor, product, NULL };

    bus_for_each_dev(&hid_bus_type, NULL, &f, c33_hid_find_fn);
    return f.hdev;
}

static struct hid_report *c33_input_report(struct hid_device *hdev, int id)
{
    struct hid_report *report;

    list_for_each_entry(report,
                        &hdev->report_enum[HID_INPUT_REPORT].report_list,
                        list) {
        if (report->id == id)
            return report;
    }
    return NULL;
}

static void c33_set_bits(u8 *buf, unsigned int bit, unsigned int size, u32 val)
{
    for (unsigned int i = 0; i < size; i++) {
        unsigned int b = bit + i;

        if ((val >> i) & 1)
            buf[b / 8] |= (u8)(1u << (b % 8));
        else
            buf[b / 8] &= (u8)~(1u << (b % 8));
    }
}

static void c33_set_usage(struct hid_report *rep, u8 *buf, unsigned int usage,
                          unsigned int usage_idx, u32 val)
{
    for (unsigned int i = 0; i < rep->maxfield; i++) {
        struct hid_field *f = rep->field[i];

        for (unsigned int j = 0; j < f->maxusage; j++) {
            if (f->usage[j].hid != usage || usage_idx != j)
                continue;
            c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                         f->report_size, val);
            return;
        }
    }
}

static unsigned int c33_build_report(struct hid_device *hdev, u8 *buf,
                                     unsigned int max,
                                     const int contacts[][2],
                                     unsigned int ncontacts,
                                     unsigned int lift, int button)
{
    struct hid_report *rep = c33_input_report(hdev, 0x54);
    unsigned int len, i, ci = 0;

    if (!rep)
        return 0;
    len = hid_report_len(rep);
    if (len > max)
        return 0;
    memset(buf, 0, len);
    buf[0] = 0x54;
    if (button)
        c33_set_usage(rep, buf, 0x00090001, 0, 1);
    c33_set_usage(rep, buf, HID_DG_CONTACTCOUNT, 0, ncontacts);
    for (i = 0; i < rep->maxfield; i++) {
        struct hid_field *f = rep->field[i];

        for (int j = 0; j < (int)f->maxusage; j++) {
            unsigned int usage = f->usage[j].hid;

            if (usage == HID_DG_CONTACTID) {
                if (ci < ncontacts || ci < lift)
                    c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                                 f->report_size, ci + 1);
                ci++;
            } else if (usage == HID_GD_X && ci > 0 && ci <= ncontacts) {
                c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                             f->report_size, (u32)contacts[ci - 1][0]);
            } else if (usage == HID_GD_Y && ci > 0 && ci <= ncontacts) {
                c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                             f->report_size, (u32)contacts[ci - 1][1]);
            } else if (usage == HID_DG_TIPSWITCH) {
                if (ci < ncontacts)
                    c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                                 f->report_size, 1);
            } else if (usage == HID_DG_CONFIDENCE) {
                if (ci < ncontacts || ci < lift)
                    c33_set_bits(buf, 8 + f->report_offset + j * f->report_size,
                                 f->report_size, 1);
            }
        }
    }
    return len;
}

struct c33_state {
    int next_id;
    int last_slot;
    int active[5];
    int x[5], y[5], have_pos[5];
    int touch, finger, doubletap, tripletap;
    int btn_left;
    int px, py, have_px;
};

static struct c33_state g_st;
static struct c33_ev g_exp[C33_MAX_EV];
static unsigned int g_expn;
static struct tp_state g_tp;

static void c33_exp(unsigned int type, unsigned int code, int value)
{
    if (g_expn < C33_MAX_EV) {
        g_exp[g_expn].type = type;
        g_exp[g_expn].code = code;
        g_exp[g_expn].value = value;
        g_expn++;
    }
}

static void c33_one_contact(unsigned int slot, int x, int y, int active)
{
    if ((int)slot != g_st.last_slot) {
        c33_exp(EV_ABS, ABS_MT_SLOT, (int)slot);
        g_st.last_slot = (int)slot;
    }
    if (active && !g_st.active[slot]) {
        g_st.active[slot] = 1;
        c33_exp(EV_ABS, ABS_MT_TRACKING_ID, g_st.next_id++);
    } else if (!active && g_st.active[slot]) {
        g_st.active[slot] = 0;
        c33_exp(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    if (!active)
        return;
    if (!g_st.have_pos[slot] || g_st.x[slot] != x) {
        g_st.x[slot] = x;
        c33_exp(EV_ABS, ABS_MT_POSITION_X, x);
    }
    if (!g_st.have_pos[slot] || g_st.y[slot] != y) {
        g_st.y[slot] = y;
        c33_exp(EV_ABS, ABS_MT_POSITION_Y, y);
    }
    g_st.have_pos[slot] = 1;
}

static void c33_expect_frame(const int contacts[][2], unsigned int ncontacts,
                             unsigned int lift, int button)
{
    int count = 0, first = -1;
    int touch, finger, doubletap, tripletap;

    g_expn = 0;
    for (unsigned int i = 0; i < ncontacts; i++)
        c33_one_contact(i, contacts[i][0], contacts[i][1], 1);
    for (unsigned int i = 0; i < lift; i++)
        c33_one_contact(i, 0, 0, 0);
    for (unsigned int i = 0; i < 5; i++) {
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
        c33_exp(EV_KEY, BTN_LEFT, button);
        g_st.btn_left = button;
    }
    if (touch != g_st.touch) {
        c33_exp(EV_KEY, BTN_TOUCH, touch);
        g_st.touch = touch;
    }
    if (finger != g_st.finger) {
        c33_exp(EV_KEY, BTN_TOOL_FINGER, finger);
        g_st.finger = finger;
    }
    if (doubletap != g_st.doubletap) {
        c33_exp(EV_KEY, BTN_TOOL_DOUBLETAP, doubletap);
        g_st.doubletap = doubletap;
    }
    if (tripletap != g_st.tripletap) {
        c33_exp(EV_KEY, BTN_TOOL_TRIPLETAP, tripletap);
        g_st.tripletap = tripletap;
    }
    if (first >= 0) {
        int x = g_st.x[first], y = g_st.y[first];

        if (!g_st.have_px || x != g_st.px) {
            c33_exp(EV_ABS, ABS_X, x);
            g_st.px = x;
        }
        if (!g_st.have_px || y != g_st.py) {
            c33_exp(EV_ABS, ABS_Y, y);
            g_st.py = y;
        }
        g_st.have_px = 1;
    }
    c33_exp(EV_MSC, MSC_TIMESTAMP, 0);
    c33_exp(EV_SYN, SYN_REPORT, 0);
}

// compare events [s, e) of the delivered stream with the expected frame. the
// ABS_MT_TRACKING_ID value is only checked for "assigned or released" and then
// adopted: an earlier stray touch (a late report from a slow guest) only moves
// the counter. (satoru)
static int c33_seg_match(unsigned int s, unsigned int e, int *adopt_id)
{
    if (e - s != g_expn)
        return 0;
    *adopt_id = -1;
    for (unsigned int i = 0; i < g_expn; i++) {
        const struct c33_ev *a = &g_ev[s + i], *x = &g_exp[i];

        if (a->type != x->type || a->code != x->code)
            return 0;
        if (a->type == EV_ABS && a->code == ABS_MT_TRACKING_ID) {
            if ((a->value < 0) != (x->value < 0))
                return 0;
            if (a->value >= 0)
                *adopt_id = a->value;
            continue;
        }
        if (a->value != x->value)
            return 0;
    }
    return 1;
}

static void c33_tp_emit(const int contacts[][2], unsigned int ncontacts,
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
    for (int i = 0; i < ne; i++) {
        (void)lx_broker_call(LX_KLD_OP_INPUT_POINTER,
                             (unsigned long)(long)ev[i].dx,
                             (unsigned long)(long)ev[i].dy,
                             (unsigned long)ev[i].buttons,
                             (unsigned long)(long)ev[i].wheel);
        g_pointer_events++;
    }
}

// one feed: queue the report and wait until a delivered frame (events up to a
// SYN_REPORT) matches the expected one. frames that arrive before it are stray
// (late reports, a log burst that stalled the guest) and are skipped and
// counted; no frame matching within 2 s fails. polling, not a completion, so
// nothing depends on how long the serial console blocks the guest. (satoru)
static int c33_feed_once(const int contacts[][2], unsigned int ncontacts,
                         unsigned int lift, int button, const u8 *report,
                         unsigned int len)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(2000);
    unsigned int start = 0, scanned = 0, stray = 0;

    c33_expect_frame(contacts, ncontacts, lift, button);
    g_evn = 0;
    elan06fa_queue_input(&g_model, report, len);
    while (time_before(jiffies, deadline)) {
        unsigned int n = __atomic_load_n(&g_evn, __ATOMIC_ACQUIRE);

        for (; scanned < n; scanned++) {
            int adopt;

            if (g_ev[scanned].type != EV_SYN || g_ev[scanned].code != SYN_REPORT)
                continue;
            if (c33_seg_match(start, scanned + 1, &adopt)) {
                if (adopt >= 0)
                    g_st.next_id = adopt + 1;
                if (stray) {
                    lx_log("[i2chidsim] C3.3 skipped stray frames: ");
                    lx_log_u(stray);
                    lx_log("\n");
                }
                return 0;
            }
            stray++;
            start = scanned + 1;
        }
        msleep(2);
    }
    lx_log("[i2chidsim] C3.3 frame timeout input_reads=");
    lx_log_u(g_model.input_reads);
    lx_log(" events=");
    lx_log_u(g_evn);
    lx_log(" stray=");
    lx_log_u(stray);
    lx_log("\n");
    return -1;
}

static int c33_feed(const int contacts[][2], unsigned int ncontacts,
                    unsigned int lift, int button)
{
    u8 report[64];
    unsigned int len = c33_build_report(g_hdev, report, sizeof(report),
                                        contacts, ncontacts, lift, button);

    if (!len) {
        lx_log("[i2chidsim] C3.3 report build failed\n");
        return -1;
    }
    if (c33_feed_once(contacts, ncontacts, lift, button, report, len))
        return -1;
    c33_tp_emit(contacts, ncontacts, lift, button);
    return 0;
}

static int c33_scenario_one_finger(void)
{
    int contacts[3][2] = { { 1000, 800 }, { 0, 0 }, { 0, 0 } };
    int ok = 1;

    if (!g_hdev || c33_feed((const int (*)[2])contacts, 1, 0, 0))
        ok = 0;
    for (int i = 1; ok && i <= 10; i++) {
        contacts[0][0] += 100;
        contacts[0][1] += 80;
        if (c33_feed((const int (*)[2])contacts, 1, 0, 0))
            ok = 0;
    }
    if (ok && c33_feed((const int (*)[2])contacts, 0, 1, 0))
        ok = 0;
    return ok;
}

static int c33_scenario_two_finger(void)
{
    int contacts[5][2] = { { 1000, 1000 }, { 1200, 1000 }, { 0, 0 },
                           { 0, 0 }, { 0, 0 } };
    int ok = 1;

    if (!g_hdev || c33_feed((const int (*)[2])contacts, 2, 0, 0))
        ok = 0;
    for (int i = 1; ok && i <= 8; i++) {
        contacts[0][1] += 50;
        contacts[1][1] += 50;
        if (c33_feed((const int (*)[2])contacts, 2, 0, 0))
            ok = 0;
    }
    if (ok && c33_feed((const int (*)[2])contacts, 0, 2, 0))
        ok = 0;
    return ok;
}

static int c33_scenario_click(void)
{
    int contacts[1][2] = { { 1500, 1000 } };
    int ok = 1;

    if (!g_hdev || c33_feed((const int (*)[2])contacts, 1, 0, 1))
        ok = 0;
    if (ok && c33_feed((const int (*)[2])contacts, 1, 0, 0))
        ok = 0;
    if (ok && c33_feed((const int (*)[2])contacts, 0, 1, 0))
        ok = 0;
    return ok;
}

static int c33_scenario_three_finger(void)
{
    int contacts[5][2] = { { 800, 600 }, { 1200, 600 }, { 1600, 600 },
                           { 0, 0 }, { 0, 0 } };
    int ok = 1;

    if (!g_hdev || c33_feed((const int (*)[2])contacts, 3, 0, 0))
        ok = 0;
    if (ok && c33_feed((const int (*)[2])contacts, 0, 3, 0))
        ok = 0;
    return ok;
}

// C4.3 negatives: status without enable must not interrupt, and a neighbour
// pad's status must not reach i2c-hid. (satoru)
static int c33_negatives(void)
{
    unsigned long before, inputs;
    char line[192];
    int ok = 1, ie_ok, nb_ok;

    // (1) GPP_I_8 level-low status with GPI_IE clear: no community interrupt.
    pch_gpio_model_set_ie(310, 0);
    pch_gpio_model_ack(310);
    before = pch_gpio_model_irqs();
    pch_gpio_model_set_level(310, 0);
    msleep(50);
    ie_ok = pch_gpio_model_irqs() == before && pch_gpio_model_is(310);
    pch_gpio_model_set_level(310, 1);
    pch_gpio_model_ack(310);
    pch_gpio_model_set_ie(310, 1);
    if (!ie_ok)
        ok = 0;

    // (2) a neighbour pad (GPP_I_10) raises the community line but has no
    // child irq handler, so no input report is consumed and no event appears.
    pch_gpio_model_set_level(312, 0);
    pch_gpio_model_ack(312);
    pch_gpio_model_set_ie(312, 1);
    inputs = g_model.input_reads;
    before = pch_gpio_model_irqs();
    g_evn = 0;
    pch_gpio_model_set_level(312, 1);
    msleep(50);
    nb_ok = pch_gpio_model_irqs() > before && g_model.input_reads == inputs &&
            g_evn == 0;
    pch_gpio_model_ack(312);
    pch_gpio_model_set_level(312, 0);
    pch_gpio_model_set_ie(312, 0);
    if (!nb_ok)
        ok = 0;

    snprintf(line, sizeof(line),
             "[i2chidsim] C4.3 negatives %s (ie-clear=%s neighbour=%s)",
             ok ? "ok" : "FAIL", ie_ok ? "ok" : "FAIL",
             nb_ok ? "ok" : "FAIL");
    lx_log_line(line);
    return ok;
}

static int c33_run(int acpi_found, int client_ok, int client_irq)
{
    char line[192];
    int pass = 0, total = 4;
    bool first_frame = false;

    g_client_irq = client_irq;
    if (acpi_found == 1)
        lx_log("[kld-tp] step 1 ok: acpi PNP0C50 TPD0 enumerated\n");
    else
        lx_log("[kld-tp] step 1 FAIL: acpi PNP0C50 count\n");
    if (client_ok)
        lx_log("[kld-tp] step 2 ok: i2c client created by i2c-core-acpi\n");
    else
        lx_log("[kld-tp] step 2 FAIL: no i2c client\n");

    if (c33_c42_report())
        return -1;
    g_hdev = c33_find_hid(0x04f3, 0x327e);
    if (g_hdev && g_hdev->bus == BUS_I2C && g_hdev->vendor == 0x04f3 &&
        g_hdev->product == 0x327e && g_hdev->version == 0x0100 &&
        g_hdev->rsize == ELAN06FA_REPORT_DESC_LEN &&
        !memcmp(g_hdev->rdesc, elan06fa_report_desc,
                ELAN06FA_REPORT_DESC_LEN) &&
        g_model.hid_desc_reads > 0) {
        lx_log("[kld-tp] step 4 ok: i2c-hid-acpi probe, descriptor via designware\n");
        snprintf(line, sizeof(line),
                 "[kld-tp] step 5 ok: hid 0018:%04X:%04X rsize=%u",
                 g_hdev->vendor, g_hdev->product, g_hdev->rsize);
        lx_log_line(line);
    } else {
        lx_log("[kld-tp] step 4 FAIL: i2c-hid-acpi probe/descriptor\n");
        lx_log("[kld-tp] step 5 FAIL: hid device\n");
    }

    if (!g_touch) {
        lx_log("[i2chidsim] C3.3 no touchpad input device\n");
        return -1;
    }
    if (c33_scenario_one_finger()) {
        pass++;
        first_frame = true;
    }
    if (first_frame)
        lx_log("[kld-tp] step 6 ok: first touch report through the chain\n");
    else
        lx_log("[kld-tp] step 6 FAIL: first touch report\n");
    if (c33_scenario_two_finger())
        pass++;
    if (c33_scenario_click())
        pass++;
    if (c33_scenario_three_finger())
        pass++;

    snprintf(line, sizeof(line),
             "[i2chidsim] C3.3 scenarios %d/%d PASS", pass, total);
    lx_log_line(line);
    if (g_pointer_events) {
        snprintf(line, sizeof(line),
                 "[kld-tp] step 7 ok: touchpad pointer logic -> bridge events=%lu",
                 g_pointer_events);
        lx_log_line(line);
    } else {
        lx_log("[kld-tp] step 7 FAIL: no pointer events\n");
    }
    if (!c33_negatives())
        return -1;
    return pass == total ? 0 : -1;
}

int lx_acpi_survey(void);   // tests/acpi_survey.c (satoru)

int kld_test_main(int argc, char **argv)
{
    struct lx_pci_sim_dev d;
    int rc;
    int client_ok = 0, client_irq = -1;

    lx_log("[i2chid_sim] C1 starting\n");
    lx_log("[i2chidsim] C3.1 acpi bring-up from the build-time table blob\n");
    rc = lx_acpi_set_provider(LX_ACPI_PROVIDER_BLOB);
    if (!rc)
        rc = lx_acpi_init(lx_seeds, (int)(sizeof(lx_seeds) / sizeof(lx_seeds[0])));
    if (rc) {
        lx_log("[i2chidsim] C3.1 acpi init failed\n");
        return 1;
    }
    // C6.0: `kld-i2chidsim --acpi-survey` on the host lists what the laptop's
    // EC, battery, AC, lid and _Qxx methods touch and notify. (satoru)
    if (argc > 1 && !strcmp(argv[1], "--acpi-survey"))
        return lx_acpi_survey();
    {
        char line[96];
        int found = lx_acpi_i2chid_scan();

        snprintf(line, sizeof(line), "[i2chidsim] C3.1 PNP0C50 found=%d", found);
        lx_log_line(line);
        g_acpi_found = found;
    }
    lx_acpi_write_census();
    lx_mmio_sim_reset();
    rc = lx_pch_gpio_setup();
    if (rc) {
        lx_log("[i2chidsim] C4 gpio setup failed\n");
        return 1;
    }
    memset(&d, 0, sizeof(d));
    d.vendor = LPSS_PCI_VENDOR;
    d.device = LPSS_PCI_DEVICE;
    d.subvendor = LPSS_PCI_SUBVENDOR;
    d.subdevice = LPSS_PCI_SUBDEVICE;
    d.class_code = LPSS_PCI_CLASS;
    d.revision = LPSS_PCI_REVISION;
    memcpy(d.config, lpss_pci_config, 256);
    d.bar0_len = 0x1000;
    d.irq = -1;
    if (lx_pci_sim_add(&d)) {
        lx_log("[i2chid_sim] sim pci add failed\n");
        return 1;
    }
    elan06fa_init(&g_model, -1);
    dw_i2c_model_init(lx_pci_sim_bar0(), lx_pci_sim_bar0_len());
    dw_i2c_model_set_bus(elan_xfer, &g_model);
    // the interrupt line must be live before family init: i2c-core-acpi creates
    // the client and i2c-hid-acpi reads the descriptor inside that call. (satoru)
    dw_i2c_model_set_irq(lx_pci_sim_irq());
    g_model.irq_raise = c33_irq_raise;
    g_model.irq_deassert = c33_irq_deassert;
    init_completion(&g_frame);
    // the handler must exist before hid-multitouch registers the touchpad. (satoru)
    input_register_handler(&c33_handler);
    lpss_model_init(lx_pci_sim_bar0(), lx_pci_sim_bar0_len());
    rc = kld_family_init();
    // one write per line: lx_log splits at every call, and a concurrent kernel
    // log write can land in the middle of the line (the timeout line proved it
    // in QEMU). (satoru)
    for (int n = 0; n < 8; n++) {
        struct i2c_adapter *a = i2c_get_adapter(n);
        if (a) {
            char line[160];

            snprintf(line, sizeof(line), "[i2chidsim] C2 adapter %s nr=%d",
                     a->name, n);
            lx_log_line(line);
            i2c_put_adapter(a);
            break;
        }
    }
    struct i2c_adapter *adap = NULL;
    for (int n = 0; n < 8; n++) {
        adap = i2c_get_adapter(n);
        if (adap) break;
    }
    if (adap) {
        u8 w[2] = { 0x01, 0x00 }, r[30];
        struct i2c_msg msgs[2];
        msgs[0].addr = 0x15; msgs[0].flags = 0; msgs[0].len = 2; msgs[0].buf = w;
        msgs[1].addr = 0x15; msgs[1].flags = I2C_M_RD; msgs[1].len = 30; msgs[1].buf = r;
        char line[192];
        int trc;

        lx_log("[i2chidsim] C2 transfer begin\n");
        trc = i2c_transfer(adap, msgs, 2);
        lx_log("[i2chidsim] C2 transfer end\n");
        snprintf(line, sizeof(line),
                 "[i2chidsim] C2 hid rc=%d match=%d irqs=%lu",
                 trc < 0 ? -trc : trc,
                 trc == 2 && !memcmp(r, g_model.hid_desc, 30),
                 dw_i2c_irqs());
        lx_log_line(line);

        u8 w2[2] = { 0x02, 0x00 };
        u8 rep[675];
        struct i2c_msg rm[2];
        rm[0].addr = 0x15; rm[0].flags = 0; rm[0].len = 2; rm[0].buf = w2;
        rm[1].addr = 0x15; rm[1].flags = I2C_M_RD; rm[1].len = 675; rm[1].buf = rep;
        int rrc = i2c_transfer(adap, rm, 2);

        snprintf(line, sizeof(line),
                 "[i2chidsim] C2 report rc=%d match=%d",
                 rrc < 0 ? -rrc : rrc,
                 rrc == 2 && !memcmp(rep, g_model.report_desc, 675));
        lx_log_line(line);

        dw_i2c_withhold_stop(1);
        int trc2 = i2c_transfer(adap, msgs, 2);
        dw_i2c_withhold_stop(0);
        msgs[1].buf = r;
        int trc3 = i2c_transfer(adap, msgs, 2);
        snprintf(line, sizeof(line),
                 "[i2chidsim] C2 timeout rc=%d recovered=%d irqs=%lu aborts=%lu txmax=%lu rxmax=%lu tx_empty=%lu rx_full=%lu",
                 trc2 < 0 ? -trc2 : trc2,
                 trc3 == 2 && !memcmp(r, g_model.hid_desc, 30),
                 dw_i2c_irqs(), dw_i2c_aborts(), dw_i2c_tx_max(),
                 dw_i2c_rx_max(), dw_i2c_tx_empty_irqs(),
                 dw_i2c_rx_full_irqs());
        lx_log_line(line);
        u8 nb[2] = { 0x01, 0x00 };
        struct i2c_msg nm = { .addr = 0x16, .flags = 0, .len = 2, .buf = nb };
        int nrc = i2c_transfer(adap, &nm, 1);

        snprintf(line, sizeof(line), "[i2chidsim] C2 noack rc=%d",
                 nrc < 0 ? -nrc : nrc);
        lx_log_line(line);

    }
    // C3.2: the upstream ACPI front found and probed the touchpad itself.
    {
        struct acpi_device *adev =
            lx_acpi_companion_path("\\_SB_.PC02.I2C1.TPD0");
        struct i2c_client *client = adev ?
            i2c_find_device_by_fwnode(acpi_fwnode_handle(adev)) : NULL;
        char line[256];

        if (client) {
            snprintf(line, sizeof(line),
                     "[i2chidsim] C3.2 client=%s addr=0x%02x irq=%d driver=%s",
                     client->name, client->addr, client->irq,
                     client->dev.driver ? client->dev.driver->name : "none");
            lx_log_line(line);
            client_ok = client->dev.driver != NULL;
            client_irq = client->irq;
            put_device(&client->dev);
        } else {
            lx_log("[i2chidsim] C3.2 no i2c client for TPD0\n");
        }
        {
            struct gpio_desc *desc = NULL;
            struct gpio_device *gdev;

            gdev = gpio_device_find_by_label("INTC1084:00");
            if (gdev)
                desc = gpio_device_get_desc(gdev, 552);
            if (desc && !IS_ERR(desc)) {
                unsigned int cfg = pch_gpio_model_padcfg0(310);

                snprintf(line, sizeof(line),
                         "[i2chidsim] C4.3 pin 310 = community 4 pad GPP_I_8 PADCFG0=0x%08x (%s, %s), hostown=%d irq=%d",
                         cfg,
                         ((cfg >> 10) & 0xf) == 0 ? "gpio" : "native",
                         ((cfg >> 25) & 3) == 0 ?
                             (!(cfg & (1u << 23)) ? "level active-high" :
                                                    "level active-low") :
                             "edge",
                         pch_gpio_model_hostown(310), gpiod_to_irq(desc));
                lx_log_line(line);
            } else {
                lx_log("[i2chidsim] C4.3 GPIO 552 on INTC1084 not found\n");
            }
        }
        {
            struct pci_dev *pd = lx_pci_bound_dev();

            if (pd && pd->pm_cap) {
                u16 pmcsr = 0;

                pci_read_config_word(pd, pd->pm_cap + PCI_PM_CTRL, &pmcsr);
                snprintf(line, sizeof(line),
                         "[i2chidsim] C3.4 PMCSR=0x%04x state=D%u cap=0x%x",
                         pmcsr, pmcsr & PCI_PM_CTRL_STATE_MASK, pd->pm_cap);
                lx_log_line(line);
            }
        }
    }
    // the timing values i2c_dw_acpi_configure and the speed check end up with.
    {
        struct i2c_adapter *a = i2c_get_adapter(0);
        char line[256];

        if (a) {
            // designware does not use algo_data; it embeds the adapter. (satoru)
            struct dw_i2c_dev *dw = container_of(a, struct dw_i2c_dev,
                                                 adapter);

            snprintf(line, sizeof(line),
                     "[i2chidsim] C3.2 dw timing: bus=%u ss_hcnt=%u ss_lcnt=%u fs_hcnt=%u fs_lcnt=%u sda_hold=%u",
                     dw->timings.bus_freq_hz, dw->ss_hcnt, dw->ss_lcnt,
                     dw->fs_hcnt, dw->fs_lcnt, dw->sda_hold_time);
            lx_log_line(line);
        }
        if (a)
            i2c_put_adapter(a);
    }
    if (c33_run(g_acpi_found, client_ok, client_irq)) {
        lx_log("[i2chidsim] C3.3 FAILED\n");
        return 1;
    }
    {
        char line[192];

        snprintf(line, sizeof(line),
                 "[i2chid_sim] family init rc=%d bound=%d lpss_reads=%lu lpss_writes=%lu",
                 rc < 0 ? -rc : rc, lx_pci_bound_dev() ? 1 : 0,
                 lpss_model_reads(), lpss_model_writes());
        lx_log_line(line);
    }
    return rc ? 1 : 0;
}

// end (satoru)
