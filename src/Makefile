# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS  -  Cross-Compilation Makefile (x86_64 bare metal)
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
#    make           -  build kernel ELF binary
#    make iso       -  build bootable ISO
#    make run       -  run in QEMU with BGA display
#    make clean     -  remove build artefacts
#    make debug     -  run in QEMU with GDB stub
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

# ─── EFI Loader Toolchain (gnu-efi) ──────────────────────────────────────
EFI_CC       = gcc
EFI_LD       = ld
EFI_OBJCOPY  = objcopy
EFI_INC      = -I/usr/include/efi -I/usr/include/efi/x86_64
EFI_CFLAGS   = $(EFI_INC) -ffreestanding -fno-stack-protector -fpic \
               -fshort-wchar -mno-red-zone
EFI_LDFLAGS  = -nostdlib -znocombreloc -shared -Bsymbolic \
               -T /usr/lib/elf_x86_64_efi.lds
EFI_CRT0     = /usr/lib/crt0-efi-x86_64.o
EFI_LIBS     = -L /usr/lib -lgnuefi -lefi

# ─── Directories ──────────────────────────────────────────────────────────
SRC      = .
BUILD    = ../build
ISO_DIR  = $(BUILD)/isodir
ALPINE_DIR = ../Alpine
DEBIAN_DIR = ../Debian

# ─── Alpine Linux guest data (embedded via objcopy) ──────────────────────
# These are only linked if the files exist (after running WSL extraction).
ALPINE_KERNEL_BIN  = $(ALPINE_DIR)/vmlinuz-virt
ALPINE_INITRD_BIN  = $(ALPINE_DIR)/initramfs-virt
ALPINE_KERNEL_OBJ  = $(BUILD)/alpine_kernel.o
ALPINE_INITRD_OBJ  = $(BUILD)/alpine_initramfs.o
ALPINE_OBJS        =
ifneq ($(wildcard $(ALPINE_KERNEL_BIN)),)
ALPINE_OBJS       += $(ALPINE_KERNEL_OBJ)
endif
ifneq ($(wildcard $(ALPINE_INITRD_BIN)),)
ALPINE_OBJS       += $(ALPINE_INITRD_OBJ)
endif

# ─── Debian Linux guest rootfs (embedded via objcopy) ────────────────────
DEBIAN_ROOTFS_BIN = $(DEBIAN_DIR)/debian-minbase.ext4
DEBIAN_ROOTFS_OBJ = $(BUILD)/debian_rootfs.o
DEBIAN_OBJS       =
ifneq ($(wildcard $(DEBIAN_ROOTFS_BIN)),)
DEBIAN_OBJS      += $(DEBIAN_ROOTFS_OBJ)
endif

# ─── Media assets (embedded via objcopy) ──────────────────────────────────
MEDIA_DIR     = ../archive/old_misc
DENJI_MP4_BIN = $(MEDIA_DIR)/denji.mp4
DENJI_MP4_OBJ = $(BUILD)/denji_mp4.o
MEDIA_OBJS    =
ifneq ($(wildcard $(DENJI_MP4_BIN)),)
MEDIA_OBJS   += $(DENJI_MP4_OBJ)
endif

# ─── Flags ────────────────────────────────────────────────────────────────
CXXFLAGS = -std=c++17 -ffreestanding -O2 -Wall -Wextra -fno-exceptions \
           -fno-rtti -fno-stack-protector -fno-use-cxa-atexit \
           -mno-red-zone -m64 -mcmodel=large -fno-pie -I$(SRC)

ASFLAGS  = -f elf64
LDFLAGS  = -T $(SRC)/boot/kurono_linker.ld -nostdlib -no-pie -lgcc

# ─── Source files ─────────────────────────────────────────────────────────
# NOTE: Run 'make' from inside the src/ directory
# Assembly (boot)
ASM_SRCS = $(SRC)/boot/kurono_boot.asm \
           $(SRC)/hal/isr_stubs.asm

# C++ sources  -  listed in dependency order for clarity
CXX_SRCS = \
    $(SRC)/kernel/types.cpp \
    $(SRC)/kernel/heap.cpp \
    $(SRC)/kernel/pmm.cpp \
    $(SRC)/kernel/vmm.cpp \
    $(SRC)/kernel/time.cpp \
    $(SRC)/kernel/system.cpp \
	$(SRC)/kernel/panic.cpp \
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
	$(SRC)/fs/fat32.cpp \
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
	$(SRC)/apps/conduit.cpp \
    $(SRC)/apps/media_player.cpp \
    $(SRC)/drivers/audio.cpp \
    $(SRC)/system/input_manager.cpp \
	$(SRC)/system/conduit.cpp \
	$(SRC)/system/logging.cpp \
	$(SRC)/system/installer.cpp \
	$(SRC)/system/ui_config.cpp \
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
    $(SRC)/virt/v9fs.cpp \
    $(SRC)/drivers/nvidia_gpu.cpp \
    $(SRC)/drivers/intel_gpu.cpp \
    $(SRC)/drivers/e1000.cpp \
    $(SRC)/drivers/nvme.cpp \
    $(SRC)/drivers/usb.cpp \
    $(SRC)/drivers/hda.cpp \
    $(SRC)/drivers/virtio_gpu.cpp \
    $(SRC)/drivers/display_mgr.cpp \
    $(SRC)/drivers/amd_gpu.cpp \
    $(SRC)/drivers/ac97.cpp \
    $(SRC)/drivers/gpu_probe.cpp \
    $(SRC)/drivers/cpu_detect.cpp \
    $(SRC)/media/mp3_decoder.cpp \
    $(SRC)/media/codec.cpp \
    $(SRC)/net/tcpip.cpp \
    $(SRC)/linux/linux_drivers.cpp \
    $(SRC)/kernel/kurono_kernel.cpp

# ─── Object files ─────────────────────────────────────────────────────────
ASM_OBJS = $(patsubst $(SRC)/%.asm, $(BUILD)/%.o, $(ASM_SRCS))
CXX_OBJS = $(patsubst $(SRC)/%.cpp, $(BUILD)/%.o, $(CXX_SRCS))

ALL_OBJS = $(ASM_OBJS) $(CXX_OBJS)

# ─── Output ───────────────────────────────────────────────────────────────
KERNEL_BASE = $(BUILD)/kurono_base.elf
KERNEL   = $(BUILD)/kurono.elf
ISO      = $(BUILD)/kurono.iso
ISO_ALT  = ../iso/kurono.iso
EFI_APP  = $(BUILD)/boot/kurono_efi.efi
EFI_APP_EMERGENCY = $(BUILD)/boot/kurono_emergency.efi
INSTALLER_PAYLOAD_OBJS = $(BUILD)/boot/install_kernel_embed.o \
	$(BUILD)/boot/install_efi_embed.o \
	$(BUILD)/boot/install_efi_emergency_embed.o

# ═══════════════════════════════════════════════════════════════════════════
#  Targets
# ═══════════════════════════════════════════════════════════════════════════
.PHONY: all iso run debug clean dirs efi debian-rootfs

all: dirs $(KERNEL)
	@echo "══════════════════════════════════════════════"
	@echo "  Kurono OS kernel built: $(KERNEL)"
	@echo "══════════════════════════════════════════════"

# ─── Link ─────────────────────────────────────────────────────────────────

$(KERNEL_BASE): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld -nostdlib -o $@ $^ -lgcc

$(KERNEL): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS) $(INSTALLER_PAYLOAD_OBJS)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld -nostdlib -o $@ $^ -lgcc

# ─── Embed Alpine kernel via objcopy (binary → ELF .rodata) ──────────────
$(ALPINE_KERNEL_OBJ): $(ALPINE_KERNEL_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/vmlinuz-virt && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  vmlinuz-virt alpine_kernel.o && \
	rm -f vmlinuz-virt

# ─── Embed Alpine initramfs via objcopy (binary → ELF .rodata) ───────────
$(ALPINE_INITRD_OBJ): $(ALPINE_INITRD_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/initramfs-virt && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  initramfs-virt alpine_initramfs.o && \
	rm -f initramfs-virt

# ─── Embed Debian rootfs image via objcopy (binary → ELF .rodata) ───────
$(DEBIAN_ROOTFS_OBJ): $(DEBIAN_ROOTFS_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/debian-minbase.ext4 && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  debian-minbase.ext4 debian_rootfs.o && \
	rm -f debian-minbase.ext4

# ─── Embed denji.mp4 via objcopy (binary → ELF .rodata) ──────────────────
$(DENJI_MP4_OBJ): $(DENJI_MP4_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/denji.mp4 && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --add-section .note.GNU-stack=/dev/null \
	  --set-section-flags .note.GNU-stack=noload,readonly \
	  denji.mp4 denji_mp4.o && \
	rm -f denji.mp4

# ─── Compile assembly ─────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.asm
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

debian-rootfs:
	sh ../tools/build_debian_rootfs.sh

# ─── Standalone EFI Loader ─────────────────────────────────────────────
#    Bypasses GRUB multiboot2  -  loads kurono.elf directly via EFI
#    protocols.  Memory allocated as EfiLoaderCode (executable).
efi: dirs $(EFI_APP) $(EFI_APP_EMERGENCY)
	@echo "  EFI loader: $(EFI_APP)"
	@echo "  EFI emergency loader: $(EFI_APP_EMERGENCY)"

$(BUILD)/boot/efi_loader.o: $(SRC)/boot/efi_loader.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -DKURONO_EFI_CMDLINE='"efi"' -c $< -o $@

$(BUILD)/boot/efi_loader_emergency.o: $(SRC)/boot/efi_loader.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -DKURONO_EFI_CMDLINE='"efi kurono_mode=emergency kurono.text=1"' -c $< -o $@

# Embed base kurono.elf as binary data inside the EFI loader
$(BUILD)/boot/kernel_embed.o: $(KERNEL_BASE)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono.elf && \
	cd $(BUILD)/boot && $(EFI_OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	    kurono.elf kernel_embed.o

# Installer payload embeddings  -  linked only into the final kernel
$(BUILD)/boot/install_kernel_embed.o: $(KERNEL_BASE)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono_installer_kernel.elf && \
	cd $(BUILD)/boot && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_installer_kernel.elf install_kernel_embed.o

$(BUILD)/boot/efi_loader.so: $(BUILD)/boot/efi_loader.o $(BUILD)/boot/kernel_embed.o
	$(EFI_LD) $(EFI_LDFLAGS) $(EFI_CRT0) $^ $(EFI_LIBS) -o $@

$(BUILD)/boot/efi_loader_emergency.so: $(BUILD)/boot/efi_loader_emergency.o $(BUILD)/boot/kernel_embed.o
	$(EFI_LD) $(EFI_LDFLAGS) $(EFI_CRT0) $^ $(EFI_LIBS) -o $@

$(EFI_APP): $(BUILD)/boot/efi_loader.so
	$(EFI_OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	    --target efi-app-x86_64 $< $@

$(BUILD)/boot/install_efi_embed.o: $(EFI_APP)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono_installer_efi.efi && \
	cd $(BUILD)/boot && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_installer_efi.efi install_efi_embed.o

$(EFI_APP_EMERGENCY): $(BUILD)/boot/efi_loader_emergency.so
	$(EFI_OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	    --target efi-app-x86_64 $< $@

$(BUILD)/boot/install_efi_emergency_embed.o: $(EFI_APP_EMERGENCY)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono_installer_emergency.efi && \
	cd $(BUILD)/boot && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_installer_emergency.efi install_efi_emergency_embed.o

# ─── Bootable ISO ─────────────────────────────────────────────────────────
iso: all efi
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kurono.elf
	cp $(EFI_APP) $(ISO_DIR)/boot/kurono_efi.efi
	cp $(EFI_APP_EMERGENCY) $(ISO_DIR)/boot/kurono_emergency.efi
	@echo 'insmod all_video'                                            > $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod multiboot2'                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod multiboot'                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod efi_gop'                                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod efi_uga'                                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod vbe'                                                 >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod gfxterm'                                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'insmod chain'                                               >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set gfxmode=auto'                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'terminal_output gfxterm'                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set timeout=10'                                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '# Auto-select EFI Direct on UEFI, Multiboot2 on BIOS'      >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'if [ "$$grub_platform" = "efi" ]; then'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set default=0'                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'else'                                                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set default=1'                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'fi'                                                         >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (EFI Direct)" {'                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono EFI Loader..."'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    chainloader /boot/kurono_efi.efi'                        >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Emergency EFI)" {'                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono Emergency EFI Loader..."'          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    chainloader /boot/kurono_emergency.efi'                  >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Multiboot2)" {'                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono OS kernel (multiboot2)..."'         >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Kernel loaded OK. Booting..."'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Emergency Multiboot2)" {'             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono Emergency kernel (multiboot2)..."' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=text'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono.elf kurono_mode=emergency kurono.text=1' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Emergency kernel loaded OK. Booting..."'          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Multiboot1)" {'                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono OS kernel (multiboot1)..."'         >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot /boot/kurono.elf'                              >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Kernel loaded OK. Booting..."'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Debug - GRUB verbose)" {'             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set debug=all'                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set pager=1'                                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "=== GRUB DEBUG MODE ==="'                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading multiboot2 module..."'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    insmod multiboot2'                                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Setting gfxpayload..."'                            >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading kernel ELF..."'                            >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Kernel loaded. Press any key to boot..."'          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    sleep 5'                                                 >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Text Mode - no framebuffer)" {'       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Text mode boot..."'                                >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=text'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (gfxpayload=keep)" {'                  >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Keep mode boot..."'                                >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_DIR)
	@mkdir -p ../iso
	cp $(ISO) $(ISO_ALT)
	@echo "══════════════════════════════════════════════"
	@echo "  Bootable ISO: $(ISO)"
	@echo "  Copy: $(ISO_ALT)"
	@echo "══════════════════════════════════════════════"

# ─── Run in QEMU (accelerated, high refresh) ─────────────────────────────
run: iso
	qemu-system-x86_64 \
	    -cdrom $(ISO) \
	    -m 10G \
	    -smp 4 \
	    -accel whpx,kernel-irqchip=off \
	    -vga std \
	    -display sdl \
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
	    -smp 4,sockets=1,cores=4,threads=1 \
	    -accel whpx,kernel-irqchip=off \
	    -machine type=q35,accel=whpx \
	    -vga none \
	    -device vfio-pci,host=$(GPU_BDF),multifunction=on \
	    -display sdl \
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
