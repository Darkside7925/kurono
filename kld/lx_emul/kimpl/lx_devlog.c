// lx_emul: the dev_*/netdev_* logging families.
//
// Cheap to implement and disproportionately valuable: these are how a driver
// reports what it is actually doing during probe. Nine references in e1000e, and
// every one of them is a diagnostic we want to read rather than an abort.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/stdarg.h>

#include "lx_kshim.h"

// implemented in lx_printk.c (userspace context). declared here by hand because
// including the shim's own header would drag libc types into kernel context.
int lx_vprintk(const char *fmt, va_list ap);

static void lx_devlog(const char *tag, const char *name, const char *fmt, va_list ap)
{
    lx_log("[");
    lx_log(tag);
    lx_log("] ");
    if (name) {
        lx_log(name);
        lx_log(": ");
    }
    lx_vprintk(fmt, ap);
}

// ── struct device ───────────────────────────────────────────────────────────
const char *dev_driver_string(const struct device *dev)
{
    if (dev && dev->driver && dev->driver->name)
        return dev->driver->name;
    return "kld";
}

static const char *lx_dev_name(const struct device *dev)
{
    if (!dev)
        return NULL;
    // dev_name() reads init_name before the kobject name is set, which is the
    // state a container's synthetic device is in. (satoru)
    if (dev->init_name)
        return dev->init_name;
    return NULL;
}

void _dev_err(const struct device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("ERR  ", lx_dev_name(dev), fmt, ap);
    va_end(ap);
}

void _dev_warn(const struct device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("WARN ", lx_dev_name(dev), fmt, ap);
    va_end(ap);
}

void _dev_info(const struct device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("INFO ", lx_dev_name(dev), fmt, ap);
    va_end(ap);
}

void _dev_notice(const struct device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("NOTE ", lx_dev_name(dev), fmt, ap);
    va_end(ap);
}

// ── struct net_device ───────────────────────────────────────────────────────
void netdev_err(const struct net_device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("ERR  ", dev ? dev->name : NULL, fmt, ap);
    va_end(ap);
}

void netdev_warn(const struct net_device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("WARN ", dev ? dev->name : NULL, fmt, ap);
    va_end(ap);
}

void netdev_info(const struct net_device *dev, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    lx_devlog("INFO ", dev ? dev->name : NULL, fmt, ap);
    va_end(ap);
}

// end (satoru)
