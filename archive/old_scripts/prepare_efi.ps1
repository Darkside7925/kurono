param(
  [string]$Base = "D:\OS\Kurono OS",
  [string]$Logo = "D:\kurono\logo.png",
  [int]$SizeMB = 64
)

$efiDir = Join-Path $Base "EFI"
New-Item -ItemType Directory -Force -Path $efiDir | Out-Null
$vhd = Join-Path $efiDir "esp.vhd"
if (!(Test-Path $vhd)) {
  $diskpartScript = @(
    "create vdisk file=`"$vhd`" maximum=$SizeMB type=fixed",
    "attach vdisk",
    "convert gpt",
    "create partition efi size=$SizeMB",
    "format quick fs=fat32 label=EFI",
    "assign letter=S",
    "exit"
  )
  $dp = Join-Path $efiDir "diskpart.txt"
  Set-Content -Path $dp -Encoding Ascii -Value ($diskpartScript -join "`r`n")
  diskpart /s "$dp"
}

$drive = Get-Volume -FileSystem FAT32 | Where-Object { $_.FileSystemLabel -eq "EFI" }
if (!$drive) { Write-Error "EFI volume not found"; exit 1 }
$letter = $drive.DriveLetter + ":"

New-Item -ItemType Directory -Force -Path "$letter\EFI\BOOT" | Out-Null
New-Item -ItemType Directory -Force -Path "$letter\EFI\KURONO" | Out-Null

if (Test-Path $Logo) { Copy-Item -Force -Path $Logo -Destination "$letter\EFI\KURONO\splash.png" }

$cfgSrc = Join-Path $efiDir "grub.cfg"
if (!(Test-Path $cfgSrc)) {
  $cfg = @(
    "set timeout=5",
    "if background_image /EFI/KURONO/splash.png; then",
    "  set color_normal=white/black",
    "  set color_highlight=yellow/black",
    "fi",
    "menuentry 'Kurono OS' {",
    "  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0",
    "  initrd /EFI/KURONO/initramfs.cpio.gz",
    "}",
    "menuentry 'EFI Shell' {",
    "  chainloader /EFI/BOOT/BOOTX64.EFI",
    "}"
  )
  Set-Content -Path $cfgSrc -Encoding Ascii -Value ($cfg -join "`r`n")
}
Copy-Item -Force -Path $cfgSrc -Destination "$letter\EFI\BOOT\grub.cfg"

Write-Host "EFI system partition prepared at $vhd" -ForegroundColor Green