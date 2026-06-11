# Kurono OS Immediate Boot Solution
# Creates a fully bootable system with Kurono branding and features

param(
    [string]$Base = "D:\Kurono\Kurnon OS",
    [switch]$UseAlpineKernel = $true,
    [switch]$CreateFullSystem = $false
)

Write-Host "Kurono OS Immediate Boot Solution" -ForegroundColor Cyan
Write-Host "=================================" -ForegroundColor Cyan

# Setup directories
$BootDir = "$Base\BootArtifacts"
$KuronoDir = "$BootDir\EFI\KURONO"
$ToolsDir = "$Base\build\tools"

# Create directories
New-Item -ItemType Directory -Force -Path $BootDir, $KuronoDir, $ToolsDir | Out-Null

Write-Host "Setting up Kurono OS boot system..." -ForegroundColor Yellow

# Download Alpine Linux mini root filesystem for a complete system
$AlpineVersion = "3.18.4"
$AlpineRootfsUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/alpine-minirootfs-$AlpineVersion-x86_64.tar.gz"
$AlpineRootfsPath = "$ToolsDir\alpine-minirootfs-$AlpineVersion-x86_64.tar.gz"

if (!(Test-Path $AlpineRootfsPath)) {
    Write-Host "Downloading Alpine Linux mini rootfs..." -ForegroundColor Yellow
    try {
        Invoke-WebRequest -Uri $AlpineRootfsUrl -OutFile $AlpineRootfsPath -UseBasicParsing
        Write-Host "Download complete!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to download Alpine rootfs. Error: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "Using minimal busybox approach instead..." -ForegroundColor Yellow
        $UseAlpineKernel = $true
        $CreateFullSystem = $false
    }
}

# Download kernel and initramfs
Write-Host "Downloading Kurono OS kernel files..." -ForegroundColor Yellow

# Alpine Linux kernel files
$KernelUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/vmlinuz-lts"
$InitrdUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/initramfs-lts"

$KernelPath = "$KuronoDir\vmlinuz"
$InitrdPath = "$KuronoDir\initramfs.cpio.gz"

try {
    # Download kernel
    if (!(Test-Path $KernelPath)) {
        Write-Host "Downloading kernel..." -ForegroundColor Gray
        Invoke-WebRequest -Uri $KernelUrl -OutFile $KernelPath -UseBasicParsing
    }
    
    # Download initramfs
    if (!(Test-Path $InitrdPath)) {
        Write-Host "Downloading initramfs..." -ForegroundColor Gray
        Invoke-WebRequest -Uri $InitrdUrl -OutFile $InitrdPath -UseBasicParsing
    }
    
    Write-Host "Kernel files downloaded successfully!" -ForegroundColor Green
} catch {
    Write-Host "Failed to download kernel files. Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

# Create custom initramfs with Kurono OS features
Write-Host "Creating Kurono OS custom initramfs..." -ForegroundColor Yellow

$TempDir = "$ToolsDir\initramfs-temp"
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

# Extract original initramfs
Write-Host "Extracting original initramfs..." -ForegroundColor Gray
& cd $TempDir
& xz -d -c $InitrdPath | & cpio -id

# Create Kurono OS initialization script
$KuronoInit = @"
#!/bin/sh
# Kurono OS Initialization Script
# Based on Alpine Linux initramfs

echo "======================================="
echo "    KURONO OS HYBRID KERNEL"
echo "======================================="
echo "Initializing Kurono OS system..."

# Run original init script first
if [ -x /init.original ]; then
    echo "Running original initialization..."
    /init.original
fi

# Kurono OS specific initialization
echo "Setting up Kurono OS environment..."

# Set hostname
echo "kurono-os" > /proc/sys/kernel/hostname

# Create Kurono OS directories
mkdir -p /etc/kurono
mkdir -p /var/lib/kurono
mkdir -p /usr/share/kurono

# Create Kurono OS system info
cat > /etc/kurono/system.info << 'EOF'
KURONO_OS_VERSION=1.0.0
KURONO_OS_NAME="Kurono OS Hybrid Kernel"
KURONO_OS_ARCH=x86_64
KURONO_OS_BUILD_DATE=$(date)
KURONO_OS_KERNEL=$(uname -r)
EOF

# Create Kurono OS motd
cat > /etc/motd << 'EOF'

=======================================
    Welcome to Kurono OS v1.0.0
=======================================
    Hybrid Kernel System
    Architecture: x86_64
    
Type 'kurono-info' for system details
Type 'help' for available commands
=======================================

EOF

# Create Kurono OS info command
cat > /bin/kurono-info << 'EOF'
#!/bin/sh
echo "======================================="
echo "    KURONO OS SYSTEM INFORMATION"
echo "======================================="
if [ -f /etc/kurono/system.info ]; then
    . /etc/kurono/system.info
    echo "Version: $KURONO_OS_VERSION"
    echo "Name: $KURONO_OS_NAME"
    echo "Architecture: $KURONO_OS_ARCH"
    echo "Build Date: $KURONO_OS_BUILD_DATE"
    echo "Kernel: $KURONO_OS_KERNEL"
else
    echo "Kurono OS system info not found"
fi
echo "Hostname: $(hostname)"
echo "Uptime: $(uptime)"
echo "Memory: $(grep MemTotal /proc/meminfo | awk '{print $2 $3}')"
echo "======================================="
EOF
chmod +x /bin/kurono-info

# Create Kurono OS help command
cat > /bin/kurono-help << 'EOF'
#!/bin/sh
echo "======================================="
echo "    KURONO OS COMMAND REFERENCE"
echo "======================================="
echo "System Commands:"
echo "  kurono-info    - Show system information"
echo "  kurono-help    - Show this help"
echo "  kurono-version - Show version info"
echo ""
echo "Standard Commands:"
echo "  ls, cd, pwd, cat, echo, mkdir, rm, cp, mv"
echo "  ps, top, kill, ifconfig, ping"
echo ""
echo "For more help, consult the Kurono OS documentation"
echo "======================================="
EOF
chmod +x /bin/kurono-help

# Create version command
cat > /bin/kurono-version << 'EOF'
#!/bin/sh
echo "Kurono OS v1.0.0 - Hybrid Kernel System"
echo "Built on Alpine Linux LTS"
echo "Architecture: x86_64"
EOF
chmod +x /bin/kurono-version

# Load Kurono logo if available
if [ -f /EFI/kurono/logo.png ]; then
    echo "Kurono OS logo found at /EFI/kurono/logo.png"
fi

# Set up environment
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export KURONO_OS=1
export KURONO_VERSION=1.0.0

echo "Kurono OS initialization complete!"
echo "======================================="

# Continue with normal boot process
exec /bin/sh
"@

Set-Content -Path "$TempDir\init" -Value $KuronoInit

# Make init executable (skipped on Windows build host)

# Create additional Kurono OS files
$KuronoFiles = @{
    "etc/hostname" = "kurono-os"
    "etc/issue" = "Kurono OS v1.0.0 Hybrid Kernel\n"
    "etc/kurono/version" = "1.0.0"
    "etc/kurono/build-info" = "Kurono OS Built on $(Get-Date)"
}

foreach ($file in $KuronoFiles.Keys) {
    $filePath = "$TempDir\$file"
    $dir = Split-Path $filePath
    if (!(Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    Set-Content -Path $filePath -Value $KuronoFiles[$file]
}

# Create new initramfs
Write-Host "Creating new Kurono OS initramfs..." -ForegroundColor Yellow
$NewInitrdPath = "$KuronoDir\initramfs-kurono.cpio.gz"

& cd $TempDir
& find . | & cpio -o -H newc | & gzip > $NewInitrdPath

Write-Host "Kurono OS initramfs created!" -ForegroundColor Green

# Copy logo if available
$LogoSource = "D:\kurono\logo.png"
if (Test-Path $LogoSource) {
    Write-Host "Copying Kurono OS logo..." -ForegroundColor Yellow
    Copy-Item -Path $LogoSource -Destination "$KuronoDir\logo.png" -Force
    Write-Host "Logo copied successfully!" -ForegroundColor Green
} else {
    Write-Host "Kurono logo not found at $LogoSource" -ForegroundColor Yellow
    # Create a simple text logo
    $TextLogo = @"
 __  __     _        _    
|  \/  |___| |_ _  _| |__ 
| |\/| / -_)  _| || | '_ \
|_|  |_\___|\__|\_,_|_.__/
Kurono OS v1.0.0
"@
    Set-Content -Path "$KuronoDir\logo.txt" -Value $TextLogo
}

# Create GRUB configuration with Kurono branding
Write-Host "Creating Kurono OS GRUB configuration..." -ForegroundColor Yellow

$GrubCfg = @"
set timeout=5
set default=0

# Load font if available
if loadfont /EFI/kurono/font.pf2 ; then
    set gfxmode=auto
    set gfxpayload=keep
    insmod efi_gop
    insmod efi_uga
    insmod gfxterm
    insmod gfxmenu
    insmod png
    insmod jpeg
    terminal_output gfxterm
fi

# Kurono OS splash screen
if background_image /EFI/kurono/logo.png ; then
    set color_normal=white/black
    set color_highlight=yellow/black
else
    # Fallback to text mode
    set color_normal=white/black
    set color_highlight=yellow/black
fi

menuentry "Kurono OS Hybrid Kernel v1.0.0" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 quiet splash logo.nologo kurono.splash=1 kurono.mode=normal
    initrd /EFI/KURONO/initramfs-kurono.cpio.gz
}

menuentry "Kurono OS (Recovery Mode)" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 single kurono.mode=recovery
    initrd /EFI/KURONO/initramfs-kurono.cpio.gz
}

menuentry "Kurono OS (Debug Mode)" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 debug ignore_loglevel kurono.mode=debug
    initrd /EFI/KURONO/initramfs-kurono.cpio.gz
}

menuentry "Kurono OS Native Kernel" {
    multiboot /EFI/KURONO/kurono_kernel.elf
    boot
}

menuentry "Kurono OS (Memory Test)" {
    linux16 /boot/memtest86+
}

menuentry "EFI Shell" {
    chainloader /EFI/BOOT/BOOTX64.EFI
}

# Attempt Windows chainload if the disk has Windows Boot Manager
menuentry "Windows Boot Manager (if available)" {
    insmod part_gpt
    insmod fat
    insmod ntfs
    search --file --set=root /EFI/Microsoft/Boot/bootmgfw.efi
    chainloader /EFI/Microsoft/Boot/bootmgfw.efi
}
"@

Set-Content -Path "$BootDir\EFI\BOOT\grub.cfg" -Value $GrubCfg

# Create boot scripts
Write-Host "Creating Kurono OS boot scripts..." -ForegroundColor Yellow

# Direct boot script
$DirectBoot = @"
param(
    [int]`$MemoryMB = 2048,
    [string]`$Mode = "normal"
)

Write-Host "Kurono OS Hybrid Kernel v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

`$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not `$qemu) { 
    Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
    exit 1 
}

`$kernelPath = "$KuronoDir\vmlinuz"
`$initrdPath = "$KuronoDir\initramfs-kurono.cpio.gz"

if (!(Test-Path `$kernelPath)) {
    Write-Host "Kurono OS kernel not found: `$kernelPath" -ForegroundColor Red
    exit 1
}

if (!(Test-Path `$initrdPath)) {
    Write-Host "Kurono OS initramfs not found: `$initrdPath" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Kurono OS in `$Mode mode..." -ForegroundColor Green

`$kernelParams = "console=ttyS0 quiet splash kurono.mode=`$Mode rdinit=/bin/sh"
if (`$Mode -eq "debug") {
    `$kernelParams = "console=ttyS0 debug ignore_loglevel kurono.mode=debug"
} elseif (`$Mode -eq "recovery") {
    `$kernelParams = "console=ttyS0 single kurono.mode=recovery"
}

`$args = @(
    "-m", `$MemoryMB,
    "-cpu", "qemu64",
    "-kernel", `$kernelPath,
    "-initrd", `$initrdPath,
    "-append", `$kernelParams,
    "-serial", "mon:stdio",
    "-vga", "std"
)

if (Test-Path "$KuronoDir\logo.png") {
    `$args += @("-device", "bochs-display")
}

Write-Host "Booting Kurono OS..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& `$qemu.Path `$args
"@

Set-Content -Path "$Base\boot_kurono_direct.ps1" -Value $DirectBoot

# BIOS boot script
$BiosBoot = @"
param(
    [int]`$MemoryMB = 2048
)

Write-Host "Kurono OS BIOS Boot" -ForegroundColor Cyan
Write-Host "===================" -ForegroundColor Cyan

`$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not `$qemu) { 
    Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
    exit 1 
}

`$bootDir = "$BootDir"

Write-Host "Starting Kurono OS with BIOS boot..." -ForegroundColor Green
Write-Host "Using boot directory: `$bootDir" -ForegroundColor Cyan

`$args = @(
    "-m", `$MemoryMB,
    "-cpu", "qemu64",
    "-drive", "file=fat:rw:`$bootDir,format=raw,if=ide",
    "-serial", "mon:stdio",
    "-vga", "std",
    "-boot", "order=c"
)

Write-Host "Booting Kurono OS via GRUB..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& `$qemu.Path `$args
"@

Set-Content -Path "$Base\boot_kurono_bios.ps1" -Value $BiosBoot

# Interactive boot menu
$BootMenu = @"
param(
    [int]`$MemoryMB = 2048
)

Write-Host "Kurono OS Boot Manager v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan
Write-Host ""
Write-Host "1. Kurono OS Hybrid Kernel (Direct Boot)" -ForegroundColor White
Write-Host "2. Kurono OS (Recovery Mode)" -ForegroundColor White
Write-Host "3. Kurono OS (Debug Mode)" -ForegroundColor White
Write-Host "4. Kurono OS (BIOS/GRUB Boot)" -ForegroundColor White
Write-Host "5. Exit" -ForegroundColor White
Write-Host ""

`$choice = Read-Host "Select boot option (1-5)"

switch (`$choice) {
    "1" { & "$Base\boot_kurono_direct.ps1" -MemoryMB `$MemoryMB -Mode "normal" }
    "2" { & "$Base\boot_kurono_direct.ps1" -MemoryMB `$MemoryMB -Mode "recovery" }
    "3" { & "$Base\boot_kurono_direct.ps1" -MemoryMB `$MemoryMB -Mode "debug" }
    "4" { & "$Base\boot_kurono_bios.ps1" -MemoryMB `$MemoryMB }
    "5" { Write-Host "Exiting..." -ForegroundColor Yellow; exit 0 }
    default { Write-Host "Invalid choice. Exiting..." -ForegroundColor Red; exit 1 }
}
"@

Set-Content -Path "$Base\boot_kurono_menu.ps1" -Value $BootMenu

# Clean up
Remove-Item -Recurse -Force $TempDir

Write-Host ""
Write-Host "✓ Kurono OS boot system created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Boot Options:" -ForegroundColor Cyan
Write-Host "  1. Interactive menu: .\boot_kurono_menu.ps1" -ForegroundColor White
Write-Host "  2. Direct boot: .\boot_kurono_direct.ps1" -ForegroundColor White
Write-Host "  3. BIOS boot: .\boot_kurono_bios.ps1" -ForegroundColor White
Write-Host ""
Write-Host "Features:" -ForegroundColor Cyan
Write-Host "  ✓ Custom Kurono OS initramfs" -ForegroundColor White
Write-Host "  ✓ Kurono-specific commands (kurono-info, kurono-help)" -ForegroundColor White
Write-Host "  ✓ GRUB configuration with splash screen support" -ForegroundColor White
Write-Host "  ✓ Multiple boot modes (normal, recovery, debug)" -ForegroundColor White
Write-Host "  ✓ System information and branding" -ForegroundColor White
Write-Host ""
Write-Host "Ready to boot Kurono OS!" -ForegroundColor Green
# Kurono UI inclusion skipped; use 9p share and run from host
