# Test Kurono OS boot readiness
Write-Host "Testing Kurono OS boot configuration..." -ForegroundColor Cyan

# Check QEMU
$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { 
  Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
  Write-Host "Download from: https://www.qemu.org/download/" -ForegroundColor Yellow
  exit 1
} else {
  Write-Host "✓ QEMU found: $($qemu.Source)" -ForegroundColor Green
}

# Check OVMF firmware
$ovmfPaths = @('C:\Program Files\qemu', 'C:\Program Files (x86)\qemu')
$ovmfCode = $null
$ovmfVars = $null
foreach ($p in $ovmfPaths) {
  if (Test-Path (Join-Path $p 'OVMF\OVMF_CODE.fd')) { $ovmfCode = Join-Path $p 'OVMF\OVMF_CODE.fd' }
  if (Test-Path (Join-Path $p 'OVMF\OVMF_VARS.fd')) { $ovmfVars = Join-Path $p 'OVMF\OVMF_VARS.fd' }
}
if (-not $ovmfCode -or -not $ovmfVars) { 
  Write-Host "OVMF firmware not found. Please install QEMU with OVMF support." -ForegroundColor Red
  Write-Host "Look for OVMF files in QEMU installation directory." -ForegroundColor Yellow
  exit 1
} else {
  Write-Host "✓ OVMF firmware found" -ForegroundColor Green
}

# Check boot files
$bootDir = 'BootArtifacts'
$kernelPath = Join-Path $bootDir 'EFI\KURONO\vmlinuz'
$initrdPath = Join-Path $bootDir 'EFI\KURONO\initramfs.cpio.gz'

if (!(Test-Path $kernelPath)) {
  Write-Host "✗ Kernel missing: $kernelPath" -ForegroundColor Red
  exit 1
} else {
  Write-Host "✓ Kernel found: $kernelPath" -ForegroundColor Green
}

if (!(Test-Path $initrdPath)) {
  Write-Host "✗ Initrd missing: $initrdPath" -ForegroundColor Red
  exit 1
} else {
  Write-Host "✓ Initrd found: $initrdPath" -ForegroundColor Green
}

Write-Host ""
Write-Host "✓ All prerequisites met!" -ForegroundColor Green
Write-Host "Ready to boot Kurono OS." -ForegroundColor Cyan
Write-Host ""
Write-Host "To start Kurono OS, run: .\qemu_uefi_boot_folder.ps1" -ForegroundColor Yellow