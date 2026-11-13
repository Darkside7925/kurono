// lx_emul: the driver-core, sysfs, chrdev and locking edges the drm core needs
// but the network shim never grew. everything here does the real thing on the
// container's own model; nothing is a "return 0 so it looks implemented".
// (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/ww_mutex.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/fwnode.h>
#include <linux/property.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/i2c.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/debugfs.h>
#include <linux/pid.h>
#include <linux/capability.h>
#include <linux/aperture.h>
#include <video/cmdline.h>
#include <video/nomodeset.h>

#include <linux/sysctl.h>
#include "lx_kshim.h"
#include "lx_broker.h"

// ── class / device registration (no sysfs in the container) ────────────────
struct class *class_create(const char *name)
{
    struct class *c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c)
        return ERR_PTR(-ENOMEM);
    c->name = kstrdup(name ? name : "class", GFP_KERNEL);
    return c;
}

void class_destroy(const struct class *cls)
{
    if (!cls)
        return;
    kfree(cls->name);
    kfree(cls);
}

int class_create_file_ns(const struct class *class, const struct class_attribute *attr,
                         const void *ns)
{
    (void)class; (void)attr; (void)ns;
    return 0;   // class attributes live in sysfs, which the container has not
}

void class_remove_file_ns(const struct class *class, const struct class_attribute *attr,
                          const void *ns)
{
    (void)class; (void)attr; (void)ns;
}

ssize_t show_class_attr_string(const struct class *class,
                               const struct class_attribute *attr, char *buf)
{
    const struct class_attribute_string *cs =
        container_of(attr, struct class_attribute_string, attr);
    (void)class;
    return scnprintf(buf, PAGE_SIZE, "%s\n", cs->str ? cs->str : "");
}

int device_register(struct device *dev)
{
    return device_add(dev);
}

void device_unregister(struct device *dev)
{
    device_del(dev);
    put_device(dev);
}

// ── sysfs emulation used by drm_sysfs/drm_debugfs ──────────────────────────
int sysfs_emit(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, PAGE_SIZE, fmt, ap);
    va_end(ap);
    return n;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf + at, PAGE_SIZE - at, fmt, ap);
    va_end(ap);
    return n;
}

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[])
{
    (void)kobj; (void)action; (void)envp;
    return 0;   // no udev in the container; nothing consumes uevents
}

// a fake dentry token: nothing in the container walks debugfs. (satoru)
struct dentry *debugfs_create_file(const char *name, umode_t mode, struct dentry *parent,
                                   void *data, const struct file_operations *fops)
{
    (void)name; (void)mode; (void)parent; (void)data; (void)fops;
    return ERR_PTR(-ENODEV);
}

// sysctl: i915 registers its perf knobs (dev.i915.*). the container has no
// /proc/sys; registration is inert and returns a header so the driver's own
// error path is not taken; unregister accepts it. (satoru)
static char lx_sysctl_dummy;
struct ctl_table_header *register_sysctl_sz(const char *path, struct ctl_table *table,
                                            size_t table_size)
{
    (void)path; (void)table; (void)table_size;
    return (struct ctl_table_header *)&lx_sysctl_dummy;
}

void unregister_sysctl_table(struct ctl_table_header *header)
{
    (void)header;
}

void debugfs_create_atomic_t(const char *name, umode_t mode,
                             struct dentry *parent, atomic_t *value)
{
    (void)name; (void)mode; (void)parent; (void)value;
}

struct dentry *debugfs_create_symlink(const char *name, struct dentry *parent,
                                      const char *target)
{
    (void)name; (void)parent; (void)target;
    return ERR_PTR(-ENODEV);
}

struct dentry *debugfs_lookup(const char *name, struct dentry *parent)
{
    (void)name; (void)parent;
    return NULL;
}

// ── chrdev registry: this is the node table D5 will serve /dev/dri from ────
#define LX_CHRDEV_MAX 8
struct lx_chrdev {
    bool used;
    unsigned int major, baseminor, count;
    char name[32];
    const struct file_operations *fops;
};
static struct lx_chrdev g_chrdev[LX_CHRDEV_MAX];
static volatile unsigned int g_chrdev_lock;

int __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
                      const char *name, const struct file_operations *fops)
{
    lx_flock(&g_chrdev_lock);
    for (int i = 0; i < LX_CHRDEV_MAX; i++) {
        if (!g_chrdev[i].used) {
            g_chrdev[i].used = true;
            g_chrdev[i].major = major;
            g_chrdev[i].baseminor = baseminor;
            g_chrdev[i].count = count;
            g_chrdev[i].fops = fops;
            int n = 0;
            while (name && name[n] && n < 31) { g_chrdev[i].name[n] = name[n]; n++; }
            g_chrdev[i].name[n] = 0;
            lx_funlock(&g_chrdev_lock);
            lx_log("[lx_drm_misc] chrdev ");
            lx_log(g_chrdev[i].name);
            lx_log(" major ");
            lx_log_u(major);
            lx_log(" registered\n");
            return major ? (int)major : 254;
        }
    }
    lx_funlock(&g_chrdev_lock);
    return -EBUSY;
}

void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count,
                         const char *name)
{
    (void)baseminor; (void)name;
    lx_flock(&g_chrdev_lock);
    for (int i = 0; i < LX_CHRDEV_MAX; i++)
        if (g_chrdev[i].used && g_chrdev[i].major == major) {
            g_chrdev[i].used = false;
            break;
        }
    lx_funlock(&g_chrdev_lock);
    (void)count;
}

// ── capability / module bookkeeping ────────────────────────────────────────
// the container is the display owner it was granted by the broker; the DRM
// master checks only ever ask about the container's own file. (satoru)
bool capable(int cap)
{
    // while serving a userspace request the caller's uid decides (kdrm_uapi.c) (satoru)
    extern int kdri_caller_euid(void) __attribute__((weak));
    if (kdri_caller_euid && kdri_caller_euid() >= 0)
        return kdri_caller_euid() == 0;
    return cap == CAP_SYS_ADMIN || cap == CAP_SYS_NICE || cap == CAP_SYS_RAWIO;
}

// the crypto container's shim also defines these three; this family has no
// CRYPTOLIB, so it owns them only when the build says so. (satoru)
#ifdef KLD_DRM_OWNS_MODULE_OPS
bool try_module_get(struct module *module)
{
    (void)module;
    return true;   // every "module" is statically linked into this container
}

void module_put(struct module *module)
{
    (void)module;
}

int __request_module(bool wait, const char *fmt, ...)
{
    (void)wait; (void)fmt;
    return -ENOENT;   // no module loader inside the container
}
#endif

// ── pids: one address space, a few shim threads ────────────────────────────
struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
    (void)pid; (void)type;
    return lx_task_ts(lx_current());
}

pid_t pid_vnr(struct pid *pid)
{
    (void)pid;
    return (pid_t)lx_os_gettid();
}

pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type,
                       struct pid_namespace *ns)
{
    (void)task; (void)type; (void)ns;
    return (pid_t)lx_os_gettid();
}

void put_pid(struct pid *pid)
{
    (void)pid;
}

struct pid *get_task_pid(struct task_struct *task, enum pid_type type)
{
    (void)task; (void)type;
    return NULL;
}

// ── ACPI bus type registration: no ACPI device list in the container ───────
int register_acpi_bus_type(struct acpi_bus_type *type)
{
    (void)type;
    return 0;
}

int unregister_acpi_bus_type(struct acpi_bus_type *type)
{
    (void)type;
    return 0;
}

// ── aperture removal: the broker grant means this device is ours ───────────
int aperture_remove_conflicting_devices(resource_size_t base, resource_size_t size,
                                        const char *name)
{
    lx_log("[lx_drm_misc] aperture ");
    lx_log(name ? name : "?");
    lx_log(" base ");
    lx_log_hex((unsigned long)base);
    lx_log(" size ");
    lx_log_u((unsigned long)size);
    lx_log(": no conflicting framebuffer in the container\n");
    return 0;
}

int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev, const char *name)
{
    (void)pdev;
    return aperture_remove_conflicting_devices(0, 0, name);
}

int devm_aperture_acquire_for_platform_device(struct platform_device *pdev,
                                              resource_size_t base, resource_size_t size)
{
    (void)pdev;
    return aperture_remove_conflicting_devices(base, size, "platform");
}

// ── firmware / device tree / backlight edges ───────────────────────────────
struct fwnode_handle *__dev_fwnode(struct device *dev)
{
    (void)dev;
    return NULL;   // no fwnode/OF graph in the container
}

const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list)
{
    (void)list;
    return NULL;   // the shim has DMI strings but no dmi_system_id table
}

struct backlight_device *devm_of_find_backlight(struct device *dev)
{
    (void)dev;
    return NULL;   // no backlight class device in the container
}

bool video_firmware_drivers_only(void)
{
    return false;
}

const char *video_get_options(const char *name)
{
    (void)name;
    return NULL;   // no video= options in the container's kernel cmdline
}

int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
    (void)dev; (void)fmt;
    return err;
}

const char *kstrdup_const(const char *s, gfp_t gfp)
{
    return s ? kstrdup(s, gfp) : NULL;
}

// ── locking: ww_mutex on the shim's futex mutex ────────────────────────────
// the container has one ww class user (drm_modeset_lock). serializing on the
// underlying mutex is the real semantics; the wound/wait avoidance only matters
// when several ww classes contend, which a single container cannot create.
// (satoru)
// drm_modeset_lock() locks connection_mutex and then, from
// __drm_helper_update_and_validate(), the same lock again through the SAME
// acquire_ctx; the real ww code treats that as already-owned. remember the ctx
// so the second acquisition is a no-op instead of a self-deadlock. (satoru)
int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
    if (ctx && lock->ctx == ctx)
        return -EALREADY;
    mutex_lock(&lock->base);
    lock->ctx = ctx;
    return 0;
}

int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
    return ww_mutex_lock(lock, ctx);
}

int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
    if (ctx && lock->ctx == ctx)
        return 0;
    if (!mutex_trylock(&lock->base))
        return 0;
    lock->ctx = ctx;
    return 1;
}

void ww_mutex_unlock(struct ww_mutex *lock)
{
    lock->ctx = NULL;
    mutex_unlock(&lock->base);
}

int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
    if (atomic_dec_and_test(cnt)) {
        mutex_lock(lock);
        return 1;
    }
    return 0;
}

// ── legacy vga port i/o via the broker (asm/io.h override) ─────────────────
void lx_drm_port_out(unsigned int port, unsigned int width, unsigned int val)
{
    // a negative return means the broker refused (EINVAL/EPERM) or deliberately
    // did not route the access (KLD_ENOROUTE); the kernel logs the one
    // not-routed line per container, so the shim just drops the write. (satoru)
    (void)lx_broker_call(LX_KLD_OP_PORT_OUT, port, width, val, 0);
}

unsigned int lx_drm_port_in(unsigned int port, unsigned int width)
{
    long r = lx_broker_call(LX_KLD_OP_PORT_IN, port, width, 0, 0);
    return r < 0 ? 0xffu : (unsigned int)r;
}

// ── irq work / scheduling helpers ──────────────────────────────────────────
bool irq_work_queue(struct irq_work *work)
{
    // shim irq handlers run on threads, so running the callback here is the
    // same context the kernel promises (hard irq) minus the wakeup. (satoru)
    if (!work || !work->func)
        return false;
    if (atomic_fetch_or(IRQ_WORK_CLAIMED, &work->node.a_flags) & IRQ_WORK_CLAIMED)
        return false;
    work->func(work);
    atomic_andnot(IRQ_WORK_CLAIMED, &work->node.a_flags);
    return true;
}

void sched_set_fifo(struct task_struct *p)
{
    (void)p;
    // no rt priority model in the shim scheduler; ordering is cooperatively
    // driven by the caller's wait, which is what the driver wants. (satoru)
}

int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                    enum dma_data_direction dir, unsigned long attrs)
{
    if (!sgt || !sgt->sgl)
        return -EINVAL;
    if (dma_map_sg_attrs(dev, sgt->sgl, sgt->nents, dir, attrs) == 0)
        return -ENOMEM;
    sgt->nents = sgt->orig_nents;   // phys == bus; every entry maps
    return 0;
}

size_t dma_max_mapping_size(struct device *dev)
{
    (void)dev;
    return 64u << 20;   // the broker's per-container dma window
}

// end (satoru)
