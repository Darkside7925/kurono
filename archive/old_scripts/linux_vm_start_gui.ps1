param(
  [string]$VmDir = "D:\Important\Kurono\LinuxVM"
)

Write-Host "Starting Kurono Linux VM (GUI) in $VmDir" -ForegroundColor Green
$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
$qimg = Get-Command qemu-img -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Error "qemu-system-x86_64 not found in PATH"; exit 1 }
if (-not $qimg) { Write-Error "qemu-img not found in PATH"; exit 1 }

$disk = Join-Path $VmDir "alpine.qcow2"
if (!(Test-Path $disk)) { Write-Error "Disk not found: $disk. Run linux_vm_start.ps1 to create."; exit 1 }

$args = @(
  "-m","2048",
  "-cpu","qemu64",
  "-drive","file=$disk,if=virtio",
  "-boot","c",
  "-vga","std",
  "-nic","user,hostfwd=tcp::2222-:22"
)

Write-Host "Launching QEMU with GUI (std VGA). SSH on localhost:2222" -ForegroundColor Cyan
& $qemu.Path $args