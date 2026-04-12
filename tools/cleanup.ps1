# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS  -  Directory Cleanup & Organization Script
#  Moves all loose files into organized folders, removes stale artifacts
# ═══════════════════════════════════════════════════════════════════════════

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $Root   # Go up from tools/ to project root

function Write-Status($msg)  { Write-Host "  [*] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)      { Write-Host "  [+] $msg" -ForegroundColor Green }
function Write-Warn($msg)    { Write-Host "  [~] $msg" -ForegroundColor Yellow }

Write-Host ""
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host "  |   Kurono OS -- Directory Cleanup      |" -ForegroundColor Magenta
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host ""

Push-Location $Root

# ──────────────────────────────────────────────────────────────────────────
#  1. Create organized folder structure
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Creating organized folder structure..."

$folders = @(
    "archive/old_scripts",
    "archive/old_sources",
    "archive/old_builds",
    "archive/old_boot",
    "archive/old_docs",
    "archive/old_logs",
    "archive/old_images",
    "archive/old_python",
    "archive/old_configs",
    "archive/old_misc",
    "tools"
)

foreach ($f in $folders) {
    $path = Join-Path $Root $f
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  2. Delete stale object files (.o) from root
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Removing stale object files from root..."
$oFiles = Get-ChildItem -Path $Root -Filter "*.o" -File
foreach ($f in $oFiles) {
    Remove-Item $f.FullName -Force
    Write-Warn "  Deleted: $($f.Name)"
}

# ──────────────────────────────────────────────────────────────────────────
#  3. Delete stale build directories (old partial builds)
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Removing stale build directories..."
$staleBuildDirs = @(
    "build_bootable", "build_clean", "build_enhanced", "build_final",
    "build_fixed", "build_working", "kurono_simple_build"
)
foreach ($d in $staleBuildDirs) {
    $path = Join-Path $Root $d
    if (Test-Path $path) {
        Remove-Item $path -Recurse -Force
        Write-Warn "  Deleted: $d/"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  4. Delete stale binary/image files from root
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Removing stale binaries and disk images..."
$staleBinaries = @(
    "kurono_kernel.bin", "kurono_kernel.elf", "kurono_kernel.o",
    "kurono_simple.bin", "working_enhanced_kernel.elf", "working_kernel.o",
    "kurono_os.img", "kurono_os_fixed.img", "kurono_os_tty.img", "kurono_os_working.img"
)
foreach ($f in $staleBinaries) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Warn "  Deleted: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  5. Delete stale log files
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Removing stale log files..."
$staleLogs = @(
    "serial.log", "serial_final.log", "serial_out.log", "serial_out.txt",
    "serial_out2.log", "serial_test2.log", "qemu_err.txt", "qemu_out.txt"
)
foreach ($f in $staleLogs) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Warn "  Deleted: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  6. Move old PowerShell/batch scripts → archive/old_scripts
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old scripts to archive/old_scripts..."
$oldScripts = @(
    # Boot scripts
    "boot_kurono.ps1", "boot_kurono_bios.ps1", "boot_kurono_complete.ps1",
    "boot_kurono_custom.ps1", "boot_kurono_direct.ps1", "boot_kurono_final.ps1",
    "boot_kurono_fixed.ps1", "boot_kurono_fixed_final.ps1", "boot_kurono_menu.ps1",
    "boot_kurono_simple.ps1", "boot_qemu_efi.ps1",
    # Build scripts
    "build.ps1", "build_bios_iso.ps1", "build_cpp.ps1",
    "build_custom_kernel.ps1", "build_custom_kernel_fixed.ps1",
    "build_enhanced_drivers.ps1", "build_kernel.sh",
    "build_kurono_basic.ps1", "build_kurono_complete.ps1", "build_kurono_cpp.ps1",
    "build_kurono_kernel.ps1", "build_kurono_minimal.ps1", "build_kurono_simple.ps1",
    "build_linux_from_source.ps1", "build_standalone_kurono.ps1",
    "build_standalone_kurono_fixed.ps1", "build_with_vsdev.bat",
    # Create scripts
    "create_disk_image.ps1", "create_iso.sh",
    "create_kurono_advanced.ps1", "create_kurono_bootable.ps1",
    "create_kurono_final.ps1", "create_kurono_kernel.ps1",
    "create_kurono_kernel_clean.ps1", "create_kurono_professional.ps1",
    "create_kurono_simple.ps1", "create_kurono_tty.ps1",
    "create_kurono_ultimate.ps1", "create_working_boot.ps1",
    # Fix/setup/test scripts
    "fix_kurono_boot.ps1", "fix_kurono_boot_simple.ps1",
    "copy_kernel_to_boot.ps1", "convert_logo.ps1",
    "make_esp_folder.ps1", "prepare_efi.ps1",
    "qemu_bios_boot.ps1", "qemu_bios_boot_fixed.ps1",
    "qemu_direct_boot.ps1", "qemu_uefi_boot_folder.ps1",
    "setup_boot_simple.ps1", "setup_kurono_linux_micro.ps1",
    "setup_kurono_linux_root.ps1", "setup_kurono_simple.ps1",
    "sign_kurono_boot.ps1", "start_kurono_standalone.ps1",
    "test_boot.bat", "test_boot_ready.ps1", "test_enhanced_kernel.ps1",
    "test_ready.ps1", "test_suite.sh",
    "linux_vm_start.ps1", "linux_vm_start_gui.ps1", "linux_vm_stop.ps1",
    # Run/cmd scripts
    "run_kernel.bat", "run_kernel_vfat.bat",
    "kurono.cmd", "kurono_complex.cmd"
)
foreach ($f in $oldScripts) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_scripts/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  7. Move old C/C++/ASM source files → archive/old_sources
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old source files to archive/old_sources..."
$oldSources = @(
    "conflict_resolver.c", "conflict_resolver.h",
    "kernel.cpp", "kernel.h",
    "kcl_interpreter.c", "kcl_interpreter.h",
    "kurono_kernel.c", "kurono_kernel.h",
    "kurono_os.c",
    "linux_bridge.c", "linux_bridge.h",
    "linux_sync.c", "linux_sync.h",
    "package_manager.c", "package_manager.h",
    "security_supr_engine.c", "security_supr_engine.h",
    "windows_bridge.c", "windows_bridge.h",
    "test_kernel.cpp", "test_suite.c", "working_kernel.cpp",
    "entry.asm", "entry.S", "entry_final.asm",
    "entry_bridge.cpp",
    "multiboot_boot.asm", "multiboot_entry.cpp",
    "multiboot_linker.ld", "kurono_linker.ld",
    "logo.h"
)
foreach ($f in $oldSources) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_sources/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  8. Move old Python files → archive/old_python
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old Python files to archive/old_python..."
$oldPython = @(
    "kurono_filesystem.py", "kurono_kernel.py",
    "kurono_linux_bridge.py", "kurono_os_sim.py", "kurono_theme.py"
)
foreach ($f in $oldPython) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_python/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  9. Move old boot artifacts → archive/old_boot
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old boot artifacts to archive/old_boot..."
$oldBootItems = @(
    "BootArtifacts", "BootDir", "KuronoUI", "linux_rootfs",
    "grubx64.efi", "grub-mem.cfg",
    "edk2-i386-vars.fd", "edk2-x86_64-code.fd", "OVMF_CODE.fd"
)
foreach ($f in $oldBootItems) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_boot/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  10. Move old docs → archive/old_docs
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old documentation to archive/old_docs..."
$oldDocs = @(
    "BOOT_FIX_COMPLETE.md", "BOOT_READY.md", "BOOT_SYSTEM_SUMMARY.md",
    "BOOT_TEST_GUIDE.md", "ENHANCED_BUILD_COMPLETE.md",
    "ENHANCED_KERNEL_TEST_VERIFICATION.md", "PROJECT_COMPLETION.md"
)
foreach ($f in $oldDocs) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_docs/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  11. Move old configs → archive/old_configs
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving old config files to archive/old_configs..."
$oldConfigs = @(
    "CMakeLists.txt", "Makefile"
)
foreach ($f in $oldConfigs) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_configs/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  12. Move misc files → archive/old_misc
# ──────────────────────────────────────────────────────────────────────────
Write-Status "Moving miscellaneous files to archive/old_misc..."
$oldMisc = @(
    "hello.txt", "sample.kcl", "googlecc0128eec031c508.html",
    "sim_commands.txt", "sim_conflicts.txt", "sim_packages.txt", "sim_supr.txt",
    "denji.mp4"
)
foreach ($f in $oldMisc) {
    $path = Join-Path $Root $f
    if (Test-Path $path) {
        Move-Item $path (Join-Path $Root "archive/old_misc/$f") -Force
        Write-Warn "  Moved: $f"
    }
}

# Also move stray media from src/
$srcMedia = Join-Path $Root "src/denji.mp4"
if (Test-Path $srcMedia) {
    Move-Item $srcMedia (Join-Path $Root "archive/old_misc/denji_src.mp4") -Force
    Write-Warn "  Moved: src/denji.mp4"
}

# ──────────────────────────────────────────────────────────────────────────
#  13. Remove empty archive subdirectories
# ──────────────────────────────────────────────────────────────────────────
Get-ChildItem (Join-Path $Root "archive") -Directory | ForEach-Object {
    if ((Get-ChildItem $_.FullName -Force | Measure-Object).Count -eq 0) {
        Remove-Item $_.FullName -Force
    }
}

# ──────────────────────────────────────────────────────────────────────────
#  Done  -  Show final structure
# ──────────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Ok "Cleanup complete! Final directory structure:"
Write-Host ""

$items = Get-ChildItem $Root -Force | Where-Object { $_.Name -ne ".trae" }
foreach ($item in $items) {
    $icon = if ($item.PSIsContainer) { "[DIR]" } else { "[   ]" }
    $size = if ($item.PSIsContainer) {
        $childCount = (Get-ChildItem $item.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
        "$childCount files"
    } else {
        "{0:N0} KB" -f ($item.Length / 1KB)
    }
    Write-Host ("  {0}  {1,-30} {2}" -f $icon, $item.Name, $size) -ForegroundColor $(if ($item.PSIsContainer) { "Cyan" } else { "White" })
}

Write-Host ""
Write-Host "  ======================================" -ForegroundColor Green
Write-Host "  Root now contains only:" -ForegroundColor Green
Write-Host "    src/       - Source code (the real OS)" -ForegroundColor White
Write-Host "    build/     - Build output (kurono.elf)" -ForegroundColor White
Write-Host "    archive/   - Old files (safe to delete)" -ForegroundColor White
Write-Host "    tools/     - Utility scripts" -ForegroundColor White
Write-Host "    start.ps1  - Launch the OS" -ForegroundColor White
Write-Host "    README.md  - Project documentation" -ForegroundColor White
Write-Host "    LICENSE    - License file" -ForegroundColor White
Write-Host "  ======================================" -ForegroundColor Green

Pop-Location
