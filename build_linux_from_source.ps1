param(
  [string]$Base = "D:\\OS\\Kurono OS",
  [string]$BuildrootVersion = "2024.08"
)

Write-Host "Kurono OS: Preparing Linux-from-source build environment" -ForegroundColor Green
$brDir = Join-Path $Base "build\buildroot"
New-Item -ItemType Directory -Force -Path $brDir | Out-Null

$brTar = Join-Path $brDir ("buildroot-" + $BuildrootVersion + ".tar.gz")
$brSrc = Join-Path $brDir ("buildroot-" + $BuildrootVersion)
if (!(Test-Path $brTar) -and !(Test-Path $brSrc)) {
  $url = "https://buildroot.org/downloads/buildroot-" + $BuildrootVersion + ".tar.gz"
  Write-Host "Downloading Buildroot $BuildrootVersion..." -ForegroundColor Yellow
  Invoke-WebRequest -Uri $url -OutFile $brTar
}
if (!(Test-Path $brSrc) -and (Test-Path $brTar)) {
  Write-Host "Extracting Buildroot..." -ForegroundColor Yellow
  tar -xf $brTar -C $brDir
}

$cfgDir = Join-Path $Base "build\config"
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$defconfig = Join-Path $cfgDir "kurono_defconfig"
$overlay = Join-Path $Base "build\overlay"
New-Item -ItemType Directory -Force -Path $overlay | Out-Null

$def = @(
  "BR2_x86_64=y",
  "BR2_TOOLCHAIN_BUILDROOT_GLIBC=y",
  "BR2_INIT_BUSYBOX=y",
  "BR2_TARGET_ROOTFS_EXT2=y",
  "BR2_TARGET_ROOTFS_EXT2_4=y",
  "BR2_TARGET_GENERIC_GETTY=y",
  "BR2_TARGET_GENERIC_GETTY_PORT=\"ttyS0\"",
  "BR2_TARGET_GENERIC_GETTY_BAUDRATE=\"115200\"",
  "BR2_PACKAGE_DROPBEAR=y",
  "BR2_PACKAGE_OPENSSH=y",
  "BR2_PACKAGE_XORG7=y",
  "BR2_PACKAGE_XFCE4=y",
  "BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW=y",
  "BR2_ROOTFS_OVERLAY=\"" + $overlay.Replace("\","/") + "\"",
  "BR2_LINUX_KERNEL=y",
  "BR2_LINUX_KERNEL_USE_DEFCONFIG=y",
  "BR2_LINUX_KERNEL_DEFCONFIG=\"x86_64_defconfig\"",
  "BR2_LINUX_KERNEL_IMAGE=y",
  "BR2_LINUX_KERNEL_VMLINUZ=y"
)
Set-Content -Encoding Ascii -Path $defconfig -Value ($def -join "`n")

$initd = Join-Path $overlay "etc\init.d\S99kurono"
New-Item -ItemType Directory -Force -Path (Split-Path $initd) | Out-Null
Set-Content -Encoding Ascii -Path $initd -Value @"
#!/bin/sh
case "$1" in
  start)
    /etc/init.d/dropbear start 2>/dev/null || true
    /etc/init.d/sshd start 2>/dev/null || true
    ;;
esac
"@

Write-Host "Buildroot prepared. To build, run MSYS2 shell and execute:" -ForegroundColor Cyan
Write-Host "  cd `"$brSrc`""
Write-Host "  make O=`"$brDir\out`" BR2_DEFCONFIG=`"$defconfig`" kurono_defconfig"
Write-Host "  make -C `"$brSrc`" O=`"$brDir\out`" -j$(nproc)"
Write-Host "Artifacts will be under: $brDir\out\images" -ForegroundColor Cyan