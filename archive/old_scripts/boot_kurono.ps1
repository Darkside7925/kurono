param(
  [string]$Mode = "menu",
  [int]$MemoryMB = 2048,
  [string]$QemuPath = $null,
  [string]$OvmfCodePath = $null,
  [string]$OvmfVarsPath = $null
)

Write-Host "Kurono OS Boot Manager" -ForegroundColor Cyan
Write-Host "=====================" -ForegroundColor Cyan

function Resolve-Qemu {
  param([string]$Override)
  if ($Override -and (Test-Path $Override)) { return $Override }
  $candidates = @('qemu-system-x86_64','qemu-system-x86_64.exe','qemu-system-x86_64.cmd','qemu-system-x86_64.bat')
  foreach ($c in $candidates) { $cmd = Get-Command $c -ErrorAction SilentlyContinue; if ($cmd) { return $cmd.Path } }
  $dirs = @()
  if ($env:ProgramFiles) { $dirs += Join-Path $env:ProgramFiles 'QEMU' ; $dirs += Join-Path $env:ProgramFiles 'qemu' }
  if ($env:ChocolateyInstall) { $dirs += Join-Path $env:ChocolateyInstall 'bin' }
  if ($env:SCOOP) { $dirs += Join-Path $env:SCOOP 'apps\qemu\current' }
  $dirs += (Join-Path $env:USERPROFILE 'scoop\apps\qemu\current')
  foreach ($d in $dirs) {
    if (Test-Path $d) {
      $found = Get-ChildItem -Path $d -Recurse -Filter 'qemu-system-x86_64.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($found) { return $found.FullName }
    }
  }
  return $null
}

function Resolve-Ovmf {
  param([string]$OverrideCode,[string]$OverrideVars)
  $code = $null; $vars = $null
  if ($OverrideCode -and (Test-Path $OverrideCode)) { $code = $OverrideCode }
  if ($OverrideVars -and (Test-Path $OverrideVars)) { $vars = $OverrideVars }
  if ($code -and $vars) { return @{code=$code;vars=$vars} }
  $roots = @()
  if ($env:ProgramFiles) { $roots += Join-Path $env:ProgramFiles 'QEMU'; $roots += Join-Path $env:ProgramFiles 'qemu' }
  foreach ($r in $roots) {
    if (Test-Path $r) {
      $c = Get-ChildItem -Path $r -Recurse -Filter '*edk2*-code.fd' -ErrorAction SilentlyContinue | Select-Object -First 1
      $v = Get-ChildItem -Path $r -Recurse -Filter '*edk2*-vars.fd' -ErrorAction SilentlyContinue | Select-Object -First 1
      if (-not $c) { $c = Get-ChildItem -Path $r -Recurse -Filter '*OVMF*CODE*.fd' -ErrorAction SilentlyContinue | Select-Object -First 1 }
      if (-not $v) { $v = Get-ChildItem -Path $r -Recurse -Filter '*OVMF*VARS*.fd' -ErrorAction SilentlyContinue | Select-Object -First 1 }
      if ($c -and $v) { return @{code=$c.FullName;vars=$v.FullName} }
    }
  }
  return $null
}

$qemuPath = Resolve-Qemu -Override $QemuPath
if (-not $qemuPath) {
  Write-Host "QEMU not found. Please install or provide -QemuPath." -ForegroundColor Red
  Write-Host "Download from: https://www.qemu.org/download/" -ForegroundColor Yellow
  exit 1
}

${bootDir} = Join-Path $PSScriptRoot "BootArtifacts"
${linuxKernelPath} = Join-Path $bootDir "EFI\KURONO\vmlinuz"
${linuxInitrdPath} = Join-Path $bootDir "EFI\KURONO\initramfs.cpio.gz"
function Resolve-CustomKernel {
  param([string]$base)
  $names = @('Kurono_kernel','kurono_kernel','Kurono_kernel.elf','kurono_kernel.elf')
  foreach ($n in $names) {
    $p = Join-Path $base ("EFI\KURONO\" + $n)
    if (Test-Path $p) { return $p }
  }
  $found = Get-ChildItem -Path (Join-Path $base 'EFI\KURONO') -Filter 'k*kernel*' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { return $found.FullName }
  return $null
}
${customKernelPath} = Resolve-CustomKernel -base $bootDir

switch ($Mode.ToLower()) {
  "help" {
    Write-Host "Available boot modes:" -ForegroundColor Yellow
    Write-Host "  menu  - Interactive boot menu (default)" -ForegroundColor White
    Write-Host "  direct - Direct kernel boot (fastest)" -ForegroundColor White
    Write-Host "  bios   - BIOS boot with GRUB" -ForegroundColor White
    Write-Host "  custom - Boot Kurono custom kernel UI" -ForegroundColor White
    Write-Host "  help   - Show this help" -ForegroundColor White
    Write-Host ""
    Write-Host "Examples:" -ForegroundColor Yellow
    Write-Host "  .\boot_kurono.ps1" -ForegroundColor White
    Write-Host "  .\boot_kurono.ps1 -Mode direct" -ForegroundColor White
    Write-Host "  .\boot_kurono.ps1 -Mode bios" -ForegroundColor White
    Write-Host "  .\boot_kurono.ps1 -Mode custom" -ForegroundColor White
  }
  
  "direct" {
    Write-Host "Starting Direct Kernel Boot..." -ForegroundColor Green
    Write-Host "This will boot the Alpine Linux kernel directly." -ForegroundColor Cyan
    Write-Host ""
    if (-not (Test-Path $linuxKernelPath) -or -not (Test-Path $linuxInitrdPath)) {
      Write-Host "Linux boot files missing. Run setup_boot_simple.ps1 first." -ForegroundColor Red
      exit 1
    }
    
    $args = @(
      "-m", $MemoryMB,
      "-cpu","qemu64",
      "-kernel", $linuxKernelPath,
      "-initrd", $linuxInitrdPath,
      "-append", "console=ttyS0 quiet splash rdinit=/init",
      "-serial","mon:stdio",
      "-vga","std"
    )
    
    Write-Host "QEMU Command: qemu-system-x86_64 $($args -join ' ')" -ForegroundColor DarkGray
    Write-Host ""
    & $qemuPath $args
  }
  
  "bios" {
    Write-Host "Starting UEFI Boot with GRUB..." -ForegroundColor Green
    Write-Host "Using BootArtifacts EFI layout." -ForegroundColor Cyan
    $ovmf = Resolve-Ovmf -OverrideCode $OvmfCodePath -OverrideVars $OvmfVarsPath
    if (-not $ovmf) {
      Write-Host "OVMF firmware not found. Provide -OvmfCodePath and -OvmfVarsPath." -ForegroundColor Red
      exit 1
    }
    $args = @(
      "-m", $MemoryMB,
      "-cpu","qemu64",
      "-drive","if=pflash,format=raw,file=$($ovmf.code)",
      "-drive","if=pflash,format=raw,file=$($ovmf.vars)",
      "-drive","file=fat:rw:$bootDir,format=raw,if=ide",
      "-serial","mon:stdio",
      "-vga","std"
    )
    Write-Host "QEMU Command: qemu-system-x86_64 $($args -join ' ')" -ForegroundColor DarkGray
    Write-Host ""
    & $qemuPath $args
  }

  "custom" {
    Write-Host "Starting Kurono Custom Kernel UI..." -ForegroundColor Green
    if (-not (Test-Path $customKernelPath)) {
      Write-Host "Custom kernel not found at $customKernelPath" -ForegroundColor Red
      Write-Host "Run copy_kernel_to_boot.ps1 first." -ForegroundColor Yellow
      exit 1
    }
    $args = @(
      "-m", $MemoryMB,
      "-cpu","qemu64",
      "-kernel", $customKernelPath,
      "-serial","mon:stdio",
      "-vga","std"
    )
    Write-Host "QEMU Command: qemu-system-x86_64 $($args -join ' ')" -ForegroundColor DarkGray
    Write-Host ""
    & $qemuPath $args
  }
  
  default {
    Write-Host "Kurono OS Boot Menu" -ForegroundColor Yellow
    Write-Host "==================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "1. Direct Kernel Boot (Recommended)" -ForegroundColor White
    Write-Host "   Fastest boot, boots Alpine Linux kernel directly" -ForegroundColor Gray
    Write-Host ""
    Write-Host "2. UEFI Boot with GRUB" -ForegroundColor White
    Write-Host "   Uses GRUB bootloader, more traditional approach" -ForegroundColor Gray
    Write-Host ""
    Write-Host "3. Kurono Custom Kernel UI" -ForegroundColor White
    Write-Host "   Boots the custom ELF kernel with logo and spinner" -ForegroundColor Gray
    Write-Host ""
    Write-Host "4. Exit" -ForegroundColor White
    Write-Host ""
    
    $choice = Read-Host "Select boot option (1-4)"
    
    switch ($choice) {
      "1" { & $PSCommandPath -Mode "direct" -MemoryMB $MemoryMB -QemuPath $qemuPath }
      "2" { & $PSCommandPath -Mode "bios" -MemoryMB $MemoryMB -QemuPath $qemuPath -OvmfCodePath $OvmfCodePath -OvmfVarsPath $OvmfVarsPath }
      "3" { & $PSCommandPath -Mode "custom" -MemoryMB $MemoryMB -QemuPath $qemuPath }
      "4" { Write-Host "Exiting..." -ForegroundColor Yellow; exit 0 }
      default { Write-Host "Invalid choice. Exiting..." -ForegroundColor Red; exit 1 }
    }
  }
}
