// iwlwifi family init order.
//
// The KLD build compiles each upstream module init with a per-object alias
// (kld_init_<basename>). run them in the order the kernel's initcall levels
// would, then let the driver's PCI probe run:
//   cfg80211 (net/wireless/core.c)
//   mac80211 (net/mac80211/main.c)
//   iwlwifi's iwl_drv_init
//   iwlmvm's iwl_mvm_init (registers the opmode before probe requests it)
// link order is the Makefile's DRVSRCS order; the level order is explicit here
// because the container has no initcall sections. (satoru)
//
// end (satoru)

#include <linux/kernel.h>

#include "kimpl/lx_kshim.h"

extern int kld_init_core(void);
extern int kld_init_main(void);
extern int kld_init_iwl_drv(void);
extern int kld_init_ops(void);

static int run_one(const char *name, int (*fn)(void))
{
    lx_log("[kld-iwlwifi] init ");
    lx_log(name);
    lx_log(" -> ");
    int rc = fn ? fn() : -1;
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else        { lx_log_u((unsigned long)rc); }
    lx_log("\n");
    return rc;
}

int kld_family_init(void)
{
    int rc;
    rc = run_one("cfg80211", kld_init_core);
    if (rc < 0)
        return rc;
    rc = run_one("mac80211", kld_init_main);
    if (rc < 0)
        return rc;
    // register the pci driver without probing yet: the opmode must exist
    // first (in a real kernel the pci bus probes after the initcalls). (satoru)
    extern void lx_pci_defer_probe(bool on);
    extern void lx_pci_probe_deferred(void);
    lx_pci_defer_probe(true);
    rc = run_one("iwl_drv", kld_init_iwl_drv);
    if (rc < 0)
        return rc;
    rc = run_one("iwlmvm", kld_init_ops);
    if (rc < 0)
        return rc;
    lx_pci_probe_deferred();
    return 0;
}

// after the probe: report the wiphy/bands from the shim side. (satoru)
void lx_wifi_scan_start(void);
void lx_wifi_connect_start(void);

static bool lx_arg_is(const char *want)
{
    extern int lx_main_argc;
    extern char **lx_main_argv;
    for (int i = 1; i < lx_main_argc; i++)
        if (lx_main_argv[i] && strcmp(lx_main_argv[i], want) == 0)
            return true;
    return false;
}

void kld_family_after_probe(void)
{
    extern void lx_net_wiphy_report(void);
    if (lx_arg_is("connect"))
        lx_wifi_connect_start();
    else if (lx_arg_is("scan"))
        lx_wifi_scan_start();
    else
        lx_net_wiphy_report();
}

// end (satoru)
