# PowerShell build script for Kurono OS
param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$Clean,
    [switch]$Test,
    [switch]$Install
)

Write-Host "Kurono OS Build Script" -ForegroundColor Green
Write-Host "=======================" -ForegroundColor Green

# Set up build directories
$BuildDir = "Build_Files"
$SourceDir = "."
$OutputDir = "D:\Kurono\KuronoOS\Build_Files"

if ($Clean) {
    Write-Host "Cleaning build directories..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
    if (Test-Path $OutputDir) {
        Remove-Item -Recurse -Force $OutputDir
    }
    Write-Host "Clean complete." -ForegroundColor Green
    return
}

# Create build directory
if (!(Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Source files
$SourceFiles = @(
    "kernel.c",
    "linux_bridge.c",
    "windows_bridge.c",
    "kcl_interpreter.c",
    "conflict_resolver.c",
    "security_supr_engine.c",
    "package_manager.c",
    "kurono_os.c"
)

$TestFiles = @(
    "test_suite.c"
)

# Check for compiler
$Compiler = $null
if (Get-Command gcc -ErrorAction SilentlyContinue) {
    $Compiler = "gcc"
    $CompilerArgs = "-Wall -Wextra -O2 -I. -o"
} elseif (Get-Command cl -ErrorAction SilentlyContinue) {
    $Compiler = "cl"
    $CompilerArgs = "/W4 /O2 /Fe:"
} else {
    Write-Host "ERROR: No C compiler found (gcc or cl)" -ForegroundColor Red
    Write-Host "Please install GCC or Visual Studio Build Tools" -ForegroundColor Red
    exit 1
}

Write-Host "Using compiler: $Compiler" -ForegroundColor Cyan

# Build main executable
Write-Host "Building Kurono OS main executable..." -ForegroundColor Yellow
$MainObjects = @()
foreach ($file in $SourceFiles) {
    $objFile = "$BuildDir\$($file -replace '\.c$', '.o')"
    Write-Host "  Compiling $file -> $objFile" -ForegroundColor Gray
    
    if ($Compiler -eq "gcc") {
        & gcc -Wall -Wextra -O2 -I. -c "$SourceDir\$file" -o $objFile
    } else {
        & cl /W4 /O2 /c "$SourceDir\$file" /Fo:$objFile
    }
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to compile $file" -ForegroundColor Red
        exit 1
    }
    $MainObjects += $objFile
}

# Link main executable
Write-Host "Linking Kurono OS executable..." -ForegroundColor Yellow
$MainExe = "$BuildDir\kurono_os.exe"
if ($Compiler -eq "gcc") {
    & gcc $MainObjects -o $MainExe -lcrypto -lssl
} else {
    & cl $MainObjects /Fe:$MainExe
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to link main executable" -ForegroundColor Red
    exit 1
}

Write-Host "Main executable built successfully: $MainExe" -ForegroundColor Green

# Build test suite
Write-Host "Building test suite..." -ForegroundColor Yellow
$TestObjects = @()
foreach ($file in $TestFiles) {
    $objFile = "$BuildDir\$($file -replace '\.c$', '.o')"
    Write-Host "  Compiling $file -> $objFile" -ForegroundColor Gray
    
    if ($Compiler -eq "gcc") {
        & gcc -Wall -Wextra -O2 -I. -c "$SourceDir\$file" -o $objFile
    } else {
        & cl /W4 /O2 /c "$SourceDir\$file" /Fo:$objFile
    }
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to compile $file" -ForegroundColor Red
        exit 1
    }
    $TestObjects += $objFile
}

# Link test executable
$TestExe = "$BuildDir\test_suite.exe"
if ($Compiler -eq "gcc") {
    & gcc $TestObjects -o $TestExe -lcrypto -lssl
} else {
    & cl $TestObjects /Fe:$TestExe
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to link test executable" -ForegroundColor Red
    exit 1
}

Write-Host "Test suite built successfully: $TestExe" -ForegroundColor Green

# Run tests if requested
if ($Test) {
    Write-Host "Running tests..." -ForegroundColor Yellow
    & $TestExe --test
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Some tests failed!" -ForegroundColor Red
        exit 1
    }
    Write-Host "All tests passed!" -ForegroundColor Green
}

# Install if requested
if ($Install) {
    Write-Host "Installing Kurono OS..." -ForegroundColor Yellow
    
    # Create output directory
    if (!(Test-Path $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    }
    
    # Copy executables
    Copy-Item $MainExe $OutputDir
    Copy-Item $TestExe $OutputDir
    
    # Copy headers and documentation
    Copy-Item "*.h" $OutputDir
    Copy-Item "README.md" $OutputDir
    Copy-Item "sample.kcl" $OutputDir
    Copy-Item "test_suite.sh" $OutputDir
    
    Write-Host "Installation complete!" -ForegroundColor Green
    Write-Host "Kurono OS installed to: $OutputDir" -ForegroundColor Cyan
}

Write-Host "Build completed successfully!" -ForegroundColor Green