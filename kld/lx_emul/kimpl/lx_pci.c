// lx_emul: PCI driver registration and the device model.
//
// This is the block the driver hits first (__pci_register_driver), so it gates
// everything else.
//
// THE HONEST DESIGN DECISION HERE:
// Registration succeeds and reports the id_table the driver wants to match, but
// NO DEVICE IS PROBED, because a userspace container cannot enumerate PCI - that
// requires the kernel broker (PCI config proxy + BAR mapping + MSI doorbell) that
// does not exist yet. Returning 0 with zero devices bound is exactly what a real
// Linux box does when the driver loads and no matching hardware is present, so
// the driver stays in a state it understands.
//
// The alternative - synthesising a fake device so probe() runs - was rejected
// deliberately. probe() immediately does pci_enable_device / ioremap / DMA, all
// of which need the broker anyway, so a fake device would just relocate the
// abort while making the log lie about a device existing.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>

#include "lx_kshim.h"
#include "lx_broker.h"

#define LX_MAX_PCI_DRIVERS 8

static struct pci_driver *lx_drivers[LX_MAX_PCI_DRIVERS];
static int lx_driver_count;

void lx_pci_probe_matching(struct pci_driver *drv);
void lx_pci_bind(struct pci_driver *drv, const struct pci_device_id *id,
                 unsigned int bdf);

static void lx_log_hex(unsigned long v)
{
    static const char d[] = "0123456789abcdef";
    char b[19];
    int i, n = 0;
    // 32-bit wide: the first version printed only 4 nibbles, which made a BAR
    // of 0xFEBC0000 read as "0x0000" in the log and looked like an unassigned
    // BAR. A truncating debug printer is worse than none. (satoru)
    b[n++] = '0'; b[n++] = 'x';
    for (i = 28; i >= 0; i -= 4)
        b[n++] = d[(v >> i) & 0xF];
    b[n] = 0;
    lx_log(b);
}

// ── driver registration ─────────────────────────────────────────────────────
int __pci_register_driver(struct pci_driver *drv, struct module *owner,
                          const char *mod_name)
{
    const struct pci_device_id *id;
    int n = 0;

    (void)owner;

    if (!drv)
        return -EINVAL;
    if (lx_driver_count >= LX_MAX_PCI_DRIVERS)
        return -ENOSPC;

    lx_drivers[lx_driver_count++] = drv;

    lx_log("[lx_pci] registered driver '");
    lx_log(drv->name ? drv->name : (mod_name ? mod_name : "?"));
    lx_log("'\n");

    // report what it would bind to. this list is the input the kernel broker
    // will need in order to hand us a matching device. (satoru)
    for (id = drv->id_table; id && id->vendor; id++, n++) {
        if (n < 6) {
            lx_log("[lx_pci]   matches vendor=");
            lx_log_hex(id->vendor);
            lx_log(" device=");
            lx_log_hex(id->device);
            lx_log("\n");
        }
    }
    lx_log("[lx_pci]   id_table entries = ");
    lx_log_u((unsigned long)n);
    lx_log("\n");

    // ── enumerate through the kernel broker ─────────────────────────────────
    if (!lx_broker_available()) {
        lx_log("[lx_pci] kernel broker not reachable - no devices bound\n");
        return 0;
    }
    lx_log("[lx_pci] broker present, scanning for a matching device...\n");
    lx_pci_probe_matching(drv);
    return 0;   // registration succeeded regardless of whether a device bound
}

// ask the broker for each id_table entry in turn; on the first hit, build a
// pci_dev from real config space and call the driver's probe(). (satoru)
void lx_pci_probe_matching(struct pci_driver *drv)
{
    const struct pci_device_id *id;

    for (id = drv->id_table; id && id->vendor; id++) {
        long bdf = lx_broker_pci_find(id->vendor, id->device, 0);
        if (bdf < 0)
            continue;

        lx_log("[lx_pci] MATCH vendor=");
        lx_log_hex(id->vendor);
        lx_log(" device=");
        lx_log_hex(id->device);
        lx_log(" at bdf=");
        lx_log_u((unsigned long)((bdf >> 16) & 0xFF));
        lx_log(":");
        lx_log_u((unsigned long)((bdf >> 8) & 0xFF));
        lx_log(".");
        lx_log_u((unsigned long)(bdf & 0xFF));
        lx_log("\n");

        lx_pci_bind(drv, id, (unsigned int)bdf);
        return;
    }
    lx_log("[lx_pci] no matching device present on this machine\n");
}

void pci_unregister_driver(struct pci_driver *drv)
{
    int i;
    for (i = 0; i < lx_driver_count; i++) {
        if (lx_drivers[i] == drv) {
            lx_log("[lx_pci] unregistered driver '");
            lx_log(drv->name ? drv->name : "?");
            lx_log("'\n");
            lx_drivers[i] = lx_drivers[--lx_driver_count];
            return;
        }
    }
}

// how many drivers are registered - the container reports this so a successful
// load is distinguishable from a silent no-op. (satoru)
int lx_pci_driver_count(void) { return lx_driver_count; }

// ── binding a real device ───────────────────────────────────────────────────
// Build a pci_dev from REAL config space (read through the broker) and hand it
// to the driver's probe(). Only the fields probe() actually reads are filled;
// anything else stays zero, which is the honest state - a half-invented pci_dev
// is how a shim makes a driver misbehave in ways that look like hardware faults.
// (satoru)
static struct pci_dev lx_pdev;
static struct pci_driver *lx_bound_drv;

// A synthetic device MUST have a bus. e1000e's probe calls e1000e_disable_aspm
// before anything else, and its first statement is
//     struct pci_dev *parent = pdev->bus->self;
// so a NULL bus is an immediate SIGSEGV *before* any shim symbol is reached -
// which is indistinguishable from a shim gap unless you go read the driver.
// `self` stays NULL (we are the root bus, no upstream bridge), which the driver
// handles: it checks parent before touching it. (satoru)
static struct pci_bus lx_pbus;

// the bdf behind lx_pdev, so the config accessors below can reach the broker.
static unsigned int lx_pdev_bdf;

unsigned int lx_pci_bdf_of(const struct pci_dev *dev)
{
    return (dev == &lx_pdev) ? lx_pdev_bdf : 0xFFFFFFFFu;
}

void lx_pci_bind(struct pci_driver *drv, const struct pci_device_id *id,
                 unsigned int bdf)
{
    long v;
    int rc;

    lx_pdev_bdf = bdf;

    lx_pdev.vendor = (u16)id->vendor;
    lx_pdev.device = (u16)id->device;
    lx_pdev.devfn  = (unsigned int)(((bdf >> 8) & 0xFF) << 3) | (bdf & 0x7);

    v = lx_broker_cfg_read(bdf, 0x2C, 2);
    if (v >= 0) lx_pdev.subsystem_vendor = (u16)v;
    v = lx_broker_cfg_read(bdf, 0x2E, 2);
    if (v >= 0) lx_pdev.subsystem_device = (u16)v;
    v = lx_broker_cfg_read(bdf, 0x08, 1);
    if (v >= 0) lx_pdev.revision = (u8)v;

    lx_pbus.number = (unsigned char)((bdf >> 16) & 0xFF);
    lx_pbus.self   = NULL;          // root bus: no upstream bridge (satoru)
    lx_pdev.bus    = &lx_pbus;

    lx_pdev.dev.init_name = drv->name;
    lx_pdev.dev.driver = &drv->driver;

    lx_log("[lx_pci] config read: subsys=");
    lx_log_hex(lx_pdev.subsystem_vendor);
    lx_log(":");
    lx_log_hex(lx_pdev.subsystem_device);
    lx_log(" rev=");
    lx_log_u((unsigned long)lx_pdev.revision);
    lx_log("\n");

    // report the BARs the broker sees; probe() will want to map BAR0. (satoru)
    for (int b = 0; b < 6; b++) {
        long bar = lx_broker_bar(bdf, (unsigned int)b);
        if (bar <= 0) continue;
        lx_log("[lx_pci]   BAR");
        lx_log_u((unsigned long)b);
        lx_log(" = ");
        lx_log_hex((unsigned long)bar);
        lx_log("\n");
    }

    if (!drv->probe) {
        lx_log("[lx_pci] driver has no probe()\n");
        return;
    }

    lx_bound_drv = drv;
    // probe()'s FIRST statement indexes a driver-internal table with
    // ent->driver_data and dereferences the result, so a bogus driver_data is an
    // immediate SIGSEGV before any shim symbol is reached. Log it so that failure
    // mode is distinguishable from a shim gap. (satoru)
    lx_log("[lx_pci] ent->driver_data = ");
    lx_log_u((unsigned long)id->driver_data);
    lx_log("\n");
    lx_log("[lx_pci] calling driver probe() with a REAL device\n");
    rc = drv->probe(&lx_pdev, id);
    lx_log("[lx_pci] probe() returned ");
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else        { lx_log_u((unsigned long)rc); }
    lx_log("\n");
    if (rc != 0)
        lx_bound_drv = NULL;
}

// ── config space, via the broker ────────────────────────────────────────────
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    long v;
    if (bdf == 0xFFFFFFFFu || !val)
        return -ENODEV;
    v = lx_broker_cfg_read(bdf, (unsigned int)where, 2);
    if (v < 0)
        return (int)v;
    *val = (u16)v;
    return 0;
}

int pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    long r;
    if (bdf == 0xFFFFFFFFu)
        return -ENODEV;
    r = lx_broker_cfg_write(bdf, (unsigned int)where, 2, val);
    return (r < 0) ? (int)r : 0;
}

// end (satoru)
