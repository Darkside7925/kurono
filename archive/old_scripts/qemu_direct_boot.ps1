param(
  [string]$Base = "D:\Kurono\Kurnon OS",
  [int]$MemoryMB = 2048
)

Write-Host "Kurono OS QEMU Direct Kernel Boot" -ForegroundColor Cyan
Write-Host "=================================" -ForegroundColor Cyan

# Check QEMU
$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { 
  Write-Error "qemu-system-x86_64 not found in PATH"; 
  Write-Host "Please install QEMU from: https://www.qemu.org/download/" -ForegroundColor Yellow
  exit 1 
}

# Check boot files
$bootDir = Join-Path $Base "BootArtifacts"
$kernelPath = Join-Path $bootDir "EFI\KURONO\vmlinuz"
$initrdPath = Join-Path $bootDir "EFI\KURONO\initramfs.cpio.gz"

if (!(Test-Path $kernelPath)) { 
  Write-Error "Kernel not found at: $kernelPath"; 
  Write-Host "Run setup_boot_simple.ps1 first." -ForegroundColor Yellow
  exit 1 
}

if (!(Test-Path $initrdPath)) { 
  Write-Error "Initrd not found at: $initrdPath"; 
  Write-Host "Run setup_boot_simple.ps1 first." -ForegroundColor Yellow
  exit 1 
}

Write-Host "Starting Kurono OS with direct kernel boot..." -ForegroundColor Green
Write-Host "Kernel: $kernelPath" -ForegroundColor Cyan
Write-Host "Initrd: $initrdPath" -ForegroundColor Cyan

# QEMU arguments for direct kernel boot
$args = @(
  "-m", $MemoryMB,
  "-cpu","qemu64",
  "-machine","pc",
  "-device","i8042",
  "-device","ich9-ahci",
  "-drive","file=" + (Join-Path $bootDir "KuronoSystem.vhd") + ",format=raw,if=ide",
  "-kernel", $kernelPath,
  "-initrd", $initrdPath,
  "-append", "console=ttyS0 quiet splash",
  "-serial","mon:stdio",
  "-vga","std"
)

Write-Host "QEMU Arguments: $($args -join ' ')" -ForegroundColor DarkGray
Write-Host ""
Write-Host "Press Ctrl+C to stop the VM" -ForegroundColor Yellow
Write-Host ""

& $qemu.Path $args
