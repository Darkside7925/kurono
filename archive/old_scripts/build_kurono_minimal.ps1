# Build script for standalone Kurono OS kernel
Write-Host "Building standalone Kurono OS kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\kurono_build"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Set-Location $buildDir

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

Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "Kernel created: $buildDir\kurono_kernel.elf" -ForegroundColor Cyan

# Test the kernel directly with QEMU
Write-Host ""
Write-Host "Testing kernel with QEMU..." -ForegroundColor Yellow
qemu-system-x86_64 -kernel kurono_kernel.elf -m 256M -vga std -serial stdio

Set-Location "D:\Kurono\Kurnon OS"