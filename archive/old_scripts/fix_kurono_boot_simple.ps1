param(
  [string]$Base = "D:\Kurono\Kurnon OS",
  [string]$KernelSource = "download"
)

Write-Host "Kurono OS Boot Fix Utility" -ForegroundColor Cyan
Write-Host "=================================" -ForegroundColor Cyan

# Step 1: Create the boot directory structure
Write-Host "Creating boot directory structure..." -ForegroundColor Yellow
$bootDir = Join-Path $Base "BootArtifacts"
$efiBootDir = Join-Path $bootDir "EFI\BOOT"
$kuronoDir = Join-Path $bootDir "EFI\KURONO"

New-Item -ItemType Directory -Force -Path $efiBootDir | Out-Null
New-Item -ItemType Directory -Force -Path $kuronoDir | Out-Null

# Step 2: Create GRUB configuration
Write-Host "Creating GRUB configuration..." -ForegroundColor Yellow
$grubCfg = @(
  "set timeout=5",
  "set default=0",
  "",
  "menuentry 'Kurono OS' {",
  "  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0 quiet splash",
  "  initrd /EFI/KURONO/initramfs.cpio.gz",
  "}",
  "",
  "menuentry 'Kurono OS (Recovery)' {",
  "  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0 single",
  "  initrd /EFI/KURONO/initramfs.cpio.gz",
  "}",
  "",
  "menuentry 'EFI Shell' {",
  "  chainloader /EFI/BOOT/BOOTX64.EFI",
  "}"
)
Set-Content -Path (Join-Path $efiBootDir "grub.cfg") -Encoding Ascii -Value ($grubCfg -join "`r`n")

# Step 3: Download kernel files
Write-Host "Downloading Alpine Linux kernel files..." -ForegroundColor Yellow
$kernelUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/vmlinuz-lts"
$initrdUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/initramfs-lts"

$kernelPath = Join-Path $kuronoDir "vmlinuz"
$initrdPath = Join-Path $kuronoDir "initramfs.cpio.gz"

try {
    Invoke-WebRequest -Uri $kernelUrl -OutFile $kernelPath -UseBasicParsing
    Invoke-WebRequest -Uri $initrdUrl -OutFile $initrdPath -UseBasicParsing
    Write-Host "Download complete!" -ForegroundColor Green
} catch {
    Write-Host "Download failed. Error: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "You may need to download manually or use a different approach." -ForegroundColor Yellow
}

# Step 4: Verify boot files
Write-Host "Verifying boot files..." -ForegroundColor Yellow
$kernelExists = Test-Path $kernelPath
$initrdExists = Test-Path $initrdPath

if ($kernelExists -and $initrdExists) {
  Write-Host "✓ Boot files are ready!" -ForegroundColor Green
  Write-Host "  Kernel: $kernelPath" -ForegroundColor Cyan
  Write-Host "  Initrd: $initrdPath" -ForegroundColor Cyan
} else {
  Write-Host "⚠ Boot files are missing!" -ForegroundColor Yellow
  if (!$kernelExists) { Write-Host "  Missing: vmlinuz" -ForegroundColor Red }
  if (!$initrdExists) { Write-Host "  Missing: initramfs.cpio.gz" -ForegroundColor Red }
}

# Step 5: Create boot test script
Write-Host "Creating boot test script..." -ForegroundColor Yellow
$testScriptContent = @"
param([string]`$Base = '$Base')
Write-Host "Testing Kurono OS boot configuration..." -ForegroundColor Cyan

# Check QEMU
`$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not `$qemu) { 
  Write-Host "QEMU not found. Please install QEMU first." -ForegroundColor Red
  exit 1
}

# Check OVMF firmware
`$ovmfPaths = @('C:\Program Files\qemu', 'C:\Program Files (x86)\qemu')
`$ovmfCode = `$null; `$ovmfVars = `$null
foreach (`$p in `$ovmfPaths) {
  if (Test-Path (Join-Path `$p 'OVMF\OVMF_CODE.fd')) { `$ovmfCode = Join-Path `$p 'OVMF\OVMF_CODE.fd' }
  if (Test-Path (Join-Path `$p 'OVMF\OVMF_VARS.fd')) { `$ovmfVars = Join-Path `$p 'OVMF\OVMF_VARS.fd' }
}
if (-not `$ovmfCode -or -not `$ovmfVars) { 
  Write-Host "OVMF firmware not found. Please install QEMU with OVMF support." -ForegroundColor Red
  exit 1
}

# Check boot files
`$bootDir = Join-Path `$Base 'BootArtifacts'
`$kernelPath = Join-Path `$bootDir 'EFI\KURONO\vmlinuz'
`$initrdPath = Join-Path `$bootDir 'EFI\KURONO\initramfs.cpio.gz'

if (!(Test-Path `$kernelPath) -or !(Test-Path `$initrdPath)) {
  Write-Host "Boot files missing. Run fix_kurono_boot.ps1 first." -ForegroundColor Red
  exit 1
}

Write-Host "✓ All prerequisites met!" -ForegroundColor Green
Write-Host "Ready to boot Kurono OS." -ForegroundColor Cyan
Write-Host ""
Write-Host "To start Kurono OS, run: .\qemu_uefi_boot_folder.ps1" -ForegroundColor Yellow
"@

Set-Content -Path (Join-Path $Base "test_boot_ready.ps1") -Value $testScriptContent

Write-Host ""
Write-Host "Boot fix complete!" -ForegroundColor Green
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Run: .\test_boot_ready.ps1" -ForegroundColor White
Write-Host "  2. If ready, run: .\qemu_uefi_boot_folder.ps1" -ForegroundColor White