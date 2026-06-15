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
#
#  Off by default  -  the rootfs is ~108 MB and bloats the ISO.  Users
#  obtain it at runtime via `kpkg install debian`, which downloads it
#  to /var/lib/kurono/debian-rootfs.ext4 and then prompts to reboot
#  into the system update screen.  To force-embed for offline ISOs:
#      make EMBED_DEBIAN=1
DEBIAN_ROOTFS_BIN = $(DEBIAN_DIR)/debian-minbase.ext4
DEBIAN_ROOTFS_OBJ = $(BUILD)/debian_rootfs.o
DEBIAN_OBJS       =
ifeq ($(EMBED_DEBIAN),1)
ifneq ($(wildcard $(DEBIAN_ROOTFS_BIN)),)
DEBIAN_OBJS      += $(DEBIAN_ROOTFS_OBJ)
endif
endif

# ─── Media assets (embedded via objcopy) ──────────────────────────────────
MEDIA_DIR     = ../archive/old_misc
DENJI_MP4_BIN = $(MEDIA_DIR)/denji.mp4
DENJI_MP4_OBJ = $(BUILD)/denji_mp4.o
DENJI_KVID_BIN = $(SRC)/media/denji.kvid
DENJI_KVID_OBJ = $(BUILD)/denji_kvid.o
# user-imported video (satoru)
SSSTIK_MP4_BIN  = $(SRC)/media/ssstik.mp4
SSSTIK_MP4_OBJ  = $(BUILD)/ssstik_mp4.o
SSSTIK_KVID_BIN = $(SRC)/media/ssstik.kvid
SSSTIK_KVID_OBJ = $(BUILD)/ssstik_kvid.o
MEDIA_OBJS    =
ifneq ($(wildcard $(DENJI_MP4_BIN)),)
MEDIA_OBJS   += $(DENJI_MP4_OBJ)
endif
ifneq ($(wildcard $(DENJI_KVID_BIN)),)
MEDIA_OBJS   += $(DENJI_KVID_OBJ)
endif
ifneq ($(wildcard $(SSSTIK_MP4_BIN)),)
MEDIA_OBJS   += $(SSSTIK_MP4_OBJ)
endif
ifneq ($(wildcard $(SSSTIK_KVID_BIN)),)
MEDIA_OBJS   += $(SSSTIK_KVID_OBJ)
endif

# ─── User programs (test ELF64 binaries embedded into the kernel) ───────
USERPROGS_DIR     = $(SRC)/userprogs
HELLO_USER_ELF    = $(BUILD)/userprogs/hello.elf
HELLO_USER_OBJ    = $(BUILD)/userprogs/hello_user_embed.o
HELLO_X64_ELF     = $(BUILD)/userprogs/hello_x64.elf
HELLO_X64_OBJ     = $(BUILD)/userprogs/hello_x64_user_embed.o
# prebuilt musl-static ET_EXEC hello, embedded as a blob (satoru)
HELLO_MUSL_ELF    = $(USERPROGS_DIR)/hello_musl.elf
HELLO_MUSL_OBJ    = $(BUILD)/userprogs/hello_musl_user_embed.o
KPY_C_SRC         = $(USERPROGS_DIR)/kpython.c
KPY_ELF           = $(BUILD)/userprogs/kpython.elf
KPY_OBJ           = $(BUILD)/userprogs/kpython_user_embed.o
# prebuilt musl-static ffmpeg n5.1.6 (~2.7 mb), embedded as a blob (satoru)
FFMPEG_ELF        = $(USERPROGS_DIR)/ffmpeg.elf
FFMPEG_OBJ        = $(BUILD)/userprogs/ffmpeg_user_embed.o
# raw-protocol wl_shm wayland test client (proves the compositor render path) (satoru)
WLSHM_ELF         = $(USERPROGS_DIR)/wl_shm_test.elf
WLSHM_OBJ         = $(BUILD)/userprogs/wl_shm_test_user_embed.o
# pthreads smoke test (proves CLONE_THREAD + futex) (satoru)
PTHREAD_ELF       = $(USERPROGS_DIR)/pthread_test.elf
PTHREAD_OBJ       = $(BUILD)/userprogs/pthread_test_user_embed.o
# dynamic musl pie + its libc.so: first real exercise of the ld-kurono dynamic path (satoru)
DYNTEST_ELF       = $(USERPROGS_DIR)/dyntest.elf
DYNTEST_OBJ       = $(BUILD)/userprogs/dyntest_user_embed.o
MUSL_LIBC_SO      = $(USERPROGS_DIR)/musl_libc.so
MUSL_LIBC_OBJ     = $(BUILD)/userprogs/musl_libc_user_embed.o
# tiny extra .so the dyntest pie depends on  -  exercises musl loading a non-interp
# library via the new file-backed mmap path (satoru)
LIBFOO_SO         = $(USERPROGS_DIR)/libfoo.so
LIBFOO_OBJ        = $(BUILD)/userprogs/libfoo_user_embed.o
USERPROG_OBJS     = $(HELLO_USER_OBJ) $(HELLO_X64_OBJ) $(HELLO_MUSL_OBJ) $(KPY_OBJ) $(FFMPEG_OBJ) $(WLSHM_OBJ) $(PTHREAD_OBJ) $(DYNTEST_OBJ) $(MUSL_LIBC_OBJ) $(LIBFOO_OBJ)

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
		   $(SRC)/kernel/userspace_entry.asm \
           $(SRC)/hal/isr_stubs.asm \
           $(SRC)/hal/syscall_entry.asm \
           $(SRC)/proc/switch_to.asm

# C++ sources  -  listed in dependency order for clarity
CXX_SRCS = \
    $(SRC)/kernel/types.cpp \
    $(SRC)/kernel/heap.cpp \
    $(SRC)/kernel/pmm.cpp \
    $(SRC)/kernel/buddy.cpp \
    $(SRC)/kernel/slab.cpp \
    $(SRC)/kernel/vmm.cpp \
    $(SRC)/kernel/kdf.cpp \
    $(SRC)/kernel/kdf_test.cpp \
    $(SRC)/kernel/udf.cpp \
    $(SRC)/kernel/irp.cpp \
    $(SRC)/kernel/kexec.cpp \
	$(SRC)/kernel/userspace.cpp \
	$(SRC)/kernel/elf_loader.cpp \
    $(SRC)/kernel/time.cpp \
    $(SRC)/kernel/system.cpp \
	$(SRC)/kernel/panic.cpp \
    $(SRC)/kernel/hrtimer.cpp \
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
    $(SRC)/proc/smp.cpp \
    $(SRC)/proc/kernel_processes.cpp \
    $(SRC)/fs/vfs.cpp \
    $(SRC)/fs/kvfs.cpp \
    $(SRC)/fs/persist.cpp \
    $(SRC)/fs/kfs.cpp \
    $(SRC)/fs/kfs_bench.cpp \
	$(SRC)/fs/fat32.cpp \
    $(SRC)/shell/shell.cpp \
    $(SRC)/shell/linux_cmds.cpp \
    $(SRC)/shell/windows_cmds.cpp \
    $(SRC)/kcl/kcl.cpp \
    $(SRC)/kcl/kcl_test.cpp \
    $(SRC)/security/supr.cpp \
    $(SRC)/security/ksa.cpp \
    $(SRC)/packages/pkgmgr.cpp \
    $(SRC)/net/network.cpp \
    $(SRC)/media/mediadecoder.cpp \
    $(SRC)/ui/font.cpp \
    $(SRC)/ui/text_layout.cpp \
    $(SRC)/ui/ui_elements.cpp \
    $(SRC)/ui/kss.cpp \
    $(SRC)/ui/app_icons.cpp \
    $(SRC)/ui/gui.cpp \
    $(SRC)/ui/lockscreen.cpp \
    $(SRC)/ui/window_manager.cpp \
    $(SRC)/ui/desktop.cpp \
    $(SRC)/ui/control_center.cpp \
    $(SRC)/ui/file_browser.cpp \
    $(SRC)/ui/notification.cpp \
    $(SRC)/ui/perf_hud.cpp \
    $(SRC)/apps/calculator.cpp \
    $(SRC)/apps/terminal.cpp \
    $(SRC)/apps/file_manager.cpp \
    $(SRC)/apps/text_editor.cpp \
    $(SRC)/apps/settings.cpp \
    $(SRC)/apps/system_settings.cpp \
    $(SRC)/apps/settings_mod_display.cpp \
    $(SRC)/apps/settings_mod_about.cpp \
    $(SRC)/apps/settings_mod_audio.cpp \
    $(SRC)/apps/settings_mod_devices.cpp \
    $(SRC)/apps/settings_mod_network.cpp \
    $(SRC)/apps/settings_mod_power.cpp \
    $(SRC)/apps/settings_mod_storage.cpp \
    $(SRC)/apps/settings_mod_personalize.cpp \
    $(SRC)/apps/settings_mod_a11y.cpp \
    $(SRC)/apps/settings_mod_security.cpp \
    $(SRC)/apps/settings_mod_system.cpp \
    $(SRC)/apps/task_manager.cpp \
    $(SRC)/apps/browser.cpp \
    $(SRC)/apps/media_player.cpp \
    $(SRC)/apps/denji_app.cpp \
    $(SRC)/apps/python_interp.cpp \
    $(SRC)/apps/kj.cpp \
    $(SRC)/apps/kj_test.cpp \
    $(SRC)/drivers/audio.cpp \
    $(SRC)/system/input_manager.cpp \
    $(SRC)/system/clipboard.cpp \
    $(SRC)/system/screenshot.cpp \
	$(SRC)/system/conduit.cpp \
	$(SRC)/system/logging.cpp \
	$(SRC)/system/kinit.cpp \
	$(SRC)/system/kinit_kservice.cpp \
	$(SRC)/system/systemd_compat.cpp \
	$(SRC)/system/kpkg_daemon.cpp \
	$(SRC)/system/kdaemons.cpp \
	$(SRC)/system/installer.cpp \
	$(SRC)/system/installer_gui.cpp \
	$(SRC)/system/gpu_driver_installer.cpp \
	$(SRC)/system/system_update.cpp \
	$(SRC)/system/ui_config.cpp \
	$(SRC)/system/vconsole.cpp \
    $(SRC)/tests/test_suite.cpp \
    $(SRC)/linux/kls.cpp \
    $(SRC)/linux/linux_syscall.cpp \
    $(SRC)/linux/linux_syscall_x64.cpp \
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
    $(SRC)/virt/debian_data.cpp \
    $(SRC)/virt/vpci.cpp \
    $(SRC)/virt/virtio_gpu_host.cpp \
    $(SRC)/virt/pci_passthrough.cpp \
    $(SRC)/drivers/nvidia_gpu.cpp \
    $(SRC)/drivers/intel_gpu.cpp \
    $(SRC)/drivers/e1000.cpp \
    $(SRC)/drivers/wifi_dev.cpp \
    $(SRC)/drivers/wifi_rtw.cpp \
    $(SRC)/drivers/wifi_ath.cpp \
    $(SRC)/drivers/wifi_iwl.cpp \
    $(SRC)/drivers/wifi_brcm.cpp \
    $(SRC)/drivers/nvme.cpp \
    $(SRC)/drivers/usb.cpp \
    $(SRC)/drivers/hda.cpp \
    $(SRC)/drivers/virtio_gpu.cpp \
    $(SRC)/drivers/display_mgr.cpp \
    $(SRC)/drivers/amd_gpu.cpp \
    $(SRC)/drivers/ac97.cpp \
    $(SRC)/drivers/audio_dma.cpp \
    $(SRC)/drivers/audio_format.cpp \
    $(SRC)/drivers/audio_mixer.cpp \
    $(SRC)/drivers/audio_server.cpp \
    $(SRC)/drivers/audio_wav.cpp \
    $(SRC)/drivers/audio_backend_sb16.cpp \
    $(SRC)/drivers/audio_backend_ac97.cpp \
    $(SRC)/drivers/audio_backend_hda.cpp \
    $(SRC)/drivers/audio_backend_pcspk.cpp \
    $(SRC)/drivers/audio_backend.cpp \
    $(SRC)/drivers/gpu_probe.cpp \
    $(SRC)/drivers/cpu_detect.cpp \
    $(SRC)/media/mp3_decoder.cpp \
    $(SRC)/media/codec.cpp \
    $(SRC)/media/mp4_demux.cpp \
    $(SRC)/media/h264_parse.cpp \
    $(SRC)/media/aac_parse.cpp \
    $(SRC)/media/kvid.cpp \
    $(SRC)/media/video_player.cpp \
    $(SRC)/net/tcpip.cpp \
    $(SRC)/net/tuntap.cpp \
    $(SRC)/net/ipv6.cpp \
    $(SRC)/net/netfilter.cpp \
    $(SRC)/net/unix_socket.cpp \
    $(SRC)/net/wifi_crypto.cpp \
    $(SRC)/net/ieee80211.cpp \
    $(SRC)/net/ieee80211_test.cpp \
    $(SRC)/proc/cgroup.cpp \
    $(SRC)/drivers/tpm.cpp \
    $(SRC)/drivers/pulse_server.cpp \
    $(SRC)/hal/cpufreq.cpp \
    $(SRC)/ui/wayland_server.cpp \
    $(SRC)/system/runtime_layout.cpp \
    $(SRC)/system/dbus_server.cpp \
    $(SRC)/apps/firefox_launcher.cpp \
    $(SRC)/linux/ld_kurono.cpp \
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

KURONO_BOOT_PROFILE ?= normal
KURONO_GRUB_TIMEOUT ?= 10
KURONO_CLI_RUN ?=
KURONO_CLI_POWEROFF ?= 0
KURONO_GUI_RUN ?=
# extra tokens appended to the main (Multiboot2 desktop) grub cmdline. used for
# headless self-test gates, e.g. KURONO_EXTRA_CMDLINE='kurono.kdf.test kurono.kdf.poweroff=1'.
# (satoru)
KURONO_EXTRA_CMDLINE ?=
# autologin: bypass the lockscreen and boot straight to the desktop. on by
# default so `make iso` / start.ps1 produce a desktop-booting image -- the
# lockscreen's blocking Show() loop otherwise stalls headless/vm boots.
# set KURONO_AUTOLOGIN=0 for a real lockscreen build. (satoru)
KURONO_AUTOLOGIN ?= 1

# bios menu indices. the "Kurono Setup" entry sits at index 3 (right after the
# main Multiboot2/desktop entry at 2), so the cli/emergency entries shift down by
# one vs. the old layout. (satoru)
ifeq ($(KURONO_BOOT_PROFILE),cli)
KURONO_BIOS_DEFAULT = 5
else ifeq ($(KURONO_BOOT_PROFILE),emergency)
KURONO_BIOS_DEFAULT = 4
else
KURONO_BIOS_DEFAULT = 2
endif

KURONO_CLI_CMDLINE = -cli kurono_mode=cli kurono.text=1
ifneq ($(strip $(KURONO_CLI_RUN)),)
KURONO_CLI_CMDLINE += kurono.cli.run="$(KURONO_CLI_RUN)"
endif
ifeq ($(KURONO_CLI_POWEROFF),1)
KURONO_CLI_CMDLINE += kurono.cli.poweroff=1
endif

# ═══════════════════════════════════════════════════════════════════════════
#  Targets
# ═══════════════════════════════════════════════════════════════════════════
.PHONY: all iso run debug clean dirs efi debian-rootfs

all: dirs $(KERNEL)
	@echo "══════════════════════════════════════════════"
	@echo "  Kurono OS kernel built: $(KERNEL)"
	@echo "══════════════════════════════════════════════"

# ─── Link ─────────────────────────────────────────────────────────────────

# ap trampoline object  -  must be defined before the link rules use it (make
# expands prerequisites at parse time). (satoru)
AP_TRAMP_OBJ = $(BUILD)/boot/ap_trampoline.o

$(KERNEL_BASE): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS) $(USERPROG_OBJS) $(AP_TRAMP_OBJ)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld -nostdlib -o $@ $^ -lgcc

$(KERNEL): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS) $(USERPROG_OBJS) $(AP_TRAMP_OBJ) $(INSTALLER_PAYLOAD_OBJS)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld -nostdlib -o $@ $^ -lgcc

# ─── AP trampoline: flat 16/32/64-bit blob (nasm -f bin) embedded via objcopy ──
# runs at physical 0x8000 after SIPI; copied there at runtime by SMP::StartAPs. (satoru)
$(BUILD)/boot/ap_trampoline.bin: $(SRC)/boot/ap_trampoline.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@
$(AP_TRAMP_OBJ): $(BUILD)/boot/ap_trampoline.bin
	cd $(BUILD)/boot && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  ap_trampoline.bin ap_trampoline.o

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

# ─── Embed denji.kvid via objcopy (binary → ELF .rodata) ─────────────────
$(DENJI_KVID_OBJ): $(DENJI_KVID_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/denji.kvid && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --add-section .note.GNU-stack=/dev/null \
	  --set-section-flags .note.GNU-stack=noload,readonly \
	  denji.kvid denji_kvid.o && \
	rm -f denji.kvid

# ─── Embed user-imported ssstik.mp4 + ssstik.kvid via objcopy (satoru) ────
$(SSSTIK_MP4_OBJ): $(SSSTIK_MP4_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/ssstik.mp4 && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --add-section .note.GNU-stack=/dev/null \
	  --set-section-flags .note.GNU-stack=noload,readonly \
	  ssstik.mp4 ssstik_mp4.o && \
	rm -f ssstik.mp4

$(SSSTIK_KVID_OBJ): $(SSSTIK_KVID_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/ssstik.kvid && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --add-section .note.GNU-stack=/dev/null \
	  --set-section-flags .note.GNU-stack=noload,readonly \
	  ssstik.kvid ssstik_kvid.o && \
	rm -f ssstik.kvid

# ─── Compile assembly ─────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@
# ─── User programs: assemble NASM → link with user.ld → embed via objcopy ──────
$(BUILD)/userprogs/%.elf: $(USERPROGS_DIR)/%.asm $(USERPROGS_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $(BUILD)/userprogs/$*.o
	$(LD) -nostdlib -static -T $(USERPROGS_DIR)/user.ld -o $@ $(BUILD)/userprogs/$*.o

$(HELLO_USER_OBJ): $(HELLO_USER_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/hello.elf.bin
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  hello.elf.bin hello_user_embed.o
	rm -f $(BUILD)/userprogs/hello.elf.bin

$(HELLO_X64_OBJ): $(HELLO_X64_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/hello_x64.elf.bin
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  hello_x64.elf.bin hello_x64_user_embed.o
	rm -f $(BUILD)/userprogs/hello_x64.elf.bin

# embed the prebuilt musl-static hello blob (satoru)
$(HELLO_MUSL_OBJ): $(HELLO_MUSL_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/hello_musl.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  hello_musl.elf hello_musl_user_embed.o
	rm -f $(BUILD)/userprogs/hello_musl.elf

# embed the prebuilt musl-static ffmpeg blob (satoru)
$(FFMPEG_OBJ): $(FFMPEG_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/ffmpeg.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  ffmpeg.elf ffmpeg_user_embed.o
	rm -f $(BUILD)/userprogs/ffmpeg.elf

# embed the wl_shm wayland test client blob (satoru)
$(WLSHM_OBJ): $(WLSHM_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/wl_shm_test.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  wl_shm_test.elf wl_shm_test_user_embed.o
	rm -f $(BUILD)/userprogs/wl_shm_test.elf

# embed the pthreads smoke-test blob (satoru)
$(PTHREAD_OBJ): $(PTHREAD_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/pthread_test.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  pthread_test.elf pthread_test_user_embed.o
	rm -f $(BUILD)/userprogs/pthread_test.elf

# embed the dynamic musl pie blob (first ld-kurono dynamic-load test) (satoru)
$(DYNTEST_OBJ): $(DYNTEST_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/dyntest.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  dyntest.elf dyntest_user_embed.o
	rm -f $(BUILD)/userprogs/dyntest.elf

# embed musl libc.so so ld-kurono can resolve the dynamic pie's DT_NEEDED (satoru)
$(MUSL_LIBC_OBJ): $(MUSL_LIBC_SO)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/musl_libc.so
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  musl_libc.so musl_libc_user_embed.o
	rm -f $(BUILD)/userprogs/musl_libc.so

# embed libfoo.so  -  the dyntest pie's extra DT_NEEDED (file-backed mmap test) (satoru)
$(LIBFOO_OBJ): $(LIBFOO_SO)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/libfoo.so
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  libfoo.so libfoo_user_embed.o
	rm -f $(BUILD)/userprogs/libfoo.so

# kpy: compile C with no libc, no PIE, link with user.ld @ 0x40100000
$(KPY_ELF): $(KPY_C_SRC) $(USERPROGS_DIR)/user.ld
	@mkdir -p $(dir $@)
	gcc -m64 -static -nostdlib -nostartfiles -ffreestanding \
	    -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	    -mno-red-zone -mno-sse -mno-mmx -O2 -Wall -Wextra \
	    -T $(USERPROGS_DIR)/user.ld \
	    $(KPY_C_SRC) -o $@

$(KPY_OBJ): $(KPY_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/kpython.elf.bin
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kpython.elf.bin kpython_user_embed.o
	rm -f $(BUILD)/userprogs/kpython.elf.bin
# ─── Compile C++ ──────────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Files exercised from interrupt context  -  disable SSE codegen so the
# compiler doesn't emit movaps/movdqa that fault on partially-aligned
# kernel stack/heap when reached via int 0x80. Graphics inline asm
# uses xmm explicitly, so we can't apply this globally.
$(BUILD)/fs/kvfs.o: $(SRC)/fs/kvfs.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/proc/scheduler.o: $(SRC)/proc/scheduler.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/proc/kernel_processes.o: $(SRC)/proc/kernel_processes.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/linux/linux_syscall.o: $(SRC)/linux/linux_syscall.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/linux/linux_syscall_x64.o: $(SRC)/linux/linux_syscall_x64.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/kernel/userspace.o: $(SRC)/kernel/userspace.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/kernel/elf_loader.o: $(SRC)/kernel/elf_loader.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/hal/hal.o: $(SRC)/hal/hal.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

# Task Manager Refresh runs every 300 frames from GUIProcessEntry (a
# kernel process). Its SortProcesses bubble-sort vectorises the
# TMProcess struct swap into movaps which fault when the kernel
# process stack drifts off 16-byte alignment.  Force scalar codegen.
$(BUILD)/apps/task_manager.o: $(SRC)/apps/task_manager.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

# Video player runs every frame from GUIProcess and decodes JPEGs via
# stb_image which contains aligned SSE intrinsics.  Same alignment risk
# as task_manager  -  disable codegen-emitted SSE.
$(BUILD)/media/video_player.o: $(SRC)/media/video_player.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

$(BUILD)/apps/denji_app.o: $(SRC)/apps/denji_app.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

# stb_image decoders contain optional SSE2 paths and emit aligned
# movdqa when SSE codegen is enabled.  Kernel-context JPEG decode
# during denji playback hit those instructions with a slightly
# misaligned stack.  Force scalar build.
$(BUILD)/third_party/stb_image_glue.o: $(SRC)/third_party/stb_image_glue.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -DSTBI_NO_SIMD -c $< -o $@

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

# binutils 2.46 removed the efi-app-x86_64 bfd alias; pei-x86-64 + efi-app subsystem is the replacement (satoru)
$(EFI_APP): $(BUILD)/boot/efi_loader.so
	$(EFI_OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	    -O pei-x86-64 --subsystem efi-app $< $@

$(BUILD)/boot/install_efi_embed.o: $(EFI_APP)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono_installer_efi.efi && \
	cd $(BUILD)/boot && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_installer_efi.efi install_efi_embed.o

# binutils 2.46 removed the efi-app-x86_64 bfd alias; pei-x86-64 + efi-app subsystem is the replacement (satoru)
$(EFI_APP_EMERGENCY): $(BUILD)/boot/efi_loader_emergency.so
	$(EFI_OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	    -O pei-x86-64 --subsystem efi-app $< $@

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
	cp $(KERNEL_BASE) $(ISO_DIR)/boot/kurono_base.elf
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
	@echo 'set timeout=$(KURONO_GRUB_TIMEOUT)'                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '# Auto-select EFI Direct on UEFI, Multiboot2 on BIOS'      >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'if [ "$$grub_platform" = "efi" ]; then'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set default=0'                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'else'                                                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set default=$(KURONO_BIOS_DEFAULT)'                      >> $(ISO_DIR)/boot/grub/grub.cfg
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
	@echo '    multiboot2 /boot/kurono_base.elf $(if $(filter 1,$(KURONO_AUTOLOGIN)),kurono.autologin=1,) $(if $(strip $(KURONO_GUI_RUN)),kurono.gui.run="$(KURONO_GUI_RUN)",) $(KURONO_EXTRA_CMDLINE)' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Kernel loaded OK. Booting..."'                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono Setup" {'                                 >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono Setup (installer / first-setup)..."' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono_base.elf kurono.setup=1 kurono.autologin=1' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Setup kernel loaded OK. Booting..."'              >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Emergency Multiboot2)" {'             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono Emergency kernel (multiboot2)..."' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=text'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono_base.elf kurono_mode=emergency kurono.text=1' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Emergency kernel loaded OK. Booting..."'          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (CLI Multiboot2)" {'                   >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono CLI kernel (multiboot2)..."'       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=text'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono_base.elf $(KURONO_CLI_CMDLINE)'  >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "CLI kernel loaded OK. Booting..."'                >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Multiboot1)" {'                       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Loading Kurono OS kernel (multiboot1)..."'         >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot /boot/kurono_base.elf'                         >> $(ISO_DIR)/boot/grub/grub.cfg
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
	@echo '    multiboot2 /boot/kurono_base.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Kernel loaded. Press any key to boot..."'          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    sleep 5'                                                 >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (Text Mode - no framebuffer)" {'       >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Text mode boot..."'                                >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=text'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono_base.elf kurono_mode=text kurono.text=1' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    boot'                                                    >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}'                                                          >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo ''                                                           >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Kurono OS (gfxpayload=keep)" {'                  >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    echo "Keep mode boot..."'                                >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep'                                     >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kurono_base.elf'                             >> $(ISO_DIR)/boot/grub/grub.cfg
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
