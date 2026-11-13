// lx_emul: firmware loading through the container's own file syscalls.
//
// /lib/firmware/<name> resolves to /system/lib/firmware/<name> in Kurono's
// LinuxSyscall::ResolvePath, so a plain open/read is the whole transport. No
// broker op and no user pointer: the data lives in the container's address
// space, which is where the driver expects it.
//
// nowait runs the load on system_wq and calls the driver's continuation there,
// like the kernel firmware loader. -ENOENT is passed through faithfully; the
// iwlwifi firmware-name fallback depends on it. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "lx_kshim.h"

#define LX_FW_DIR "/lib/firmware/"

static int lx_fw_load(const struct firmware **fw_out, const char *name)
{
    char path[256];
    long size;
    void *data;
    struct firmware *fw;

    if (!fw_out || !name)
        return -EINVAL;
    *fw_out = NULL;
    // reject a path that escapes /lib/firmware. (satoru)
    if (!name[0] || name[0] == '/' || strstr(name, "..")) {
        lx_log("[lx_fw] ");
        lx_log(name);
        lx_log(" -> -ENOENT (plain name)\n");
        lx_log("[kld-fw] reject ");
        lx_log(name);
        lx_log(": not a plain firmware file name\n");
        return -ENOENT;
    }
    if (strlen(name) + sizeof(LX_FW_DIR) >= sizeof(path))
        return -ENAMETOOLONG;
    memcpy(path, LX_FW_DIR, sizeof(LX_FW_DIR) - 1);
    strcpy(path + sizeof(LX_FW_DIR) - 1, name);

    size = lx_os_file_size(path);
    if (size < 0) {
        lx_log("[lx_fw] ");
        lx_log(name);
        lx_log(" -> -ENOENT\n");
        return -ENOENT;
    }
    data = lx_os_malloc((unsigned long)size ? (unsigned long)size : 1);
    if (!data)
        return -ENOMEM;
    if (size > 0 && lx_os_read_file(path, data, (unsigned long)size) != size) {
        lx_os_free(data);
        return -EIO;
    }
    fw = lx_os_malloc(sizeof(*fw));
    if (!fw) {
        lx_os_free(data);
        return -ENOMEM;
    }
    fw->size = (size_t)size;
    fw->data = data;
    fw->priv = NULL;
    *fw_out = fw;
    lx_log("[lx_fw] ");
    lx_log(name);
    lx_log(" -> ok ");
    lx_log_u((unsigned long)size);
    lx_log(" bytes\n");
    return 0;
}

int request_firmware(const struct firmware **fw, const char *name,
                     struct device *device)
{
    (void)device;
    return lx_fw_load(fw, name);
}

int request_firmware_direct(const struct firmware **fw, const char *name,
                            struct device *device)
{
    (void)device;
    return lx_fw_load(fw, name);
}

int firmware_request_nowarn(const struct firmware **fw, const char *name,
                            struct device *device)
{
    (void)device;
    return lx_fw_load(fw, name);
}

void release_firmware(const struct firmware *fw)
{
    if (!fw)
        return;
    lx_os_free((void *)fw->data);
    lx_os_free((void *)fw);
}

struct lx_fw_nowait {
    struct work_struct work;
    char *name;
    void *context;
    void (*cont)(const struct firmware *fw, void *context);
};

static void lx_fw_nowait_work(struct work_struct *work)
{
    struct lx_fw_nowait *r = container_of(work, struct lx_fw_nowait, work);
    const struct firmware *fw = NULL;
    int rc = lx_fw_load(&fw, r->name);
    if (r->cont)
        r->cont(rc ? NULL : fw, r->context);
    lx_os_free(r->name);
    lx_os_free(r);
}

int request_firmware_nowait(struct module *module, bool uevent,
                            const char *name, struct device *device,
                            gfp_t gfp, void *context,
                            void (*cont)(const struct firmware *fw, void *context))
{
    struct lx_fw_nowait *r;
    size_t n;
    (void)module; (void)uevent; (void)device; (void)gfp;

    if (!name || !cont)
        return -EINVAL;
    r = lx_os_malloc(sizeof(*r));
    if (!r)
        return -ENOMEM;
    n = strlen(name) + 1;
    r->name = lx_os_malloc(n);
    if (!r->name) {
        lx_os_free(r);
        return -ENOMEM;
    }
    memcpy(r->name, name, n);
    r->context = context;
    r->cont = cont;
    INIT_WORK(&r->work, lx_fw_nowait_work);
    if (!queue_work(system_wq, &r->work)) {
        lx_os_free(r->name);
        lx_os_free(r);
        return -EBUSY;
    }
    return 0;
}

// end (satoru)
