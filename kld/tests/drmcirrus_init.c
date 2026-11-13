// drmcirrus family init order and the D1.2 probe report.
//
// the container links the unmodified drm core, the kms/shmem helpers and the
// tiny cirrus driver. a real kernel runs drm_core_init() at module_init before
// the pci driver's probe; the container has no initcall sections, so the
// harness runs the two module entries in that order. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/err.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <linux/sched.h>
#include <asm/current.h>

#include "kimpl/lx_kshim.h"
#include "drm/kdrm_client.h"

extern int kld_init_drm_drv(void);
extern int kld_init_cirrus(void);
extern struct pci_dev *lx_pci_bound_dev(void);
extern int kdrm_uapi_start(struct drm_device *dev);

static void cirrus_report(void)
{
    struct pci_dev *pdev = lx_pci_bound_dev();
    struct drm_device *dev;
    struct drm_connector *conn;
    struct drm_connector_list_iter iter;
    int n = 0;

    if (!pdev) {
        lx_log("[kld-drmcirrus] no pci device bound\n");
        return;
    }
    dev = pci_get_drvdata(pdev);
    if (!dev) {
        lx_log("[kld-drmcirrus] pci driver data is NULL\n");
        return;
    }
    lx_log("[kld-drmcirrus] drm_dev_register ok, primary minor ");
    lx_log_u(dev->primary ? dev->primary->index : 0);
    lx_log("\n");

    lx_log("[kld-drmcirrus] connection_mutex locked before probe = ");
    lx_log_u((unsigned long)mutex_is_locked(&dev->mode_config.connection_mutex));
    lx_log("\n");
    drm_connector_list_iter_begin(dev, &iter);
    drm_for_each_connector_iter(conn, &iter) {
        struct drm_display_mode *mode;
        int modes = 0;
        int w = 0, h = 0;
        lx_log("[kld-drmcirrus] probing connector ");
        lx_log(conn->name ? conn->name : "?");
        lx_log("\n");
        mutex_lock(&dev->mode_config.mutex);
        int rc = drm_helper_probe_single_connector_modes(conn, 4096, 4096);
        mutex_unlock(&dev->mode_config.mutex);
        if (rc < 0) {
            lx_log("[kld-drmcirrus] connector probe failed ");
            lx_log_u((unsigned long)(-rc));
            lx_log("\n");
        }
        list_for_each_entry(mode, &conn->modes, head) {
            if (!modes) { w = mode->hdisplay; h = mode->vdisplay; }
            modes++;
        }
        lx_log("[kld-drmcirrus] connector ");
        lx_log(conn->name ? conn->name : "?");
        lx_log(" status=");
        lx_log(conn->status == connector_status_connected ? "connected" :
               conn->status == connector_status_disconnected ? "disconnected" :
               conn->status == connector_status_unknown ? "unknown" : "?");
        lx_log(" modes=");
        lx_log_u((unsigned long)modes);
        lx_log(" preferred=");
        lx_log_u((unsigned long)w);
        lx_log("x");
        lx_log_u((unsigned long)h);
        lx_log("\n");
        list_for_each_entry(mode, &conn->modes, head) {
            if (n++ >= 16)
                break;
            lx_log("[kld-drmcirrus]   mode ");
            lx_log_u((unsigned long)mode->hdisplay);
            lx_log("x");
            lx_log_u((unsigned long)mode->vdisplay);
            lx_log(mode->type & DRM_MODE_TYPE_PREFERRED ? " preferred" : "");
            lx_log("\n");
        }
    }
    drm_connector_list_iter_end(&iter);
}

int kld_family_init(void)
{
    int rc;
    lx_log("[kld-drmcirrus] init drm core\n");
    rc = kld_init_drm_drv();
    if (rc) {
        lx_log("[kld-drmcirrus] drm core init rc=");
        lx_log_u((unsigned long)rc);
        lx_log("\n");
        return rc;
    }
    lx_log("[kld-drmcirrus] init cirrus\n");
    rc = kld_init_cirrus();
    if (rc) {
        lx_log("[kld-drmcirrus] cirrus init rc=");
        lx_log_u((unsigned long)rc);
        lx_log("\n");
    }
    return rc;
}

void kld_family_after_probe(void)
{
    struct pci_dev *pdev = lx_pci_bound_dev();
    struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;

    cirrus_report();
    if (dev)
        kdrm_uapi_start(dev);   /* /dev/dri, only when the kernel flag is on */
    if (dev && !kdrm_client_setup(dev)) {
        if (kdrm_client_serve_enable() != 0)
            kdrm_client_test_pattern();
    }
}

// end (satoru)
