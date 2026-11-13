// lx_emul: one-time initialisation of the imported core library.
//
// lib/radix-tree.c creates its node cache in radix_tree_init(), which the real
// kernel calls from init/main.c. the container has no init/main.c, so call it
// once before any xarray/idr user runs. (satoru)
//
// end (satoru)

#include <linux/kernel.h>

#include "lx_kshim.h"

extern void radix_tree_init(void) __attribute__((weak));
extern initcall_entry_t __initcall0_end[];
extern initcall_entry_t __initcall1_end[];
extern initcall_entry_t __initcall2_end[];
extern initcall_entry_t __initcall3_end[];
extern initcall_entry_t __initcall4_end[];
extern initcall_entry_t __initcall5_end[];
extern initcall_entry_t __initcall6_end[];
extern initcall_entry_t __initcall7_end[];
extern initcall_entry_t __initcall7s_start[];
extern initcall_entry_t __initcall7s_end[];

static void lx_run_one(const char *level, initcall_entry_t *b, initcall_entry_t *e)
{
    if (!b || !e)
        return;
    (void)level;
    for (initcall_entry_t *p = b; p < e; p++) {
        initcall_t fn = initcall_from_entry(p);
        if (fn)
            (void)fn();
    }
}

// lx_work.c: starts the timer thread, which also keeps jiffies current. (satoru)
void lx_timers_start(void);

// the drm families never ran the built-in initcalls (dma-buf's needs a real
// kern_mount); they keep that behaviour, the others run every level. (satoru)
void lx_corelib_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    lx_timers_start();
    if (radix_tree_init)
        radix_tree_init();
#ifndef KLD_DRM_OWNS_MODULE_OPS
    lx_run_one("0", __initcall0_start, __initcall0_end);
    lx_run_one("1", __initcall1_start, __initcall1_end);
    lx_run_one("2", __initcall2_start, __initcall2_end);
    lx_run_one("3", __initcall3_start, __initcall3_end);
    lx_run_one("4", __initcall4_start, __initcall4_end);
    lx_run_one("5", __initcall5_start, __initcall5_end);
    lx_run_one("6", __initcall6_start, __initcall6_end);
    lx_run_one("7", __initcall7_start, __initcall7_end);
    lx_run_one("7s", __initcall7s_start, __initcall7s_end);
#endif
}

// end (satoru)
