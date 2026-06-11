param(
    [int]$MemoryMB = 2048,
    [string]$Mode = "normal"
)

Write-Host "Kurono OS Hybrid Kernel v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { 
    Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
    exit 1 
}

$kernelPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\vmlinuz"
$initrdPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\initramfs-kurono.cpio.gz"

if (!(Test-Path $kernelPath)) {
    Write-Host "Kurono OS kernel not found: $kernelPath" -ForegroundColor Red
    exit 1
}

if (!(Test-Path $initrdPath)) {
    Write-Host "Kurono OS initramfs not found: $initrdPath" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Kurono OS in $Mode mode..." -ForegroundColor Green

$kernelParams = "console=ttyS0 quiet splash kurono.mode=$Mode rdinit=/bin/sh"
if ($Mode -eq "debug") {
    $kernelParams = "console=ttyS0 debug ignore_loglevel kurono.mode=debug"
} elseif ($Mode -eq "recovery") {
    $kernelParams = "console=ttyS0 single kurono.mode=recovery"
}

$args = @(
    "-m", $MemoryMB,
    "-cpu", "qemu64",
    "-kernel", $kernelPath,
    "-initrd", $initrdPath,
    "-append", $kernelParams,
    "-serial", "mon:stdio",
    "-vga", "std"
)

if (Test-Path "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\logo.png") {
    $args += @("-device", "bochs-display")
}

Write-Host "Booting Kurono OS..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& $qemu.Path $args
