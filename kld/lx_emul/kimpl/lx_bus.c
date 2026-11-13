// lx_bus: the minimal driver-core binding the container needs.
//
// lx_device.c creates devices but does not match them to drivers. i2c-core and
// hid-core register a bus and drivers and expect driver_register()/device_add()
// to run the bus match and probe, exactly as drivers/base does. this file
// implements that subset with plain lists; no sysfs, no uevent, no device
// links. probes run without a lock held, since they sleep. (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "lx_kshim.h"

struct lx_bus_rec {
    const struct bus_type *bus;
    struct list_head list;
};

struct lx_drv_rec {
    struct device_driver *drv;
    struct list_head list;
};

struct lx_dev_rec {
    struct device *dev;
    struct list_head list;
};

static LIST_HEAD(lx_buses);
static LIST_HEAD(lx_drivers);
static LIST_HEAD(lx_devices);

// one module exists in a container: __this_module. the driver core takes a
// reference around a bound driver; it must not fail on the initial zero
// refcount, so this module is always live. (satoru)
__attribute__((weak)) bool try_module_get(struct module *module)
{
    if (!module || module == &__this_module)
        return true;
    return atomic_inc_not_zero(&module->refcnt) != 0;
}

__attribute__((weak)) void module_put(struct module *module)
{
    if (!module || module == &__this_module)
        return;
    atomic_dec(&module->refcnt);
}

void __module_get(struct module *module)
{
    if (module && module != &__this_module)
        atomic_inc(&module->refcnt);
}

int module_refcount(struct module *module)
{
    return (module && module != &__this_module) ?
           atomic_read(&module->refcnt) : 1;
}

int bus_register(const struct bus_type *bus)
{
    struct lx_bus_rec *r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    r->bus = bus;
    list_add_tail(&r->list, &lx_buses);
    return 0;
}

void bus_unregister(const struct bus_type *bus)
{
    struct lx_bus_rec *r, *n;
    list_for_each_entry_safe(r, n, &lx_buses, list) {
        if (r->bus == bus) {
            list_del(&r->list);
            kfree(r);
            return;
        }
    }
}

static int lx_try_bind(struct device *dev, struct device_driver *drv)
{
    struct bus_type *bus = dev->bus;
    int ret;

    if (dev->driver || !bus || bus != drv->bus)
        return 0;
    if (bus->match && !bus->match(dev, drv))
        return 0;
    dev->driver = drv;
    if (bus->probe) {
        ret = bus->probe(dev);
        if (ret) {
            dev->driver = NULL;
            return ret;
        }
    }
    return 0;
}

int driver_register(struct device_driver *drv)
{
    struct lx_drv_rec *r, *n;

    struct lx_dev_rec *d;

    list_for_each_entry(r, &lx_drivers, list)
        if (r->drv == drv)
            return 0;
    r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    r->drv = drv;
    list_add_tail(&r->list, &lx_drivers);

    list_for_each_entry(d, &lx_devices, list)
        lx_try_bind(d->dev, drv);
    (void)n;

    return 0;
}

void driver_unregister(struct device_driver *drv)
{
    struct lx_drv_rec *r, *n;
    struct lx_dev_rec *d;

    list_for_each_entry(d, &lx_devices, list) {
        if (d->dev->driver == drv && d->dev->bus && d->dev->bus->remove)
            d->dev->bus->remove(d->dev);
        if (d->dev->driver == drv)
            d->dev->driver = NULL;
    }
    list_for_each_entry_safe(r, n, &lx_drivers, list) {
        if (r->drv == drv) {
            list_del(&r->list);
            kfree(r);
            return;
        }
    }
}

int device_add(struct device *dev)
{
    struct lx_drv_rec *r;
    struct lx_dev_rec *d;

    list_for_each_entry(d, &lx_devices, list)
        if (d->dev == dev)
            return 0;
    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;
    d->dev = dev;
    list_add_tail(&d->list, &lx_devices);
    dev->kobj.state_in_sysfs = 1;
    lx_log("[lx_device] device_add ");
    lx_log_line(dev_name(dev) ? dev_name(dev) : "?");
    if (dev->bus) {
        list_for_each_entry(r, &lx_drivers, list)
            lx_try_bind(dev, r->drv);
    }
    return 0;
}

void device_del(struct device *dev)
{
    struct lx_dev_rec *d, *n;

    list_for_each_entry(d, &lx_devices, list) {
        if (d->dev != dev)
            continue;
        if (dev->driver && dev->bus && dev->bus->remove)
            dev->bus->remove(dev);
        dev->driver = NULL;
        dev->kobj.state_in_sysfs = 0;
        list_del(&d->list);
        kfree(d);
        break;
    }
    (void)n;
}

// drivers/base/bus.c device_reprobe(): drop the current driver and let every
// registered driver match again. hid-core uses it when hid-multitouch is
// registered after a device already bound to hid-generic. (satoru)
int device_reprobe(struct device *dev)
{
    struct lx_drv_rec *r;

    if (!dev)
        return -EINVAL;
    if (dev->driver && dev->bus && dev->bus->remove)
        dev->bus->remove(dev);
    dev->driver = NULL;
    if (dev->bus) {
        list_for_each_entry(r, &lx_drivers, list)
            lx_try_bind(dev, r->drv);
    }
    return 0;
}

__attribute__((weak)) int device_register(struct device *dev)
{
    device_initialize(dev);
    return device_add(dev);
}

__attribute__((weak)) void device_unregister(struct device *dev)
{
    device_del(dev);
    put_device(dev);
}

int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
                     int (*fn)(struct device *dev, void *data))
{
    struct lx_dev_rec *d;
    list_for_each_entry(d, &lx_devices, list) {
        int ret;
        if (d->dev->bus != bus)
            continue;
        if (start && d->dev == start)
            start = NULL;
        if (start)
            continue;
        ret = fn(d->dev, data);
        if (ret)
            return ret;
    }
    return 0;
}

int bus_for_each_drv(const struct bus_type *bus, struct device_driver *start,
                     void *data,
                     int (*fn)(struct device_driver *drv, void *data))
{
    struct lx_drv_rec *r;
    list_for_each_entry(r, &lx_drivers, list) {
        int ret;
        if (r->drv->bus != bus)
            continue;
        if (start && r->drv == start)
            start = NULL;
        if (start)
            continue;
        ret = fn(r->drv, data);
        if (ret)
            return ret;
    }
    return 0;
}

struct device *bus_find_device(const struct bus_type *bus, struct device *start,
                               const void *data,
                               int (*match)(struct device *dev, const void *data))
{
    struct lx_dev_rec *d;
    list_for_each_entry(d, &lx_devices, list) {
        if (d->dev->bus != bus)
            continue;
        if (start && d->dev == start)
            start = NULL;
        if (start)
            continue;
        if (match(d->dev, data))
            return get_device(d->dev);
    }
    return NULL;
}

int device_for_each_child(struct device *parent, void *data,
                          int (*fn)(struct device *dev, void *data))
{
    struct lx_dev_rec *d;
    list_for_each_entry(d, &lx_devices, list) {
        if (d->dev->parent != parent)
            continue;
        if (fn(d->dev, data))
            return 1;
    }
    return 0;
}

// no sysfs in the container; groups and links are accepted and never shown.
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp)
{
    (void)kobj; (void)grp;
    return 0;
}

void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp)
{
    (void)kobj; (void)grp;
}

int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr,
                         const void *ns)
{
    (void)kobj; (void)attr; (void)ns;
    return 0;
}

void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr,
                          const void *ns)
{
    (void)kobj; (void)attr; (void)ns;
}

int sysfs_create_dir_ns(struct kobject *kobj, const void *ns)
{
    (void)kobj; (void)ns;
    return 0;
}

void sysfs_remove_dir(struct kobject *kobj)
{
    (void)kobj;
}

// devtmpfs is absent; hidraw still creates a class device per node. keep the
// object alive for the caller but never publish a /dev node. (satoru)
struct device *device_create(const struct class *cls, struct device *parent,
                             dev_t devt, void *drvdata, const char *fmt, ...)
{
    struct device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    va_list ap;

    if (!dev)
        return ERR_PTR(-ENOMEM);
    device_initialize(dev);
    dev->class = (struct class *)cls;
    dev->parent = parent;
    dev->devt = devt;
    dev->driver_data = drvdata;
    va_start(ap, fmt);
    kobject_set_name_vargs(&dev->kobj, fmt, ap);
    va_end(ap);
    device_add(dev);
    return dev;
}

int device_create_file(struct device *dev,
                       const struct device_attribute *attr)
{
    (void)dev;
    (void)attr;
    return 0;
}

void device_remove_file(struct device *dev,
                        const struct device_attribute *attr)
{
    (void)dev;
    (void)attr;
}

int device_create_bin_file(struct device *dev,
                           const struct bin_attribute *attr)
{
    (void)dev;
    (void)attr;
    return 0;
}

void device_remove_bin_file(struct device *dev,
                            const struct bin_attribute *attr)
{
    (void)dev;
    (void)attr;
}

void device_destroy(const struct class *cls, dev_t devt)
{
    (void)cls;
    (void)devt;
}

// no ACPI hotplug in the container; the reconfig notifier is bookkeeping.
int acpi_reconfig_notifier_register(struct notifier_block *nb)
{
    (void)nb;
    return 0;
}

int acpi_reconfig_notifier_unregister(struct notifier_block *nb)
{
    (void)nb;
    return 0;
}

#ifndef KLD_ACPI_BUILD
// no ACPI companion is ever attached to a hidsim/upstream-generated device;
// the ACPICA families bind upstream's acpi_match_device_ids instead. (satoru)
bool acpi_driver_match_device(struct device *dev, const struct device_driver *drv)
{
    (void)dev;
    (void)drv;
    return false;
}
#endif

// end (satoru)
