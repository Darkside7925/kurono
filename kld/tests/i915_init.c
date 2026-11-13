// i915 family init order (d4, display only): the drm core, the display helper
// module, then the upstream i915 module (i915_init registers the pci driver;
// its probe runs i915_driver_probe). after the probe the translator client
// takes the device exactly as for the emulated families. (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/err.h>
#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_client.h>
#include <linux/iosys-map.h>
#include "gem/i915_gem_object.h"
#include "gem/i915_gem_object_types.h"
#include "i915_drv.h"
#include "i915_scatterlist.h"
#include "intel_uncore.h"
#include "display/intel_display_types.h"
#include "display/intel_pipe_crc_regs.h"

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_broker.h"
#include "drm/kdrm_client.h"
#include "drm/i915_model.h"

extern int kld_init_drm_drv(void);
extern int kld_init_drm_display_helper_mod(void) __attribute__((weak));
extern int kld_init_i915_module(void);
extern int kld_init_i2c_core_base(void) __attribute__((weak));
extern struct pci_dev *lx_pci_bound_dev(void);
extern char **lx_main_argv;
extern int lx_main_argc;

static const char *arg_value(const char *prefix)
{
    for (int i = 1; i < lx_main_argc; i++) {
        const char *a = lx_main_argv[i];
        int k = 0;
        if (!a)
            continue;
        while (prefix[k] && a[k] == prefix[k])
            k++;
        if (!prefix[k])
            return a + k;
    }
    return NULL;
}

// "bartest" (rig only, with kurono.kld.bar=ro|window): map bar0 of the
// granted device straight through the broker and write one register inside
// the manifest's display window (0x45400, a power well request register of
// the model) and then one outside it (0x9400, a gt register). with window the
// first write lands and the second faults the container; with ro the first
// one faults already. the driver is never loaded. (satoru)
static void bar_policy_test(void)
{
    long bdf = lx_broker_pci_find(0x8086, 0x7d67, 0);
    unsigned long len = 16ul << 20;
    void *va;
    long rc;
    if (bdf < 0) {
        lx_log("[kld-i915] bartest: no device\n");
        return;
    }
    // review f2: asls (0xFC) and swsci (0xE8) are not writable. (satoru)
    {
        long w1 = lx_broker_cfg_write((unsigned int)bdf, 0xFC, 4, 0x12345000u);
        long w2 = lx_broker_cfg_write((unsigned int)bdf, 0xE8, 4, 0x1u);
        lx_log(w1 < 0 && w2 < 0 ? "[kld-i915] bartest: asls/swsci config writes refused PASS\n"
                                : "[kld-i915] bartest: asls/swsci config write FAIL (accepted)\n");
    }
    // review f4: bar 1 is the upper dword of the 64-bit bar 0, never a bar
    // of its own. (satoru)
    {
        void *up = lx_os_reserve(1ul << 20);
        long r1 = up ? lx_broker_call(LX_KLD_OP_MAP_BAR, (unsigned long)bdf, 1, (unsigned long)up, 1ul << 20) : 0;
        lx_log(r1 < 0 ? "[kld-i915] bartest: upper-half bar map refused PASS\n"
                      : "[kld-i915] bartest: upper-half bar map FAIL (mapped)\n");
    }
    va = lx_os_reserve(len);
    rc = va ? lx_broker_call(LX_KLD_OP_MAP_BAR, (unsigned long)bdf, 0, (unsigned long)va, len) : -1;
    if (rc < 0) {
        lx_log("[kld-i915] bartest: MAP_BAR refused\n");
        return;
    }
    // review g1: the kernel must not write a read-only page for the
    // container either. read() of the container's own image into the page:
    // outside the window (0x9000) it must fail and leave the page as it was;
    // inside (0x45400) it lands with window and fails with ro. (satoru)
    {
        extern long lx_os_read_into(const char *path, void *dst, unsigned long n);
        const char *self = lx_main_argv[0];
        u32 before = *(volatile u32 *)((char *)va + 0x9000);
        long r1 = lx_os_read_into(self, (char *)va + 0x9000, 4);
        u32 after = *(volatile u32 *)((char *)va + 0x9000);
        long r2 = lx_os_read_into(self, (char *)va + 0x45400, 4);
        char line[200];
        snprintf(line, sizeof(line),
                 "[kld-i915] bartest: read() into out-of-window page rc=%ld %s, into 0x45400 rc=%ld\n",
                 r1, (r1 < 0 && before == after) ? "refused, page unchanged PASS" : "FAIL", r2);
        lx_log(line);
    }
    // turn 6: ioctl output, recvmsg and readlink into the read-only page 0x9000
    // (outside the window in both modes): each must fail with -EFAULT (-14),
    // leave the page unchanged and not panic (kurono.wp=1). (satoru)
    {
        extern long lx_os_syscall_into(int kind, void *dst, unsigned long n);
        static const char *what[4] = { "", "ioctl(FIONREAD)", "recvmsg", "readlink" };
        for (int k = 1; k <= 3; k++) {
            u32 b0 = *(volatile u32 *)((char *)va + 0x9000);
            long r = lx_os_syscall_into(k, (char *)va + 0x9000, 64);
            u32 b1 = *(volatile u32 *)((char *)va + 0x9000);
            char line[160];
            snprintf(line, sizeof(line), "[kld-i915] bartest: %s into a read-only bar page rc=%ld %s\n",
                     what[k], r, (r == -14 && b0 == b1) ? "refused, page unchanged PASS" : "FAIL");
            lx_log(line);
        }
    }
    lx_log("[kld-i915] bartest: write inside the window (0x45400)\n");
    *(volatile u32 *)((char *)va + 0x45400) = 0;
    lx_log("[kld-i915] bartest: inside write landed\n");
    lx_log("[kld-i915] bartest: write outside the window (0x9400)\n");
    *(volatile u32 *)((char *)va + 0x9400) = 0;
    lx_log("[kld-i915] bartest: FAIL outside write landed\n");
}

int kld_family_init(void)
{
    int rc;
    const char *model = arg_value("model:");
    if (arg_value("bartest")) {
        bar_policy_test();
        return -EPERM;
    }
    if (model)
        i915_model_enable(model);
    // the i2c core's own init (bus, dummy driver): gmbus and dp aux register
    // i2c adapters. (satoru)
    if (kld_init_i2c_core_base) {
        rc = kld_init_i2c_core_base();
        lx_log(rc ? "[kld-i915] i2c core init failed\n" : "[kld-i915] i2c core up\n");
    }
    lx_log("[kld-i915] init drm core\n");
    rc = kld_init_drm_drv();
    if (rc) {
        lx_log("[kld-i915] drm core init rc=-");
        lx_log_u((unsigned long)-rc);
        lx_log("\n");
        return rc;
    }
    if (kld_init_drm_display_helper_mod) {
        lx_log("[kld-i915] init drm display helper\n");
        rc = kld_init_drm_display_helper_mod();
        if (rc)
            return rc;
    }
    lx_log("[kld-i915] init i915\n");
    rc = kld_init_i915_module();
    i915_model_summary("after i915 init");
    if (rc) {
        lx_log("[kld-i915] i915 init rc=-");
        lx_log_u((unsigned long)-rc);
        lx_log("\n");
    }
    return rc;
}

// the translator's buffer is a shmem gem object; i915 has no generic gem
// vmap, so the family pins a write-back cpu map of it (i915 flushes display
// objects itself). (satoru)
static int g_failsetup_done;
int kld_family_vmap(struct drm_framebuffer *fb, struct iosys_map *map)
{
    struct drm_i915_gem_object *obj = to_intel_bo(fb->obj[0]);
    if (arg_value("failsetup") && !g_failsetup_done) {
        g_failsetup_done = 1;
        lx_log("[kld-i915] failsetup: first translator vmap fails once\n");
        return -EIO;
    }
    void *va = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WB);
    if (IS_ERR(va))
        return PTR_ERR(va);
    iosys_map_set_vaddr(map, va);
    lx_log("[kld-i915] translator buffer mapped (pin_map wb)\n");
    return 0;
}

void kld_family_vunmap(struct drm_framebuffer *fb)
{
    i915_gem_object_unpin_map(to_intel_bo(fb->obj[0]));
}

// ── model run helpers (argv model:<variant>; inert on hardware) ──────────
extern void i915_model_irq_start(unsigned int irq);
extern void i915_model_set_scanout(const void *va, const u64 *dma, unsigned long pages, u32 pitch);
static struct drm_i915_private *g_i915;
static volatile int g_pattern_done;

// the translator's buffer on the crtc: the model may hash it for the pipe
// crc, with the dma address of each page so it can check the ggtt. (satoru)
void kld_family_after_commit(struct drm_framebuffer *fb, const void *vaddr, bool is_iomem)
{
    struct drm_i915_gem_object *obj = to_intel_bo(fb->obj[0]);
    unsigned long n = obj->base.size >> PAGE_SHIFT, i = 0;
    struct sgt_iter iter;
    dma_addr_t addr;
    u64 *dma;
    (void)is_iomem;
    if (!i915_model_on() || !obj->mm.pages)
        return;
    dma = kcalloc(n, sizeof(*dma), GFP_KERNEL);
    if (!dma)
        return;
    for_each_sgt_daddr(addr, iter, obj->mm.pages) {
        if (i >= n)
            break;
        dma[i++] = addr;
    }
    i915_model_set_scanout(vaddr, dma, i, fb->pitches[0]);
}

static u32 mmio_r(u32 off) { return intel_uncore_read(&g_i915->uncore, _MMIO(off)); }

static void wait_frames(int pipe, u32 n)
{
    u32 f0 = mmio_r(0x70040 + 0x1000 * pipe);                    // PIPE_FRMCOUNT
    for (int t = 0; t < 400 && mmio_r(0x70040 + 0x1000 * pipe) - f0 < n; t++)
        lx_os_sleep_ns(5000000ull);
}

// pipe crc of plane 1 read straight from PIPE_CRC_RES_1 (no debugfs): stable
// while the pattern is static, different after the moving block ran. the
// same steps read the hardware crc in d4.6. (satoru)
static int crc_test(void *arg)
{
    struct drm_crtc *crtc, *on = NULL;
    u32 a, b, c, d;
    int pipe;
    (void)arg;
    lx_os_sleep_ns(3000000000ull);
    drm_for_each_crtc(crtc, &g_i915->drm)
        if (crtc->state && crtc->state->active) { on = crtc; break; }
    if (!on) {
        lx_log("[kld-i915] crc test: no active crtc\n");
        return 0;
    }
    pipe = to_intel_crtc(on)->pipe;
    intel_uncore_write(&g_i915->uncore, _MMIO(0x60050 + 0x1000 * pipe),     // PIPE_CRC_CTL
                       PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PLANE_1_SKL);
    wait_frames(pipe, 2);
    a = mmio_r(0x60064 + 0x1000 * pipe);                         // PIPE_CRC_RES_1
    wait_frames(pipe, 2);
    b = mmio_r(0x60064 + 0x1000 * pipe);
    while (!g_pattern_done)
        lx_os_sleep_ns(100000000ull);
    wait_frames(pipe, 2);
    c = mmio_r(0x60064 + 0x1000 * pipe);
    wait_frames(pipe, 2);
    d = mmio_r(0x60064 + 0x1000 * pipe);
    {
        char line[160];
        int ok = a == b && c == d && a != c && a != 0xbad0bad0u && c != 0xbad0bad0u;
        snprintf(line, sizeof(line),
                 "[kld-i915] crc test pipe %c: static %08x %08x, moved %08x %08x -> %s\n",
                 'A' + pipe, a, b, c, d, ok ? "PASS" : "FAIL");
        lx_log(line);
    }
    return 0;
}

// argv "failsetup" (rig only): the first translator vmap fails once, so the
// hotplug setup rolls back and is retried. (satoru)
int kld_family_vmap(struct drm_framebuffer *fb, struct iosys_map *map);

void kld_family_after_probe(void)
{
    struct pci_dev *pdev = lx_pci_bound_dev();
    struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;

    lx_log(dev ? "[kld-i915] probe left a drm device\n"
               : "[kld-i915] no drm device after probe\n");
    if (!dev)
        return;
    g_i915 = to_i915(dev);
    // review g3: with gtbroker, a gt register that is not on the measured
    // allowlist is refused by the broker (0x9400 is never written by i915 on
    // this path). (satoru)
    if (arg_value("gtbroker")) {
        long r = lx_broker_call(0x0029u, 0x9400, 4, 0x1, 0);
        lx_log(r < 0 ? "[kld-i915] gtbroker: unlisted gt register refused PASS\n"
                     : "[kld-i915] gtbroker: unlisted gt register FAIL (accepted)\n");
    }
    if (i915_model_on() && pdev->irq)
        i915_model_irq_start(pdev->irq);
    if (kdrm_client_setup(dev)) {
        // failsetup rig: the first setup was made to fail; retry it through
        // the drm client restore path (the path whose rollback crashed).
        // (satoru)
        if (g_failsetup_done) {
            lx_log("[kld-i915] failsetup: retrying the setup through drm_client_dev_restore\n");
            drm_client_dev_restore(dev);
            if (kdrm_client_setup(dev) == 0) {
                lx_log("[kld-i915] failsetup: retry set the mode up\n");
                goto ready;
            }
        }
        lx_log("[kld-i915] no output to drive: probe done, container idle\n");
        return;
    }
ready:
    if (kdrm_client_serve_enable() != 0) {
        if (i915_model_on())
            lx_task_spawn("i915-crc-test", crc_test, NULL);
        kdrm_client_test_pattern();
        g_pattern_done = 1;
    }
}

// end (satoru)
