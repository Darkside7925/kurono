# Simple boot setup script for Kurono OS
Write-Host "Setting up Kurono OS boot environment..." -ForegroundColor Cyan

# Create boot directory structure
$bootDir = "BootArtifacts"
$efiBootDir = "$bootDir\EFI\BOOT"
$kuronoDir = "$bootDir\EFI\KURONO"

Write-Host "Creating directories..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $efiBootDir | Out-Null
New-Item -ItemType Directory -Force -Path $kuronoDir | Out-Null

# Create GRUB configuration
Write-Host "Creating GRUB configuration..." -ForegroundColor Yellow
$grubConfig = @"
set timeout=5
set default=0

menuentry 'Kurono OS' {
  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0 quiet splash
  initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry 'Kurono OS (Recovery)' {
  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0 single
  initrd /EFI/KURONO/initramfs.cpio.gz
}

menuentry 'EFI Shell' {
  chainloader /EFI/BOOT/BOOTX64.EFI
}
"@

Set-Content -Path "$efiBootDir\grub.cfg" -Value $grubConfig
Write-Host "GRUB configuration created." -ForegroundColor Green

# Download kernel files
Write-Host "Downloading Alpine Linux kernel files..." -ForegroundColor Yellow
$kernelUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/vmlinuz-lts"
$initrdUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/initramfs-lts"

$kernelPath = "$kuronoDir\vmlinuz"
$initrdPath = "$kuronoDir\initramfs.cpio.gz"

try {
    Write-Host "Downloading kernel..." -ForegroundColor Yellow
    Invoke-WebRequest -Uri $kernelUrl -OutFile $kernelPath -UseBasicParsing
    Write-Host "Downloading initrd..." -ForegroundColor Yellow
    Invoke-WebRequest -Uri $initrdUrl -OutFile $initrdPath -UseBasicParsing
    Write-Host "Download complete!" -ForegroundColor Green
} catch {
    Write-Host "Download failed. Error: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "You may need to download manually." -ForegroundColor Yellow
}

# Verify files
Write-Host "Verifying boot files..." -ForegroundColor Yellow
if (Test-Path $kernelPath) {
    Write-Host "✓ Kernel found: $kernelPath" -ForegroundColor Green
} else {
    Write-Host "✗ Kernel missing: $kernelPath" -ForegroundColor Red
}

if (Test-Path $initrdPath) {
    Write-Host "✓ Initrd found: $initrdPath" -ForegroundColor Green
} else {
    Write-Host "✗ Initrd missing: $initrdPath" -ForegroundColor Red
}

Write-Host ""
Write-Host "Boot setup complete!" -ForegroundColor Green
Write-Host "Next step: Run .\qemu_uefi_boot_folder.ps1 to start Kurono OS" -ForegroundColor Cyan