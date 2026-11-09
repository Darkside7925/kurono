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
MUSLCC    ?= musl-gcc

# where each supported driver's sources live inside the vendored tree. (satoru)
DRVDIR_e1000e := $(LINUX)/drivers/net/ethernet/intel/e1000e
DRVDIR        := $(DRVDIR_$(driver))

# explicit source lists (paths inside the vendored tree) for families and
# tests, instead of a directory wildcard. (satoru)
DRVSRCS_hidparse := drivers/hid/hid-core.c drivers/hid/hid-quirks.c drivers/hid/hid-debug.c
TEST_hidparse    := tests/hidparse.c
# the laptop touchpad stack (ELAN06FA, i2c-hid over LPSS i2c 80:15.1, GpioInt on
# INTC1084), bottom to top. needs the fragment-configured headers:
#   make driver=i2chid LINUX=../third_party/linux-6.12.99-kld   (satoru)
DRVSRCS_i2chid := drivers/mfd/intel-lpss.c drivers/mfd/intel-lpss-pci.c drivers/mfd/mfd-core.c \
  drivers/i2c/busses/i2c-designware-common.c drivers/i2c/busses/i2c-designware-master.c \
  drivers/i2c/busses/i2c-designware-platdrv.c \
  drivers/i2c/i2c-core-base.c drivers/i2c/i2c-core-smbus.c drivers/i2c/i2c-core-acpi.c \
  drivers/pinctrl/intel/pinctrl-intel.c drivers/pinctrl/intel/pinctrl-meteorpoint.c \
  drivers/pinctrl/core.c drivers/pinctrl/pinctrl-utils.c drivers/pinctrl/pinmux.c \
  drivers/pinctrl/pinconf.c drivers/pinctrl/pinconf-generic.c \
  drivers/gpio/gpiolib.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c \
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
# objects whose option is =y in the pinned config are BUILT-IN in a real kernel:
# compiled without -DMODULE, their initcalls land in .initcall sections instead
# of an init_module alias. (satoru)
BUILTIN_i2chid := drivers/mfd/mfd-core.c drivers/i2c/i2c-core-base.c drivers/i2c/i2c-core-acpi.c \
  drivers/i2c/i2c-core-smbus.c drivers/pinctrl/core.c drivers/pinctrl/pinmux.c drivers/pinctrl/pinconf.c \
  drivers/pinctrl/pinctrl-utils.c drivers/pinctrl/pinconf-generic.c \
  drivers/gpio/gpiolib.c drivers/gpio/gpiolib-acpi-core.c drivers/gpio/gpiolib-acpi-quirks.c \
  drivers/hid/hid-core.c drivers/hid/hid-input.c drivers/hid/hid-quirks.c drivers/hid/hid-debug.c \
  drivers/hid/hidraw.c drivers/hid/hid-generic.c drivers/input/input.c drivers/input/input-compat.c \
  drivers/input/input-mt.c drivers/input/input-poller.c drivers/input/ff-core.c drivers/input/touchscreen.c

ifneq ($(DRVSRCS_$(driver)),)
DRV_SRCS  := $(addprefix $(LINUX)/,$(DRVSRCS_$(driver)))
DRV_OBJS  := $(patsubst $(LINUX)/%.c,$(BUILD)/driver/%.o,$(DRV_SRCS))
DRVDIR    := $(LINUX)
else
ifeq ($(DRVDIR),)
$(error unknown driver '$(driver)' - add DRVDIR_$(driver) or DRVSRCS_$(driver) to this Makefile)
endif
DRV_SRCS  := $(wildcard $(DRVDIR)/*.c)
DRV_OBJS  := $(patsubst $(DRVDIR)/%.c,$(BUILD)/driver/%.o,$(DRV_SRCS))
endif

# IMPORTED UPSTREAM CORES: unmodified linux .c files the family links in
# instead of a shim rewrite (paths relative to the vendored tree). they are
# compiled exactly like driver files and counted in their own census column.
# (satoru)
# the shim itself (lx_net.c) uses BQL, so that core is in every container. (satoru)
CORES_ALL    := lib/dynamic_queue_limits.c
CORES_e1000e :=
CORE_SRCS := $(addprefix $(LINUX)/,$(sort $(CORES_ALL) $(CORES_$(driver))))
CORE_OBJS := $(patsubst $(LINUX)/%.c,$(BUILD)/core/%.o,$(CORE_SRCS))

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
KIMPL_OBJS := $(patsubst lx_emul/kimpl/%.c,$(BUILD)/emul/kimpl_%.o,$(KIMPL_SRCS))
EMUL_OBJS  += $(KIMPL_OBJS)
# a test harness is kernel-context code too (checked prototypes). (satoru)
TEST_SRCS  := $(TEST_$(driver))
TEST_OBJS  := $(patsubst tests/%.c,$(BUILD)/emul/test_%.o,$(TEST_SRCS))
EMUL_OBJS  += $(TEST_OBJS)

# ── include order matters, so copy Kbuild's exactly ─────────────────────────
# This is LINUXINCLUDE + USERINCLUDE from the upstream Makefile, verbatim in
# order. Getting this wrong produces "asm/types.h not found" and similar, because
# the arch's generated/ wrapper dirs must precede the generic ones. Our own
# shim-include comes FIRST so a hand-written override can win. (satoru)
INCS := -Ilx_emul \
        -Ilx_emul/shim-include \
        -I$(LINUX)/arch/x86/include \
        -I$(LINUX)/arch/x86/include/generated \
        -I$(LINUX)/include \
        -I$(LINUX)/arch/x86/include/uapi \
        -I$(LINUX)/arch/x86/include/generated/uapi \
        -I$(LINUX)/include/uapi \
        -I$(LINUX)/include/generated/uapi \
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
CFLAGS := -std=gnu11 -O1 -g -fno-strict-aliasing -fno-common \
          -ffreestanding -fno-stack-protector \
          -include $(LINUX)/include/linux/kconfig.h \
          -Wno-unused -Wno-implicit-function-declaration \
          -Wno-builtin-declaration-mismatch -Wno-incompatible-pointer-types \
          -Wno-int-conversion -Wno-discarded-qualifiers \
          $(KDEFS) $(INCS)

# the container itself is ordinary userspace code. (satoru)
EMUL_CFLAGS := -std=gnu11 -O2 -g -Wall -Ilx_emul \
               -DKLD_DRIVER_NAME='"$(driver)"' \
               -DKLD_KERNEL_VERSION='"$(KVER)"'

CONTAINER := $(BUILD)/kld-$(driver).elf

.PHONY: all census clean check-tree
all: $(CONTAINER)

check-tree:
	@test -d $(LINUX)/include/linux || { \
	  echo "KLD: vendored tree missing: $(LINUX)"; \
	  echo "KLD: run tools/fetch_linux.sh $(KVER)"; exit 1; }
	@test -d $(DRVDIR) || { echo "KLD: driver sources missing: $(DRVDIR)"; exit 1; }

$(BUILD)/emul/test_%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

MODNAME_OF = $(subst -,_,$(basename $(notdir $(1))))
$(BUILD)/driver/%.o: $(DRVDIR)/%.c | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(if $(filter $(patsubst $(LINUX)/%,%,$<),$(BUILTIN_$(driver))),$(filter-out -DMODULE,$(CFLAGS)),$(CFLAGS)) \
	  -DKBUILD_BASENAME='"$(call MODNAME_OF,$<)"' -DKBUILD_MODFILE='"$(basename $(patsubst $(LINUX)/%,%,$<))"' \
	  $(if $(MULTI_$(driver)),-Dinit_module=kld_init_$(call MODNAME_OF,$<) -Dcleanup_module=kld_exit_$(call MODNAME_OF,$<)) -c -o $@ $<

$(BUILD)/core/%.o: $(LINUX)/%.c | check-tree
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

$(BUILD)/emul/%.o: lx_emul/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

# kernel-context shim: same flags as the driver, so prototypes are checked. (satoru)
$(BUILD)/emul/kimpl_%.o: lx_emul/kimpl/%.c
	@mkdir -p $(dir $@)
	$(MUSLCC) $(CFLAGS) -c -o $@ $<

# generated stubs depend on both halves being compiled: we can only know what
# is missing once we can see what the driver needs and what we provide. (satoru)
$(BUILD)/lx_stubs_gen.c: $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS)
	@mkdir -p $(dir $@)
	bash tools/gen_stubs.sh $(BUILD) $@

$(BUILD)/emul/lx_stubs_gen.o: $(BUILD)/lx_stubs_gen.c
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

$(CONTAINER): $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS) $(BUILD)/emul/lx_stubs_gen.o
	$(MUSLCC) -static -no-pie -pthread -o $@ $^
	@echo "KLD: built $@"
	@bash tools/symcensus.sh $(BUILD)

census: $(DRV_OBJS) $(CORE_OBJS) $(EMUL_OBJS)
	@bash tools/gen_stubs.sh $(BUILD) $(BUILD)/lx_stubs_gen.c >/dev/null
	@bash tools/symcensus.sh $(BUILD)

clean:
	rm -rf build
