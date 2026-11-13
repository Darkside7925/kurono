# KLD out-of-tree build harness.
#
#   make driver=e1000e          build the container for one driver
#   make driver=e1000e census   build + print the symbol census
#   make clean
#
# Produces a musl-static Kurono userspace binary containing the UNMODIFIED
# upstream driver sources plus lx_emul. The Kurono kernel links none of this -
# that invariant is the point of the whole design.
#
# The kernel version is pinned in KLD_KERNEL_VERSION and must not drift; see
# feats/kld-linux-driver-compat/readme.md.
# end (satoru)

KVER      := $(shell cat KLD_KERNEL_VERSION)
LINUX     ?= ../third_party/linux-$(KVER)
driver    ?= e1000e
BUILD     := build/$(driver)

# ── per-worktree generated headers (KGEN) ───────────────────────────────────
# the shared linux tree is configured under a lock, then the generated output
# is snapshotted into THIS worktree. no compile may read the shared tree's
# generated dirs: another worker's config would otherwise change our layouts.
# a family with kld/config/<family>.config (i915) gets its own snapshot,
# build/kgen-<family>, from kld.config + that overlay; the shared snapshot
# and every other family are untouched by it. (satoru)
KGEN_OVERLAY := $(wildcard config/$(driver).config)
ifneq ($(KGEN_OVERLAY),)
KGEN := $(abspath build/kgen-$(driver))
KGEN_ENV := KLD_KGEN="$(KGEN)" KLD_CONFIG_OVERLAY="$(abspath $(KGEN_OVERLAY))" KLD_FAMILY="$(driver)"
else
KGEN := $(abspath build/kgen)
KGEN_ENV :=
endif
KGEN_SRC := kld/config/kld.config
KGEN_GEN := $(shell $(KGEN_ENV) LINUX="$(LINUX)" bash tools/gen_config.sh)
$(info $(KGEN_GEN))
KGEN_CHECK := $(shell $(KGEN_ENV) bash tools/kgen_check.sh 2>&1)
ifneq ($(KGEN_CHECK),)
$(error $(KGEN_CHECK))
endif
KGEN_STALE := $(shell $(KGEN_ENV) bash tools/kgen_stale.sh 2>&1)
$(info $(KGEN_STALE))
MUSLCC    ?= musl-gcc

# where each supported driver's sources live inside the vendored tree. (satoru)
DRVDIR_e1000e := $(LINUX)/drivers/net/ethernet/intel/e1000e
DRVDIR        := $(DRVDIR_$(driver))

# explicit source lists (paths inside the vendored tree) for families and
# tests, instead of a directory wildcard. (satoru)
DRVSRCS_hidparse := drivers/hid/hid-core.c drivers/hid/hid-quirks.c drivers/hid/hid-debug.c
TEST_hidparse    := tests/hidparse.c
# shim self-test family: no device, runs kld_test_main() instead of a driver.
# the three upstream lib self-tests are compiled as driver objects so their
# module_init aliases are renamed by MULTI_apitest and callable by name. (satoru)
TEST_apitest     := tests/apitest.c tests/apitest_crypto.c tests/apitest_wpa.c
TEST_iwlwifi     := tests/iwlwifi_init.c tests/wifi_ctl.c
# HID-over-I2C simulation: upstream i2c-hid + hid + hid-multitouch + input core
# against a software ELAN06FA model. no PCI, no ACPI, no pinctrl. (satoru)
DRVSRCS_hidsim := drivers/i2c/i2c-boardinfo.c drivers/i2c/i2c-core-base.c \
  drivers/i2c/i2c-core-smbus.c drivers/i2c/i2c-core-acpi.c \
  drivers/hid/i2c-hid/i2c-hid-core.c drivers/hid/i2c-hid/i2c-hid-dmi-quirks.c \
  drivers/hid/hid-core.c drivers/hid/hid-input.c drivers/hid/hid-quirks.c \
  drivers/hid/hidraw.c drivers/hid/hid-debug.c \
  drivers/hid/hid-generic.c drivers/hid/hid-multitouch.c \
  drivers/gpio/gpiolib.c drivers/gpio/gpiolib-devres.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c drivers/gpio/gpio-mockup.c \
  drivers/input/input.c drivers/input/input-mt.c drivers/input/input-compat.c \
  drivers/input/input-poller.c drivers/input/ff-core.c drivers/input/touchscreen.c
TEST_hidsim      := tests/hidsim.c tests/elan06fa_model.c tests/hidsim_acpi_data.c
MULTI_hidsim     := 1
BUILTIN_hidsim   := drivers/gpio/gpiolib.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c
FAMCFLAGS_hidsim := -I$(LINUX)/drivers/hid/i2c-hid
DRVSRCS_apitest  := lib/test_xarray.c lib/test_ida.c lib/test_rhashtable.c
MULTI_apitest    := 1
# the laptop touchpad stack (ELAN06FA, i2c-hid over LPSS i2c 80:15.1, GpioInt on
# INTC1084), bottom to top. needs the fragment-configured headers:
#   make driver=i2chid LINUX=../third_party/linux-6.12.99-kld   (satoru)
TEST_i2chid     := tests/i2chid_init.c
TEST_tplogic    := tests/tp_logic_test.c
DRVSRCS_i2chid := drivers/mfd/intel-lpss.c drivers/mfd/intel-lpss-pci.c drivers/mfd/mfd-core.c \
  drivers/i2c/busses/i2c-designware-common.c drivers/i2c/busses/i2c-designware-master.c \
  drivers/i2c/busses/i2c-designware-platdrv.c \
  drivers/acpi/resource.c drivers/acpi/utils.c drivers/acpi/property.c \
  drivers/i2c/i2c-boardinfo.c drivers/i2c/i2c-core-base.c drivers/i2c/i2c-core-smbus.c drivers/i2c/i2c-core-acpi.c \
  drivers/pinctrl/intel/pinctrl-intel.c drivers/pinctrl/intel/pinctrl-meteorpoint.c \
  drivers/pinctrl/core.c drivers/pinctrl/pinctrl-utils.c drivers/pinctrl/pinmux.c \
  drivers/pinctrl/pinconf.c drivers/pinctrl/pinconf-generic.c \
  drivers/gpio/gpiolib.c drivers/gpio/gpiolib-devres.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c \
  drivers/hid/i2c-hid/i2c-hid-core.c drivers/hid/i2c-hid/i2c-hid-dmi-quirks.c drivers/hid/i2c-hid/i2c-hid-acpi.c \
  drivers/hid/hid-core.c drivers/hid/hid-input.c drivers/hid/hid-quirks.c drivers/hid/hid-debug.c \
  drivers/hid/hidraw.c drivers/hid/hid-multitouch.c drivers/hid/hid-generic.c \
  drivers/input/input.c drivers/input/input-compat.c drivers/input/input-mt.c \
  drivers/input/input-poller.c drivers/input/ff-core.c drivers/input/touchscreen.c
# object lists of the composite modules come from upstream's makefiles via
# tools/kbuild2mk.py (gpiolib-acpi, i2c-hid, hid, i2c-core, input-core). (satoru)
# several modules in one container: give each object's module_init alias its
# own name (upstream source untouched; the container calls them in order). (satoru)
MULTI_i2chid := 1
# C1 simulated hardware: the same upstream objects, plus the MMIO seam and
# the LPSS/PCI models. only this family compiles with -DKLD_MMIO_SIM. (satoru)
DRVSRCS_i2chidsim = $(DRVSRCS_i2chid)
BUILTIN_i2chidsim  = $(BUILTIN_i2chid)
CORELIB_i2chidsim  = $(CORELIB_i2chid)
MULTI_i2chidsim    := 1
FAMCFLAGS_i2chidsim := -DKLD_MMIO_SIM -I$(LINUX)/drivers/i2c/busses -I$(LINUX)/drivers/hid/i2c-hid $(FAMCFLAGS_i2chid)
TEST_i2chidsim     := tests/i2chid_sim.c tests/acpi_survey.c tests/i2chid_init.c tests/models/lpss_model.c tests/models/dw_i2c_model.c tests/models/pch_gpio_model.c tests/elan06fa_model.c
# ACPICA in kernel context: linux's own acpica source list for the pinned
# config, compiled with the family's CFLAGS and this worktree's KGEN headers.
# only the families that need an ACPI namespace link it; kld/acpi keeps its
# userspace copy untouched. (satoru)
ACPICA_FAMILIES := i2chidsim i2chid
ACPICA_OBJNAMES := $(shell python3 -I tools/kbuild2mk.py $(LINUX)/drivers/acpi/acpica/Makefile acpi $(KGEN)/include/generated/autoconf.h)
ACPICA_OBJS := $(if $(filter $(driver),$(ACPICA_FAMILIES)),$(addprefix $(BUILD)/core/acpica/,$(ACPICA_OBJNAMES)))
# the laptop tables are not tracked: one blob generated from the capture
# directory into build/, linked only into the ACPICA families. (satoru)
ACPI_TABLES ?= /mnt/windows/Users/ibrah/Documents/vm-lab/acpi/tables
ACPICA_BLOB_SRC := $(BUILD)/acpi_blob/acpi_tables_blob.c
ACPICA_BLOB_OBJ := $(BUILD)/acpi_blob/acpi_tables_blob.o
# objects whose option is =y in the pinned config are BUILT-IN in a real kernel:
# compiled without -DMODULE, their initcalls land in .initcall sections instead
# of an init_module alias. (satoru)
BUILTIN_i2chid := drivers/mfd/mfd-core.c drivers/i2c/i2c-core-acpi.c \
  drivers/acpi/resource.c drivers/acpi/utils.c drivers/acpi/property.c \
  drivers/i2c/i2c-core-smbus.c drivers/pinctrl/core.c drivers/pinctrl/pinmux.c drivers/pinctrl/pinconf.c \
  drivers/pinctrl/pinctrl-utils.c drivers/pinctrl/pinconf-generic.c \
  drivers/gpio/gpiolib.c drivers/gpio/gpiolib-devres.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c \
  drivers/hid/hid-core.c drivers/hid/hid-input.c drivers/hid/hid-quirks.c drivers/hid/hid-debug.c \
  drivers/hid/hidraw.c drivers/hid/hid-generic.c drivers/input/input.c drivers/input/input-compat.c \
  drivers/input/input-mt.c drivers/input/input-poller.c drivers/input/ff-core.c drivers/input/touchscreen.c

# cfg80211's shipped-certs.c is a GENERATED source in the kernel tree (its
# makefile builds it from net/wireless/certs/*.hex). reproduce it here so the
# family builds from a freshly configured tree. (satoru)
$(LINUX)/net/wireless/shipped-certs.c: $(sort $(wildcard $(LINUX)/net/wireless/certs/*.hex))
	@{ echo '#include "reg.h"'; echo 'const u8 shipped_regdb_certs[] = {'; \
	   echo | cat - $^; echo '};'; \
	   echo 'unsigned int shipped_regdb_certs_len = sizeof(shipped_regdb_certs);'; } > $@

# the BE200 (8086:272b, iwl_cfg_gl / iwl_gl_trans_cfg, device family Bz, mvm
# opmode) plus the two subsystem cores it needs. object lists are
# tools/kbuild2mk.py output for the pinned config with CONFIG_IWLWIFI=m,
# CONFIG_IWLMVM=m, CONFIG_CFG80211=m, CONFIG_MAC80211=m, debugfs/tracing off.
# LEDs are off in the pinned config, so mvm/led.c and mac80211/led.c are not
# in the list (their structs lose the led member when the option is off).
# rc80211_minstrel_ht.c is a make-variable expansion kbuild2mk.py cannot see,
# so it is added explicitly. (satoru)
DRVSRCS_iwlwifi := \
  drivers/net/wireless/intel/iwlwifi/iwl-io.c \
  drivers/net/wireless/intel/iwlwifi/iwl-drv.c \
  drivers/net/wireless/intel/iwlwifi/iwl-debug.c \
  drivers/net/wireless/intel/iwlwifi/iwl-nvm-utils.c \
  drivers/net/wireless/intel/iwlwifi/iwl-phy-db.c \
  drivers/net/wireless/intel/iwlwifi/iwl-nvm-parse.c \
  drivers/net/wireless/intel/iwlwifi/iwl-dbg-tlv.c \
  drivers/net/wireless/intel/iwlwifi/iwl-trans.c \
  drivers/net/wireless/intel/iwlwifi/pcie/drv.c \
  drivers/net/wireless/intel/iwlwifi/pcie/rx.c \
  drivers/net/wireless/intel/iwlwifi/pcie/tx.c \
  drivers/net/wireless/intel/iwlwifi/pcie/trans.c \
  drivers/net/wireless/intel/iwlwifi/pcie/ctxt-info.c \
  drivers/net/wireless/intel/iwlwifi/pcie/ctxt-info-gen3.c \
  drivers/net/wireless/intel/iwlwifi/pcie/trans-gen2.c \
  drivers/net/wireless/intel/iwlwifi/pcie/tx-gen2.c \
  drivers/net/wireless/intel/iwlwifi/cfg/7000.c \
  drivers/net/wireless/intel/iwlwifi/cfg/8000.c \
  drivers/net/wireless/intel/iwlwifi/cfg/9000.c \
  drivers/net/wireless/intel/iwlwifi/cfg/22000.c \
  drivers/net/wireless/intel/iwlwifi/cfg/ax210.c \
  drivers/net/wireless/intel/iwlwifi/cfg/bz.c \
  drivers/net/wireless/intel/iwlwifi/cfg/sc.c \
  drivers/net/wireless/intel/iwlwifi/cfg/dr.c \
  drivers/net/wireless/intel/iwlwifi/fw/img.c \
  drivers/net/wireless/intel/iwlwifi/fw/notif-wait.c \
  drivers/net/wireless/intel/iwlwifi/fw/rs.c \
  drivers/net/wireless/intel/iwlwifi/fw/dbg.c \
  drivers/net/wireless/intel/iwlwifi/fw/pnvm.c \
  drivers/net/wireless/intel/iwlwifi/fw/dump.c \
  drivers/net/wireless/intel/iwlwifi/fw/regulatory.c \
  drivers/net/wireless/intel/iwlwifi/fw/paging.c \
  drivers/net/wireless/intel/iwlwifi/fw/smem.c \
  drivers/net/wireless/intel/iwlwifi/fw/init.c \
  drivers/net/wireless/intel/iwlwifi/fw/acpi.c \
  drivers/net/wireless/intel/iwlwifi/mvm/fw.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mac80211.c \
  drivers/net/wireless/intel/iwlwifi/mvm/nvm.c \
  drivers/net/wireless/intel/iwlwifi/mvm/ops.c \
  drivers/net/wireless/intel/iwlwifi/mvm/phy-ctxt.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mac-ctxt.c \
  drivers/net/wireless/intel/iwlwifi/mvm/utils.c \
  drivers/net/wireless/intel/iwlwifi/mvm/rx.c \
  drivers/net/wireless/intel/iwlwifi/mvm/rxmq.c \
  drivers/net/wireless/intel/iwlwifi/mvm/tx.c \
  drivers/net/wireless/intel/iwlwifi/mvm/binding.c \
  drivers/net/wireless/intel/iwlwifi/mvm/quota.c \
  drivers/net/wireless/intel/iwlwifi/mvm/sta.c \
  drivers/net/wireless/intel/iwlwifi/mvm/sf.c \
  drivers/net/wireless/intel/iwlwifi/mvm/scan.c \
  drivers/net/wireless/intel/iwlwifi/mvm/time-event.c \
  drivers/net/wireless/intel/iwlwifi/mvm/rs.c \
  drivers/net/wireless/intel/iwlwifi/mvm/rs-fw.c \
  drivers/net/wireless/intel/iwlwifi/mvm/power.c \
  drivers/net/wireless/intel/iwlwifi/mvm/coex.c \
  drivers/net/wireless/intel/iwlwifi/mvm/tt.c \
  drivers/net/wireless/intel/iwlwifi/mvm/offloading.c \
  drivers/net/wireless/intel/iwlwifi/mvm/tdls.c \
  drivers/net/wireless/intel/iwlwifi/mvm/ftm-responder.c \
  drivers/net/wireless/intel/iwlwifi/mvm/ftm-initiator.c \
  drivers/net/wireless/intel/iwlwifi/mvm/rfi.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mld-key.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mld-mac.c \
  drivers/net/wireless/intel/iwlwifi/mvm/link.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mld-sta.c \
  drivers/net/wireless/intel/iwlwifi/mvm/mld-mac80211.c \
  drivers/net/wireless/intel/iwlwifi/mvm/ptp.c \
  drivers/net/wireless/intel/iwlwifi/mvm/time-sync.c \
  drivers/net/wireless/intel/iwlwifi/mvm/d3.c \
  net/mac80211/main.c \
  net/mac80211/status.c \
  net/mac80211/driver-ops.c \
  net/mac80211/sta_info.c \
  net/mac80211/wep.c \
  net/mac80211/aead_api.c \
  net/mac80211/wpa.c \
  net/mac80211/scan.c \
  net/mac80211/offchannel.c \
  net/mac80211/ht.c \
  net/mac80211/agg-tx.c \
  net/mac80211/agg-rx.c \
  net/mac80211/vht.c \
  net/mac80211/he.c \
  net/mac80211/s1g.c \
  net/mac80211/ibss.c \
  net/mac80211/iface.c \
  net/mac80211/link.c \
  net/mac80211/rate.c \
  net/mac80211/michael.c \
  net/mac80211/tkip.c \
  net/mac80211/aes_cmac.c \
  net/mac80211/aes_gmac.c \
  net/mac80211/fils_aead.c \
  net/mac80211/cfg.c \
  net/mac80211/ethtool.c \
  net/mac80211/rx.c \
  net/mac80211/spectmgmt.c \
  net/mac80211/tx.c \
  net/mac80211/key.c \
  net/mac80211/util.c \
  net/mac80211/parse.c \
  net/mac80211/wme.c \
  net/mac80211/chan.c \
  net/mac80211/trace.c \
  net/mac80211/mlme.c \
  net/mac80211/tdls.c \
  net/mac80211/ocb.c \
  net/mac80211/airtime.c \
  net/mac80211/eht.c \
  net/mac80211/pm.c \
  net/mac80211/wbrf.c \
  net/mac80211/rc80211_minstrel_ht.c \
  net/wireless/core.c \
  net/wireless/sysfs.c \
  net/wireless/radiotap.c \
  net/wireless/util.c \
  net/wireless/reg.c \
  net/wireless/scan.c \
  net/wireless/nl80211.c \
  net/wireless/mlme.c \
  net/wireless/ibss.c \
  net/wireless/sme.c \
  net/wireless/chan.c \
  net/wireless/ethtool.c \
  net/wireless/mesh.c \
  net/wireless/ap.c \
  net/wireless/trace.c \
  net/wireless/ocb.c \
  net/wireless/pmsr.c \
  net/wireless/shipped-certs.c
MULTI_iwlwifi := 1

# the mac80211/cfg80211 cores the second and third wifi families link. the
# iwlwifi family keeps its own inline copy (same files, same order). (satoru)
WIFI_CORE_SRCS := \
  net/mac80211/main.c \
  net/mac80211/status.c \
  net/mac80211/driver-ops.c \
  net/mac80211/sta_info.c \
  net/mac80211/wep.c \
  net/mac80211/aead_api.c \
  net/mac80211/wpa.c \
  net/mac80211/scan.c \
  net/mac80211/offchannel.c \
  net/mac80211/ht.c \
  net/mac80211/agg-tx.c \
  net/mac80211/agg-rx.c \
  net/mac80211/vht.c \
  net/mac80211/he.c \
  net/mac80211/s1g.c \
  net/mac80211/ibss.c \
  net/mac80211/iface.c \
  net/mac80211/link.c \
  net/mac80211/rate.c \
  net/mac80211/michael.c \
  net/mac80211/tkip.c \
  net/mac80211/aes_cmac.c \
  net/mac80211/aes_gmac.c \
  net/mac80211/fils_aead.c \
  net/mac80211/cfg.c \
  net/mac80211/ethtool.c \
  net/mac80211/rx.c \
  net/mac80211/spectmgmt.c \
  net/mac80211/tx.c \
  net/mac80211/key.c \
  net/mac80211/util.c \
  net/mac80211/parse.c \
  net/mac80211/wme.c \
  net/mac80211/chan.c \
  net/mac80211/trace.c \
  net/mac80211/mlme.c \
  net/mac80211/tdls.c \
  net/mac80211/ocb.c \
  net/mac80211/airtime.c \
  net/mac80211/eht.c \
  net/mac80211/pm.c \
  net/mac80211/wbrf.c \
  net/mac80211/rc80211_minstrel_ht.c \
  net/wireless/core.c \
  net/wireless/sysfs.c \
  net/wireless/radiotap.c \
  net/wireless/util.c \
  net/wireless/reg.c \
  net/wireless/scan.c \
  net/wireless/nl80211.c \
  net/wireless/mlme.c \
  net/wireless/ibss.c \
  net/wireless/sme.c \
  net/wireless/chan.c \
  net/wireless/ethtool.c \
  net/wireless/mesh.c \
  net/wireless/ap.c \
  net/wireless/trace.c \
  net/wireless/ocb.c \
  net/wireless/pmsr.c \
  net/wireless/shipped-certs.c

# family 2: mediatek mt7921e on mt76. object lists from the upstream makefiles
# (mt76/Makefile, mt76/mt7921/Makefile) with CONFIG_PCI=y, CONFIG_ACPI=y,
# CONFIG_PM=y and CONFIG_MT76_DEBUGFS/trace objects as the makefiles list them.
# (satoru)
DRVSRCS_mt7921e := \
  drivers/net/wireless/mediatek/mt76/mmio.c \
  drivers/net/wireless/mediatek/mt76/util.c \
  drivers/net/wireless/mediatek/mt76/trace.c \
  drivers/net/wireless/mediatek/mt76/dma.c \
  drivers/net/wireless/mediatek/mt76/mac80211.c \
  drivers/net/wireless/mediatek/mt76/debugfs.c \
  drivers/net/wireless/mediatek/mt76/eeprom.c \
  drivers/net/wireless/mediatek/mt76/tx.c \
  drivers/net/wireless/mediatek/mt76/agg-rx.c \
  drivers/net/wireless/mediatek/mt76/mcu.c \
  drivers/net/wireless/mediatek/mt76/wed.c \
  drivers/net/wireless/mediatek/mt76/pci.c \
  drivers/net/wireless/mediatek/mt76/mt76_connac_mcu.c \
  drivers/net/wireless/mediatek/mt76/mt76_connac_mac.c \
  drivers/net/wireless/mediatek/mt76/mt76_connac3_mac.c \
  drivers/net/wireless/mediatek/mt76/mt792x_core.c \
  drivers/net/wireless/mediatek/mt76/mt792x_mac.c \
  drivers/net/wireless/mediatek/mt76/mt792x_trace.c \
  drivers/net/wireless/mediatek/mt76/mt792x_debugfs.c \
  drivers/net/wireless/mediatek/mt76/mt792x_dma.c \
  drivers/net/wireless/mediatek/mt76/mt792x_acpi_sar.c \
  drivers/net/wireless/mediatek/mt76/mt7921/mac.c \
  drivers/net/wireless/mediatek/mt76/mt7921/mcu.c \
  drivers/net/wireless/mediatek/mt76/mt7921/main.c \
  drivers/net/wireless/mediatek/mt76/mt7921/init.c \
  drivers/net/wireless/mediatek/mt76/mt7921/debugfs.c \
  drivers/net/wireless/mediatek/mt76/mt7921/pci.c \
  drivers/net/wireless/mediatek/mt76/mt7921/pci_mac.c \
  drivers/net/wireless/mediatek/mt76/mt7921/pci_mcu.c \
  $(WIFI_CORE_SRCS)
FAMCFLAGS_mt7921e := -I$(LINUX)/drivers/net/wireless/mediatek/mt76 \
                     -I$(LINUX)/drivers/net/wireless/mediatek/mt76/mt7921
TEST_mt7921e := tests/mt7921e_init.c
MULTI_mt7921e := 1

# family 3: realtek rtw89 pci, 8852be and 8852ce chips. object lists from
# rtw89/Makefile with CONFIG_PM=y (wow.o) and no CONFIG_RTW89_DEBUG. (satoru)
DRVSRCS_rtw89 := \
  drivers/net/wireless/realtek/rtw89/core.c \
  drivers/net/wireless/realtek/rtw89/mac80211.c \
  drivers/net/wireless/realtek/rtw89/mac.c \
  drivers/net/wireless/realtek/rtw89/mac_be.c \
  drivers/net/wireless/realtek/rtw89/phy.c \
  drivers/net/wireless/realtek/rtw89/phy_be.c \
  drivers/net/wireless/realtek/rtw89/fw.c \
  drivers/net/wireless/realtek/rtw89/cam.c \
  drivers/net/wireless/realtek/rtw89/efuse.c \
  drivers/net/wireless/realtek/rtw89/efuse_be.c \
  drivers/net/wireless/realtek/rtw89/regd.c \
  drivers/net/wireless/realtek/rtw89/sar.c \
  drivers/net/wireless/realtek/rtw89/coex.c \
  drivers/net/wireless/realtek/rtw89/ps.c \
  drivers/net/wireless/realtek/rtw89/chan.c \
  drivers/net/wireless/realtek/rtw89/ser.c \
  drivers/net/wireless/realtek/rtw89/acpi.c \
  drivers/net/wireless/realtek/rtw89/util.c \
  drivers/net/wireless/realtek/rtw89/wow.c \
  drivers/net/wireless/realtek/rtw89/rtw8852b_common.c \
  drivers/net/wireless/realtek/rtw89/rtw8852b.c \
  drivers/net/wireless/realtek/rtw89/rtw8852b_table.c \
  drivers/net/wireless/realtek/rtw89/rtw8852b_rfk.c \
  drivers/net/wireless/realtek/rtw89/rtw8852b_rfk_table.c \
  drivers/net/wireless/realtek/rtw89/rtw8852be.c \
  drivers/net/wireless/realtek/rtw89/rtw8852c.c \
  drivers/net/wireless/realtek/rtw89/rtw8852c_table.c \
  drivers/net/wireless/realtek/rtw89/rtw8852c_rfk.c \
  drivers/net/wireless/realtek/rtw89/rtw8852c_rfk_table.c \
  drivers/net/wireless/realtek/rtw89/rtw8852ce.c \
  drivers/net/wireless/realtek/rtw89/pci.c \
  drivers/net/wireless/realtek/rtw89/pci_be.c \
  $(WIFI_CORE_SRCS)
FAMCFLAGS_rtw89 := -I$(LINUX)/drivers/net/wireless/realtek/rtw89
TEST_rtw89 := tests/rtw89_init.c
MULTI_rtw89 := 1

# family 4: drm (d0 survey, d1 bring-up). the upstream drm core, the kms and
# shmem helpers, the dma-buf/sync_file subsystem and the tiny cirrus driver,
# all unmodified. the object lists are generated by tools/kbuild2mk.py from
# upstream's own makefiles against this worktree's kgen config - no hand-typed
# list can drift from what the real build compiles. cirrus.o and the dma-buf
# objects come from the obj-y / obj-$(CONFIG) forms via the "obj" module name.
# (satoru)
DRM_DRM_OBJS   := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile drm $(KGEN)/include/generated/autoconf.h)
DRM_KMS_OBJS   := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile drm_kms_helper $(KGEN)/include/generated/autoconf.h)
DRM_SHMEM_OBJS := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile drm_shmem_helper $(KGEN)/include/generated/autoconf.h)
DRM_TINY_OBJS  := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/tiny/Makefile obj $(KGEN)/include/generated/autoconf.h)
DRM_QUIRK_OBJS := $(filter drm_panel_orientation_quirks.o,$(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile obj $(KGEN)/include/generated/autoconf.h))
DMABUF_OBJS    := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/dma-buf/Makefile obj $(KGEN)/include/generated/autoconf.h)
DRVSRCS_drmcirrus := $(addprefix drivers/gpu/drm/,$(patsubst %.o,%.c,$(DRM_DRM_OBJS) $(DRM_KMS_OBJS) $(DRM_SHMEM_OBJS) $(DRM_QUIRK_OBJS))) \
  $(addprefix drivers/gpu/drm/tiny/,$(patsubst %.o,%.c,$(DRM_TINY_OBJS))) \
  $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
# dma-buf and sync_file are obj-y (built-in) in the pinned config, and
# dma-buf.c uses __exitcall, which upstream only defines without -DMODULE.
# compile that subsystem exactly like the real built-in build; cirrus itself
# keeps -DMODULE so its module_init produces the container entry alias. (satoru)
BUILTIN_drmcirrus := $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
# several modules in one container: each object's module_init alias gets its
# own name, the harness calls the driver's entry. (satoru)
MULTI_drmcirrus := 1
# the legacy VGA port override in shim-include/asm/io.h is opt-in per family:
# it must not change what the network or touchpad containers compile. (satoru)
FAMCFLAGS_drmcirrus := -DKLD_DRM_PORTIO
TEST_drmcirrus := tests/drmcirrus_init.c
DRMCLIENT_drmcirrus := drm/kdrm_client.c drm/kdrm_uapi.c
# the drm userspace test program is ordinary musl code (satoru)
APPTEST_drmcirrus := tests/dri_app.c

# family 5: bochs (d3). the same unmodified drm core/kms helpers, plus the
# ttm core, drm_gem_vram_helper and drm_gem_ttm_helper the tiny bochs driver
# selects, and bochs itself. object lists from upstream's makefiles. (satoru)
DRM_VRAM_OBJS       := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile drm_vram_helper $(KGEN)/include/generated/autoconf.h)
DRM_TTM_HELPER_OBJS := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/Makefile drm_ttm_helper $(KGEN)/include/generated/autoconf.h)
DRM_TTM_OBJS        := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/ttm/Makefile ttm $(KGEN)/include/generated/autoconf.h)
DRVSRCS_bochs := $(addprefix drivers/gpu/drm/,$(patsubst %.o,%.c,$(DRM_DRM_OBJS) $(DRM_KMS_OBJS) $(DRM_QUIRK_OBJS) $(DRM_VRAM_OBJS) $(DRM_TTM_HELPER_OBJS))) \
  $(addprefix drivers/gpu/drm/ttm/,$(patsubst %.o,%.c,$(DRM_TTM_OBJS))) \
  drivers/gpu/drm/tiny/bochs.c \
  $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
BUILTIN_bochs := $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
MULTI_bochs := 1
# family 6: i915 (d4, display only). its own config snapshot (config/i915.config
# -> build/kgen-i915). drm core + kms + display helpers + buddy + ttm + the
# whole upstream i915 module (i915-y, gt-y, gem-y lists from its makefile),
# unmodified. (satoru)
ifeq ($(driver),i915)
I915_OBJS := $(sort $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/i915/Makefile i915 $(KGEN)/include/generated/autoconf.h) \
                    $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/i915/Makefile gt $(KGEN)/include/generated/autoconf.h) \
                    $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/i915/Makefile gem $(KGEN)/include/generated/autoconf.h))
DRM_DISP_OBJS := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/display/Makefile drm_display_helper $(KGEN)/include/generated/autoconf.h)
DRM_TTM_OBJS_I := $(shell python3 tools/kbuild2mk.py $(LINUX)/drivers/gpu/drm/ttm/Makefile ttm $(KGEN)/include/generated/autoconf.h)
endif
DRVSRCS_i915 := $(addprefix drivers/gpu/drm/,$(patsubst %.o,%.c,$(DRM_DRM_OBJS) $(DRM_KMS_OBJS) $(DRM_QUIRK_OBJS) drm_buddy.o drm_mipi_dsi.o)) \
  $(addprefix drivers/gpu/drm/display/,$(patsubst %.o,%.c,$(DRM_DISP_OBJS))) \
  $(addprefix drivers/gpu/drm/ttm/,$(patsubst %.o,%.c,$(DRM_TTM_OBJS_I))) \
  $(addprefix drivers/gpu/drm/i915/,$(patsubst %.o,%.c,$(filter-out $$%,$(I915_OBJS)))) \
  $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
BUILTIN_i915 := $(addprefix drivers/dma-buf/,$(patsubst %.o,%.c,$(DMABUF_OBJS)))
MULTI_i915 := 1
TEST_i915 := tests/i915_init.c
DRMCLIENT_i915 := drm/kdrm_client.c drm/i915_model.c drm/i915_model_regs.c drm/i915_model_tables.c drm/i915_model_aux.c drm/i915_model_phy.c drm/i915_model_irq.c
# KLD_MMIO_SIM: every driver mmio access goes through lx_mmio_read/write
# (kld/drm/i915_model.c), a plain accessor unless argv says model:. (satoru)
# KLD_DRM_PORTIO: i915 touches the legacy vga ports (intel_vga.c); the broker
# refuses them for a family without legacy_vga 1 (in reads 0xff, out is
# dropped), instead of a #GP in ring 3. (satoru)
FAMCFLAGS_i915 := -fno-pie -DI915 -I$(LINUX)/drivers/gpu/drm/i915 -DKLD_MMIO_SIM -DKLD_MMIO_SIM_IOMAP -DKLD_DRM_PORTIO

# ttm_pool.c's inline asm needs absolute addresses ("i" constraints): this
# family's objects are built -fno-pie (the toolchain defaults to pie); the
# other families keep the default. (satoru)
FAMCFLAGS_bochs := -fno-pie
TEST_bochs := tests/bochs_init.c
DRMCLIENT_bochs := drm/kdrm_client.c drm/kdrm_uapi.c
APPTEST_bochs := tests/dri_app.c


ifneq ($(DRVSRCS_$(driver)),)
DRV_SRCS  := $(addprefix $(LINUX)/,$(DRVSRCS_$(driver)))
DRV_OBJS  := $(patsubst $(LINUX)/%.c,$(BUILD)/driver/%.o,$(DRV_SRCS))
DRVDIR    := $(LINUX)
else
ifeq ($(DRVDIR),)
ifneq ($(TEST_$(driver)),)
DRV_SRCS  :=
DRV_OBJS  :=
DRVDIR    := $(LINUX)
else
$(error unknown driver '$(driver)' - add DRVDIR_$(driver) or DRVSRCS_$(driver) to this Makefile)
endif
else
DRV_SRCS  := $(wildcard $(DRVDIR)/*.c)
DRV_OBJS  := $(patsubst $(DRVDIR)/%.c,$(BUILD)/driver/%.o,$(DRV_SRCS))
endif
endif

# IMPORTED UPSTREAM CORES: unmodified linux .c files the family links in
# instead of a shim rewrite (paths relative to the vendored tree). they are
# compiled exactly like driver files and counted in their own census column.
# (satoru)
# the shim itself (lx_net.c) uses BQL, so that core is in every container. (satoru)
CORES_ALL    := lib/dynamic_queue_limits.c arch/x86/lib/hweight.S
CORES_e1000e :=

# CORE LIBRARY: unmodified upstream files linked into the families that need
# them. paths are relative to LINUX, exactly like the driver list; no copy of
# an upstream file lives in this repo. (satoru)
CORELIB_COMMON := lib/ctype.c lib/find_bit.c lib/bitmap.c lib/bsearch.c \
                  lib/sort.c lib/math/gcd.c lib/kstrtox.c lib/string_helpers.c \
                  lib/crc32.c lib/rbtree.c lib/scatterlist.c net/ethernet/eth.c
CORELIB_STORE  := lib/xarray.c lib/idr.c lib/radix-tree.c lib/kfifo.c \
                  lib/kobject.c lib/klist.c kernel/notifier.c \
                  drivers/base/devres.c
CORELIB_e1000e := lib/find_bit.c lib/bitmap.c lib/crc32.c lib/scatterlist.c net/ethernet/eth.c lib/nlattr.c
CORELIB_i2chid := $(CORELIB_COMMON) lib/nlattr.c $(CORELIB_STORE)
CORELIB_iwlwifi := $(CORELIB_COMMON) lib/rhashtable.c lib/nlattr.c $(CORELIB_STORE)
CORELIB_mt7921e := $(CORELIB_iwlwifi)
CORELIB_rtw89  := $(CORELIB_iwlwifi)
CORELIB_apitest := $(CORELIB_i2chid) lib/rhashtable.c lib/nlattr.c
CORELIB_hidsim  := $(CORELIB_apitest) kernel/irq/irq_sim.c
CORELIB_tplogic := $(CORELIB_apitest)
# drm family: the unmodified upstream helpers the drm core and cirrus call.
# lib/string.c is deliberately NOT here (it defines strcpy/strcmp, which musl
# owns in this container); the few string helpers the drm core needs are
# hand-written in lx_drm_misc.c. (satoru)
CORELIB_drmcirrus := lib/find_bit.c lib/bitmap.c lib/bsearch.c lib/sort.c \
                     lib/list_sort.c lib/llist.c lib/ctype.c \
                     lib/kstrtox.c lib/string_helpers.c lib/rbtree.c \
                     lib/scatterlist.c lib/xarray.c lib/idr.c lib/radix-tree.c \
                     lib/seq_buf.c fs/seq_file.c lib/kobject.c lib/klist.c \
                     lib/nlattr.c drivers/base/devres.c lib/devres.c \
                     drivers/base/component.c drivers/video/hdmi.c \
                     drivers/i2c/i2c-core-base.c
CORELIB_bochs := $(CORELIB_drmcirrus)
CORELIB_i915 := $(CORELIB_drmcirrus) lib/interval_tree.c drivers/i2c/algos/i2c-algo-bit.c

# WPA2 supplicant library: pure logic plus the six kernel-crypto bindings.
# linked into the families that install keys (apitest, iwlwifi). (satoru)
WPA_apitest := wifi/wpa_psk.c wifi/wpa_psk_crypto_kernel.c
WPA_iwlwifi := wifi/wpa_psk.c wifi/wpa_psk_crypto_kernel.c
WPA_SRCS := $(WPA_$(driver))

# CRYPTO LIBRARY: the unmodified kernel crypto API core, the generic ciphers
# and hashes it needs, and the templates mac80211 instantiates by name
# ("ccm(aes)", "gcm(aes)", "cmac(aes)", "hmac(sha1)", "hmac(sha256)",
# "ctr(aes)"). only the families that install keys link it. (satoru)
CRYPTOLIB_COMMON := crypto/api.c crypto/algapi.c crypto/cipher.c crypto/aead.c \
                    crypto/shash.c crypto/ahash.c crypto/skcipher.c crypto/lskcipher.c \
                    crypto/scatterwalk.c crypto/algboss.c \
                    crypto/aes_generic.c crypto/ccm.c crypto/gcm.c \
                    crypto/ghash-generic.c crypto/ctr.c crypto/cbc.c \
                    crypto/cmac.c crypto/hmac.c crypto/sha1_generic.c \
                    crypto/sha256_generic.c crypto/crypto_null.c \
                    lib/crypto/aes.c lib/crypto/gf128mul.c lib/crypto/utils.c \
                    lib/crypto/memneq.c lib/crypto/arc4.c \
                    lib/crypto/sha1.c lib/crypto/sha256.c lib/ctype.c
CRYPTOLIB_iwlwifi := $(CRYPTOLIB_COMMON)
CRYPTOLIB_apitest := $(CRYPTOLIB_COMMON)
CRYPTOLIB := $(CRYPTOLIB_$(driver))

CORE_SRCS := $(addprefix $(LINUX)/,$(sort $(CORES_ALL) $(CORES_$(driver)) $(CORELIB_$(driver)) $(CRYPTOLIB)))
CORE_OBJS := $(patsubst $(LINUX)/%.c,$(BUILD)/core/%.o,$(patsubst $(LINUX)/%.S,$(BUILD)/core/%.o,$(CORE_SRCS)))
# imported acpica objects are counted with the cores: their undefined symbols
# need providers too, and their definitions provide for the drivers. (satoru)
CORE_OBJS += $(ACPICA_OBJS)

# Two classes of shim source, and the distinction matters:
#   lx_emul/*.c        userspace context - no kernel headers. the container
#                      plumbing (entry point, logging, the abort trap).
#   lx_emul/kimpl/*.c  KERNEL context - compiled with the same CFLAGS and the
#                      same Linux headers as the driver, so the COMPILER
#                      enforces every prototype. Implementing a shim function
#                      with a subtly wrong signature is a silent-corruption bug
#                      that no amount of care catches by hand; this makes it a
#                      build error instead. (satoru)
EMUL_SRCS  := $(wildcard lx_emul/*.c)
EMUL_OBJS  := $(patsubst lx_emul/%.c,$(BUILD)/emul/%.o,$(EMUL_SRCS))
KIMPL_SRCS := $(wildcard lx_emul/kimpl/*.c)
# the crypto binding only exists for the families that link CRYPTOLIB; the
# other containers must not grow its references. (satoru)
ifeq ($(CRYPTOLIB),)
KIMPL_SRCS := $(filter-out $(wildcard lx_emul/kimpl/lx_crypto*.c),$(KIMPL_SRCS))
endif
# the acpica binding only exists for the families that link ACPICA; the other
# containers must not grow its references. (satoru)
ifeq ($(filter $(driver),$(ACPICA_FAMILIES)),)
KIMPL_SRCS := $(filter-out $(wildcard lx_emul/kimpl/lx_acpi*.c),$(KIMPL_SRCS))
endif
# the drm shim (lx_drm_*) is family-specific: other containers must not grow
# its references or its parse helpers. (satoru)
ifneq ($(filter $(driver),drmcirrus bochs i915),$(driver))
KIMPL_SRCS := $(filter-out $(wildcard lx_emul/kimpl/lx_drm_*.c),$(KIMPL_SRCS))
endif
# the i915 family has its own mmio seam with the register model
# (kld/drm/i915_model.c, it logs the calling driver function); the touchpad's
# claimed-range seam is not linked there. (satoru)
ifeq ($(driver),i915)
KIMPL_SRCS := $(filter-out lx_emul/kimpl/lx_mmio_sim.c,$(KIMPL_SRCS))
endif
KIMPL_OBJS := $(patsubst lx_emul/kimpl/%.c,$(BUILD)/emul/kimpl_%.o,$(KIMPL_SRCS))
EMUL_OBJS  += $(KIMPL_OBJS)
# a test harness is kernel-context code too (checked prototypes). (satoru)
TEST_SRCS  := $(TEST_$(driver))
TEST_OBJS  := $(patsubst tests/%.c,$(BUILD)/emul/test_%.o,$(TEST_SRCS))
EMUL_OBJS  += $(TEST_OBJS)
WPA_OBJS   := $(patsubst wifi/%.c,$(BUILD)/emul/wifi_%.o,$(WPA_SRCS))
EMUL_OBJS  += $(WPA_OBJS)
TP_LOGIC_OBJ := $(BUILD)/input/tp_logic.o
EMUL_OBJS  += $(TP_LOGIC_OBJ)
$(TP_LOGIC_OBJ): input/tp_logic.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(EMUL_CFLAGS) -I$(CURDIR) -c -o $@ $<
# the generated ACPI table blob belongs to the SIMULATED family only: the real
# i2chid family reads the firmware tables through the kernel broker. (satoru)
ifeq ($(driver),i2chidsim)
EMUL_OBJS  += $(ACPICA_BLOB_OBJ)
endif
# in-container drm client (kernel context) is family code, not a test. (satoru)
DRMCLIENT_SRCS := $(DRMCLIENT_$(driver))
DRMCLIENT_OBJS := $(patsubst drm/%.c,$(BUILD)/emul/drm_%.o,$(DRMCLIENT_SRCS))
EMUL_OBJS  += $(DRMCLIENT_OBJS)
APPTEST_OBJS := $(patsubst tests/%.c,$(BUILD)/emul/app_%.o,$(APPTEST_$(driver)))
EMUL_OBJS  += $(APPTEST_OBJS)

# ── include order matters, so copy Kbuild's exactly ─────────────────────────
# This is LINUXINCLUDE + USERINCLUDE from the upstream Makefile, verbatim in
# order. Getting this wrong produces "asm/types.h not found" and similar, because
# the arch's generated/ wrapper dirs must precede the generic ones. Our own
# shim-include comes FIRST so a hand-written override can win. (satoru)
INCS := -Ilx_emul \
        -Ilx_emul/shim-include \
        -I$(KGEN)/arch/x86/include/generated \
        -I$(LINUX)/arch/x86/include \
        -I$(KGEN)/include \
        -I$(LINUX)/include \
        -I$(LINUX)/arch/x86/include/uapi \
        -I$(KGEN)/arch/x86/include/generated/uapi \
        -I$(LINUX)/include/uapi \
        -I$(KGEN)/include/generated/uapi \
        -I$(DRVDIR)

# -DMODULE is load-bearing: upstream's module_init() then expands to an
# init_module() alias of the driver's init function, which is exactly the entry
# point the container calls. No macro surgery needed. (satoru)
KDEFS := -D__KERNEL__ -DMODULE -DKBUILD_MODNAME='"$(driver)"' \
         -DCONFIG_X86_64 -DCONFIG_64BIT

# freestanding-ish: kernel code must not pull musl's headers for things it
# defines itself. -fno-strict-aliasing matches the kernel's own build. (satoru)
# Kbuild force-includes kconfig.h into every kernel TU; it pulls in
# generated/autoconf.h. Matching that exactly means the driver sees the same
# CONFIG_* view it would in-tree. (satoru)
CFLAGS := -std=gnu11 -O2 -g -fno-strict-aliasing -fno-common \
          -ffreestanding -fno-stack-protector \
          -include $(LINUX)/include/linux/kconfig.h \
          -include $(LINUX)/include/linux/compiler_types.h \
          -include $(CURDIR)/lx_emul/kimpl/lx_undef.h \
          -Wno-unused -Wno-implicit-function-declaration \
          -Wno-builtin-declaration-mismatch -Wno-incompatible-pointer-types \
          -Wno-int-conversion -Wno-discarded-qualifiers \
          $(KDEFS) $(INCS)

# per-family flags mirroring the upstream makefiles: iwlwifi adds -I$(src) for
# its own headers and mvm adds -I$(src)/../; mac80211 is built with -DDEBUG. (satoru)
FAMCFLAGS_iwlwifi := -I$(LINUX)/drivers/net/wireless/intel/iwlwifi \
                     -I$(LINUX)/drivers/net/wireless/intel/iwlwifi/mvm
FAMCFLAGS := $(FAMCFLAGS_$(driver))

# aggregate-session counter in tests/wifi_ctl.c. (satoru)
LDWRAP_iwlwifi := -Wl,--wrap=ieee80211_start_tx_ba_session -Wl,--wrap=ieee80211_stop_rx_ba_session

# the container itself is ordinary userspace code. (satoru)
EMUL_CFLAGS := -std=gnu11 -O2 -g -Wall -Ilx_emul \
               -DKLD_DRIVER_NAME='"$(driver)"' \
               -DKLD_KERNEL_VERSION='"$(KVER)"'

CONTAINER := $(BUILD)/kld-$(driver).elf

# ── generated kernel match table ───────────────────────────────────────────
# every built family's container is run on the HOST with --kld-ids, which
# prints the pci id table the driver itself registered; the result is written
# to build/kld_match_table.cpp for the kernel to embed. families without a
# kld/families/<family>.manifest are test containers and are ignored. not
# checked in. (satoru)
.PHONY: match-table
match-table: $(CONTAINER)
	@bash tools/gen_match_table.sh $(abspath build)
all: match-table

.PHONY: all census clean check-tree
.DEFAULT_GOAL := all
all: $(CONTAINER)

check-tree:
	@test -d $(LINUX)/include/linux || { \
	  echo "KLD: vendored tree missing: $(LINUX)"; \
	  echo "KLD: run tools/fetch_linux.sh $(KVER)"; exit 1; }
	@test -d $(DRVDIR) || { echo "KLD: driver sources missing: $(DRVDIR)"; exit 1; }

$(BUILD)/emul/app_%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

$(BUILD)/emul/test_%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) $(FAMCFLAGS) -I$(CURDIR) -c -o $@ $<

MODNAME_OF = $(subst -,_,$(basename $(notdir $(1))))
# iwlwifi's pcie/drv.c declares iwl_dev_info_table (the device -> full cfg
# mapping the probe uses) as static unless CONFIG_IWLWIFI_KUNIT_TESTS is set.
# --kld-fw needs that table to resolve the cards whose id entry carries only a
# trans cfg, so this ONE object is compiled with the kunit visibility switch.
# the source is untouched and no other translation unit sees the macro.
# (satoru)
IWL_KUNIT_VIS = $(if $(filter drivers/net/wireless/intel/iwlwifi/pcie/drv.c,$(patsubst $(LINUX)/%,%,$<)),-DCONFIG_IWLWIFI_KUNIT_TESTS=1)

$(BUILD)/driver/%.o: $(DRVDIR)/%.c | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(if $(filter $(patsubst $(LINUX)/%,%,$<),$(BUILTIN_$(driver))),$(filter-out -DMODULE,$(CFLAGS)),$(CFLAGS)) $(IWL_KUNIT_VIS) $(if $(filter iwlwifi mt7921e rtw89,$(driver)),-I$(dir $<)) $(FAMCFLAGS) \
	  $(if $(filter net/mac80211/%,$(patsubst $(LINUX)/%,%,$<)),-DDEBUG) \
	  -DKBUILD_BASENAME='"$(call MODNAME_OF,$<)"' -DKBUILD_MODFILE='"$(basename $(patsubst $(LINUX)/%,%,$<))"' \
	  $(if $(MULTI_$(driver)),-Dinit_module=kld_init_$(call MODNAME_OF,$<) -Dcleanup_module=kld_exit_$(call MODNAME_OF,$<)) -c -o $@ $<

$(BUILD)/core/%.o: $(LINUX)/%.c | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) $(FAMCFLAGS) \
	  -Dinit_module=kld_init_$(call MODNAME_OF,$<) \
	  -Dcleanup_module=kld_exit_$(call MODNAME_OF,$<) \
	  -c -o $@ $<

# upstream assembly cores (arch/x86/lib/hweight.S) are assembled unmodified. (satoru)
$(BUILD)/core/%.o: $(LINUX)/%.S | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -D__ASSEMBLY__ -c -o $@ $<

# acpica in kernel context: upstream's own ccflags and its private headers,
# otherwise exactly the family's kernel CFLAGS and KGEN headers. (satoru)
$(BUILD)/core/acpica/%.o: $(LINUX)/drivers/acpi/acpica/%.c | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -D_LINUX -DBUILDING_ACPICA -I$(LINUX)/drivers/acpi/acpica \
	  -Wno-stringop-truncation -c -o $@ $<

$(BUILD)/emul/%.o: lx_emul/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

# the wpa library is kernel-context code (checked prototypes), but it is
# independent of any driver family. (satoru)
$(BUILD)/emul/wifi_%.o: wifi/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

# drm client: kernel-context, includes the family headers by kld-relative path. (satoru)
$(BUILD)/emul/drm_%.o: drm/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -I$(CURDIR) -c -o $@ $<

# e1000e does not link lib/kobject.c, so its lx_device keeps the local
# kobject_init/kobject_set_name_vargs; the other families use upstream. (satoru)
KOBJECT_DEFS := $(if $(filter e1000e,$(driver)),-DKLD_KOBJECT_LOCAL)

# the ACPICA families replace the pre-ACPI fake bindings in lx_misc/lx_bus. (satoru)
KIMPL_ACPI_DEFS := $(if $(filter $(driver),$(ACPICA_FAMILIES)),-DKLD_ACPI_BUILD,)

# kernel-context shim: same flags as the driver, so prototypes are checked. (satoru)
$(BUILD)/emul/kimpl_%.o: lx_emul/kimpl/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) $(FAMCFLAGS) -I$(LINUX) $(KIMPL_ACPI_DEFS) $(KOBJECT_DEFS) \
	  $(if $(filter drmcirrus bochs i915,$(driver)),-DKLD_DRM_OWNS_MODULE_OPS,) \
	  $(if $(filter bochs,$(driver)),-DKLD_PAGE_CENSUS,) -c -o $@ $<

# the acpica binding: aclinux.h selects the kernel branch from __KERNEL__ plus
# these defines, and the namespace internals live next to the acpica objects. (satoru)
$(BUILD)/emul/kimpl_lx_acpi_%.o: lx_emul/kimpl/lx_acpi_%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -I$(LINUX) -D_LINUX -DBUILDING_ACPICA \
	  -I$(LINUX)/drivers/acpi/acpica $(KOBJECT_DEFS) -c -o $@ $<

# the laptop table blob: generated, ignored, and only in the ACPICA families. (satoru)
$(ACPICA_BLOB_SRC): tools/gen_acpi_blob.py
	@mkdir -p $(dir $@)
	@python3 -I tools/gen_acpi_blob.py --tables "$(ACPI_TABLES)" --out "$@" || \
	  { echo "KLD: ACPI tables missing: set ACPI_TABLES=<capture dir> and rebuild" >&2; \
	    rm -f "$@"; exit 1; }

$(ACPICA_BLOB_OBJ): $(ACPICA_BLOB_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

# generated stubs depend on both halves being compiled: we can only know what
# is missing once we can see what the driver needs and what we provide. (satoru)
$(BUILD)/lx_stubs_gen.c: $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS)
	@mkdir -p $(dir $@)
	# no family is lenient any more: the drm families got their data and
	# address-taken providers in turn 5 (lx_drm_addrfn.c, lx_drm_kapi.c). (satoru)
	bash tools/gen_stubs.sh $(BUILD) $@

$(BUILD)/emul/lx_stubs_gen.o: $(BUILD)/lx_stubs_gen.c
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

$(CONTAINER): $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS) $(BUILD)/emul/lx_stubs_gen.o
	$(MUSLCC) -static -no-pie -pthread $(LDWRAP_$(driver)) -Wl,-T,$(CURDIR)/lx_emul/lx_initcalls.ld -o $@ $^
	@echo "KLD: built $@"
	@bash tools/symcensus.sh $(BUILD)
	@cp $(KGEN)/config.sha256 $(BUILD)/config.sha256

census: $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS)
	@bash tools/gen_stubs.sh $(BUILD) $(BUILD)/lx_stubs_gen.c >/dev/null
	@bash tools/symcensus.sh $(BUILD)

clean:
	rm -rf build
