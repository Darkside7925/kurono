# Enhanced Kurono OS Build Script with 180Hz Support
# Builds keyboard, mouse, graphics drivers and kernel optimized for high refresh rates

param(
    [string]$BuildDir = "c:\Users\genie\OS\build_enhanced",
    [switch]$Clean,
    [switch]$Verbose
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Kurono OS Enhanced Driver Build" -ForegroundColor Cyan  
Write-Host "  180Hz Graphics & Gaming Features" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$ErrorActionPreference = "Stop"
$SourceDir = "c:\Users\genie\OS"

# Clean if requested
if ($Clean) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
    Write-Host "Clean complete." -ForegroundColor Green
    return
}

# Create build directory
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Location $BuildDir

# Set up MSYS2 environment
$env:PATH = "C:\msys64\usr\bin;$env:PATH"

Write-Host "Checking build tools..." -ForegroundColor Yellow
try {
    & C:\msys64\usr\bin\bash.exe -lc "nasm --version | head -1; gcc --version | head -1; echo 'Tools OK'"
} catch {
    Write-Host "ERROR: Build tools not available. Please run setup first." -ForegroundColor Red
    exit 1
}

# Enhanced compiler flags for performance
$cppFlags = @(
    "-m32",
    "-ffreestanding", 
    "-O3",                    # Aggressive optimization for 180Hz performance
    "-Wall", "-Wextra",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-threadsafe-statics",
    "-nostdlib",
    "-fno-stack-protector",
    "-D_FORTIFY_SOURCE=0",
    "-march=i686",            # Target i686 specifically
    "-mtune=generic",
    "-ffast-math",            # Fast math for graphics calculations
    "-funroll-loops",         # Loop optimization for pixel operations
    "-fomit-frame-pointer",   # Extra performance
    "-DKURONO_180HZ=1",       # Enable 180Hz optimizations
    "-I`"$SourceDir/src`""    # Include source directory
)

$asmFlags = "-f elf32"

# Linker flags
$ldFlags = @(
    "-m elf_i386",
    "-T `"$SourceDir/kurono_linker.ld`"",
    "--gc-sections"           # Remove unused sections for smaller kernel
)

Write-Host "Building Enhanced Kurono OS Kernel..." -ForegroundColor Green

# Source files for enhanced kernel (only existing ones)
$sourceFiles = @(
    "src/kernel/kurono_kernel_simplified.cpp", # Simplified enhanced kernel with 180Hz
    "src/drivers/display.cpp",                 # New display controller
    "src/drivers/graphics.cpp",                # Enhanced graphics with double buffering
    "src/drivers/keyboard.cpp",                # Enhanced keyboard with LED control
    "src/drivers/mouse.cpp"                    # Enhanced mouse with high DPI
)

$objectFiles = @()

# Assemble entry point  
Write-Host "Assembling kernel entry point..." -ForegroundColor Cyan
$entryPath = Join-Path $SourceDir "entry.asm"  
if (Test-Path $entryPath) {
    $entryPathUnix = $entryPath -replace '\\', '/'
    $cmd = "nasm -f elf32 '$entryPathUnix' -o entry.o"
    Write-Host "Running: $cmd" -ForegroundColor DarkGray
    & C:\msys64\usr\bin\bash.exe -lc $cmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to assemble entry point" -ForegroundColor Red
        exit 1
    }
    $objectFiles += "entry.o"
} else {
    Write-Host "Entry point not found at: $entryPath" -ForegroundColor Red  
    exit 1
}

# Compile C++ sources
Write-Host "Compiling enhanced C++ sources..." -ForegroundColor Cyan
$compileCount = 0
$totalSources = ($sourceFiles | Where-Object { $_ -match "\.cpp$" -or $_ -match "\.c$" }).Count

foreach ($src in $sourceFiles) {
    if ($src -match "\.cpp$" -or $src -match "\.c$") {
        $srcPath = Join-Path $SourceDir $src
        if (Test-Path $srcPath) {
            $compileCount++
            $objName = [System.IO.Path]::GetFileNameWithoutExtension($src) + ".o"
            
            Write-Host "[$compileCount/$totalSources] Compiling $src..." -ForegroundColor White
            
            $cppFlagsStr = $cppFlags -join " "
            $srcPathUnix = $srcPath -replace '\\', '/'
            $cmd = "g++ $cppFlagsStr -c '$srcPathUnix' -o '$objName'"
            
            if ($Verbose) {
                Write-Host "Command: $cmd" -ForegroundColor DarkGray
            }
            
            & C:\msys64\usr\bin\bash.exe -lc $cmd
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Failed to compile $src" -ForegroundColor Red
                exit 1
            }
            
            $objectFiles += $objName
        } else {
            Write-Host "Warning: Source file not found: $srcPath" -ForegroundColor Yellow
        }
    }
}

# Link kernel using GCC
Write-Host "Linking enhanced kernel using GCC..." -ForegroundColor Cyan
$kernelElf = "kurono_enhanced_kernel.elf"
$objectList = ($objectFiles | ForEach-Object { "'$_'" }) -join " "
$linkerScript = (Join-Path $SourceDir "kurono_linker.ld") -replace '\\', '/'

# Use GCC for linking with custom linker script
$linkCmd = "gcc -m32 -nostdlib -static -Wl,-T'$linkerScript' -Wl,--gc-sections -o '$kernelElf' $objectList"

Write-Host "Linking with command: $linkCmd" -ForegroundColor DarkGray
& C:\msys64\usr\bin\bash.exe -lc $linkCmd

if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to link kernel" -ForegroundColor Red
    exit 1
}

# Verify kernel was created
if (!(Test-Path $kernelElf)) {
    Write-Host "Kernel file was not created!" -ForegroundColor Red
    exit 1
}

$kernelSize = (Get-Item $kernelElf).Length
Write-Host "Kernel successfully built! Size: $kernelSize bytes" -ForegroundColor Green

# Copy to artifacts directory
$artifactsDir = "$SourceDir/BootArtifacts/EFI/KURONO"
New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null
Copy-Item $kernelElf "$artifactsDir/kurono_enhanced_kernel.elf" -Force

Write-Host "Kernel copied to: $artifactsDir/kurono_enhanced_kernel.elf" -ForegroundColor Green

# Generate build info
$buildInfo = @"
Kurono OS Enhanced Build Information
====================================
Build Date: $(Get-Date)
Kernel Size: $kernelSize bytes
Features:
  - 180Hz refresh rate support
  - Enhanced display controller with VBE mode enumeration  
  - Double buffered graphics with VSync
  - High-DPI mouse support (up to 6400 DPI)
  - 1000Hz mouse polling
  - Keyboard LED control and USB stubs
  - Advanced acceleration curves
  - Gaming-optimized performance

Optimizations:
  - -O3 aggressive compiler optimization
  - Loop unrolling for pixel operations
  - Fast math computations
  - Frame pointer omission
  - Dead code elimination

Build Configuration:
  - Target: i686 (32-bit x86)
  - Compiler: $(& C:\msys64\usr\bin\bash.exe -lc "gcc --version | head -1")
  - Assembler: $(& C:\msys64\usr\bin\bash.exe -lc "nasm --version | head -1")

Next Steps:
  1. Test kernel in QEMU with graphics acceleration
  2. Verify 180Hz refresh rate capability  
  3. Test high-DPI mouse precision
  4. Benchmark frame timing consistency
"@

$buildInfo | Out-File "$BuildDir/BUILD_INFO.txt" -Encoding UTF8
Write-Host "Build information saved to BUILD_INFO.txt" -ForegroundColor Cyan

# Test kernel symbols (optional)
Write-Host "Checking kernel symbols..." -ForegroundColor Yellow
try {
    & C:\msys64\usr\bin\bash.exe -lc "objdump -t `"$kernelElf`" | grep -E '(kernel_main|Graphics::Init|Mouse::Init|DisplayController)' | head -10"
} catch {
    Write-Host "Symbol check skipped (objdump not available)" -ForegroundColor Yellow
}

Write-Host "" 
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Enhanced Kurono OS Build Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host "Enhanced kernel with 180Hz support is ready for deployment!" -ForegroundColor White
Write-Host "Location: $artifactsDir/kurono_enhanced_kernel.elf" -ForegroundColor White

# Quick test suggestion
Write-Host ""
Write-Host "To test with QEMU (if available):" -ForegroundColor Cyan
Write-Host "qemu-system-i386 -kernel `"$artifactsDir/kurono_enhanced_kernel.elf`" -m 256M -vga std" -ForegroundColor Gray