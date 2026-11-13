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
#include <linux/devcoredump.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/kobject.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ratelimit.h>

#include "lx_kshim.h"

// upstream kobject_init() rejects a NULL ktype. the device release is driven
// by put_device()/lx_device_release, so this ktype only satisfies the API.
static void lx_dev_kobj_release(struct kobject *kobj)
{
    (void)kobj;
}
static struct kobj_type lx_device_ktype = { .release = lx_dev_kobj_release };

void device_initialize(struct device *dev)
{
    kobject_init(&dev->kobj, &lx_device_ktype);
    INIT_LIST_HEAD(&dev->dma_pools);
    mutex_init(&dev->mutex);
    spin_lock_init(&dev->devres_lock);
    INIT_LIST_HEAD(&dev->devres_head);
    dev->numa_node = NUMA_NO_NODE;
#ifdef CONFIG_PM
    // upstream pm_runtime_init(): a device starts with runtime PM disabled
    // until its driver calls pm_runtime_enable(), so pm_runtime_enabled() is
    // false at probe time. (satoru)
    dev->power.runtime_status = RPM_SUSPENDED;
    dev->power.last_status = RPM_INVALID;
    dev->power.disable_depth = 1;
    dev->power.runtime_auto = true;
#endif
}

#ifdef KLD_KOBJECT_LOCAL
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
#endif /* KLD_KOBJECT_LOCAL */

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

// platform devices: cfg80211 creates the "regulatory" device so firmware
// request_firmware() has a struct device. no platform bus probe is run; the
// device is only a home for dev_* data. (satoru)
static void lx_platform_dev_release(struct device *dev)
{
    kfree(container_of(dev, struct platform_device, dev));
}

// iwlwifi calls this from its probe failure path; there is no bus to unbind
// from in the container and the driver is about to return the error. (satoru)
// the device coredump framework has no /sys reader in this container; the
// driver's crash path only needs the call to complete. (satoru)
void dev_coredumpsg(struct device *dev, struct scatterlist *table,
                    size_t datalen, gfp_t gfp)
{
    (void)dev; (void)table; (void)datalen; (void)gfp;
}

void device_release_driver(struct device *dev)
{
    extern void lx_pci_mark_released(void);
    (void)dev;
    lx_pci_mark_released();
}

struct platform_device *platform_device_register_full(
    const struct platform_device_info *pdevinfo)
{
    struct platform_device *pdev;
    if (!pdevinfo)
        return ERR_PTR(-EINVAL);
    pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
    if (!pdev)
        return ERR_PTR(-ENOMEM);
    device_initialize(&pdev->dev);
    pdev->dev.release = lx_platform_dev_release;
    pdev->name = pdevinfo->name;
    pdev->id = pdevinfo->id;
    pdev->dev.parent = pdevinfo->parent;
    if (pdevinfo->dma_mask)
        pdev->dev.dma_mask = pdevinfo->dma_mask;
    if (pdevinfo->num_res && pdevinfo->res) {
        pdev->resource = kmemdup(pdevinfo->res,
                                 sizeof(*pdevinfo->res) * pdevinfo->num_res,
                                 GFP_KERNEL);
        if (!pdev->resource) {
            put_device(&pdev->dev);
            return ERR_PTR(-ENOMEM);
        }
        pdev->num_resources = pdevinfo->num_res;
    }
    if (pdevinfo->data && pdevinfo->size_data) {
        pdev->dev.platform_data = kmemdup(pdevinfo->data, pdevinfo->size_data,
                                          GFP_KERNEL);
        if (!pdev->dev.platform_data) {
            kfree(pdev->resource);
            put_device(&pdev->dev);
            return ERR_PTR(-ENOMEM);
        }
    }
    if (pdevinfo->name)
        dev_set_name(&pdev->dev, "%s.%d", pdevinfo->name, pdevinfo->id);
    device_add(&pdev->dev);
    return pdev;
}

void platform_device_unregister(struct platform_device *pdev)
{
    if (!pdev)
        return;
    device_del(&pdev->dev);
    put_device(&pdev->dev);
}

// classes are sysfs bookkeeping; the container has no sysfs. (satoru)
int class_register(const struct class *cls)
{
    (void)cls;
    return 0;
}

void class_unregister(const struct class *cls)
{
    (void)cls;
}

struct platform_device *platform_device_alloc(const char *name, int id)
{
    struct platform_device *pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
    if (!pdev)
        return NULL;
    device_initialize(&pdev->dev);
    pdev->dev.release = lx_platform_dev_release;
    pdev->name = name;
    pdev->id = id;
    // upstream leaves the name alone for PLATFORM_DEVID_NONE (-1). (satoru)
    if (name) {
        if (id != -1)
            dev_set_name(&pdev->dev, "%s.%d", name, id);
        else
            dev_set_name(&pdev->dev, "%s", name);
    }
    return pdev;
}

int platform_device_add_resources(struct platform_device *pdev,
                                  const struct resource *res, unsigned int num)
{
    if (!pdev || !res || !num)
        return 0;
    kfree(pdev->resource);
    pdev->resource = kmemdup(res, sizeof(*res) * num, GFP_KERNEL);
    if (!pdev->resource)
        return -ENOMEM;
    pdev->num_resources = num;
    return 0;
}

int platform_device_add_data(struct platform_device *pdev, const void *data,
                             size_t size)
{
    if (!pdev || !data || !size)
        return 0;
    kfree(pdev->dev.platform_data);
    pdev->dev.platform_data = kmemdup(data, size, GFP_KERNEL);
    return pdev->dev.platform_data ? 0 : -ENOMEM;
}

int platform_device_add(struct platform_device *pdev)
{
    if (!pdev)
        return -EINVAL;
    pdev->dev.bus = &platform_bus_type;
    if (!pdev->dev.init_name && !pdev->dev.kobj.name && pdev->name)
        dev_set_name(&pdev->dev, "%s.%d", pdev->name, pdev->id);
    return device_add(&pdev->dev);
}

void platform_device_del(struct platform_device *pdev)
{
    if (pdev)
        device_del(&pdev->dev);
}

void platform_device_put(struct platform_device *pdev)
{
    if (pdev)
        put_device(&pdev->dev);
}

struct resource *platform_get_resource(struct platform_device *pdev,
                                       unsigned int type, unsigned int num)
{
    if (!pdev)
        return NULL;
    for (unsigned int i = 0; i < pdev->num_resources; i++) {
        struct resource *r = &pdev->resource[i];
        if ((r->flags & IORESOURCE_TYPE_BITS) == type && num-- == 0)
            return r;
    }
    return NULL;
}

int platform_get_irq(struct platform_device *pdev, unsigned int num)
{
    struct resource *r = platform_get_resource(pdev, IORESOURCE_IRQ, num);
    return r ? (int)r->start : -ENXIO;
}

__attribute__((weak)) void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res)
{
    (void)dev;
    void __iomem *p;

    if (!res || !res->start || !resource_size(res))
        return ERR_PTR(-EINVAL);
    p = ioremap(res->start, resource_size(res));
    return p ? p : ERR_PTR(-ENOMEM);
}

void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev,
                                             unsigned int index)
{
    return devm_ioremap_resource(&pdev->dev, platform_get_resource(pdev, IORESOURCE_MEM, index));
}

__attribute__((weak)) void __iomem *devm_ioremap_uc(struct device *dev, resource_size_t start,
                              resource_size_t size)
{
    (void)dev;
    return ioremap(start, size);
}

int device_for_each_child_reverse(struct device *parent, void *data,
                                  int (*fn)(struct device *, void *))
{
    (void)parent; (void)data; (void)fn;
    return 0;
}

struct device_link *device_link_add(struct device *consumer,
                                    struct device *supplier, u32 flags)
{
    (void)consumer; (void)supplier; (void)flags;
    return NULL;
}

int device_add_software_node(struct device *dev, const struct software_node *node)
{
    (void)dev; (void)node;
    return 0;
}

void device_remove_software_node(struct device *dev)
{
    (void)dev;
}

// the platform bus subset: name match, probe/remove. the real platform core
// also handles OF/ACPI/module aliases; the container's devices are board
// devices created by its own tests, so the name is the whole match. (satoru)
static int lx_platform_match(struct device *dev, const struct device_driver *drv)
{
    const char *dn = dev_name(dev);
    const char *name = drv->name;
    size_t n;
    // upstream platform_match order: id table, ACPI, OF, then name. the
    // container's board devices are name-matched; ACPI-enumerated ones (the
    // PCH pinctrl controller) match their companion's HID. (satoru)
    if (acpi_driver_match_device(dev, drv))
        return 1;
    if (!dn || !name || !name[0])
        return 0;
    n = strlen(name);
    return !strncmp(dn, name, n) && (dn[n] == '\0' || dn[n] == '.');
}

static int lx_platform_probe(struct device *dev)
{
    struct platform_driver *drv = container_of(dev->driver, struct platform_driver, driver);
    struct platform_device *pdev = container_of(dev, struct platform_device, dev);
    lx_log("[lx_platform] probe enter ");
    lx_log_line(dev_name(dev));
    int rc = drv->probe ? drv->probe(pdev) : 0;
    lx_log("[lx_platform] probe ");
    lx_log_line(dev_name(dev));
    lx_log(" rc=");
    lx_log_u((unsigned long)(rc < 0 ? -rc : rc));
    lx_log("\n");
    return rc;
}

static void lx_platform_remove(struct device *dev)
{
    struct platform_driver *drv = container_of(dev->driver, struct platform_driver, driver);
    struct platform_device *pdev = container_of(dev, struct platform_device, dev);
    if (drv->remove)
        drv->remove(pdev);
}

const struct bus_type platform_bus_type = {
    .name = "platform",
    .match = lx_platform_match,
    .probe = lx_platform_probe,
    .remove = lx_platform_remove,
};

int __platform_driver_register(struct platform_driver *drv, struct module *owner)
{
    (void)owner;
    if (!drv)
        return -EINVAL;
    drv->driver.bus = &platform_bus_type;
    return driver_register(&drv->driver);
}

void platform_driver_unregister(struct platform_driver *drv)
{
    if (drv)
        driver_unregister(&drv->driver);
}

// classes are sysfs bookkeeping; the container has no sysfs. (satoru)
// struct gpio_device's first member is struct device dev; the cdev face is
// not needed in the container, so register the device itself. (satoru)
int gpiolib_cdev_register(struct gpio_device *gdev, dev_t devt)
{
    (void)devt;
    return device_add((struct device *)gdev);
}

void gpiolib_cdev_unregister(struct gpio_device *gdev)
{
    device_del((struct device *)gdev);
}

struct device *device_find_any_child(struct device *parent)
{
    (void)parent;
    return NULL;
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
