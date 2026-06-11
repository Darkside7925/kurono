param(
  [string]$Base = "D:\Kurono\Kurnon OS",
  [string]$IsoOut = "D:\Kurono\Kurnon OS\BootArtifacts\kurono_bios.iso"
)

$Boot = Join-Path $Base "BootArtifacts"
$Grub = Get-Command grub-mkrescue -ErrorAction SilentlyContinue
if (-not $Grub) { Write-Host "grub-mkrescue not found; install GRUB tools (WSL/MSYS)" -ForegroundColor Yellow; exit 0 }

$IsoDir = Join-Path $Boot "iso"
New-Item -ItemType Directory -Force -Path (Join-Path $IsoDir "boot\grub") | Out-Null
Copy-Item -Force (Join-Path $Boot "EFI\KURONO\vmlinuz") (Join-Path $IsoDir "boot\vmlinuz")
Copy-Item -Force (Join-Path $Boot "EFI\KURONO\initramfs.cpio.gz") (Join-Path $IsoDir "boot\initramfs.cpio.gz")

$GrubCfg = @(
  "set timeout=5",
  "set default=0",
  "menuentry 'Kurono OS (BIOS)' {",
  "  linux /boot/vmlinuz console=ttyS0 quiet splash rdinit=/sbin/kurono-init rw",
  "  initrd /boot/initramfs.cpio.gz",
  "}",
  "menuentry 'Recovery' {",
  "  linux /boot/vmlinuz console=ttyS0 single rdinit=/bin/sh rw",
  "  initrd /boot/initramfs.cpio.gz",
  "}"
)
Set-Content -Path (Join-Path $IsoDir "boot\grub\grub.cfg") -Encoding Ascii -Value ($GrubCfg -join "`r`n")

& $Grub.Path -o $IsoOut $IsoDir
Write-Host "BIOS ISO created: $IsoOut" -ForegroundColor Green