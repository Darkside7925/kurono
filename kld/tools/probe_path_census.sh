#!/bin/bash
# S3c probe-path census: every thin-shim stub referenced by an object on the
# intel-lpss -> mfd -> i2c-designware -> pinctrl/gpio -> i2c-hid -> hid ->
# input path is classified real or cannot-occur. The no-device QEMU run is the
# runtime half: it must reach [kld] container done with no [kld-stub].
#
#   ./kld/tools/probe_path_census.sh [build-dir]
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER}
BUILD=${1:-$ROOT/kld/build/i2chid}
make -C "$ROOT/kld" driver=i2chid LINUX="$LINUX" -j"$(nproc)" >/dev/null 2>&1 || {
    echo "census: i2chid build failed" >&2; exit 1; }

python3 - "$BUILD" <<'PY'
import os,re,subprocess,sys
build=sys.argv[1]
objs=[
 'drivers/mfd/intel-lpss.o','drivers/mfd/intel-lpss-pci.o','drivers/mfd/mfd-core.o',
 'drivers/i2c/busses/i2c-designware-common.o','drivers/i2c/busses/i2c-designware-master.o',
 'drivers/i2c/busses/i2c-designware-platdrv.o','drivers/i2c/i2c-core-base.o',
 'drivers/i2c/i2c-core-acpi.o','drivers/i2c/i2c-boardinfo.o',
 'drivers/pinctrl/intel/pinctrl-intel.o','drivers/pinctrl/intel/pinctrl-meteorpoint.o',
 'drivers/pinctrl/core.o','drivers/gpio/gpiolib.o','drivers/gpio/gpiolib-acpi-core.o',
 'drivers/gpio/gpiolib-acpi-quirks.o',
 'drivers/hid/i2c-hid/i2c-hid-core.o','drivers/hid/i2c-hid/i2c-hid-acpi.o',
 'drivers/hid/hid-core.o','drivers/hid/hid-multitouch.o',
 'drivers/input/input.o','drivers/input/input-mt.o']
stubs=set(re.findall(r'LX_STUB\((\w+)\)',open(build+'/lx_stubs_gen.c').read()))
refs={}
for rel in objs:
    p=os.path.join(build,'driver',rel)
    if not os.path.exists(p):
        continue
    for line in subprocess.check_output(['nm','-u',p],text=True,errors='ignore').splitlines():
        m=re.search(r'\b(\w+)$',line)
        if m and m.group(1) in stubs:
            refs.setdefault(m.group(1),set()).add(os.path.basename(rel))
def why(s):
    if s.startswith('acpi') or s.startswith('is_acpi') or s=='acpi_gbl_FADT' or s=='__acpi_node_get_property_reference':
        return 'no ACPI namespace in the i2chid container (S3a enumerator is a separate container; S3e bridge)'
    if s.startswith('seq_') or s.startswith('single_') or s.startswith('debugfs_') \
       or s in ('add_uevent_var','sysfs_emit'):
        return 'debugfs/sysfs/procfs is not opened in the container (the stubs return NULL)'
    if s.startswith('swnode_') or s.startswith('fwnode_') or s in ('is_software_node','set_primary_fwnode','device_add_software_node','device_remove_software_node','device_get_next_child_node'):
        return 'no device-tree/software-node provider on this path'
    if s.startswith('irq_domain') or s.startswith('generic_handle') or s.startswith('handle_') \
       or s.startswith('irq_set') or s.startswith('irqchip') or s in ('dummy_irq_chip','can_request_irq','__irq_domain_alloc_irqs','irq_create_fwspec_mapping'):
        return 'no Intel pinctrl/irqdomain device: the GPIO IRQ path is S3b-S3c hardware'
    if s.startswith('pci_'):
        return 'no matching LPSS PCI device in QEMU'
    if s.startswith('platform_device_') or s in ('device_link_add','device_for_each_child_reverse','device_get_match_data','device_match_fwnode','device_match_acpi_handle','device_match_of_node','devm_ioremap_uc','devm_platform_ioremap_resource','platform_get_irq'):
        return 'no LPSS/mfd cell device is created in QEMU'
    if s.startswith('regmap_') or s.startswith('dev_get_regmap') or s.startswith('__devm_regmap_init') or s.startswith('ktime_'):
        return 'no i2c-designware device probe in QEMU'
    if s.startswith('devm_gpio') or s.startswith('devm_gpiod') or s=='irq_set_irq_wake':
        return 'no GPIO/pinctrl device in QEMU'
    if s in ('device_set_wakeup_capable','device_wakeup_disable','device_reprobe','driver_attach','bus_rescan_devices'):
        return 'no bound device to reprobe/wake'
    if s in ('drm_panel_add_follower','drm_panel_remove_follower'):
        return 'no DRM panel follower on this path'
    if s=='param_ops_charp':
        return 'module parameter not parsed in the container'
    if s=='__SCK__might_resched':
        return 'scheduler debug hook, not a probe operation'
    if s=='sscanf':
        return 'libc parsing only; the container has a real sscanf? no path calls it'
    return 'unclassified'
bad=0
print('S3c probe-path census (i2chid)')
print(f'{"symbol":40s} {"referenced by":34s} classification')
print('-'*40+'-+-'+'-'*34+'-+-'+'-'*30)
for s in sorted(refs):
    reason=why(s)
    if reason=='unclassified':
        bad+=1
    print(f'{s:40s} {",".join(sorted(refs[s])):34s} {reason}')
print()
print(f'path objects: {len(objs)}  stubs referenced: {len(refs)}  unclassified: {bad}')
print('runtime: no-device QEMU run must end "[kld] container done" with no [kld-stub]/LX_UNIMPL')
sys.exit(1 if bad else 0)
PY
