// kld-acpi: the ACPICA OS services layer (acpi_os*) for the KLD ACPI container.
//
// ACPICA is imported UNMODIFIED from linux 6.12.99 (drivers/acpi/acpica, the
// same code as upstream ACPICA, BSD/GPL dual) and compiled in USERSPACE mode
// (no __KERNEL__): include/acpi/platform/aclinux.h then selects standard
// headers, which is how linux's own tools/power/acpi builds it. Everything the
// interpreter needs from an OS comes through this file. Two backends:
//
//   FILE   tables come from acpidump output files. physical memory is simulated:
//          the tables sit at synthetic physical addresses behind a synthesized
//          RSDP/XSDT (the acpiexec design), and any other SystemMemory address an
//          opregion touches is a zero-filled sparse page. port and pci config
//          reads return 0. this is how the laptop's DSDT is exercised from a
//          host without touching its hardware - methods EVALUATE, but anything
//          that depends on live register values sees zeros, and that is said in
//          the output, not hidden.
//   BROKER (inside Kurono) the RSDP, physical mappings, port i/o and pci config
//          come from the kernel broker, restricted to what ACPI needs.
//
// end (satoru)

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <acpi/acpi.h>
#include "kld_acpi.h"

// ── synthetic physical memory (file mode) ───────────────────────────────────
struct kld_phys_region {
    acpi_physical_address base;
    acpi_size len;
    unsigned char *host;
};

#define KLD_MAX_REGIONS 4096
static struct kld_phys_region kld_regions[KLD_MAX_REGIONS];
static int kld_nregions;
static pthread_mutex_t kld_phys_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned long kld_sim_pages;     // pages synthesized for opregions (reported) (satoru)
unsigned long kld_port_reads, kld_port_writes, kld_pci_reads, kld_pci_writes;

int kld_osl_broker_mode;         // set by main in broker mode (satoru)
acpi_physical_address kld_osl_rsdp;

void kld_phys_add(acpi_physical_address base, void *host, acpi_size len)
{
    pthread_mutex_lock(&kld_phys_lock);
    if (kld_nregions < KLD_MAX_REGIONS) {
        kld_regions[kld_nregions].base = base;
        kld_regions[kld_nregions].len = len;
        kld_regions[kld_nregions].host = host;
        kld_nregions++;
    }
    pthread_mutex_unlock(&kld_phys_lock);
}

static void *kld_phys_lookup(acpi_physical_address addr, acpi_size len, int create)
{
    void *r = NULL;
    pthread_mutex_lock(&kld_phys_lock);
    for (int i = 0; i < kld_nregions; i++) {
        struct kld_phys_region *g = &kld_regions[i];
        if (addr >= g->base && addr + len <= g->base + g->len) {
            r = g->host + (addr - g->base);
            break;
        }
    }
    if (!r && create && kld_nregions < KLD_MAX_REGIONS) {
        // a SystemMemory opregion outside every table: simulate it as zeroed
        // memory, page granular, persistent for the run. (satoru)
        acpi_physical_address pb = addr & ~0xFFFull;
        acpi_size pl = ((addr + len + 0xFFF) & ~0xFFFull) - pb;
        unsigned char *h = calloc(1, pl);
        if (h) {
            kld_regions[kld_nregions].base = pb;
            kld_regions[kld_nregions].len = pl;
            kld_regions[kld_nregions].host = h;
            kld_nregions++;
            kld_sim_pages += pl / 4096;
            r = h + (addr - pb);
        }
    }
    pthread_mutex_unlock(&kld_phys_lock);
    return r;
}

// broker backend hooks (kld_acpi_broker.c in Kurono builds; weak NULLs here
// keep the host build self-contained). (satoru)
void *kld_broker_map_phys(acpi_physical_address addr, acpi_size len) __attribute__((weak));
long  kld_broker_port_read(unsigned long port, unsigned width) __attribute__((weak));
long  kld_broker_port_write(unsigned long port, unsigned width, unsigned long val) __attribute__((weak));
long  kld_broker_pci_read(unsigned seg, unsigned bus, unsigned dev, unsigned fn, unsigned reg, unsigned width) __attribute__((weak));
long  kld_broker_pci_write(unsigned seg, unsigned bus, unsigned dev, unsigned fn, unsigned reg, unsigned width, unsigned long val) __attribute__((weak));

// ── init / tables ───────────────────────────────────────────────────────────
acpi_status acpi_os_initialize(void) { return AE_OK; }
acpi_status acpi_os_terminate(void) { return AE_OK; }

acpi_physical_address acpi_os_get_root_pointer(void) { return kld_osl_rsdp; }

acpi_status acpi_os_predefined_override(const struct acpi_predefined_names *init, acpi_string *nv)
{
    (void)init;
    *nv = NULL;
    return AE_OK;
}

acpi_status acpi_os_table_override(struct acpi_table_header *existing, struct acpi_table_header **nt)
{
    (void)existing;
    *nt = NULL;
    return AE_OK;
}

acpi_status acpi_os_physical_table_override(struct acpi_table_header *existing, acpi_physical_address *addr, u32 *len)
{
    (void)existing;
    *addr = 0;
    *len = 0;
    return AE_OK;
}

// ── memory ──────────────────────────────────────────────────────────────────
void *acpi_os_map_memory(acpi_physical_address where, acpi_size length)
{
    if (kld_osl_broker_mode && kld_broker_map_phys) {
        // the rsdp is a local copy registered at a synthetic address. (satoru)
        void *local = kld_phys_lookup(where, length ? length : 1, 0);
        return local ? local : kld_broker_map_phys(where, length);
    }
    return kld_phys_lookup(where, length ? length : 1, 1);
}

void acpi_os_unmap_memory(void *where, acpi_size length) { (void)where; (void)length; }

void *acpi_os_allocate(acpi_size size) { return malloc(size ? size : 1); }
void acpi_os_free(void *mem) { free(mem); }

acpi_status acpi_os_get_physical_address(void *logical, acpi_physical_address *physical)
{
    (void)logical;
    *physical = 0;
    return AE_SUPPORT;
}

u8 acpi_os_readable(void *ptr, acpi_size len) { (void)ptr; (void)len; return TRUE; }
u8 acpi_os_writable(void *ptr, acpi_size len) { (void)ptr; (void)len; return TRUE; }

acpi_status acpi_os_read_memory(acpi_physical_address addr, u64 *value, u32 width)
{
    void *p = acpi_os_map_memory(addr, width / 8);
    if (!p) { *value = 0; return AE_BAD_ADDRESS; }
    switch (width) {
    case 8:  *value = *(volatile u8 *)p; break;
    case 16: *value = *(volatile u16 *)p; break;
    case 32: *value = *(volatile u32 *)p; break;
    case 64: *value = *(volatile u64 *)p; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

acpi_status acpi_os_write_memory(acpi_physical_address addr, u64 value, u32 width)
{
    void *p = acpi_os_map_memory(addr, width / 8);
    if (!p) return AE_BAD_ADDRESS;
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
// in the kernel these are kmem_cache wrappers (and linux's acpica object list
// leaves out ACPICA's own utcache.c). objects are handed out ZEROED, matching
// kmem_cache_zalloc in drivers/acpi/osl.c. (satoru)
struct kld_cache { acpi_size size; };

acpi_status acpi_os_create_cache(char *name, u16 size, u16 depth, acpi_cache_t **ret)
{
    (void)name; (void)depth;
    struct kld_cache *c = malloc(sizeof(*c));
    if (!c) return AE_NO_MEMORY;
    c->size = size;
    *ret = (acpi_cache_t *)c;
    return AE_OK;
}

acpi_status acpi_os_delete_cache(acpi_cache_t *cache) { free(cache); return AE_OK; }
acpi_status acpi_os_purge_cache(acpi_cache_t *cache) { (void)cache; return AE_OK; }

void *acpi_os_acquire_object(acpi_cache_t *cache)
{
    return calloc(1, ((struct kld_cache *)cache)->size);
}

acpi_status acpi_os_release_object(acpi_cache_t *cache, void *object)
{
    (void)cache;
    free(object);
    return AE_OK;
}

// ── port i/o + pci config ───────────────────────────────────────────────────
acpi_status acpi_os_read_port(acpi_io_address addr, u32 *value, u32 width)
{
    kld_port_reads++;
    if (kld_osl_broker_mode && kld_broker_port_read) {
        long v = kld_broker_port_read((unsigned long)addr, width);
        if (v < 0) { *value = 0xFFFFFFFFu >> (32 - width); return AE_OK; }
        *value = (u32)v;
        return AE_OK;
    }
    *value = 0;
    return AE_OK;
}

acpi_status acpi_os_write_port(acpi_io_address addr, u32 value, u32 width)
{
    kld_port_writes++;
    if (kld_osl_broker_mode && kld_broker_port_write)
        kld_broker_port_write((unsigned long)addr, width, value);
    return AE_OK;
}

acpi_status acpi_os_read_pci_configuration(struct acpi_pci_id *id, u32 reg, u64 *value, u32 width)
{
    kld_pci_reads++;
    if (kld_osl_broker_mode && kld_broker_pci_read) {
        long v = kld_broker_pci_read(id->segment, id->bus, id->device, id->function, reg, width);
        *value = v < 0 ? (u64)~0ull : (u64)v;
        return AE_OK;
    }
    *value = 0;
    return AE_OK;
}

acpi_status acpi_os_write_pci_configuration(struct acpi_pci_id *id, u32 reg, u64 value, u32 width)
{
    kld_pci_writes++;
    if (kld_osl_broker_mode && kld_broker_pci_write)
        kld_broker_pci_write(id->segment, id->bus, id->device, id->function, reg, width, (unsigned long)value);
    return AE_OK;
}

// ── synchronisation (real: ACPICA is called from several threads) ───────────
acpi_status acpi_os_create_semaphore(u32 max, u32 initial, acpi_semaphore *out)
{
    (void)max;
    sem_t *s = malloc(sizeof(*s));
    if (!s || sem_init(s, 0, initial)) { free(s); return AE_NO_MEMORY; }
    *out = s;
    return AE_OK;
}

acpi_status acpi_os_delete_semaphore(acpi_semaphore h)
{
    if (!h) return AE_BAD_PARAMETER;
    sem_destroy((sem_t *)h);
    free(h);
    return AE_OK;
}

acpi_status acpi_os_wait_semaphore(acpi_semaphore h, u32 units, u16 timeout)
{
    sem_t *s = (sem_t *)h;
    (void)units;
    if (!s) return AE_BAD_PARAMETER;
    if (timeout == ACPI_WAIT_FOREVER) {
        while (sem_wait(s) && errno == EINTR) { }
        return AE_OK;
    }
    if (timeout == 0)
        return sem_trywait(s) ? AE_TIME : AE_OK;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout / 1000;
    ts.tv_nsec += (long)(timeout % 1000) * 1000000l;
    if (ts.tv_nsec >= 1000000000l) { ts.tv_sec++; ts.tv_nsec -= 1000000000l; }
    while (sem_timedwait(s, &ts)) {
        if (errno == ETIMEDOUT) return AE_TIME;
        if (errno != EINTR) return AE_ERROR;
    }
    return AE_OK;
}

acpi_status acpi_os_signal_semaphore(acpi_semaphore h, u32 units)
{
    if (!h) return AE_BAD_PARAMETER;
    while (units--) sem_post((sem_t *)h);
    return AE_OK;
}

acpi_status acpi_os_create_lock(acpi_spinlock *out)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return AE_NO_MEMORY;
    pthread_mutex_init(m, NULL);
    *out = m;
    return AE_OK;
}

void acpi_os_delete_lock(acpi_spinlock h) { if (h) { pthread_mutex_destroy(h); free(h); } }

acpi_cpu_flags acpi_os_acquire_lock(acpi_spinlock h)
{
    pthread_mutex_lock(h);
    return 0;
}

void acpi_os_release_lock(acpi_spinlock h, acpi_cpu_flags flags)
{
    (void)flags;
    pthread_mutex_unlock(h);
}

// ── threads, time ───────────────────────────────────────────────────────────
acpi_thread_id acpi_os_get_thread_id(void)
{
    // must never be 0 (ACPICA reserves it) and must be unique per thread. (satoru)
    return (acpi_thread_id)(uintptr_t)pthread_self() | 1;
}

struct kld_exec { acpi_osd_exec_callback fn; void *ctx; };
static volatile int kld_exec_inflight;

static void *kld_exec_thread(void *p)
{
    struct kld_exec e = *(struct kld_exec *)p;
    free(p);
    e.fn(e.ctx);
    __atomic_sub_fetch(&kld_exec_inflight, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

acpi_status acpi_os_execute(acpi_execute_type type, acpi_osd_exec_callback fn, void *ctx)
{
    (void)type;
    struct kld_exec *e = malloc(sizeof(*e));
    pthread_t t;
    if (!e) return AE_NO_MEMORY;
    e->fn = fn;
    e->ctx = ctx;
    __atomic_add_fetch(&kld_exec_inflight, 1, __ATOMIC_ACQ_REL);
    if (pthread_create(&t, NULL, kld_exec_thread, e)) {
        __atomic_sub_fetch(&kld_exec_inflight, 1, __ATOMIC_ACQ_REL);
        free(e);
        return AE_NO_MEMORY;
    }
    pthread_detach(t);
    return AE_OK;
}

void acpi_os_wait_events_complete(void)
{
    while (__atomic_load_n(&kld_exec_inflight, __ATOMIC_ACQUIRE))
        usleep(1000);
}

void acpi_os_sleep(u64 ms) { usleep((useconds_t)(ms * 1000)); }

void acpi_os_stall(u32 us)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000l + (b.tv_nsec - a.tv_nsec) / 1000 < (long)us);
}

u64 acpi_os_get_timer(void)
{
    // 100 ns units, per the acpi spec. (satoru)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 10000000ull + (u64)ts.tv_nsec / 100ull;
}

// ── interrupts (SCI) ────────────────────────────────────────────────────────
// the SCI is not delivered yet: in file mode there is no hardware, in broker
// mode it needs a broker legacy-irq op. accepting the install keeps the event
// subsystem initialised; GPEs simply never fire. (satoru)
acpi_status acpi_os_install_interrupt_handler(u32 irq, acpi_osd_handler h, void *ctx)
{
    (void)irq; (void)h; (void)ctx;
    return AE_OK;
}

acpi_status acpi_os_remove_interrupt_handler(u32 irq, acpi_osd_handler h)
{
    (void)irq; (void)h;
    return AE_OK;
}

// ── output, misc ────────────────────────────────────────────────────────────
int kld_acpi_quiet;

void ACPI_INTERNAL_VAR_XFACE acpi_os_printf(const char *fmt, ...)
{
    va_list ap;
    if (kld_acpi_quiet) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void acpi_os_vprintf(const char *fmt, va_list ap)
{
    if (kld_acpi_quiet) return;
    vfprintf(stderr, fmt, ap);
}

acpi_status acpi_os_signal(u32 fn, void *info)
{
    if (fn == ACPI_SIGNAL_FATAL) {
        struct acpi_signal_fatal_info *f = info;
        fprintf(stderr, "[kld-acpi] AML Fatal: type %u code %u arg %u\n",
                f ? f->type : 0, f ? f->code : 0, f ? f->argument : 0);
    }
    return AE_OK;
}

acpi_status acpi_os_enter_sleep(u8 state, u32 a, u32 b)
{
    (void)state; (void)a; (void)b;
    return AE_OK;
}

// end (satoru)
