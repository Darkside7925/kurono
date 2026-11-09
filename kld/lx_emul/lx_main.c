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

int main(int argc, char **argv) {
    if (kld_test_main) {
        lx_dma_init();
        return kld_test_main(argc, argv);
    }
    lx_log("[kld] container starting\n");
    lx_log("[kld]   driver  = ");
    lx_log_line(KLD_DRIVER_NAME);
    lx_log("[kld]   kernel  = ");
    lx_log_line(KLD_KERNEL_VERSION);

    // Built with -DMODULE, so upstream's module_init(x) expands to an
    // init_module() alias of the driver's init function, and module_exit(x) to
    // cleanup_module(). That IS the entry point - no macro surgery needed, and
    // it is exactly what the kernel's own module loader calls. (satoru)
    if (!init_module) {
        lx_log("[kld] FAIL: no init_module symbol - the driver's module_init()\n"
               "[kld]       never produced an alias. check that -DMODULE is set\n"
               "[kld]       and the driver object is actually linked (not GC'd).\n");
        return 2;
    }

    // the page arena must exist before any driver code can call virt_to_page:
    // it defines vmemmap_base. (satoru)
    lx_dma_init();

    lx_log("[kld] calling init_module() - driver probe path begins here\n");
    int rc = init_module();
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
        lx_log("[kld] driver is live; container serving\n");
        // the leader idles in poll(): on the bsp that donates time to Kurono's
        // cooperative kernel processes instead of starving them (lx_os.c). (satoru)
        for (;;) {
            for (int i = 0; i < 5; i++)
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
