param(
  [string]$Base = "D:\\OS\\Kurono OS",
  [int]$MemoryMB = 2048
)

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Error "qemu-system-x86_64 not found in PATH"; exit 1 }

$images = Join-Path $Base "build\\buildroot\\out\\images"
$kernel = Join-Path $images "vmlinuz"
$initrd = Join-Path $images "initramfs.cpio.gz"

if (!(Test-Path $kernel) -or !(Test-Path $initrd)) { Write-Error "Kernel/initramfs not found under $images"; exit 1 }

$args = @("-m", $MemoryMB, "-cpu","qemu64", "-kernel", $kernel, "-initrd", $initrd, "-append", "root=/dev/sda console=ttyS0 quiet")
$args += @(
  "-machine","pc",
  "-device","i8042",
  "-device","ich9-ahci",
  "-drive","file=" + (Join-Path (Join-Path $Base "EFI") "KuronoSystem.vhd") + ",format=raw,if=ide",
  "-serial","mon:stdio",
  "-vga","std"
)

& $qemu.Path $args
