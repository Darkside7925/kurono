// lx_pm: runtime PM is bookkeeping in the container.
//
// a container has no system suspend and no runtime power states; drivers and
// the i2c/hid cores still call the PM helpers during probe. they report the
// device as active and do nothing, which is the truth. (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>

void pm_runtime_no_callbacks(struct device *dev) { (void)dev; }
// the status is read by drivers (i2c-designware's isr bails out while the
// device is suspended), so it must follow the calls, not stay zeroed. (satoru)
int __pm_runtime_set_status(struct device *dev, unsigned int status)
{
    dev->power.runtime_status = status;
    return 0;
}
void __pm_runtime_use_autosuspend(struct device *dev, bool use)
{
    (void)dev; (void)use;
}
void pm_runtime_set_autosuspend_delay(struct device *dev, int delay)
{
    (void)dev; (void)delay;
}
// runtime_auto is the "user allows autosuspend" bit; pci_pm_init() forbids it
// for every PCI device before probe. (satoru)
void pm_runtime_allow(struct device *dev) { dev->power.runtime_auto = true; }
void pm_runtime_forbid(struct device *dev) { dev->power.runtime_auto = false; }
int dev_pm_qos_expose_latency_tolerance(struct device *dev) { (void)dev; return 0; }
void dev_pm_qos_hide_latency_tolerance(struct device *dev) { (void)dev; }
// disable_depth as upstream tracks it: enable clears the initial depth (set in
// device_initialize), disable adds one, so pm_runtime_enabled() answers the
// same question it would on a real device. (satoru)
void pm_runtime_enable(struct device *dev)
{
    if (dev->power.disable_depth)
        dev->power.disable_depth--;
}
void __pm_runtime_disable(struct device *dev, bool check_resume)
{
    (void)check_resume;
    dev->power.disable_depth++;
}
void dev_pm_set_wake_irq(struct device *dev, int irq) { (void)dev; (void)irq; }
void dev_pm_clear_wake_irq(struct device *dev) { (void)dev; }
int dev_pm_domain_attach(struct device *dev, bool power_on)
{
    (void)dev; (void)power_on;
    return 0;
}
void dev_pm_domain_detach(struct device *dev, bool power_off)
{
    (void)dev; (void)power_off;
}
void pm_wakeup_dev_event(struct device *dev, unsigned int msec, bool hard)
{
    (void)dev; (void)msec; (void)hard;
}

// no DRM panel follower in the container.
__attribute__((weak)) bool drm_is_panel_follower(struct device *dev)
{
    (void)dev;
    return false;
}

// input event entropy stirring is not needed; the container crng is seeded
// from hardware entropy (see lx_os_random). (satoru)
void add_input_randomness(unsigned int type, unsigned int code,
                          unsigned int value)
{
    (void)type; (void)code; (void)value;
}
// end (satoru)
