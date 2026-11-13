// lx_emul: PCI driver registration, the device model, config space and BARs.
//
// Everything a driver learns about its device comes from the device itself,
// through the kernel broker: config space reads, BAR addresses and BAR sizes
// (the broker sizes BARs, since the container may not write them). Nothing is
// invented. A field the shim cannot fill truthfully stays zero, which is the
// state a driver handles ("capability absent").
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/pm_qos.h>
#include <linux/pm_wakeup.h>

#include "lx_kshim.h"
#include "lx_broker.h"

#define LX_MAX_PCI_DRIVERS 8
#define LX_SIM_BAR_PHYS 0xF0000000ul
#define LX_SIM_BDF      0x00801501u

static struct lx_pci_sim_dev g_sim;
static int g_sim_present;
// the single shimmed pci function's legacy line and whether it came from
// config space (an 8259 source) or from acpi _prt (already a gsi). (satoru)
static int lx_pci_irq_line;
static int lx_pci_irq_isa;
static void *g_sim_bar;
static unsigned long g_sim_bar_len;
static int g_sim_irq = -1;


void *lx_pci_sim_bar0(void) { return g_sim_bar; }
int lx_pci_sim_irq(void) { return g_sim_irq; }
unsigned long lx_pci_sim_bar0_len(void) { return g_sim_bar_len; }

static struct pci_driver *lx_drivers[LX_MAX_PCI_DRIVERS];
static int lx_driver_count;

int lx_pci_sim_add(const struct lx_pci_sim_dev *d)
{
    if (!d)
        return -EINVAL;
    if (g_sim_bar)
        kfree(g_sim_bar);
    g_sim = *d;
    g_sim_bar_len = d->bar0_len ? (d->bar0_len + 0xFFF) & ~0xFFFul : 0x1000;
    g_sim_bar = kzalloc(g_sim_bar_len, GFP_KERNEL);
    if (!g_sim_bar)
        return -ENOMEM;
    g_sim_present = 1;
    lx_log("[lx_pci] simulated device ");
    lx_log_hex(d->vendor);
    lx_log(":");
    lx_log_hex(d->device);
    lx_log(" bar0 ");
    lx_log_hex((unsigned long)g_sim_bar);
    lx_log(" size ");
    lx_log_hex(g_sim_bar_len);
    lx_log("\n");
    // a driver registered before the device still gets the normal match. (satoru)
    for (int i = 0; i < lx_driver_count; i++)
        lx_pci_probe_matching(lx_drivers[i]);
    return 0;
}


void lx_pci_probe_matching(struct pci_driver *drv);
void lx_pci_bind(struct pci_driver *drv, const struct pci_device_id *id,
                 unsigned int bdf);

// ── driver registration ─────────────────────────────────────────────────────
static bool g_defer_probe;
void lx_pci_defer_probe(bool on) { g_defer_probe = on; }

// --kld-ids runs on the host, where there is no broker: registration must not
// even ask for it. the flag is container-side only and never reaches a boot
// with a real device. (satoru)
static bool g_bus_disabled;
void lx_pci_set_bus_disabled(bool on) { g_bus_disabled = on; }

struct pci_driver *lx_pci_driver_at(int idx)
{
    return (idx >= 0 && idx < lx_driver_count) ? lx_drivers[idx] : NULL;
}

void lx_pci_probe_deferred(void)
{
    g_defer_probe = false;
    if (g_bus_disabled)
        return;
    for (int i = 0; i < lx_driver_count; i++)
        lx_pci_probe_matching(lx_drivers[i]);
}

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

    for (id = drv->id_table; id && id->vendor; id++)
        n++;
    lx_log("[lx_pci]   id_table entries = ");
    lx_log_u((unsigned long)n);
    lx_log("\n");

    if (g_bus_disabled)
        return 0;
    if (!g_sim_present && !lx_broker_available()) {
        lx_log("[lx_pci] kernel broker not reachable - no devices bound\n");
        return 0;
    }
    if (!g_defer_probe)
        lx_pci_probe_matching(drv);
    return 0;   // registration succeeded regardless of whether a device bound
}

// ask the broker for each id_table entry in turn; on the first hit, build a
// pci_dev from real config space and call the driver's probe(). (satoru)
void lx_pci_probe_matching(struct pci_driver *drv)
{
    const struct pci_device_id *id;

    if (g_sim_present) {
        for (id = drv->id_table; id && id->vendor; id++) {
            if ((id->vendor == PCI_ANY_ID || id->vendor == g_sim.vendor) &&
                (id->device == PCI_ANY_ID || id->device == g_sim.device) &&
                (id->subvendor == PCI_ANY_ID || id->subvendor == g_sim.subvendor) &&
                (id->subdevice == PCI_ANY_ID || id->subdevice == g_sim.subdevice)) {
                lx_log("[lx_pci] SIM MATCH ");
                lx_log(drv->name ? drv->name : "?");
                lx_log("\n");
                lx_pci_bind(drv, id, LX_SIM_BDF);
                return;
            }
        }
        return;
    }

    for (id = drv->id_table; id && id->vendor; id++) {
        // PCI_ANY_ID entries would need a class match; none of the supported
        // drivers rely on that for their primary ids. (satoru)
        if (id->device == PCI_ANY_ID)
            continue;
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

void lx_acpi_attach_pci_companion(struct pci_dev *pdev) __attribute__((weak));
int lx_acpi_pci_power_on(struct pci_dev *pdev) __attribute__((weak));

static struct pci_dev lx_pdev;
static struct pci_bus lx_pbus;
// every linux pci device has a parent: the root bus device "pci0000:00". drivers
// dereference it (e1000e: pm_runtime_suspended(pdev->dev.parent) in its phy
// status path), so it must exist. it is a plain, always-active device. (satoru)
static struct device lx_pci_root_dev = { .init_name = "pci0000:00" };
static struct pci_driver *lx_bound_drv;
static unsigned int lx_pdev_bdf = 0xFFFFFFFFu;
static char lx_pdev_name[16];

void pci_unregister_driver(struct pci_driver *drv)
{
    int i;
    if (drv == lx_bound_drv && drv->remove) {
        lx_log("[lx_pci] calling driver remove()\n");
        drv->remove(&lx_pdev);
        lx_bound_drv = NULL;
    }
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

int lx_pci_driver_count(void) { return lx_driver_count; }
struct pci_dev *lx_pci_bound_dev(void) { return lx_bound_drv ? &lx_pdev : NULL; }

// iwlwifi calls device_release_driver() from its async firmware failure path.
// the container has no bus to unbind, so mark the device released and let
// lx_main leave the serving loop with a nonzero status. (satoru)
static int lx_pci_released;
void lx_pci_mark_released(void) { lx_pci_released = 1; }
int lx_pci_was_released(void) { return lx_pci_released; }

unsigned int lx_pci_bdf_of(const struct pci_dev *dev)
{
    return (dev == &lx_pdev) ? lx_pdev_bdf : 0xFFFFFFFFu;
}

// the host bridge 00:00.0 as a read-only device (pci_get_domain_bus_and_slot
// below): its config reads go through the broker's HOST_CFG_READ op, which
// only a family with manifest host_bridge_read 1 gets; writes are refused.
// (satoru)
static struct pci_dev lx_host_bridge;
static bool lx_host_bridge_ready;

// ── config space, via the broker ────────────────────────────────────────────
// a failed read returns all-ones, as a real bus does for an absent register,
// and a PCIBIOS error code, as upstream's accessors do. (satoru)
static int lx_cfg_read(const struct pci_dev *dev, int where, int width, u32 *val)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    long v;
    if (g_sim_present && bdf == LX_SIM_BDF) {
        u32 r = 0;
        if (where < 0 || where + width > 256)
            return PCIBIOS_BAD_REGISTER_NUMBER;
        for (int i = 0; i < width; i++)
            r |= (u32)g_sim.config[where + i] << (8 * i);
        *val = r;
        return PCIBIOS_SUCCESSFUL;
    }
    if (dev == &lx_host_bridge && lx_host_bridge_ready) {
        v = lx_broker_call(LX_KLD_OP_HOST_CFG_READ, (unsigned long)where,
                           (unsigned long)width, 0, 0);
        if (v < 0) {
            *val = ~0u;
            return PCIBIOS_BAD_REGISTER_NUMBER;
        }
        *val = (u32)v;
        return PCIBIOS_SUCCESSFUL;
    }
    if (bdf == 0xFFFFFFFFu) {
        *val = ~0u;
        return PCIBIOS_DEVICE_NOT_FOUND;
    }
    v = lx_broker_cfg_read(bdf, (unsigned int)where, (unsigned int)width);
    if (v < 0) {
        *val = ~0u;
        return PCIBIOS_BAD_REGISTER_NUMBER;
    }
    *val = (u32)v;
    return PCIBIOS_SUCCESSFUL;
}

static int lx_cfg_write(const struct pci_dev *dev, int where, int width, u32 val)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    if (g_sim_present && bdf == LX_SIM_BDF) {
        if (where < 0 || where + width > 256)
            return PCIBIOS_BAD_REGISTER_NUMBER;
        for (int i = 0; i < width; i++)
            g_sim.config[where + i] = (u8)(val >> (8 * i));
        return PCIBIOS_SUCCESSFUL;
    }
    if (dev == &lx_host_bridge) {
        lx_log("[lx_pci] host bridge config write refused (read-only)\n");
        return PCIBIOS_SET_FAILED;
    }
    if (bdf == 0xFFFFFFFFu)
        return PCIBIOS_DEVICE_NOT_FOUND;
    long r = lx_broker_cfg_write(bdf, (unsigned int)where, (unsigned int)width, val);
    if (r < 0)
        return PCIBIOS_SET_FAILED;   // includes the broker refusing a bar/msi write (satoru)
    return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
    u32 v; int r = lx_cfg_read(dev, where, 1, &v); *val = (u8)v; return r;
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
    u32 v; int r = lx_cfg_read(dev, where, 2, &v); *val = (u16)v; return r;
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
    return lx_cfg_read(dev, where, 4, val);
}

int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)  { return lx_cfg_write(dev, where, 1, val); }
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val) { return lx_cfg_write(dev, where, 2, val); }
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) { return lx_cfg_write(dev, where, 4, val); }

static u8 lx_find_cap(struct pci_dev *dev, int cap)
{
    u16 status;
    u8 pos;
    if (pci_read_config_word(dev, PCI_STATUS, &status) || !(status & PCI_STATUS_CAP_LIST))
        return 0;
    pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos);
    for (int guard = 0; pos >= 0x40 && guard < 48; guard++) {
        u8 id, next;
        pos &= ~3;
        pci_read_config_byte(dev, pos, &id);
        if (id == 0xFF)
            break;
        if (id == cap)
            return pos;
        pci_read_config_byte(dev, pos + 1, &next);
        pos = next;
    }
    return 0;
}

u8 pci_find_capability(struct pci_dev *dev, int cap) { return lx_find_cap(dev, cap); }

// extended capabilities live at 0x100+, which needs ECAM; the broker does not
// map ECAM yet, so they are honestly absent. (satoru)
u16 pci_find_ext_capability(struct pci_dev *dev, int cap) { (void)dev; (void)cap; return 0; }

// ── pcie capability accessors (mirrors drivers/pci/access.c) ────────────────
static bool lx_pcie_reg_ok(struct pci_dev *dev, int pos)
{
    if (!pci_is_pcie(dev))
        return false;
    switch (pos) {
    case PCI_EXP_FLAGS: case PCI_EXP_DEVCAP: case PCI_EXP_DEVCTL: case PCI_EXP_DEVSTA:
    case PCI_EXP_LNKCAP: case PCI_EXP_LNKCTL: case PCI_EXP_LNKSTA:
    case PCI_EXP_DEVCAP2: case PCI_EXP_DEVCTL2: case PCI_EXP_DEVSTA2:
    case PCI_EXP_LNKCAP2: case PCI_EXP_LNKCTL2: case PCI_EXP_LNKSTA2:
        return true;
    default:
        return false;
    }
}

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
    *val = 0;
    if (pos & 1)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    if (!lx_pcie_reg_ok(dev, pos))
        return 0;   // upstream: unimplemented register reads as 0 (satoru)
    return pci_read_config_word(dev, pci_pcie_cap(dev) + pos, val);
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
    *val = 0;
    if (pos & 3)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    if (!lx_pcie_reg_ok(dev, pos))
        return 0;
    return pci_read_config_dword(dev, pci_pcie_cap(dev) + pos, val);
}

int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
    if (pos & 1)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    if (!lx_pcie_reg_ok(dev, pos))
        return 0;
    return pci_write_config_word(dev, pci_pcie_cap(dev) + pos, val);
}

static volatile unsigned int lx_pcie_cap_lock;

int pcie_capability_clear_and_set_word_unlocked(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
    u16 val;
    int ret = pcie_capability_read_word(dev, pos, &val);
    if (ret)
        return ret;
    val &= ~clear;
    val |= set;
    return pcie_capability_write_word(dev, pos, val);
}

int pcie_capability_clear_and_set_word_locked(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
    lx_flock(&lx_pcie_cap_lock);
    int r = pcie_capability_clear_and_set_word_unlocked(dev, pos, clear, set);
    lx_funlock(&lx_pcie_cap_lock);
    return r;
}

// ASPM: clear the requested states in the device's own Link Control. Upstream
// also reprograms the upstream bridge; the container is not granted the
// bridge, so only the endpoint side changes (link stays in L0 either way, the
// purpose of every disable request). (satoru)
static int lx_disable_link_state(struct pci_dev *pdev, int state)
{
    u16 clr = 0;
    if (state & PCIE_LINK_STATE_L0S)
        clr |= PCI_EXP_LNKCTL_ASPM_L0S;
    if (state & PCIE_LINK_STATE_L1)
        clr |= PCI_EXP_LNKCTL_ASPM_L1;
    if (!clr)
        return 0;
    return pcie_capability_clear_and_set_word_unlocked(pdev, PCI_EXP_LNKCTL, clr, 0);
}

int pci_disable_link_state(struct pci_dev *pdev, int state)        { return lx_disable_link_state(pdev, state); }
int pci_disable_link_state_locked(struct pci_dev *pdev, int state) { return lx_disable_link_state(pdev, state); }

// ── enable / master / regions ───────────────────────────────────────────────
// upstream pci_enable_device -> do_pci_enable_device -> pci_set_power_state(D0):
// PMCSR's state field goes to D0 and, on ACPI systems, the companion's _PS0
// runs (acpi_pci_set_power_state). both happen on the first enable only. (satoru)
static void lx_pci_power_on(struct pci_dev *dev)
{
    if (dev->pm_cap) {
        u16 pmcsr = 0;

        pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
        pmcsr &= ~PCI_PM_CTRL_STATE_MASK;
        pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);
        lx_log("[lx_pci] PMCSR -> D0 at cap ");
        lx_log_hex(dev->pm_cap);
        lx_log("\n");
    }
    if (lx_acpi_pci_power_on)
        lx_acpi_pci_power_on(dev);
}

int pci_enable_device_mem(struct pci_dev *dev)
{
    int first = atomic_read(&dev->enable_cnt) == 0;

    if (g_sim_present && lx_pci_bdf_of(dev) == LX_SIM_BDF) {
        dev->current_state = PCI_D0;
        atomic_inc(&dev->enable_cnt);
        if (first)
            lx_pci_power_on(dev);
        return 0;
    }
    long r = lx_broker_enable(lx_pci_bdf_of(dev));
    if (r < 0)
        return -EIO;
    dev->current_state = PCI_D0;
    atomic_inc(&dev->enable_cnt);
    if (first)
        lx_pci_power_on(dev);
    return 0;
}

int pci_enable_device(struct pci_dev *dev) { return pci_enable_device_mem(dev); }

// pcim_* is the devres-managed wrapper. the container's device teardown already
// releases the broker grant, so enable is the real operation and the mapping
// table is per-pdev bookkeeping. (satoru)
int pcim_enable_device(struct pci_dev *pdev)
{
    return pci_enable_device(pdev);
}

struct lx_pcim_table {
    struct pci_dev *pdev;
    void __iomem *bar[6];
};
static struct lx_pcim_table g_pcim[8];

static struct lx_pcim_table *lx_pcim_slot(struct pci_dev *pdev, bool create)
{
    for (int i = 0; i < 8; i++)
        if (g_pcim[i].pdev == pdev)
            return &g_pcim[i];
    if (!create)
        return NULL;
    for (int i = 0; i < 8; i++) {
        if (!g_pcim[i].pdev) {
            memset(&g_pcim[i], 0, sizeof(g_pcim[i]));
            g_pcim[i].pdev = pdev;
            return &g_pcim[i];
        }
    }
    return NULL;
}

int pcim_iomap_regions_request_all(struct pci_dev *pdev, int mask, const char *name)
{
    (void)name;
    struct lx_pcim_table *t = lx_pcim_slot(pdev, true);
    if (!t)
        return -ENOMEM;
    for (int b = 0; b < 6; b++) {
        if (!(mask & (1 << b)))
            continue;
        if (t->bar[b])
            continue;
        t->bar[b] = pci_iomap(pdev, b, 0);
        if (!t->bar[b])
            return -ENOMEM;
    }
    return 0;
}

void __iomem * const *pcim_iomap_table(struct pci_dev *pdev)
{
    struct lx_pcim_table *t = lx_pcim_slot(pdev, true);
    return t ? t->bar : NULL;
}

void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
    struct lx_pcim_table *t = lx_pcim_slot(pdev, true);
    if (!t || bar < 0 || bar > 5)
        return NULL;
    if (!t->bar[bar])
        t->bar[bar] = pci_iomap(pdev, bar, maxlen);
    return t->bar[bar];
}

void pci_disable_device(struct pci_dev *dev)
{
    lx_broker_disable(lx_pci_bdf_of(dev));
    dev->is_busmaster = 0;
    atomic_dec(&dev->enable_cnt);
}

// the broker's ENABLE op turns bus mastering on together with memory decode;
// set_master records it. (satoru)
void pci_set_master(struct pci_dev *dev)
{
    if (!(g_sim_present && lx_pci_bdf_of(dev) == LX_SIM_BDF))
        lx_broker_enable(lx_pci_bdf_of(dev));
    dev->is_busmaster = 1;
}
void pci_clear_master(struct pci_dev *dev) { if (!(g_sim_present && lx_pci_bdf_of(dev) == LX_SIM_BDF)) lx_broker_disable(lx_pci_bdf_of(dev)); dev->is_busmaster = 0; }

int lx_pci_irq_is_isa(unsigned int irq)
{
    return lx_pci_irq_isa && lx_pci_irq_line != 0 && (int)irq == lx_pci_irq_line;
}

int lx_pci_legacy_line(void)
{
    return lx_pci_irq_line;
}

// acpi _prt fills in the gsi for a function whose config line is unassigned;
// that number is already an ioapic gsi, not an 8259 source. (satoru)
void lx_pci_set_acpi_irq(struct pci_dev *dev, unsigned int gsi)
{
    lx_pci_irq_line = (int)gsi;
    lx_pci_irq_isa = 0;
    dev->irq = (int)gsi;
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int minvec,
                          unsigned int maxvec, unsigned int flags)
{
    (void)maxvec;
    if (minvec > 1)
        return -EINVAL;
    if (g_sim_present) {
        unsigned int irq;
        if (lx_irq_alloc_soft(&irq))
            return -ENOSPC;
        dev->irq = (int)irq;
        g_sim_irq = (int)irq;
        return 1;
    }
    // try msi first unless the caller only asked for intx; a container whose
    // broker refuses msi (kurono.kld.nomsi=1, or no capability) falls back to
    // the legacy line the same way upstream does. (satoru)
    if (!(flags & PCI_IRQ_INTX) && pci_enable_msi(dev) == 0)
        return 1;
    int irq = lx_pci_irq_line ? lx_pci_irq_line : dev->irq;
    if (irq > 0 && irq < 256) {
        dev->irq = irq;
        return 1;
    }
    return -ENOSPC;
}
int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
    return nr ? -EINVAL : dev->irq;
}
void pci_free_irq_vectors(struct pci_dev *dev) { (void)dev; }
int pci_try_set_mwi(struct pci_dev *dev) { (void)dev; return 0; }
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *dev)
{
    for (; ids && ids->vendor; ids++)
        if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
            (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
            (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
            (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device) &&
            ((ids->class ^ dev->class) & ids->class_mask) == 0)
            return ids;
    return NULL;
}

int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
    int i, bars = 0;
    for (i = 0; i < PCI_NUM_RESOURCES && i < 6; i++)
        if (pci_resource_flags(dev, i) & flags)
            bars |= (1 << i);
    return bars;
}

// the broker grants the whole device to one container, so regions are already
// exclusive. (satoru)
int pci_request_selected_regions_exclusive(struct pci_dev *dev, int bars, const char *name)
{
    (void)dev; (void)bars; (void)name;
    return 0;
}
int pci_request_selected_regions(struct pci_dev *dev, int bars, const char *name)
{
    (void)dev; (void)bars; (void)name;
    return 0;
}
void pci_release_selected_regions(struct pci_dev *dev, int bars) { (void)dev; (void)bars; }
int pci_request_regions(struct pci_dev *dev, const char *name) { (void)dev; (void)name; return 0; }
void pci_release_regions(struct pci_dev *dev) { (void)dev; }

// save: the standard header dwords. restore: everything EXCEPT the BARs and
// the rom (the broker owns decode and refuses those writes anyway). (satoru)
int pci_save_state(struct pci_dev *dev)
{
    for (int i = 0; i < 16; i++)
        pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]);
    dev->state_saved = true;
    return 0;
}

void pci_restore_state(struct pci_dev *dev)
{
    if (!dev->state_saved)
        return;
    for (int i = 15; i >= 0; i--) {
        if (i >= 4 && i <= 9)
            continue;   // bars (satoru)
        if (i == 12 || i == 0 || i == 2)
            continue;   // rom, ids, class (read-only) (satoru)
        pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]);
    }
}

// power management: the device stays in D0, wake is not armed. These answer
// the questions honestly ("no wake capability configured") rather than
// pretending to suspend. (satoru)
int pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable)
{
    (void)dev; (void)state; (void)enable;
    return 0;
}
int pci_prepare_to_sleep(struct pci_dev *dev) { (void)dev; return -EIO; }
bool pci_dev_run_wake(struct pci_dev *dev)    { (void)dev; return false; }

int __pm_runtime_resume(struct device *dev, int rpmflags) { (void)dev; (void)rpmflags; return 1; }
int __pm_runtime_idle(struct device *dev, int rpmflags)   { (void)dev; (void)rpmflags; return -EAGAIN; }
int __pm_runtime_suspend(struct device *dev, int rpmflags) { (void)dev; (void)rpmflags; return -EAGAIN; }
int pm_schedule_suspend(struct device *dev, unsigned int delay) { (void)dev; (void)delay; return -EAGAIN; }
int device_set_wakeup_enable(struct device *dev, bool enable) { (void)dev; (void)enable; return 0; }
int device_wakeup_enable(struct device *dev) { (void)dev; return 0; }

// cpu latency qos: no cpuidle governor in the container to constrain. (satoru)
// no cpu idle states to constrain in a container; the request is still
// tracked so _request_active tells added from never added. (satoru)
static struct pm_qos_constraints lx_cpu_latency_qos;
void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value) { (void)value; req->qos = &lx_cpu_latency_qos; }
void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value) { (void)req; (void)new_value; }
void cpu_latency_qos_remove_request(struct pm_qos_request *req) { req->qos = NULL; }
bool cpu_latency_qos_request_active(struct pm_qos_request *req) { return req->qos == &lx_cpu_latency_qos; }

// ── MMIO: BAR mapping through the broker ────────────────────────────────────
struct lx_bar_map {
    resource_size_t start;
    resource_size_t len;
    void *va;
};
static struct lx_bar_map lx_bars[6];

// ioremap(phys) is only legitimate for a phys inside one of OUR granted BARs.
// Any other address is refused loudly - a container has no business mapping
// physical memory it was not granted. (satoru)
void __iomem *ioremap(resource_size_t offset, unsigned long size)
{
    lx_log("[lx_pci] ioremap off=");
    lx_log_hex((unsigned int)offset);
    lx_log(" size=");
    lx_log_u(size);
    lx_log("\n");
#ifdef KLD_MMIO_SIM
    // a register model claimed this range: the model lives at the physical
    // address and the MMIO accessors dispatch there. (satoru)
    if (lx_mmio_sim_claimed((unsigned long)offset, (unsigned long)size))
        return (void __iomem *)(unsigned long)offset;
#endif
    if (g_sim_present && offset >= LX_SIM_BAR_PHYS &&
        offset + size <= LX_SIM_BAR_PHYS + g_sim_bar_len)
        return (void __iomem *)((char *)g_sim_bar + (offset - LX_SIM_BAR_PHYS));
    for (int b = 0; b < 6; b++) {
        struct resource *r = &lx_pdev.resource[b];
        if (!(r->flags & IORESOURCE_MEM) || !r->start)
            continue;
        if (offset < r->start || offset + size - 1 > r->end)
            continue;
        if (!lx_bars[b].va) {
            unsigned long len = (unsigned long)resource_size(r);
            unsigned long rlen = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            void *va = lx_os_reserve(rlen);
            if (!va)
                return NULL;
            long rc = lx_broker_call(LX_KLD_OP_MAP_BAR, lx_pdev_bdf, (unsigned long)b,
                                     (unsigned long)va, rlen);
            if (rc < 0) {
                lx_log("[lx_pci] broker refused MAP_BAR\n");
                lx_os_unreserve(va, rlen);
                return NULL;
            }
            lx_bars[b].start = r->start;
            lx_bars[b].len = len;
            lx_bars[b].va = va;
            lx_log("[lx_pci] bar mapped bar=");
            lx_log_u((unsigned long)b);
            lx_log(" rel=");
            lx_log_hex((unsigned int)(offset - r->start));
            lx_log(" va=");
            lx_log_hex((unsigned int)(unsigned long)va);
            lx_log("\n");
        }
        return (void __iomem *)((char *)lx_bars[b].va + (offset - r->start));
    }
    lx_log("[lx_pci] ioremap of a physical address outside every granted BAR: refused\n");
    return NULL;
}

// the container va of a mapped bar (0 when that bar is not mapped yet): the
// i915 register model asks which accesses are its own. (satoru)
int lx_pci_bar_range(int bar, unsigned long *va, unsigned long *len)
{
    if (bar < 0 || bar > 5 || !lx_bars[bar].va)
        return -1;
    *va = (unsigned long)lx_bars[bar].va;
    *len = (unsigned long)lx_bars[bar].len;
    return 0;
}

void __iomem *ioremap_uc(resource_size_t offset, unsigned long size) { return ioremap(offset, size); }
void __iomem *ioremap_wc(resource_size_t offset, unsigned long size) { return ioremap(offset, size); }

// BAR mappings live as long as the container; the broker tears them down. (satoru)
void iounmap(volatile void __iomem *addr) { (void)addr; }

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
    resource_size_t start = pci_resource_start(dev, bar);
    resource_size_t len = pci_resource_len(dev, bar);
    if (!start || !len || !(pci_resource_flags(dev, bar) & IORESOURCE_MEM))
        return NULL;
    if (maxlen && len > maxlen)
        len = maxlen;
    return ioremap(start, len);
}

void pci_iounmap(struct pci_dev *dev, void __iomem *addr) { (void)dev; (void)addr; }

// ── binding a real device ───────────────────────────────────────────────────
static void lx_fill_resources(struct pci_dev *pdev, unsigned int bdf)
{
    if (g_sim_present && bdf == LX_SIM_BDF) {
        struct resource *r = &pdev->resource[0];
        r->start = LX_SIM_BAR_PHYS;
        r->end = LX_SIM_BAR_PHYS + g_sim_bar_len - 1;
        r->flags = IORESOURCE_MEM;
        r->name = lx_pdev_name;
        lx_log("[lx_pci]   SIM BAR0 = ");
        lx_log_hex((unsigned long)r->start);
        lx_log(" size ");
        lx_log_hex(g_sim_bar_len);
        lx_log("\n");
        return;
    }
    for (int b = 0; b < 6; b++) {
        long raw = lx_broker_bar(bdf, (unsigned int)b);
        long size = lx_broker_bar_size(bdf, (unsigned int)b);
        struct resource *r = &pdev->resource[b];
        if (raw < 0 || size <= 0 || raw == 0)
            continue;
        unsigned long lo = (unsigned long)raw;
        if (lo & PCI_BASE_ADDRESS_SPACE_IO) {
            r->start = lo & PCI_BASE_ADDRESS_IO_MASK;
            r->flags = IORESOURCE_IO;
        } else {
            unsigned long long base = lo & PCI_BASE_ADDRESS_MEM_MASK;
            r->flags = IORESOURCE_MEM;
            if (lo & PCI_BASE_ADDRESS_MEM_PREFETCH)
                r->flags |= IORESOURCE_PREFETCH;
            if ((lo & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64 && b < 5) {
                long hi = lx_broker_bar(bdf, (unsigned int)(b + 1));
                if (hi > 0)
                    base |= (unsigned long long)hi << 32;
                r->flags |= IORESOURCE_MEM_64;
            }
            r->start = base;
        }
        r->end = r->start + (unsigned long)size - 1;
        r->name = lx_pdev_name;
        lx_log("[lx_pci]   BAR");
        lx_log_u((unsigned long)b);
        lx_log(" = ");
        lx_log_hex((unsigned long)r->start);
        lx_log(" size ");
        lx_log_hex((unsigned long)size);
        lx_log((r->flags & IORESOURCE_IO) ? " io\n" : " mem\n");
        if (r->flags & IORESOURCE_MEM_64)
            b++;   // the upper half is not a bar of its own (satoru)
    }
}

static const char lx_hexd[] = "0123456789abcdef";

static void lx_fmt_name(char *out, unsigned int bdf)
{
    unsigned int bus = (bdf >> 16) & 0xFF, dev = (bdf >> 8) & 0xFF, fn = bdf & 7;
    // "0000:bb:dd.f" (satoru)
    out[0] = '0'; out[1] = '0'; out[2] = '0'; out[3] = '0'; out[4] = ':';
    out[5] = lx_hexd[bus >> 4]; out[6] = lx_hexd[bus & 15]; out[7] = ':';
    out[8] = lx_hexd[dev >> 4]; out[9] = lx_hexd[dev & 15]; out[10] = '.';
    out[11] = lx_hexd[fn]; out[12] = 0;
}

void lx_pci_bind(struct pci_driver *drv, const struct pci_device_id *id,
                 unsigned int bdf)
{
    u32 v;
    int rc;

    lx_pdev_bdf = bdf;
    lx_fmt_name(lx_pdev_name, bdf);

    // the Linux device core requires device_initialize before devres/devm_*
    // can use dev->devres_head; iwlwifi's probe starts with pcim_enable_device
    // and devm_kmalloc on &pdev->dev. the root "pci0000:00" parent needs the
    // same one-time setup. (satoru)
    static bool root_inited;
    if (!root_inited) {
        root_inited = true;
        device_initialize(&lx_pci_root_dev);
    }
    device_initialize(&lx_pdev.dev);
#ifdef CONFIG_PM
    // upstream pci_pm_init(): a PCI device is active and runtime-PM enabled,
    // with autosuspend forbidden, before its driver's probe runs. (satoru)
    pm_runtime_forbid(&lx_pdev.dev);
    pm_runtime_set_active(&lx_pdev.dev);
    pm_runtime_enable(&lx_pdev.dev);
#ifdef KLD_PM_DEBUG
    lx_log("[lx_pci] pm state at probe: enabled=");
    lx_log_u(pm_runtime_enabled(&lx_pdev.dev) ? 1 : 0);
    lx_log(" active=");
    lx_log_u(pm_runtime_active(&lx_pdev.dev) ? 1 : 0);
    lx_log(" suspended=");
    lx_log_u(pm_runtime_suspended(&lx_pdev.dev) ? 1 : 0);
    lx_log(" runtime_auto=");
    lx_log_u(lx_pdev.dev.power.runtime_auto ? 1 : 0);
    lx_log("\n");
#endif
#endif
    lx_pdev.dev.parent = &lx_pci_root_dev;
    dev_set_name(&lx_pdev.dev, "%s", lx_pdev_name);

    lx_pdev.vendor = (u16)id->vendor;
    lx_pdev.device = (u16)id->device;
    lx_pdev.devfn  = (unsigned int)(((bdf >> 8) & 0xFF) << 3) | (bdf & 0x7);
    lx_pdev.cfg_size = PCI_CFG_SPACE_SIZE;   // no ecam yet (satoru)
    lx_pdev.current_state = PCI_D0;

    lx_cfg_read(&lx_pdev, PCI_SUBSYSTEM_VENDOR_ID, 2, &v); lx_pdev.subsystem_vendor = (u16)v;
    lx_cfg_read(&lx_pdev, PCI_SUBSYSTEM_ID, 2, &v);        lx_pdev.subsystem_device = (u16)v;
    lx_cfg_read(&lx_pdev, PCI_REVISION_ID, 1, &v);         lx_pdev.revision = (u8)v;
    lx_cfg_read(&lx_pdev, PCI_CLASS_REVISION, 4, &v);      lx_pdev.class = v >> 8;
    lx_cfg_read(&lx_pdev, PCI_HEADER_TYPE, 1, &v);         lx_pdev.hdr_type = (u8)(v & 0x7F);

    // A synthetic device MUST have a bus: e1000e's probe starts with
    // `pdev->bus->self`. self stays NULL (root bus, no upstream bridge granted),
    // which the driver checks before use. (satoru)
    lx_pbus.number = (unsigned char)((bdf >> 16) & 0xFF);
    lx_pbus.self   = NULL;
    lx_pdev.bus    = &lx_pbus;
    // pci_domain_nr() reads the bus sysdata (CONFIG_PCI_DOMAINS): domain 0.
    // (satoru)
    {
        static struct pci_sysdata lx_sysdata;
        lx_pbus.sysdata = &lx_sysdata;
    }
    // dev_is_pci() compares dev->bus with &pci_bus_type; the drm core uses it
    // to tell a real pci device from a platform one. (satoru)
    lx_pdev.dev.bus = &pci_bus_type;

    lx_pdev.pcie_cap = lx_find_cap(&lx_pdev, PCI_CAP_ID_EXP);
    if (lx_pdev.pcie_cap) {
        u16 f;
        pci_read_config_word(&lx_pdev, lx_pdev.pcie_cap + PCI_EXP_FLAGS, &f);
        lx_pdev.pcie_flags_reg = f;
    }
    lx_pdev.msi_cap = lx_find_cap(&lx_pdev, PCI_CAP_ID_MSI);
    lx_pdev.msix_cap = lx_find_cap(&lx_pdev, PCI_CAP_ID_MSIX);
    lx_pdev.pm_cap = lx_find_cap(&lx_pdev, PCI_CAP_ID_PM);

    // dma masks: 32-bit until the driver negotiates, like upstream. (satoru)
    lx_pdev.dma_mask = DMA_BIT_MASK(32);
    lx_pdev.dev.dma_mask = &lx_pdev.dma_mask;
    lx_pdev.dev.coherent_dma_mask = DMA_BIT_MASK(32);
    // like the pci core: the segment limits live in the pci_dev. (satoru)
    lx_pdev.dev.dma_parms = &lx_pdev.dma_parms;

    // legacy line from config space. the value is an 8259 source irq (or 0xff
    // = no line); request_irq() attaches it through the broker's line ops,
    // and the broker resolves the MADT source override. (satoru)
    lx_cfg_read(&lx_pdev, PCI_INTERRUPT_LINE, 1, &v);
    lx_pdev.irq = g_sim_present ? 0 : ((v & 0xFF) == 0xFF ? 0 : (int)(v & 0xFF));
    lx_pci_irq_line = lx_pdev.irq;
    lx_pci_irq_isa = 1;

    lx_pdev.dev.init_name = lx_pdev_name;
    lx_pdev.dev.parent = &lx_pci_root_dev;
    lx_pbus.bridge = &lx_pci_root_dev;
    lx_pdev.dev.driver = &drv->driver;

    lx_log("[lx_pci] config read: subsys=");
    lx_log_hex(lx_pdev.subsystem_vendor);
    lx_log(":");
    lx_log_hex(lx_pdev.subsystem_device);
    lx_log(" rev=");
    lx_log_u((unsigned long)lx_pdev.revision);
    lx_log(" pcie_cap=");
    lx_log_hex(lx_pdev.pcie_cap);
    lx_log(" msi_cap=");
    lx_log_hex(lx_pdev.msi_cap);
    lx_log(" msix_cap=");
    lx_log_hex(lx_pdev.msix_cap);
    lx_log("\n");

    lx_fill_resources(&lx_pdev, bdf);

    // the ACPICA family binds the device to its ACPI namespace node by _ADR
    // under the _BBN/_SEG bridge; other families do not link this. (satoru)
    if (lx_acpi_attach_pci_companion)
        lx_acpi_attach_pci_companion(&lx_pdev);

    if (!drv->probe) {
        lx_log("[lx_pci] driver has no probe()\n");
        return;
    }

    lx_bound_drv = drv;
    lx_log("[lx_pci] calling driver probe() with a REAL device ");
    lx_log_line(lx_pdev_name);
    rc = drv->probe(&lx_pdev, id);
    lx_log("[lx_pci] probe() returned ");
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else        { lx_log_u((unsigned long)rc); }
    lx_log("\n");
    if (rc != 0)
        lx_bound_drv = NULL;
}


// ── region claims ───────────────────────────────────────────────────────────
// the broker already owns the BAR grants and the BAR page mapping; a container
// cannot race another owner for the device. the claim is bookkeeping only.
// (satoru)
int pci_request_region(struct pci_dev *pdev, int bar, const char *res_name)
{
    (void)pdev; (void)bar; (void)res_name;
    return 0;
}

void pci_release_region(struct pci_dev *pdev, int bar)
{
    (void)pdev; (void)bar;
}


// ── host bridge, pci_iomap_range, the graphics opregion (d4) ────────────────
static void lx_host_bridge_release(struct device *dev) { (void)dev; }

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
                                            unsigned int devfn)
{
    long v;
    if (domain != 0 || bus != 0 || devfn != 0) {
        lx_log("[lx_pci] pci_get_domain_bus_and_slot: only the host bridge 00:00.0 is reachable\n");
        return NULL;
    }
    if (!lx_host_bridge_ready) {
        v = lx_broker_call(LX_KLD_OP_HOST_CFG_READ, PCI_VENDOR_ID, 4, 0, 0);
        if (v < 0 || (u32)v == 0xFFFFFFFFu) {
            lx_log("[lx_pci] host bridge not readable (broker refused)\n");
            return NULL;
        }
        device_initialize(&lx_host_bridge.dev);
        lx_host_bridge.dev.release = lx_host_bridge_release;
        dev_set_name(&lx_host_bridge.dev, "0000:00:00.0");
        lx_host_bridge.vendor = (u16)v;
        lx_host_bridge.device = (u16)((u32)v >> 16);
        lx_host_bridge.devfn = 0;
        lx_host_bridge.bus = lx_pdev.bus;
        lx_host_bridge.cfg_size = PCI_CFG_SPACE_SIZE;
        lx_host_bridge_ready = true;
        lx_log("[lx_pci] host bridge 00:00.0 vendor=");
        lx_log_hex(lx_host_bridge.vendor);
        lx_log(" device=");
        lx_log_hex(lx_host_bridge.device);
        lx_log(" (read-only)\n");
    }
    get_device(&lx_host_bridge.dev);
    return &lx_host_bridge;
}

// references handed out by pci_get_* on the bound device; a put without a
// matching get (iwlwifi's removal path) must not drop the device's own
// reference (review f7). (satoru)
static int lx_pdev_got;

void pci_dev_put(struct pci_dev *dev)
{
    if (!dev)
        return;
    if (dev == &lx_pdev) {
        if (lx_pdev_got <= 0) {
            lx_log("[lx_pci] pci_dev_put without a matching get: ignored\n");
            return;
        }
        lx_pdev_got--;
    }
    put_device(&dev->dev);
}

void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
                              unsigned long maxlen)
{
    resource_size_t len = pci_resource_len(dev, bar);
    void __iomem *base;
    if (!len || offset >= len)
        return NULL;
    base = pci_iomap(dev, bar, 0);
    return base ? (void __iomem *)((char __iomem *)base + offset) : NULL;
}

// memremap serves exactly one thing: the bound display device's graphics
// opregion (config 0xFC) and its rvda vbt, as the broker's private copy
// (manifest opregion 1). any other physical address is refused with NULL,
// which every caller handles. (satoru)
#define LX_OPREGION_VA_PAGES 20
static void *lx_opregion_va;
static unsigned long lx_opregion_phys;
static unsigned long lx_opregion_bytes;

void *memremap(resource_size_t offset, size_t size, unsigned long flags)
{
    u32 asls = 0;
    (void)flags;
    if (!lx_bound_drv || pci_read_config_dword(&lx_pdev, 0xFC, &asls) != 0 || !asls)
        goto refuse;
    if (!lx_opregion_va) {
        void *va = lx_os_reserve(LX_OPREGION_VA_PAGES * PAGE_SIZE);
        long rc;
        if (!va)
            return NULL;
        rc = lx_broker_call(LX_KLD_OP_OPREGION_MAP, (unsigned long)va,
                            lx_pdev_bdf, 0, 0);
        if (rc < 0) {
            lx_log("[lx_pci] opregion copy refused by the broker\n");
            lx_os_unreserve(va, LX_OPREGION_VA_PAGES * PAGE_SIZE);
            return NULL;
        }
        lx_opregion_va = (char *)va + (rc & 0xFFF);
        lx_opregion_phys = asls;
        lx_opregion_bytes = (unsigned long)rc >> 12;
        lx_log("[lx_pci] opregion private copy mapped, bytes=");
        lx_log_u(lx_opregion_bytes);
        lx_log("\n");
    }
    if (offset >= lx_opregion_phys && size <= lx_opregion_bytes &&
        offset - lx_opregion_phys <= lx_opregion_bytes - size)
        return (char *)lx_opregion_va + (offset - lx_opregion_phys);
refuse:
    lx_log("[lx_pci] memremap of a physical address that is not the device opregion: refused\n");
    return NULL;
}

// the opregion mapping lives as long as the container (like bar maps). (satoru)
void memunmap(void *addr) { (void)addr; }

// pci_get_class: the container sees exactly one device, its own. (satoru)
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from)
{
    struct pci_dev *dev = NULL;
    if (!from && lx_bound_drv && lx_pdev.class == class) {
        get_device(&lx_pdev.dev);
        lx_pdev_got++;
        dev = &lx_pdev;
    }
    if (from)
        pci_dev_put(from);
    return dev;
}

// end (satoru)
