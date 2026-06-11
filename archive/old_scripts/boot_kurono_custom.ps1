# Boot Kurono OS with custom kernel using QEMU UEFI
param(
    [switch]$BIOS,
    [switch]$Debug,
    [string]$Kernel = "Kurono_kernel.elf"
)

$ErrorActionPreference = "Stop"

# Set paths
$bootRoot = "D:\Kurono\Kurnon OS\BootArtifacts"
$qemuPath = "C:\Program Files\qemu\qemu-system-x86_64.exe"

# Verify QEMU exists
if (!(Test-Path $qemuPath)) {
    Write-Error "QEMU not found at $qemuPath"
    exit 1
}

# Verify boot artifacts exist
if (!(Test-Path "$bootRoot\EFI\BOOT\grub.cfg")) {
    Write-Error "GRUB configuration not found"
    exit 1
}

if (!(Test-Path "$bootRoot\EFI\KURONO\$Kernel")) {
    Write-Error "Kernel $Kernel not found"
    exit 1
}

# Verify OVMF firmware files
if (!(Test-Path "$bootRoot\OVMF_CODE.fd") -or !(Test-Path "$bootRoot\OVMF_VARS.fd")) {
    Write-Error "OVMF firmware files not found"
    exit 1
}

Write-Host "Booting Kurono OS with custom kernel: $Kernel" -ForegroundColor Green
Write-Host "Using boot artifacts: $bootRoot" -ForegroundColor Cyan
Write-Host "Kernel: $Kernel" -ForegroundColor Cyan

# Prepare BOOT folder (FAT32 vvfat) with required UEFI files
$bootFolder = Join-Path $bootRoot "BOOT"
if (!(Test-Path $bootFolder)) { New-Item -ItemType Directory -Path $bootFolder | Out-Null }
if (!(Test-Path "$bootFolder\EFI\BOOT")) { New-Item -ItemType Directory -Path "$bootFolder\EFI\BOOT" -Force | Out-Null }
if (!(Test-Path "$bootFolder\EFI\KURONO")) { New-Item -ItemType Directory -Path "$bootFolder\EFI\KURONO" -Force | Out-Null }
Copy-Item -Force "$bootRoot\EFI\BOOT\BOOTX64.EFI" "$bootFolder\EFI\BOOT\BOOTX64.EFI"
Copy-Item -Force "$bootRoot\EFI\BOOT\grub.cfg" "$bootFolder\EFI\BOOT\grub.cfg"
Copy-Item -Force "$bootRoot\EFI\KURONO\$Kernel" "$bootFolder\EFI\KURONO\$Kernel"
Copy-Item -Force "$bootRoot\EFI\KURONO\logo.png" "$bootFolder\EFI\KURONO\logo.png"
Copy-Item -Force "$bootRoot\EFI\KURONO\logo.raw" "$bootFolder\EFI\KURONO\logo.raw"
Copy-Item -Force "$bootRoot\EFI\KURONO\wallpaper.png" "$bootFolder\EFI\KURONO\wallpaper.png"
Copy-Item -Force "$bootRoot\EFI\KURONO\font.ttf" "$bootFolder\EFI\KURONO\font.ttf"

# Copy themes
if (Test-Path "$bootRoot\EFI\KURONO\themes") {
    Copy-Item -Recurse -Force "$bootRoot\EFI\KURONO\themes" "$bootFolder\EFI\KURONO\"
}

Write-Host "BOOT folder prepared: $bootFolder" -ForegroundColor Cyan

# Generate UEFI Shell startup script to auto-run BOOTX64.EFI
$startupNsh = @"
echo -off
map -r
fs0:\EFI\BOOT\BOOTX64.EFI
fs1:\EFI\BOOT\BOOTX64.EFI
fs2:\EFI\BOOT\BOOTX64.EFI
fs3:\EFI\BOOT\BOOTX64.EFI
fs4:\EFI\BOOT\BOOTX64.EFI
fs5:\EFI\BOOT\BOOTX64.EFI
fs6:\EFI\BOOT\BOOTX64.EFI
fs7:\EFI\BOOT\BOOTX64.EFI
fs8:\EFI\BOOT\BOOTX64.EFI
fs9:\EFI\BOOT\BOOTX64.EFI
"@
Set-Content -Path (Join-Path $bootFolder "startup.nsh") -Value $startupNsh -Encoding Ascii
Write-Host "startup.nsh written: $bootFolder\startup.nsh" -ForegroundColor Cyan

# Base QEMU arguments
# Note: We use PS/2 devices (i8042) because the kernel drivers are PS/2 based.
# We disable USB input devices to avoid conflicts or absolute positioning (tablet) which the driver doesn't support yet.
$qemuArgs = @(
    "-m", "4G",
    "-smp", "4",
    "-machine", "pc",
    "-cpu", "max",
    "-device", "ich9-ahci",
    "-drive", "if=none,id=bootfolder,file=fat:rw:fat-type=32:$bootFolder,format=vvfat",
    "-device", "ide-hd,drive=bootfolder",
    "-vga", "std",
    "-display", "sdl,gl=off",
    "-serial", "mon:stdio"
)

if ($Debug) {
    $qemuArgs += @("-s", "-S", "-display", "curses")
    Write-Host "Debug mode enabled - waiting for debugger connection" -ForegroundColor Yellow
}

# No system image attached: boot entirely from ESP vvfat folder

if ($BIOS) {
    Write-Host "Using BIOS boot mode" -ForegroundColor Yellow
    $qemuArgs += @(
        "-kernel", "$bootRoot\EFI\KURONO\$Kernel",
        "-append", "console=ttyS0"
    )
} else {
    $bootx64 = Join-Path $bootRoot "EFI\BOOT\BOOTX64.EFI"
    $useUefi = $false
    if (Test-Path $bootx64) {
        try {
            $fsb = [System.IO.File]::OpenRead($bootx64)
            $buf = New-Object byte[] 2
            [void]$fsb.Read($buf,0,2)
            $fsb.Close()
            if ([System.Text.Encoding]::ASCII.GetString($buf) -eq "MZ") { $useUefi = $true }
        } catch { $useUefi = $false }
    }
    if (-not $useUefi) {
        Write-Warning "UEFI loader (BOOTX64.EFI) not found. Falling back to BIOS direct-kernel boot."
        $qemuArgs += @(
            "-kernel", "$bootRoot\EFI\KURONO\$Kernel",
            "-append", "console=ttyS0"
        )
    } else {
        Write-Host "Using UEFI boot mode" -ForegroundColor Cyan
        $qemuArgs += @(
            "-drive", "if=pflash,format=raw,unit=0,file=$bootRoot\OVMF_CODE.fd,readonly=on",
            "-drive", "if=pflash,format=raw,unit=1,file=$bootRoot\OVMF_VARS.fd"
        )
    }
}

# Start QEMU
Write-Host "Starting QEMU..." -ForegroundColor Green
Write-Host "Command: & `"$qemuPath`" $qemuArgs" -ForegroundColor DarkGray

& "$qemuPath" $qemuArgs
