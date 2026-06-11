# Build script for C++ Kurono OS kernel
Write-Host "Building C++ Kurono OS kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\build_cpp_kernel"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# WSL Paths
$wslBuildDir = "/mnt/d/Kurono/Kurnon OS/build_cpp_kernel"
$wslSourceDir = "/mnt/d/Kurono/Kurnon OS"

# Assemble bootloader
Write-Host "Assembling bootloader..." -ForegroundColor Yellow
wsl nasm -f elf32 "$wslSourceDir/src/boot/kurono_boot.asm" -o "$wslBuildDir/boot.o"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to assemble bootloader" -ForegroundColor Red
    exit 1
}

# Compile C++ kernel components
Write-Host "Compiling C++ kernel..." -ForegroundColor Yellow

$cppFlags = "-m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib -fno-stack-protector -D_FORTIFY_SOURCE=0 -I""$wslSourceDir/src"""

$sourceFiles = @(
    "src/kernel/kurono_kernel.cpp",
    "src/drivers/graphics.cpp",
    "src/drivers/keyboard.cpp",
    "src/drivers/mouse.cpp",
    "src/drivers/rtc.cpp",
    "src/drivers/serial.cpp",
    "src/drivers/timer.cpp",
    "src/kernel/heap.cpp",
    "src/kernel/system.cpp",
    "src/kernel/time.cpp",
    "src/kernel/types.cpp",
    "src/hal/hal.cpp",
    "src/fs/vfs.cpp",
    "src/proc/scheduler.cpp",
    "src/tests/test_suite.cpp",
    "src/media/mediadecoder.cpp",
    "src/ui/font.cpp",
    "src/ui/gui.cpp",
    "src/ui/lockscreen.cpp",
    "src/ui/ui_elements.cpp",
    "src/ui/file_browser.cpp",
    "src/ui/text_layout.cpp",
    "src/system/user_mgmt.cpp",
    "src/system/input_manager.cpp",
    "src/third_party/stb_image_glue.cpp",
    "src/third_party/stb_truetype_glue.cpp",
    "src/apps/calculator.cpp"
)

$objectFiles = """$wslBuildDir/boot.o"""

foreach ($src in $sourceFiles) {
    $winPath = "D:\Kurono\Kurnon OS\$($src -replace '/','\')"
    if (Test-Path $winPath) {
        $objName = [System.IO.Path]::GetFileNameWithoutExtension($src) + ".o"
        $objPath = "$wslBuildDir/$objName"
        $srcPath = "$wslSourceDir/$src"
        Write-Host "Compiling $src..."
        $cmd = "g++ -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib -fno-stack-protector -D_FORTIFY_SOURCE=0 -I""$wslSourceDir/src"" -c ""$srcPath"" -o ""$objPath"""
        # Write command to temp script to avoid quoting issues
        $shScriptPath = "$buildDir\compile_temp.sh"
        $wslShScriptPath = "$wslBuildDir/compile_temp.sh"
        $scriptContent = "#!/bin/bash`n$cmd"
        # Ensure Unix line endings
        $scriptContent = $scriptContent -replace "`r`n", "`n"
        [System.IO.File]::WriteAllText($shScriptPath, $scriptContent)
        
        wsl bash "$wslShScriptPath"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Failed to compile $src" -ForegroundColor Red
            exit 1
        }
        $objectFiles += " ""$objPath"""
    } else {
        Write-Host "Warning: Source file not found: $winPath" -ForegroundColor Yellow
    }
}

# Link kernel
Write-Host "Linking kernel..." -ForegroundColor Yellow
$kernelElf = "$wslBuildDir/kurono_kernel.elf"
$ldCmd = "ld -m elf_i386 -T ""$wslSourceDir/src/boot/kurono_linker.ld"" -o ""$kernelElf"" $objectFiles"
$shScriptPath = "$buildDir\link_temp.sh"
$wslShScriptPath = "$wslBuildDir/link_temp.sh"
$scriptContent = "#!/bin/bash`n$ldCmd"
$scriptContent = $scriptContent -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($shScriptPath, $scriptContent)

wsl bash "$wslShScriptPath"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Failed to link kernel" -ForegroundColor Red
    exit 1
}

Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "Kernel created: $buildDir\kurono_kernel.elf" -ForegroundColor Cyan

# Copy to BootArtifacts
$srcElf = "$buildDir\kurono_kernel.elf"
$dest = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel.elf"
Copy-Item -Force $srcElf $dest
Write-Host "Copied to: $dest" -ForegroundColor Cyan
