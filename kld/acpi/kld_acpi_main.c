// kld-acpi: the KLD ACPI container front end.
//
// Loads ACPI tables, brings the unmodified ACPICA interpreter up, and answers
// the questions Linux drivers ask ACPI: which devices exist (_HID/_CID/_ADR/
// _STA), what resources they consume (_CRS: I2cSerialBus, GpioInt, IRQ, MMIO),
// and device-specific methods (_DSM, e.g. the i2c-hid descriptor address).
//
//   kld-acpi --tables DIR [--devices] [--crs PATH]... [--eval PATH]...
//            [--i2chid PATH]...
//
// DIR holds acpidump/acpixtract output (one raw table per *.dat file). Inside
// Kurono the same binary runs with --broker (tables + hardware via the kernel
// broker, kld_acpi_broker.c). (satoru)
//
// end (satoru)

#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "kld_acpi.h"
#include <unistd.h>

// ── file mode: synthesize RSDP + XSDT over the dumped tables ────────────────
#define KLD_FAKE_PHYS_BASE 0x10000000ull

static u8 kld_cksum(const void *p, size_t n)
{
    const u8 *b = p;
    u8 s = 0;
    while (n--) s += *b++;
    return s;
}

struct kld_tbl { struct acpi_table_header *t; acpi_physical_address pa; };
static struct kld_tbl kld_tbls[128];
static int kld_ntbl;

static int kld_load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    acpi_physical_address next = KLD_FAKE_PHYS_BASE + 0x10000;
    if (!d) { fprintf(stderr, "[kld-acpi] cannot open %s\n", dir); return -1; }
    while ((e = readdir(d)) && kld_ntbl < 128) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".dat")) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < (long)sizeof(struct acpi_table_header)) { fclose(f); continue; }
        struct acpi_table_header *t = calloc(1, (size_t)sz + 4096);
        if (fread(t, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(t); continue; }
        fclose(f);
        if (t->length > (u32)sz) {   // FACS has no checksum, length is authoritative (satoru)
            fprintf(stderr, "[kld-acpi] %s: truncated\n", e->d_name);
            free(t);
            continue;
        }
        kld_tbls[kld_ntbl].t = t;
        kld_tbls[kld_ntbl].pa = next;
        kld_phys_add(next, t, t->length);
        next += (t->length + 0xFFFF) & ~0xFFFFull;
        kld_ntbl++;
    }
    closedir(d);
    return kld_ntbl;
}

static struct kld_tbl *kld_find(const char *sig)
{
    for (int i = 0; i < kld_ntbl; i++)
        if (!memcmp(kld_tbls[i].t->signature, sig, 4)) return &kld_tbls[i];
    return NULL;
}

static int kld_build_root(void)
{
    struct kld_tbl *facp = kld_find("FACP"), *dsdt = kld_find("DSDT"), *facs = kld_find("FACS");
    if (!facp || !dsdt) {
        fprintf(stderr, "[kld-acpi] need at least FACP and DSDT\n");
        return -1;
    }
    // point the FADT at our copies of the DSDT/FACS and re-checksum it. (satoru)
    struct acpi_table_fadt *fadt = (struct acpi_table_fadt *)facp->t;
    fadt->dsdt = (u32)dsdt->pa;
    if (fadt->header.length >= ACPI_FADT_OFFSET(Xdsdt) + 8) fadt->Xdsdt = dsdt->pa;
    if (facs) {
        fadt->facs = (u32)facs->pa;
        if (fadt->header.length >= ACPI_FADT_OFFSET(Xfacs) + 8) fadt->Xfacs = facs->pa;
    } else {
        fadt->facs = 0;
        if (fadt->header.length >= ACPI_FADT_OFFSET(Xfacs) + 8) fadt->Xfacs = 0;
    }
    fadt->header.checksum = 0;
    fadt->header.checksum = (u8)(0 - kld_cksum(fadt, fadt->header.length));

    // XSDT: every table except DSDT and FACS (those are reached via the FADT). (satoru)
    int n = 0;
    for (int i = 0; i < kld_ntbl; i++)
        if (memcmp(kld_tbls[i].t->signature, "DSDT", 4) && memcmp(kld_tbls[i].t->signature, "FACS", 4)) n++;
    u32 xl = (u32)(sizeof(struct acpi_table_header) + 8u * (unsigned)n);
    struct acpi_table_xsdt *x = calloc(1, xl);
    memcpy(x->header.signature, "XSDT", 4);
    x->header.length = xl;
    x->header.revision = 1;
    memcpy(x->header.oem_id, "KURONO", 6);
    memcpy(x->header.oem_table_id, "KLDACPI ", 8);
    int k = 0;
    for (int i = 0; i < kld_ntbl; i++)
        if (memcmp(kld_tbls[i].t->signature, "DSDT", 4) && memcmp(kld_tbls[i].t->signature, "FACS", 4))
            x->table_offset_entry[k++] = kld_tbls[i].pa;
    x->header.checksum = (u8)(0 - kld_cksum(x, xl));
    acpi_physical_address xpa = KLD_FAKE_PHYS_BASE + 0x1000;
    kld_phys_add(xpa, x, xl);

    struct acpi_table_rsdp *r = calloc(1, sizeof(*r));
    memcpy(r->signature, ACPI_SIG_RSDP, 8);
    memcpy(r->oem_id, "KURONO", 6);
    r->revision = 2;
    r->length = sizeof(*r);
    r->xsdt_physical_address = xpa;
    r->checksum = (u8)(0 - kld_cksum(r, ACPI_RSDP_CHECKSUM_LENGTH));
    r->extended_checksum = (u8)(0 - kld_cksum(r, ACPI_RSDP_XCHECKSUM_LENGTH));
    kld_phys_add(KLD_FAKE_PHYS_BASE, r, sizeof(*r));
    kld_osl_rsdp = KLD_FAKE_PHYS_BASE;
    fprintf(stderr, "[kld-acpi] %d tables loaded from files, xsdt with %d entries\n", kld_ntbl, n);
    return 0;
}

// _STA as an integer (acpi_evaluate_integer is a linux drivers/acpi helper,
// not part of ACPICA, so it is spelled out here). (satoru)
static acpi_status kld_eval_int(acpi_handle h, const char *m, u64 *out)
{
    union acpi_object o;
    struct acpi_buffer b = { sizeof(o), &o };
    acpi_status st = acpi_evaluate_object_typed(h, (char *)m, NULL, &b, ACPI_TYPE_INTEGER);
    if (ACPI_SUCCESS(st))
        *out = o.integer.value;
    return st;
}

// ── reporting ───────────────────────────────────────────────────────────────
static int kld_ndev, kld_ndev_present;

static acpi_status kld_dev_cb(acpi_handle h, u32 level, void *ctx, void **ret)
{
    (void)level; (void)ret;
    int verbose = ctx != NULL;
    struct acpi_device_info *info = NULL;
    struct acpi_buffer path = { ACPI_ALLOCATE_BUFFER, NULL };
    kld_ndev++;
    if (ACPI_FAILURE(acpi_get_name(h, ACPI_FULL_PATHNAME, &path))) return AE_OK;
    if (ACPI_SUCCESS(acpi_get_object_info(h, &info))) {
        u64 sta = ACPI_UINT32_MAX;
        // _STA: absent method means "present" per the spec. (satoru)
        if (ACPI_FAILURE(kld_eval_int(h, "_STA", &sta))) sta = 0x0F;
        if (sta & ACPI_STA_DEVICE_PRESENT) kld_ndev_present++;
        if (verbose && (info->valid & ACPI_VALID_HID)) {
            printf("dev %-40s hid %-10s sta 0x%02llx", (char *)path.pointer,
                   info->hardware_id.string, (unsigned long long)sta);
            if (info->valid & ACPI_VALID_CID)
                for (u32 i = 0; i < info->compatible_id_list.count; i++)
                    printf(" cid %s", info->compatible_id_list.ids[i].string);
            if (info->valid & ACPI_VALID_ADR) printf(" adr 0x%llx", (unsigned long long)info->address);
            printf("\n");
        }
        ACPI_FREE(info);
    }
    ACPI_FREE(path.pointer);
    return AE_OK;
}

static acpi_status kld_res_cb(struct acpi_resource *r, void *ctx)
{
    (void)ctx;
    switch (r->type) {
    case ACPI_RESOURCE_TYPE_SERIAL_BUS:
        if (r->data.common_serial_bus.type == ACPI_RESOURCE_SERIAL_TYPE_I2C) {
            struct acpi_resource_i2c_serialbus *i = &r->data.i2c_serial_bus;
            printf("  I2cSerialBus addr 0x%02x speed %u hz %s-bit controller %s\n",
                   i->slave_address, i->connection_speed, i->access_mode ? "10" : "7",
                   i->resource_source.string_ptr ? i->resource_source.string_ptr : "?");
        } else {
            printf("  SerialBus type %u\n", r->data.common_serial_bus.type);
        }
        break;
    case ACPI_RESOURCE_TYPE_GPIO: {
        struct acpi_resource_gpio *g = &r->data.gpio;
        printf("  Gpio%s pin %u %s %s controller %s\n",
               g->connection_type == ACPI_RESOURCE_GPIO_TYPE_INT ? "Int" : "Io",
               g->pin_table_length ? g->pin_table[0] : 0xFFFF,
               g->triggering == ACPI_LEVEL_SENSITIVE ? "level" : "edge",
               g->polarity == ACPI_ACTIVE_LOW ? "active-low" : (g->polarity == ACPI_ACTIVE_BOTH ? "both" : "active-high"),
               g->resource_source.string_ptr ? g->resource_source.string_ptr : "?");
        break;
    }
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
        printf("  Interrupt %u %s %s (count %u)\n", r->data.extended_irq.interrupts[0],
               r->data.extended_irq.triggering == ACPI_LEVEL_SENSITIVE ? "level" : "edge",
               r->data.extended_irq.polarity == ACPI_ACTIVE_LOW ? "active-low" : "active-high",
               r->data.extended_irq.interrupt_count);
        break;
    case ACPI_RESOURCE_TYPE_IRQ:
        printf("  IRQ %u\n", r->data.irq.interrupts[0]);
        break;
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        printf("  Memory32Fixed 0x%08x len 0x%x\n", r->data.fixed_memory32.address, r->data.fixed_memory32.address_length);
        break;
    case ACPI_RESOURCE_TYPE_ADDRESS64:
        printf("  QWordSpace type %u 0x%llx-0x%llx\n", r->data.address64.resource_type,
               (unsigned long long)r->data.address64.address.minimum,
               (unsigned long long)r->data.address64.address.maximum);
        break;
    case ACPI_RESOURCE_TYPE_IO:
        printf("  IO 0x%04x len %u\n", r->data.io.minimum, r->data.io.address_length);
        break;
    case ACPI_RESOURCE_TYPE_END_TAG:
        break;
    default:
        printf("  resource type %u\n", r->type);
    }
    return AE_OK;
}

static void kld_print_obj(const union acpi_object *o, int depth)
{
    switch (o->type) {
    case ACPI_TYPE_INTEGER: printf("Integer 0x%llx", (unsigned long long)o->integer.value); break;
    case ACPI_TYPE_STRING:  printf("String \"%s\"", o->string.pointer); break;
    case ACPI_TYPE_BUFFER:
        printf("Buffer[%u]", o->buffer.length);
        for (u32 i = 0; i < o->buffer.length && i < 16; i++) printf(" %02x", o->buffer.pointer[i]);
        break;
    case ACPI_TYPE_PACKAGE:
        printf("Package[%u] {", o->package.count);
        for (u32 i = 0; i < o->package.count && i < 8 && depth < 3; i++) {
            printf(i ? ", " : " ");
            kld_print_obj(&o->package.elements[i], depth + 1);
        }
        printf(" }");
        break;
    case ACPI_TYPE_LOCAL_REFERENCE: printf("Reference"); break;
    default: printf("type %u", o->type);
    }
}

static void kld_eval(const char *path)
{
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status st = acpi_evaluate_object(NULL, (char *)path, NULL, &out);
    printf("eval %s: %s", path, acpi_format_exception(st));
    if (ACPI_SUCCESS(st) && out.pointer) {
        printf(" -> ");
        kld_print_obj(out.pointer, 0);
    }
    printf("\n");
    if (out.pointer) ACPI_FREE(out.pointer);
}

static void kld_crs(const char *path)
{
    acpi_handle h;
    if (ACPI_FAILURE(acpi_get_handle(NULL, (char *)path, &h))) { printf("crs %s: no such object\n", path); return; }
    printf("crs %s:\n", path);
    acpi_status st = acpi_walk_resources(h, METHOD_NAME__CRS, kld_res_cb, NULL);
    if (ACPI_FAILURE(st)) printf("  _CRS: %s\n", acpi_format_exception(st));
}

// i2c-hid: _DSM(3cdff6f7-4267-4555-ad05-b30a3d8938de, rev 1, func 1) returns the
// HID descriptor register, exactly what drivers/hid/i2c-hid/i2c-hid-acpi.c asks.
// (satoru)
static void kld_i2chid(const char *path)
{
    static const u8 uuid[16] = { 0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
                                    0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde };
    union acpi_object args[4];
    struct acpi_object_list al = { 4, args };
    args[0].type = ACPI_TYPE_BUFFER;  args[0].buffer.length = 16; args[0].buffer.pointer = (u8 *)uuid;
    args[1].type = ACPI_TYPE_INTEGER; args[1].integer.value = 1;
    args[2].type = ACPI_TYPE_INTEGER; args[2].integer.value = 1;
    args[3].type = ACPI_TYPE_PACKAGE; args[3].package.count = 0; args[3].package.elements = NULL;
    char m[256];
    snprintf(m, sizeof(m), "%s._DSM", path);
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status st = acpi_evaluate_object(NULL, m, &al, &out);
    printf("i2c-hid %s: _DSM(hid descriptor) %s", path, acpi_format_exception(st));
    if (ACPI_SUCCESS(st) && out.pointer) {
        printf(" -> ");
        kld_print_obj(out.pointer, 0);
    }
    printf("\n");
    if (out.pointer) ACPI_FREE(out.pointer);
}

// ── NVS seeding (file mode) ─────────────────────────────────────────────────
// firmware fills its NVS area (GNVS) before the OS runs, and module-level AML
// branches on it ("If (PCHA != Zero) { Device (PC02) ... }"). simulated memory
// reads as zero, which hides whole subtrees. --nvs NAME=VALUE seeds a named NVS
// field with a value the operator knows from the real machine (for this laptop:
// facts Linux observed, e.g. PC02 exists, TPD0 is ELAN06FA). pass 1 asks
// ACPICA's own field object where NAME lives; the tool then re-executes itself
// with --poke ADDR:BIT:LEN=VALUE applied to simulated memory BEFORE any table is
// loaded. every seed is printed - nothing is applied silently. (satoru)
struct kld_poke { u64 addr; u32 bit, len; u64 val; };
static struct kld_poke kld_pokes[64];
static int kld_npokes;

static void kld_apply_pokes(void)
{
    for (int i = 0; i < kld_npokes; i++) {
        struct kld_poke *p = &kld_pokes[i];
        for (u32 b = 0; b < p->len; b++) {
            u64 bit = p->bit + b;
            u8 *byte = acpi_os_map_memory(p->addr + bit / 8, 1);
            if (!byte) continue;
            if ((p->val >> b) & 1) *byte |= (u8)(1u << (bit % 8));
            else                   *byte &= (u8)~(1u << (bit % 8));
        }
        printf("[kld-acpi] seeded NVS bits @0x%llx bit %u len %u = 0x%llx\n",
               (unsigned long long)p->addr, p->bit, p->len, (unsigned long long)p->val);
    }
}

static int kld_locate_field(const char *name, struct kld_poke *out)
{
    acpi_handle h;
    char path[64];
    snprintf(path, sizeof(path), "\\%s", name);
    if (ACPI_FAILURE(acpi_get_handle(NULL, path, &h))) return -1;
    union acpi_operand_object *o = acpi_ns_get_attached_object((struct acpi_namespace_node *)h);
    if (!o || o->common.type != ACPI_TYPE_LOCAL_REGION_FIELD) return -2;
    union acpi_operand_object *r = o->field.region_obj;
    if (!r || r->region.space_id != ACPI_ADR_SPACE_SYSTEM_MEMORY) return -3;
    out->addr = r->region.address + o->common_field.base_byte_offset;
    out->bit = o->common_field.start_field_bit_offset;
    out->len = o->common_field.bit_length;
    return 0;
}

int kld_acpi_broker_init(void) __attribute__((weak));

// the broker refuses writes to registers the kernel owns but returns the written
// value on the next read, so acpica's write-then-verify pattern works while the
// kernel's pic/pit/cmos state and a device's bars are untouched. this self test
// is the C5.4 proof for that contract. (satoru)
extern acpi_status acpi_os_read_port(acpi_io_address addr, u32 *value, u32 width);
extern acpi_status acpi_os_write_port(acpi_io_address addr, u32 value, u32 width);
extern acpi_status acpi_os_read_pci_configuration(struct acpi_pci_id *id, u32 reg,
                                                  u64 *value, u32 width);
extern acpi_status acpi_os_write_pci_configuration(struct acpi_pci_id *id, u32 reg,
                                                   u64 value, u32 width);

static int kld_shadow_self_test(void)
{
    u32 pv = 0;
    acpi_os_write_port(0x70, 0xA5, 8);
    acpi_os_read_port(0x70, &pv, 8);
    int okp = (pv & 0xFF) == 0xA5;

    struct acpi_pci_id id;
    memset(&id, 0, sizeof(id));
    id.device = 2;
    u64 cv = 0;
    acpi_os_write_pci_configuration(&id, 0x10, 0x11223344, 32);
    acpi_os_read_pci_configuration(&id, 0x10, &cv, 32);
    int okc = cv == 0x11223344;

    fprintf(stderr, "[kld-acpi-shadow] cmos port 0x70 write/read: %s (0x%x)\n",
            okp ? "PASS" : "FAIL", pv & 0xFF);
    fprintf(stderr, "[kld-acpi-shadow] pci 0:2.0 bar0 write/read: %s (0x%llx)\n",
            okc ? "PASS" : "FAIL", (unsigned long long)cv);
    return okp && okc ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    int devices = 0, hid_i2c = 0, shadowtest = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tables") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--broker")) kld_osl_broker_mode = 1;
        else if (!strcmp(argv[i], "--quiet")) kld_acpi_quiet = 1;
        else if (!strcmp(argv[i], "--shadowtest")) shadowtest = 1;
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc && kld_npokes < 64) {
            struct kld_poke *p = &kld_pokes[kld_npokes];
            if (sscanf(argv[++i], "%llx:%u:%u=%llx", (unsigned long long *)&p->addr, &p->bit, &p->len,
                       (unsigned long long *)&p->val) == 4) kld_npokes++;
        }
    }
    if (kld_osl_broker_mode) {
        // inside Kurono only fd 2 reaches the serial console. (satoru)
        dup2(2, 1);
        if (!kld_acpi_broker_init || kld_acpi_broker_init() != 0) {
            fprintf(stderr, "[kld-acpi] broker backend unavailable\n");
            return 2;
        }
        if (shadowtest && kld_shadow_self_test() != 0)
            fprintf(stderr, "[kld-acpi-shadow] SELF TEST FAILED\n");
    } else {
        if (!dir) { fprintf(stderr, "usage: kld-acpi --tables DIR | --broker ...\n"); return 2; }
        if (kld_load_dir(dir) <= 0 || kld_build_root()) return 2;
        kld_apply_pokes();
    }

    acpi_status st;
    if (ACPI_FAILURE(st = acpi_initialize_subsystem())) { fprintf(stderr, "init subsystem: %s\n", acpi_format_exception(st)); return 3; }
    if (ACPI_FAILURE(st = acpi_initialize_tables(NULL, 32, FALSE))) { fprintf(stderr, "init tables: %s\n", acpi_format_exception(st)); return 3; }
    if (ACPI_FAILURE(st = acpi_load_tables())) { fprintf(stderr, "load tables: %s\n", acpi_format_exception(st)); return 3; }
    // no hardware takeover: the kernel stays in charge of the sci, the pm1
    // registers and the switch into acpi mode. (satoru)
    u32 fl = ACPI_NO_ACPI_ENABLE | ACPI_NO_HARDWARE_INIT;
    if (!kld_osl_broker_mode) fl |= ACPI_NO_EVENT_INIT | ACPI_NO_HANDLER_INIT;
    if (ACPI_FAILURE(st = acpi_enable_subsystem(fl))) { fprintf(stderr, "enable: %s\n", acpi_format_exception(st)); return 3; }
    if (ACPI_FAILURE(st = acpi_initialize_objects(ACPI_FULL_INITIALIZATION))) { fprintf(stderr, "init objects: %s\n", acpi_format_exception(st)); return 3; }
    printf("[kld-acpi] ACPICA %08x namespace initialized (%s mode)\n", ACPI_CA_VERSION,
           kld_osl_broker_mode ? "broker" : "file");

    // pass 1 of --nvs: resolve names, re-exec with pokes. (satoru)
    if (!kld_npokes) {
        char *nav[256];
        char pk[64][64];
        int na = 0, np = 0, want = 0;
        for (int i = 0; i < argc && na < 200; i++) {
            if (!strcmp(argv[i], "--nvs") && i + 1 < argc) {
                char nm[16]; unsigned long long v;
                want++;
                if (sscanf(argv[++i], "%15[^=]=%llx", nm, &v) == 2) {
                    struct kld_poke p;
                    int rc = kld_locate_field(nm, &p);
                    if (rc) { printf("[kld-acpi] --nvs %s: not a SystemMemory field (%d)\n", nm, rc); continue; }
                    printf("[kld-acpi] NVS field %s is @0x%llx bit %u len %u\n", nm,
                           (unsigned long long)p.addr, p.bit, p.len);
                    snprintf(pk[np], sizeof(pk[np]), "%llx:%u:%u=%llx", (unsigned long long)p.addr, p.bit, p.len, v);
                    nav[na++] = "--poke";
                    nav[na++] = pk[np++];
                }
                continue;
            }
            nav[na++] = argv[i];
        }
        if (want) {
            nav[na] = NULL;
            fflush(stdout);
            execv("/proc/self/exe", nav);
            perror("execv");
            return 4;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--devices")) devices = 1;
        else if (!strcmp(argv[i], "--hid-i2c")) hid_i2c = 1;
        else if (!strcmp(argv[i], "--eval") && i + 1 < argc) kld_eval(argv[++i]);
        else if (!strcmp(argv[i], "--crs") && i + 1 < argc) kld_crs(argv[++i]);
        else if (!strcmp(argv[i], "--i2chid") && i + 1 < argc) kld_i2chid(argv[++i]);
    }
    if (hid_i2c)
        kld_acpi_hid_i2c_scan();
    acpi_get_devices(NULL, kld_dev_cb, devices ? (void *)1 : NULL, NULL);
    printf("[kld-acpi] devices in namespace: %d (present per _STA: %d)\n", kld_ndev, kld_ndev_present);
    if (!kld_osl_broker_mode)
        printf("[kld-acpi] file mode simulated: %lu opregion pages, %lu port reads, %lu pci config reads (all read as 0)\n",
               kld_sim_pages, kld_port_reads, kld_pci_reads);
    return 0;
}

// end (satoru)
