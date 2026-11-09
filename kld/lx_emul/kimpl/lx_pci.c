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

static struct pci_driver *lx_drivers[LX_MAX_PCI_DRIVERS];
static int lx_driver_count;

void lx_pci_probe_matching(struct pci_driver *drv);
void lx_pci_bind(struct pci_driver *drv, const struct pci_device_id *id,
                 unsigned int bdf);

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

    for (id = drv->id_table; id && id->vendor; id++)
        n++;
    lx_log("[lx_pci]   id_table entries = ");
    lx_log_u((unsigned long)n);
    lx_log("\n");

    if (!lx_broker_available()) {
        lx_log("[lx_pci] kernel broker not reachable - no devices bound\n");
        return 0;
    }
    lx_pci_probe_matching(drv);
    return 0;   // registration succeeded regardless of whether a device bound
}

// ask the broker for each id_table entry in turn; on the first hit, build a
// pci_dev from real config space and call the driver's probe(). (satoru)
void lx_pci_probe_matching(struct pci_driver *drv)
{
    const struct pci_device_id *id;

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

unsigned int lx_pci_bdf_of(const struct pci_dev *dev)
{
    return (dev == &lx_pdev) ? lx_pdev_bdf : 0xFFFFFFFFu;
}

// ── config space, via the broker ────────────────────────────────────────────
// a failed read returns all-ones, as a real bus does for an absent register,
// and a PCIBIOS error code, as upstream's accessors do. (satoru)
static int lx_cfg_read(const struct pci_dev *dev, int where, int width, u32 *val)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    long v;
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
int pci_enable_device_mem(struct pci_dev *dev)
{
    long r = lx_broker_enable(lx_pci_bdf_of(dev));
    if (r < 0)
        return -EIO;
    dev->current_state = PCI_D0;
    atomic_inc(&dev->enable_cnt);
    return 0;
}

int pci_enable_device(struct pci_dev *dev) { return pci_enable_device_mem(dev); }

void pci_disable_device(struct pci_dev *dev)
{
    lx_broker_disable(lx_pci_bdf_of(dev));
    dev->is_busmaster = 0;
    atomic_dec(&dev->enable_cnt);
}

// the broker's ENABLE op turns bus mastering on together with memory decode;
// set_master records it. (satoru)
void pci_set_master(struct pci_dev *dev)  { lx_broker_enable(lx_pci_bdf_of(dev)); dev->is_busmaster = 1; }
void pci_clear_master(struct pci_dev *dev) { lx_broker_disable(lx_pci_bdf_of(dev)); dev->is_busmaster = 0; }

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
void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value) { (void)req; (void)value; }
void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value) { (void)req; (void)new_value; }
void cpu_latency_qos_remove_request(struct pm_qos_request *req) { (void)req; }

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
        }
        return (void __iomem *)((char *)lx_bars[b].va + (offset - r->start));
    }
    lx_log("[lx_pci] ioremap of a physical address outside every granted BAR: refused\n");
    return NULL;
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

    // legacy line from config space; not brokered, kept for the record. (satoru)
    lx_cfg_read(&lx_pdev, PCI_INTERRUPT_LINE, 1, &v);
    lx_pdev.irq = 0;

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

// end (satoru)
