param(
    [int]$MemoryMB = 2048
)

Write-Host "Kurono OS Hybrid Kernel v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) {
    $fallbacks = @(
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
    )
    foreach ($qp in $fallbacks) { if (Test-Path $qp) { $qemu = @{ Path = $qp }; break } }
}
if (-not $qemu) { Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red; exit 1 }

$kernelPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\vmlinuz"
$initrdPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\initramfs-kurono.cpio.gz"
if (!(Test-Path $initrdPath)) {
    $initrdPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\initramfs.cpio.gz"
}

if (!(Test-Path $kernelPath)) {
    Write-Host "Kurono OS kernel not found: $kernelPath" -ForegroundColor Red
    exit 1
}

if (!(Test-Path $initrdPath)) { Write-Host "Kurono OS initramfs not found: $initrdPath" -ForegroundColor Red; exit 1 }

Write-Host "Starting Kurono OS..." -ForegroundColor Green

$args = @(
    "-m", $MemoryMB,
    "-cpu", "qemu64",
    "-machine", "pc",
    "-kernel", $kernelPath,
    "-initrd", $initrdPath,
    "-append", "root=/dev/ram0 rw rdinit=/bin/sh console=ttyS0 quiet",
    "-serial", "stdio",
    "-device", "bochs-display",
    "-device", "i8042",
    "-device", "qemu-xhci",
    "-device", "usb-tablet",
    "-device", "usb-kbd"
)

# Always enable GOP-friendly display

# Expose KuronoUI via 9p for easy execution inside Linux shell
$uiDir = "D:\Kurono\Kurnon OS\KuronoUI"
if (Test-Path $uiDir) {
    $args += @("-drive", "file=fat:rw:$uiDir,format=raw,if=ide")
}

Write-Host "Booting Kurono OS..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& $qemu.Path $args
