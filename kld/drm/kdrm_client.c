// kld/drm/kdrm_client.c - the in-container drm client.
//
// This is the translator from the lead's architecture B: it uses only the
// upstream in-kernel client API (drm_client_init/register/modeset_probe,
// drm_client_framebuffer_create, drm_client_buffer_vmap,
// drm_client_modeset_commit, drm_client_framebuffer_flush). no driver symbol
// is referenced here, so cirrus today and i915/xe later run the same code.
//
// d1 uses the dumb framebuffer as a test surface: 8 colour bars, a 1-pixel
// checkerboard block and a 32x32 frame-counter block, then 120 frames of a
// moving block with one flush each. the pattern is held between the two phases
// so the host can take a screendump and compare pixels. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/err.h>
#include <linux/iosys-map.h>
#include <drm/drm_device.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>

#include "kimpl/lx_kshim.h"
#include "drm/kdrm_client.h"
#include "kimpl/lx_display.h"

extern struct pci_dev *lx_pci_bound_dev(void);
extern int lx_display_try_attach(void);
extern int lx_display_report_modes(const struct lx_disp_mode* modes,
                                   unsigned int count, unsigned int current);
extern int lx_display_start_serve(void);

struct kdrm_client {
    struct drm_client_dev client;
    struct drm_client_buffer *buffer;
    struct drm_connector *connector;
    struct drm_crtc *crtc;
    struct drm_display_mode mode;
    u32 *pixels;
    u32 pitch;
    u32 width, height;
    bool ready;
    bool committing;
    bool serving;
    struct drm_mode_set *modeset;
    struct drm_display_mode modes[32];
    int mode_map[32];       // reported (filtered) index -> connector mode index (satoru)
    int mode_count;
    int mode_index;
    int reported_count;     // entries of mode_map the kernel was given (satoru)
    bool log_full_copy;
};

// optional family hook, called after every commit with the framebuffer now on
// the crtc and the cpu pointer the client copies into. only the family knows
// its memory manager, so only it can say where that buffer lives in the
// device; the client stays driver-agnostic. (satoru)
extern void kld_family_after_commit(struct drm_framebuffer *fb,
                                    const void *vaddr, bool is_iomem)
    __attribute__((weak));

// optional family hooks for a driver whose gem objects have no generic
// vmap (drm_gem_vmap answers -EOPNOTSUPP, i915 is one): the family maps and
// unmaps the buffer of a framebuffer with its own api. (satoru)
extern int kld_family_vmap(struct drm_framebuffer *fb, struct iosys_map *map)
    __attribute__((weak));
extern void kld_family_vunmap(struct drm_framebuffer *fb) __attribute__((weak));

static int kdrm_vmap(struct drm_client_buffer *b, struct iosys_map *map)
{
    int ret = drm_client_buffer_vmap(b, map);
    if (ret == -EOPNOTSUPP && kld_family_vmap)
        ret = kld_family_vmap(b->fb, map);
    return ret;
}

static void kdrm_vunmap(struct drm_client_buffer *b)
{
    if (kld_family_vunmap)
        kld_family_vunmap(b->fb);
    else
        drm_client_buffer_vunmap(b);
}

static struct kdrm_client g_kdrm;
static const struct drm_client_funcs kdrm_client_funcs;

static void kdrm_fill(struct kdrm_client *k, u32 x, u32 y, u32 w, u32 h, u32 color)
{
    u32 px = k->pitch / 4;
    for (u32 yy = y; yy < y + h && yy < k->height; yy++) {
        u32 *row = k->pixels + (size_t)yy * px;
        for (u32 xx = x; xx < x + w && xx < k->width; xx++)
            row[xx] = color;
    }
}

static u32 kdrm_frame_color(unsigned int f)
{
    // exact through the xr24 -> rgb565 shadow conversion: every channel keeps
    // its top bits only. (satoru)
    return ((u32)((f * 8u) & 0xf8u) << 16) |
           ((u32)((f * 4u) & 0xfcu) << 8) |
           ((u32)((f * 8u) & 0xf8u));
}

static void kdrm_draw_static(struct kdrm_client *k, unsigned int frame)
{
    static const u32 bars[8] = {
        0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
        0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
    };
    u32 bw = k->width / 8;
    for (u32 i = 0; i < 8; i++) {
        u32 x = i * bw;
        u32 w = (i == 7) ? k->width - x : bw;
        kdrm_fill(k, x, 0, w, k->height, bars[i]);
    }
    // one-pixel checkerboard, 16x16, fixed place. (satoru)
    for (u32 y = 0; y < 16; y++)
        for (u32 x = 0; x < 16; x++)
            kdrm_fill(k, 64 + x, 64 + y, 1, 1,
                      ((x ^ y) & 1) ? 0x00ffffff : 0x00000000);
    // 32x32 frame counter block. (satoru)
    kdrm_fill(k, 96, 64, 32, 32, kdrm_frame_color(frame));
}

static int kdrm_flush(struct kdrm_client *k, int x1, int y1, int x2, int y2)
{
    struct drm_rect r = DRM_RECT_INIT(x1, y1, x2 - x1, y2 - y1);
    return drm_client_framebuffer_flush(k->buffer, &r);
}

// vunmap and delete the client's buffer, if any. (satoru)
static void kdrm_drop_buffer(struct kdrm_client *k)
{
    if (k->buffer) {
        if (k->pixels)
            kdrm_vunmap(k->buffer);
        drm_client_framebuffer_delete(k->buffer);
        k->buffer = NULL;
    }
    k->pixels = NULL;
}

static int kdrm_hotplug(struct drm_client_dev *client)
{
    struct kdrm_client *k = container_of(client, struct kdrm_client, client);
    struct drm_mode_set *ms = NULL;
    struct drm_display_mode *pref = NULL;
    int ret;

    if (k->ready || k->committing)
        return 0;
    k->committing = true;

    ret = drm_client_modeset_probe(client, 0, 0);
    if (ret)
        goto out;

    ms = NULL;
    for (struct drm_mode_set *it = client->modesets; it && it->crtc; it++) {
        if (it->mode && it->num_connectors) {
            ms = it;
            break;
        }
    }
    if (!ms) {
        ret = -ENODEV;
        goto out;
    }
    k->crtc = ms->crtc;
    k->connector = ms->connectors[0];
    k->mode = *ms->mode;
    k->width = ms->mode->hdisplay;
    k->height = ms->mode->vdisplay;

    k->buffer = drm_client_framebuffer_create(client, k->width, k->height,
                                              DRM_FORMAT_XRGB8888);
    if (IS_ERR(k->buffer)) {
        ret = PTR_ERR(k->buffer);
        k->buffer = NULL;
        goto out;
    }
    mutex_lock(&client->modeset_mutex);
    ms->fb = k->buffer->fb;
    // one output per translator: every other probed crtc is committed off.
    // a modeset with a mode but no fb is invalid (set_config warns and fails
    // the whole commit), which a device with two connected outputs hits.
    // (satoru)
    for (struct drm_mode_set *it = client->modesets; it && it->crtc; it++) {
        if (it == ms)
            continue;
        if (it->mode) {
            drm_mode_destroy(it->crtc->dev, it->mode);
            it->mode = NULL;
        }
        for (unsigned int i = 0; i < it->num_connectors; i++) {
            drm_connector_put(it->connectors[i]);
            it->connectors[i] = NULL;
        }
        it->num_connectors = 0;
        it->fb = NULL;
    }
    mutex_unlock(&client->modeset_mutex);

    ret = drm_client_modeset_commit(client);
    if (ret)
        goto out;

    // vmap AFTER the commit: a driver may move the buffer to its scanout
    // memory (a ttm driver moves a system buffer to vram) during the commit. the
    // cpu mapping must be the one the device scans out, not the old placement.
    // (satoru)
    {
        struct iosys_map map;
        ret = kdrm_vmap(k->buffer, &map);
        if (ret)
            goto out;
        k->pixels = map.vaddr;
        k->pitch = k->buffer->pitch;
        k->log_full_copy = true;
        if (kld_family_after_commit)
            kld_family_after_commit(k->buffer->fb, map.vaddr, map.is_iomem);
    }
    if (!k->pixels) {
        ret = -ENOMEM;
        goto out;
    }

    k->modeset = ms;
    k->mode_count = 0;
    {
        struct drm_display_mode *m;
        list_for_each_entry(m, &k->connector->modes, head) {
            if (k->mode_count >= 32)
                break;
            k->mode_map[k->mode_count] = k->mode_count;
            k->modes[k->mode_count++] = *m;
            if (m->hdisplay == k->width && m->vdisplay == k->height)
                k->mode_index = k->mode_count - 1;
        }
    }
    k->ready = true;
    lx_log("[kdrm] modeset committed ");
    lx_log_u(k->width);
    lx_log("x");
    lx_log_u(k->height);
    lx_log(" pitch ");
    lx_log_u(k->pitch);
    lx_log(" connector ");
    lx_log(k->connector->name);
    lx_log("\n");
out:
    if (ret) {
        // roll back: nothing of this attempt may survive into the next
        // hotplug or restore (it would overwrite k->buffer and leak the
        // buffer and its fb). the crtc lets go of the fb first. (satoru)
        if (ms) {
            mutex_lock(&client->modeset_mutex);
            ms->fb = NULL;
            mutex_unlock(&client->modeset_mutex);
        }
        kdrm_drop_buffer(k);
        lx_log("[kdrm] hotplug setup failed rc=-");
        lx_log_u((unsigned long)(-ret));
        lx_log(", deferred to the next hotplug or restore\n");
    }
    k->committing = false;
    // never report failure to the drm core: drm_client_dev_hotplug latches
    // hotplug_failed on a nonzero return and never calls us again (a
    // userspace master makes the commit fail with -EBUSY for a while). the
    // next hotplug or .restore retries. (satoru)
    return 0;
}

// lastclose / master drop: drm_client_dev_restore calls this. put the
// translator's own mode and framebuffer back on the crtc and flush the whole
// buffer, which kept receiving the desktop's frames meanwhile. returns 0 when
// the desktop is on the crtc again. (satoru)
static int kdrm_restore(struct drm_client_dev *client)
{
    struct kdrm_client *k = container_of(client, struct kdrm_client, client);
    int ret;

    if (k->committing)
        return -EBUSY;
    if (!k->ready) {
        // setup never completed (e.g. a master held the device): retry it.
        kdrm_hotplug(client);
        if (!k->ready)
            return -EAGAIN;
        lx_log("[kdrm] restore: deferred setup completed\n");
        return 0;
    }
    mutex_lock(&client->modeset_mutex);
    if (k->modeset)
        k->modeset->fb = k->buffer->fb;
    mutex_unlock(&client->modeset_mutex);
    ret = drm_client_modeset_commit(client);
    if (ret) {
        lx_log("[kdrm] restore: commit rc=-");
        lx_log_u((unsigned long)(-ret));
        lx_log("\n");
        return ret;
    }
    drm_client_framebuffer_flush(k->buffer, NULL);
    lx_log("[kdrm] restore: desktop framebuffer back on the crtc\n");
    return 0;
}

static void kdrm_unregister(struct drm_client_dev *client)
{
    struct kdrm_client *k = container_of(client, struct kdrm_client, client);
    kdrm_drop_buffer(k);
    k->ready = false;
    drm_client_release(client);
}

static const struct drm_client_funcs kdrm_client_funcs = {
    .unregister = kdrm_unregister,
    .restore = kdrm_restore,
    .hotplug = kdrm_hotplug,
};

int kdrm_client_setup(struct drm_device *dev)
{
    int ret;
    if (!dev)
        return -ENODEV;
    if (g_kdrm.ready)
        return 0;
    ret = drm_client_init(dev, &g_kdrm.client, "kdrm", &kdrm_client_funcs);
    if (ret)
        return ret;
    drm_client_register(&g_kdrm.client);
    if (!g_kdrm.ready) {
        // released without .unregister: drop what a partial setup left. (satoru)
        kdrm_drop_buffer(&g_kdrm);
        drm_client_release(&g_kdrm.client);
        return -ENODEV;
    }
    return 0;
}

int kdrm_client_serve_enable(void)
{
    struct kdrm_client *k = &g_kdrm;
    struct lx_disp_mode modes[32];
    int n, cur = -1;

    if (!k->ready || k->serving)
        return k->serving ? 0 : -1;
    // a restart can arrive while the gui is still restoring the previous
    // instance (the precheck refuses with -16 and the 1 s attach spacing
    // refuses with -11). retry instead of falling into the d1 pattern path,
    // which is not a display bring-up and leaves the desktop restored.
    // (satoru)
    {
        int tries = 0;
        while (lx_display_try_attach() != 0) {
            if (++tries > 400)
                return -1;
            lx_os_sleep_ns(25000000ull);
        }
    }
    n = 0;
    for (int i = 0; i < k->mode_count && n < 32; i++) {
        // the bridge's scanout cap is a hard limit; an EDID can list modes
        // beyond it, and the kernel refuses the whole table if one does not
        // fit. report only modes the bridge can actually scan out. (satoru)
        unsigned int mw = k->modes[i].hdisplay;
        unsigned int mh = k->modes[i].vdisplay;
        if (!mw || !mh || mw > LX_DISP_CAP_W || mh > LX_DISP_CAP_H)
            continue;
        if ((unsigned long long)mw * 4ull * mh > LX_DISP_SCANOUT_BYTES)
            continue;
        k->mode_map[n] = i;
        modes[n].width = mw;
        modes[n].height = mh;
        modes[n].pitch = mw * 4u;
        modes[n].refresh_mhz = 60000;
        modes[n].flags = 0;
        if (modes[n].width == k->width && modes[n].height == k->height)
            cur = n;
        n++;
    }
    if (n == 0 || cur < 0)
        return -1;
    k->reported_count = n;
    if (lx_display_report_modes(modes, (unsigned int)n, (unsigned int)cur) != 0)
        return -1;
    if (lx_display_start_serve() != 0)
        return -1;
    k->serving = true;
    lx_log("[kdrm] display bridge attached, serving modes=");
    lx_log_u((unsigned long)n);
    lx_log("\n");
    return 0;
}

// kernel ring -> dumb framebuffer. the scanout is xrgb8888 with spitch; the
// client buffer may have a different pitch. one flush per rect. (satoru)
int kdrm_serve_copy(const unsigned char* src, unsigned int spitch,
                    int x, int y, int w, int h)
{
    struct kdrm_client *k = &g_kdrm;
    if (!k->ready || !src || w <= 0 || h <= 0 || k->width == 0)
        return -1;
    if (x < 0 || y < 0 || x + w > (int)k->width || y + h > (int)k->height)
        return -1;
    // the first full copy after a mode set names the destination once: the
    // d3 rig checks it against where the device scans out. (satoru)
    if (k->log_full_copy && x == 0 && y == 0 && w == (int)k->width &&
        h == (int)k->height) {
        k->log_full_copy = false;
        lx_log("[kdrm] full copy dst=");
        lx_log_hex((unsigned int)(unsigned long)(uintptr_t)k->pixels);
        lx_log(" pitch=");
        lx_log_u(k->pitch);
        lx_log(" ");
        lx_log_u((unsigned long)w);
        lx_log("x");
        lx_log_u((unsigned long)h);
        lx_log("\n");
    }
    for (int r = 0; r < h; r++) {
        const unsigned char* s = src + (size_t)(y + r) * spitch + (size_t)x * 4u;
        unsigned char* d = (unsigned char*)k->pixels + (size_t)(y + r) * k->pitch +
                           (size_t)x * 4u;
        memcpy(d, s, (size_t)w * 4u);
    }
    struct drm_rect rect = DRM_RECT_INIT(x, y, w, h);
    return drm_client_framebuffer_flush(k->buffer, &rect);
}

// re-create the dumb buffer for a mode already in the reported list and commit
// it. the kernel picked the index from the same list. (satoru)
int kdrm_serve_set_mode(unsigned int index)
{
    struct kdrm_client *k = &g_kdrm;
    struct drm_client_buffer *nb;
    struct iosys_map map;
    int ret;

    // the index is from the filtered list reported to the kernel. (satoru)
    if (!k->ready || index >= (unsigned int)k->reported_count || !k->modeset)
        return -1;
    int mi = k->mode_map[index];
    if (mi < 0 || mi >= k->mode_count)
        return -1;
    nb = drm_client_framebuffer_create(&k->client, k->modes[mi].hdisplay,
                                       k->modes[mi].vdisplay,
                                       DRM_FORMAT_XRGB8888);
    if (IS_ERR(nb))
        return PTR_ERR(nb);
    // the mode change must reach the crtc, not only the framebuffer: a bigger
    // buffer with the old mode leaves the device in the old mode (the d2.4
    // test only checked the kernel scanout). (satoru)
    struct drm_display_mode *nm = drm_mode_duplicate(k->client.dev,
                                                     &k->modes[mi]);
    if (!nm) {
        drm_client_framebuffer_delete(nb);
        return -ENOMEM;
    }
    struct drm_display_mode *old_mode;
    mutex_lock(&k->client.modeset_mutex);
    old_mode = k->modeset->mode;
    k->modeset->mode = nm;
    k->modeset->fb = nb->fb;
    mutex_unlock(&k->client.modeset_mutex);
    ret = drm_client_modeset_commit(&k->client);
    // same ordering as hotplug: map the buffer where the device put it. (satoru)
    if (!ret)
        ret = kdrm_vmap(nb, &map);
    if (ret) {
        // put the old mode and buffer back on the crtc before nb goes away, so
        // the crtc never scans out a deleted framebuffer. (satoru)
        mutex_lock(&k->client.modeset_mutex);
        k->modeset->mode = old_mode;
        k->modeset->fb = k->buffer ? k->buffer->fb : NULL;
        mutex_unlock(&k->client.modeset_mutex);
        if (k->buffer)
            drm_client_modeset_commit(&k->client);
        drm_mode_destroy(k->client.dev, nm);
        drm_client_framebuffer_delete(nb);
        lx_log("[kdrm] setmode failed, old mode kept\n");
        return ret;
    }
    drm_mode_destroy(k->client.dev, old_mode);
    lx_log("[kdrm] setmode fb=");
    lx_log_u((unsigned long)nb->fb->base.id);
    lx_log(" vmap=");
    lx_log_hex((unsigned int)(unsigned long)(uintptr_t)map.vaddr);
    lx_log(" pitch=");
    lx_log_u(nb->pitch);
    lx_log(" ");
    lx_log_u((unsigned long)k->modes[mi].hdisplay);
    lx_log("x");
    lx_log_u((unsigned long)k->modes[mi].vdisplay);
    lx_log("\n");
    if (k->buffer) {
        kdrm_vunmap(k->buffer);
        drm_client_framebuffer_delete(k->buffer);
    }
    k->buffer = nb;
    k->pixels = map.vaddr;
    k->pitch = nb->pitch;
    k->width = k->modes[mi].hdisplay;
    k->height = k->modes[mi].vdisplay;
    k->mode_index = (int)index;
    k->mode = k->modes[mi];
    k->log_full_copy = true;
    if (kld_family_after_commit)
        kld_family_after_commit(nb->fb, map.vaddr, map.is_iomem);
    lx_log("[kdrm] mode set ");
    lx_log_u(k->width);
    lx_log("x");
    lx_log_u(k->height);
    lx_log("\n");
    return 0;
}

static void kdrm_print_frame_stats(const char *what, u64 ns, unsigned int frames)
{
    lx_log("[kdrm] ");
    lx_log(what);
    lx_log(" frames=");
    lx_log_u(frames);
    lx_log(" ms=");
    lx_log_u((unsigned long)(ns / 1000000ull));
    lx_log(" us/frame=");
    lx_log_u((unsigned long)(frames ? (ns / 1000ull) / frames : 0));
    lx_log("\n");
}

int kdrm_client_test_pattern(void)
{
    struct kdrm_client *k = &g_kdrm;
    int ret;

    if (!k->ready)
        return -ENODEV;

    kdrm_draw_static(k, 0);
    ret = kdrm_flush(k, 0, 0, k->width, k->height);
    if (ret) {
        lx_log("[kdrm] static flush failed ");
        lx_log_u((unsigned long)(-ret));
        lx_log("\n");
        return ret;
    }
    lx_log("[kdrm] pattern ready ");
    lx_log_u(k->width);
    lx_log("x");
    lx_log_u(k->height);
    lx_log("\n");

    // hold the pattern long enough for a host screendump. (satoru)
    lx_os_sleep_ns(20000000000ull);

    {
        u64 t0 = lx_os_now_ns();
        int bx = 0, by = k->height - 96;
        for (unsigned int f = 1; f <= 120; f++) {
            int nx = (int)((u64)(k->width - 32) * f / 120u);
            int x1 = bx < nx ? bx : nx;
            int x2 = (bx > nx ? bx : nx) + 32;
            kdrm_fill(k, bx, by, 32, 32, 0x00000000);
            kdrm_fill(k, nx, by, 32, 32, kdrm_frame_color(f));
            ret = kdrm_flush(k, x1, by, x2, by + 32);
            if (ret) {
                lx_log("[kdrm] frame flush failed ");
                lx_log_u((unsigned long)(-ret));
                lx_log("\n");
                return ret;
            }
            bx = nx;
        }
        kdrm_print_frame_stats("moving block", lx_os_now_ns() - t0, 120);
    }
    return 0;
}

// end (satoru)
