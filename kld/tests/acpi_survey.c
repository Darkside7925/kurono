// acpi_survey: C6.0 measurement of the laptop's ACPI event side, blob mode.
// run on the host as `kld-i2chidsim --acpi-survey`: ACPICA comes up on the
// build-time table blob (the saved laptop tables), a recording EmbeddedControl
// handler is installed on the EC device (which runs its _REG), a notify handler
// sits on the root, and the methods of interest are evaluated one by one. for
// each one the opregion accesses it made and the Notify() calls it issued are
// printed. nothing here touches hardware: every access lands in a simulated
// page or in a 256 byte EC array. (satoru)

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/acpi.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_acpi.h"

void *lx_acpi_provider_map(u64 where, unsigned long len);   // lx_acpi_osl.c (satoru)

#define SV_MAXACC 160
#define EC0_PATH "\\_SB.PC00.LPCB.EC0"

struct sv_acc {
    const char *space;
    int write;
    u64 addr;
    u32 width;
    u32 count;
    u64 last;
};

static struct sv_acc g_acc[SV_MAXACC];
static int g_nacc;
static int g_acc_overflow;
static const char *g_label = "";
static u8 g_ec[256];
static unsigned int g_espi;     // value the LPC config dword 0xDC reads as (satoru)
static unsigned int g_notifies;

static void sv_line(const char *fmt, ...) __printf(1, 2);
static void sv_line(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lx_log_line(buf);
}

static void sv_trace(const char *space, int write, u64 addr, u32 width, u64 value)
{
    int i;

    for (i = 0; i < g_nacc; i++) {
        struct sv_acc *a = &g_acc[i];

        if (a->write == write && a->addr == addr && a->width == width &&
            !strcmp(a->space, space)) {
            a->count++;
            a->last = value;
            return;
        }
    }
    if (g_nacc >= SV_MAXACC) {
        g_acc_overflow++;
        return;
    }
    g_acc[g_nacc].space = space;
    g_acc[g_nacc].write = write;
    g_acc[g_nacc].addr = addr;
    g_acc[g_nacc].width = width;
    g_acc[g_nacc].count = 1;
    g_acc[g_nacc].last = value;
    g_nacc++;
}

static void sv_flush(const char *what)
{
    int i, nr = 0, nw = 0;

    for (i = 0; i < g_nacc; i++) {
        if (g_acc[i].write)
            nw++;
        else
            nr++;
    }
    sv_line("[survey] %s: %d distinct reads, %d distinct writes%s", what, nr,
            nw, g_acc_overflow ? " (table full, more not listed)" : "");
    for (i = 0; i < g_nacc; i++) {
        struct sv_acc *a = &g_acc[i];

        sv_line("[survey]   %s %-15s 0x%010llx w%-2u x%u val=0x%llx",
                a->write ? "WRITE" : "read ", a->space,
                (unsigned long long)a->addr, a->width, a->count,
                (unsigned long long)a->last);
    }
    g_nacc = 0;
    g_acc_overflow = 0;
}

// the recording EC space: a plain array, every access traced and every write
// also counted by the write census. (satoru)
static acpi_status sv_ec_handler(u32 function, acpi_physical_address address,
                                 u32 bit_width, u64 *value, void *hctx,
                                 void *rctx)
{
    u32 n = bit_width / 8;
    u32 i;
    u64 v = 0;

    (void)hctx;
    (void)rctx;
    if (address + n > sizeof(g_ec))
        return AE_BAD_ADDRESS;
    if (function == ACPI_WRITE) {
        sv_trace("EmbeddedControl", 1, address, bit_width, *value);
        lx_acpi_write_note("EmbeddedControl", address, bit_width);
        for (i = 0; i < n; i++)
            g_ec[address + i] = (u8)(*value >> (8 * i));
    } else {
        for (i = 0; i < n; i++)
            v |= (u64)g_ec[address + i] << (8 * i);
        *value = v;
        sv_trace("EmbeddedControl", 0, address, bit_width, v);
    }
    return AE_OK;
}

static void sv_notify(acpi_handle h, u32 value, void *ctx)
{
    struct acpi_buffer b = { ACPI_ALLOCATE_BUFFER, NULL };
    const char *name = "?";

    (void)ctx;
    if (ACPI_SUCCESS(acpi_get_name(h, ACPI_FULL_PATHNAME, &b)))
        name = b.pointer;
    sv_line("[survey]   NOTIFY %s value 0x%x (during %s)", name, value, g_label);
    g_notifies++;
    if (b.pointer)
        ACPI_FREE(b.pointer);
}

// the LPC function's config dword 0xDC bit 2 is the eSPI flag the EC's _GPE
// tests; everything else reads zero. (satoru)
static long sv_pci_read(unsigned int seg, unsigned int bus, unsigned int dev,
                        unsigned int fn, unsigned int reg, unsigned int width)
{
    (void)seg;
    if (bus == 0 && dev == 0x1f && fn == 0 && reg <= 0xDC && reg + width / 8 > 0xDC)
        return (long)((g_espi ? 0x4u : 0u) << (8 * (0xDC - reg)));
    return 0;
}

static void sv_obj(union acpi_object *o, int depth)
{
    unsigned int i;

    if (!o) {
        sv_line("[survey]   result: (none)");
        return;
    }
    switch (o->type) {
    case ACPI_TYPE_INTEGER:
        sv_line("[survey]   %*sinteger 0x%llx", depth * 2, "",
                (unsigned long long)o->integer.value);
        break;
    case ACPI_TYPE_STRING:
        sv_line("[survey]   %*sstring \"%s\"", depth * 2, "", o->string.pointer);
        break;
    case ACPI_TYPE_BUFFER:
        sv_line("[survey]   %*sbuffer of %u bytes", depth * 2, "", o->buffer.length);
        break;
    case ACPI_TYPE_PACKAGE:
        sv_line("[survey]   %*spackage of %u", depth * 2, "", o->package.count);
        if (depth < 1)
            for (i = 0; i < o->package.count && i < 21; i++)
                sv_obj(&o->package.elements[i], depth + 1);
        break;
    default:
        sv_line("[survey]   %*stype %u", depth * 2, "", o->type);
    }
}

static acpi_status sv_eval(const char *path, const char *label)
{
    acpi_handle h;
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status st;

    g_label = label;
    g_nacc = 0;
    g_acc_overflow = 0;
    st = acpi_get_handle(NULL, (char *)path, &h);
    if (ACPI_FAILURE(st)) {
        sv_line("[survey] %s: no such object %s (%s)", label, path,
                acpi_format_exception(st));
        return st;
    }
    sv_line("[survey] evaluate %s", label);
    st = acpi_evaluate_object(h, NULL, NULL, &out);
    sv_line("[survey]   status %s", acpi_format_exception(st));
    if (ACPI_SUCCESS(st) && out.pointer)
        sv_obj(out.pointer, 0);
    if (out.pointer)
        ACPI_FREE(out.pointer);
    acpi_os_wait_events_complete();
    msleep(20);
    sv_flush(label);
    return st;
}

// state injection into the simulated opregion pages: bits of one byte. (satoru)
static void sv_poke(u64 addr, u8 mask, u8 val)
{
    u8 *p = lx_acpi_provider_map(addr, 1);

    if (!p) {
        sv_line("[survey] poke 0x%llx: no page", (unsigned long long)addr);
        return;
    }
    *p = (u8)((*p & ~mask) | (val & mask));
}

static void sv_reg(const char *label)
{
    acpi_handle h;
    union acpi_object arg[2];
    struct acpi_object_list args = { 2, arg };
    acpi_status st;

    arg[0].type = ACPI_TYPE_INTEGER;
    arg[0].integer.value = 3;
    arg[1].type = ACPI_TYPE_INTEGER;
    arg[1].integer.value = 1;
    g_label = label;
    g_nacc = 0;
    if (ACPI_FAILURE(acpi_get_handle(NULL, EC0_PATH "._REG", &h)))
        return;
    sv_line("[survey] evaluate %s", label);
    st = acpi_evaluate_object(h, NULL, &args, NULL);
    sv_line("[survey]   status %s", acpi_format_exception(st));
    acpi_os_wait_events_complete();
    msleep(20);
    sv_flush(label);
}

static acpi_status sv_crs_cb(struct acpi_resource *r, void *ctx)
{
    (void)ctx;
    if (r->type == ACPI_RESOURCE_TYPE_IO)
        sv_line("[survey]   _CRS io 0x%x-0x%x len %u align %u",
                r->data.io.minimum, r->data.io.maximum,
                r->data.io.address_length, r->data.io.alignment);
    else if (r->type == ACPI_RESOURCE_TYPE_FIXED_IO)
        sv_line("[survey]   _CRS fixed io 0x%x len %u", r->data.fixed_io.address,
                r->data.fixed_io.address_length);
    else if (r->type == ACPI_RESOURCE_TYPE_IRQ ||
             r->type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ)
        sv_line("[survey]   _CRS irq resource type %u", r->type);
    else if (r->type != ACPI_RESOURCE_TYPE_END_TAG)
        sv_line("[survey]   _CRS resource type %u", r->type);
    return AE_OK;
}

// every _Qxx under the EC, in namespace order. (satoru)
#define SV_MAXQ 64
static char g_q[SV_MAXQ][5];
static int g_nq;

static acpi_status sv_q_cb(acpi_handle h, u32 level, void *ctx, void **ret)
{
    struct acpi_buffer b = { ACPI_ALLOCATE_BUFFER, NULL };

    (void)level;
    (void)ctx;
    (void)ret;
    if (ACPI_SUCCESS(acpi_get_name(h, ACPI_SINGLE_NAME, &b)) && b.pointer) {
        const char *n = b.pointer;

        if (n[0] == '_' && n[1] == 'Q' && g_nq < SV_MAXQ) {
            memcpy(g_q[g_nq], n, 4);
            g_q[g_nq][4] = 0;
            g_nq++;
        }
        ACPI_FREE(b.pointer);
    }
    return AE_OK;
}

static char g_gpe[96][5];
static int g_ngpe;

static acpi_status sv_gpe_cb(acpi_handle h, u32 level, void *ctx, void **ret)
{
    struct acpi_buffer b = { ACPI_ALLOCATE_BUFFER, NULL };

    (void)level;
    (void)ctx;
    (void)ret;
    if (ACPI_SUCCESS(acpi_get_name(h, ACPI_SINGLE_NAME, &b)) && b.pointer) {
        const char *n = b.pointer;

        if (n[0] == '_' && (n[1] == 'L' || n[1] == 'E') && g_ngpe < 96) {
            memcpy(g_gpe[g_ngpe], n, 4);
            g_gpe[g_ngpe][4] = 0;
            g_ngpe++;
        }
        ACPI_FREE(b.pointer);
    }
    return AE_OK;
}

static void sv_fadt(void)
{
    struct acpi_table_header *hdr = NULL;
    acpi_status st;

    sv_line("[survey] FADT: revision %u sci_irq %u smi_cmd 0x%x acpi_enable 0x%x acpi_disable 0x%x",
            acpi_gbl_FADT.header.revision, acpi_gbl_FADT.sci_interrupt,
            acpi_gbl_FADT.smi_command, acpi_gbl_FADT.acpi_enable,
            acpi_gbl_FADT.acpi_disable);
    sv_line("[survey] FADT: pm1a_evt 0x%llx len %u  pm1b_evt 0x%llx  pm1a_ctl 0x%llx len %u  pm1b_ctl 0x%llx  pm2_ctl 0x%llx len %u  pm_tmr 0x%llx len %u",
            (unsigned long long)acpi_gbl_FADT.xpm1a_event_block.address,
            acpi_gbl_FADT.pm1_event_length,
            (unsigned long long)acpi_gbl_FADT.xpm1b_event_block.address,
            (unsigned long long)acpi_gbl_FADT.xpm1a_control_block.address,
            acpi_gbl_FADT.pm1_control_length,
            (unsigned long long)acpi_gbl_FADT.xpm1b_control_block.address,
            (unsigned long long)acpi_gbl_FADT.xpm2_control_block.address,
            acpi_gbl_FADT.pm2_control_length,
            (unsigned long long)acpi_gbl_FADT.xpm_timer_block.address,
            acpi_gbl_FADT.pm_timer_length);
    sv_line("[survey] FADT: gpe0 0x%llx len %u  gpe1 0x%llx len %u base %u  flags 0x%x (hw_reduced %d, fixed power button %d, fixed sleep button %d)",
            (unsigned long long)acpi_gbl_FADT.xgpe0_block.address,
            acpi_gbl_FADT.gpe0_block_length,
            (unsigned long long)acpi_gbl_FADT.xgpe1_block.address,
            acpi_gbl_FADT.gpe1_block_length, acpi_gbl_FADT.gpe1_base,
            acpi_gbl_FADT.flags,
            !!(acpi_gbl_FADT.flags & ACPI_FADT_HW_REDUCED),
            !(acpi_gbl_FADT.flags & ACPI_FADT_POWER_BUTTON),
            !(acpi_gbl_FADT.flags & ACPI_FADT_SLEEP_BUTTON));
    st = acpi_get_table("ECDT", 1, &hdr);
    sv_line("[survey] ECDT: %s", ACPI_SUCCESS(st) ? "present" : acpi_format_exception(st));
}

static void sv_pass(acpi_handle ec, const char *tag)
{
    int i;
    char path[64], lab[48];

    sv_line("[survey] ===== pass %s =====", tag);
    sv_eval(EC0_PATH ".BAT0._STA", "BAT0._STA");
    sv_eval(EC0_PATH ".BAT0._BIF", "BAT0._BIF");
    sv_eval(EC0_PATH ".BAT0._BIX", "BAT0._BIX");
    sv_eval(EC0_PATH ".BAT0._BST", "BAT0._BST");
    sv_eval("\\_SB.ADP0._PSR", "ADP0._PSR");
    sv_eval("\\_SB.LID0._LID", "LID0._LID");

    g_nq = 0;
    acpi_walk_namespace(ACPI_TYPE_METHOD, ec, 1, sv_q_cb, NULL, NULL, NULL);
    sv_line("[survey] _Qxx methods under the EC: %d", g_nq);
    for (i = 0; i < g_nq; i++) {
        snprintf(path, sizeof(path), EC0_PATH ".%s", g_q[i]);
        snprintf(lab, sizeof(lab), "EC0.%s", g_q[i]);
        sv_eval(path, lab);
    }
}

int lx_acpi_survey(void)
{
    acpi_handle ec = NULL;
    acpi_status st;
    int i;

    lx_acpi_trace_hook = sv_trace;
    lx_acpi_pci_read_hook = sv_pci_read;
    sv_line("[survey] C6.0 start");
    sv_fadt();
    st = acpi_get_handle(NULL, EC0_PATH, &ec);
    if (ACPI_FAILURE(st)) {
        sv_line("[survey] no %s: %s", EC0_PATH, acpi_format_exception(st));
        return 1;
    }
    st = acpi_install_notify_handler(ACPI_ROOT_OBJECT, ACPI_ALL_NOTIFY, sv_notify, NULL);
    sv_line("[survey] notify handler: %s", acpi_format_exception(st));

    // installing the EC space handler runs the EC's _REG(3, 1). (satoru)
    g_label = "EC0._REG";
    g_nacc = 0;
    st = acpi_install_address_space_handler(ec, ACPI_ADR_SPACE_EC, sv_ec_handler, NULL, NULL);
    sv_line("[survey] install EC space handler (runs %s._REG): %s", EC0_PATH,
            acpi_format_exception(st));
    acpi_os_wait_events_complete();
    sv_flush("EC0._REG");

    sv_eval(EC0_PATH "._STA", "EC0._STA");
    for (g_espi = 0; g_espi < 2; g_espi++) {
        char lab[48];

        snprintf(lab, sizeof(lab), "EC0._GPE (LPC eSPI flag %u)", g_espi);
        sv_eval(EC0_PATH "._GPE", lab);
    }
    g_espi = 0;
    sv_line("[survey] EC0 _CRS");
    st = acpi_walk_resources(ec, "_CRS", sv_crs_cb, NULL);
    sv_line("[survey]   walk status %s", acpi_format_exception(st));

    sv_pass(ec, "A idle (all EC memory zero)");

    // pass B: battery present and charging on AC, lid open, performance mode, so
    // the branches that write more show up. SPMO=3, SPAC=1, CDSB=1, B1DC=0x1770,
    // B1ST=2 (charging), B1RC=0x400, B1FC=0x800, B1FV=15000, B1AC=900, ADPT=1,
    // LSTE=1, BA1P=1. (satoru)
    sv_poke(0xFE500420, 0x0f, 3);
    sv_poke(0xFE5002A7, 0x01, 1);
    sv_poke(0xFE50056D, 0x01, 1);
    sv_poke(0xFE5004CA, 0xff, 0x70); sv_poke(0xFE5004CB, 0xff, 0x17);
    sv_poke(0xFE5004C1, 0xff, 2);
    sv_poke(0xFE5004C2, 0xff, 0x00); sv_poke(0xFE5004C3, 0xff, 0x04);
    sv_poke(0xFE5004CC, 0xff, 0x00); sv_poke(0xFE5004CD, 0xff, 0x08);
    sv_poke(0xFE5004C6, 0xff, 0x98); sv_poke(0xFE5004C7, 0xff, 0x3a);
    sv_poke(0xFE5004D2, 0xff, 0x84); sv_poke(0xFE5004D3, 0xff, 0x03);
    sv_poke(0xFE5004A3, 0x80, 0x80);
    sv_poke(0xFE5004B8, 0x02, 0x02);
    sv_poke(0xFE5004BE, 0x01, 0x01);
    sv_reg("EC0._REG (pass B state)");
    sv_pass(ec, "B charging on AC, lid open, performance mode");

    st = acpi_get_handle(NULL, "\\_GPE", &ec);
    if (ACPI_SUCCESS(st))
        st = acpi_walk_namespace(ACPI_TYPE_METHOD, ec, 1, sv_gpe_cb, NULL, NULL, NULL);
    sv_line("[survey] _Lxx/_Exx methods under _GPE: %d", g_ngpe);
    for (i = 0; i < g_ngpe; i++)
        sv_line("[survey]   \\_GPE.%s", g_gpe[i]);
    sv_line("[survey] notifies seen: %u", g_notifies);
    lx_acpi_write_census();
    sv_line("[survey] C6.0 done");
    lx_acpi_trace_hook = NULL;
    lx_acpi_pci_read_hook = NULL;
    return 0;
}

// end (satoru)
