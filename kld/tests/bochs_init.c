// bochs family init order: the drm core, then the tiny bochs driver.
//
// the container links the unmodified drm core, the kms helpers, the ttm core,
// the vram/ttm gem helpers and the tiny bochs driver. a real kernel runs
// drm_core_init() at module_init before the pci driver's probe; the container
// has no initcall sections, so the harness runs the two module entries in that
// order. (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/err.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_client.h>
#include <drm/drm_fourcc.h>
#include <linux/iosys-map.h>
#include <linux/sched.h>
#include <asm/current.h>

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"
#include "drm/kdrm_client.h"

extern int kld_init_drm_drv(void);
extern int kld_init_bochs(void);
extern struct pci_dev *lx_pci_bound_dev(void);
extern int kdrm_uapi_start(struct drm_device *dev);   /* kld/drm/kdrm_uapi.c (satoru) */
extern int lx_main_argc;      // lx_main.c (satoru)
extern char **lx_main_argv;

int kld_family_init(void)
{
    int rc;
    lx_log("[kld-bochs] init drm core\n");
    rc = kld_init_drm_drv();
    if (rc) {
        lx_log("[kld-bochs] drm core init rc=");
        lx_log_u((unsigned long)rc);
        lx_log("\n");
        return rc;
    }
    lx_log("[kld-bochs] init bochs\n");
    rc = kld_init_bochs();
    if (rc) {
        lx_log("[kld-bochs] bochs init rc=");
        lx_log_u((unsigned long)rc);
        lx_log("\n");
    }
    return rc;
}

// kurono.kld.aliastest=1 (argv "aliastest"): a vmap of deliberately scattered
// arena pages goes through the broker alias op, is checked against the
// frames' own arena mapping, a hostile munmap of it must be refused, one
// vunmap/vmap round trip runs, and the last alias stays live for the
// container's lifetime so kill loops tear it down. (satoru)
static void bochs_alias_selfcheck(void)
{
    enum { N = 48 };
    struct page *keep[N], *gap[N];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        keep[i] = alloc_page(GFP_KERNEL);
        gap[i] = alloc_page(GFP_KERNEL);   // leaves a hole after every kept page (satoru)
        if (!keep[i] || !gap[i]) { ok = 0; break; }
    }
    void *va = ok ? vmap(keep, N, VM_MAP, PAGE_KERNEL) : NULL;
    if (va) {
        vunmap(va);   // round trip: the broker drops the alias (satoru)
        va = vmap(keep, N, VM_MAP, PAGE_KERNEL);
    }
    int same = 0;
    if (va) {
        for (int i = 0; i < N; i++) {
            u32 *a = (u32 *)((char *)va + (size_t)i * PAGE_SIZE);
            u32 *b = (u32 *)page_address(keep[i]);
            a[7] = 0x5a5a0000u + (u32)i;
            if (b[7] == 0x5a5a0000u + (u32)i)
                same++;
        }
    }
    long mr = va ? lx_os_munmap_rc(va, PAGE_SIZE) : 0;
    for (int i = 0; i < N; i++)
        if (gap[i])
            __free_page(gap[i]);
    lx_log("[kld-bochs] aliastest va=");
    lx_log_hex((unsigned int)(unsigned long)(uintptr_t)va);
    lx_log(" same=");
    lx_log_u((unsigned long)same);
    lx_log("/");
    lx_log_u((unsigned long)N);
    lx_log(" munmap_rc=-");
    lx_log_u((unsigned long)(mr < 0 ? -mr : mr));
    lx_log((va && same == N && mr < 0) ? " PASS\n" : " FAIL\n");

    // case 2: 70 adjacent arena pages (a run that crosses a broker DMA record
    // boundary, the arena chunk is 64 pages) plus one separate page, so vmap
    // must take the alias path and split the run at the record edge. (satoru)
    enum { R = 70 };
    struct page *run = lx_alloc_page_run(R);
    struct page *extra = alloc_page(GFP_KERNEL);
    struct page *arr[R + 1];
    void *va2 = NULL;
    int same2 = 0;
    if (run && extra) {
        for (int i = 0; i < R; i++) arr[i] = run + i;
        arr[R] = extra;
        va2 = vmap(arr, R + 1, VM_MAP, PAGE_KERNEL);
    }
    if (va2) {
        for (int i = 0; i <= R; i++) {
            u32 *a = (u32 *)((char *)va2 + (size_t)i * PAGE_SIZE);
            u32 *b = (u32 *)page_address(arr[i]);
            a[3] = 0x6b6b0000u + (u32)i;
            if (b[3] == 0x6b6b0000u + (u32)i)
                same2++;
        }
    }
    lx_log("[kld-bochs] aliastest chunk-cross same=");
    lx_log_u((unsigned long)same2);
    lx_log("/");
    lx_log_u((unsigned long)(R + 1));
    lx_log((va2 && same2 == R + 1) ? " PASS\n" : " FAIL\n");
}

// kurono.kld.restoretest=1 (argv "restoretest"): 45 s after the desktop is
// served, a second in-kernel drm client (the stand-in for a userspace master)
// commits a solid magenta framebuffer, then drm_client_dev_restore() runs the
// way lastclose does; the translator's .restore must put the desktop back.
// the host takes a screendump after each log line. (satoru)
static struct drm_device *g_restore_dev;

static void bochs_restore_test(void *arg)
{
    static struct drm_client_dev intruder;
    struct drm_client_buffer *fb = NULL;
    struct iosys_map map;
    (void)arg;
    lx_os_sleep_ns(45ull * 1000000000ull);
    if (drm_client_init(g_restore_dev, &intruder, "intruder", NULL) ||
        drm_client_modeset_probe(&intruder, 0, 0)) {
        lx_log("[kld-bochs] restoretest FAIL intruder client\n");
        return;
    }
    struct drm_mode_set *ms = intruder.modesets;
    if (!ms || !ms->mode) {
        lx_log("[kld-bochs] restoretest FAIL no modeset\n");
        return;
    }
    fb = drm_client_framebuffer_create(&intruder, ms->mode->hdisplay,
                                       ms->mode->vdisplay, DRM_FORMAT_XRGB8888);
    if (IS_ERR(fb)) {
        lx_log("[kld-bochs] restoretest FAIL intruder fb\n");
        return;
    }
    // commit first, map after (a vmap before the commit pins the bo in
    // system memory and the plane then scans vram offset 0). (satoru)
    mutex_lock(&intruder.modeset_mutex);
    ms->fb = fb->fb;
    mutex_unlock(&intruder.modeset_mutex);
    int rc = drm_client_modeset_commit(&intruder);
    if (!rc && drm_client_buffer_vmap(fb, &map))
        rc = -ENOMEM;
    if (!rc) {
        for (u32 y = 0; y < ms->mode->vdisplay; y++) {
            u32 *row = (u32 *)((char *)map.vaddr + (size_t)y * fb->pitch);
            for (u32 x = 0; x < ms->mode->hdisplay; x++)
                row[x] = 0x00ff00ffu;
        }
        drm_client_framebuffer_flush(fb, NULL);
    }
    lx_log(rc ? "[kld-bochs] restoretest FAIL intruder commit\n"
              : "[kld-bochs] restoretest intruder committed\n");
    lx_os_sleep_ns(8ull * 1000000000ull);
    drm_client_dev_restore(g_restore_dev);
    lx_log("[kld-bochs] restoretest restore called\n");
}

void kld_family_after_probe(void)
{
    struct pci_dev *pdev = lx_pci_bound_dev();
    struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;

    for (int i = 1; i < lx_main_argc; i++)
        if (lx_main_argv[i] && !strcmp(lx_main_argv[i], "aliastest"))
            bochs_alias_selfcheck();
    if (dev)
        kdrm_uapi_start(dev);   /* /dev/dri, only when the kernel flag is on (satoru) */
    if (dev && !kdrm_client_setup(dev)) {
        for (int i = 1; i < lx_main_argc; i++)
            if (lx_main_argv[i] && !strcmp(lx_main_argv[i], "restoretest")) {
                g_restore_dev = dev;
                lx_os_thread_create(bochs_restore_test, NULL);
            }
        if (kdrm_client_serve_enable() != 0)
            kdrm_client_test_pattern();
    }
}

// family half of the client's per-commit report (d3 device-side check): the
// buffer object the plane scans out, as the vram helper places it, next to
// the pointer the translator copies into. bochs_hw_setbase programs the
// device's x/y offset from exactly this offset. (satoru)
void kld_family_after_commit(struct drm_framebuffer *fb, const void *vaddr,
                             bool is_iomem)
{
    struct drm_gem_vram_object *gbo;
    s64 off;

    if (!fb || !fb->obj[0])
        return;
    gbo = drm_gem_vram_of_gem(fb->obj[0]);
    off = drm_gem_vram_offset(gbo);
    lx_log("[kld-bochs] commit fb=");
    lx_log_u((unsigned long)fb->base.id);
    lx_log(" bo_offset=");
    lx_log_hex((unsigned int)off);
    lx_log(" pitch=");
    lx_log_u((unsigned long)fb->pitches[0]);
    lx_log(" vaddr=");
    lx_log_hex((unsigned int)(unsigned long)(uintptr_t)vaddr);
    lx_log(is_iomem ? " iomem\n" : " sysmem\n");
}

// end (satoru)
