param(
  [string]$Base = "D:\OS\Kurono OS",
  [int]$MemoryMB = 2048
)

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Error "qemu-system-x86_64 not found in PATH"; exit 1 }

$ovmfPaths = @(
  "C:\Program Files\qemu",
  "C:\Program Files (x86)\qemu"
)
$ovmfCode = $null
$ovmfVars = $null
foreach ($p in $ovmfPaths) {
  if (Test-Path (Join-Path $p "OVMF\OVMF_CODE.fd")) { $ovmfCode = Join-Path $p "OVMF\OVMF_CODE.fd" }
  if (Test-Path (Join-Path $p "OVMF\OVMF_VARS.fd")) { $ovmfVars = Join-Path $p "OVMF\OVMF_VARS.fd" }
}
if (-not $ovmfCode -or -not $ovmfVars) { Write-Error "OVMF firmware not found under Program Files\qemu"; exit 1 }

$efiDir = Join-Path $Base "EFI"
$esp = Join-Path $efiDir "esp.vhd"
if (!(Test-Path $esp)) { Write-Error "ESP VHD not found: $esp. Run prepare_efi.ps1 first."; exit 1 }

$images = Join-Path $Base "build\buildroot\out\images"
$kernel = Join-Path $images "vmlinuz"
$initrd = Join-Path $images "initramfs.cpio.gz"
$rootfs = Join-Path $Base "LinuxVM\alpine.qcow2"

$args = @(
  "-m", $MemoryMB,
  "-cpu","qemu64",
  "-drive","if=pflash,format=raw,unit=0,file=$ovmfCode,readonly=on",
  "-drive","if=pflash,format=raw,unit=1,file=$ovmfVars",
  "-drive","file=$esp,format=raw,if=virtio"
)

if (Test-Path $kernel -and Test-Path $initrd) {
  $args += @("-kernel", $kernel, "-initrd", $initrd, "-append", "root=/dev/sda console=ttyS0 quiet splash")
} elseif (Test-Path $rootfs) {
  $args += @("-drive","file=$rootfs,if=virtio")
}

$args += @("-serial","mon:stdio","-vga","std")

& $qemu.Path $args