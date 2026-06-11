param(
    [int]$MemoryMB = 2048
)

Write-Host "Kurono OS BIOS Boot" -ForegroundColor Cyan
Write-Host "===================" -ForegroundColor Cyan

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { 
    Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
    exit 1 
}

$bootDir = "D:\Kurono\Kurnon OS\BootArtifacts"

Write-Host "Starting Kurono OS with BIOS boot..." -ForegroundColor Green
Write-Host "Using boot directory: $bootDir" -ForegroundColor Cyan

$args = @(
    "-m", $MemoryMB,
    "-cpu", "qemu64",
    "-drive", "file=fat:rw:$bootDir,format=raw,if=ide",
    "-serial", "mon:stdio",
    "-vga", "std",
    "-boot", "order=c"
)

Write-Host "Booting Kurono OS via GRUB..." -ForegroundColor Yellow
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& $qemu.Path $args
