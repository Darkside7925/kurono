# Kurono OS Simple Boot Setup
param(
    [string]$Base = "D:\Kurono\Kurnon OS"
)

Write-Host "Kurono OS Simple Boot Setup" -ForegroundColor Cyan

# Setup directories
$BootDir = "$Base\BootArtifacts"
$KuronoDir = "$BootDir\EFI\KURONO"

# Create directories
New-Item -ItemType Directory -Force -Path $BootDir, $KuronoDir | Out-Null

Write-Host "Downloading Kurono OS kernel files..." -ForegroundColor Yellow

# Download kernel and initramfs
$KernelUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/vmlinuz-lts"
$InitrdUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/initramfs-lts"

$KernelPath = "$KuronoDir\vmlinuz"
$InitrdPath = "$KuronoDir\initramfs.cpio.gz"

try {
    if (!(Test-Path $KernelPath)) {
        Write-Host "Downloading kernel..." -ForegroundColor Gray
        Invoke-WebRequest -Uri $KernelUrl -OutFile $KernelPath -UseBasicParsing
    }
    
    if (!(Test-Path $InitrdPath)) {
        Write-Host "Downloading initramfs..." -ForegroundColor Gray
        Invoke-WebRequest -Uri $InitrdUrl -OutFile $InitrdPath -UseBasicParsing
    }
    
    Write-Host "Kernel files downloaded successfully!" -ForegroundColor Green
} catch {
    Write-Host "Failed to download kernel files. Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

# Copy logo if available
$LogoSource = "D:\kurono\logo.png"
if (Test-Path $LogoSource) {
    Write-Host "Copying Kurono OS logo..." -ForegroundColor Yellow
    Copy-Item -Path $LogoSource -Destination "$KuronoDir\logo.png" -Force
    Write-Host "Logo copied successfully!" -ForegroundColor Green
}

# Create GRUB configuration
Write-Host "Creating Kurono OS GRUB configuration..." -ForegroundColor Yellow

$GrubCfg = @"
set timeout=5
set default=0

menuentry "Kurono OS Hybrid Kernel v1.0.0" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 quiet splash logo.nologo
    initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry "Kurono OS (Recovery Mode)" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 single
    initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry "Kurono OS (Debug Mode)" {
    linux /EFI/KURONO/vmlinuz root=/dev/sda1 console=ttyS0 debug ignore_loglevel
    initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry "EFI Shell" {
    chainloader /EFI/BOOT/BOOTX64.EFI
}
"@

New-Item -ItemType Directory -Force -Path "$BootDir\EFI\BOOT" | Out-Null
Set-Content -Path "$BootDir\EFI\BOOT\grub.cfg" -Value $GrubCfg

# Create boot script
Write-Host "Creating Kurono OS boot script..." -ForegroundColor Yellow

$BootScript = @"
param(
    [int]`$MemoryMB = 2048
)

Write-Host "Kurono OS Hybrid Kernel v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

`$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not `$qemu) { 
    Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
    exit 1 
}

`$kernelPath = "$KuronoDir\vmlinuz"
`$initrdPath = "$KuronoDir\initramfs.cpio.gz"

if (!(Test-Path `$kernelPath)) {
    Write-Host "Kurono OS kernel not found: `$kernelPath" -ForegroundColor Red
    exit 1
}

if (!(Test-Path `$initrdPath)) {
    Write-Host "Kurono OS initramfs not found: `$initrdPath" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Kurono OS..." -ForegroundColor Green

`$args = @(
    "-m", `$MemoryMB,
    "-cpu", "qemu64",
    "-kernel", `$kernelPath,
    "-initrd", `$initrdPath,
    "-append", "console=ttyS0 quiet splash logo.nologo",
    "-serial", "mon:stdio",
    "-vga", "std"
)

if (Test-Path "$KuronoDir\logo.png") {
    `args += @("-device", "bochs-display")
}

Write-Host "Booting Kurono OS..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& `$qemu.Path `$args
"@

Set-Content -Path "$Base\boot_kurono_final.ps1" -Value $BootScript

Write-Host ""
Write-Host "Kurono OS boot system created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "To boot Kurono OS:" -ForegroundColor Cyan
Write-Host "  Run: .\boot_kurono_final.ps1" -ForegroundColor White