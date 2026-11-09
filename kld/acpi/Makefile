# kld-acpi: the KLD ACPI container. ACPICA is the UNMODIFIED linux 6.12.99 copy
# (drivers/acpi/acpica), compiled in userspace mode; its object list is read
# from upstream's own Makefile by tools/kbuild2mk.py, evaluated against the
# pinned config (so the debugger objects stay out exactly as in the kernel build).
#
#   make            host build (file mode: --tables DIR)
#   make kurono     the same with the broker backend (runs inside Kurono)
#
# end (satoru)

KVER    := $(shell cat ../KLD_KERNEL_VERSION)
LINUX   := ../../third_party/linux-$(KVER)
ACPICA  := $(LINUX)/drivers/acpi/acpica
CC      := musl-gcc
OBJS_UP := $(shell python3 -I ../tools/kbuild2mk.py $(ACPICA)/Makefile acpi $(LINUX)/include/generated/autoconf.h)
BUILD   := ../build/acpi
UP_OBJS := $(addprefix $(BUILD)/acpica/,$(OBJS_UP))
OWN     := kld_acpi_osl.c kld_acpi_main.c kld_acpi_broker.c
OWN_OBJS:= $(patsubst %.c,$(BUILD)/%.o,$(OWN))

# ACPI_PCI_CONFIGURED: the kernel-mode aclinux.h derives it from CONFIG_PCI=y
# (pinned config); the userspace branch does not, so it is set here. (satoru)
# upstream's own ccflags (-D_LINUX -DBUILDING_ACPICA), userspace mode (no
# __KERNEL__), and the vendored include/ for <acpi/...>. (satoru)
ACFLAGS := -D_LINUX -DBUILDING_ACPICA -DACPI_PCI_CONFIGURED -I$(BUILD)/inc -Ishim -I$(ACPICA) -O2 -g \
           -fno-strict-aliasing -Wno-unused -Wno-address-of-packed-member \
           -Wno-stringop-truncation -Wno-format-truncation
OWNFLAGS:= -D_LINUX -DACPI_PCI_CONFIGURED -DBUILDING_ACPICA -I$(BUILD)/inc -Ishim -I$(ACPICA) -O2 -g -Wall -Wno-address-of-packed-member

.PHONY: all clean
all: $(BUILD)/kld-acpi.elf

# only include/acpi may be visible: putting the whole kernel include/ on the
# path lets <linux/...> from the libc headers resolve to KERNEL headers. (satoru)
$(BUILD)/inc/acpi:
	@mkdir -p $(BUILD)/inc
	ln -sfn $(abspath $(LINUX)/include/acpi) $@

$(UP_OBJS) $(OWN_OBJS): | $(BUILD)/inc/acpi

$(BUILD)/acpica/%.o: $(ACPICA)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ACFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(OWNFLAGS) -c -o $@ $<

$(BUILD)/kld-acpi.elf: $(UP_OBJS) $(OWN_OBJS)
	$(CC) -static -no-pie -pthread -o $@ $^
	@echo "kld-acpi: $(words $(UP_OBJS)) unmodified ACPICA objects + $(words $(OWN_OBJS)) KLD objects"

clean:
	rm -rf $(BUILD)
