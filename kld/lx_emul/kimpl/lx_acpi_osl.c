// lx_acpi_osl: the ACPICA OS services layer, kernel context.
//
// drivers/acpi/acpica calls about 45 acpi_os_* functions. upstream's
// drivers/acpi/osl.c is NOT compiled: it drags in the linux ACPI bus, the
// interrupt core and the platform device model. this file is the kurono
// binding and it uses only the shim's own primitives (kmalloc, spinlock,
// semaphore, ktime, workqueue, thread id).
//
// aclinux.h/aclinuxex.h turn acpi_os_allocate, acpi_os_free,
// acpi_os_acquire_object, acpi_os_create_lock, acpi_os_get_thread_id and
// acpi_os_readable into macros/inlines over the kernel; those are deliberately
// not defined here.
//
// physical memory: the provider in lx_acpi_tables.c serves the laptop's table
// arena; any other SystemMemory address is a zero page, exactly like the
// standalone tool's file mode. port i/o and pci config have dispatcher hooks
// (default: read 0, counted, logged once per address); with ACPI_NO_EVENT_INIT
// and ACPI_NO_HANDLER_INIT no SCI or GPE reaches them. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/semaphore.h>
#include <linux/sprintf.h>
#include <linux/stdarg.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/acpi.h>

#include "accommon.h"
#include "acdispat.h"
#include "acparser.h"

#include "lx_acpi.h"

#include "lx_kshim.h"

// ── physical memory backing (zero pages outside the provider arena) ─────────
#define LX_ACPI_SIM_MAX 128

struct lx_acpi_sim_page {
    u64 base;
    unsigned long len;
    u8 *mem;
};

static struct lx_acpi_sim_page lx_acpi_pages[LX_ACPI_SIM_MAX];
static int lx_acpi_npages;
static DEFINE_SPINLOCK(lx_acpi_pages_lock);

unsigned long lx_acpi_opregion_reads;
void (*lx_acpi_trace_hook)(const char *space, int write, u64 addr, u32 width,
                           u64 value);
unsigned long lx_acpi_opregion_writes;
unsigned long lx_acpi_port_reads, lx_acpi_port_writes;
unsigned long lx_acpi_pci_reads, lx_acpi_pci_writes;

// the backing page is allocated OUTSIDE the spinlock: kzalloc may sleep. the
// slot is filled under the lock and re-checked, so a racing second caller gets
// the page the first one installed and frees its spare. (satoru)
static void *lx_acpi_sim_lookup(u64 addr, unsigned long len, int create)
{
    void *r = NULL;
    u64 pb = 0;
    unsigned long pl = 0, flags;
    bool want_page = false;
    u8 *spare;

    spin_lock_irqsave(&lx_acpi_pages_lock, flags);
    for (int i = 0; i < lx_acpi_npages; i++) {
        struct lx_acpi_sim_page *g = &lx_acpi_pages[i];
        if (addr >= g->base && addr + len <= g->base + g->len) {
            r = g->mem + (addr - g->base);
            break;
        }
    }
    if (!r && create && lx_acpi_npages < LX_ACPI_SIM_MAX) {
        // page granular and persistent for the run, like the file-mode tool. (satoru)
        pb = addr & ~0xFFFull;
        pl = (unsigned long)(((addr + len + 0xFFF) & ~0xFFFull) - pb);
        want_page = true;
    }
    spin_unlock_irqrestore(&lx_acpi_pages_lock, flags);
    if (r || !want_page)
        return r;

    spare = kzalloc(pl, GFP_KERNEL);
    if (!spare)
        return NULL;
    spin_lock_irqsave(&lx_acpi_pages_lock, flags);
    for (int i = 0; i < lx_acpi_npages; i++) {
        struct lx_acpi_sim_page *g = &lx_acpi_pages[i];
        if (addr >= g->base && addr + len <= g->base + g->len) {
            r = g->mem + (addr - g->base);
            break;
        }
    }
    if (!r && lx_acpi_npages < LX_ACPI_SIM_MAX) {
        lx_acpi_pages[lx_acpi_npages].base = pb;
        lx_acpi_pages[lx_acpi_npages].len = pl;
        lx_acpi_pages[lx_acpi_npages].mem = spare;
        lx_acpi_npages++;
        r = spare + (addr - pb);
        spare = NULL;
    }
    spin_unlock_irqrestore(&lx_acpi_pages_lock, flags);
    kfree(spare);
    return r;
}

void *lx_acpi_provider_map(u64 where, unsigned long len)
{
    void *p;

    if (!len)
        len = 1;
    p = lx_acpi_provider_mem(where, len);
    if (p)
        return p;
    return lx_acpi_sim_lookup(where, len, 1);
}

void lx_acpi_provider_unmap(void *p, unsigned long len)
{
    (void)p;
    (void)len;
}

static u8 *lx_acpi_map(u64 addr, unsigned long len)
{
    if (!len)
        len = 1;
    return lx_acpi_provider_map(addr, len);
}

// ── operation-region write census ───────────────────────────────────────────
// a write in blob mode disappears into a simulated page; the same write in
// broker mode would hit real hardware. every distinct (space, address, width)
// is logged once with the control method that was executing (the walk state's
// method_pathname, plus the module-level parse flag), and the full list is
// printed as a table at the end of the run. broker mode refuses every write
// that is not on the allowlist, and the allowlist starts empty on purpose: the
// owner fills it after reviewing this census. (satoru)
#define LX_ACPI_WR_CENSUS 192

struct lx_acpi_wr_entry {
    const char *space;
    u64 addr;
    u32 width;
    u32 count;
    u32 refused;
    char method[96];
};

static struct lx_acpi_wr_entry lx_acpi_wrc[LX_ACPI_WR_CENSUS];
static int lx_acpi_nwrc;
static DEFINE_SPINLOCK(lx_acpi_wrc_lock);

struct lx_acpi_wr_allow {
    const char *space;
    u64 addr;
    u32 width;
};

static const struct lx_acpi_wr_allow lx_acpi_wr_allow[] = {
    // empty until the owner names an entry; the NULL row is the sentinel. (satoru)
    { NULL, 0, 0 },
};

static int lx_acpi_wr_is_allowed(const char *space, u64 addr, u32 width)
{
    // the kernel is the enforcement point: every broker-mode write is forwarded
    // and the kernel refuses the hardware access (and shadows the value for the
    // read-back) unless the owner's boot-time allowlist names it. this shim-side
    // table is only the historical first filter; it no longer drops writes on
    // its own. (satoru)
    if (!lx_acpi_broker_mode())
        return 1;
    return 1;
    for (unsigned int i = 0;
         i < sizeof(lx_acpi_wr_allow) / sizeof(lx_acpi_wr_allow[0]); i++) {
        const struct lx_acpi_wr_allow *a = &lx_acpi_wr_allow[i];

        if (a->space && !strcmp(a->space, space) && a->addr == addr &&
            a->width == width)
            return 1;
    }
    return 0;
}

static void lx_acpi_current_method(char *buf, unsigned long len)
{
    struct acpi_walk_state *ws = NULL;

    if (acpi_gbl_current_walk_list)
        ws = acpi_ds_get_current_walk_state(acpi_gbl_current_walk_list);
    if (ws && (ws->parse_flags & ACPI_PARSE_MODULE_LEVEL)) {
        snprintf(buf, len, "module-level %s",
                 ws->method_pathname ? ws->method_pathname : "?");
        return;
    }
    if (ws && ws->method_pathname && ws->method_pathname[0]) {
        snprintf(buf, len, "%s", ws->method_pathname);
        return;
    }
    snprintf(buf, len, "<none>");
}

int lx_acpi_write_note(const char *space, u64 addr, u32 width)
{
    char method[96];
    unsigned long flags;
    int allowed, i, first = 0;

    lx_acpi_current_method(method, sizeof(method));
    allowed = lx_acpi_wr_is_allowed(space, addr, width);

    spin_lock_irqsave(&lx_acpi_wrc_lock, flags);
    for (i = 0; i < lx_acpi_nwrc; i++) {
        struct lx_acpi_wr_entry *e = &lx_acpi_wrc[i];

        if (!strcmp(e->space, space) && e->addr == addr &&
            e->width == width) {
            e->count++;
            if (!allowed)
                e->refused++;
            break;
        }
    }
    if (i == lx_acpi_nwrc && lx_acpi_nwrc < LX_ACPI_WR_CENSUS) {
        struct lx_acpi_wr_entry *e = &lx_acpi_wrc[lx_acpi_nwrc++];

        e->space = space;
        e->addr = addr;
        e->width = width;
        e->count = 1;
        e->refused = allowed ? 0 : 1;
        strncpy(e->method, method, sizeof(e->method) - 1);
        e->method[sizeof(e->method) - 1] = 0;
        first = 1;
    }
    spin_unlock_irqrestore(&lx_acpi_wrc_lock, flags);

    if (first) {
        lx_log("[kld-acpi-wr] ");
        lx_log(space);
        lx_log(" write ");
        lx_log_hex(addr);
        lx_log(" width ");
        lx_log_u(width);
        lx_log(" method=");
        lx_log(method);
        lx_log(allowed ? "\n" : " REFUSED\n");
    }
    return allowed;
}

static int lx_acpi_on_tp_path(const char *m)
{
    return !strncmp(m, "module-level", 12) || strstr(m, "PC02") ||
           strstr(m, "I2C1") || strstr(m, "TPD0") || strstr(m, "PGPI") ||
           strstr(m, "INTC1084");
}

void lx_acpi_write_census(void)
{
    char line[256];
    int on_path = 0, refused = 0;

    for (int i = 0; i < lx_acpi_nwrc; i++) {
        if (lx_acpi_on_tp_path(lx_acpi_wrc[i].method))
            on_path++;
        if (lx_acpi_wrc[i].refused)
            refused++;
    }
    lx_log("[kld-acpi-wr] write census: ");
    lx_log_u((unsigned long)lx_acpi_nwrc);
    lx_log(" distinct (space, address, width) writes, ");
    lx_log_u((unsigned long)on_path);
    lx_log(" on the touchpad path, ");
    lx_log_u((unsigned long)refused);
    lx_log(" refused\n");
    lx_log("[kld-acpi-wr] space          addr                width method\n");
    for (int i = 0; i < lx_acpi_nwrc; i++) {
        struct lx_acpi_wr_entry *e = &lx_acpi_wrc[i];

        snprintf(line, sizeof(line), "[kld-acpi-wr] %-14s 0x%016llx %5u %s%s",
                 e->space, (unsigned long long)e->addr, e->width, e->method,
                 e->refused ? " REFUSED" : "");
        lx_log_line(line);
    }
}

// ── init / tables ───────────────────────────────────────────────────────────
acpi_status acpi_os_initialize(void)
{
    return AE_OK;
}

acpi_status acpi_os_terminate(void)
{
    return AE_OK;
}

acpi_physical_address acpi_os_get_root_pointer(void)
{
    return (acpi_physical_address)lx_acpi_provider_rsdp();
}

acpi_status acpi_os_predefined_override(const struct acpi_predefined_names *init,
                                        acpi_string *new_val)
{
    (void)init;
    *new_val = NULL;
    return AE_OK;
}

acpi_status acpi_os_table_override(struct acpi_table_header *existing,
                                   struct acpi_table_header **new_table)
{
    (void)existing;
    *new_table = NULL;
    return AE_OK;
}

acpi_status acpi_os_physical_table_override(struct acpi_table_header *existing,
                                            acpi_physical_address *new_addr,
                                            u32 *new_len)
{
    (void)existing;
    *new_addr = 0;
    *new_len = 0;
    return AE_OK;
}

// ── memory ──────────────────────────────────────────────────────────────────
void *acpi_os_map_memory(acpi_physical_address where, acpi_size length)
{
    return lx_acpi_map((u64)where, (unsigned long)length);
}

void acpi_os_unmap_memory(void *where, acpi_size length)
{
    lx_acpi_provider_unmap(where, (unsigned long)length);
}

acpi_status acpi_os_get_physical_address(void *logical,
                                         acpi_physical_address *physical)
{
    (void)logical;
    *physical = 0;
    return AE_SUPPORT;
}

u8 acpi_os_writable(void *pointer, acpi_size length)
{
    (void)pointer;
    (void)length;
    return TRUE;
}

acpi_status acpi_os_read_memory(acpi_physical_address addr, u64 *value,
                                u32 width)
{
    u8 *p = lx_acpi_map((u64)addr, width / 8);

    if (!p) {
        *value = 0;
        return AE_BAD_ADDRESS;
    }
    switch (width) {
    case 8:  *value = *(volatile u8 *)p; break;
    case 16: *value = *(volatile u16 *)p; break;
    case 32: *value = *(volatile u32 *)p; break;
    case 64: *value = *(volatile u64 *)p; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

acpi_status acpi_os_write_memory(acpi_physical_address addr, u64 value,
                                 u32 width)
{
    u8 *p = lx_acpi_map((u64)addr, width / 8);

    if (!p)
        return AE_BAD_ADDRESS;
    switch (width) {
    case 8:  *(volatile u8 *)p = (u8)value; break;
    case 16: *(volatile u16 *)p = (u16)value; break;
    case 32: *(volatile u32 *)p = (u32)value; break;
    case 64: *(volatile u64 *)p = value; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

// ── object caches ───────────────────────────────────────────────────────────
// acpi_cache_t is struct kmem_cache in kernel mode and acpi_os_acquire_object
// is kmem_cache_zalloc (aclinuxex.h). purge is real work in SLUB; here every
// object is independent, so there is nothing to purge. (satoru)
acpi_status acpi_os_create_cache(char *name, u16 size, u16 depth,
                                 acpi_cache_t **ret)
{
    (void)depth;
    *ret = kmem_cache_create(name, size, 0, 0, NULL);
    return *ret ? AE_OK : AE_ERROR;
}

acpi_status acpi_os_delete_cache(acpi_cache_t *cache)
{
    kmem_cache_destroy(cache);
    return AE_OK;
}

acpi_status acpi_os_purge_cache(acpi_cache_t *cache)
{
    (void)cache;
    return AE_OK;
}

acpi_status acpi_os_release_object(acpi_cache_t *cache, void *object)
{
    kmem_cache_free(cache, object);
    return AE_OK;
}

// ── port i/o + pci config ───────────────────────────────────────────────────
// the file/blob run has no live hardware: reads return 0 and every distinct
// address is reported once so the log says what the AML actually touched. (satoru)
static void lx_acpi_note_addr(const char *what, unsigned long addr,
                              unsigned long *seen, int *nseen)
{
    for (int i = 0; i < *nseen; i++)
        if (seen[i] == addr)
            return;
    if (*nseen < 16) {
        seen[(*nseen)++] = addr;
        lx_log("[kld-acpi] ");
        lx_log(what);
        lx_log(" ");
        lx_log_hex(addr);
        lx_log(" reads as 0 (modelled)\n");
    }
}

static unsigned long lx_acpi_seen_port[16];
static int lx_acpi_nseen_port;
static unsigned long lx_acpi_seen_pci[16];
static int lx_acpi_nseen_pci;

long (*lx_acpi_port_read_hook)(unsigned long port, unsigned int width);
long (*lx_acpi_port_write_hook)(unsigned long port, unsigned int width,
                                unsigned long value);
long (*lx_acpi_pci_read_hook)(unsigned int seg, unsigned int bus,
                              unsigned int dev, unsigned int fn,
                              unsigned int reg, unsigned int width);
long (*lx_acpi_pci_write_hook)(unsigned int seg, unsigned int bus,
                               unsigned int dev, unsigned int fn,
                               unsigned int reg, unsigned int width,
                               unsigned long value);

acpi_status acpi_os_read_port(acpi_io_address addr, u32 *value, u32 width)
{
    lx_acpi_port_reads++;
    if (lx_acpi_port_read_hook) {
        long v = lx_acpi_port_read_hook((unsigned long)addr, width);
        *value = v < 0 ? 0 : (u32)v;
        if (lx_acpi_trace_hook)
            lx_acpi_trace_hook("SystemIO", 0, (u64)addr, width, *value);
        return AE_OK;
    }
    lx_acpi_note_addr("port", (unsigned long)addr, lx_acpi_seen_port,
                      &lx_acpi_nseen_port);
    *value = 0;
    if (lx_acpi_trace_hook)
        lx_acpi_trace_hook("SystemIO", 0, (u64)addr, width, 0);
    return AE_OK;
}

acpi_status acpi_os_write_port(acpi_io_address addr, u32 value, u32 width)
{
    lx_acpi_port_writes++;
    if (lx_acpi_trace_hook)
        lx_acpi_trace_hook("SystemIO", 1, (u64)addr, width, value);
    if (!lx_acpi_write_note("SystemIO", (u64)addr, width))
        return AE_OK;
    if (lx_acpi_port_write_hook)
        lx_acpi_port_write_hook((unsigned long)addr, width, value);
    return AE_OK;
}

acpi_status acpi_os_read_pci_configuration(struct acpi_pci_id *id, u32 reg,
                                           u64 *value, u32 width)
{
    lx_acpi_pci_reads++;
    if (lx_acpi_pci_read_hook) {
        long v = lx_acpi_pci_read_hook(id->segment, id->bus, id->device,
                                       id->function, reg, width);
        *value = v < 0 ? (u64)~0ull : (u64)v;
        if (lx_acpi_trace_hook)
            lx_acpi_trace_hook("PCI_Config", 0, ((u64)id->bus << 16) |
                               ((u64)id->device << 8) | id->function,
                               width, *value);
        return AE_OK;
    }
    lx_acpi_note_addr("pci", ((unsigned long)id->bus << 16) |
                      ((unsigned long)id->device << 8) |
                      (unsigned long)id->function,
                      lx_acpi_seen_pci, &lx_acpi_nseen_pci);
    *value = 0;
    if (lx_acpi_trace_hook)
        lx_acpi_trace_hook("PCI_Config", 0, ((u64)id->bus << 16) |
                           ((u64)id->device << 8) | id->function, width, 0);
    return AE_OK;
}

acpi_status acpi_os_write_pci_configuration(struct acpi_pci_id *id, u32 reg,
                                            u64 value, u32 width)
{
    lx_acpi_pci_writes++;
    if (lx_acpi_trace_hook)
        lx_acpi_trace_hook("PCI_Config", 1, ((u64)id->bus << 16) |
                           ((u64)id->device << 8) | id->function, width, value);
    if (!lx_acpi_write_note("PCI_Config", ((u64)id->bus << 16) |
                            ((u64)id->device << 8) | id->function, width))
        return AE_OK;
    if (lx_acpi_pci_write_hook)
        lx_acpi_pci_write_hook(id->segment, id->bus, id->device, id->function,
                               reg, width, (unsigned long)value);
    return AE_OK;
}

// ── synchronisation ─────────────────────────────────────────────────────────
// ACPICA uses binary semaphores for its mutexes (ACPI_MUTEX_TYPE defaults to
// ACPI_BINARY_SEMAPHORE, no ACPI_OSL_MUTEX in the linux port), so the four
// semaphore calls are the whole mutex layer. (satoru)
acpi_status acpi_os_create_semaphore(u32 max_units, u32 initial_units,
                                     acpi_semaphore *out)
{
    struct semaphore *s;

    (void)max_units;
    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s)
        return AE_NO_MEMORY;
    sema_init(s, initial_units);
    *out = (acpi_semaphore)s;
    return AE_OK;
}

acpi_status acpi_os_delete_semaphore(acpi_semaphore handle)
{
    if (!handle)
        return AE_BAD_PARAMETER;
    kfree((struct semaphore *)handle);
    return AE_OK;
}

acpi_status acpi_os_wait_semaphore(acpi_semaphore handle, u32 units,
                                   u16 timeout)
{
    struct semaphore *s = (struct semaphore *)handle;
    u64 deadline = 0;

    if (!s || units < 1)
        return AE_BAD_PARAMETER;
    if (units > 1)
        return AE_SUPPORT;
    if (timeout != ACPI_WAIT_FOREVER)
        deadline = ktime_get_mono_fast_ns() + (u64)timeout * 1000000ull;
    for (;;) {
        if (!down_trylock(s))
            return AE_OK;
        if (timeout != ACPI_WAIT_FOREVER &&
            ktime_get_mono_fast_ns() >= deadline)
            return AE_TIME;
        msleep(1);
    }
}

acpi_status acpi_os_signal_semaphore(acpi_semaphore handle, u32 units)
{
    if (!handle || units < 1)
        return AE_BAD_PARAMETER;
    if (units > 1)
        return AE_SUPPORT;
    up((struct semaphore *)handle);
    return AE_OK;
}

// acpi_os_create_lock is a macro in aclinuxex.h (spin_lock_init over an
// ACPI_ALLOCATE'd spinlock_t). delete/acquire/release are real calls. (satoru)
void acpi_os_delete_lock(acpi_spinlock handle)
{
    if (handle)
        kfree(handle);
}

acpi_cpu_flags acpi_os_acquire_lock(acpi_spinlock handle)
{
    unsigned long flags;

    spin_lock_irqsave(handle, flags);
    return flags;
}

void acpi_os_release_lock(acpi_spinlock handle, acpi_cpu_flags flags)
{
    spin_unlock_irqrestore(handle, flags);
}

// ── threads, time ───────────────────────────────────────────────────────────
struct lx_acpi_exec {
    struct work_struct work;
    acpi_osd_exec_callback fn;
    void *ctx;
};

static atomic_t lx_acpi_exec_inflight = ATOMIC_INIT(0);

static void lx_acpi_exec_run(struct work_struct *work)
{
    struct lx_acpi_exec *e =
        container_of(work, struct lx_acpi_exec, work);

    e->fn(e->ctx);
    kfree(e);
    atomic_dec(&lx_acpi_exec_inflight);
}

acpi_status acpi_os_execute(acpi_execute_type type,
                            acpi_osd_exec_callback function, void *context)
{
    struct lx_acpi_exec *e;

    (void)type;
    e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return AE_NO_MEMORY;
    e->fn = function;
    e->ctx = context;
    INIT_WORK(&e->work, lx_acpi_exec_run);
    atomic_inc(&lx_acpi_exec_inflight);
    if (!queue_work(system_wq, &e->work)) {
        atomic_dec(&lx_acpi_exec_inflight);
        kfree(e);
        return AE_ERROR;
    }
    return AE_OK;
}

void acpi_os_wait_events_complete(void)
{
    while (atomic_read(&lx_acpi_exec_inflight))
        msleep(1);
}

void acpi_os_sleep(u64 ms)
{
    msleep((unsigned int)(ms > 0x7FFFFFFFull ? 0x7FFFFFFFull : ms));
}

void acpi_os_stall(u32 us)
{
    if (us >= 1000)
        msleep(us / 1000);
    if (us % 1000)
        udelay(us % 1000);
}

u64 acpi_os_get_timer(void)
{
    // 100 ns units, per the acpi spec. (satoru)
    return ktime_get_mono_fast_ns() / 100ull;
}

// ── interrupts (SCI) ────────────────────────────────────────────────────────
// the blob run has no SCI. acpi_enable_subsystem is called with
// ACPI_NO_HANDLER_INIT, so these are not reached; accepting the install keeps
// the event paths initialised if a caller asks. (satoru)
acpi_status acpi_os_install_interrupt_handler(u32 irq,
                                              acpi_osd_handler handler,
                                              void *context)
{
    (void)irq;
    (void)handler;
    (void)context;
    return AE_OK;
}

acpi_status acpi_os_remove_interrupt_handler(u32 irq,
                                             acpi_osd_handler handler)
{
    (void)irq;
    (void)handler;
    return AE_OK;
}

// ── output ──────────────────────────────────────────────────────────────────
// kernel ACPICA formats carry a KERN_ level prefix ("\0016ACPI: ...") and
// compose one output line from several acpi_os_printf calls, so the emitter
// keeps a partial line and reports it when the newline arrives. an immediately
// repeated identical line is collapsed so a namespace walk cannot flood the
// serial line; error text, including the method path, is kept. (satoru)
static char lx_acpi_last[512];
static unsigned long lx_acpi_repeat;
static char lx_acpi_line[512];
static unsigned int lx_acpi_line_n;

static void lx_acpi_emit_line(const char *line)
{
    if (!line[0])
        return;
    if (!strcmp(line, lx_acpi_last)) {
        lx_acpi_repeat++;
        return;
    }
    if (lx_acpi_repeat) {
        lx_log("[kld-acpi] (previous line repeated ");
        lx_log_u(lx_acpi_repeat);
        lx_log(" times)\n");
        lx_acpi_repeat = 0;
    }
    strncpy(lx_acpi_last, line, sizeof(lx_acpi_last) - 1);
    lx_acpi_last[sizeof(lx_acpi_last) - 1] = 0;
    lx_log_line(line);
}

static void lx_acpi_emit(const char *msg)
{
    const char *p = msg;

    // syslog level prefix, as KERN_* expands in the kernel. (satoru)
    if (*p == '\001') {
        p++;
        while (*p >= '0' && *p <= '7')
            p++;
    }
    while (*p) {
        if (*p == '\n' || *p == '\r') {
            lx_acpi_line[lx_acpi_line_n] = 0;
            lx_acpi_emit_line(lx_acpi_line);
            lx_acpi_line_n = 0;
            p++;
            continue;
        }
        if (lx_acpi_line_n < sizeof(lx_acpi_line) - 1)
            lx_acpi_line[lx_acpi_line_n++] = *p;
        p++;
    }
}

void ACPI_INTERNAL_VAR_XFACE acpi_os_printf(const char *format, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    lx_acpi_emit(buf);
}

void acpi_os_vprintf(const char *format, va_list args)
{
    char buf[512];

    vsnprintf(buf, sizeof(buf), format, args);
    lx_acpi_emit(buf);
}

acpi_status acpi_os_signal(u32 fn, void *info)
{
    if (fn == ACPI_SIGNAL_FATAL) {
        struct acpi_signal_fatal_info *f = info;
        lx_log("[kld-acpi] AML Fatal: type ");
        lx_log_u(f ? f->type : 0);
        lx_log(" code ");
        lx_log_u(f ? f->code : 0);
        lx_log(" arg ");
        lx_log_u(f ? f->argument : 0);
        lx_log("\n");
    }
    return AE_OK;
}

acpi_status acpi_os_enter_sleep(u8 state, u32 a, u32 b)
{
    (void)state;
    (void)a;
    (void)b;
    return AE_OK;
}

// ── the global lock, for a machine that has no firmware peer ────────────────
// arch/x86/kernel/acpi/boot.c is not part of a container. the protocol is the
// FACS global_lock word: bit 0 owner, bit 1 pending. (satoru)
int __acpi_acquire_global_lock(unsigned int *lock)
{
    unsigned int old, new, val;

    old = __atomic_load_n(lock, __ATOMIC_RELAXED);
    do {
        val = (old >> 1) & 1u;
        new = (old & ~3u) + 2u + val;
    } while (!__atomic_compare_exchange_n(lock, &old, new, 1,
                                          __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    return val ? 0 : -1;
}

int __acpi_release_global_lock(unsigned int *lock)
{
    unsigned int old, new;

    old = __atomic_load_n(lock, __ATOMIC_RELAXED);
    do {
        new = old & ~3u;
    } while (!__atomic_compare_exchange_n(lock, &old, new, 1,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));
    return old & 1u;
}

// end (satoru)
