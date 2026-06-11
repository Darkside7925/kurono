param(
  [string]$Base = "D:\Kurono\Kurnon OS",
  [int]$MemoryMB = 2048
)

Write-Host "Kurono OS QEMU BIOS Boot" -ForegroundColor Cyan
Write-Host "=========================" -ForegroundColor Cyan

# Check QEMU
$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { 
  Write-Error "qemu-system-x86_64 not found in PATH"; 
  Write-Host "Please install QEMU from: https://www.qemu.org/download/" -ForegroundColor Yellow
  exit 1 
}

# Check boot files
$boot = Join-Path $Base "BootArtifacts"
if (!(Test-Path $boot)) { 
  Write-Error "BootArtifacts folder not found. Run setup_boot_simple.ps1 first."; 
  exit 1 
}

Write-Host "Starting Kurono OS with BIOS boot..." -ForegroundColor Green
Write-Host "Using boot directory: $boot" -ForegroundColor Cyan

# QEMU arguments for BIOS boot with FAT disk
$args = @(
  "-m", $MemoryMB,
  "-cpu","qemu64",
  "-drive","file=fat:rw:$boot,format=raw,if=ide",
  "-serial","mon:stdio",
  "-vga","std",
  "-boot","order=c"  # Boot from hard disk (our FAT drive)
)

Write-Host "QEMU Arguments: $($args -join ' ')" -ForegroundColor DarkGray
Write-Host ""
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& $qemu.Path $args