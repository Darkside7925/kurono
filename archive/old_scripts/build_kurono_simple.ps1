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
Write-Host "Checking for required tools..." -ForegroundColor Yellow
try {
    Get-Command nasm -ErrorAction Stop | Out-Null
    Write-Host "✓ Found nasm" -ForegroundColor Green
} catch {
    Write-Host "✗ Missing nasm - please install it" -ForegroundColor Red
    exit 1
}

try {
    Get-Command gcc -ErrorAction Stop | Out-Null
    Write-Host "✓ Found gcc" -ForegroundColor Green
} catch {
    Write-Host "✗ Missing gcc - please install it" -ForegroundColor Red
    exit 1
}

try {
    Get-Command ld -ErrorAction Stop | Out-Null
    Write-Host "✓ Found ld" -ForegroundColor Green
} catch {
    Write-Host "✗ Missing ld - please install it" -ForegroundColor Red
    exit 1
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

Write-Host ""
Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "Kernel created: $buildDir\kurono_kernel.elf" -ForegroundColor Cyan

# Test the kernel directly with QEMU
Write-Host ""
Write-Host "Testing kernel with QEMU..." -ForegroundColor Yellow
Write-Host "Starting Kurono OS in QEMU..." -ForegroundColor Green

# Create a simple test script
$testContent = @'
Write-Host "Starting Kurono OS in QEMU..." -ForegroundColor Green
qemu-system-x86_64 -kernel kurono_kernel.elf -m 256M -vga std -serial stdio
'@

Set-Content -Path "test_kurono_simple.ps1" -Value $testContent

Write-Host ""
Write-Host "To test Kurono OS, run:" -ForegroundColor Yellow
Write-Host ".\test_kurono_simple.ps1" -ForegroundColor White

Set-Location "D:\Kurono\Kurnon OS"