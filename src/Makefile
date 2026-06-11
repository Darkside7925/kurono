# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS — Cross-Compilation Makefile (x86_64 bare metal)
# ═══════════════════════════════════════════════════════════════════════════
#
#  Requirements:
#    • x86_64-elf cross toolchain (x86_64-elf-g++, x86_64-elf-as, x86_64-elf-ld)
#      (falls back to native g++ -m64 if unavailable)
#    • NASM assembler
#    • grub-mkrescue + xorriso (for ISO)
#    • QEMU (for testing)
#
#  Targets:
#    make          — build kernel ELF binary
#    make iso      — build bootable ISO
#    make run      — run in QEMU with BGA display
#    make clean    — remove build artefacts
#    make debug    — run in QEMU with GDB stub
# ═══════════════════════════════════════════════════════════════════════════

# ─── Toolchain ────────────────────────────────────────────────────────────
# Use native GCC with -m64 if x86_64-elf isn't available
PREFIX  ?= x86_64-elf
HAVE_CROSS := $(shell which $(PREFIX)-g++ 2>/dev/null)
ifdef HAVE_CROSS
CC       = $(PREFIX)-gcc
CXX      = $(PREFIX)-g++
AS       = $(PREFIX)-as
LD       = $(PREFIX)-ld
OBJCOPY  = $(PREFIX)-objcopy
else
CC       = gcc
CXX      = g++
AS       = as
LD       = ld
OBJCOPY  = objcopy
endif
NASM     = nasm

# ─── Directories ──────────────────────────────────────────────────────────
SRC      = .
BUILD    = ../build
ISO_DIR  = $(BUILD)/isodir

# ─── Flags ────────────────────────────────────────────────────────────────
CXXFLAGS = -std=c++17 -ffreestanding -O2 -Wall -Wextra -fno-exceptions \
           -fno-rtti -fno-stack-protector -fno-use-cxa-atexit \
           -mno-red-zone -m64 -mcmodel=large -fno-pie -I$(SRC)

ASFLAGS  = -f elf64
LDFLAGS  = -T $(SRC)/boot/kurono_linker.ld -nostdlib -no-pie -lgcc

# ─── Source files ─────────────────────────────────────────────────────────
# NOTE: Run 'make' from inside the src/ directory
# Assembly (boot)
ASM_SRCS = $(SRC)/boot/kurono_boot.asm

# C++ sources — listed in dependency order for clarity
CXX_SRCS = \
    $(SRC)/kernel/types.cpp \
    $(SRC)/kernel/heap.cpp \
    $(SRC)/kernel/time.cpp \
    $(SRC)/kernel/system.cpp \
    $(SRC)/hal/hal.cpp \
    $(SRC)/drivers/serial.cpp \
    $(SRC)/drivers/display.cpp \
    $(SRC)/drivers/graphics.cpp \
    $(SRC)/drivers/bga.cpp \
    $(SRC)/drivers/timer.cpp \
    $(SRC)/drivers/keyboard.cpp \
    $(SRC)/drivers/mouse.cpp \
    $(SRC)/drivers/rtc.cpp \
    $(SRC)/proc/scheduler.cpp \
    $(SRC)/fs/vfs.cpp \
    $(SRC)/fs/kvfs.cpp \
    $(SRC)/shell/shell.cpp \
    $(SRC)/shell/linux_cmds.cpp \
    $(SRC)/shell/windows_cmds.cpp \
    $(SRC)/kcl/kcl.cpp \
    $(SRC)/security/supr.cpp \
    $(SRC)/packages/pkgmgr.cpp \
    $(SRC)/net/network.cpp \
    $(SRC)/media/mediadecoder.cpp \
    $(SRC)/ui/font.cpp \
    $(SRC)/ui/text_layout.cpp \
    $(SRC)/ui/ui_elements.cpp \
    $(SRC)/ui/gui.cpp \
    $(SRC)/ui/lockscreen.cpp \
    $(SRC)/ui/window_manager.cpp \
    $(SRC)/ui/desktop.cpp \
    $(SRC)/ui/file_browser.cpp \
    $(SRC)/apps/calculator.cpp \
    $(SRC)/apps/terminal.cpp \
    $(SRC)/apps/file_manager.cpp \
    $(SRC)/apps/text_editor.cpp \
    $(SRC)/apps/settings.cpp \
    $(SRC)/apps/task_manager.cpp \
    $(SRC)/apps/browser.cpp \
    $(SRC)/apps/media_player.cpp \
    $(SRC)/drivers/audio.cpp \
    $(SRC)/system/input_manager.cpp \
    $(SRC)/tests/test_suite.cpp \
    $(SRC)/linux/kls.cpp \
    $(SRC)/linux/linux_syscall.cpp \
    $(SRC)/linux/ext4.cpp \
    $(SRC)/linux/shared_mount.cpp \
    $(SRC)/linux/user_bridge.cpp \
    $(SRC)/linux/linux_kernel.cpp \
    $(SRC)/linux/linux_signals.cpp \
    $(SRC)/linux/linux_devices.cpp \
    $(SRC)/linux/linux_init.cpp \
    $(SRC)/linux/dual_boot.cpp \
    $(SRC)/linux/linux_netbridge.cpp \
    $(SRC)/system/user_mgmt.cpp \
    $(SRC)/third_party/stb_image_glue.cpp \
    $(SRC)/third_party/stb_truetype_glue.cpp \
    $(SRC)/virt/vmm.cpp \
    $(SRC)/virt/ept.cpp \
    $(SRC)/virt/vmexit.cpp \
    $(SRC)/virt/vdevices.cpp \
    $(SRC)/virt/vserial.cpp \
    $(SRC)/virt/vdisk.cpp \
    $(SRC)/virt/guest_mem.cpp \
    $(SRC)/virt/linux_boot.cpp \
    $(SRC)/virt/hypervisor.cpp \
    $(SRC)/virt/iommu.cpp \
    $(SRC)/drivers/nvidia_gpu.cpp \
    $(SRC)/drivers/e1000.cpp \
    $(SRC)/linux/linux_drivers.cpp \
    $(SRC)/kernel/kurono_kernel.cpp

# ─── Object files ─────────────────────────────────────────────────────────
ASM_OBJS = $(patsubst $(SRC)/%.asm, $(BUILD)/%.o, $(ASM_SRCS))
CXX_OBJS = $(patsubst $(SRC)/%.cpp, $(BUILD)/%.o, $(CXX_SRCS))

ALL_OBJS = $(ASM_OBJS) $(CXX_OBJS)

# ─── Output ───────────────────────────────────────────────────────────────
KERNEL   = $(BUILD)/kurono.elf
ISO      = $(BUILD)/kurono.iso

# ═══════════════════════════════════════════════════════════════════════════
#  Targets
# ═══════════════════════════════════════════════════════════════════════════
.PHONY: all iso run debug clean dirs

all: dirs $(KERNEL)
	@echo "══════════════════════════════════════════════"
	@echo "  Kurono OS kernel built: $(KERNEL)"
	@echo "══════════════════════════════════════════════"

# ─── Link ─────────────────────────────────────────────────────────────────
$(KERNEL): $(ALL_OBJS)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld -nostdlib -o $@ $^ -lgcc

# ─── Compile assembly ─────────────────────────────────────────────────────
$(BUILD)/boot/kurono_boot.o: $(SRC)/boot/kurono_boot.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

# ─── Compile C++ ──────────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ─── Create build directories ────────────────────────────────────────────
dirs:
	@mkdir -p $(BUILD)/kernel $(BUILD)/hal $(BUILD)/drivers $(BUILD)/proc \
	          $(BUILD)/fs $(BUILD)/shell $(BUILD)/kcl $(BUILD)/security \
	          $(BUILD)/packages $(BUILD)/net $(BUILD)/media $(BUILD)/ui \
	          $(BUILD)/apps $(BUILD)/system $(BUILD)/tests $(BUILD)/boot \
	          $(BUILD)/third_party $(BUILD)/linux $(BUILD)/virt

# ─── Bootable ISO ─────────────────────────────────────────────────────────
iso: all
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kurono.elf
	@echo 'menuentry "Kurono OS" {'       > $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot /boot/kurono.elf' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                              >> $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_DIR)
	@echo "══════════════════════════════════════════════"
	@echo "  Bootable ISO: $(ISO)"
	@echo "══════════════════════════════════════════════"

# ─── Run in QEMU (accelerated, high refresh) ─────────────────────────────
run: all
	qemu-system-x86_64 \
	    -kernel $(KERNEL) \
	    -m 10G \
	    -cpu host \
	    -smp 4 \
	    -accel whpx,kernel-irqchip=off \
	    -vga std \
	    -display gtk,gl=on \
	    -serial stdio \
	    -device sb16 \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	    -device virtio-balloon-pci \
	    -no-reboot -no-shutdown

# ─── Run without acceleration (fallback) ──────────────────────────────────
run-noaccel: all
	qemu-system-x86_64 \
	    -kernel $(KERNEL) \
	    -m 10G \
	    -smp 4 \
	    -vga std \
	    -serial stdio \
	    -device sb16 \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	    -no-reboot -no-shutdown

# ─── Run with GPU passthrough (requires IOMMU + VFIO) ─────────────────────
# Usage: make run-passthrough GPU_BDF=01:00.0
GPU_BDF ?= 01:00.0
run-passthrough: all
	qemu-system-x86_64 \
	    -kernel $(KERNEL) \
	    -m 10G \
	    -cpu host \
	    -smp 4,sockets=1,cores=4,threads=1 \
	    -accel whpx,kernel-irqchip=off \
	    -machine type=q35,accel=whpx \
	    -vga none \
	    -device vfio-pci,host=$(GPU_BDF),multifunction=on \
	    -display gtk,gl=on \
	    -serial stdio \
	    -device sb16 \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	    -device virtio-balloon-pci \
	    -no-reboot -no-shutdown

# ─── Run with KVM on Linux host (AMD-V / VT-x) ───────────────────────────
run-kvm: all
	qemu-system-x86_64 \
	    -kernel $(KERNEL) \
	    -m 10G \
	    -cpu host \
	    -smp 4,sockets=1,cores=4,threads=1 \
	    -enable-kvm \
	    -machine type=q35 \
	    -vga std \
	    -display gtk,gl=on \
	    -serial stdio \
	    -device sb16 \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	    -device virtio-balloon-pci \
	    -no-reboot -no-shutdown

# ─── Debug with GDB ───────────────────────────────────────────────────────
debug: all
	qemu-system-x86_64 \
	    -kernel $(KERNEL) \
	    -m 10G \
	    -cpu host \
	    -smp 4 \
	    -accel whpx,kernel-irqchip=off \
	    -vga std \
	    -serial stdio \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	    -s -S \
	    -no-reboot -no-shutdown

# ─── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
	@echo "Build directory cleaned."
