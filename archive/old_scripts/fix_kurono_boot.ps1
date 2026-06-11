param(
  [string]$Base = "D:\Kurono\Kurnon OS",
  [string]$KernelSource = "download", # options: download, buildroot, custom
  [switch]$SetupOnly
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

# Step 3: Handle kernel files based on source option
Write-Host "Setting up kernel files (source: $KernelSource)..." -ForegroundColor Yellow

switch ($KernelSource) {
  "download" {
    Write-Host "Option 1: Downloading minimal Linux kernel..." -ForegroundColor Green
    Write-Host "This will download a minimal Alpine Linux kernel for testing." -ForegroundColor Cyan
    
    # Create download script
    $downloadScript = @"
# Alpine Linux minimal kernel download
Write-Host "Downloading Alpine Linux kernel files..." -ForegroundColor Yellow

# Download Alpine Linux netboot files
`$kernelUrl = 'https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/vmlinuz-lts'
`$initrdUrl = 'https://dl-cdn.alpinelinux.org/alpine/v3.18/releases/x86_64/netboot/initramfs-lts'

`$kernelPath = '$kuronoDir\vmlinuz'
`$initrdPath = '$kuronoDir\initramfs.cpio.gz'

try {
    Invoke-WebRequest -Uri `$kernelUrl -OutFile `$kernelPath -UseBasicParsing
    Invoke-WebRequest -Uri `$initrdUrl -OutFile `$initrdPath -UseBasicParsing
    Write-Host "Download complete!" -ForegroundColor Green
} catch {
    Write-Host "Download failed. Error: `$(`$_.Exception.Message)" -ForegroundColor Red
    Write-Host "You may need to download manually or use a different kernel source." -ForegroundColor Yellow
}
"@
    
    if (!$SetupOnly) {
        Invoke-Expression $downloadScript
    } else {
        Write-Host "Setup complete. Run the download manually when ready." -ForegroundColor Cyan
    }
  }
  
  "buildroot" {
    Write-Host "Option 2: Using Buildroot to build kernel..." -ForegroundColor Green
    Write-Host "This requires running build_linux_from_source.ps1 first." -ForegroundColor Cyan
    
    $buildrootPath = Join-Path $Base "build\buildroot\out\images"
    if (Test-Path $buildrootPath) {
      $kernelSrc = Join-Path $buildrootPath "vmlinuz"
      $initrdSrc = Join-Path $buildrootPath "initramfs.cpio.gz"
      
      if ((Test-Path $kernelSrc) -and (Test-Path $initrdSrc)) {
        Copy-Item -Path $kernelSrc -Destination (Join-Path $kuronoDir "vmlinuz")
        Copy-Item -Path $initrdSrc -Destination (Join-Path $kuronoDir "initramfs.cpio.gz")
        Write-Host "Buildroot kernel files copied successfully!" -ForegroundColor Green
      } else {
        Write-Host "Buildroot images not found. Run build process first." -ForegroundColor Red
      }
    } else {
      Write-Host "Buildroot output directory not found." -ForegroundColor Red
    }
  }
  
  "custom" {
    Write-Host "Option 3: Custom kernel path..." -ForegroundColor Green
    $customKernel = Read-Host "Enter path to vmlinuz file"
    $customInitrd = Read-Host "Enter path to initramfs/initrd file"
    
    if (Test-Path $customKernel) {
      Copy-Item -Path $customKernel -Destination (Join-Path $kuronoDir "vmlinuz")
      Write-Host "Custom kernel copied." -ForegroundColor Green
    } else {
      Write-Host "Custom kernel not found at specified path." -ForegroundColor Red
    }
    
    if (Test-Path $customInitrd) {
      Copy-Item -Path $customInitrd -Destination (Join-Path $kuronoDir "initramfs.cpio.gz")
      Write-Host "Custom initrd copied." -ForegroundColor Green
    } else {
      Write-Host "Custom initrd not found at specified path." -ForegroundColor Red
    }
  }
}

# Step 4: Verify boot files
Write-Host "Verifying boot files..." -ForegroundColor Yellow
$kernelExists = Test-Path (Join-Path $kuronoDir "vmlinuz")
$initrdExists = Test-Path (Join-Path $kuronoDir "initramfs.cpio.gz")

if ($kernelExists -and $initrdExists) {
  Write-Host "✓ Boot files are ready!" -ForegroundColor Green
  Write-Host "  Kernel: $(Join-Path $kuronoDir "vmlinuz")" -ForegroundColor Cyan
  Write-Host "  Initrd: $(Join-Path $kuronoDir "initramfs.cpio.gz")" -ForegroundColor Cyan
} else {
  Write-Host "⚠ Boot files are missing!" -ForegroundColor Yellow
  if (!$kernelExists) { Write-Host "  Missing: vmlinuz" -ForegroundColor Red }
  if (!$initrdExists) { Write-Host "  Missing: initramfs.cpio.gz" -ForegroundColor Red }
  Write-Host "" -ForegroundColor Yellow
  Write-Host "To fix this, you can:" -ForegroundColor Cyan
  Write-Host "  1. Download Alpine Linux kernel: ./fix_kurono_boot.ps1 -KernelSource download" -ForegroundColor White
  Write-Host "  2. Build with Buildroot: ./fix_kurono_boot.ps1 -KernelSource buildroot" -ForegroundColor White
  Write-Host "  3. Use custom kernel: ./fix_kurono_boot.ps1 -KernelSource custom" -ForegroundColor White
}

# Step 5: Create boot test script
Write-Host "Creating boot test script..." -ForegroundColor Yellow
$testScript = @"
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

Set-Content -Path (Join-Path $Base "test_boot_ready.ps1") -Value $testScript

Write-Host ""
Write-Host "Boot fix complete!" -ForegroundColor Green
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Run: .\test_boot_ready.ps1" -ForegroundColor White
Write-Host "  2. If ready, run: .\qemu_uefi_boot_folder.ps1" -ForegroundColor White
Write-Host ""
Write-Host "For help, run: .\fix_kurono_boot.ps1 -KernelSource download" -ForegroundColor Yellow