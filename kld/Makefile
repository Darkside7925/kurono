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
LINUX     := ../third_party/linux-$(KVER)
driver    ?= e1000e
BUILD     := build/$(driver)
MUSLCC    ?= musl-gcc

# where each supported driver's sources live inside the vendored tree. (satoru)
DRVDIR_e1000e := $(LINUX)/drivers/net/ethernet/intel/e1000e
DRVDIR        := $(DRVDIR_$(driver))

ifeq ($(DRVDIR),)
$(error unknown driver '$(driver)' - add DRVDIR_$(driver) to this Makefile)
endif

DRV_SRCS  := $(wildcard $(DRVDIR)/*.c)
DRV_OBJS  := $(patsubst $(DRVDIR)/%.c,$(BUILD)/driver/%.o,$(DRV_SRCS))

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

$(BUILD)/driver/%.o: $(DRVDIR)/%.c | check-tree
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
$(BUILD)/lx_stubs_gen.c: $(DRV_OBJS) $(EMUL_OBJS)
	@mkdir -p $(dir $@)
	bash tools/gen_stubs.sh $(BUILD) $@

$(BUILD)/emul/lx_stubs_gen.o: $(BUILD)/lx_stubs_gen.c
	$(MUSLCC) $(EMUL_CFLAGS) -c -o $@ $<

$(CONTAINER): $(DRV_OBJS) $(EMUL_OBJS) $(BUILD)/emul/lx_stubs_gen.o
	$(MUSLCC) -static -no-pie -o $@ $^
	@echo "KLD: built $@"
	@bash tools/symcensus.sh $(BUILD)

census: $(DRV_OBJS) $(EMUL_OBJS)
	@bash tools/gen_stubs.sh $(BUILD) $(BUILD)/lx_stubs_gen.c >/dev/null
	@bash tools/symcensus.sh $(BUILD)

clean:
	rm -rf build
