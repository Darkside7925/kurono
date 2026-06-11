param(
  [string]$Base = "D:\\OS\\Kurono OS",
  [string]$Logo = "D:\\kurono\\logo.png"
)

$boot = Join-Path $Base "BootArtifacts"
$bootEfiBoot = Join-Path $boot "EFI\\BOOT"
$bootKurono = Join-Path $boot "EFI\\KURONO"
New-Item -ItemType Directory -Force -Path $bootEfiBoot | Out-Null
New-Item -ItemType Directory -Force -Path $bootKurono | Out-Null

if (Test-Path $Logo) { Copy-Item -Force -Path $Logo -Destination (Join-Path $bootKurono "splash.png") }

$grubCfg = @(
  "set timeout=5",
  "if background_image /EFI/KURONO/splash.png; then",
  "  set color_normal=white/black",
  "  set color_highlight=yellow/black",
  "fi",
  "menuentry 'Kurono OS' {",
  "  linux /EFI/KURONO/vmlinuz root=/dev/sda console=ttyS0 quiet splash",
  "  initrd /EFI/KURONO/initramfs.cpio.gz",
  "}"
)
Set-Content -Path (Join-Path $bootEfiBoot "grub.cfg") -Encoding Ascii -Value ($grubCfg -join "`r`n")

Write-Host "ESP folder ready: $boot" -ForegroundColor Green