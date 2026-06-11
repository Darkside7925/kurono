# Build script for standalone Kurono OS kernel
# This creates a completely independent OS kernel that doesn't depend on Linux

Write-Host "Building standalone Kurono OS kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\kurono_build"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir

Set-Location $buildDir

# Check for required tools
$requiredTools = @("nasm", "gcc", "ld", "grub-mkrescue", "qemu-system-x86_64")
foreach ($tool in $requiredTools) {
    try {
        Get-Command $tool -ErrorAction Stop | Out-Null
        Write-Host "✓ Found $tool" -ForegroundColor Green
    }
    catch {
        Write-Host "✗ Missing $tool - please install it" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "Assembling bootloader..." -ForegroundColor Yellow
nasm -f elf32 "D:\Kurono\Kurnon OS\kurono_boot.asm" -o boot.o
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to assemble bootloader" -ForegroundColor Red
    exit 1
}

Write-Host "Compiling kernel..." -ForegroundColor Yellow
gcc -m32 -c -ffreestanding -O2 -Wall -Wextra -nostdlib -nostartfiles -nodefaultlibs `
    "D:\Kurono\Kurnon OS\kurono_kernel.c" -o kernel.o
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to compile kernel" -ForegroundColor Red
    exit 1
}

Write-Host "Linking kernel..." -ForegroundColor Yellow
ld -m elf_i386 -T "D:\Kurono\Kurnon OS\kurono_linker.ld" boot.o kernel.o -o kurono_kernel.elf
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to link kernel" -ForegroundColor Red
    exit 1
}

# Create ISO structure
Write-Host "Creating bootable ISO..." -ForegroundColor Yellow
$isoDir = "$buildDir\iso"
New-Item -ItemType Directory -Force -Path "$isoDir\boot\grub"

# Copy kernel
Copy-Item "kurono_kernel.elf" "$isoDir\boot\"

# Create GRUB config
$grubConfig = @"
set timeout=5
set default=0

menuentry "Kurono OS" {
    multiboot /boot/kurono_kernel.elf
    boot
}

menuentry "Kurono OS (Safe Mode)" {
    multiboot /boot/kurono_kernel.elf
    boot
}
"@

Set-Content -Path "$isoDir\boot\grub\grub.cfg" -Value $grubConfig

# Create bootable ISO
grub-mkrescue -o kurono_os.iso $isoDir
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to create ISO" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "ISO created: $buildDir\kurono_os.iso" -ForegroundColor Cyan
Write-Host ""
Write-Host "To test Kurono OS, run:" -ForegroundColor Yellow
Write-Host "qemu-system-x86_64 -cdrom kurono_os.iso -m 256M" -ForegroundColor White

# Create test script
$testScript = @"
Write-Host "Starting Kurono OS in QEMU..." -ForegroundColor Green
qemu-system-x86_64 -cdrom kurono_os.iso -m 256M -vga std
"@

Set-Content -Path "$buildDir\test_kurono.ps1" -Value $testScript

Write-Host ""
Write-Host "Or run: .\test_kurono.ps1" -ForegroundColor Yellow

Set-Location "D:\Kurono\Kurnon OS"