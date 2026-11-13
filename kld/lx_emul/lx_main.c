// kld container entry point.
//
// One container process per driver. It brings the shim up, runs the driver's
// module_init (which registers the pci driver; the shim then asks the broker
// for a matching device and calls probe), brings any registered net_device up,
// and then serves the device for as long as the container lives.
//
// Launched under Kurono like any other userspace program. Everything it says
// goes to fd 2 (com1) so a headless boot log is the whole story.
//
// end (satoru)

#include "lx_emul.h"
#include <stdlib.h>   // getenv for KLD_STATS (satoru)

// kernel-context pieces, declared by hand (their headers are kernel-only). (satoru)
void  lx_dma_init(void);
void  lx_net_bringup_all(void);
void  lx_net_stats_log(void);
void *lx_pci_bound_dev(void);
void  lx_os_sleep_ns(unsigned long long ns);
void  lx_os_idle_ms(int ms);

#ifndef KLD_DRIVER_NAME
#define KLD_DRIVER_NAME "unknown"
#endif
#ifndef KLD_KERNEL_VERSION
#define KLD_KERNEL_VERSION "unknown"
#endif

// provided by the driver via upstream's module_init/module_exit expansion under
// -DMODULE. weak so the container links even with no driver attached. (satoru)
extern int  init_module(void)    __attribute__((weak));
extern void cleanup_module(void) __attribute__((weak));
// a test family (kld/tests) provides this and runs instead of the probe path. (satoru)
extern int  kld_test_main(int argc, char **argv) __attribute__((weak));

int lx_main_argc;
char **lx_main_argv;

static int lx_arg_is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    lx_main_argc = argc;
    lx_main_argv = argv;
    // KLD_STATS=1 enables the per-shim-thread wait/yield dump (N1). (satoru)
    if (getenv("KLD_STATS")) {
        extern void lx_stats_start(void);
        lx_stats_start();
    }
    // --kld-ids / --kld-fw: run the family's initcalls with the pci bus
    // disabled and print the driver's own id table (host-side build step; no
    // broker call is made). --kld-fw also prints the per-family cfg fields.
    // (satoru)
    if (argc == 2 && (lx_arg_is(argv[1], "--kld-ids") ||
                      lx_arg_is(argv[1], "--kld-fw"))) {
        extern int lx_ids_run(int with_extra);
        return lx_ids_run(argv[1][6] == 'f');
    }
    // the drm userspace test program started itself as a child process (kld/tests/dri_app.c) (satoru)
    extern int kdri_child_main(int argc, char **argv) __attribute__((weak));
    if (kdri_child_main && argc >= 3 && lx_arg_is(argv[1], "--dri-child"))
        return kdri_child_main(argc, argv);
    if (kld_test_main) {
        extern void lx_corelib_init(void);
        lx_dma_init();
        lx_corelib_init();
        return kld_test_main(argc, argv);
    }
    lx_log("[kld] container starting\n");
    // supervisor heartbeat thread, harmless without kurono.kld.sup (satoru)
    extern void lx_os_heartbeat_start(void);
    lx_os_heartbeat_start();
    lx_log("[kld]   driver  = ");
    lx_log_line(KLD_DRIVER_NAME);
    lx_log("[kld]   kernel  = ");
    lx_log_line(KLD_KERNEL_VERSION);

    // Built with -DMODULE, so upstream's module_init(x) expands to an
    // init_module() alias of the driver's init function, and module_exit(x) to
    // cleanup_module(). That IS the entry point - no macro surgery needed, and
    // it is exactly what the kernel's own module loader calls. (satoru)
    // a multi-module family (iwlwifi) supplies kld_family_init() and the
    // per-object aliases; the single-module families keep using init_module.
    // (satoru)
    extern int kld_family_init(void) __attribute__((weak));
    if (!kld_family_init && !init_module) {
        lx_log("[kld] FAIL: no init_module symbol - the driver's module_init()\n"
               "[kld]       never produced an alias. check that -DMODULE is set\n"
               "[kld]       and the driver object is actually linked (not GC'd).\n");
        return 2;
    }

    // kurono.kld.iommutest=1 starts the container with this argument: arm the
    // negative iommu test before the driver maps its rx buffers. (satoru)
    int test_mode = 0;
    for (int i = 1; i < argc; i++)
        if (lx_arg_is(argv[i], "iommutest")) test_mode = 1;
    if (test_mode)
        lx_dma_iommu_test_arm();

    // the page arena must exist before any driver code can call virt_to_page:
    // it defines vmemmap_base. (satoru)
    lx_dma_init();
    // radix_tree_init() is an init/main.c call in a real kernel; xarray/idr
    // users (mac80211, iwlwifi, the apitest family) need its node cache. (satoru)
    extern void lx_corelib_init(void);
    lx_corelib_init();

    lx_log("[kld] calling init_module() - driver probe path begins here\n");
    int rc = kld_family_init ? kld_family_init() : init_module();
    lx_log("[kld] init_module returned ");
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else        { lx_log_u((unsigned long)rc); }
    lx_log("\n");
    if (rc != 0)
        return 1;

    // a probed driver is now a live device: bring its interfaces up (what
    // `ip link set up` does on linux) and serve it. the container only exits
    // by crashing or by an LX_UNIMPL trap, and the kernel runner then releases
    // every broker resource it held. (satoru)
    if (lx_pci_bound_dev()) {
        lx_net_bringup_all();
        extern void kld_family_after_probe(void) __attribute__((weak));
        if (kld_family_after_probe)
            kld_family_after_probe();
        lx_log("[kld] driver is live; container serving\n");
        if (test_mode) {
            // let dhcp drive a frame into the canary, then exit so the runner
            // releases the domain and the teardown check runs. (satoru)
            lx_os_sleep_ns(15000000000ull);
            lx_log("[kld] iommutest done, exiting for the domain teardown\n");
            return 0;
        }
        // the leader idles in poll(): on the bsp that donates time to Kurono's
        // cooperative kernel processes instead of starving them (lx_os.c). (satoru)
        extern int lx_os_hang_requested(void);
        for (;;) {
            for (int i = 0; i < 5; i++)
                lx_os_idle_ms(1000);
            extern int lx_pci_was_released(void);
            if (lx_pci_was_released()) {
                lx_log("[kld] driver released without a live device; exiting\n");
                return 1;
            }
            // injected exit0: a clean return; injected hang: block the
            // serving loop for good (satoru)
            if (lx_os_hang_requested() == 2)
                return 0;
            while (lx_os_hang_requested())
                lx_os_idle_ms(1000);
            lx_net_stats_log();
        }
    }

    if (cleanup_module) {
        lx_log("[kld] no device bound; calling cleanup_module()\n");
        cleanup_module();
    }

    // also drive anything that registered through the shim registry (reserved
    // for future multi-module containers). (satoru)
    if (lx_initcall_count() > 0) lx_run_initcalls();

    lx_log("[kld] container done\n");
    return 0;
}

// end (satoru)
