// kernel services the i915 probe path needs (d4.1), drm families only. the
// list and the call sites are in scratch/kld/gfx/survey_stubs.md. each one is
// either the upstream semantics on the container's own state, or the answer
// upstream gives when the subsystem is not configured (sysfs, iosf mbi, vga
// arbiter, perf, acpi video), which every caller handles. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kernfs.h>
#include <linux/pm_runtime.h>
#include <linux/rtmutex.h>
#include <linux/kthread.h>
#include <linux/irq_work.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/i2c.h>
#include <linux/uuid.h>
#include <asm/hypervisor.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/refcount.h>
#include <linux/perf_event.h>
#include <linux/fwnode.h>
#include <linux/property.h>
#include <linux/vgaarb.h>
#include <linux/interrupt.h>
#include <linux/backlight.h>
#include <acpi/video.h>
#include <linux/irq.h>
#include <linux/suspend.h>
#include <linux/pci.h>
#include <linux/relay.h>
#include <linux/moduleparam.h>

#include "lx_kshim.h"

// ── clocks (every register poll of i915 reads one of these) ─────────────────
ktime_t ktime_get_raw(void) { return (ktime_t)lx_uptime_ns(); }
u64 ktime_get_raw_fast_ns(void) { return lx_uptime_ns(); }
u64 local_clock(void) { return lx_uptime_ns(); }

// timer coalescing hint: no coalescing, the exact relative value. (satoru)
unsigned long round_jiffies_up_relative(unsigned long j) { return j; }

// ── data objects ────────────────────────────────────────────────────────────
// a container is not a guest of a hypervisor it could see. (satoru)
enum x86_hypervisor_type x86_hyper_type = X86_HYPER_NATIVE;
atomic_long_t _totalram_pages;
atomic_long_t nr_swap_pages = ATOMIC_LONG_INIT(0);   // no swap (satoru)
unsigned int nr_cpu_ids = 1;
// the stolen memory early quirk only runs on bare metal kernels: the empty
// resource it starts as. (satoru)
struct resource intel_graphics_stolen_res = DEFINE_RES_MEM(0, 0);
int __i2c_first_dynamic_bus_num;
LIST_HEAD(__i2c_board_list);
DECLARE_RWSEM(__i2c_board_lock);
const char power_group_name[] = "power";
const guid_t guid_null;
const int sysctl_vals[] = { 0, 1, 2, 3, 4, 100, 200, 1000, 3000, INT_MAX, 65535, -1 };

// review turn-5 data-symbol audit (scratch/kld/gfx/survey_datastubs.md): these
// are DATA in Linux but were function stubs, so a read got instruction bytes.
// the five i915 ones with real loads are defined here; the address-only and
// jump-label-only ones get a real object so nothing reads code as data. (satoru)
struct resource iomem_resource = { .name = "PCI mem", .start = 0, .end = -1, .flags = IORESOURCE_MEM };
struct resource ioport_resource = { .name = "PCI IO", .start = 0, .end = 0xffff, .flags = IORESOURCE_IO };
pteval_t __supported_pte_mask = ~(pteval_t)0;
unsigned int tsc_khz = 2000000;
suspend_state_t pm_suspend_target_state;
unsigned long pci_mem_start = 0xaeedbabe;
const char *pci_power_names[] = { "error", "D0", "D1", "D2", "D3hot", "D3cold", "unknown" };
const struct file_operations relay_file_operations;
struct static_key_false init_on_free;                         // key false: branch is a nop (satoru)
// dummy_irq_chip, param_ops_charp, __SCK__might_resched: lx_aux_providers.c
// (main) defines them for every family. (satoru)

extern unsigned long lx_os_phys_pages(void);
extern long lx_os_ncpus(void);
__attribute__((constructor)) static void lx_kapi_data_init(void)
{
    long n = lx_os_ncpus();
    atomic_long_set(&_totalram_pages, (long)lx_os_phys_pages());
    if (n > 0)
        nr_cpu_ids = (unsigned int)n;
}

// ── taint, memory helpers ──────────────────────────────────────────────────
static unsigned long lx_tainted_mask;
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
    (void)lockdep_ok;
    set_bit(flag, &lx_tainted_mask);
    lx_log("[lx_kapi] kernel tainted, flag ");
    lx_log_u(flag);
    lx_log("\n");
}

void *kmemdup_array(const void *src, size_t count, size_t element_size, gfp_t gfp)
{
    size_t n;
    if (check_mul_overflow(count, element_size, &n))
        return NULL;
    return kmemdup(src, n, gfp);
}

// write-back is the only page attribute container mappings have today; pat
// itself is on, so wc requests are honoured as wb mappings (d4.3 item:
// OBJ_SET_CACHING). (satoru)
bool pat_enabled(void) { return true; }
int arch_phys_wc_add(unsigned long base, unsigned long size) { (void)base; (void)size; return 0; }
void arch_phys_wc_del(int handle) { (void)handle; }

// no page cache writeback in the container: nothing to mark. (satoru)
bool set_page_dirty(struct page *page) { (void)page; return false; }

size_t strlcat(char *dest, const char *src, size_t count)
{
    size_t dsize = strlen(dest), len = strlen(src), res = dsize + len;
    if (dsize >= count)
        return res;
    dest += dsize;
    count -= dsize;
    if (len >= count)
        len = count - 1;
    memcpy(dest, src, len);
    dest[len] = 0;
    return res;
}

void guid_gen(guid_t *lu)
{
    get_random_bytes(lu, sizeof(guid_t));
    lu->b[7] = (lu->b[7] & 0x0F) | 0x40;
    lu->b[8] = (lu->b[8] & 0x3F) | 0x80;
}

void yield(void) { lx_os_yield(); }

// ── locks ───────────────────────────────────────────────────────────────────
int _atomic_dec_and_lock_irqsave(atomic_t *atomic, spinlock_t *lock, unsigned long *flags)
{
    if (atomic_add_unless(atomic, -1, 1))
        return 0;
    spin_lock_irqsave(lock, *flags);
    if (atomic_dec_and_test(atomic))
        return 1;
    spin_unlock_irqrestore(lock, *flags);
    return 0;
}

// lib/refcount.c refcount_dec_not_one: false when the count is 1. (satoru)
static bool lx_refcount_dec_not_one(refcount_t *r)
{
    int val = atomic_read(&r->refs);
    for (;;) {
        if (val == 1)
            return false;
        if (atomic_try_cmpxchg_release(&r->refs, &val, val - 1))
            return true;
    }
}
#define refcount_dec_not_one lx_refcount_dec_not_one

bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock)
{
    if (refcount_dec_not_one(r))
        return false;
    spin_lock(lock);
    if (!refcount_dec_and_test(r)) {
        spin_unlock(lock);
        return false;
    }
    return true;
}

bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock, unsigned long *flags)
{
    if (refcount_dec_not_one(r))
        return false;
    spin_lock_irqsave(lock, *flags);
    if (!refcount_dec_and_test(r)) {
        spin_unlock_irqrestore(lock, *flags);
        return false;
    }
    return true;
}

// rt_mutex, runtime pm enable/allow/autosuspend, buses/drivers, sysfs dirs and
// groups, irq_work_sync, device_get_next_child_node, ktime_get_mono_fast_ns:
// provided for every family by main's shim (lx_bus.c, lx_pm.c, lx_misc.c,
// lx_irqdomain.c) since the touchpad merge. (satoru)

// ── runtime pm (the container's device never suspends; these keep the
// counters the inline helpers and get_if_* read) ─────────────────────────────
static int lx_rpm_get_conditional(struct device *dev, bool ign_usage_count)
{
    if (dev->power.disable_depth > 0)
        return -EINVAL;
    if (dev->power.runtime_status != RPM_ACTIVE)
        return 0;
    if (!ign_usage_count && atomic_read(&dev->power.usage_count) == 0)
        return 0;
    atomic_inc(&dev->power.usage_count);
    return 1;
}
int pm_runtime_get_if_in_use(struct device *dev) { return lx_rpm_get_conditional(dev, false); }
int pm_runtime_get_if_active(struct device *dev) { return lx_rpm_get_conditional(dev, true); }

// ── kthread workers (drm vblank works) ─────────────────────────────────────
static int lx_kworker_fn(void *arg)
{
    struct kthread_worker *w = arg;
    for (;;) {
        struct kthread_work *work = NULL;
        if (kthread_should_stop())
            break;
        raw_spin_lock_irq(&w->lock);
        if (!list_empty(&w->work_list)) {
            work = list_first_entry(&w->work_list, struct kthread_work, node);
            list_del_init(&work->node);
        }
        w->current_work = work;
        raw_spin_unlock_irq(&w->lock);
        if (work) {
            work->func(work);
            raw_spin_lock_irq(&w->lock);
            w->current_work = NULL;
            raw_spin_unlock_irq(&w->lock);
            continue;
        }
        lx_task_park(-1);
    }
    return 0;
}

struct kthread_worker *kthread_create_worker(unsigned int flags, const char namefmt[], ...)
{
    char name[TASK_COMM_LEN];
    va_list ap;
    struct kthread_worker *w = kzalloc(sizeof(*w), GFP_KERNEL);
    struct task_struct *t;
    if (!w)
        return ERR_PTR(-ENOMEM);
    w->flags = flags;
    raw_spin_lock_init(&w->lock);
    INIT_LIST_HEAD(&w->work_list);
    INIT_LIST_HEAD(&w->delayed_work_list);
    va_start(ap, namefmt);
    vsnprintf(name, sizeof(name), namefmt, ap);
    va_end(ap);
    t = kthread_create_on_node(lx_kworker_fn, w, NUMA_NO_NODE, "%s", name);
    if (IS_ERR(t)) {
        kfree(w);
        return ERR_CAST(t);
    }
    w->task = t;
    wake_up_process(t);
    return w;
}

bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
    unsigned long flags;
    bool ret = false;
    raw_spin_lock_irqsave(&worker->lock, flags);
    if (list_empty(&work->node) && !work->canceling) {
        work->worker = worker;
        list_add_tail(&work->node, &worker->work_list);
        ret = true;
    }
    raw_spin_unlock_irqrestore(&worker->lock, flags);
    if (ret)
        wake_up_process(worker->task);
    return ret;
}

struct lx_kflush { struct kthread_work work; struct completion done; };
static void lx_kflush_fn(struct kthread_work *work)
{
    complete(&container_of(work, struct lx_kflush, work)->done);
}

void kthread_flush_worker(struct kthread_worker *worker)
{
    struct lx_kflush f;
    kthread_init_work(&f.work, lx_kflush_fn);
    init_completion(&f.done);
    kthread_queue_work(worker, &f.work);
    wait_for_completion(&f.done);
}

// waits for the work if it is queued or running; works queued behind it are
// not waited for (upstream inserts the barrier right after it, same effect for
// the work itself). (satoru)
void kthread_flush_work(struct kthread_work *work)
{
    struct kthread_worker *w = work->worker;
    bool busy;
    if (!w)
        return;
    for (;;) {
        raw_spin_lock_irq(&w->lock);
        busy = !list_empty(&work->node) || w->current_work == work;
        raw_spin_unlock_irq(&w->lock);
        if (!busy)
            return;
        lx_os_yield();
    }
}

bool kthread_cancel_work_sync(struct kthread_work *work)
{
    struct kthread_worker *w = work->worker;
    bool pending = false;
    if (!w)
        return false;
    raw_spin_lock_irq(&w->lock);
    work->canceling++;
    if (!list_empty(&work->node)) {
        list_del_init(&work->node);
        pending = true;
    }
    raw_spin_unlock_irq(&w->lock);
    kthread_flush_work(work);
    raw_spin_lock_irq(&w->lock);
    work->canceling--;
    raw_spin_unlock_irq(&w->lock);
    return pending;
}

void kthread_destroy_worker(struct kthread_worker *worker)
{
    kthread_flush_worker(worker);
    kthread_stop(worker->task);
    kfree(worker);
}

// irq_work_queue runs the callback synchronously in this shim: wait until no
// caller is inside it. (satoru)

// ── wait on a variable (upstream kernel/sched/wait_bit.c semantics) ─────────
#define LX_VAR_BITS 8
static wait_queue_head_t lx_var_table[1 << LX_VAR_BITS];
static int lx_var_inited;

wait_queue_head_t *__var_waitqueue(void *p)
{
    if (!__atomic_load_n(&lx_var_inited, __ATOMIC_ACQUIRE)) {
        static volatile int once;
        if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
            for (int i = 0; i < (1 << LX_VAR_BITS); i++)
                init_waitqueue_head(&lx_var_table[i]);
            __atomic_store_n(&lx_var_inited, 1, __ATOMIC_RELEASE);
        }
        while (!__atomic_load_n(&lx_var_inited, __ATOMIC_ACQUIRE))
            lx_os_yield();
    }
    return &lx_var_table[hash_ptr(p, LX_VAR_BITS)];
}

static int lx_var_wake_function(struct wait_queue_entry *wq_entry, unsigned int mode,
                                int sync, void *arg)
{
    struct wait_bit_key *key = arg;
    struct wait_bit_queue_entry *wbq_entry =
        container_of(wq_entry, struct wait_bit_queue_entry, wq_entry);
    if (wbq_entry->key.flags != key->flags || wbq_entry->key.bit_nr != key->bit_nr)
        return 0;
    return autoremove_wake_function(wq_entry, mode, sync, key);
}

void init_wait_var_entry(struct wait_bit_queue_entry *wbq_entry, void *var, int flags)
{
    *wbq_entry = (struct wait_bit_queue_entry){
        .key = { .flags = (var), .bit_nr = -1, },
        .wq_entry = {
            .flags = flags,
            .private = current,
            .func = lx_var_wake_function,
            .entry = LIST_HEAD_INIT(wbq_entry->wq_entry.entry),
        },
    };
}

void wake_up_var(void *var)
{
    struct wait_bit_key key = { .flags = var, .bit_nr = -1 };
    wait_queue_head_t *wq = __var_waitqueue(var);
    if (waitqueue_active(wq))
        __wake_up(wq, TASK_NORMAL, 1, &key);
}

int wake_up_state(struct task_struct *p, unsigned int state)
{
    (void)state;
    return wake_up_process(p);
}

long io_schedule_timeout(long timeout) { return schedule_timeout(timeout); }

// ── rcu grace-period cookies: no cookie is ever proven elapsed, so the
// conditional form always waits (correct, never early). (satoru)
unsigned long get_state_synchronize_rcu(void) { return 0; }
void cond_synchronize_rcu(unsigned long oldstate) { (void)oldstate; synchronize_rcu(); }

// ── workqueue drain ────────────────────────────────────────────────────────
void drain_workqueue(struct workqueue_struct *wq) { __flush_workqueue(wq); }

// ── sysfs / kernfs: the container has no sysfs. these are the answers of a
// kernel built without CONFIG_SYSFS (include/linux/sysfs.h), which every
// caller handles: success, nothing created, kobj->sd stays NULL. (satoru)
int sysfs_rename_dir_ns(struct kobject *kobj, const char *new_name, const void *new_ns)
{ (void)kobj; (void)new_name; (void)new_ns; return 0; }
int sysfs_move_dir_ns(struct kobject *kobj, struct kobject *new_parent_kobj, const void *new_ns)
{ (void)kobj; (void)new_parent_kobj; (void)new_ns; return 0; }
int sysfs_create_files(struct kobject *kobj, const struct attribute * const *attr)
{ (void)kobj; (void)attr; return 0; }
int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups)
{ (void)kobj; (void)groups; return 0; }
int sysfs_merge_group(struct kobject *kobj, const struct attribute_group *grp)
{ (void)kobj; (void)grp; return 0; }
void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups) { (void)kobj; (void)groups; }
void kernfs_get(struct kernfs_node *kn) { (void)kn; }
void kernfs_put(struct kernfs_node *kn) { (void)kn; }

// ── notifier chains that never fire in a container (no oom killer, no vmap
// purge, no acpi events): registration is recorded on a real chain. (satoru)
static DEFINE_SPINLOCK(lx_nb_lock);
static struct notifier_block *lx_oom_chain, *lx_vmap_chain, *lx_acpi_chain;

static int lx_nb_add(struct notifier_block **head, struct notifier_block *nb)
{
    spin_lock(&lx_nb_lock);
    nb->next = *head;
    *head = nb;
    spin_unlock(&lx_nb_lock);
    return 0;
}

static int lx_nb_del(struct notifier_block **head, struct notifier_block *nb)
{
    int rc = -ENOENT;
    spin_lock(&lx_nb_lock);
    for (struct notifier_block **p = head; *p; p = &(*p)->next)
        if (*p == nb) { *p = nb->next; rc = 0; break; }
    spin_unlock(&lx_nb_lock);
    return rc;
}

int register_oom_notifier(struct notifier_block *nb) { return lx_nb_add(&lx_oom_chain, nb); }
int unregister_oom_notifier(struct notifier_block *nb) { return lx_nb_del(&lx_oom_chain, nb); }
int register_vmap_purge_notifier(struct notifier_block *nb) { return lx_nb_add(&lx_vmap_chain, nb); }
int unregister_vmap_purge_notifier(struct notifier_block *nb) { return lx_nb_del(&lx_vmap_chain, nb); }
int register_acpi_notifier(struct notifier_block *nb) { return lx_nb_add(&lx_acpi_chain, nb); }
int unregister_acpi_notifier(struct notifier_block *nb) { return lx_nb_del(&lx_acpi_chain, nb); }

// ── iosf mbi: no punit/pmic bridge on this platform; the CONFIG_IOSF_MBI=n
// answers of include/asm/iosf_mbi.h. (satoru)
void iosf_mbi_punit_acquire(void) {}
void iosf_mbi_punit_release(void) {}
void iosf_mbi_assert_punit_acquired(void) {}
int iosf_mbi_register_pmic_bus_access_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int iosf_mbi_unregister_pmic_bus_access_notifier_unlocked(struct notifier_block *nb) { (void)nb; return 0; }

// ── vga arbiter: none in the container; -ENODEV is the arbiter's own answer
// for a device it does not track, which i915 treats as ok. vga_get/put are
// the CONFIG_VGA_ARB=n answers. (satoru)
int vga_client_register(struct pci_dev *pdev, unsigned int (*set_decode)(struct pci_dev *pdev, bool state))
{ (void)pdev; (void)set_decode; return -ENODEV; }
int vga_get(struct pci_dev *pdev, unsigned int rsrc, int interruptible)
{ (void)pdev; (void)rsrc; (void)interruptible; return 0; }
void vga_put(struct pci_dev *pdev, unsigned int rsrc) { (void)pdev; (void)rsrc; }

// ── firmware nodes: the container's device has no acpi/of companion. (satoru)
struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *fwnode) { return fwnode; }
struct acpi_device *acpi_find_child_device(struct acpi_device *parent, u64 address, bool check_children)
{ (void)parent; (void)address; (void)check_children; return NULL; }

// ── i2c core pieces for adapters without acpi companions or smbus alert ─────
int i2c_setup_smbus_alert(struct i2c_adapter *adap) { (void)adap; return 0; }
void i2c_acpi_install_space_handler(struct i2c_adapter *adap) { (void)adap; }
void i2c_acpi_remove_space_handler(struct i2c_adapter *adap) { (void)adap; }
void i2c_acpi_register_devices(struct i2c_adapter *adap) { (void)adap; }
// the i2c bus type here has no registered drivers or devices. (satoru)

// ── perf, acpi video: subsystems absent from the container. honest errors
// or the "not configured" answer. (satoru)
int perf_pmu_register(struct pmu *pmu, const char *name, int type)
{ (void)pmu; (void)name; (void)type; return -ENODEV; }
int __cpuhp_state_add_instance(enum cpuhp_state state, struct hlist_node *node, bool invoke)
{ (void)state; (void)node; (void)invoke; return 0; }
#if IS_ENABLED(CONFIG_ACPI_VIDEO)
int acpi_video_register(void) { return -ENODEV; }
void acpi_video_register_backlight(void) {}
#endif

// ── debugfs (absent, as lx_misc.c debugfs_create_dir) ───────────────────────
struct dentry *debugfs_create_file_unsafe(const char *name, umode_t mode, struct dentry *parent,
                                          void *data, const struct file_operations *fops)
{ (void)name; (void)mode; (void)parent; (void)data; (void)fops; return ERR_PTR(-ENODEV); }
void debugfs_create_bool(const char *name, umode_t mode, struct dentry *parent, bool *value)
{ (void)name; (void)mode; (void)parent; (void)value; }
void debugfs_create_str(const char *name, umode_t mode, struct dentry *parent, char **value)
{ (void)name; (void)mode; (void)parent; (void)value; }

// ── buses and drivers in the shim device model: bookkeeping like
// class_register (lx_device.c); nothing binds drivers to devices here. the i2c
// core registers its bus and the dummy driver at init. (satoru)
static struct notifier_block *lx_acpi_reconfig_chain;
// i2c-core-acpi.c is not linked: its notifier block exists so i2c_init can
// register it; the chain never fires (no acpi table reconfiguration). (satoru)
struct notifier_block i2c_acpi_notifier;

// tasklet run-state helpers (kernel/softirq.c). (satoru)
void tasklet_unlock(struct tasklet_struct *t)
{
    smp_mb__before_atomic();
    clear_bit(TASKLET_STATE_RUN, &t->state);
    smp_mb__after_atomic();
    wake_up_var(&t->state);
}

void tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
    while (test_bit(TASKLET_STATE_RUN, &t->state))
        lx_os_yield();
}

// page cache state hints: the container's shmem pages have no writeback and
// no lru, so dirty/accessed marks have nothing to record. (satoru)
bool folio_mark_dirty(struct folio *folio) { (void)folio; return false; }
void folio_mark_accessed(struct folio *folio) { (void)folio; }
void mark_page_accessed(struct page *page) { (void)page; }

// ── backlight class without sysfs (drivers/video/backlight/backlight.c):
// a registry of devices by name; nothing in the container reads it. (satoru)
static LIST_HEAD(lx_bl_list);
static DEFINE_MUTEX(lx_bl_lock);

static void lx_bl_release(struct device *dev)
{
    kfree(container_of(dev, struct backlight_device, dev));
}

struct backlight_device *backlight_device_register(const char *name, struct device *parent,
        void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props)
{
    struct backlight_device *bd = kzalloc(sizeof(*bd), GFP_KERNEL);
    int rc;
    if (!bd)
        return ERR_PTR(-ENOMEM);
    mutex_init(&bd->update_lock);
    mutex_init(&bd->ops_lock);
    device_initialize(&bd->dev);
    bd->dev.parent = parent;
    bd->dev.release = lx_bl_release;
    dev_set_name(&bd->dev, "%s", name);
    dev_set_drvdata(&bd->dev, devdata);
    if (props)
        bd->props = *props;
    if (bd->props.type <= 0 || bd->props.type >= BACKLIGHT_TYPE_MAX)
        bd->props.type = BACKLIGHT_RAW;
    rc = device_add(&bd->dev);
    if (rc) {
        put_device(&bd->dev);
        return ERR_PTR(rc);
    }
    bd->ops = ops;
    mutex_lock(&lx_bl_lock);
    list_add(&bd->entry, &lx_bl_list);
    mutex_unlock(&lx_bl_lock);
    lx_log("[lx_kapi] backlight device registered: ");
    lx_log_line(name);
    return bd;
}

struct backlight_device *backlight_device_get_by_name(const char *name)
{
    struct backlight_device *bd, *found = NULL;
    mutex_lock(&lx_bl_lock);
    list_for_each_entry(bd, &lx_bl_list, entry)
        if (!strcmp(dev_name(&bd->dev), name)) {
            get_device(&bd->dev);
            found = bd;
            break;
        }
    mutex_unlock(&lx_bl_lock);
    return found;
}

void backlight_device_unregister(struct backlight_device *bd)
{
    if (!bd)
        return;
    mutex_lock(&lx_bl_lock);
    list_del(&bd->entry);
    mutex_unlock(&lx_bl_lock);
    mutex_lock(&bd->ops_lock);
    bd->ops = NULL;
    mutex_unlock(&bd->ops_lock);
    device_del(&bd->dev);
    put_device(&bd->dev);
}

#if IS_ENABLED(CONFIG_ACPI_VIDEO)
// no acpi video bus in the container: the panel's own (native) backlight
// interface is the one to use. (satoru)
enum acpi_backlight_type __acpi_video_get_backlight_type(bool native, bool *auto_detect)
{
    (void)native;
    if (auto_detect)
        *auto_detect = false;
    return acpi_backlight_native;
}
#endif

// end (satoru)
