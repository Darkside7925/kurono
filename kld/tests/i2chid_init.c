// i2chid: the laptop touchpad family's ordered module entry points.
//
// This is the container equivalent of init/main.c calling each module_init in
// link order. Built-in cores (i2c-core, hid-core, pinctrl, gpiolib, input) are
// driven by lx_corelib_init() from their .initcall sections; this file starts
// the modular drivers that own the probe path. (satoru)

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/acpi.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_acpi.h"
#include "kimpl/lx_broker.h"

extern int kld_init_i2c_core_base(void);
extern int kld_init_pinctrl_meteorpoint(void);
extern int kld_init_intel_lpss(void);
extern int kld_init_intel_lpss_pci(void);
extern int kld_init_i2c_designware_platdrv(void);
extern int kld_init_i2c_hid_acpi(void);
extern int kld_init_hid_multitouch(void);

static int step(const char *name, int (*fn)(void))
{
    int rc = fn ? fn() : -ENODEV;
    lx_log("[i2chid] init ");
    lx_log(name);
    lx_log(" rc=");
    lx_log_u((unsigned long)(rc < 0 ? -rc : rc));
    lx_log("\n");
    return rc;
}

// ── C5.4b raw-op negatives ──────────────────────────────────────────────────
// the family is acpi-enabled, so these calls reach the broker with the strict
// policy. the map attempts must be refused by the kernel; the port/pci writes
// are refused at the hardware and shadowed, and the kernel's release counters
// must show real writes = 0. (satoru)
extern int lx_main_argc;
extern char **lx_main_argv;

// the host-side --kld-ids/--kld-fw modes run with no broker and must not call
// it even once (gen_match_table checks broker calls=0). (satoru)
static int host_ids_mode(void)
{
    if (lx_main_argc != 2 || !lx_main_argv || !lx_main_argv[1])
        return 0;
    const char *a = lx_main_argv[1];
    return a[0] == '-' && a[1] == '-' && a[2] == 'k' && a[3] == 'l' &&
           a[4] == 'd' && a[5] == '-';
}

static int g_c54_refused, g_c54_total;

static void c54_case(const char *name, int refused, const char *why)
{
    g_c54_total++;
    if (refused) g_c54_refused++;
    lx_log("[kld-c54] ");
    lx_log(name);
    lx_log(refused ? ": PASS (refused)" : ": FAIL (not refused)");
    if (why) { lx_log(" "); lx_log(why); }
    lx_log("\n");
}

static acpi_status c54_count_cb(acpi_handle h, u32 level, void *ctx, void **rv)
{
    (void)h; (void)level; (void)rv;
    (*(int *)ctx)++;
    return AE_OK;
}

static void c54_negatives(void)
{
    void *va = lx_os_reserve(4096);
    if (!va) {
        lx_log("[kld-c54] no reserved page for the map negatives\n");
        return;
    }
    unsigned long p = (unsigned long)va;
    long r;

    r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, 0x1000000ul, 0x1000, p, 0);
    c54_case("map_usable_ram", r == -1, 0);

    r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, 0xFEE00000ul, 0x1000, p, 0);
    c54_case("map_lapic", r == -1, 0);

    const struct acpi_table_header *h = NULL;
    acpi_status mst = acpi_get_table("MCFG", 0, &h);
    if (ACPI_SUCCESS(mst) && h && h->length >= 60) {
        u64 ecam = *(const u64 *)((const u8 *)h + 44);
        lx_log("[kld-c54] mcfg ecam ");
        lx_log_hex((unsigned long)ecam);
        lx_log("\n");
        r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, ecam, 0x1000, p, 0);
        c54_case("map_ecam", r == -1, 0);
        acpi_put_table((struct acpi_table_header *)h);
    } else {
        lx_log("[kld-c54] mcfg table unavailable (status ");
        lx_log_u((unsigned long)mst);
        lx_log(")\n");
    }

    // a real pci bar: read config through the allowed acpi read op, then try to
    // map the bar address as platform/opregion memory. (satoru)
    for (unsigned int dev = 2; dev <= 7; dev++) {
        long bar = lx_broker_call(LX_KLD_OP_ACPI_PCI_READ,
                                  (0u << 16) | (dev << 8) | 0u, 0x10, 4, 0);
        if (bar <= 0 || (uint32_t)bar == 0xFFFFFFFFu || (bar & 1))
            continue;
        if (!((unsigned long)bar & ~0xFFFull))
            continue;
        lx_log("[kld-c54] pci bar 0:2.");
        lx_log_u(dev);
        lx_log(" = ");
        lx_log_hex((unsigned long)bar);
        lx_log("\n");
        r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS,
                           (unsigned long)bar & ~0xFFFull, 0x1000, p, 0);
        c54_case("map_pci_bar", r == -1, 0);
        break;
    }

    // writes: refused at the hardware, visible to the read-back. (satoru)
    lx_broker_call(LX_KLD_OP_ACPI_PORT_OUT, 0x70, 8, 0xA5, 0);
    long pv = lx_broker_call(LX_KLD_OP_ACPI_PORT_IN, 0x70, 8, 0, 0);
    c54_case("port_write_shadowed", (pv & 0xFF) == 0xA5, 0);

    lx_broker_call(LX_KLD_OP_ACPI_PCI_WRITE, 0x0200u, 0x10, 4, 0x11223344u);
    long cv = lx_broker_call(LX_KLD_OP_ACPI_PCI_READ, 0x0200u, 0x10, 4, 0);
    c54_case("pci_write_shadowed", (cv & 0xFFFFFFFF) == 0x11223344, 0);

    // item 11: a strict family's ACPI opregion map is read-only at the page
    // tables, and mprotect must not promote it (the kernel refuses broker
    // mappings). map a firmware-reserved page read-only and try.
    r = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, 0x7FFE0000ul, 0x1000, p, 0);
    if (r == 0) {
        int denied = (mprotect(va, 4096, 2 /* PROT_READ|PROT_WRITE */) != 0);
        c54_case("ro_map_mprotect", denied, denied ? "refused" : "promoted");
    }

    lx_log("[kld-c54] raw-op negatives: ");
    lx_log_u((unsigned long)g_c54_refused);
    lx_log("/");
    lx_log_u((unsigned long)g_c54_total);
    lx_log(" refused or shadowed\n");
}

int kld_family_init(void)
{
    int rc;

    // the real family has no table blob and no hardware models: the namespace
    // comes from the kernel's ACPI provider and the devices from real firmware.
    // the host-side --kld-ids mode has no broker and only registers drivers, so
    // it skips the namespace. (satoru)
    // only the REAL family owns the namespace here. the simulated family's
    // test main has already installed the blob provider before calling this;
    // leave it alone (it has no acpi manifest key, so the broker would refuse).
    // (satoru)
    int we_own_acpi = 0;
    if (host_ids_mode()) {
        lx_log("[i2chid] host mode: acpi provider skipped\n");
    } else if (!lx_acpi_provider_rsdp()) {
    we_own_acpi = 1;
    if ((rc = lx_acpi_set_provider(LX_ACPI_PROVIDER_BROKER))) {
        lx_log("[i2chid] acpi broker provider rc=");
        lx_log_u((unsigned long)(rc < 0 ? -rc : rc));
        lx_log("\n");
        return rc;
    }
    if ((rc = lx_acpi_init(NULL, 0))) {
        lx_log("[i2chid] acpi namespace bring-up rc=");
        lx_log_u((unsigned long)(rc < 0 ? -rc : rc));
        lx_log("\n");
        return rc;
    }
    }
    if (we_own_acpi) {
    int tp = 0;
    acpi_get_devices("PNP0C50", c54_count_cb, &tp, NULL);
    lx_log("[kld-tp-acpi] PNP0C50 devices: ");
    lx_log_u((unsigned long)tp);
    lx_log(tp ? " (namespace readable)\n" : " (none on this machine; namespace readable)\n");

    c54_negatives();
    // C5.8 hostile-store gate: map a firmware-reserved page read-only and
    // write it. the kernel must kill this container (broker map, not the
    // container's to promote), never make the page writable.
    for (int ai = 1; ai < lx_main_argc; ai++)
        if (lx_main_argv[ai] && strcmp(lx_main_argv[ai], "--hostile") == 0) {
            void *hv = lx_os_reserve(4096);
            unsigned long hp = (unsigned long)hv;
            long hr = lx_broker_call(LX_KLD_OP_ACPI_MAP_PHYS, 0x7FFE0000ul, 0x1000, hp, 0);
            lx_log("[kld-c58] hostile map rc=");
            lx_log_u((unsigned long)(hr < 0 ? -hr : hr));
            lx_log(", storing now (must die)\n");
            *(volatile unsigned char *)(uintptr_t)hp = 0x5A;
            lx_log("[kld-c58] FAIL: the store survived\n");
            return -1;
        }
    }
    if ((rc = step("i2c-core-base", kld_init_i2c_core_base)))
        return rc;
    if ((rc = step("pinctrl-meteorpoint", kld_init_pinctrl_meteorpoint)))
        return rc;
    if ((rc = step("intel-lpss", kld_init_intel_lpss)))
        return rc;
    if ((rc = step("intel-lpss-pci", kld_init_intel_lpss_pci)))
        return rc;
    if ((rc = step("i2c-designware-platdrv", kld_init_i2c_designware_platdrv)))
        return rc;
    if ((rc = step("i2c-hid-acpi", kld_init_i2c_hid_acpi)))
        return rc;
    if ((rc = step("hid-multitouch", kld_init_hid_multitouch)))
        return rc;
    return 0;
}

// end (satoru)
