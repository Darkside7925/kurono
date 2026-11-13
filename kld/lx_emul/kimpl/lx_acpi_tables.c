// lx_acpi_tables: the physical table provider and the namespace bring-up.
//
// two providers, one interface:
//   BLOB   the laptop's captured tables, embedded at build time as a C array
//          (never tracked by git) and copied into an arena at made-up but
//          consistent physical addresses; the FADT is repointed at the arena's
//          DSDT/FACS copies and an RSDP/XSDT are synthesized around them, the
//          same construction the standalone kld-acpi file mode uses.
//   BROKER the laptop itself: the bootloader's RSDP comes from the kernel's
//          ACPI op and every other table page is mapped through MAP_PHYS,
//          which refuses usable ram.
//
// seeds: S3a proved that this DSDT's module-level code hides \_SB.PC02 (and
// so TPD0) unless the firmware NVS fields PCHA, TPTY, SDS1, GPDI and PBSB are
// non-zero. pass one loads the tables with zero NVS, resolves each seed name
// to its field object (address/bit/len), then acpi_terminate(); pass two loads
// again with a root SystemMemory handler that overlays those bits. that is the
// same two-pass the userspace tool does with exec, warm restart per ACPICA's
// own "allows for a warm restart" note in acpi_ut_init_globals. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/acpi.h>

#include "accommon.h"
#include "acglobal.h"
#include "acnamesp.h"

#include "lx_acpi.h"
#include "lx_broker.h"
#include "lx_kshim.h"


// ── provider state ──────────────────────────────────────────────────────────
#define LX_ACPI_BASE 0x10000000ull
#define LX_ACPI_ALIGN 0x10000ull
#define LX_ACPI_RSDP_FAKE 0xFFFFE000ull

static enum lx_acpi_provider lx_acpi_provider;

static u8 *lx_acpi_arena;
static unsigned long lx_acpi_arena_len;

static u64 lx_acpi_rsdp;

// broker windows and the local RSDP copy (the broker's RSDP is read as eight
// byte pairs into this process, exactly like the standalone tool). (satoru)
struct lx_acpi_win {
    u64 pa;
    unsigned long len;
    u8 *va;
};
#define LX_ACPI_WINS 64
static struct lx_acpi_win lx_acpi_wins[LX_ACPI_WINS];
static int lx_acpi_nwins;
static u8 lx_acpi_rsdp_copy[64];
static unsigned long lx_acpi_rsdp_copy_len;

// resolved seeds, served by the root SystemMemory handler in pass two. (satoru)
static struct lx_acpi_seed *lx_acpi_seeds;
static int lx_acpi_nseeds;

// ── small helpers ───────────────────────────────────────────────────────────
static u8 lx_acpi_cksum(const void *p, unsigned long n)
{
    const u8 *b = p;
    u8 s = 0;

    while (n--)
        s += *b++;
    return s;
}

static const struct acpi_table_header *lx_acpi_tbl(const char *sig,
                                                   unsigned long *off,
                                                   u64 *pa)
{
    for (unsigned int i = 0; i < lx_acpi_blob_ntables; i++) {
        const struct lx_acpi_blob_table *t = &lx_acpi_blob_tables[i];
        const struct acpi_table_header *h =
            (const struct acpi_table_header *)(lx_acpi_blob_data + t->offset);

        if (!memcmp(h->signature, sig, 4)) {
            if (off)
                *off = t->offset;
            if (pa)
                *pa = LX_ACPI_BASE + t->offset;
            return h;
        }
    }
    return NULL;
}

// ── blob provider ───────────────────────────────────────────────────────────
static int lx_acpi_blob_build(void)
{
    struct acpi_table_header *facp_t = NULL;
    struct acpi_table_header *dsdt_t = NULL;
    struct acpi_table_header *facs_t = NULL;
    struct acpi_table_fadt *fadt = NULL;
    struct acpi_table_xsdt *xsdt;
    struct acpi_table_rsdp *rsdp;
    unsigned long n = 0, xsdt_len, next, dsdt_off = 0, facs_off = 0;
    unsigned int entries = 0, k = 0;

    if (lx_acpi_arena)
        return 0;
    if (!lx_acpi_blob_ntables || !lx_acpi_blob_data) {
        lx_log("[kld-acpi] no table blob in the container\n");
        return -1;
    }

    // table space: one 64k-aligned slot each, after the RSDP/XSDT area. (satoru)
    n = LX_ACPI_ALIGN;
    for (unsigned int i = 0; i < lx_acpi_blob_ntables; i++)
        n += (lx_acpi_blob_tables[i].length + LX_ACPI_ALIGN - 1) &
             ~(LX_ACPI_ALIGN - 1);
    if (n > 64ul * 1024 * 1024) {
        lx_log("[kld-acpi] table blob too large\n");
        return -1;
    }
    lx_acpi_arena = kzalloc(n, GFP_KERNEL);
    if (!lx_acpi_arena)
        return -1;
    lx_acpi_arena_len = n;

    next = LX_ACPI_ALIGN;
    for (unsigned int i = 0; i < lx_acpi_blob_ntables; i++) {
        const struct lx_acpi_blob_table *t = &lx_acpi_blob_tables[i];
        struct acpi_table_header *h =
            (struct acpi_table_header *)(lx_acpi_arena + next);

        memcpy(h, lx_acpi_blob_data + t->offset, t->length);
        if (!memcmp(h->signature, "FACP", 4)) {
            facp_t = h;
        } else if (!memcmp(h->signature, "DSDT", 4)) {
            dsdt_t = h;
            dsdt_off = next;
        } else if (!memcmp(h->signature, "FACS", 4)) {
            facs_t = h;
            facs_off = next;
        }
        next += (t->length + LX_ACPI_ALIGN - 1) & ~(LX_ACPI_ALIGN - 1);
    }
    if (!facp_t || !dsdt_t) {
        lx_log("[kld-acpi] blob needs FACP and DSDT\n");
        return -1;
    }

    // point the FADT at our copies and re-checksum it, exactly as file mode. (satoru)
    fadt = (struct acpi_table_fadt *)facp_t;
    fadt->dsdt = (u32)(LX_ACPI_BASE + dsdt_off);
    if (fadt->header.length >= ACPI_FADT_OFFSET(Xdsdt) + 8)
        fadt->Xdsdt = LX_ACPI_BASE + dsdt_off;
    if (facs_t) {
        fadt->facs = (u32)(LX_ACPI_BASE + facs_off);
        if (fadt->header.length >= ACPI_FADT_OFFSET(Xfacs) + 8)
            fadt->Xfacs = LX_ACPI_BASE + facs_off;
    } else {
        fadt->facs = 0;
        if (fadt->header.length >= ACPI_FADT_OFFSET(Xfacs) + 8)
            fadt->Xfacs = 0;
    }
    fadt->header.checksum = 0;
    fadt->header.checksum = (u8)(0 - lx_acpi_cksum(fadt, fadt->header.length));

    // XSDT: every table except DSDT and FACS (reached through the FADT). (satoru)
    for (unsigned int i = 0; i < lx_acpi_blob_ntables; i++) {
        const struct acpi_table_header *h =
            (const struct acpi_table_header *)(lx_acpi_blob_data +
                                               lx_acpi_blob_tables[i].offset);
        if (memcmp(h->signature, "DSDT", 4) && memcmp(h->signature, "FACS", 4))
            entries++;
    }
    xsdt_len = (unsigned long)sizeof(struct acpi_table_header) +
               8ul * entries;
    xsdt = (struct acpi_table_xsdt *)(lx_acpi_arena + 0x1000);
    memcpy(xsdt->header.signature, "XSDT", 4);
    xsdt->header.length = (u32)xsdt_len;
    xsdt->header.revision = 1;
    memcpy(xsdt->header.oem_id, "KURONO", 6);
    memcpy(xsdt->header.oem_table_id, "KLDACPI ", 8);
    next = LX_ACPI_ALIGN;
    for (unsigned int i = 0; i < lx_acpi_blob_ntables; i++) {
        const struct lx_acpi_blob_table *t = &lx_acpi_blob_tables[i];
        const struct acpi_table_header *h =
            (const struct acpi_table_header *)(lx_acpi_blob_data + t->offset);
        u64 pa = LX_ACPI_BASE + next;

        // advance for every table, including the two the XSDT does not list. (satoru)
        next += (t->length + LX_ACPI_ALIGN - 1) & ~(LX_ACPI_ALIGN - 1);
        if (!memcmp(h->signature, "DSDT", 4) || !memcmp(h->signature, "FACS", 4))
            continue;
        xsdt->table_offset_entry[k++] = pa;
    }
    xsdt->header.checksum = (u8)(0 - lx_acpi_cksum(xsdt, xsdt_len));

    rsdp = (struct acpi_table_rsdp *)lx_acpi_arena;
    memcpy(rsdp->signature, ACPI_SIG_RSDP, 8);
    memcpy(rsdp->oem_id, "KURONO", 6);
    rsdp->revision = 2;
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_physical_address = LX_ACPI_BASE + 0x1000;
    rsdp->checksum = (u8)(0 - lx_acpi_cksum(rsdp, ACPI_RSDP_CHECKSUM_LENGTH));
    rsdp->extended_checksum =
        (u8)(0 - lx_acpi_cksum(rsdp, ACPI_RSDP_XCHECKSUM_LENGTH));

    lx_acpi_rsdp = LX_ACPI_BASE;
    lx_log("[kld-acpi] table blob: ");
    lx_log_u(lx_acpi_blob_ntables);
    lx_log(" tables, xsdt ");
    lx_log_u(entries);
    lx_log(" entries, arena ");
    lx_log_u(lx_acpi_arena_len);
    lx_log(" bytes\n");
    return 0;
}

static void *lx_acpi_broker_map(u64 addr, unsigned long len);

void *lx_acpi_provider_mem(u64 where, unsigned long len)
{
    if (!len)
        len = 1;
    if (lx_acpi_arena && where >= LX_ACPI_BASE &&
        where + len <= LX_ACPI_BASE + lx_acpi_arena_len)
        return lx_acpi_arena + (where - LX_ACPI_BASE);
    if (lx_acpi_provider == LX_ACPI_PROVIDER_BROKER) {
        if (where >= LX_ACPI_RSDP_FAKE &&
            where + len <= LX_ACPI_RSDP_FAKE + lx_acpi_rsdp_copy_len)
            return lx_acpi_rsdp_copy + (where - LX_ACPI_RSDP_FAKE);
        return lx_acpi_broker_map(where, len);
    }
    return NULL;
}

u64 lx_acpi_provider_rsdp(void)
{
    return lx_acpi_rsdp;
}

int lx_acpi_broker_mode(void)
{
    return lx_acpi_provider == LX_ACPI_PROVIDER_BROKER;
}

// ── broker provider ─────────────────────────────────────────────────────────
static long lx_acpi_broker_port_read(unsigned long port, unsigned int width)
{
    return lx_broker_call(LX_KLD_OP_ACPI_PORT_IN, port, width, 0, 0);
}

static long lx_acpi_broker_port_write(unsigned long port, unsigned int width,
                                      unsigned long value)
{
    return lx_broker_call(LX_KLD_OP_ACPI_PORT_OUT, port, width, value, 0);
}

static long lx_acpi_broker_pci_read(unsigned int seg, unsigned int bus,
                                    unsigned int dev, unsigned int fn,
                                    unsigned int reg, unsigned int width)
{
    if (seg)
        return -1;
    return lx_broker_call(LX_KLD_OP_ACPI_PCI_READ,
                          ((unsigned long)bus << 16) |
                          ((unsigned long)dev << 8) | fn,
                          reg, width / 8, 0);
}

static long lx_acpi_broker_pci_write(unsigned int seg, unsigned int bus,
                                     unsigned int dev, unsigned int fn,
                                     unsigned int reg, unsigned int width,
                                     unsigned long value)
{
    if (seg)
        return -1;
    return lx_broker_call(LX_KLD_OP_ACPI_PCI_WRITE,
                          ((unsigned long)bus << 16) |
                          ((unsigned long)dev << 8) | fn,
                          reg, width / 8, value);
}

static void *lx_acpi_broker_map(u64 addr, unsigned long len)
{
    u64 pb;
    unsigned long pl;
    u8 *va;
    long r;

    for (int i = 0; i < lx_acpi_nwins; i++)
        if (addr >= lx_acpi_wins[i].pa &&
            addr + len <= lx_acpi_wins[i].pa + lx_acpi_wins[i].len)
            return lx_acpi_wins[i].va + (addr - lx_acpi_wins[i].pa);
    if (lx_acpi_nwins >= LX_ACPI_WINS)
        return NULL;
    pb = addr & ~0xFFFull;
    pl = (unsigned long)(((addr + len + 0xFFF) & ~0xFFFull) - pb);
    va = lx_os_reserve(pl);
    if (!va)
        return NULL;
    r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, pb, pl,
                       (unsigned long)va, 0);
    if (r < 0) {
        lx_os_unreserve(va, pl);
        lx_log("[kld-acpi] MAP_PHYS ");
        lx_log_hex(pb);
        lx_log(" refused (");
        lx_log_u((unsigned long)(-r));
        lx_log(")\n");
        return NULL;
    }
    lx_acpi_wins[lx_acpi_nwins].pa = pb;
    lx_acpi_wins[lx_acpi_nwins].len = pl;
    lx_acpi_wins[lx_acpi_nwins].va = va;
    lx_acpi_nwins++;
    return va + (addr - pb);
}

static int lx_acpi_broker_start(void)
{
    if (!lx_broker_available()) {
        lx_log("[kld-acpi] broker not available\n");
        return -1;
    }
    for (unsigned long off = 0; off <= 32; off += 8) {
        long v = lx_broker_call(LX_KLD_OP_ACPI_RSDP_READ, off, 0, 0, 0);

        if (v < 0) {
            lx_log("[kld-acpi] broker refused RSDP_READ (");
            lx_log_u((unsigned long)(-v));
            lx_log("), not the privileged container?\n");
            return -1;
        }
        memcpy(lx_acpi_rsdp_copy + off, &v, 8);
    }
    if (memcmp(lx_acpi_rsdp_copy, "RSD PTR ", 8)) {
        lx_log("[kld-acpi] no RSDP from the bootloader\n");
        return -1;
    }
    lx_acpi_rsdp_copy_len = 36;
    lx_acpi_rsdp = LX_ACPI_RSDP_FAKE;
    lx_acpi_port_read_hook = lx_acpi_broker_port_read;
    lx_acpi_port_write_hook = lx_acpi_broker_port_write;
    lx_acpi_pci_read_hook = lx_acpi_broker_pci_read;
    lx_acpi_pci_write_hook = lx_acpi_broker_pci_write;
    lx_log("[kld-acpi] RSDP from the broker, rev ");
    lx_log_u(lx_acpi_rsdp_copy[15]);
    lx_log("\n");
    return 0;
}

int lx_acpi_set_provider(enum lx_acpi_provider provider)
{
    lx_acpi_provider = provider;
    if (provider == LX_ACPI_PROVIDER_BLOB)
        return lx_acpi_blob_build();
    if (provider == LX_ACPI_PROVIDER_BROKER)
        return lx_acpi_broker_start();
    return -1;
}

// ── SystemMemory handler with the NVS seeds ─────────────────────────────────
static void lx_acpi_note_opregion(u64 addr, u32 width)
{
    static u64 seen[16];
    static int nseen;

    for (int i = 0; i < nseen; i++)
        if (seen[i] == addr)
            return;
    if (nseen < 16) {
        seen[nseen++] = addr;
        lx_log("[kld-acpi] SystemMemory opregion ");
        lx_log_hex(addr);
        lx_log(" width ");
        lx_log_u(width);
        lx_log(" reads as 0 (modelled)\n");
    }
}

static u64 lx_acpi_seed_overlay(u64 addr, u32 width, u64 value)
{
    u64 wbit = addr * 8ull;

    for (int i = 0; i < lx_acpi_nseeds; i++) {
        struct lx_acpi_seed *s = &lx_acpi_seeds[i];
        u64 sbit;

        if (!s->resolved)
            continue;
        sbit = s->addr * 8ull + s->bit;
        if (sbit >= wbit + width || sbit + s->len <= wbit)
            continue;
        for (u32 b = 0; b < s->len; b++) {
            u64 ab = sbit + b;
            u64 mask;

            if (ab < wbit || ab >= wbit + width)
                continue;
            mask = 1ull << (ab - wbit);
            if ((s->value >> b) & 1ull)
                value |= mask;
            else
                value &= ~mask;
        }
    }
    return value;
}

static acpi_status lx_acpi_mem_handler(u32 function,
                                       acpi_physical_address address,
                                       u32 bit_width, u64 *value,
                                       void *handler_context,
                                       void *region_context)
{
    (void)handler_context;
    (void)region_context;
    if (function == ACPI_READ) {
        u64 v = 0;
        acpi_status st = acpi_os_read_memory(address, &v, bit_width);

        if (ACPI_FAILURE(st))
            return st;
        *value = lx_acpi_seed_overlay((u64)address, bit_width, v);
        if (lx_acpi_trace_hook)
            lx_acpi_trace_hook("SystemMemory", 0, (u64)address, bit_width, *value);
        lx_acpi_opregion_reads++;
        lx_acpi_note_opregion((u64)address, bit_width);
        return AE_OK;
    }
    if (function == ACPI_WRITE) {
        lx_acpi_opregion_writes++;
        if (lx_acpi_trace_hook)
            lx_acpi_trace_hook("SystemMemory", 1, (u64)address, bit_width, *value);
        // broker mode refuses anything not on the (empty) allowlist; the
        // caller still sees AE_OK, as AML expects for a handled region. (satoru)
        if (!lx_acpi_write_note("SystemMemory", (u64)address, bit_width))
            return AE_OK;
        return acpi_os_write_memory(address, *value, bit_width);
    }
    return AE_OK;
}

static int lx_acpi_locate_seed(struct lx_acpi_seed *s)
{
    struct acpi_namespace_node *node = NULL;
    union acpi_operand_object *obj, *reg;
    char path[64];
    unsigned int n;

    // NVS fields live at the root scope (S3a resolves them the same way). (satoru)
    path[0] = '\\';
    for (n = 0; n < sizeof(path) - 2 && s->name[n]; n++)
        path[n + 1] = s->name[n];
    path[n + 1] = 0;
    if (ACPI_FAILURE(acpi_get_handle(NULL, path, (acpi_handle *)&node))) {
        lx_log("[kld-acpi] seed ");
        lx_log(s->name);
        lx_log(": no such namespace object\n");
        return -1;
    }
    obj = acpi_ns_get_attached_object(node);
    if (!obj || obj->common.type != ACPI_TYPE_LOCAL_REGION_FIELD) {
        lx_log("[kld-acpi] seed ");
        lx_log(s->name);
        lx_log(": not a field\n");
        return -1;
    }
    reg = obj->field.region_obj;
    if (!reg || reg->region.space_id != ACPI_ADR_SPACE_SYSTEM_MEMORY) {
        lx_log("[kld-acpi] seed ");
        lx_log(s->name);
        lx_log(": not a SystemMemory field\n");
        return -1;
    }
    s->addr = reg->region.address + obj->common_field.base_byte_offset;
    s->bit = obj->common_field.start_field_bit_offset;
    s->len = obj->common_field.bit_length;
    s->resolved = 1;
    lx_log("[kld-acpi] NVS field ");
    lx_log(s->name);
    lx_log(" is @");
    lx_log_hex(s->addr);
    lx_log(" bit ");
    lx_log_u(s->bit);
    lx_log(" len ");
    lx_log_u(s->len);
    lx_log(" = ");
    lx_log_hex(s->value);
    lx_log("\n");
    return 0;
}

// ── power methods through the census ────────────────────────────────────────
// Linux runs _INI from acpi_initialize_objects (already done by lx_acpi_load),
// then, when a driver asks for D0, _PSC and _PS0, and _ON/_OFF for every
// resource in _PR0/_PR3. the same methods are evaluated here so their opregion
// writes appear in the census with the method path. this laptop's active
// \_SB.PC02.I2C1 and its TPD0 have no _PSn/_PRn/_PSC at all; the list is
// printed either way. (satoru)
static void lx_acpi_power_list(acpi_handle dev, const char *path)
{
    static const char *const names[] = {
        "_INI", "_PSC", "_PS0", "_PS3", "_PR0", "_PR3", "_DSW", "_S0W",
    };
    acpi_handle h;

    lx_log("[kld-acpi] power ");
    lx_log(path);
    lx_log(":");
    for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
        int exists = ACPI_SUCCESS(acpi_get_handle(dev, (char *)names[i], &h));

        lx_log(" ");
        lx_log(names[i]);
        lx_log(exists ? "=yes" : "=no");
    }
    lx_log("\n");
}

static acpi_status lx_acpi_run(acpi_handle dev, const char *name)
{
    acpi_handle h;

    if (ACPI_FAILURE(acpi_get_handle(dev, (char *)name, &h)))
        return AE_NOT_FOUND;
    return acpi_evaluate_object(h, NULL, NULL, NULL);
}

static void lx_acpi_run_if(acpi_handle dev, const char *name)
{
    acpi_status st = lx_acpi_run(dev, name);
    char line[160];

    if (st == AE_NOT_FOUND)
        return;
    snprintf(line, sizeof(line), "[kld-acpi] power eval %s: %s", name,
             ACPI_SUCCESS(st) ? "ok" : acpi_format_exception(st));
    lx_log_line(line);
}

static void lx_acpi_power_resources(acpi_handle dev, const char *name)
{
    acpi_handle h;
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    union acpi_object *o;
    acpi_status st;

    if (ACPI_FAILURE(acpi_get_handle(dev, (char *)name, &h)))
        return;
    st = acpi_evaluate_object(h, NULL, NULL, &out);
    if (ACPI_FAILURE(st) || !out.pointer) {
        char line[160];

        snprintf(line, sizeof(line), "[kld-acpi] power %s: %s", name,
                 acpi_format_exception(st));
        lx_log_line(line);
        return;
    }
    o = out.pointer;
    if (o->type == ACPI_TYPE_PACKAGE) {
        for (u32 i = 0; i < o->package.count; i++) {
            union acpi_object *e = &o->package.elements[i];

            if (e->type != ACPI_TYPE_LOCAL_REFERENCE || !e->reference.handle)
                continue;
            lx_acpi_run_if(e->reference.handle, "_ON");
            lx_acpi_run_if(e->reference.handle, "_STA");
            lx_acpi_run_if(e->reference.handle, "_OFF");
        }
    }
    ACPI_FREE(out.pointer);
}

static void lx_acpi_power_walk(const char *path)
{
    acpi_handle dev = NULL;

    if (ACPI_FAILURE(acpi_get_handle(NULL, (char *)path, &dev)))
        return;
    lx_acpi_power_list(dev, path);
    // _INI already ran inside acpi_initialize_objects; do not run it twice.
    lx_acpi_run_if(dev, "_PSC");
    lx_acpi_run_if(dev, "_PS0");
    lx_acpi_run_if(dev, "_PS3");
    lx_acpi_run_if(dev, "_PS0");
    lx_acpi_power_resources(dev, "_PR0");
    lx_acpi_power_resources(dev, "_PR3");
}

// ── namespace bring-up ──────────────────────────────────────────────────────
static int lx_acpi_load(void)
{
    acpi_status st;
    u32 flags = ACPI_NO_ACPI_ENABLE | ACPI_NO_HARDWARE_INIT |
                ACPI_NO_EVENT_INIT | ACPI_NO_HANDLER_INIT;

    st = acpi_initialize_subsystem();
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] initialize_subsystem: ");
        lx_log_line(acpi_format_exception(st));
#ifdef KLD_ACPI_DEBUG
        lx_log("[kld-dbg] root 0x");
        lx_log_hex((unsigned long)(uintptr_t)acpi_gbl_root_node);
        struct acpi_namespace_node *ch = acpi_gbl_root_node_struct.child;
        lx_log(" child0 0x");
        lx_log_hex((unsigned long)(uintptr_t)ch);
        if (ch) {
            lx_log(" name 0x");
            lx_log_hex((unsigned long)ch->name.integer);
            lx_log(" type ");
            lx_log_u(ch->type);
        }
        lx_log("\n");
#endif
        return -1;
    }
    // acpi_load_tables installs the default SystemMemory handler at the root,
    // but tbxfload.c allows an OSPM handler installed after initialize_subsystem
    // and before load_tables to take its place. (satoru)
    st = acpi_install_address_space_handler(ACPI_ROOT_OBJECT,
                                            ACPI_ADR_SPACE_SYSTEM_MEMORY,
                                            lx_acpi_mem_handler, NULL, NULL);
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] install SystemMemory handler: ");
        lx_log_line(acpi_format_exception(st));
        return -1;
    }
    st = acpi_initialize_tables(NULL, 32, FALSE);
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] initialize_tables: ");
        lx_log_line(acpi_format_exception(st));
        return -1;
    }
    st = acpi_load_tables();
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] load_tables: ");
        lx_log_line(acpi_format_exception(st));
        return -1;
    }
    st = acpi_enable_subsystem(flags);
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] enable_subsystem: ");
        lx_log_line(acpi_format_exception(st));
        return -1;
    }
    st = acpi_initialize_objects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(st)) {
        lx_log("[kld-acpi] initialize_objects: ");
        lx_log_line(acpi_format_exception(st));
        return -1;
    }
    return 0;
}

static acpi_status lx_acpi_count_dev(acpi_handle h, u32 level, void *ctx,
                                     void **ret)
{
    int *counts = ctx;
    struct acpi_device_info *info = NULL;

    (void)level;
    (void)ret;
    counts[0]++;
    if (ACPI_SUCCESS(acpi_get_object_info(h, &info))) {
        u64 sta = 0x0F;
        union acpi_object o;
        struct acpi_buffer b = { sizeof(o), &o };

        if (ACPI_FAILURE(acpi_evaluate_object_typed(h, "_STA", NULL, &b,
                                                    ACPI_TYPE_INTEGER)))
            sta = 0x0F;
        else
            sta = o.integer.value;
        if (sta & ACPI_STA_DEVICE_PRESENT)
            counts[1]++;
        ACPI_FREE(info);
    }
    return AE_OK;
}

int lx_acpi_init(struct lx_acpi_seed *seeds, int nseeds)
{
    int counts[2] = { 0, 0 };

    lx_acpi_seeds = seeds;
    lx_acpi_nseeds = nseeds;

    if (!lx_acpi_rsdp) {
        lx_log("[kld-acpi] no provider\n");
        return -1;
    }

    if (nseeds > 0) {
        // pass one: zero NVS, enough namespace to find the fields. (satoru)
        if (lx_acpi_load())
            return -1;
        for (int i = 0; i < nseeds; i++)
            lx_acpi_locate_seed(&seeds[i]);
        if (ACPI_FAILURE(acpi_terminate())) {
            lx_log("[kld-acpi] terminate for the seeded pass failed\n");
            return -1;
        }
        // warm restart: early_initialization is the one global the shutdown
        // path does not reset, and load two must see it as boot does. (satoru)
        acpi_gbl_early_initialization = TRUE;
    }

    // pass two (or the only pass): seeds are served from the first byte the
    // DSDT's module-level code reads, so \_SB.PC02 exists afterwards. (satoru)
    if (lx_acpi_load())
        return -1;

    lx_acpi_power_walk("\\_SB.PC02.I2C1");
    lx_acpi_power_walk("\\_SB.PC02.I2C1.TPD0");
    lx_log("[kld-acpi] power limits: device-level _PSn only; no _PR0/_PR3"
           " resources, no _PSC state and no parent ordering\n");
    acpi_get_devices(NULL, lx_acpi_count_dev, counts, NULL);
    lx_log("[kld-tp-acpi] devices in namespace: ");
    lx_log_u((unsigned long)counts[0]);
    lx_log(" (present per _STA: ");
    lx_log_u((unsigned long)counts[1]);
    lx_log(")\n");
    lx_log("[kld-acpi] file mode simulated: ");
    lx_log_u(lx_acpi_opregion_reads);
    lx_log(" opregion reads, ");
    lx_log_u(lx_acpi_opregion_writes);
    lx_log(" writes, ");
    lx_log_u(lx_acpi_port_reads);
    lx_log(" port reads, ");
    lx_log_u(lx_acpi_pci_reads);
    lx_log(" pci config reads\n");
    return 0;
}

// end (satoru)
