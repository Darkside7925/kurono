param(
  [string]$VmDir = "D:\Important\Kurono\LinuxVM",
  [string]$IsoUrl = "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-standard-3.20.3-x86_64.iso",
  [int]$DiskGB = 4
)

Write-Host "Starting Kurono Linux VM setup in $VmDir" -ForegroundColor Green
New-Item -ItemType Directory -Force -Path $VmDir | Out-Null

$qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
$qimg = Get-Command qemu-img -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Error "qemu-system-x86_64 not found in PATH"; exit 1 }
if (-not $qimg) { Write-Error "qemu-img not found in PATH"; exit 1 }

$iso = Join-Path $VmDir "alpine.iso"
if (!(Test-Path $iso)) {
  Write-Host "Downloading Alpine ISO..." -ForegroundColor Yellow
  Invoke-WebRequest -Uri $IsoUrl -OutFile $iso
}

$disk = Join-Path $VmDir "alpine.qcow2"
if (!(Test-Path $disk)) {
  & $qimg.Path create -f qcow2 $disk "$DiskGB"G | Out-Null
}

$args = @(
  "-m","1024",
  "-cpu","qemu64",
  "-drive","file=$disk,if=virtio",
  "-cdrom",$iso,
  "-boot","d",
  "-nographic",
  "-serial","mon:stdio",
  "-nic","user,hostfwd=tcp::2222-:22"
)

Write-Host "Launching QEMU... (SSH will be on localhost:2222 after install)" -ForegroundColor Cyan
& $qemu.Path $args