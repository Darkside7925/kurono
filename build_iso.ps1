# ===========================================================================
#  Kurono OS -- Build Bootable ISO for Bare-Metal Intel
#  Creates a GRUB2 multiboot ISO that can be written to USB or burned to CD
# ===========================================================================

param(
    [switch]$NoBuild,       # Skip kernel build
    [switch]$Clean,         # Clean build first
    [string]$Output = "kurono.iso",
    [switch]$FlashUSB,
    [Nullable[int]]$DiskNumber = $null
)

$ErrorActionPreference = "Stop"
$Root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kernel   = Join-Path $Root "build\kurono.elf"
$SrcDir   = Join-Path $Root "src"
$IsoDir   = Join-Path $Root "build\iso"

function Write-Status($msg)  { Write-Host "  [*] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)      { Write-Host "  [+] $msg" -ForegroundColor Green }
function Write-Err($msg)     { Write-Host "  [!] $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host "  |    K U R O N O   O S   I S O         |" -ForegroundColor Magenta
Write-Host "  |    Bare-Metal Boot Builder            |" -ForegroundColor Magenta
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host ""

# -- Build kernel --
if (-not $NoBuild) {
    $makeCmd = "cd /mnt/c/Users/genie/OS/src && "
    if ($Clean) {
        Write-Status "Cleaning previous build..."
        $makeCmd += "make clean && "
    }
    Write-Status "Building Kurono OS kernel..."
    $buildLog = wsl -e bash -c ($makeCmd + "make 2>&1")
    $lastLine = ($buildLog | Select-Object -Last 3) -join "`n"
    if ($lastLine -match "kurono.elf") {
        Write-Ok "Kernel build successful"
    } else {
        Write-Err "Kernel build failed:"
        $buildLog | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        exit 1
    }
}

if (-not (Test-Path $Kernel)) {
    Write-Err "Kernel not found at $Kernel"
    exit 1
}

$kSize = (Get-Item $Kernel).Length
Write-Ok ("Kernel: {0:N2} MB ({1:N0} bytes)" -f ($kSize / 1MB), $kSize)

# -- Create ISO directory structure in WSL --
Write-Status "Creating ISO directory structure..."

$wslIsoScript = @'
#!/bin/bash
set -e

ROOT="/mnt/c/Users/genie/OS"
KERNEL="$ROOT/build/kurono.elf"
ISO_DIR="$ROOT/build/iso"
OUTPUT="$ROOT/OUTPUT_PLACEHOLDER"

# Clean previous
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot/grub"

# Copy kernel
cp "$KERNEL" "$ISO_DIR/boot/kurono.elf"

# Create GRUB config for bare-metal boot
cat > "$ISO_DIR/boot/grub/grub.cfg" << 'GRUBCFG'
set timeout=10
set default=0

set color_normal=white/black
set color_highlight=yellow/black

insmod all_video
insmod multiboot2
insmod multiboot
insmod vbe
insmod video_bochs
insmod video_cirrus

if [ "$grub_platform" = "efi" ]; then
    insmod efi_gop
    insmod efi_uga
fi
insmod gfxterm
set gfxmode=auto
terminal_output gfxterm

menuentry "Kurono OS (Multiboot2)" --class os {
    echo "Loading Kurono OS kernel (multiboot2)..."
    set gfxpayload=keep
    multiboot2 /boot/kurono.elf
    echo "Kernel loaded OK. Booting..."
    boot
}

menuentry "Kurono OS (Multiboot1)" --class os {
    echo "Loading Kurono OS kernel (multiboot1)..."
    set gfxpayload=keep
    multiboot /boot/kurono.elf
    echo "Kernel loaded OK. Booting..."
    boot
}

menuentry "Kurono OS (Debug - GRUB verbose)" --class os {
    set debug=all
    set pager=1
    echo "=== GRUB DEBUG MODE ==="
    echo "Loading multiboot2 module..."
    insmod multiboot2
    echo "Setting gfxpayload..."
    set gfxpayload=keep
    echo "Loading kernel ELF..."
    multiboot2 /boot/kurono.elf
    echo "Kernel loaded. Waiting 5s..."
    sleep 5
    boot
}

menuentry "Kurono OS (Text Mode - no framebuffer)" --class os {
    echo "Text mode boot..."
    set gfxpayload=text
    multiboot2 /boot/kurono.elf
    boot
}

menuentry "Kurono OS (gfxpayload=keep)" --class os {
    echo "Keep mode boot..."
    set gfxpayload=keep
    multiboot2 /boot/kurono.elf
    boot
}

menuentry "Reboot" {
    reboot
}

menuentry "Shutdown" {
    halt
}
GRUBCFG

# Check for grub-mkrescue
if ! command -v grub-mkrescue &>/dev/null; then
    echo "ERROR: grub-mkrescue not found. Install with:"
    echo "  sudo apt-get install grub-pc-bin grub-efi-amd64-bin xorriso mtools"
    exit 1
fi

echo "Building ISO with grub-mkrescue..."
grub-mkrescue -o "$OUTPUT" "$ISO_DIR" 2>&1

if [ -f "$OUTPUT" ]; then
    SIZE=$(stat -c%s "$OUTPUT")
    echo "ISO created: $OUTPUT ($SIZE bytes)"
else
    echo "ERROR: ISO creation failed"
    exit 1
fi
'@

# Replace placeholder with actual output path
$wslOutput = $Output -replace '\\','/'
if (-not ($wslOutput -match '^/')) {
    $wslOutput = "/mnt/c/Users/genie/OS/new/$wslOutput"
}
$wslIsoScript = $wslIsoScript -replace 'OUTPUT_PLACEHOLDER', ($Output -replace '\\','/')

$scriptPath = Join-Path $Root "build\_build_iso.sh"
# Write with Unix LF line endings so bash can execute it
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$lfContent = $wslIsoScript -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($scriptPath, $lfContent, $utf8NoBom)

Write-Status "Building bootable ISO via WSL..."
$result = wsl -e bash -c "chmod +x /mnt/c/Users/genie/OS/build/_build_iso.sh && /mnt/c/Users/genie/OS/build/_build_iso.sh 2>&1"
$result | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }

$isoPath = Join-Path $Root $Output
if (Test-Path $isoPath) {
    $isoSize = (Get-Item $isoPath).Length
    Write-Host ""
    Write-Ok ("ISO created: $Output ({0:N2} MB)" -f ($isoSize / 1MB))
    if ($FlashUSB) {
        Write-Host ""
        Write-Status "Flashing ISO to removable USB..."
        $flashScript = Join-Path $Root "flash_usb.ps1"
        $flashArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $flashScript,
            "-IsoPath", $isoPath,
            "-Force"
        )
        if ($null -ne $DiskNumber) {
            $flashArgs += @("-DiskNumber", $DiskNumber)
        }

        & powershell.exe @flashArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Err "USB flashing failed"
            exit 1
        }
    }
    Write-Host ""
    Write-Host "  +---------------------------------------------+" -ForegroundColor Green
    Write-Host "  |  BARE-METAL BOOT INSTRUCTIONS               |" -ForegroundColor Green
    Write-Host "  |                                             |" -ForegroundColor Green
    Write-Host "  |  1. Write to USB:                           |" -ForegroundColor Green
    if ($FlashUSB) {
        Write-Host "  |     Done automatically                      |" -ForegroundColor Green
    } else {
        Write-Host "  |     Run .\flash_usb.ps1                     |" -ForegroundColor Green
    }
    Write-Host "  |                                             |" -ForegroundColor Green
    Write-Host "  |  2. BIOS Settings (Intel):                  |" -ForegroundColor Green
    Write-Host "  |     - Enable Intel VT-x / VMX               |" -ForegroundColor Green
    Write-Host "  |     - Enable Intel VT-d (optional)          |" -ForegroundColor Green
    Write-Host "  |     - Disable Secure Boot                   |" -ForegroundColor Green
    Write-Host "  |     - Set boot to Legacy/CSM or UEFI        |" -ForegroundColor Green
    Write-Host "  |                                             |" -ForegroundColor Green
    Write-Host "  |  3. Boot from USB and select 'Kurono OS'    |" -ForegroundColor Green
    Write-Host "  +---------------------------------------------+" -ForegroundColor Green
} else {
    Write-Err "ISO build failed"
    exit 1
}
