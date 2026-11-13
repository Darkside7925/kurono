// lx_acpi_bus: the minimal ACPI device model the upstream i2c/hid fronts use.
//
// Linux's acpi bus (drivers/acpi/bus.c, scan.c, glue.c, device_pm.c) is not
// isolable: it owns the global ACPI bus, driver binding, power resources and
// sysfs. What the touchpad path actually needs is a struct acpi_device per
// namespace node with its _HID/_CID/_UID/_ADR/_STA, a parent/child walk, the
// companion attached to the right struct device, and the handful of helpers
// i2c-core-acpi / i2c-hid-acpi / mfd-core / gpiolib-acpi call. Those helpers
// are written here against the ACPICA namespace; the big pure library parts
// (resource parsing, _DSM evaluation, fwnode property ops) are the unmodified
// drivers/acpi/{resource,utils,property}.c.
//
// nothing here reimplements an i2c or hid driver: the client is created by
// i2c_acpi_register_devices() and probed by i2c-hid-acpi exactly as on Linux.
// (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>
#include <linux/stdarg.h>
#include <linux/acpi.h>
#include <linux/property.h>
#include <linux/resource_ext.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/pci.h>

#include "accommon.h"
#include "acnamesp.h"

#include "lx_acpi.h"
#include "lx_kshim.h"

// ── companions: one struct acpi_device per namespace node, made on demand ───
struct lx_acpi_host {
    struct list_head list;
    struct acpi_device adev;
};

static LIST_HEAD(lx_acpi_hosts);
static DEFINE_MUTEX(lx_acpi_hosts_lock);

static void lx_acpi_host_release(struct device *dev)
{
    (void)dev;
}

static void lx_acpi_add_id(struct acpi_device *adev, const char *id)
{
    struct acpi_hardware_id *hwid;

    if (!id || !id[0])
        return;
    hwid = kzalloc(sizeof(*hwid), GFP_KERNEL);
    if (!hwid)
        return;
    hwid->id = kstrdup(id, GFP_KERNEL);
    if (!hwid->id) {
        kfree(hwid);
        return;
    }
    list_add_tail(&hwid->list, &adev->pnp.ids);
}

static void lx_acpi_fill_pnp(struct acpi_device *adev, acpi_handle handle)
{
    struct acpi_device_info *info = NULL;
    struct acpi_buffer nb = { ACPI_ALLOCATE_BUFFER, NULL };

    INIT_LIST_HEAD(&adev->pnp.ids);
    if (ACPI_SUCCESS(acpi_get_name(handle, ACPI_SINGLE_NAME, &nb))) {
        snprintf(adev->pnp.bus_id, sizeof(adev->pnp.bus_id), "%s",
                 (char *)nb.pointer);
        ACPI_FREE(nb.pointer);
    }
    if (ACPI_FAILURE(acpi_get_object_info(handle, &info)))
        return;
    if ((info->valid & ACPI_VALID_HID) && info->hardware_id.string) {
        lx_acpi_add_id(adev, info->hardware_id.string);
        adev->pnp.type.hardware_id = 1;
    }
    if (info->valid & ACPI_VALID_CID)
        for (u32 i = 0; i < info->compatible_id_list.count; i++)
            lx_acpi_add_id(adev, info->compatible_id_list.ids[i].string);
    if (info->valid & ACPI_VALID_ADR) {
        adev->pnp.bus_address = info->address;
        adev->pnp.type.bus_address = 1;
    }
    if ((info->valid & ACPI_VALID_UID) && info->unique_id.string) {
        adev->pnp.unique_id = kstrdup(info->unique_id.string, GFP_KERNEL);
        adev->pnp.type.platform_id = 1;
    }
    ACPI_FREE(info);
}

static struct acpi_device *lx_acpi_host_new(acpi_handle handle)
{
    struct lx_acpi_host *h = kzalloc(sizeof(*h), GFP_KERNEL);

    if (!h)
        return NULL;
    h->adev.handle = handle;
    fwnode_init(&h->adev.fwnode, &acpi_device_fwnode_ops);
    device_initialize(&h->adev.dev);
    h->adev.dev.release = lx_acpi_host_release;
    h->adev.dev.fwnode = &h->adev.fwnode;
    lx_acpi_fill_pnp(&h->adev, handle);
    dev_set_name(&h->adev.dev, "%s", h->adev.pnp.bus_id);
    acpi_bus_get_status(&h->adev);
    h->adev.flags.initialized = 1;
    list_add_tail(&h->list, &lx_acpi_hosts);
    return &h->adev;
}

// every namespace device the path touches gets a companion on first use. the
// alternative (a pre-scan of the whole namespace) would cost the same work
// because i2c-core-acpi walks all devices anyway. (satoru)
struct acpi_device *acpi_fetch_acpi_dev(acpi_handle handle)
{
    struct lx_acpi_host *h;
    struct acpi_device *adev = NULL;

    if (!handle)
        return NULL;
    mutex_lock(&lx_acpi_hosts_lock);
    list_for_each_entry(h, &lx_acpi_hosts, list) {
        if (h->adev.handle == handle) {
            adev = &h->adev;
            goto out;
        }
    }
    adev = lx_acpi_host_new(handle);
out:
    mutex_unlock(&lx_acpi_hosts_lock);
    return adev;
}

struct acpi_device *lx_acpi_companion_path(const char *path)
{
    acpi_handle handle = NULL;

    if (!path || ACPI_FAILURE(acpi_get_handle(NULL, (char *)path, &handle)))
        return NULL;
    return acpi_fetch_acpi_dev(handle);
}

int lx_acpi_set_companion(struct device *dev, const char *path)
{
    struct acpi_device *adev = lx_acpi_companion_path(path);

    if (!dev || !adev)
        return -ENODEV;
    set_primary_fwnode(dev, acpi_fwnode_handle(adev));
    return 0;
}

// ── status from _STA ────────────────────────────────────────────────────────
acpi_status acpi_bus_get_status_handle(acpi_handle handle,
                                       unsigned long long *sta)
{
    acpi_status status = acpi_evaluate_integer(handle, "_STA", NULL, sta);

    if (ACPI_SUCCESS(status))
        return AE_OK;
    if (status == AE_NOT_FOUND) {
        *sta = ACPI_STA_DEVICE_PRESENT | ACPI_STA_DEVICE_ENABLED |
               ACPI_STA_DEVICE_UI | ACPI_STA_DEVICE_FUNCTIONING;
        return AE_OK;
    }
    return status;
}

// no _STA override table, no battery dependency graph and no power-resource
// state: this is the status bits the firmware returned. (satoru)
int acpi_bus_get_status(struct acpi_device *device)
{
    unsigned long long sta;
    acpi_status status;

    if (!device || !device->handle)
        return -ENODEV;
    status = acpi_bus_get_status_handle(device->handle, &sta);
    if (ACPI_FAILURE(status))
        return -ENODEV;
    acpi_set_device_status(device, (u32)sta);
    return 0;
}

bool acpi_device_is_present(const struct acpi_device *adev)
{
    return adev->status.present || adev->status.functional;
}

bool acpi_dev_ready_for_enumeration(const struct acpi_device *device)
{
    if (device->flags.honor_deps && device->dep_unmet)
        return false;
    return acpi_device_is_present(device);
}

const char *acpi_device_hid(struct acpi_device *device)
{
    struct acpi_hardware_id *hwid;

    if (!device)
        return NULL;
    list_for_each_entry(hwid, &device->pnp.ids, list)
        return hwid->id;
    return NULL;
}

// ── namespace walks ─────────────────────────────────────────────────────────
struct lx_acpi_walk {
    int (*fn)(struct acpi_device *adev, void *data);
    void *data;
    int ret;
};

static acpi_status lx_acpi_walk_cb(acpi_handle handle, u32 level, void *ctx,
                                   void **ret)
{
    struct lx_acpi_walk *w = ctx;
    struct acpi_device *adev = acpi_fetch_acpi_dev(handle);
    int rc;

    (void)level;
    (void)ret;
    rc = adev ? w->fn(adev, w->data) : 0;
    if (rc) {
        w->ret = rc;
        return AE_CTRL_TERMINATE;
    }
    return AE_OK;
}

int acpi_dev_for_each_child(struct acpi_device *adev,
                            int (*fn)(struct acpi_device *, void *),
                            void *data)
{
    struct lx_acpi_walk w = { .fn = fn, .data = data, .ret = 0 };

    if (!adev)
        return 0;
    acpi_walk_namespace(ACPI_TYPE_DEVICE, adev->handle, 1,
                        lx_acpi_walk_cb, NULL, &w, NULL);
    return w.ret;
}

struct lx_acpi_find_child {
    u64 address;
    bool check_children;
    bool check_sta;
    struct acpi_device *adev;
};

static int lx_acpi_check_child(struct acpi_device *adev, void *data)
{
    struct lx_acpi_find_child *f = data;

    if (!adev->pnp.type.bus_address || adev->pnp.bus_address != f->address)
        return 0;
    if (f->check_children) {
        // a _ADR match without children is the backlight special case upstream
        // answers with a lower score; the touchpad path has one match only. (satoru)
    }
    if (f->check_sta && !adev->status.enabled)
        return 0;
    f->adev = adev;
    return 1;
}

struct acpi_device *acpi_find_child_device(struct acpi_device *parent,
                                           u64 address, bool check_children)
{
    struct lx_acpi_find_child f = {
        .address = address,
        .check_children = check_children,
        .check_sta = true,
        .adev = NULL,
    };

    acpi_dev_for_each_child(parent, lx_acpi_check_child, &f);
    return f.adev;
}

// ── a platform device for an ACPI node, its _CRS parsed by resource.c ──────
static int lx_acpi_res_preproc(struct acpi_resource *ares, void *data)
{
    (void)data;
    if (ares->type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        struct acpi_resource_extended_irq *e = &ares->data.extended_irq;

        lx_log("[kld-acpi] _CRS irq gsi=");
        lx_log_u(e->interrupt_count ? e->interrupts[0] : 0);
        lx_log(" count=");
        lx_log_u(e->interrupt_count);
        lx_log(e->triggering == ACPI_LEVEL_SENSITIVE ? " level" : " edge");
        lx_log(e->polarity == ACPI_ACTIVE_LOW ? " active-low" : " active-high");
        lx_log(e->shareable == ACPI_SHARED ? " shared" : " exclusive");
        lx_log("\n");
    } else if (ares->type == ACPI_RESOURCE_TYPE_GPIO) {
        struct acpi_resource_gpio *g = &ares->data.gpio;

        if (g->connection_type == ACPI_RESOURCE_GPIO_TYPE_INT) {
            lx_log("[kld-acpi] _CRS gpio-int pin=");
            lx_log_u(g->pin_table_length ? g->pin_table[0] : 0);
            lx_log(" source=");
            lx_log(g->resource_source.string_ptr ?
                   g->resource_source.string_ptr : "-");
            lx_log(g->triggering == ACPI_LEVEL_SENSITIVE ? " level" : " edge");
            lx_log(g->polarity == ACPI_ACTIVE_LOW ? " active-low" : " active-high");
            lx_log("\n");
        }
    }
    return 0;
}

static const char *lx_acpi_irq_desc(unsigned long flags)
{
    if (flags & IORESOURCE_IRQ_LOWLEVEL)
        return "level active-low";
    if (flags & IORESOURCE_IRQ_HIGHLEVEL)
        return "level active-high";
    if (flags & IORESOURCE_IRQ_LOWEDGE)
        return "edge falling";
    if (flags & IORESOURCE_IRQ_HIGHEDGE)
        return "edge rising";
    return "irq";
}

// up to 16 resources on this path; the caller owns the returned device and
// adds it AFTER pointing its register model at the memory ranges. (satoru)
struct platform_device *lx_acpi_platform_device_alloc(const char *path)
{
    struct acpi_device *adev;
    struct platform_device *pdev;
    struct resource res[16];
    struct resource_entry *e;
    struct acpi_hardware_id *hwid;
    LIST_HEAD(list);
    char name[32];
    int n = 0, rc;

    adev = lx_acpi_companion_path(path);
    if (!adev)
        return ERR_PTR(-ENODEV);
    hwid = NULL;
    list_for_each_entry(hwid, &adev->pnp.ids, list)
        break;
    snprintf(name, sizeof(name), "%s:00", hwid ? hwid->id : "ACPI");
    lx_log("[kld-acpi] platform ");
    lx_log(name);
    lx_log(" hi");
    lx_log("d=");
    lx_log(hwid ? hwid->id : "-");
    lx_log(" sta=");
    lx_log_hex(*(u32 *)&adev->status & 0xff);
    lx_log("\n");

    rc = acpi_dev_get_resources(adev, &list, lx_acpi_res_preproc, NULL);
    if (rc < 0) {
        lx_log("[kld-acpi] platform _CRS: ");
        lx_log_line(acpi_format_exception((acpi_status)rc));
        return ERR_PTR(rc);
    }
    list_for_each_entry(e, &list, node) {
        if (n >= (int)ARRAY_SIZE(res))
            break;
        res[n] = *e->res;
        lx_log("[kld-acpi] platform res ");
        if (res[n].flags & IORESOURCE_MEM) {
            lx_log("mem ");
            lx_log_hex(res[n].start);
            lx_log("-");
            lx_log_hex(res[n].end);
        } else if (res[n].flags & IORESOURCE_IRQ) {
            lx_log("irq ");
            lx_log_u(res[n].start);
            lx_log(" (");
            lx_log(lx_acpi_irq_desc(res[n].flags));
            lx_log((res[n].flags & IORESOURCE_IRQ_SHAREABLE) ?
                   " shared)" : " exclusive)");
        } else {
            lx_log("type ");
            lx_log_hex(res[n].flags & IORESOURCE_TYPE_BITS);
        }
        lx_log("\n");
        n++;
    }
    acpi_dev_free_resource_list(&list);

    pdev = platform_device_alloc(name, -1);
    if (!pdev)
        return ERR_PTR(-ENOMEM);
    // the companion before probe, so gpiolib-acpi can attach the chip. (satoru)
    ACPI_COMPANION_SET(&pdev->dev, adev);
    if (n && platform_device_add_resources(pdev, res, (unsigned int)n)) {
        platform_device_put(pdev);
        return ERR_PTR(-ENOMEM);
    }
    return pdev;
}

// ── ACPI id matching (upstream __acpi_match_device, string ids only) ───────
static const struct acpi_device_id *
lx_acpi_match_ids(struct acpi_device *adev, const struct acpi_device_id *ids)
{
    struct acpi_hardware_id *hwid;

    if (!adev || !adev->status.present)
        return NULL;
    list_for_each_entry(hwid, &adev->pnp.ids, list) {
        const struct acpi_device_id *id;

        for (id = ids; ids && (id->id[0] || id->cls); id++)
            if (id->id[0] && !strcmp((char *)id->id, hwid->id))
                return id;
    }
    return NULL;
}

int acpi_match_device_ids(struct acpi_device *device,
                          const struct acpi_device_id *ids)
{
    return lx_acpi_match_ids(device, ids) ? 0 : -ENOENT;
}

const struct acpi_device *acpi_companion_match(const struct device *dev)
{
    struct acpi_device *adev = ACPI_COMPANION(dev);

    if (!adev || list_empty(&adev->pnp.ids))
        return NULL;
    return adev;
}

bool acpi_driver_match_device(struct device *dev,
                              const struct device_driver *drv)
{
    const struct acpi_device_id *ids = drv->acpi_match_table;

    if (!ids)
        return false;
    return lx_acpi_match_ids((struct acpi_device *)acpi_companion_match(dev),
                             ids) != NULL;
}

// the platform/ACPI match data a driver's id table carries (pinctrl-meteorpoint
// puts its SoC data there and reads it through device_get_match_data). (satoru)
const void *device_get_match_data(const struct device *dev)
{
    const struct acpi_device_id *match;
    const struct acpi_device_id *ids;

    if (!dev || !dev->driver)
        return NULL;
    ids = dev->driver->acpi_match_table;
    if (!ids)
        return NULL;
    match = lx_acpi_match_ids((struct acpi_device *)acpi_companion_match(dev),
                              ids);
    return match ? (const void *)match->driver_data : NULL;
}

const void *acpi_device_get_match_data(const struct device *dev)
{
    const struct acpi_device_id *match;
    const struct acpi_device_id *ids = dev->driver->acpi_match_table;

    if (!ids)
        return NULL;
    match = lx_acpi_match_ids((struct acpi_device *)acpi_companion_match(dev),
                              ids);
    return match ? (const void *)match->driver_data : NULL;
}

// ── modalias, power, dependencies ───────────────────────────────────────────
static int lx_acpi_build_modalias(const struct acpi_device *adev, char *buf,
                                  int size)
{
    struct acpi_hardware_id *hwid;
    int len = 0;

    if (!adev || list_empty(&adev->pnp.ids))
        return 0;
    len = snprintf(buf, size, "acpi:");
    list_for_each_entry(hwid, &adev->pnp.ids, list) {
        int n = snprintf(buf + len, size - len, "%s:", hwid->id);

        if (n < 0 || n >= size - len)
            return -ENOMEM;
        len += n;
    }
    return len;
}

// no "compatible" _DSD in the container model: the default id is the modalias
// upstream would fall back to. (satoru)
void acpi_set_modalias(struct acpi_device *adev, const char *default_id,
                       char *modalias, size_t len)
{
    (void)adev;
    strscpy(modalias, default_id, len);
}

int acpi_device_modalias(struct device *dev, char *buf, int size)
{
    return lx_acpi_build_modalias(ACPI_COMPANION(dev), buf, size);
}

int acpi_device_uevent_modalias(const struct device *dev,
                                struct kobj_uevent_env *env)
{
    char buf[256];
    int len = lx_acpi_build_modalias(ACPI_COMPANION(dev), buf, sizeof(buf));

    if (len <= 0)
        return len;
    return add_uevent_var(env, "MODALIAS=%s", buf);
}

// the container has no ACPI power resources and no _PSC state machine. the
// device-level _PSn method is the part the firmware actually implements here,
// so it is executed (and its opregion writes land in the census). (satoru)
int acpi_device_set_power(struct acpi_device *device, int state)
{
    static const char *const ps[] = {
        [ACPI_STATE_D0] = "_PS0",
        [ACPI_STATE_D1] = "_PS1",
        [ACPI_STATE_D2] = "_PS2",
        [ACPI_STATE_D3_HOT] = "_PS3",
        [ACPI_STATE_D3_COLD] = "_PS3",
    };

    if (!device || !device->handle)
        return -ENODEV;
    if (state < ACPI_STATE_D0 || state > ACPI_STATE_D3_COLD || !ps[state])
        return -EINVAL;
    if (acpi_has_method(device->handle, (char *)ps[state]))
        acpi_execute_simple_method(device->handle, (char *)ps[state], 0);
    return 0;
}

int acpi_device_fix_up_power(struct acpi_device *device)
{
    return device ? acpi_device_set_power(device, ACPI_STATE_D0) : 0;
}

void acpi_dev_clear_dependencies(struct acpi_device *supplier)
{
    // no _DEP graph in the container model; device links are not created. (satoru)
    (void)supplier;
}

int acpi_unbind_one(struct device *dev)
{
    if (dev)
        set_primary_fwnode(dev, NULL);
    return 0;
}

// ── private data, keyed by namespace handle (i2c's GSBUS handler uses it) ──
struct lx_acpi_priv {
    struct list_head list;
    acpi_handle handle;
    void *data;
};

static LIST_HEAD(lx_acpi_privs);
static DEFINE_MUTEX(lx_acpi_privs_lock);

int acpi_bus_attach_private_data(acpi_handle handle, void *data)
{
    struct lx_acpi_priv *p = kzalloc(sizeof(*p), GFP_KERNEL);

    if (!p)
        return -ENOMEM;
    p->handle = handle;
    p->data = data;
    mutex_lock(&lx_acpi_privs_lock);
    list_add_tail(&p->list, &lx_acpi_privs);
    mutex_unlock(&lx_acpi_privs_lock);
    return 0;
}

int acpi_bus_get_private_data(acpi_handle handle, void **data)
{
    struct lx_acpi_priv *p;

    if (!data)
        return -EINVAL;
    mutex_lock(&lx_acpi_privs_lock);
    list_for_each_entry(p, &lx_acpi_privs, list) {
        if (p->handle == handle) {
            *data = p->data;
            mutex_unlock(&lx_acpi_privs_lock);
            return 0;
        }
    }
    mutex_unlock(&lx_acpi_privs_lock);
    return -ENOENT;
}

void acpi_bus_detach_private_data(acpi_handle handle)
{
    struct lx_acpi_priv *p, *n;

    mutex_lock(&lx_acpi_privs_lock);
    list_for_each_entry_safe(p, n, &lx_acpi_privs, list) {
        if (p->handle == handle) {
            list_del(&p->list);
            kfree(p);
            break;
        }
    }
    mutex_unlock(&lx_acpi_privs_lock);
}

// ── small kernel-side bindings the compiled upstream files expect ──────────
// the container has no global iomem resource tree to test against. (satoru)
int acpi_check_resource_conflict(const struct resource *res)
{
    (void)res;
    return 0;
}

// there is one fwnode per device; the secondary-fwnode chain of the real
// device core is not modelled. (satoru)
void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode)
{
    struct fwnode_handle *fn;

    if (!dev)
        return;
    fn = dev->fwnode;
    if (fwnode) {
        // an existing primary becomes the new primary's secondary. (satoru)
        if (fn && !IS_ERR(fn->secondary))
            fn = fn->secondary;
        if (fn)
            fwnode->secondary = fn;
        dev->fwnode = fwnode;
    } else if (fn && !IS_ERR(fn->secondary)) {
        dev->fwnode = fn->secondary;
        fn->secondary = NULL;
    } else {
        dev->fwnode = NULL;
    }
}

// kernel/resource.c's list helpers, copied because linking the whole kernel
// resource tree is not an option in a container. (satoru)
struct resource_entry *resource_list_create_entry(struct resource *res,
                                                  size_t extra_size)
{
    struct resource_entry *entry;

    entry = kzalloc(sizeof(*entry) + extra_size, GFP_KERNEL);
    if (entry) {
        INIT_LIST_HEAD(&entry->node);
        entry->res = res ? res : &entry->__res;
    }
    return entry;
}

void resource_list_free(struct list_head *head)
{
    struct resource_entry *entry, *tmp;

    list_for_each_entry_safe(entry, tmp, head, node)
        resource_list_destroy_entry(entry);
}

// GSI translation: the container's irq numbers are its software lines. a GSI
// gets one line on first use, the way the kernel maps a GSI to a Linux virq;
// level/interrupt routing on the real chip is the broker part. (satoru)
#define LX_ACPI_GSI_MAX 8
static struct {
    u32 gsi;
    int irq;
} lx_acpi_gsi_map[LX_ACPI_GSI_MAX];
static int lx_acpi_ngsi;

int lx_acpi_gsi_to_irq(u32 gsi)
{
    unsigned int irq;

    for (int i = 0; i < lx_acpi_ngsi; i++)
        if (lx_acpi_gsi_map[i].gsi == gsi)
            return lx_acpi_gsi_map[i].irq;
    if (lx_acpi_ngsi >= LX_ACPI_GSI_MAX ||
        lx_irq_alloc_soft(&irq))
        return -1;
    lx_acpi_gsi_map[lx_acpi_ngsi].gsi = gsi;
    lx_acpi_gsi_map[lx_acpi_ngsi].irq = (int)irq;
    lx_acpi_ngsi++;
    lx_log("[kld-acpi] GSI ");
    lx_log_u(gsi);
    lx_log(" -> container irq ");
    lx_log_u(irq);
    lx_log("\n");
    return (int)irq;
}

// no IO-APIC source override table in the container. (satoru)
int acpi_get_override_irq(u32 gsi, int *triggering, int *polarity)
{
    (void)gsi;
    if (triggering)
        *triggering = 0;
    if (polarity)
        *polarity = 0;
    return -1;
}

int acpi_register_gsi(struct device *dev, u32 gsi, int trigger, int polarity)
{
    (void)dev;
    (void)trigger;
    (void)polarity;
    return lx_acpi_gsi_to_irq(gsi);
}

bool acpi_int_src_ovr[16];

// the ACPICA families attach real fwnodes, so the generic device matchers
// must compare them; the pre-ACPI shim answered 0 for every device. (satoru)
int device_match_fwnode(struct device *dev, const void *fwnode)
{
    return dev && dev_fwnode(dev) == fwnode;
}

int device_match_acpi_handle(struct device *dev, const void *handle)
{
    return dev && ACPI_HANDLE(dev) == (acpi_handle)handle;
}

// wakeup is bookkeeping in a container: there is no suspend, so a device can
// be marked wake-capable but no wakeup source is created. (satoru)
// device_init_wakeup() is an inline in pm_wakeup.h over these two. (satoru)
void device_wakeup_disable(struct device *dev)
{
    (void)dev;
}

void device_set_wakeup_capable(struct device *dev, bool capable)
{
    if (dev)
        dev->power.can_wakeup = capable;
}

// ── uevent buffer append (lib/kobject_uevent.c shape) ──────────────────────
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
    va_list args;
    int len;

    if (!env || env->envp_idx >= ARRAY_SIZE(env->envp))
        return -ENOMEM;
    va_start(args, format);
    len = vsnprintf(&env->buf[env->buflen],
                    sizeof(env->buf) - env->buflen, format, args);
    va_end(args);
    if (len >= (int)(sizeof(env->buf) - env->buflen))
        return -ENOMEM;
    env->envp[env->envp_idx++] = &env->buf[env->buflen];
    env->buflen += len + 1;
    return 0;
}

// ── the PCI function's companion, by _ADR under the _BBN/_SEG bridge ───────
struct lx_acpi_pci_find {
    u64 adr;
    unsigned int bus;
    unsigned int seg;
    struct acpi_device *adev;
};

static int lx_acpi_pci_eval_int(acpi_handle h, const char *m, u64 *out)
{
    union acpi_object o;
    struct acpi_buffer b = { sizeof(o), &o };

    if (!h)
        return -1;
    if (ACPI_FAILURE(acpi_evaluate_object_typed(h, (char *)m, NULL, &b,
                                                ACPI_TYPE_INTEGER)))
        return -1;
    *out = o.integer.value;
    return 0;
}

static int lx_acpi_pci_match(struct acpi_device *adev, void *data)
{
    struct lx_acpi_pci_find *f = data;
    acpi_handle cur = adev->handle;
    u64 bbn = 0, seg = 0;
    bool have_bus = false;
    int have_seg = -1;

    if (!adev->pnp.type.bus_address || adev->pnp.bus_address != f->adr)
        return 0;
    for (; cur; ) {
        acpi_handle parent = NULL;

        if (!have_bus && !lx_acpi_pci_eval_int(cur, "_BBN", &bbn))
            have_bus = true;
        if (have_seg < 0 && !lx_acpi_pci_eval_int(cur, "_SEG", &seg))
            have_seg = 0;
        if ((have_bus && have_seg >= 0) ||
            ACPI_FAILURE(acpi_get_parent(cur, &parent)) || !parent)
            break;
        cur = parent;
    }
    if (!have_bus || bbn != f->bus || (have_seg >= 0 && seg != f->seg))
        return 0;
    if (!adev->status.enabled)
        return 0;
    f->adev = adev;
    return 1;
}

static struct acpi_device *lx_acpi_pci_companion(unsigned int bus,
                                                 unsigned int seg, u64 adr)
{
    struct lx_acpi_pci_find f = {
        .adr = adr,
        .bus = bus,
        .seg = seg,
        .adev = NULL,
    };
    struct lx_acpi_walk w = { .fn = lx_acpi_pci_match, .data = &f, .ret = 0 };

    acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, 32,
                        lx_acpi_walk_cb, NULL, &w, NULL);
    return f.adev;
}

// pci_enable_device's D0, ACPI side: the companion's _PS0 method. this laptop's
// I2C1 has no _PS0 (the firmware leaves it in D0), so the hook reports that and
// does nothing; when a machine does define it, the method runs with the census
// and the broker allowlist in force. (satoru)
int lx_acpi_pci_power_on(struct pci_dev *pdev)
{
    struct acpi_device *adev;
    acpi_handle h;

    if (!pdev)
        return -ENODEV;
    adev = ACPI_COMPANION(&pdev->dev);
    if (!adev)
        return 0;
    if (ACPI_FAILURE(acpi_get_handle(adev->handle, "_PS0", &h))) {
        lx_log("[kld-acpi] companion ");
        lx_log(adev->pnp.bus_id);
        lx_log(" has no _PS0; D0 is the firmware default\n");
        return 0;
    }
    lx_log("[kld-acpi] companion ");
    lx_log(adev->pnp.bus_id);
    lx_log(" _PS0 through lx_acpi\n");
    return acpi_device_set_power(adev, ACPI_STATE_D0);
}

void lx_acpi_attach_pci_companion(struct pci_dev *pdev)
{
    struct acpi_device *adev;
    u64 adr;
    unsigned int bus, devfn;

    if (!pdev || !pdev->bus)
        return;
    bus = pdev->bus->number;
    devfn = pdev->devfn;
    adr = ((u64)(devfn & 0xF8) << 13) | (devfn & 0x7);
    adev = lx_acpi_pci_companion(bus, 0, adr);
    if (!adev) {
        lx_log("[kld-acpi] no companion for PCI ");
        lx_log_hex(bus);
        lx_log(":");
        lx_log_hex(devfn);
        lx_log(" adr ");
        lx_log_hex((unsigned long)adr);
        lx_log("\n");
        return;
    }
    set_primary_fwnode(&pdev->dev, acpi_fwnode_handle(adev));
    lx_log("[kld-acpi] PCI companion ");
    lx_log(adev->pnp.bus_id);
    lx_log(" adr ");
    lx_log_hex((unsigned long)adr);
    lx_log("\n");
}

// end (satoru)
