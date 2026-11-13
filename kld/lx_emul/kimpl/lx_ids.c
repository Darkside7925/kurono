// lx_emul: --kld-ids, the driver's own pci id table on stdout.
//
// Run by tools/gen_match_table.sh and tools/list_firmware.sh on the HOST, as a
// normal Linux process. The family's initcalls run exactly as in a container,
// except the pci bus is disabled: __pci_register_driver records the driver but
// never probes, and lx_pci_probe_deferred() is a no-op, so no SYS_KLD_CALL is
// ever issued (the broker does not exist on the host). Then every registered
// pci_driver's id_table is printed, one line per entry. This is the evidence
// the kernel matches against: the ids come from the DRIVER, not from a list
// kept next to the kernel.
//
// output per line (lowercase hex, PCI_ANY_ID as ffffffff):
//   <driver name> <vendor> <device> <subvendor> <subdevice> <class> <class_mask>
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/mod_devicetable.h>

#include "drivers/net/wireless/intel/iwlwifi/iwl-config.h"

#include "lx_kshim.h"

void lx_dma_init(void);
void lx_corelib_init(void);
void lx_pci_set_bus_disabled(bool on);
struct pci_driver *lx_pci_driver_at(int idx);
int lx_pci_driver_count(void);
extern unsigned long lx_broker_call_count;

extern int  kld_family_init(void) __attribute__((weak));
extern int  init_module(void)     __attribute__((weak));

// kernel context cannot include libc, and the ids are a machine-read table, so
// a raw host write(2) syscall is the whole output path. (satoru)
static long host_write(int fd, const char *p, unsigned long n)
{
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(1L), "D"((long)fd), "S"(p), "d"(n),
                           "r"(r10), "r"(r8)
                         : "rcx", "r11", "memory");
    return ret;
}

static char  lx_ids_buf[256];
static unsigned lx_ids_len;

static void ids_flush(void)
{
    if (lx_ids_len)
        host_write(1, lx_ids_buf, lx_ids_len);
    lx_ids_len = 0;
}

static void ids_ch(char c)
{
    if (lx_ids_len >= sizeof(lx_ids_buf) - 1)
        ids_flush();
    lx_ids_buf[lx_ids_len++] = c;
}

static void ids_str(const char *s)
{
    if (!s)
        s = "?";
    while (*s)
        ids_ch(*s++);
}

static void ids_hex(unsigned int v)
{
    static const char d[] = "0123456789abcdef";
    char t[8];
    int k = 0;
    if (v == 0) {
        ids_ch('0');
        return;
    }
    while (v) {
        t[k++] = d[v & 15u];
        v >>= 4;
    }
    while (k)
        ids_ch(t[--k]);
}

static void ids_id(const struct pci_device_id *id)
{
    ids_hex(id->vendor);      ids_ch(' ');
    ids_hex(id->device);      ids_ch(' ');
    ids_hex(id->subvendor);   ids_ch(' ');
    ids_hex(id->subdevice);   ids_ch(' ');
    ids_hex(id->class);       ids_ch(' ');
    ids_hex(id->class_mask);
}

static void ids_u(unsigned long v)
{
    char t[24];
    int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) ids_ch(t[--k]);
}

static int ids_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// --kld-fw: iwlwifi's id entries carry a struct iwl_cfg pointer in
// driver_data, or a trans cfg pointer with bit 0 set (TRANS_CFG_MARKER). for
// the latter the probe resolves the full cfg through iwl_dev_info_table using
// the card's mac/rf/step registers; a host run cannot know those, so print
// every table candidate for that pci id. list_firmware.sh turns the fields
// into file names. (satoru)
// only the iwlwifi container has pcie/drv.c; other families link these as
// null/zero and never call this path. (satoru)
extern const struct iwl_dev_info iwl_dev_info_table[] __attribute__((weak));
extern const unsigned int iwl_dev_info_table_size __attribute__((weak));

static void ids_cfg_fields(const struct iwl_cfg *cfg, const char *key)
{
    ids_str(key); ids_str("pre=");
    ids_str(cfg->fw_name_pre ? cfg->fw_name_pre : "-");
    ids_str(" "); ids_str(key); ids_str("mac=");
    ids_str(cfg->fw_name_mac ? cfg->fw_name_mac : "-");
    ids_str(" "); ids_str(key); ids_str("min=");
    ids_u(cfg->ucode_api_min);
    ids_str(" "); ids_str(key); ids_str("max=");
    ids_u(cfg->ucode_api_max);
}

static void ids_iwl_extra(const struct pci_device_id *id, const char *driver)
{
    if (!driver || !ids_eq(driver, "iwlwifi"))
        return;
    ids_ch(' ');

    kernel_ulong_t data = id->driver_data;
    if (data & 1u) {
        const struct iwl_cfg_trans_params *tc =
            (const struct iwl_cfg_trans_params *)(data & ~1UL);
        ids_str("trans=1 fam=");
        ids_u((unsigned long)tc->device_family);
        if (!iwl_dev_info_table || !iwl_dev_info_table_size) {
            ids_str(" cands=0");
            return;
        }
        int n = 0;
        const struct iwl_cfg *seen[16];
        int nseen = 0;
        for (unsigned i = 0; i < iwl_dev_info_table_size; i++) {
            const struct iwl_dev_info *di = &iwl_dev_info_table[i];
            if (di->device != 0xFFFF && di->device != (u16)id->device)
                continue;
            if (di->subdevice != 0xFFFF && id->subdevice != 0xFFFF &&
                di->subdevice != (u16)id->subdevice)
                continue;
            if (!di->cfg)
                continue;
            // the trans cfg in this id entry fixes the device family; the
            // probe still has to read mac/rf steps, so every full cfg of the
            // same family is a possible answer. (satoru)
            if (di->cfg->trans.device_family != tc->device_family)
                continue;
            bool dup = false;
            for (int k = 0; k < nseen; k++)
                if (seen[k] == di->cfg)
                    dup = true;
            if (dup)
                continue;
            if (nseen < 16)
                seen[nseen++] = di->cfg;
            ids_str(" c");
            ids_u((unsigned long)n);
            ids_str(".");
            ids_str("pre=");
            ids_str(di->cfg->fw_name_pre ? di->cfg->fw_name_pre : "-");
            ids_str(" c");
            ids_u((unsigned long)n);
            ids_str(".mac=");
            ids_str(di->cfg->fw_name_mac ? di->cfg->fw_name_mac : "-");
            ids_str(" c");
            ids_u((unsigned long)n);
            ids_str(".min=");
            ids_u(di->cfg->ucode_api_min);
            ids_str(" c");
            ids_u((unsigned long)n);
            ids_str(".max=");
            ids_u(di->cfg->ucode_api_max);
            n++;
        }
        ids_str(" cands=");
        ids_u((unsigned long)n);
        return;
    }

    const struct iwl_cfg *cfg = (const struct iwl_cfg *)data;
    if (!cfg) {
        ids_str("cfg_none=1");
        return;
    }
    ids_cfg_fields(cfg, "");
}

int lx_ids_run(int with_extra)
{
    int i, entries = 0;

    // pci bus disabled: registration only, no probe, no broker call. (satoru)
    lx_pci_set_bus_disabled(true);

    // same prerequisites the normal container start-up provides. (satoru)
    lx_dma_init();
    lx_corelib_init();

    int rc = kld_family_init ? kld_family_init()
                             : (init_module ? init_module() : -1);
    if (rc < 0) {
        lx_log("[kld-ids] family init failed\n");
        return 1;
    }

    for (i = 0; i < lx_pci_driver_count(); i++) {
        struct pci_driver *drv = lx_pci_driver_at(i);
        const struct pci_device_id *id;

        if (!drv || !drv->id_table)
            continue;
        // the linux terminator test (drivers/pci/pci-driver.c pci_match_id),
        // not merely vendor==0: a class-only entry is still a real entry.
        // (satoru)
        for (id = drv->id_table;
             id->vendor || id->subvendor || id->class_mask; id++) {
            ids_str(drv->name ? drv->name : "?");
            ids_ch(' ');
            ids_id(id);
            if (with_extra) {
                ids_flush();
                ids_iwl_extra(id, drv->name);
            }
            ids_ch('\n');
            entries++;
        }
    }
    ids_flush();

    lx_log("[kld-ids] drivers=");
    lx_log_u((unsigned long)lx_pci_driver_count());
    lx_log(" entries=");
    lx_log_u((unsigned long)entries);
    lx_log(" broker calls=");
    lx_log_u(lx_broker_call_count);
    lx_log("\n");
    return lx_broker_call_count ? 1 : 0;
}

// end (satoru)
