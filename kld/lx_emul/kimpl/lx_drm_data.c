// lx_emul: the data symbols the drm core references but never owns at link
// time. a generated [kld-stub] is a zero-arg FUNCTION; reading one of these as
// data would interpret its code bytes as a value, which is silent corruption.
// they must be real objects. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/device/bus.h>
#include <linux/moduleparam.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>

// x86's uaccess range check: the container's pointers live below the 47-bit
// user boundary, the same value a 4-level kernel bakes in. (satoru)
unsigned long USER_PTR_MAX = (1ul << 47) - PAGE_SIZE;

// kernel/sys.c. (satoru)
int oops_in_progress;
int overflowuid = 65534;

// arch/x86/mm/init_64.c: the mask for kernel pte attributes. the container
// never installs a kernel pte through these helpers, so ~0 is the configured
// default and keeps every attribute legal. (satoru)
pteval_t __default_kernel_pte_mask __read_mostly = ~0;

// module parameter ops the drm core references from its __param section.
// nothing parses module parameters in the container, but if the debugfs param
// files are ever read these do the real conversion. (satoru)
int lx_param_set_bool(const char *val, const struct kernel_param *kp)
{
    if (!val)
        val = "1";
    return kstrtobool(val, kp->arg);
}

int lx_param_get_bool(char *buffer, const struct kernel_param *kp)
{
    return scnprintf(buffer, PAGE_SIZE, "%c\n", *(bool *)kp->arg ? 'Y' : 'N');
}

const struct kernel_param_ops param_ops_bool = {
    .flags = KERNEL_PARAM_OPS_FL_NOARG,
    .set = lx_param_set_bool,
    .get = lx_param_get_bool,
};

int lx_param_set_ulong(const char *val, const struct kernel_param *kp)
{
    return kstrtoul(val, 0, (unsigned long *)kp->arg);
}

int lx_param_get_ulong(char *buffer, const struct kernel_param *kp)
{
    return scnprintf(buffer, PAGE_SIZE, "%lu\n", *(unsigned long *)kp->arg);
}

const struct kernel_param_ops param_ops_ulong = {
    .set = lx_param_set_ulong,
    .get = lx_param_get_ulong,
};

// end (satoru)
