// kld container entry point.
//
// One container process per driver. It brings the shim up, runs the driver's
// module_init, and reports. In phase 1 the expected outcome is that module_init
// is REACHED and then aborts on the first LX_UNIMPL - that is the phase exit
// criterion, not a failure.
//
// Launched under Kurono like any other userspace program. Everything it says
// goes to fd 2 (com1) so a headless boot log is the whole story.
//
// end (satoru)

#include "lx_emul.h"

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

int main(void) {
    lx_log("[kld] container starting\n");
    lx_log("[kld]   driver  = ");
    lx_log_line(KLD_DRIVER_NAME);
    lx_log("[kld]   kernel  = ");
    lx_log_line(KLD_KERNEL_VERSION);

    // Built with -DMODULE, so upstream's module_init(x) expands to an
    // init_module() alias of the driver's init function, and module_exit(x) to
    // cleanup_module(). That IS the entry point - no macro surgery needed, and
    // it is exactly what the kernel's own module loader calls. Weak so a
    // container built without a driver still links and reports cleanly. (satoru)
    if (!init_module) {
        lx_log("[kld] FAIL: no init_module symbol - the driver's module_init()\n"
               "[kld]       never produced an alias. check that -DMODULE is set\n"
               "[kld]       and the driver object is actually linked (not GC'd).\n");
        return 2;
    }

    lx_log("[kld] calling init_module() - driver probe path begins here\n");
    int rc = init_module();
    lx_log("[kld] init_module returned ");
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else        { lx_log_u((unsigned long)rc); }
    lx_log("\n");

    if (cleanup_module) {
        lx_log("[kld] calling cleanup_module()\n");
        cleanup_module();
    }

    // also drive anything that registered through the shim registry (reserved
    // for future multi-module containers). (satoru)
    if (lx_initcall_count() > 0) lx_run_initcalls();

    lx_log("[kld] container done\n");
    return rc == 0 ? 0 : 1;
}

// end (satoru)
