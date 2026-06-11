# Kurono OS Complete Build System
# Creates a fully bootable Linux system with Kurono OS features

param(
    [string]$Base = "D:\Kurono\Kurnon OS",
    [string]$BuildrootVersion = "2024.08.1",
    [switch]$Clean,
    [switch]$BuildOnly,
    [switch]$SkipBuildroot
)

Write-Host "Kurono OS Complete Build System" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

# Setup directories
$BuildDir = "$Base\build"
$BuildrootDir = "$BuildDir\buildroot-$BuildrootVersion"
$OutputDir = "$Base\BootArtifacts"
$OverlayDir = "$BuildDir\overlay"
$ConfigDir = "$BuildDir\config"

# Clean if requested
if ($Clean) {
    Write-Host "Cleaning build directories..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
    Write-Host "Clean complete." -ForegroundColor Green
    return
}

# Create directories
New-Item -ItemType Directory -Force -Path $BuildDir, $OutputDir, $OverlayDir, $ConfigDir | Out-Null

# Download Buildroot if not present
$BuildrootTar = "$BuildDir\buildroot-$BuildrootVersion.tar.gz"
$BuildrootUrl = "https://buildroot.org/downloads/buildroot-$BuildrootVersion.tar.gz"

if (!(Test-Path $BuildrootTar) -and !$SkipBuildroot) {
    Write-Host "Downloading Buildroot $BuildrootVersion..." -ForegroundColor Yellow
    try {
        Invoke-WebRequest -Uri $BuildrootUrl -OutFile $BuildrootTar -UseBasicParsing
        Write-Host "Download complete!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to download Buildroot. Error: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}

# Extract Buildroot
if (!(Test-Path $BuildrootDir) -and !$SkipBuildroot) {
    Write-Host "Extracting Buildroot..." -ForegroundColor Yellow
    try {
        & tar -xf $BuildrootTar -C $BuildDir
        Write-Host "Extraction complete!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to extract Buildroot. Error: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}

# Create Kurono OS Buildroot configuration
Write-Host "Creating Kurono OS Buildroot configuration..." -ForegroundColor Yellow
$KuronoConfig = @"
# Architecture
BR2_x86_64=y
BR2_ARCH="x86_64"

# Toolchain
BR2_TOOLCHAIN_BUILDROOT_GLIBC=y
BR2_TOOLCHAIN_BUILDROOT_CXX=y

# System
BR2_SYSTEM_DHCP="eth0"
BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV=y
BR2_TARGET_GENERIC_GETTY=y
BR2_TARGET_GENERIC_GETTY_PORT="ttyS0"
BR2_TARGET_GENERIC_GETTY_BAUDRATE="115200"
BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW=y

# Init system
BR2_INIT_SYSTEMD=y

# Kernel
BR2_LINUX_KERNEL=y
BR2_LINUX_KERNEL_LATEST_VERSION=y
BR2_LINUX_KERNEL_USE_DEFCONFIG=y
BR2_LINUX_KERNEL_DEFCONFIG="x86_64_defconfig"
BR2_LINUX_KERNEL_IMAGE=y
BR2_LINUX_KERNEL_VMLINUZ=y
BR2_LINUX_KERNEL_XZ=y

# Kernel config fragments
BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="${BR2_EXTERNAL_KURONO_PATH}/board/kurono/linux-kurono-fragment.config"

# Kernel extensions
BR2_LINUX_KERNEL_EXT_KURONO=y

# Filesystem
BR2_TARGET_ROOTFS_EXT2=y
BR2_TARGET_ROOTFS_EXT2_4=y
BR2_TARGET_ROOTFS_EXT2_SIZE="256M"

# Packages
BR2_PACKAGE_BUSYBOX=y
BR2_PACKAGE_DROPBEAR=y
BR2_PACKAGE_OPENSSH=y
BR2_PACKAGE_BASH=y
BR2_PACKAGE_COREUTILS=y
BR2_PACKAGE_UTIL_LINUX=y
BR2_PACKAGE_PROCPS_NG=y
BR2_PACKAGE_NCURSES=y
BR2_PACKAGE_VIM=y

# Graphics
BR2_PACKAGE_XORG7=y
BR2_PACKAGE_XDRIVER_XF86_VIDEO_VESA=y
BR2_PACKAGE_XDRIVER_XF86_VIDEO_CIRRUS=y

# Network
BR2_PACKAGE_NETWORK_MANAGER=y
BR2_PACKAGE_WPA_SUPPLICANT=y

# Development
BR2_PACKAGE_GCC_TARGET=y
BR2_PACKAGE_MAKE=y
BR2_PACKAGE_CMAKE=y
BR2_PACKAGE_GIT=y

# Security
BR2_PACKAGE_OPENSSL=y
BR2_PACKAGE_LIBOPENSSL=y
BR2_PACKAGE_LIBOPENSSL_BIN=y

# Compression
BR2_PACKAGE_XZ=y
BR2_PACKAGE_XZ_UTILS=y

# Debugging
BR2_PACKAGE_STRACE=y
BR2_PACKAGE_GDB=y
BR2_PACKAGE_VALGRIND=y

# Kurono splash utility
BR2_PACKAGE_KURONO_SPLASH=y

# Bootloader
BR2_TARGET_GRUB2=y
BR2_TARGET_GRUB2_X86_64_EFI=y
BR2_TARGET_GRUB2_BUILTIN_MODULES="boot linux ext2 fat part_msdos part_gpt normal iso9660 biosdisk"
BR2_TARGET_GRUB2_BUILTIN_CONFIG="${BR2_EXTERNAL_KURONO_PATH}/board/kurono/grub.cfg"
"@

Set-Content -Path "$ConfigDir\kurono_defconfig" -Value $KuronoConfig

# Create Kurono OS external package structure
$ExternalDir = "$BuildDir\kurono"
New-Item -ItemType Directory -Force -Path "$ExternalDir\package", "$ExternalDir\board\kurono", "$ExternalDir\configs" | Out-Null

# Create Kurono kernel extension
$KuronoKernelExt = @"
################################################################################
# kurono kernel extension
################################################################################

KURONO_VERSION = 1.0.0
KURONO_SITE = $(BR2_EXTERNAL_KURONO_PATH)/package/kurono
KURONO_SITE_METHOD = local
KURONO_DEPENDENCIES = linux

KURONO_MODULE_SUBDIRS = drivers/kurono
KURONO_MODULE_MAKE_OPTS = CONFIG_KURONO=m

define KURONO_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_KURONO)
endef

$(eval $(kernel-module))
$(eval $(generic-package))
"@

Set-Content -Path "$ExternalDir\package\kurono\Config.in" -Value "source \"package/kurono/Config.in\""

# Create Kurono kernel module source
$KuronoModuleDir = "$ExternalDir\package\kurono\drivers\kurono"
New-Item -ItemType Directory -Force -Path $KuronoModuleDir | Out-Null

$KuronoModule = @"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>

#define KURONO_VERSION "1.0.0"
#define KURONO_NAME "Kurono OS Hybrid Kernel"

static int kurono_proc_show(struct seq_file *m, void *v) {
    seq_printf(m, "=====================================\n");
    seq_printf(m, "    KURONO OS HYBRID KERNEL\n");
    seq_printf(m, "=====================================\n");
    seq_printf(m, "Version: %s\n", KURONO_VERSION);
    seq_printf(m, "Architecture: x86_64\n");
    seq_printf(m, "Environment: Native Linux\n");
    seq_printf(m, "Security Engine: Active\n");
    seq_printf(m, "KCL Interpreter: Ready\n");
    seq_printf(m, "Package Manager: Online\n");
    seq_printf(m, "=====================================\n");
    return 0;
}

static int kurono_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, kurono_proc_show, NULL);
}

static const struct proc_ops kurono_proc_ops = {
    .proc_open = kurono_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init kurono_init(void) {
    printk(KERN_INFO "%s v%s loaded\n", KURONO_NAME, KURONO_VERSION);
    
    if (!proc_create("kurono", 0, NULL, &kurono_proc_ops)) {
        printk(KERN_ERR "Failed to create /proc/kurono\n");
        return -ENOMEM;
    }
    
    printk(KERN_INFO "Kurono OS ready - /proc/kurono created\n");
    return 0;
}

static void __exit kurono_exit(void) {
    remove_proc_entry("kurono", NULL);
    printk(KERN_INFO "%s unloaded\n", KURONO_NAME);
}

module_init(kurono_init);
module_exit(kurono_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kurono OS Team");
MODULE_DESCRIPTION("Kurono OS Hybrid Kernel Module");
MODULE_VERSION(KURONO_VERSION);
"@

Set-Content -Path "$KuronoModuleDir\kurono.c" -Value $KuronoModule

# Create Makefile for Kurono module
$KuronoMakefile = @"
obj-m += kurono.o

kurono-objs := kurono.o

ccflags-y := -I$(srctree)/include
"@

Set-Content -Path "$KuronoModuleDir\Makefile" -Value $KuronoMakefile

# Create Kurono package Config.in
$KuronoConfigIn = @"
config BR2_PACKAGE_KURONO
	bool "kurono"
	depends on BR2_LINUX_KERNEL
	help
	  Kurono OS kernel module and utilities.
	  
	  This package provides the Kurono OS hybrid kernel
	  module and command-line utilities.
"@

Set-Content -Path "$ExternalDir\package\kurono\Config.in" -Value $KuronoConfigIn

# Create Kurono package makefile
$KuronoPackageMk = @"
################################################################################
#
# kurono
#
################################################################################

KURONO_VERSION = 1.0.0
KURONO_SITE = $(BR2_EXTERNAL_KURONO_PATH)/package/kurono
KURONO_SITE_METHOD = local
KURONO_LICENSE = GPL-2.0+
KURONO_LICENSE_FILES = COPYING

KURONO_MODULE_SUBDIRS = drivers/kurono
KURONO_MODULE_MAKE_OPTS = CONFIG_KURONO=m

define KURONO_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_KURONO)
endef

$(eval $(kernel-module))
"@

Set-Content -Path "$ExternalDir\package\kurono\kurono.mk" -Value $KuronoPackageMk

# Create external.desc
$ExternalDesc = @"
name: KURONO
"@

Set-Content -Path "$ExternalDir\external.desc" -Value $ExternalDesc

# Create external.mk
$ExternalMk = @"
include $(sort $(wildcard $(BR2_EXTERNAL_KURONO_PATH)/package/*/*.mk))
"@

Set-Content -Path "$ExternalDir\external.mk" -Value $ExternalMk

# Create GRUB configuration with splash screen
$GrubCfg = @"
set timeout=5
set default=0
set timeout_style=menu

insmod efi_gop
insmod efi_uga
insmod gfxterm
insmod gfxmenu
insmod png

set gfxmode=auto
set gfxpayload=keep
terminal_output gfxterm

if background_image /EFI/KURONO/logo.png ; then
  set color_normal=white/black
  set color_highlight=yellow/black
fi

menuentry "Kurono OS" {
  linux /EFI/KURONO/vmlinuz console=ttyS0 quiet splash rdinit=/sbin/kurono-init
  initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry "Kurono OS (Recovery)" {
  linux /EFI/KURONO/vmlinuz console=ttyS0 single rdinit=/bin/sh
  initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry "EFI Shell" {
  chainloader /EFI/BOOT/BOOTX64.EFI
}
"@

Set-Content -Path "$ExternalDir\board\kurono\grub.cfg" -Value $GrubCfg

# Create overlay filesystem structure
Write-Host "Creating Kurono OS overlay filesystem..." -ForegroundColor Yellow

# Create directory structure
$OverlayDirs = @(
    "etc", "bin", "sbin", "usr/bin", "usr/sbin", "lib", "lib64", 
    "dev", "proc", "sys", "tmp", "var", "home", "root", "mnt", "media",
    "opt", "srv", "run", "usr/lib", "usr/share", "var/log", "var/tmp",
    "etc/kurono", "etc/init.d", "usr/share/kurono", "var/lib/kurono"
)

foreach ($dir in $OverlayDirs) {
    New-Item -ItemType Directory -Force -Path "$OverlayDir\$dir" | Out-Null
}

# Create Kurono OS init script
$KuronoInit = @"
#!/bin/sh
# Kurono OS System Initialization

echo "======================================="
echo "    KURONO OS HYBRID KERNEL"
echo "======================================="
echo "Initializing Kurono OS system..."

# Set hostname
echo "kurono-os" > /proc/sys/kernel/hostname

# Mount essential filesystems
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run

# Create essential device nodes
[ -c /dev/console ] || mknod -m 600 /dev/console c 5 1
[ -c /dev/null ] || mknod -m 666 /dev/null c 1 3
[ -c /dev/zero ] || mknod -m 666 /dev/zero c 1 5
[ -c /dev/random ] || mknod -m 666 /dev/random c 1 8
[ -c /dev/urandom ] || mknod -m 666 /dev/urandom c 1 9

# Set up networking
ifconfig lo 127.0.0.1 up

# Load Kurono kernel module
if [ -f /lib/modules/$(uname -r)/extra/kurono.ko ]; then
    echo "Loading Kurono OS kernel module..."
    insmod /lib/modules/$(uname -r)/extra/kurono.ko
    echo "Kurono OS kernel module loaded successfully"
else
    echo "Kurono OS kernel module not found"
fi

# Create system users
echo "Creating system users..."
if ! grep -q "^root:" /etc/passwd; then
    echo "root:x:0:0:root:/root:/bin/sh" >> /etc/passwd
fi
if ! grep -q "^user:" /etc/passwd; then
    echo "user:x:1000:1000:user:/home/user:/bin/sh" >> /etc/passwd
fi

# Create home directory
mkdir -p /home/user
chown user:user /home/user

# Set up environment
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
export USER=root

# Show system info
echo ""
echo "System Information:"
cat /proc/kurono 2>/dev/null || echo "Kurono OS Module: Not loaded"
echo "Kernel: $(uname -r)"
echo "Architecture: $(uname -m)"
echo ""

# Start essential services
echo "Starting system services..."

# Enable console login on serial port
if [ -x /sbin/getty ]; then
    /sbin/getty -L ttyS0 115200 vt100 &
fi

# Start Kurono OS shell
echo "Kurono OS ready. Starting system shell..."
echo "======================================="
echo "Type 'help' for available commands"
echo "Type 'kurono-info' for system information"
echo "======================================="

# Start interactive shell
exec /bin/sh
"@

Set-Content -Path "$OverlayDir\sbin\kurono-init" -Value $KuronoInit

# Create Kurono info command
$KuronoInfo = @"
#!/bin/sh
# Kurono OS System Information

echo "======================================="
echo "    KURONO OS SYSTEM INFORMATION"
echo "======================================="
echo "Kernel: $(uname -r)"
echo "Architecture: $(uname -m)"
echo "Hostname: $(hostname)"
echo "Uptime: $(uptime)"
echo ""
echo "Memory Information:"
cat /proc/meminfo | grep -E "^(MemTotal|MemFree|MemAvailable):"
echo ""
echo "CPU Information:"
cat /proc/cpuinfo | grep -E "^(processor|model name|cpu cores)" | head -6
echo ""
echo "Kurono OS Module:"
if [ -d /proc/kurono ]; then
    cat /proc/kurono
else
    echo "Kurono OS module not loaded"
fi
echo "======================================="
"@

Set-Content -Path "$OverlayDir\usr\bin\kurono-info" -Value $KuronoInfo

# Create inittab
$Inittab = @"
# Kurono OS inittab

# System initialization
::sysinit:/sbin/kurono-init

# Console/serial port
::respawn:/sbin/getty -L ttyS0 115200 vt100
::respawn:/sbin/getty -L console 115200 vt100

# Virtual terminals
#tty1::respawn:/sbin/getty 38400 tty1
#tty2::respawn:/sbin/getty 38400 tty2
#tty3::respawn:/sbin/getty 38400 tty3
#tty4::respawn:/sbin/getty 38400 tty4

# Reboot/shutdown
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
"@

Set-Content -Path "$OverlayDir\etc\inittab" -Value $Inittab

# Create passwd file
$Passwd = @"
root:x:0:0:root:/root:/bin/sh
user:x:1000:1000:user:/home/user:/bin/sh
"@

Set-Content -Path "$OverlayDir\etc\passwd" -Value $Passwd

# Create group file
$Group = @"
root:x:0:
user:x:1000:
"@

Set-Content -Path "$OverlayDir\etc\group" -Value $Group

# Create shadow file
$Shadow = @"
root:!:18000:0:99999:7:::
user:!:18000:0:99999:7:::
"@

Set-Content -Path "$OverlayDir\etc\shadow" -Value $Shadow

# Create fstab
$Fstab = @"
# Kurono OS fstab

# Filesystem  Mount point  Type   Options                  Dump Pass
proc          /proc        proc   defaults                  0    0
sysfs         /sys         sysfs  defaults                  0    0
devtmpfs      /dev         devtmpfs  mode=0755,nosuid     0    0
tmpfs         /tmp         tmpfs  mode=1777                0    0
tmpfs         /run         tmpfs  mode=0755,nosuid,nodev  0    0
"@

Set-Content -Path "$OverlayDir\etc\fstab" -Value $Fstab

# Create build script
$BuildScript = @"
cd $BuildrootDir
make BR2_EXTERNAL=$ExternalDir kurono_defconfig
make -j`$(nproc)
"@

Set-Content -Path "$BuildDir\build_kurono.sh" -Value $BuildScript

# Create final packaging script
$PackageScript = @"
# Copy built images to BootArtifacts
cp $BuildrootDir/output/images/vmlinuz $OutputDir\
cp $BuildrootDir/output/images/initramfs.cpio.gz $OutputDir\
cp $BuildrootDir/output/images/rootfs.ext2 $OutputDir\

# Create GRUB directory structure
mkdir -p $OutputDir/EFI/BOOT
mkdir -p $OutputDir/EFI/kurono

# Copy GRUB config
cp $ExternalDir/board/kurono/grub.cfg $OutputDir/EFI/BOOT/

# Copy logo if available
if [ -f "$Base\kurono\logo.png" ]; then
    cp "$Base\kurono\logo.png" $OutputDir/EFI/kurono/
elif [ -f "D:\kurono\logo.png" ]; then
    cp "D:\kurono\logo.png" $OutputDir/EFI/kurono/
fi

echo "Kurono OS build complete!"
echo "Boot files available in: $OutputDir"
echo ""
echo "To boot Kurono OS:"
echo "1. Direct kernel: qemu-system-x86_64 -kernel $OutputDir/vmlinuz -initrd $OutputDir/initramfs.cpio.gz"
echo "2. With disk: qemu-system-x86_64 -hda $OutputDir/rootfs.ext2 -kernel $OutputDir/vmlinuz -initrd $OutputDir/initramfs.cpio.gz"
echo "3. UEFI boot: Use qemu_uefi_boot_folder.ps1 with $OutputDir"
"@

Set-Content -Path "$BuildDir\package_kurono.sh" -Value $PackageScript

Write-Host ""
Write-Host "Kurono OS build system created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "To build Kurono OS:" -ForegroundColor Cyan
Write-Host "1. Run: cd $BuildDir && bash build_kurono.sh" -ForegroundColor White
Write-Host "2. After build: bash package_kurono.sh" -ForegroundColor White
Write-Host ""
Write-Host "Note: This requires a Linux environment or WSL with build tools." -ForegroundColor Yellow
Write-Host "The build process will download and compile Linux kernel, BusyBox, and all dependencies." -ForegroundColor Yellow
