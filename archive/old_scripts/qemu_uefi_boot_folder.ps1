param(
  [string]$Base = "D:\\OS\\Kurono OS",
  [int]$MemoryMB = 2048
)

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
$qemuExe = $null
if ($qemu) {
  $qemuExe = $qemu.Path
} else {
  $qemuFallbacks = @(
    "C:\\Program Files\\qemu\\qemu-system-x86_64.exe",
    "C:\\Program Files (x86)\\qemu\\qemu-system-x86_64.exe"
  )
  foreach ($qp in $qemuFallbacks) {
    if (Test-Path $qp) { $qemuExe = $qp; break }
  }
}
if (-not $qemuExe) { Write-Error "qemu-system-x86_64 not found (PATH or Program Files)"; exit 1 }

$ovmfPaths = @("C:\\Program Files\\qemu","C:\\Program Files (x86)\\qemu")
$ovmfCode = $null; $ovmfVars = $null
foreach ($p in $ovmfPaths) {
  $share = Join-Path $p "share"
  if (Test-Path (Join-Path $share "edk2-x86_64-code.fd")) { $ovmfCode = Join-Path $share "edk2-x86_64-code.fd" }
  if (Test-Path (Join-Path $share "edk2-x86_64-vars.fd")) { $ovmfVars = Join-Path $share "edk2-x86_64-vars.fd" }
  if (-not $ovmfVars -and (Test-Path (Join-Path $share "edk2-i386-vars.fd"))) { $ovmfVars = Join-Path $share "edk2-i386-vars.fd" }
}
if (-not $ovmfCode -or -not $ovmfVars) { Write-Error "OVMF firmware not found (edk2-* under Program Files\\qemu\\share)"; exit 1 }

$boot = Join-Path $Base "BootArtifacts"
if (!(Test-Path $boot)) { Write-Error "BootArtifacts folder not found. Run make_esp_folder.ps1 first."; exit 1 }

# Copy firmware to BootArtifacts to avoid Program Files access issues
$ovmfLocalCode = Join-Path $boot "OVMF_CODE.fd"
$ovmfLocalVars = Join-Path $boot "OVMF_VARS.fd"
Copy-Item -Force $ovmfCode $ovmfLocalCode
Copy-Item -Force $ovmfVars $ovmfLocalVars

$args = @(
  "-m", $MemoryMB,
  "-cpu","qemu64",
  "-machine","pc",
  "-device","i8042",
  "-device","qemu-xhci",
  "-device","usb-kbd",
  "-device","usb-mouse",
  "-device","usb-tablet",
  "-drive","if=pflash,format=raw,unit=0,file=$ovmfLocalCode,readonly=on",
  "-drive","if=pflash,format=raw,unit=1,file=$ovmfLocalVars",
  "-drive","file=fat:rw:$boot,format=raw,if=ide",
  "-drive","file=$boot\KuronoSystem.vhd,format=raw,if=ide",
  "-serial","mon:stdio",
  "-vga","std"
)

& $qemuExe $args
