// lx_emul: pure-computation helpers and diagnostics.
//
// Everything in here is real, complete, and stateless - no device interaction,
// so there is no reason to stub any of it. These are the cheapest symbols in the
// census to retire.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/string.h>
#include <linux/bitmap.h>
#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/uaccess.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/dmi.h>
#include <linux/property.h>
#include <net/net_namespace.h>
#include <net/dropreason.h>

#include "lx_kshim.h"

extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern void *malloc(unsigned long);

// ── strings ─────────────────────────────────────────────────────────────────
// strscpy semantics: NUL-terminate, return the length copied, or -E2BIG on
// truncation. Getting the return convention wrong here silently truncates
// device names, so it is worth being exact. (satoru)
ssize_t sized_strscpy(char *dst, const char *src, size_t count)
{
    size_t i;
    if (count == 0)
        return -E2BIG;
    for (i = 0; i < count - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    return src[i] ? -E2BIG : (ssize_t)i;
}

char *kstrdup(const char *s, gfp_t gfp)
{
    size_t n;
    char *p;
    (void)gfp;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

// ── bitmap/find_bit/crc32 come from lib/bitmap.c, lib/find_bit.c and
// lib/crc32.c (CORE LIBRARY), so there is no hand-written copy here. (satoru)

// ── kobject string helpers used by lib/kobject.c ────────────────────────────
// kvasprintf_const may return .rodata in the kernel; ours always allocates, so
// kstrdup_const always copies in the container; the strings this shim sees are
// short driver labels, not the firmware's rodata. (satoru)
__attribute__((weak)) const char *kstrdup_const(const char *s, gfp_t gfp)
{
    return s ? kstrdup(s, gfp) : NULL;
}

// kfree_const is a plain kfree. (satoru)
extern char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap);
extern void kfree(const void *p);

const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list ap)
{
    return kvasprintf(gfp, fmt, ap);
}

void kfree_const(const void *x)
{
    kfree((void *)x);
}

// ── checksums ───────────────────────────────────────────────────────────────
// only reached on the IPv6 TX-offload path. Folding is standard ones-complement.
__sum16 csum_ipv6_magic(const struct in6_addr *saddr, const struct in6_addr *daddr,
                        __u32 len, __u8 proto, __wsum csum)
{
    u64 sum = (u64)csum;
    int i;
    for (i = 0; i < 4; i++) sum += (u64)saddr->s6_addr32[i];
    for (i = 0; i < 4; i++) sum += (u64)daddr->s6_addr32[i];
    sum += (u64)htonl(len);
    sum += (u64)htonl(proto);
    while (sum >> 32)
        sum = (sum & 0xffffffff) + (sum >> 32);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (__sum16)~sum;
}

// ── ethtool link-mode conversion ────────────────────────────────────────────
void ethtool_convert_legacy_u32_to_link_mode(unsigned long *dst, u32 legacy)
{
    bitmap_zero(dst, __ETHTOOL_LINK_MODE_MASK_NBITS);
    dst[0] = legacy;
}

bool ethtool_convert_link_mode_to_legacy_u32(u32 *legacy, const unsigned long *src)
{
    bool retval = true;
    *legacy = (u32)src[0];
    // report whether any bit above 32 was dropped, like upstream does
    if (__ETHTOOL_LINK_MODE_MASK_NBITS > 32) {
        int i;
        for (i = 1; i < BITS_TO_LONGS(__ETHTOOL_LINK_MODE_MASK_NBITS); i++)
            if (src[i])
                retval = false;
    }
    return retval;
}

// warn/bug sites: one line per call site, first time it fires. M2 reads these
// in the container log. (satoru)
struct lx_warn_site_rec {
    const char *file;
    int line;
};

static struct lx_warn_site_rec g_warn_sites[512];
static int g_warn_n;
static volatile unsigned int g_warn_lock;

void lx_warn_site(const char *file, int line, const char *func)
{
    lx_flock(&g_warn_lock);
    for (int i = 0; i < g_warn_n; i++) {
        if (g_warn_sites[i].line == line && g_warn_sites[i].file &&
            file && strcmp(g_warn_sites[i].file, file) == 0) {
            lx_funlock(&g_warn_lock);
            return;
        }
    }
    if (g_warn_n < (int)(sizeof(g_warn_sites) / sizeof(g_warn_sites[0]))) {
        g_warn_sites[g_warn_n].file = file;
        g_warn_sites[g_warn_n].line = line;
        g_warn_n++;
    }
    lx_funlock(&g_warn_lock);
    lx_log("[kld-warn] ");
    lx_log(file ? file : "?");
    lx_log(":");
    lx_log_u((unsigned long)line);
    lx_log(" ");
    lx_log_line(func ? func : "?");
    lx_log("[kld-warn-bt] thread=");
    lx_log(lx_current_name());
    lx_log(" ra0=0x");
    lx_log_hex((unsigned long)__builtin_return_address(0));
    // the work item running on this thread, if any: its callback is the
    // caller to look up with addr2line. (satoru)
    struct work_struct *rw = lx_work_running();
    if (rw) {
        lx_log(" work_fn=0x");
        lx_log_hex((unsigned long)rw->func);
    }
    lx_log("\n");
}

__attribute__((noreturn)) void lx_bug_site(const char *file, int line, const char *func)
{
    lx_warn_site(file, line, func);
    lx_log("[kld-warn] BUG: aborting container\n");
    lx_os_exit(42);
}

#ifndef KLD_ACPI_BUILD
// no ACPI namespace is exposed; the driver treats NULL as "not supported". the
// ACPICA families link upstream drivers/acpi/utils.c instead. (satoru)
union acpi_object *acpi_evaluate_dsm(acpi_handle handle, const guid_t *guid,
                                     u64 rev, u64 func, union acpi_object *argv4)
{
    (void)handle; (void)guid; (void)rev; (void)func; (void)argv4;
    return NULL;
}
#endif

// sysfs is not modelled; the wiphy/netdev symlinks are cosmetic. (satoru)
int sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name)
{
    (void)kobj; (void)target; (void)name;
    return 0;
}

void sysfs_remove_link(struct kobject *kobj, const char *name)
{
    (void)kobj; (void)name;
}

// the netlink extack tracepoint is tracing-only; no tracepoints in this
// config. (satoru)
void do_trace_netlink_extack(const char *msg)
{
    (void)msg;
}

// no DMI/SMBIOS table is exposed to the container; DMI quirks never match and
// dmi_get_system_info has no string. (satoru)
int dmi_check_system(const struct dmi_system_id *list)
{
    (void)list;
    return 0;
}

const char *dmi_get_system_info(int field)
{
    (void)field;
    return NULL;
}

#ifndef KLD_ACPI_BUILD
// gpio-mockup is a board device with no firmware node in the container: its
// probe asks for nr-gpios. every other property read is "absent", which makes
// the driver use its own defaults. the ACPICA families link the real matcher
// in lx_acpi_bus.c. (satoru)
const void *device_get_match_data(const struct device *dev)
{
    (void)dev;
    return NULL;
}
#endif

#ifndef KLD_ACPI_BUILD
int device_match_fwnode(struct device *dev, const void *fwnode)
{
    (void)dev; (void)fwnode;
    return 0;
}

int device_match_acpi_handle(struct device *dev, const void *handle)
{
    (void)dev; (void)handle;
    return 0;
}
#endif

int device_match_of_node(struct device *dev, const void *node)
{
    (void)dev; (void)node;
    return 0;
}

// ── software nodes: the built-in property arrays platform devices carry ─────
// upstream creates these with fwnode_create_software_node() (gpio-mockup's
// configfs path) or device_create_managed_software_node() (platform core with
// pdevinfo.properties). the container needs the property reads, not the sysfs
// face, so each node is a fwnode plus its property array. (satoru)
struct lx_swnode {
    struct list_head list;
    struct fwnode_handle fwnode;
    const struct property_entry *props;
};

static LIST_HEAD(lx_swnodes);
static DEFINE_SPINLOCK(lx_swnodes_lock);

static const struct property_entry *lx_swnode_props(
    const struct fwnode_handle *fwnode)
{
    struct lx_swnode *n;

    if (!fwnode)
        return NULL;
    list_for_each_entry(n, &lx_swnodes, list)
        if (&n->fwnode == fwnode)
            return n->props;
    return NULL;
}

static const struct property_entry *lx_prop_find_array(
    const struct property_entry *p, const char *name)
{
    if (!p || !name)
        return NULL;
    for (; p->name; p++)
        if (!strcmp(p->name, name))
            return p;
    return NULL;
}

static const struct property_entry *lx_prop_find_dev(const struct device *dev,
                                                     const char *name)
{
    const struct fwnode_handle *fwnode;
    const struct property_entry *p;

    if (!dev)
        return NULL;
    fwnode = __dev_fwnode_const(dev);
    p = lx_prop_find_array(lx_swnode_props(fwnode), name);
    if (p)
        return p;
    // a software node attached with device_create_managed_software_node() is
    // the device's secondary fwnode, so the ACPI companion stays primary. (satoru)
    if (fwnode && !IS_ERR(fwnode->secondary)) {
        p = lx_prop_find_array(lx_swnode_props(fwnode->secondary), name);
        if (p)
            return p;
    }
    return NULL;
}

static const struct property_entry *lx_prop_find_fwnode(
    const struct fwnode_handle *fwnode, const char *name)
{
    return lx_prop_find_array(lx_swnode_props(fwnode), name);
}

static const char *lx_swnode_get_name(const struct fwnode_handle *fwnode)
{
    (void)fwnode;
    return "software-node";
}

static struct fwnode_handle *lx_swnode_get_parent(
    const struct fwnode_handle *fwnode)
{
    (void)fwnode;
    return NULL;
}

static bool lx_swnode_property_present(const struct fwnode_handle *fwnode,
                                       const char *propname)
{
    return lx_prop_find_fwnode(fwnode, propname) != NULL;
}

static int lx_swnode_property_read_int_array(const struct fwnode_handle *fwnode,
                                             const char *propname,
                                             unsigned int elem_size, void *val,
                                             size_t nval)
{
    const struct property_entry *p = lx_prop_find_fwnode(fwnode, propname);
    unsigned int type_size;
    size_t n, bytes;

    if (!p)
        return -EINVAL;
    switch (p->type) {
    case DEV_PROP_U8: type_size = sizeof(u8); break;
    case DEV_PROP_U16: type_size = sizeof(u16); break;
    case DEV_PROP_U32: type_size = sizeof(u32); break;
    case DEV_PROP_U64: type_size = sizeof(u64); break;
    default: return -EINVAL;
    }
    if (elem_size != type_size)
        return -EINVAL;
    n = p->length / type_size;
    if (nval > n)
        nval = n;
    bytes = nval * type_size;
    if (p->is_inline)
        memcpy(val, &p->value, bytes);
    else if (p->pointer)
        memcpy(val, p->pointer, bytes);
    else
        return -EINVAL;
    return 0;
}

static int lx_swnode_property_read_string_array(
    const struct fwnode_handle *fwnode, const char *propname, const char **val,
    size_t nval)
{
    const struct property_entry *p = lx_prop_find_fwnode(fwnode, propname);
    size_t n, i;

    if (!p || p->type != DEV_PROP_STRING)
        return -EINVAL;
    n = p->length / sizeof(char *);
    if (nval > n)
        nval = n;
    for (i = 0; i < nval; i++) {
        if (p->is_inline)
            val[i] = p->value.str[i];
        else
            val[i] = ((const char *const *)p->pointer)[i];
    }
    return 0;
}

static const struct fwnode_operations lx_swnode_ops = {
    .property_present = lx_swnode_property_present,
    .property_read_int_array = lx_swnode_property_read_int_array,
    .property_read_string_array = lx_swnode_property_read_string_array,
    .get_name = lx_swnode_get_name,
    .get_parent = lx_swnode_get_parent,
};

struct fwnode_handle *fwnode_create_software_node(
    const struct property_entry *properties, const struct fwnode_handle *parent)
{
    struct lx_swnode *n;

    (void)parent;
    n = kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n)
        return ERR_PTR(-ENOMEM);
    INIT_LIST_HEAD(&n->list);
    fwnode_init(&n->fwnode, &lx_swnode_ops);
    n->props = properties;
    spin_lock(&lx_swnodes_lock);
    list_add_tail(&n->list, &lx_swnodes);
    spin_unlock(&lx_swnodes_lock);
    return &n->fwnode;
}

void fwnode_remove_software_node(struct fwnode_handle *fwnode)
{
    struct lx_swnode *n, *tmp;

    if (!fwnode)
        return;
    spin_lock(&lx_swnodes_lock);
    list_for_each_entry_safe(n, tmp, &lx_swnodes, list) {
        if (&n->fwnode == fwnode) {
            list_del(&n->list);
            kfree(n);
            break;
        }
    }
    spin_unlock(&lx_swnodes_lock);
}

int device_create_managed_software_node(struct device *dev,
                                        const struct property_entry *properties,
                                        const struct software_node *parent)
{
    struct fwnode_handle *fwnode;

    if (!dev || !properties)
        return -EINVAL;
    fwnode = fwnode_create_software_node(properties, NULL);
    if (IS_ERR(fwnode))
        return PTR_ERR(fwnode);
    // upstream swnode.c: a managed software node is the *secondary* fwnode, so
    // an ACPI companion stays primary. (satoru)
    set_secondary_fwnode(dev, fwnode);
    return 0;
}

// upstream core.c semantics, including the primary/secondary marker. (satoru)
void set_secondary_fwnode(struct device *dev, struct fwnode_handle *fwnode)
{
    if (!dev)
        return;
    if (fwnode)
        fwnode->secondary = ERR_PTR(-ENODEV);
    if (dev->fwnode && !IS_ERR(dev->fwnode->secondary))
        dev->fwnode->secondary = fwnode;
    else
        dev->fwnode = fwnode;
}

bool is_software_node(const struct fwnode_handle *fwnode)
{
    return fwnode && fwnode->ops == &lx_swnode_ops;
}

bool fwnode_property_present(const struct fwnode_handle *fwnode, const char *propname)
{
    (void)fwnode; (void)propname;
    return false;
}

int fwnode_property_read_string(const struct fwnode_handle *fwnode,
                                const char *propname, const char **val)
{
    (void)fwnode; (void)propname; (void)val;
    return -EINVAL;
}

int fwnode_property_read_u32_array(const struct fwnode_handle *fwnode,
                                   const char *propname, u32 *val, size_t nval)
{
    (void)fwnode; (void)propname; (void)val; (void)nval;
    return -EINVAL;
}

int fwnode_property_match_string(const struct fwnode_handle *fwnode,
                                 const char *propname, const char *string)
{
    (void)fwnode; (void)propname; (void)string;
    return -EINVAL;
}

struct fwnode_handle *device_get_next_child_node(const struct device *dev,
                                                 struct fwnode_handle *child)
{
    (void)dev; (void)child;
    return NULL;
}


void device_set_node(struct device *dev, struct fwnode_handle *fwnode)
{
    if (dev)
        dev->fwnode = fwnode;
}

__attribute__((weak)) struct fwnode_handle *__dev_fwnode(struct device *dev)
{
    return dev ? dev->fwnode : NULL;
}

const struct fwnode_handle *__dev_fwnode_const(const struct device *dev)
{
    return dev ? dev->fwnode : NULL;
}

bool device_property_present(const struct device *dev, const char *propname)
{
    return lx_prop_find_dev(dev, propname) != NULL;
}

int device_property_read_u16_array(const struct device *dev, const char *propname,
                                   u16 *val, size_t nval)
{
    const struct property_entry *p = lx_prop_find_dev(dev, propname);
    size_t n;

    if (!p || p->type != DEV_PROP_U16 || !val)
        return -EINVAL;
    n = p->length / sizeof(u16);
    if (nval < n)
        n = nval;
    for (size_t i = 0; i < n; i++)
        val[i] = p->is_inline ? p->value.u16_data[i] :
                 ((const u16 *)p->pointer)[i];
    return 0;
}

int device_property_read_u32_array(const struct device *dev, const char *propname,
                                   u32 *val, size_t nval)
{
    const struct property_entry *p = lx_prop_find_dev(dev, propname);
    size_t n;

    if (!p || p->type != DEV_PROP_U32 || !val)
        return -EINVAL;
    n = p->length / sizeof(u32);
    if (nval < n)
        n = nval;
    for (size_t i = 0; i < n; i++)
        val[i] = p->is_inline ? p->value.u32_data[i] :
                 ((const u32 *)p->pointer)[i];
    return 0;
}

int device_property_read_string_array(const struct device *dev,
                                      const char *propname, const char **val,
                                      size_t nval)
{
    const struct property_entry *p = lx_prop_find_dev(dev, propname);
    size_t n;

    if (!p || p->type != DEV_PROP_STRING || !val)
        return -EINVAL;
    n = p->length / sizeof(char *);
    if (nval < n)
        n = nval;
    for (size_t i = 0; i < n; i++)
        val[i] = p->is_inline ? p->value.str[i] :
                 ((const char *const *)p->pointer)[i];
    return 0;
}

int device_property_match_string(const struct device *dev, const char *propname,
                                 const char *string)
{
    (void)dev; (void)propname; (void)string;
    return -EINVAL;
}

int device_property_read_string(const struct device *dev, const char *propname,
                                const char **val)
{
    (void)dev; (void)propname; (void)val;
    return -EINVAL;
}

__attribute__((weak)) const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list)
{
    (void)list;
    return NULL;
}

#ifndef KLD_ACPI_BUILD
// no ACPI fwnode is attached to the shim's devices; the ACPICA families link
// upstream drivers/acpi/property.c, which owns these two. (satoru)
bool is_acpi_data_node(const struct fwnode_handle *fwnode)
{
    (void)fwnode;
    return false;
}

bool is_acpi_device_node(const struct fwnode_handle *fwnode)
{
    (void)fwnode;
    return false;
}
#endif

// local_bh_disable is the inline per-thread preempt_count_add; the enable half
// must subtract the same softirq offset or every local_bh_disable/enable pair
// leaks count. the container has no softirq core to run here. (satoru)
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
    (void)ip;
    preempt_count_sub(cnt);
}

// drop-reason names are used by tracing/debug only; no tracing in the pinned
// config, so registration is bookkeeping. (satoru)
void drop_reasons_register_subsys(enum skb_drop_reason_subsys subsys,
                                  const struct drop_reason_list *list)
{
    (void)subsys; (void)list;
}

void drop_reasons_unregister_subsys(enum skb_drop_reason_subsys subsys)
{
    (void)subsys;
}

// pernet: one namespace. cfg80211 registers a device ops with only .exit. (satoru)
#define LX_PERNET_MAX 8
static struct pernet_operations *g_pernet[LX_PERNET_MAX];
static int g_pernet_n;

int register_pernet_device(struct pernet_operations *ops)
{
    if (!ops)
        return -EINVAL;
    int err = ops->init ? ops->init(&init_net) : 0;
    if (err)
        return err;
    if (g_pernet_n < LX_PERNET_MAX)
        g_pernet[g_pernet_n++] = ops;
    return 0;
}

void unregister_pernet_device(struct pernet_operations *ops)
{
    for (int i = 0; i < g_pernet_n; i++) {
        if (g_pernet[i] != ops)
            continue;
        if (ops->exit)
            ops->exit(&init_net);
        for (int j = i; j + 1 < g_pernet_n; j++)
            g_pernet[j] = g_pernet[j + 1];
        g_pernet_n--;
        return;
    }
}

int register_pernet_subsys(struct pernet_operations *ops) { return register_pernet_device(ops); }
void unregister_pernet_subsys(struct pernet_operations *ops) { unregister_pernet_device(ops); }

// debugfs is absent in the container. Linux returns NULL when debugfs is
// disabled, and every debugfs_create_* caller tolerates it. (satoru)
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
    (void)name; (void)parent;
    return NULL;
}

struct dentry *debugfs_create_x32(const char *name, umode_t mode,
                                  struct dentry *parent, u32 *value)
{
    (void)name; (void)mode; (void)parent; (void)value;
    return NULL;
}

__attribute__((weak)) struct dentry *debugfs_create_file(const char *name, umode_t mode,
                                   struct dentry *parent, void *data,
                                   const struct file_operations *fops)
{
    (void)name; (void)mode; (void)parent; (void)data; (void)fops;
    return NULL;
}

struct dentry *debugfs_create_file_size(const char *name, umode_t mode,
                                        struct dentry *parent, void *data,
                                        const struct file_operations *fops,
                                        loff_t file_size)
{
    (void)name; (void)mode; (void)parent; (void)data; (void)fops;
    (void)file_size;
    return NULL;
}

void debugfs_remove(struct dentry *dentry)
{
    (void)dentry;
}

struct dentry *debugfs_rename(struct dentry *old_dir, struct dentry *old_dentry,
                              struct dentry *new_dir, const char *new_name)
{
    (void)old_dir; (void)old_dentry; (void)new_dir; (void)new_name;
    return NULL;
}

__attribute__((weak)) char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
    va_list ap;
    int n;
    char *p;

    (void)gfp;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NULL;
    p = malloc((unsigned long)n + 1);
    if (!p)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

void dump_stack(void)
{
    lx_log("[lx_emul] dump_stack called\n");
}

void dump_stack_lvl(const char *log_lvl)
{
    (void)log_lvl;
    lx_log("[lx_emul] dump_stack called\n");
}

// cpu hotplug state registration is bookkeeping in a container with no hotplug;
// radix_tree_init() calls it once at archive-cache creation. (satoru)
int __cpuhp_setup_state(enum cpuhp_state state, const char *name, bool invoke,
                        int (*startup)(unsigned int cpu),
                        int (*teardown)(unsigned int cpu), bool multi_instance)
{
    (void)state; (void)name; (void)invoke; (void)startup; (void)teardown;
    (void)multi_instance;
    return 0;
}

int __cpuhp_setup_state_cpuslocked(enum cpuhp_state state, const char *name,
                                   bool invoke, int (*startup)(unsigned int cpu),
                                   int (*teardown)(unsigned int cpu),
                                   bool multi_instance)
{
    return __cpuhp_setup_state(state, name, invoke, startup, teardown, multi_instance);
}

// ── random ──────────────────────────────────────────────────────────────────
// one source: lx_os_random (getrandom(2), the same crng as /dev/urandom). (satoru)
void get_random_bytes(void *buf, size_t len)
{
    lx_os_random(buf, (unsigned long)len);
}

u8 get_random_u8(void)
{
    u8 v = 0;
    lx_os_random(&v, sizeof(v));
    return v;
}

u16 get_random_u16(void)
{
    u16 v = 0;
    lx_os_random(&v, sizeof(v));
    return v;
}

u32 get_random_u32(void)
{
    u32 v = 0;
    lx_os_random(&v, sizeof(v));
    return v;
}

u64 get_random_u64(void)
{
    u64 v = 0;
    lx_os_random(&v, sizeof(v));
    return v;
}

u32 __get_random_u32_below(u32 ceil)
{
    if (ceil <= 1)
        return 0;
    return get_random_u32() % ceil;
}

// ── static keys ─────────────────────────────────────────────────────────────
// plain branches: the enabled counter is real, so static_branch_unlikely()
// reads a truthful value and inc/dec pairs balance. no text patching. (satoru)
bool static_key_slow_inc(struct static_key *key)
{
    return atomic_inc_not_zero(&key->enabled) ? true : (atomic_inc_return(&key->enabled) == 1);
}

bool static_key_fast_inc_not_disabled(struct static_key *key)
{
    return static_key_slow_inc(key);
}

void static_key_slow_dec(struct static_key *key)
{
    atomic_dec(&key->enabled);
}

bool static_key_slow_inc_cpuslocked(struct static_key *key)
{
    return static_key_slow_inc(key);
}

void static_key_slow_dec_cpuslocked(struct static_key *key)
{
    static_key_slow_dec(key);
}

int static_key_count(struct static_key *key)
{
    return atomic_read(&key->enabled);
}

void static_key_enable(struct static_key *key)
{
    if (atomic_read(&key->enabled) <= 0)
        atomic_set(&key->enabled, 1);
}

void static_key_disable(struct static_key *key)
{
    atomic_set(&key->enabled, 0);
}

void static_key_enable_cpuslocked(struct static_key *key)
{
    static_key_enable(key);
}

void static_key_disable_cpuslocked(struct static_key *key)
{
    static_key_disable(key);
}

u32 ethtool_op_get_link(struct net_device *dev)
{
    return netif_carrier_ok(dev) ? 1 : 0;
}

int ethtool_op_get_ts_info(struct net_device *dev, struct kernel_ethtool_ts_info *info)
{
    (void)dev;
    info->so_timestamping = 0;
    info->phc_index = -1;
    info->tx_types = 0;
    info->rx_filters = 0;
    return 0;
}

// ── diagnostics ─────────────────────────────────────────────────────────────
int net_ratelimit(void)
{
    return 1;   // never suppress: a container's log volume is not a problem
}

void __warn_printk(const char *fmt, ...)
{
    (void)fmt;
    lx_log("[lx_emul] WARN from driver (see preceding context)\n");
}

void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t)
{
    (void)r; (void)t;
    lx_log("[lx_emul] refcount saturated - a real refcount bug in the shim\n");
}

void print_hex_dump(const char *level, const char *prefix_str, int prefix_type,
                    int rowsize, int groupsize, const void *buf, size_t len,
                    bool ascii)
{
    (void)level; (void)prefix_type; (void)rowsize; (void)groupsize; (void)ascii;
    lx_log("[lx_emul] hexdump ");
    lx_log(prefix_str ? prefix_str : "");
    lx_log(" len=");
    lx_log_u((unsigned long)len);
    lx_log(" (contents omitted)\n");
    (void)buf;
}

// ── user copies ─────────────────────────────────────────────────────────────
// Reached only from the ethtool ioctl path, which nothing in the container
// drives yet. A container and its caller share no address space boundary here,
// so a straight copy is the right semantic once ioctls are wired. (satoru)
// the drm container routes these to the calling process while it serves a
// request (kld/drm/kdrm_uapi.c); every other family keeps the direct copy. (satoru)
unsigned long (*lx_uaccess_hook)(int dir, unsigned long uptr, void *kbuf, unsigned long n);

unsigned long _copy_from_user(void *to, const void __user *from, unsigned long n)
{
    if (lx_uaccess_hook)
        return lx_uaccess_hook(1, (unsigned long)from, to, n);
    memcpy(to, (const void *)from, n);
    return 0;
}

unsigned long _copy_to_user(void __user *to, const void *from, unsigned long n)
{
    if (lx_uaccess_hook)
        return lx_uaccess_hook(2, (unsigned long)to, (void *)from, n);
    memcpy((void *)to, from, n);
    return 0;
}

// ── ptp ─────────────────────────────────────────────────────────────────────
// there is no PTP clock class in the container (no /dev/ptpN, no consumer).
// refusing registration is upstream's own "PTP unavailable" path: e1000e logs
// "ptp_clock_register failed" and carries on without hardware timestamps.
// (satoru)
#include <linux/ptp_clock_kernel.h>

struct ptp_clock *ptp_clock_register(struct ptp_clock_info *info, struct device *parent)
{
    (void)info; (void)parent;
    return ERR_PTR(-EOPNOTSUPP);
}

int ptp_clock_unregister(struct ptp_clock *ptp) { (void)ptp; return 0; }
int ptp_clock_index(struct ptp_clock *ptp)      { (void)ptp; return -1; }

// ── lib/vsprintf.c number parsing ───────────────────────────────────────────
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base)
{
    unsigned long long v = 0;
    if (!base) {
        base = 10;
        if (cp[0] == '0') {
            base = 8;
            if ((cp[1] | 0x20) == 'x') { base = 16; cp += 2; }
        }
    } else if (base == 16 && cp[0] == '0' && (cp[1] | 0x20) == 'x') {
        cp += 2;
    }
    for (;; cp++) {
        unsigned int d;
        char c = *cp;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') d = (unsigned int)((c | 0x20) - 'a' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (endp) *endp = (char *)cp;
    return v;
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base)
{
    return (unsigned long)simple_strtoull(cp, endp, base);
}

// end (satoru)
