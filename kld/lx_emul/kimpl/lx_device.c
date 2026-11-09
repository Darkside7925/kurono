// lx_emul: the driver-core subset that subsystem cores call while setting up
// a device (device_initialize/dev_set_name/get_device/put_device/device_add),
// semaphores, and a few lib/ helpers.
//
// Faithful where it matters: the refcount is the real kobject kref, and the
// final put_device() runs the device's release callback (dev->release, then
// type->release, then class->dev_release), exactly the order drivers/base/core.c
// uses, because cores free their containing struct there (hid_device_release).
// There is no sysfs, no uevent and no device tree here: device_add records the
// device and succeeds, which is what a core needs to proceed. Bus matching and
// driver binding are not done by this file. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ratelimit.h>

#include "lx_kshim.h"

void device_initialize(struct device *dev)
{
    kobject_init(&dev->kobj, NULL);
    INIT_LIST_HEAD(&dev->dma_pools);
    mutex_init(&dev->mutex);
    spin_lock_init(&dev->devres_lock);
    INIT_LIST_HEAD(&dev->devres_head);
    dev->numa_node = NUMA_NO_NODE;
}

void kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
    memset(kobj, 0, sizeof(*kobj));
    kref_init(&kobj->kref);
    INIT_LIST_HEAD(&kobj->entry);
    kobj->ktype = ktype;
    kobj->state_initialized = 1;
}

int kobject_set_name_vargs(struct kobject *kobj, const char *fmt, va_list vargs)
{
    char *s = kvasprintf(GFP_KERNEL, fmt, vargs);
    if (!s)
        return -ENOMEM;
    kfree(kobj->name);
    kobj->name = s;
    return 0;
}

int dev_set_name(struct device *dev, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = kobject_set_name_vargs(&dev->kobj, fmt, ap);
    va_end(ap);
    return r;
}

struct device *get_device(struct device *dev)
{
    if (dev)
        kref_get(&dev->kobj.kref);
    return dev;
}

static void lx_device_release(struct kref *kref)
{
    struct kobject *kobj = container_of(kref, struct kobject, kref);
    struct device *dev = container_of(kobj, struct device, kobj);
    const char *name = kobj->name;
    if (dev->release)
        dev->release(dev);
    else if (dev->type && dev->type->release)
        dev->type->release(dev);
    else if (dev->class && dev->class->dev_release)
        dev->class->dev_release(dev);
    kfree(name);
}

void put_device(struct device *dev)
{
    if (dev)
        kref_put(&dev->kobj.kref, lx_device_release);
}

int device_add(struct device *dev)
{
    get_device(dev);
    dev->kobj.state_in_sysfs = 0;
    lx_log("[lx_device] device_add ");
    lx_log_line(dev_name(dev) ? dev_name(dev) : "?");
    return 0;
}

void device_del(struct device *dev)
{
    put_device(dev);
}

// ── semaphores (counting, on the futex primitive) ───────────────────────────
#define LX_SEM_LOCK(s) ((volatile unsigned int *)&(s)->lock.raw_lock.val.counter)

void down(struct semaphore *sem)
{
    for (;;) {
        lx_flock(LX_SEM_LOCK(sem));
        if (sem->count > 0) {
            sem->count--;
            lx_funlock(LX_SEM_LOCK(sem));
            return;
        }
        unsigned int c = sem->count;
        lx_funlock(LX_SEM_LOCK(sem));
        lx_os_futex_wait((volatile unsigned int *)&sem->count, c, 10000000ll);
    }
}

int down_interruptible(struct semaphore *sem) { down(sem); return 0; }
int down_killable(struct semaphore *sem)      { down(sem); return 0; }

int down_trylock(struct semaphore *sem)
{
    int got = 0;
    lx_flock(LX_SEM_LOCK(sem));
    if (sem->count > 0) { sem->count--; got = 1; }
    lx_funlock(LX_SEM_LOCK(sem));
    return !got;   // upstream: 0 = acquired (satoru)
}

void up(struct semaphore *sem)
{
    lx_flock(LX_SEM_LOCK(sem));
    sem->count++;
    lx_funlock(LX_SEM_LOCK(sem));
    lx_os_futex_wake((volatile unsigned int *)&sem->count, 1);
}

// ── lib helpers ─────────────────────────────────────────────────────────────
void *kmemdup_noprof(const void *src, size_t len, gfp_t gfp)
{
    void *p = kmalloc(len, gfp);
    if (p)
        memcpy(p, src, len);
    return p;
}

void *__kvmalloc_node_noprof(DECL_BUCKET_PARAMS(size, b), gfp_t flags, int node)
{
    (void)node;
    return kmalloc(size, flags);
}

void kvfree(const void *addr) { kfree(addr); }

extern void *realloc(void *, unsigned long);
void *krealloc_noprof(const void *p, size_t new_size, gfp_t flags)
{
    void *r;
    if (!new_size) {
        kfree(p);
        return ZERO_SIZE_PTR;
    }
    r = realloc((void *)p, new_size);
    (void)flags;
    return r;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!size)
        return 0;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n >= (int)size ? (int)size - 1 : n;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    char *p = kmalloc(n + 1, gfp);
    if (p)
        vsnprintf(p, n + 1, fmt, ap);
    return p;
}

int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
    (void)rs; (void)func;
    return 1;   // every message prints; the container's log is the only sink (satoru)
}

// end (satoru)
