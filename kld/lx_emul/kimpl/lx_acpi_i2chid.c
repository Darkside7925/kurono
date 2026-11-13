// lx_acpi_i2chid: the S3a PNP0C50 enumerator, in-container.
//
// same walk and same fields as the standalone tool (kld/acpi/kld_acpi_i2chid.c):
// every device whose _HID or _CID is PNP0C50, its controller resolved from the
// I2cSerialBus resource source (_BBN/_SEG/_ADR -> PCI BDF), the GpioInt line,
// and the i2c-hid _DSM function 1 (descriptor register address). the difference
// is only the output path: lx_log, so it reaches the container's serial log.
// no laptop path is special-cased. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/acpi.h>

#include "accommon.h"
#include "acnamesp.h"

#include "lx_acpi.h"
#include "lx_kshim.h"

struct lx_ai_line {
    char b[640];
    unsigned int n;
};

static void lx_ai_puts(struct lx_ai_line *l, const char *s)
{
    unsigned int room;

    if (!s)
        return;
    room = (unsigned int)sizeof(l->b) - l->n - 1;
    if (!room)
        return;
    while (*s && room) {
        l->b[l->n++] = *s++;
        room--;
    }
    l->b[l->n] = 0;
}

static void lx_ai_printf(struct lx_ai_line *l, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(l->b + l->n, sizeof(l->b) - l->n, fmt, ap);
    va_end(ap);
    if (n > 0)
        l->n += (unsigned int)n < sizeof(l->b) - l->n ?
                (unsigned int)n : (unsigned int)(sizeof(l->b) - l->n - 1);
    l->b[l->n] = 0;
}

static void lx_ai_end(struct lx_ai_line *l)
{
    lx_log_line(l->b);
    l->n = 0;
    l->b[0] = 0;
}

static acpi_status lx_ai_eval_int(acpi_handle h, const char *m, u64 *out)
{
    union acpi_object o;
    struct acpi_buffer b = { sizeof(o), &o };
    acpi_status st = acpi_evaluate_object_typed(h, (char *)m, NULL, &b,
                                                ACPI_TYPE_INTEGER);

    if (ACPI_SUCCESS(st))
        *out = o.integer.value;
    return st;
}

static unsigned int lx_ai_speed_hz(u16 code)
{
    switch (code) {
    case 0: return 100000;
    case 1: return 400000;
    case 2: return 1000000;
    case 3: return 3400000;
    default: return 0;
    }
}

static const char *lx_ai_trigger(u8 t)
{
    return t == ACPI_LEVEL_SENSITIVE ? "level" : "edge";
}

static const char *lx_ai_polarity(u8 p)
{
    if (p == ACPI_ACTIVE_LOW)
        return "active-low";
    if (p == ACPI_ACTIVE_BOTH)
        return "active-both";
    return "active-high";
}

static const char *lx_ai_share(u8 s)
{
    return s == ACPI_SHARED ? "shared" : "exclusive";
}

// resolve the resource source string and report its _HID and PCI BDF. (satoru)
static void lx_ai_controller(struct lx_ai_line *l, const char *source)
{
    acpi_handle h = NULL;
    struct acpi_device_info *inf = NULL;
    u64 adr = 0, bbn = 0, seg = 0;
    int have_adr, have_bus = 0, have_seg = 0;
    acpi_handle cur;
    char hid[16] = "-";

    if (!source || !source[0]) {
        lx_ai_puts(l, " controller=-");
        return;
    }
    lx_ai_puts(l, " controller=");
    lx_ai_puts(l, source);
    if (ACPI_FAILURE(acpi_get_handle(NULL, (char *)source, &h))) {
        lx_ai_puts(l, " controller-hid=? pci=?");
        return;
    }
    if (ACPI_SUCCESS(acpi_get_object_info(h, &inf)) && inf &&
        (inf->valid & ACPI_VALID_HID) && inf->hardware_id.string) {
        unsigned int i;

        for (i = 0; i < sizeof(hid) - 1 && inf->hardware_id.string[i]; i++)
            hid[i] = inf->hardware_id.string[i];
        hid[i] = 0;
    }
    if (inf)
        ACPI_FREE(inf);
    have_adr = ACPI_SUCCESS(lx_ai_eval_int(h, "_ADR", &adr));
    for (cur = h; cur; ) {
        acpi_handle parent = NULL;

        if (ACPI_SUCCESS(lx_ai_eval_int(cur, "_BBN", &bbn))) {
            have_bus = 1;
            break;
        }
        if (ACPI_FAILURE(acpi_get_parent(cur, &parent)) || !parent)
            break;
        cur = parent;
    }
    for (cur = h; cur; ) {
        acpi_handle parent = NULL;

        if (ACPI_SUCCESS(lx_ai_eval_int(cur, "_SEG", &seg))) {
            have_seg = 1;
            break;
        }
        if (ACPI_FAILURE(acpi_get_parent(cur, &parent)) || !parent)
            break;
        cur = parent;
    }
    lx_ai_puts(l, " controller-hid=");
    lx_ai_puts(l, hid);
    if (have_adr && have_bus)
        lx_ai_printf(l,
                     " pci=%04x:%02x:%02x.%x adr=0x%llx bbn=0x%llx seg=0x%llx",
                     (unsigned)seg, (unsigned)bbn,
                     (unsigned)((adr >> 16) & 0xFFFF), (unsigned)(adr & 0xFFFF),
                     (unsigned long long)adr, (unsigned long long)bbn,
                     (unsigned long long)seg);
    else
        lx_ai_printf(l, " pci=? adr=0x%llx bbn=%s", (unsigned long long)adr,
                     have_bus ? "yes" : "no");
    (void)have_seg;
}

static acpi_status lx_ai_res(struct acpi_resource *r, void *ctx)
{
    struct lx_ai_line *l = ctx;

    (void)ctx;
    switch (r->type) {
    case ACPI_RESOURCE_TYPE_SERIAL_BUS:
        if (r->data.common_serial_bus.type == ACPI_RESOURCE_SERIAL_TYPE_I2C) {
            struct acpi_resource_i2c_serialbus *i = &r->data.i2c_serial_bus;

            lx_ai_printf(l, "  i2c addr=0x%02x speed=%u access=%d-bit",
                         i->slave_address,
                         i->connection_speed > 3 ? i->connection_speed :
                         lx_ai_speed_hz(i->connection_speed),
                         i->access_mode ? 10 : 7);
            lx_ai_controller(l, i->resource_source.string_ptr);
            lx_ai_end(l);
        } else {
            lx_ai_printf(l, "  serial-bus type=%u",
                         r->data.common_serial_bus.type);
            lx_ai_controller(l,
                             r->data.common_serial_bus.resource_source.string_ptr);
            lx_ai_end(l);
        }
        break;
    case ACPI_RESOURCE_TYPE_GPIO: {
        struct acpi_resource_gpio *g = &r->data.gpio;

        if (g->connection_type != ACPI_RESOURCE_GPIO_TYPE_INT)
            break;
        lx_ai_printf(l, "  gpio-int pin=%u",
                     g->pin_table_length ? g->pin_table[0] : 0);
        if (g->pin_table_length > 1)
            lx_ai_printf(l, "+%u", g->pin_table_length - 1);
        lx_ai_printf(l, " trigger=%s polarity=%s share=%s debounce=%u",
                     lx_ai_trigger(g->triggering),
                     lx_ai_polarity(g->polarity),
                     lx_ai_share(g->shareable), g->debounce_timeout);
        lx_ai_controller(l, g->resource_source.string_ptr);
        lx_ai_end(l);
        break;
    }
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
        struct acpi_resource_extended_irq *e = &r->data.extended_irq;

        lx_ai_printf(l, "  irq=%u",
                     e->interrupt_count ? e->interrupts[0] : 0);
        if (e->interrupt_count > 1)
            lx_ai_printf(l, "+%u", e->interrupt_count - 1);
        lx_ai_printf(l, " trigger=%s polarity=%s share=%s",
                     lx_ai_trigger(e->triggering),
                     lx_ai_polarity(e->polarity),
                     lx_ai_share(e->shareable));
        lx_ai_controller(l, e->resource_source.string_ptr);
        lx_ai_end(l);
        break;
    }
    case ACPI_RESOURCE_TYPE_IRQ: {
        struct acpi_resource_irq *i = &r->data.irq;

        lx_ai_printf(l, "  irq=%u", i->interrupt_count ? i->interrupts[0] : 0);
        if (i->interrupt_count > 1)
            lx_ai_printf(l, "+%u", i->interrupt_count - 1);
        lx_ai_printf(l, " trigger=%s polarity=%s share=%s",
                     lx_ai_trigger(i->triggering),
                     lx_ai_polarity(i->polarity),
                     lx_ai_share(i->shareable));
        lx_ai_end(l);
        break;
    }
    default:
        break;
    }
    return AE_OK;
}

static void lx_ai_dsm(struct lx_ai_line *l, const char *path)
{
    static const u8 uuid[16] = { 0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
                                 0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde };
    union acpi_object args[4];
    struct acpi_object_list al = { 4, args };
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    char m[512];
    int n;
    acpi_status st;

    args[0].type = ACPI_TYPE_BUFFER;
    args[0].buffer.length = 16;
    args[0].buffer.pointer = (u8 *)uuid;
    args[1].type = ACPI_TYPE_INTEGER;
    args[1].integer.value = 1;
    args[2].type = ACPI_TYPE_INTEGER;
    args[2].integer.value = 1;
    args[3].type = ACPI_TYPE_PACKAGE;
    args[3].package.count = 0;
    args[3].package.elements = NULL;
    n = snprintf(m, sizeof(m), "%s._DSM", path);
    (void)n;
    st = acpi_evaluate_object(NULL, m, &al, &out);
    lx_ai_puts(l, "  hid-descriptor=");
    if (ACPI_FAILURE(st)) {
        lx_ai_puts(l, "? (");
        lx_ai_puts(l, acpi_format_exception(st));
        lx_ai_puts(l, ")");
    } else if (out.pointer) {
        union acpi_object *o = out.pointer;

        if (o->type == ACPI_TYPE_INTEGER)
            lx_ai_printf(l, "0x%llx", (unsigned long long)o->integer.value);
        else if (o->type == ACPI_TYPE_BUFFER && o->buffer.length >= 4) {
            u32 v = (u32)o->buffer.pointer[0] |
                    ((u32)o->buffer.pointer[1] << 8) |
                    ((u32)o->buffer.pointer[2] << 16) |
                    ((u32)o->buffer.pointer[3] << 24);

            lx_ai_printf(l, "buffer 0x%x", v);
        } else
            lx_ai_printf(l, "type %u", o->type);
    } else
        lx_ai_puts(l, "empty");
    lx_ai_end(l);
    if (out.pointer)
        ACPI_FREE(out.pointer);
}

// exactly "PNP0C50", ascii case-insensitive; local so the container does not
// grow a libc string symbol the stub census would have to know about. (satoru)
static int lx_ai_hid_is_pnp0c50(const char *s)
{
    static const char id[] = "PNP0C50";

    if (!s)
        return 0;
    for (int i = 0; i < 7; i++) {
        char c = s[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != id[i])
            return 0;
    }
    return s[7] == 0;
}

static int lx_ai_is_pnp0c50(struct acpi_device_info *info)
{
    u32 i;

    if ((info->valid & ACPI_VALID_HID) && info->hardware_id.string &&
        lx_ai_hid_is_pnp0c50(info->hardware_id.string))
        return 1;
    if (info->valid & ACPI_VALID_CID)
        for (i = 0; i < info->compatible_id_list.count; i++)
            if (lx_ai_hid_is_pnp0c50(info->compatible_id_list.ids[i].string))
                return 1;
    return 0;
}

static acpi_status lx_ai_dev(acpi_handle h, u32 level, void *ctx, void **ret)
{
    int *found = ctx;
    struct acpi_device_info *info = NULL;
    struct acpi_buffer pb = { ACPI_ALLOCATE_BUFFER, NULL };
    struct lx_ai_line l = { .n = 0 };
    acpi_status st;

    (void)level;
    (void)ret;
    if (ACPI_FAILURE(acpi_get_object_info(h, &info)))
        return AE_OK;
    if (!lx_ai_is_pnp0c50(info)) {
        ACPI_FREE(info);
        return AE_OK;
    }
    if (ACPI_FAILURE(acpi_get_name(h, ACPI_FULL_PATHNAME, &pb))) {
        ACPI_FREE(info);
        return AE_OK;
    }
    (*found)++;
    lx_ai_puts(&l, "[kld-tp-acpi] device ");
    lx_ai_puts(&l, (char *)pb.pointer);
    lx_ai_puts(&l, " hid=");
    lx_ai_puts(&l, (info->valid & ACPI_VALID_HID && info->hardware_id.string) ?
               info->hardware_id.string : "-");
    if (info->valid & ACPI_VALID_CID) {
        u32 i;

        lx_ai_puts(&l, " cid=");
        for (i = 0; i < info->compatible_id_list.count; i++)
            lx_ai_printf(&l, "%s%s", i ? "," : "",
                         info->compatible_id_list.ids[i].string ?
                         info->compatible_id_list.ids[i].string : "?");
    }
    {
        u64 sta = 0x0F;

        if (ACPI_FAILURE(lx_ai_eval_int(h, "_STA", &sta)))
            sta = 0x0F;
        lx_ai_printf(&l, " sta=0x%llx", (unsigned long long)sta);
    }
    lx_ai_end(&l);
    st = acpi_walk_resources(h, METHOD_NAME__CRS, lx_ai_res, &l);
    if (ACPI_FAILURE(st)) {
        lx_ai_puts(&l, "  _CRS: ");
        lx_ai_puts(&l, acpi_format_exception(st));
        lx_ai_end(&l);
    }
    lx_ai_dsm(&l, (char *)pb.pointer);
    ACPI_FREE(pb.pointer);
    ACPI_FREE(info);
    return AE_OK;
}

int lx_acpi_i2chid_scan(void)
{
    int found = 0;

    acpi_get_devices(NULL, lx_ai_dev, &found, NULL);
    lx_log("[kld-tp-acpi] PNP0C50 devices: ");
    lx_log_u((unsigned long)found);
    lx_log("\n");
    return found;
}

// end (satoru)
