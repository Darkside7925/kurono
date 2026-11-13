// kld_acpi_i2chid: generic HID-over-I2C (PNP0C50) enumeration.
//
// Walk the ACPI namespace and print, for every device whose _HID or _CID is
// PNP0C50: the path, the I2C controller and its PCI address (from _ADR/_BBN/
// _SEG), the I2cSerialBus address/speed, the GpioInt or IRQ resource, and the
// HID descriptor register from _DSM UUID 3cdff6f7-4267-4555-ad05-b30a3d8938de
// function 1. no laptop path is special-cased; this is the same data Linux's
// i2c-hid-acpi and pinctrl/gpiolib consume. (satoru)
//
// end (satoru)

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "kld_acpi.h"

static acpi_status kld_ai_eval_int(acpi_handle h, const char *m, u64 *out)
{
    union acpi_object o;
    struct acpi_buffer b = { sizeof(o), &o };
    acpi_status st = acpi_evaluate_object_typed(h, (char *)m, NULL, &b,
                                                ACPI_TYPE_INTEGER);
    if (ACPI_SUCCESS(st))
        *out = o.integer.value;
    return st;
}

static unsigned int kld_ai_speed_hz(u16 code)
{
    switch (code) {
    case 0: return 100000;
    case 1: return 400000;
    case 2: return 1000000;
    case 3: return 3400000;
    default: return 0;
    }
}

static const char *kld_ai_trigger(u8 t)
{
    return t == ACPI_LEVEL_SENSITIVE ? "level" : "edge";
}

static const char *kld_ai_polarity(u8 p)
{
    if (p == ACPI_ACTIVE_LOW) return "active-low";
    if (p == ACPI_ACTIVE_BOTH) return "active-both";
    return "active-high";
}

static const char *kld_ai_share(u8 s)
{
    return s == ACPI_SHARED ? "shared" : "exclusive";
}

// resolve the resource source string and report its _HID and PCI BDF. the PCI
// bus number lives on the nearest ancestor with _BBN (the root bridge); the
// device/function are the controller's own _ADR. (satoru)
static void kld_ai_controller(const char *source)
{
    acpi_handle h = NULL;
    if (!source || !source[0]) {
        printf(" controller=-");
        return;
    }
    printf(" controller=%s", source);
    if (ACPI_FAILURE(acpi_get_handle(NULL, (char *)source, &h))) {
        printf(" controller-hid=? pci=?");
        return;
    }
    char hid[16] = "-";
    struct acpi_device_info *inf = NULL;
    if (ACPI_SUCCESS(acpi_get_object_info(h, &inf)) && inf &&
        (inf->valid & ACPI_VALID_HID) && inf->hardware_id.string)
        snprintf(hid, sizeof(hid), "%s", inf->hardware_id.string);
    if (inf)
        ACPI_FREE(inf);
    u64 adr = 0, bbn = 0, seg = 0;
    int have_adr = ACPI_SUCCESS(kld_ai_eval_int(h, "_ADR", &adr));
    int have_bus = 0;
    acpi_handle cur = h;
    for (int i = 0; i < 32 && cur; i++) {
        if (ACPI_SUCCESS(kld_ai_eval_int(cur, "_BBN", &bbn))) {
            have_bus = 1;
            break;
        }
        acpi_handle parent = NULL;
        if (ACPI_FAILURE(acpi_get_parent(cur, &parent)) || !parent)
            break;
        cur = parent;
    }
    // segment is optional; the nearest one wins. (satoru)
    for (cur = h; cur; ) {
        if (ACPI_SUCCESS(kld_ai_eval_int(cur, "_SEG", &seg)))
            break;
        acpi_handle parent = NULL;
        if (ACPI_FAILURE(acpi_get_parent(cur, &parent)) || !parent)
            break;
        cur = parent;
    }
    printf(" controller-hid=%s", hid);
    if (have_adr && have_bus)
        printf(" pci=%04x:%02x:%02x.%x adr=0x%llx bbn=0x%llx seg=0x%llx",
               (unsigned)seg, (unsigned)bbn,
               (unsigned)((adr >> 16) & 0xFFFF), (unsigned)(adr & 0xFFFF),
               (unsigned long long)adr, (unsigned long long)bbn,
               (unsigned long long)seg);
    else
        printf(" pci=? adr=0x%llx bbn=%s", (unsigned long long)adr,
               have_bus ? "yes" : "no");
}

struct kld_ai_ctx {
    const char *path;
};

static acpi_status kld_ai_res(struct acpi_resource *r, void *ctx)
{
    (void)ctx;
    switch (r->type) {
    case ACPI_RESOURCE_TYPE_SERIAL_BUS:
        if (r->data.common_serial_bus.type == ACPI_RESOURCE_SERIAL_TYPE_I2C) {
            struct acpi_resource_i2c_serialbus *i = &r->data.i2c_serial_bus;
            printf("  i2c addr=0x%02x speed=%u access=%d-bit",
                   i->slave_address,
                   i->connection_speed > 3 ? i->connection_speed :
                   kld_ai_speed_hz(i->connection_speed),
                   i->access_mode ? 10 : 7);
            kld_ai_controller(i->resource_source.string_ptr);
            printf("\n");
        } else {
            printf("  serial-bus type=%u", r->data.common_serial_bus.type);
            kld_ai_controller(r->data.common_serial_bus.resource_source.string_ptr);
            printf("\n");
        }
        break;
    case ACPI_RESOURCE_TYPE_GPIO: {
        struct acpi_resource_gpio *g = &r->data.gpio;
        if (g->connection_type != ACPI_RESOURCE_GPIO_TYPE_INT)
            break;
        printf("  gpio-int pin=%u", g->pin_table_length ? g->pin_table[0] : 0);
        if (g->pin_table_length > 1)
            printf("+%u", g->pin_table_length - 1);
        printf(" trigger=%s polarity=%s share=%s debounce=%u",
               kld_ai_trigger(g->triggering), kld_ai_polarity(g->polarity),
               kld_ai_share(g->shareable), g->debounce_timeout);
        kld_ai_controller(g->resource_source.string_ptr);
        printf("\n");
        break;
    }
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
        struct acpi_resource_extended_irq *e = &r->data.extended_irq;
        printf("  irq=%u", e->interrupt_count ? e->interrupts[0] : 0);
        if (e->interrupt_count > 1)
            printf("+%u", e->interrupt_count - 1);
        printf(" trigger=%s polarity=%s share=%s", kld_ai_trigger(e->triggering),
               kld_ai_polarity(e->polarity), kld_ai_share(e->shareable));
        kld_ai_controller(e->resource_source.string_ptr);
        printf("\n");
        break;
    }
    case ACPI_RESOURCE_TYPE_IRQ: {
        struct acpi_resource_irq *i = &r->data.irq;
        printf("  irq=%u", i->interrupt_count ? i->interrupts[0] : 0);
        if (i->interrupt_count > 1)
            printf("+%u", i->interrupt_count - 1);
        printf(" trigger=%s polarity=%s share=%s\n",
               kld_ai_trigger(i->triggering), kld_ai_polarity(i->polarity),
               kld_ai_share(i->shareable));
        break;
    }
    default:
        break;
    }
    return AE_OK;
}

static void kld_ai_dsm(const char *path)
{
    static const u8 uuid[16] = { 0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
                                    0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde };
    union acpi_object args[4];
    struct acpi_object_list al = { 4, args };
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
    char m[512];
    snprintf(m, sizeof(m), "%s._DSM", path);
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status st = acpi_evaluate_object(NULL, m, &al, &out);
    printf("  hid-descriptor=");
    if (ACPI_FAILURE(st)) {
        printf("? (%s)\n", acpi_format_exception(st));
    } else if (out.pointer) {
        union acpi_object *o = out.pointer;
        if (o->type == ACPI_TYPE_INTEGER)
            printf("0x%llx\n", (unsigned long long)o->integer.value);
        else if (o->type == ACPI_TYPE_BUFFER && o->buffer.length >= 4) {
            u32 v = (u32)o->buffer.pointer[0] | ((u32)o->buffer.pointer[1] << 8) |
                    ((u32)o->buffer.pointer[2] << 16) |
                    ((u32)o->buffer.pointer[3] << 24);
            printf("buffer 0x%x\n", v);
        } else
            printf("type %u\n", o->type);
    } else
        printf("empty\n");
    if (out.pointer)
        ACPI_FREE(out.pointer);
}

static int kld_ai_is_pnp0c50(struct acpi_device_info *info)
{
    if ((info->valid & ACPI_VALID_HID) && info->hardware_id.string &&
        !strcasecmp(info->hardware_id.string, "PNP0C50"))
        return 1;
    if (info->valid & ACPI_VALID_CID)
        for (u32 i = 0; i < info->compatible_id_list.count; i++)
            if (info->compatible_id_list.ids[i].string &&
                !strcasecmp(info->compatible_id_list.ids[i].string, "PNP0C50"))
                return 1;
    return 0;
}

static acpi_status kld_ai_dev(acpi_handle h, u32 level, void *ctx, void **ret)
{
    (void)level; (void)ret;
    int *found = ctx;
    struct acpi_device_info *info = NULL;
    struct acpi_buffer pb = { ACPI_ALLOCATE_BUFFER, NULL };
    if (ACPI_FAILURE(acpi_get_object_info(h, &info)))
        return AE_OK;
    if (!kld_ai_is_pnp0c50(info)) {
        ACPI_FREE(info);
        return AE_OK;
    }
    if (ACPI_FAILURE(acpi_get_name(h, ACPI_FULL_PATHNAME, &pb))) {
        ACPI_FREE(info);
        return AE_OK;
    }
    u64 sta = 0x0F;
    if (ACPI_FAILURE(kld_ai_eval_int(h, "_STA", &sta)))
        sta = 0x0F;
    (*found)++;
    printf("[kld-tp-acpi] device %s hid=%s", (char *)pb.pointer,
           (info->valid & ACPI_VALID_HID && info->hardware_id.string) ?
           info->hardware_id.string : "-");
    if (info->valid & ACPI_VALID_CID) {
        printf(" cid=");
        for (u32 i = 0; i < info->compatible_id_list.count; i++)
            printf("%s%s", i ? "," : "",
                   info->compatible_id_list.ids[i].string ?
                   info->compatible_id_list.ids[i].string : "?");
    }
    printf(" sta=0x%llx\n", (unsigned long long)sta);
    acpi_status st = acpi_walk_resources(h, METHOD_NAME__CRS, kld_ai_res, NULL);
    if (ACPI_FAILURE(st))
        printf("  _CRS: %s\n", acpi_format_exception(st));
    kld_ai_dsm((char *)pb.pointer);
    ACPI_FREE(pb.pointer);
    ACPI_FREE(info);
    return AE_OK;
}

int kld_acpi_hid_i2c_scan(void)
{
    int found = 0;
    acpi_get_devices(NULL, kld_ai_dev, &found, NULL);
    printf("[kld-tp-acpi] PNP0C50 devices: %d\n", found);
    return found;
}

// end (satoru)
