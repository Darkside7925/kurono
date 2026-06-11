# Quick QEMU Setup and Test Script for Enhanced 180Hz Kernel

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "   Enhanced Kurono OS 180Hz Test" -ForegroundColor Cyan  
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$kernelPath = "BootArtifacts/EFI/KURONO/kurono_enhanced_kernel.elf"

# Check if enhanced kernel exists
if (!(Test-Path $kernelPath)) {
    Write-Host "❌ Enhanced kernel not found at: $kernelPath" -ForegroundColor Red
    Write-Host "Please build the kernel first using build_enhanced_drivers.ps1" -ForegroundColor Yellow
    exit 1
}

$kernelInfo = Get-Item $kernelPath
Write-Host "✅ Enhanced kernel found:" -ForegroundColor Green
Write-Host "   📁 Location: $($kernelInfo.FullName)" -ForegroundColor White
Write-Host "   📏 Size: $($kernelInfo.Length) bytes" -ForegroundColor White  
Write-Host "   📅 Built: $($kernelInfo.LastWriteTime)" -ForegroundColor White
Write-Host ""

# Check for QEMU
$qemuPaths = @(
    "qemu-system-i386.exe",
    "C:\Program Files\qemu\qemu-system-i386.exe", 
    "C:\QEMU\qemu-system-i386.exe",
    "C:\msys64\mingw64\bin\qemu-system-i386.exe"
)

$qemuFound = $null
foreach ($path in $qemuPaths) {
    try {
        if (Get-Command $path -ErrorAction SilentlyContinue) {
            $qemuFound = $path
            break
        }
    } catch {} 
}

if (!$qemuFound) {
    Write-Host "❌ QEMU not found in standard locations" -ForegroundColor Red
    Write-Host ""
    Write-Host "📥 To install QEMU, choose one option:" -ForegroundColor Yellow
    Write-Host "   1. Download from: https://www.qemu.org/download/" -ForegroundColor White
    Write-Host "   2. Install via Chocolatey: choco install qemu" -ForegroundColor White  
    Write-Host "   3. Install via winget: winget install qemu" -ForegroundColor White
    Write-Host ""
    Write-Host "Then run this script again to test the 180Hz kernel!" -ForegroundColor Cyan
    
    # Offer to open download page
    $choice = Read-Host "Open QEMU download page now? (y/n)"
    if ($choice -eq 'y' -or $choice -eq 'Y') {
        Start-Process "https://www.qemu.org/download/"
    }
    exit 1
}

Write-Host "✅ QEMU found at: $qemuFound" -ForegroundColor Green
Write-Host ""

# Test command 
$testCmd = "`"$qemuFound`" -kernel `"$kernelPath`" -m 256M -vga std -display sdl"

Write-Host "🚀 Starting Enhanced 180Hz Kernel Test..." -ForegroundColor Green
Write-Host ""
Write-Host "Expected behavior:" -ForegroundColor Yellow
Write-Host "  • Kernel boots with multiboot detection" -ForegroundColor White
Write-Host "  • DisplayController detects VBE modes" -ForegroundColor White  
Write-Host "  • Graphics system attempts 180Hz mode" -ForegroundColor White
Write-Host "  • Green rectangle appears with smooth rendering" -ForegroundColor White
Write-Host "  • 180Hz main loop operates continuously" -ForegroundColor White
Write-Host ""
Write-Host "Command: $testCmd" -ForegroundColor Gray
Write-Host ""
Write-Host "Press any key to launch QEMU test..." -ForegroundColor Cyan
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")

try {
    Write-Host "🎮 Launching Enhanced Kurono OS..." -ForegroundColor Green
    Invoke-Expression $testCmd
} catch {
    Write-Host ""
    Write-Host "❌ QEMU test failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ""
    Write-Host "Manual test command:" -ForegroundColor Yellow
    Write-Host $testCmd -ForegroundColor White
    Write-Host ""
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "   180Hz Graphics Test Complete!" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan