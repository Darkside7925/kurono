// address-taken functions of the drm families (turn 5, strict gen_stubs).
// these functions are only stored in driver tables (file_operations, dev_pm_ops,
// drm_driver, sysctl tables, wait entries) on paths a container never runs
// (debugfs/sysfs files, system suspend, perf, an unused tiny driver in the same
// image). gen_stubs refuses a stub for an address it cannot classify, so each
// gets its real prototype here and is loud if anything ever calls it ("real or
// loud", the rule of lx_aux_providers.c). weak: a family that links the real
// implementation (bochs: the vram helpers; kdrm_uapi.c: kill_anon_super)
// keeps it. survey: scratch/kld/gfx/survey_datastubs.md. (satoru)

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/perf_event.h>
#include <linux/sysctl.h>
#include <linux/wait.h>
#include <linux/wait_bit.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>

#include "lx_kshim.h"

extern void lx_unimpl(const char *sym) __attribute__((noreturn));

#define LOUD(name) lx_unimpl(#name)
#define W __attribute__((weak))

W int bit_wait(struct wait_bit_key *key, int mode) { LOUD(bit_wait); }
W loff_t default_llseek(struct file *file, loff_t offset, int whence) { LOUD(default_llseek); }
W loff_t generic_file_llseek(struct file *file, loff_t offset, int whence) { LOUD(generic_file_llseek); }
W void kill_anon_super(struct super_block *sb) { LOUD(kill_anon_super); }
W resource_size_t pcibios_align_resource(void *data, const struct resource *res,
                                         resource_size_t size, resource_size_t align)
{ LOUD(pcibios_align_resource); }
W ssize_t perf_event_sysfs_show(struct device *dev, struct device_attribute *attr, char *page)
{ LOUD(perf_event_sysfs_show); }
W int pm_generic_freeze(struct device *dev) { LOUD(pm_generic_freeze); }
W int pm_generic_poweroff(struct device *dev) { LOUD(pm_generic_poweroff); }
W int pm_generic_restore(struct device *dev) { LOUD(pm_generic_restore); }
W int pm_generic_resume(struct device *dev) { LOUD(pm_generic_resume); }
W int pm_generic_runtime_resume(struct device *dev) { LOUD(pm_generic_runtime_resume); }
W int pm_generic_runtime_suspend(struct device *dev) { LOUD(pm_generic_runtime_suspend); }
W int pm_generic_suspend(struct device *dev) { LOUD(pm_generic_suspend); }
W int pm_generic_thaw(struct device *dev) { LOUD(pm_generic_thaw); }
W int proc_dointvec_minmax(const struct ctl_table *t, int write, void *buf, size_t *lenp, loff_t *ppos)
{ LOUD(proc_dointvec_minmax); }
W ssize_t simple_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{ LOUD(simple_attr_read); }
W ssize_t simple_attr_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{ LOUD(simple_attr_write); }
W int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{ LOUD(woken_wake_function); }
W int drm_gem_ttm_dumb_map_offset(struct drm_file *file, struct drm_device *dev,
                                  uint32_t handle, uint64_t *offset)
{ LOUD(drm_gem_ttm_dumb_map_offset); }
W int drm_gem_vram_driver_dumb_create(struct drm_file *file, struct drm_device *dev,
                                      struct drm_mode_create_dumb *args)
{ LOUD(drm_gem_vram_driver_dumb_create); }
W void drm_gem_vram_simple_display_pipe_cleanup_fb(struct drm_simple_display_pipe *pipe,
                                                  struct drm_plane_state *old_state)
{ LOUD(drm_gem_vram_simple_display_pipe_cleanup_fb); }
W int drm_gem_vram_simple_display_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
                                                 struct drm_plane_state *new_state)
{ LOUD(drm_gem_vram_simple_display_pipe_prepare_fb); }
W enum drm_mode_status drm_vram_helper_mode_valid(struct drm_device *dev,
                                                 const struct drm_display_mode *mode)
{ LOUD(drm_vram_helper_mode_valid); }
W void drm_vram_mm_debugfs_init(struct drm_minor *minor) { LOUD(drm_vram_mm_debugfs_init); }

// cpu_sibling_map is per-cpu DATA (topology_sibling_cpumask), read only by the
// i915 pmu cpu-offline callback, which never runs (perf_pmu_register answers
// -ENODEV). a real per-cpu object so the read would see a cpumask. (satoru)
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_sibling_map);

// end (satoru)
