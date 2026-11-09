# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS - Cross-Compilation Makefile (x86_64 bare metal)
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
#    make          - build kernel ELF binary
#    make iso      - build bootable ISO
#    make run      - run in QEMU with BGA display
#    make clean    - remove build artefacts
#    make debug    - run in QEMU with GDB stub
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
# the background vm's kernel + initramfs (Alpine 3.23.8 netboot 6.18.60 + the
# kurono initramfs, see scratchpad/agent_alp_guest) live in ../Alpine as
# ALPKERN.BIN / ALPINIT.BIN; the old stock vmlinuz-virt / initramfs-virt pair is
# only the fallback when those are missing. (satoru)
ALPINE_KERNEL_BIN  = $(firstword $(wildcard $(ALPINE_DIR)/ALPKERN.BIN) $(ALPINE_DIR)/vmlinuz-virt)
ALPINE_INITRD_BIN  = $(firstword $(wildcard $(ALPINE_DIR)/ALPINIT.BIN) $(ALPINE_DIR)/initramfs-virt)
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
#  Off by default - the rootfs is ~108 MB and bloats the ISO.  Users
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

# ─── xkeyboard-config data (embedded via objcopy) ──────────────────────────
# gtk/gecko builds a default xkb keymap from names (evdev/pc105/us) at wl_seat
# init via xkbcommon, which needs the xkeyboard-config data the firefox tar
# omits; without it gdk g_error()s and gecko aborts. embedded ustar tree,
# unpacked to kvfs + pointed at by XKB_CONFIG_ROOT at firefox launch. (satoru)
XKB_TAR_BIN  = $(SRC)/assets_xkb.tar
XKB_TAR_OBJ  = $(BUILD)/kurono_xkb.o
ifneq ($(wildcard $(XKB_TAR_BIN)),)
MEDIA_OBJS   += $(XKB_TAR_OBJ)
endif

# ─── DejaVu fonts data (embedded via objcopy) ──────────────────────────────
# gecko's gfxPlatformFontList::GetDefaultFontLocked MOZ_RELEASE_ASSERTs when there
# are zero font families; kurono shipped only a .ttf placeholder. embed real
# DejaVu TTFs (sans/serif/mono), unpacked to /system/fonts at firefox launch so
# fontconfig has a real default font. (satoru)
FONTS_TAR_BIN = $(SRC)/assets_fonts.tar
FONTS_TAR_OBJ = $(BUILD)/kurono_fonts.o
ifneq ($(wildcard $(FONTS_TAR_BIN)),)
MEDIA_OBJS   += $(FONTS_TAR_OBJ)
endif

# ─── NSS runtime libs (embedded via objcopy) ────────────────────────────────
# firefox's libsoftokn3.so has DT_NEEDED libsqlite3.so.0 which the firefox tar
# does NOT ship (its libmozsqlite3.so exports nothing), so NSS/PSM init died
# before opening cert9.db - the "Personal Security Manager" / https blocker.
# embed a musl-built libsqlite3.so.0, unpacked to /apps/firefox/lib at firefox
# launch (self-heals persisted installs too). (satoru)
NSSLIBS_TAR_BIN = $(SRC)/assets_nsslibs.tar
NSSLIBS_TAR_OBJ = $(BUILD)/kurono_nsslibs.o
ifneq ($(wildcard $(NSSLIBS_TAR_BIN)),)
MEDIA_OBJS   += $(NSSLIBS_TAR_OBJ)
endif


# ─── ICU common data (embedded via objcopy) ───────────────────────────────
# firefox links alpine's system icu, whose libicudata is a stub; without the
# real icudt78l.dat u_init fails and gecko MOZ_CRASHes with "ICU4CLibrary::
# Initialize() failed". written to /apps/firefox at firefox launch.
ICU_DAT_BIN = $(SRC)/assets/icu/icudt78l.dat
ICU_DAT_OBJ = $(BUILD)/kurono_icudt.o
ifneq ($(wildcard $(ICU_DAT_BIN)),)
MEDIA_OBJS  += $(ICU_DAT_OBJ)
endif

# ─── gdk-pixbuf 2.42 (embedded via objcopy) ───────────────────────────────
# the firefox package's gdk-pixbuf 2.44 decodes through a stubbed-out glycin,
# so gtk aborts on its first icon. 2.42 has png/jpeg built in; written over the
# package copy at firefox launch. see assets/gdk-pixbuf/README.txt.
PIXBUF_BIN = $(SRC)/assets/gdk-pixbuf/libgdk_pixbuf-2.0.so.0
PIXBUF_OBJ = $(BUILD)/kurono_pixbuf.o
ifneq ($(wildcard $(PIXBUF_BIN)),)
MEDIA_OBJS += $(PIXBUF_OBJ)
endif

# ─── Inter UI typeface (embedded via objcopy) ─────────────────────────────
# the modern ui style renders all text with inter regular + semibold through
# the stb_truetype engine. sil open font license 1.1, see assets/fonts/OFL.txt.
# (satoru)
INTER_REG_BIN  = $(SRC)/assets/fonts/Inter-Regular.otf
INTER_SEMI_BIN = $(SRC)/assets/fonts/Inter-SemiBold.otf
INTER_REG_OBJ  = $(BUILD)/inter_regular.o
INTER_SEMI_OBJ = $(BUILD)/inter_semibold.o
MEDIA_OBJS    += $(INTER_REG_OBJ) $(INTER_SEMI_OBJ)
# dejavu sans mono for the terminal grid (bitstream vera license, see
# assets/fonts/LICENSE-DejaVu.txt). (satoru)
MONO_BIN       = $(SRC)/assets/fonts/DejaVuSansMono.ttf
MONO_OBJ       = $(BUILD)/dejavu_mono.o
MEDIA_OBJS    += $(MONO_OBJ)
# modern app icon artwork: 128 px premultiplied bitmaps rasterized from
# assets/icons/*.svg by tools/icons/icons_to_blob.py. (satoru)
ICONS_BIN      = $(SRC)/assets/icons/modern_icons.bin
ICONS_OBJ      = $(BUILD)/modern_icons.o
MEDIA_OBJS    += $(ICONS_OBJ)

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
# signal-delivery self-test (proves rt_sigaction/procmask/sigreturn + kill). built
# from source with musl-gcc (unlike the prebuilt .elf blobs) then embedded. (satoru)
MUSLCC            = musl-gcc
SIGNAL_SRC        = $(USERPROGS_DIR)/signal_test.c
SIGNAL_ELF        = $(BUILD)/userprogs/signal_test.elf
IRQLAT_SRC        = $(USERPROGS_DIR)/irqlat.c
IRQLAT_ELF        = $(BUILD)/userprogs/irqlat.elf
IRQLAT_OBJ        = $(BUILD)/userprogs/irqlat_user_embed.o
# kld phase-c network a/b instrument (kurono.kld.bench=1) (satoru)
KLDBENCH_SRC      = $(USERPROGS_DIR)/kldnetbench.c
KLDBENCH_ELF      = $(BUILD)/userprogs/kldnetbench.elf
KLDBENCH_OBJ      = $(BUILD)/userprogs/kldnetbench_user_embed.o
# kld acpica container, prebuilt by kld/acpi/Makefile (make kurono) (satoru)
KLDACPI_ELF       = $(USERPROGS_DIR)/kld_acpi.elf
KLDACPI_OBJ       = $(if $(wildcard $(KLDACPI_ELF)),$(BUILD)/userprogs/kld_acpi_user_embed.o,)
UATEST_SRC        = $(USERPROGS_DIR)/uatest.c
UATEST_ELF        = $(BUILD)/userprogs/uatest.elf
UATEST_OBJ        = $(BUILD)/userprogs/uatest_user_embed.o
KENTEST_SRC       = $(USERPROGS_DIR)/kentest.c
KENTEST_ELF       = $(BUILD)/userprogs/kentest.elf
KENTEST_OBJ       = $(BUILD)/userprogs/kentest_user_embed.o
# ring-3 memory fill + verify across most of free ram (kurono.memtest=1). the
# user-space proof for the pmm managing ram above the old 16 gb cap. (satoru)
MEMTEST_SRC       = $(USERPROGS_DIR)/memtest.c
MEMTEST_ELF       = $(BUILD)/userprogs/memtest.elf
MEMTEST_OBJ       = $(BUILD)/userprogs/memtest_user_embed.o
# fork/exec/mmap/thread churn for the leak census (kurono.churn=1). (satoru)
CHURN_SRC         = $(USERPROGS_DIR)/churn.c
CHURN_ELF         = $(BUILD)/userprogs/churn.elf
CHURN_OBJ         = $(BUILD)/userprogs/churn_user_embed.o
KSHTEST_SRC       = $(USERPROGS_DIR)/kshtest.c
KSHTEST_ELF       = $(BUILD)/userprogs/kshtest.elf
KSHTEST_OBJ       = $(BUILD)/userprogs/kshtest_user_embed.o
# third-party linux binary smoke test (kurono.bbtest=1). a prebuilt static
# busybox: the point is that we did NOT compile it, so it exercises the kls
# syscall surface the way a real recovery tool would. (satoru)
BBOX_ELF          = $(USERPROGS_DIR)/busybox.elf
# optional: the blob is not committed, so a fresh clone links without it (satoru)
BBOX_OBJ          = $(if $(wildcard $(BBOX_ELF)),$(BUILD)/userprogs/busybox_user_embed.o,)
# ai recovery client. links the vendored musl bearssl; see
# src/third_party/bearssl/README.md for why debian's copy cannot be used. (satoru)
BEARSSL_DIR       = $(SRC)/third_party/bearssl
DSAI_SRC          = $(USERPROGS_DIR)/dsai.c
DSAI_ELF          = $(BUILD)/userprogs/dsai.elf
DSAI_OBJ          = $(if $(wildcard $(DSAI_SRC)),$(BUILD)/userprogs/dsai_user_embed.o,)
# optional api key baked into the image at /system/etc/deepseek.key.
# DELIBERATELY EMPTY BY DEFAULT - opt in per build with
#   make iso DSAI_KEY_FILE=/var/tmp/deepseek.key
# pointing this at a real path by default would silently embed a live
# credential in every iso built on a machine that happens to have the file.
# (satoru)
DSAI_KEY_FILE    ?=
# https downloader for `kpkg install claude` (musl-static + bearssl, like dsai).
# optional like the other blobs: without musl-gcc the build still links and kpkg
# says the helper is missing. (satoru)
KFETCH_SRC        = $(USERPROGS_DIR)/kfetch.c
KFETCH_ELF        = $(BUILD)/userprogs/kfetch.elf
KFETCH_OBJ        = $(if $(wildcard $(KFETCH_SRC)),$(BUILD)/userprogs/kfetch_user_embed.o,)
DSAI_KEY_OBJ      = $(if $(wildcard $(DSAI_KEY_FILE)),$(BUILD)/userprogs/dsai_key_embed.o,)
# recovery toolbox: a ustar of musl-static rescue tools, unpacked to /usr/bin
# at boot. built by scratchpad/build_rtools.sh; optional like the other blobs. (satoru)
RTOOLS_TAR        = $(USERPROGS_DIR)/recovery_tools.tar
RTOOLS_OBJ        = $(if $(wildcard $(RTOOLS_TAR)),$(BUILD)/userprogs/recovery_tools_embed.o,)
KLDE1K_ELF        = $(USERPROGS_DIR)/kld_e1000e.elf
# optional: build output of kld/Makefile, only linked when present (satoru)
KLDE1K_OBJ        = $(if $(wildcard $(KLDE1K_ELF)),$(BUILD)/userprogs/kld_e1000e_user_embed.o,)
SIGNAL_OBJ        = $(BUILD)/userprogs/signal_test_user_embed.o
# parallel tree-walk regression for the claude find/Glob/Grep freeze (kurono.findtest=1).
# musl-static like signal_test; walks /system /usr /home with getdents64 + fstatat. (satoru)
FINDTEST_SRC      = $(USERPROGS_DIR)/findtest.c
FINDTEST_ELF      = $(BUILD)/userprogs/findtest.elf
FINDTEST_OBJ      = $(BUILD)/userprogs/findtest_user_embed.o
# dynamic musl pie + its libc.so: first real exercise of the ld-kurono dynamic path (satoru)
DYNTEST_ELF       = $(USERPROGS_DIR)/dyntest.elf
DYNTEST_OBJ       = $(BUILD)/userprogs/dyntest_user_embed.o
MUSL_LIBC_SO      = $(USERPROGS_DIR)/musl_libc.so
MUSL_LIBC_OBJ     = $(BUILD)/userprogs/musl_libc_user_embed.o
# tiny extra .so the dyntest pie depends on - exercises musl loading a non-interp
# library via the new file-backed mmap path (satoru)
LIBFOO_SO         = $(USERPROGS_DIR)/libfoo.so
LIBFOO_OBJ        = $(BUILD)/userprogs/libfoo_user_embed.o
# libkurono-ui ring-3 apps: the same app source as the kernel build, compiled with g++ against musl
# (no libstdc++: no exceptions, no rtti, kui supplies new/delete) and linked static. the kernel's own
# ui/font.cpp and stb glue go in unchanged, so text matches the in-kernel app pixel for pixel. (satoru)
KUI_R3_CXX        = g++ -specs=/usr/lib/x86_64-linux-musl/musl-gcc.specs
KUI_R3_FLAGS      = -nostdinc++ -std=c++17 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                    -fno-math-errno -fno-stack-protector -fno-use-cxa-atexit -DKUI_BACKEND_WAYLAND -I$(SRC)
KUI_R3_LIB        = $(SRC)/kui/kui_raster.cpp $(SRC)/kui/kui_canvas.cpp $(SRC)/kui/ring3/kui_wayland.cpp \
                    $(SRC)/kui/ring3/kui_r3_text.cpp $(SRC)/kui/ring3/kui_r3_audio.cpp \
                    $(SRC)/kui/ring3/kui_r3_launch.cpp \
                    $(SRC)/ui/font.cpp $(SRC)/third_party/stb_truetype_glue.cpp
KUI_R3_DEPS       = $(KUI_R3_LIB) $(wildcard $(SRC)/kui/*.h) $(SRC)/ui/font.h $(SRC)/ui/vga_font.h
KUI_CALC_ELF      = $(BUILD)/userprogs/kui_calculator.elf
KUI_CALC_OBJ      = $(BUILD)/userprogs/kui_calculator_user_embed.o
KUI_EDITOR_ELF    = $(BUILD)/userprogs/kui_texteditor.elf
KUI_EDITOR_OBJ    = $(BUILD)/userprogs/kui_texteditor_user_embed.o
KUI_FILES_ELF     = $(BUILD)/userprogs/kui_files.elf
KUI_FILES_OBJ     = $(BUILD)/userprogs/kui_files_user_embed.o
KUI_TASKMGR_ELF   = $(BUILD)/userprogs/kui_taskmgr.elf
KUI_TASKMGR_OBJ   = $(BUILD)/userprogs/kui_taskmgr_user_embed.o
KUI_MEDIA_ELF     = $(BUILD)/userprogs/kui_mediaplayer.elf
KUI_MEDIA_OBJ     = $(BUILD)/userprogs/kui_mediaplayer_user_embed.o
KUI_DENJI_ELF     = $(BUILD)/userprogs/kui_denji.elf
KUI_DENJI_OBJ     = $(BUILD)/userprogs/kui_denji_user_embed.o
KUI_TERM_ELF      = $(BUILD)/userprogs/kui_terminal.elf
KUI_TERM_OBJ      = $(BUILD)/userprogs/kui_terminal_user_embed.o
KUI_SETTINGS_ELF  = $(BUILD)/userprogs/kui_settings.elf
KUI_SETTINGS_OBJ  = $(BUILD)/userprogs/kui_settings_user_embed.o
# the audio window needs the codec registry (wav/mp3/aac/flac/mp4); the video player needs the
# kvid/mp4/h264/aac parsers and stb_image for the jpeg frames. (satoru)
KUI_R3_MEDIA_SRC  = $(SRC)/apps/media_player.cpp $(SRC)/apps/media_audio.cpp \
                    $(SRC)/media/codec.cpp $(SRC)/media/mp3_decoder.cpp
KUI_R3_DENJI_SRC  = $(SRC)/apps/denji_app.cpp $(SRC)/apps/media_text.cpp \
                    $(SRC)/media/video_player.cpp $(SRC)/media/kvid.cpp \
                    $(SRC)/media/mp4_demux.cpp $(SRC)/media/h264_parse.cpp \
                    $(SRC)/media/aac_parse.cpp $(SRC)/third_party/stb_image_glue.cpp
# the settings shell, the control kit and every page source, unchanged. (satoru)
KUI_R3_SETTINGS_SRC = $(SRC)/apps/settings_r3_main.cpp $(SRC)/apps/system_settings.cpp \
                    $(SRC)/apps/settings_kit.cpp \
                    $(SRC)/apps/settings_mod_a11y.cpp $(SRC)/apps/settings_mod_about.cpp \
                    $(SRC)/apps/settings_mod_audio.cpp $(SRC)/apps/settings_mod_devices.cpp \
                    $(SRC)/apps/settings_mod_display.cpp $(SRC)/apps/settings_mod_linux.cpp \
                    $(SRC)/apps/settings_mod_network.cpp $(SRC)/apps/settings_mod_personalize.cpp \
                    $(SRC)/apps/settings_mod_power.cpp \
                    $(SRC)/apps/settings_mod_security.cpp $(SRC)/apps/settings_mod_storage.cpp \
                    $(SRC)/apps/settings_mod_system.cpp
USERPROG_OBJS     = $(KUI_CALC_OBJ) $(KUI_EDITOR_OBJ) $(KUI_FILES_OBJ) $(KUI_TASKMGR_OBJ) $(KUI_MEDIA_OBJ) $(KUI_DENJI_OBJ) $(KUI_TERM_OBJ) $(KUI_SETTINGS_OBJ) $(HELLO_USER_OBJ) $(HELLO_X64_OBJ) $(HELLO_MUSL_OBJ) $(KPY_OBJ) $(FFMPEG_OBJ) $(WLSHM_OBJ) $(PTHREAD_OBJ) $(SIGNAL_OBJ) $(IRQLAT_OBJ) $(KLDBENCH_OBJ) $(KLDACPI_OBJ) $(UATEST_OBJ) $(KENTEST_OBJ) $(MEMTEST_OBJ) $(CHURN_OBJ) $(KSHTEST_OBJ) $(FINDTEST_OBJ) $(BBOX_OBJ) $(DSAI_OBJ) $(DSAI_KEY_OBJ) $(KFETCH_OBJ) $(RTOOLS_OBJ) $(KLDE1K_OBJ) $(DYNTEST_OBJ) $(MUSL_LIBC_OBJ) $(LIBFOO_OBJ)

# ─── KURONO_SLIM=1: drop the big optional blobs from the kernel image ──────
# MEASURED 2026-07-27: 72% of boot (21.4s of 29.7s wall) is grub loading the
# kernel BEFORE the cpu runs a single instruction. .text is only 2.2MB - the
# image is 79MB because ~52MB of assets are objcopy'd into .rodata, and grub
# loads every PT_LOAD byte at ~3.7MB/s over qemu's atapi pio path.
#
# everything dropped here is declared __attribute__((weak)) with a Has*()
# null-guard at its use site (embedded_media.h, embedded_userprogs.h,
# alpine_data.h), so the link succeeds and the features degrade gracefully
# rather than breaking:
#   demo videos     ~12.9MB  media player finds no clip
#   alpine guest    ~22.9MB  hypervisor has no built-in guest image
#   kpython          ~8.3MB  no built-in python interpreter
# fonts / xkb / nss libs are deliberately KEPT - firefox needs them.
# (satoru)
ifeq ($(KURONO_SLIM),1)
MEDIA_OBJS    := $(filter-out $(DENJI_MP4_OBJ) $(DENJI_KVID_OBJ) $(SSSTIK_MP4_OBJ) $(SSSTIK_KVID_OBJ),$(MEDIA_OBJS))
ALPINE_OBJS   :=
DEBIAN_OBJS   :=
USERPROG_OBJS := $(filter-out $(KPY_OBJ),$(USERPROG_OBJS))
endif

# --- on-demand assets (default): the big optional payloads are NOT linked into
# the kernel any more. they ship as root files on the KURONO partition (make pack
# puts them in ../build/pack) and system/kasset.cpp loads each on first use.
# KURONO_EMBED_ALL=1 links them in like before. (satoru)
ifneq ($(KURONO_EMBED_ALL),1)
MEDIA_OBJS    := $(filter-out $(DENJI_MP4_OBJ) $(DENJI_KVID_OBJ) $(SSSTIK_MP4_OBJ) $(SSSTIK_KVID_OBJ),$(MEDIA_OBJS))
ALPINE_OBJS   :=
USERPROG_OBJS := $(filter-out $(KPY_OBJ) $(FFMPEG_OBJ) $(RTOOLS_OBJ) $(if $(filter 1,$(KURONO_KLD_EMBED)),,$(KLDE1K_OBJ)),$(USERPROG_OBJS))
endif
# KURONO_KLD_EMBED=1 keeps the KLD driver container linked in even in the default
# on-demand build, for vm tests that boot from a cd with no KURONO partition. (satoru)

# ─── Flags ────────────────────────────────────────────────────────────────
CXXFLAGS = -std=c++17 -ffreestanding -O2 -Wall -Wextra -fno-exceptions \
           -fno-rtti -fno-stack-protector -fno-use-cxa-atexit \
           -mno-red-zone -m64 -mcmodel=large -fno-pie -I$(SRC) -g

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

# C++ sources - listed in dependency order for clarity
CXX_SRCS = \
    $(SRC)/kernel/types.cpp \
    $(SRC)/kernel/heap.cpp \
    $(SRC)/kernel/pmm.cpp \
    $(SRC)/kernel/buddy.cpp \
    $(SRC)/kernel/slab.cpp \
    $(SRC)/kernel/vmm.cpp \
    $(SRC)/kernel/kmemx_lz4.cpp \
    $(SRC)/kernel/kmemx_pool.cpp \
    $(SRC)/kernel/kmemx.cpp \
    $(SRC)/kernel/kmemx_process.cpp \
    $(SRC)/kernel/kmemx_config.cpp \
    $(SRC)/kernel/kmemx_test.cpp \
    $(SRC)/kernel/memcensus.cpp \
    $(SRC)/kernel/taskmem.cpp \
    $(SRC)/kernel/memacct.cpp \
    $(SRC)/kernel/kdf.cpp \
    $(SRC)/kernel/kdf_test.cpp \
    $(SRC)/kernel/udf.cpp \
    $(SRC)/kernel/kld_broker.cpp \
    $(SRC)/kernel/kld_net.cpp \
    $(SRC)/kernel/irp.cpp \
    $(SRC)/kernel/kexec.cpp \
	$(SRC)/kernel/userspace.cpp \
	$(SRC)/kernel/elf_loader.cpp \
    $(SRC)/kernel/time.cpp \
    $(SRC)/kernel/kvdso.cpp \
    $(SRC)/kernel/system.cpp \
	$(SRC)/kernel/panic.cpp \
    $(SRC)/kernel/hrtimer.cpp \
    $(SRC)/hal/hal.cpp \
    $(SRC)/drivers/serial.cpp \
    $(SRC)/drivers/display.cpp \
    $(SRC)/drivers/graphics.cpp \
    $(SRC)/drivers/outputs.cpp \
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
    $(SRC)/security/ken.cpp \
    $(SRC)/security/ksa.cpp \
    $(SRC)/security/admin.cpp \
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
    $(SRC)/ui/wm_motion.cpp \
    $(SRC)/ui/boot_anim.cpp \
    $(SRC)/ui/style.cpp \
    $(SRC)/ui/modern_draw.cpp \
    $(SRC)/ui/desktop.cpp \
    $(SRC)/ui/control_center.cpp \
    $(SRC)/ui/file_browser.cpp \
    $(SRC)/ui/notification.cpp \
    $(SRC)/ui/perf_hud.cpp \
    $(SRC)/ui/frame_pacer.cpp \
    $(SRC)/ui/clipboard_ui.cpp \
    $(SRC)/apps/calculator.cpp \
    $(SRC)/kui/kui_raster.cpp \
    $(SRC)/kui/kui_canvas.cpp \
    $(SRC)/kui/kui_kernel.cpp \
    $(SRC)/kui/kui_launch.cpp \
    $(SRC)/apps/terminal.cpp \
    $(SRC)/apps/file_manager.cpp \
    $(SRC)/apps/text_editor.cpp \
    $(SRC)/apps/settings.cpp \
    $(SRC)/apps/system_settings.cpp \
    $(SRC)/apps/settings_kit.cpp \
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
    $(SRC)/apps/settings_mod_linux.cpp \
    $(SRC)/apps/task_manager.cpp \
    $(SRC)/apps/browser.cpp \
    $(SRC)/apps/media_player.cpp \
    $(SRC)/apps/media_audio.cpp \
    $(SRC)/apps/kui_audio_kernel.cpp \
    $(SRC)/apps/denji_app.cpp \
    $(SRC)/apps/media_text.cpp \
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
	$(SRC)/system/claude_host.cpp \
	$(SRC)/system/ktools.cpp \
	$(SRC)/system/kasset.cpp \
	$(SRC)/system/kdaemons.cpp \
	$(SRC)/system/installer.cpp \
	$(SRC)/system/installer_gui.cpp \
	$(SRC)/system/gpu_driver_installer.cpp \
	$(SRC)/system/system_update.cpp \
	$(SRC)/system/ui_config.cpp \
	$(SRC)/system/settings_service.cpp \
	$(SRC)/system/vconsole.cpp \
    $(SRC)/tests/test_suite.cpp \
    $(SRC)/linux/kls.cpp \
    $(SRC)/linux/linux_syscall.cpp \
    $(SRC)/linux/kpty.cpp \
    $(SRC)/apps/vt.cpp \
    $(SRC)/linux/uaccess.cpp \
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
    $(SRC)/virt/virtio_dev.cpp \
    $(SRC)/virt/vblk.cpp \
    $(SRC)/virt/vm_host.cpp \
    $(SRC)/virt/vcon.cpp \
    $(SRC)/virt/vnet.cpp \
    $(SRC)/virt/wlbridge.cpp \
    $(SRC)/virt/vaudio.cpp \
    $(SRC)/apps/alpine_term.cpp \
    $(SRC)/virt/pci_passthrough.cpp \
    $(SRC)/drivers/nvidia_gpu.cpp \
    $(SRC)/drivers/intel_gpu.cpp \
    $(SRC)/drivers/e1000.cpp \
    $(SRC)/drivers/igc.cpp \
    $(SRC)/drivers/wifi_dev.cpp \
    $(SRC)/drivers/wifi_rtw.cpp \
    $(SRC)/drivers/wifi_ath.cpp \
    $(SRC)/drivers/wifi_iwl.cpp \
    $(SRC)/drivers/wifi_brcm.cpp \
    $(SRC)/drivers/nvme.cpp \
    $(SRC)/drivers/blockdev.cpp \
    $(SRC)/drivers/ahci.cpp \
    $(SRC)/drivers/disklog.cpp \
    $(SRC)/drivers/hwinfo.cpp \
    $(SRC)/drivers/usb.cpp \
    $(SRC)/drivers/usb_audio.cpp \
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
    $(SRC)/drivers/intel_display_probe.cpp \
    $(SRC)/drivers/intel_dp_aux.cpp \
    $(SRC)/drivers/dp_edid_decode.cpp \
    $(SRC)/drivers/intel_dp_lt.cpp \
    $(SRC)/drivers/intel_modeset_calc.cpp \
    $(SRC)/drivers/intel_modeset.cpp \
    $(SRC)/drivers/intel_accel.cpp \
    $(SRC)/drivers/cpu_detect.cpp \
    $(SRC)/media/mp3_decoder.cpp \
    $(SRC)/media/codec.cpp \
    $(SRC)/media/mp4_demux.cpp \
    $(SRC)/media/h264_parse.cpp \
    $(SRC)/media/aac_parse.cpp \
    $(SRC)/media/kvid.cpp \
    $(SRC)/media/video_player.cpp \
    $(SRC)/net/tcpip.cpp \
    $(SRC)/net/dhcp.cpp \
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
EFI_APP_GPU = $(BUILD)/boot/kurono_efi_gpu.efi
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
# firefox on the KLS needs AP thread-scheduling. the scheduler is cooperative
# with no preemption on the bsp, so without this every firefox worker thread
# (webrender compositor, rayon, ipc) timeshares cpu0 and any one that spins in
# userspace before parking (rust parking_lot) starves the rest - the chrome main
# wedges mid-startup and the window never paints (the boot-stall lottery). the
# whole prior smp render/futex/migration body of work was built and validated
# WITH kurono.apthreads=1, but the shipped grub cmdline was missing it. measured
# on this tree: with the flag firefox progresses ~2x further into window setup
# and 0 boots crashed; without it firefox goes quiet at ~t=66s. auto-enable it
# whenever we launch firefox; other gui targets are unaffected. (satoru)
KURONO_FF_APTHREADS := $(if $(findstring firefox,$(KURONO_GUI_RUN)),kurono.apthreads=1,)
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
.PHONY: all iso run debug clean dirs efi gpu-efi debian-rootfs

all: dirs $(KERNEL)
	@echo "══════════════════════════════════════════════"
	@echo "  Kurono OS kernel built: $(KERNEL)"
	@echo "══════════════════════════════════════════════"

# ─── Link ─────────────────────────────────────────────────────────────────

# ap trampoline object - must be defined before the link rules use it (make
# expands prerequisites at parse time). (satoru)
AP_TRAMP_OBJ = $(BUILD)/boot/ap_trampoline.o

# KURONO_KERNEL_BASE=0x14000000 links the kernel above the 0x200000..0x12e1d000 span of the
# claude code binary (a fixed-address ET_EXEC). unset = the classic 0x100000. (satoru)
KBASE_LDFLAG = $(if $(KURONO_KERNEL_BASE),-Wl$(comma)--defsym$(comma)KERNEL_BASE=$(KURONO_KERNEL_BASE),)
comma := ,
$(KERNEL_BASE): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS) $(USERPROG_OBJS) $(AP_TRAMP_OBJ)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld $(KBASE_LDFLAG) -nostdlib -o $@ $^ -lgcc

$(KERNEL): $(ALL_OBJS) $(ALPINE_OBJS) $(DEBIAN_OBJS) $(MEDIA_OBJS) $(USERPROG_OBJS) $(AP_TRAMP_OBJ) $(INSTALLER_PAYLOAD_OBJS)
	$(CXX) $(CXXFLAGS) -no-pie -T $(SRC)/boot/kurono_linker.ld $(KBASE_LDFLAG) -nostdlib -o $@ $^ -lgcc

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

# ─── Embed xkeyboard-config tar via objcopy (binary → ELF .rodata) ────────
$(XKB_TAR_OBJ): $(XKB_TAR_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/kurono_xkb && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_xkb kurono_xkb.o && \
	rm -f kurono_xkb

# ─── Embed DejaVu fonts tar via objcopy (binary → ELF .rodata) ────────────
$(FONTS_TAR_OBJ): $(FONTS_TAR_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/kurono_fonts && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_fonts kurono_fonts.o && \
	rm -f kurono_fonts

# ─── Embed NSS runtime libs tar via objcopy (binary → ELF .rodata) ────────
$(NSSLIBS_TAR_OBJ): $(NSSLIBS_TAR_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/kurono_nsslibs && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_nsslibs kurono_nsslibs.o && \
	rm -f kurono_nsslibs

# ─── Embed the ICU data via objcopy (binary → ELF .rodata) ────────────────
$(ICU_DAT_OBJ): $(ICU_DAT_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/kurono_icudt && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_icudt kurono_icudt.o && \
	rm -f kurono_icudt

# ─── Embed gdk-pixbuf 2.42 via objcopy (binary → ELF .rodata) ─────────────
$(PIXBUF_OBJ): $(PIXBUF_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/kurono_pixbuf && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kurono_pixbuf kurono_pixbuf.o && \
	rm -f kurono_pixbuf

# ─── Embed the Inter faces via objcopy (binary → ELF .rodata) ─────────────
$(INTER_REG_OBJ): $(INTER_REG_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/inter_regular_otf && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  inter_regular_otf inter_regular.o && \
	rm -f inter_regular_otf

$(INTER_SEMI_OBJ): $(INTER_SEMI_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/inter_semibold_otf && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  inter_semibold_otf inter_semibold.o && \
	rm -f inter_semibold_otf

$(MONO_OBJ): $(MONO_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/dejavu_mono_ttf && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  dejavu_mono_ttf dejavu_mono.o && \
	rm -f dejavu_mono_ttf

$(ICONS_OBJ): $(ICONS_BIN)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/modern_icons_bin && \
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  modern_icons_bin modern_icons.o && \
	rm -f modern_icons_bin

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

# the ring-3 calculator, /system/apps/calculator (kurono.r3apps=calculator). (satoru)
$(KUI_CALC_ELF): $(SRC)/apps/calculator.cpp $(SRC)/apps/calculator.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(SRC)/apps/calculator.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_CALC_OBJ): $(KUI_CALC_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_calculator.elf kui_calculator_user_embed.o

# the ring-3 text editor, Files and Task Manager, /system/apps/<name>. (satoru)
$(BUILD)/apps/text_editor.o: $(wildcard $(SRC)/apps/editor/*.h) $(wildcard $(SRC)/apps/editor/*.inc)
$(KUI_EDITOR_ELF): $(SRC)/apps/text_editor.cpp $(SRC)/apps/text_editor.h $(wildcard $(SRC)/apps/editor/*.h) $(wildcard $(SRC)/apps/editor/*.inc) $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(SRC)/apps/text_editor.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_EDITOR_OBJ): $(KUI_EDITOR_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_texteditor.elf kui_texteditor_user_embed.o
$(KUI_FILES_ELF): $(SRC)/apps/file_manager.cpp $(SRC)/apps/file_manager.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(SRC)/apps/file_manager.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_FILES_OBJ): $(KUI_FILES_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_files.elf kui_files_user_embed.o
$(KUI_TASKMGR_ELF): $(SRC)/apps/task_manager.cpp $(SRC)/apps/task_manager.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(SRC)/apps/task_manager.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_TASKMGR_OBJ): $(KUI_TASKMGR_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_taskmgr.elf kui_taskmgr_user_embed.o
$(KUI_MEDIA_ELF): $(KUI_R3_MEDIA_SRC) $(SRC)/apps/media_player.h $(SRC)/apps/media_audio.h $(SRC)/media/codec.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(KUI_R3_MEDIA_SRC) $(KUI_R3_LIB)
	strip $@
$(KUI_MEDIA_OBJ): $(KUI_MEDIA_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_mediaplayer.elf kui_mediaplayer_user_embed.o
$(KUI_DENJI_ELF): $(KUI_R3_DENJI_SRC) $(SRC)/apps/denji_app.h $(SRC)/apps/media_text.h $(SRC)/media/video_player.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(KUI_R3_DENJI_SRC) $(KUI_R3_LIB)
	strip $@
$(KUI_DENJI_OBJ): $(KUI_DENJI_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_denji.elf kui_denji_user_embed.o
# the ring-3 terminal: a plain pty emulator, /system/apps/terminal. the Vt emulator is the (satoru)
# same source the in-kernel terminal uses. (satoru)
$(KUI_TERM_ELF): $(SRC)/apps/terminal_r3.cpp $(SRC)/apps/vt.cpp $(SRC)/apps/vt.h $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(SRC)/apps/terminal_r3.cpp $(SRC)/apps/vt.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_TERM_OBJ): $(KUI_TERM_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_terminal.elf kui_terminal_user_embed.o
$(KUI_SETTINGS_ELF): $(KUI_R3_SETTINGS_SRC) $(SRC)/apps/system_settings.h $(SRC)/apps/settings_kit.h $(SRC)/kui/ring3/kui_r3_settings.cpp $(KUI_R3_DEPS)
	@mkdir -p $(dir $@)
	$(KUI_R3_CXX) $(KUI_R3_FLAGS) -static -no-pie -o $@ $(KUI_R3_SETTINGS_SRC) $(SRC)/kui/ring3/kui_r3_settings.cpp $(KUI_R3_LIB)
	strip $@
$(KUI_SETTINGS_OBJ): $(KUI_SETTINGS_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kui_settings.elf kui_settings_user_embed.o

# compile the signal-delivery self-test from source (musl-static, no-pie ET_EXEC)
# then embed it as a blob, same as the prebuilt userprogs (satoru)
$(SIGNAL_ELF): $(SIGNAL_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(SIGNAL_OBJ): $(SIGNAL_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  signal_test.elf signal_test_user_embed.o

# compile + embed the parallel tree-walk test (kurono.findtest=1). (satoru)
$(FINDTEST_ELF): $(FINDTEST_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(FINDTEST_OBJ): $(FINDTEST_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  findtest.elf findtest_user_embed.o

# compile + embed the userspace wake-latency probe (kurono.irqlat) - the KLD
# phase-0 go/no-go instrument. musl-static no-pie, same shape as signal_test. (satoru)
# embed the prebuilt kld acpica container (kurono.kld.acpi=1). (satoru)
$(KLDACPI_OBJ): $(KLDACPI_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/kld_acpi.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 --rename-section .data=.rodata,alloc,load,readonly,data,contents kld_acpi.elf kld_acpi_user_embed.o
	rm -f $(BUILD)/userprogs/kld_acpi.elf

# compile + embed the kld network a/b instrument (kurono.kld.bench=1). (satoru)
$(KLDBENCH_ELF): $(KLDBENCH_SRC) $(USERPROGS_DIR)/ds_tas.h
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $< -idirafter $(BEARSSL_DIR)/inc -idirafter /usr/include $(BEARSSL_DIR)/libbearssl.a
$(KLDBENCH_OBJ): $(KLDBENCH_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 --rename-section .data=.rodata,alloc,load,readonly,data,contents kldnetbench.elf kldnetbench_user_embed.o

$(IRQLAT_ELF): $(IRQLAT_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(IRQLAT_OBJ): $(IRQLAT_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 	  --rename-section .data=.rodata,alloc,load,readonly,data,contents 	  irqlat.elf irqlat_user_embed.o

# embed the KLD driver container (kurono.kld=1). built separately by kld/Makefile
# against the pinned linux tree; this only wraps the prebuilt binary. (satoru)
$(KLDE1K_OBJ): $(KLDE1K_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/kld_e1000e.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 	  --rename-section .data=.rodata,alloc,load,readonly,data,contents 	  kld_e1000e.elf kld_e1000e_user_embed.o
	rm -f $(BUILD)/userprogs/kld_e1000e.elf

# compile + embed the user-pointer validation regression test (kurono.uatest=1).
# proves uaccess actually rejects a kernel pointer rather than dereferencing it. (satoru)
$(UATEST_ELF): $(UATEST_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(UATEST_OBJ): $(UATEST_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  uatest.elf uatest_user_embed.o

# compile + embed the privilege-gate regression (kurono.kentest=1). (satoru)
$(KENTEST_ELF): $(KENTEST_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(KENTEST_OBJ): $(KENTEST_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kentest.elf kentest_user_embed.o

# compile + embed the ring-3 memory fill test (kurono.memtest=1). (satoru)
$(CHURN_ELF): $(CHURN_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $< -lpthread
$(CHURN_OBJ): $(CHURN_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  churn.elf churn_user_embed.o

$(MEMTEST_ELF): $(MEMTEST_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(MEMTEST_OBJ): $(MEMTEST_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  memtest.elf memtest_user_embed.o

# compile + embed the /dev/kshell gate (kurono.kshtest=1). proves the rescue
# agent's command channel runs a diagnostic AND refuses a destructive one - the
# kernel-side whitelist is a security boundary, so it needs a regression. (satoru)
$(KSHTEST_ELF): $(KSHTEST_SRC)
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $<
$(KSHTEST_OBJ): $(KSHTEST_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kshtest.elf kshtest_user_embed.o

$(RTOOLS_OBJ): $(RTOOLS_TAR)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/recovery_tools.tar
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  recovery_tools.tar recovery_tools_embed.o
	rm -f $(BUILD)/userprogs/recovery_tools.tar

$(BBOX_OBJ): $(BBOX_ELF)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/userprogs/busybox.elf
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  busybox.elf busybox_user_embed.o

# compile + embed the ai recovery client (kurono.dsai=1). musl-static against
# the vendored bearssl. -idirafter for BOTH include paths: a plain -I/usr/include
# jumps ahead of musl's own headers and you compile glibc's stdint.h against
# musl's libc. (satoru)
$(DSAI_ELF): $(DSAI_SRC) $(USERPROGS_DIR)/ds_tas.h
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $< \
	  -idirafter $(BEARSSL_DIR)/inc -idirafter /usr/include \
	  $(BEARSSL_DIR)/libbearssl.a
$(DSAI_OBJ): $(DSAI_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  dsai.elf dsai_user_embed.o

# kfetch: same toolchain recipe as dsai above. (satoru)
$(KFETCH_ELF): $(KFETCH_SRC) $(USERPROGS_DIR)/kf_tas.h
	@mkdir -p $(dir $@)
	$(MUSLCC) -static -no-pie -O2 -Wall -o $@ $< \
	  -idirafter $(BEARSSL_DIR)/inc -idirafter /usr/include \
	  $(BEARSSL_DIR)/libbearssl.a
$(KFETCH_OBJ): $(KFETCH_ELF)
	@mkdir -p $(dir $@)
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  kfetch.elf kfetch_user_embed.o

# wrap the api key as its own blob. built ONLY when DSAI_KEY_FILE exists, so
# the default image contains no key at all. (satoru)
$(BUILD)/userprogs/dsai_key_embed.o: $(DSAI_KEY_FILE)
	@mkdir -p $(dir $@)
	cp $(DSAI_KEY_FILE) $(BUILD)/userprogs/dsai_key.bin
	cd $(BUILD)/userprogs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  dsai_key.bin dsai_key_embed.o
	rm -f $(BUILD)/userprogs/dsai_key.bin

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

# embed libfoo.so - the dyntest pie's extra DT_NEEDED (file-backed mmap test) (satoru)
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

# Files exercised from interrupt context - disable SSE codegen so the
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

# SerialLogger runs from EVERY context including irq + exception paths; the
# LogHex64 string-literal buf init vectorised into movaps %xmm0,(%rsp) and
# #GP'd on the misaligned irq stack when the futex sweep logged a wedge (the
# p4 boot3 / p5 boot5 "graph strings on the panic stack" kernel panics). (satoru)
$(BUILD)/drivers/serial.o: $(SRC)/drivers/serial.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

# smp carries the ap timer preempt + tlb ipi handlers (isr context). (satoru)
$(BUILD)/proc/smp.o: $(SRC)/proc/smp.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -mno-sse -mno-mmx -c $< -o $@

# vmm's hardened page walks now run from the futex sweep (irq context). (satoru)
$(BUILD)/kernel/vmm.o: $(SRC)/kernel/vmm.cpp
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
# as task_manager - disable codegen-emitted SSE.
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
#    Bypasses GRUB multiboot2 - loads kurono.elf directly via EFI
#    protocols.  Memory allocated as EfiLoaderCode (executable).
efi: dirs $(EFI_APP) $(EFI_APP_EMERGENCY)
	@echo "  EFI loader: $(EFI_APP)"
	@echo "  EFI emergency loader: $(EFI_APP_EMERGENCY)"

$(BUILD)/boot/efi_loader.o: $(SRC)/boot/efi_loader.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -DKURONO_EFI_CMDLINE='"efi kurono.autologin=1"' -c $< -o $@

$(BUILD)/boot/efi_loader_emergency.o: $(SRC)/boot/efi_loader.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -DKURONO_EFI_CMDLINE='"efi kurono_mode=emergency kurono.text=1"' -c $< -o $@

# separate test loader for the intel display acceleration work. it is never
# copied over the default boot efi and carries the test flags in its command
# line (copy it to EFI/KURONO/GPUTEST.EFI and pick it in the boot menu). (satoru)
$(BUILD)/boot/efi_loader_gpu.o: $(SRC)/boot/efi_loader.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -DKURONO_EFI_CMDLINE='"efi kurono.autologin=1 kurono.uiscale=100 kurono.wpdamage=1 kurono.igpu.accel=1 kurono.igpu.zerocopy=1"' -c $< -o $@

$(BUILD)/boot/efi_loader_gpu.so: $(BUILD)/boot/efi_loader_gpu.o $(BUILD)/boot/kernel_embed.o
	$(EFI_LD) $(EFI_LDFLAGS) $(EFI_CRT0) $^ $(EFI_LIBS) -o $@

$(EFI_APP_GPU): $(BUILD)/boot/efi_loader_gpu.so
	$(EFI_OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
	    -O pei-x86-64 --subsystem efi-app $< $@

gpu-efi: dirs $(EFI_APP_GPU)
	@echo "  EFI gpu test loader: $(EFI_APP_GPU)"

# Embed base kurono.elf as binary data inside the EFI loader
$(BUILD)/boot/kernel_embed.o: $(KERNEL_BASE)
	@mkdir -p $(dir $@)
	cp $< $(BUILD)/boot/kurono.elf && \
	cd $(BUILD)/boot && $(EFI_OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	    kurono.elf kernel_embed.o

# Installer payload embeddings - linked only into the final kernel
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
iso: all efi pack
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
	@echo '    multiboot2 /boot/kurono_base.elf $(if $(filter 1,$(KURONO_AUTOLOGIN)),kurono.autologin=1,) $(if $(strip $(KURONO_GUI_RUN)),kurono.gui.run="$(KURONO_GUI_RUN)",) $(KURONO_FF_APTHREADS) $(KURONO_EXTRA_CMDLINE)' >> $(ISO_DIR)/boot/grub/grub.cfg
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

# --- pack: the on-demand asset files, named for the KURONO partition root (8.3).
# copy everything in ../build/pack next to FIREFOX.TAR on the stick. missing
# sources are skipped (each feature then just reports its file as absent). (satoru)
.PHONY: pack
pack: $(KPY_ELF)
	@mkdir -p $(BUILD)/pack
	@for pair in "$(ALPINE_KERNEL_BIN):ALPKERN.BIN" "$(ALPINE_INITRD_BIN):ALPINIT.BIN" \
	  "$(RTOOLS_TAR):RTOOLS.TAR" "$(FFMPEG_ELF):FFMPEG.ELF" "$(KPY_ELF):KPYTHON.ELF" \
	  "$(KLDE1K_ELF):KLDE1K.ELF" "$(DENJI_MP4_BIN):DENJI.MP4" "$(DENJI_KVID_BIN):DENJI.KVD" \
	  "$(SSSTIK_MP4_BIN):SSSTIK.MP4" "$(SSSTIK_KVID_BIN):SSSTIK.KVD"; do \
	  src=$${pair%%:*}; dst=$${pair##*:}; \
	  if [ -f "$$src" ]; then cmp -s "$$src" $(BUILD)/pack/$$dst || cp "$$src" $(BUILD)/pack/$$dst; fi; \
	done
	@echo "  Asset pack: $(BUILD)/pack ($$(du -sh $(BUILD)/pack | cut -f1))"
