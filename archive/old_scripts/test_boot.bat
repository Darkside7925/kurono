@echo off
echo === Kurono OS Boot Test ===
echo.
echo Testing Kurono OS boot components...
echo.

REM Check if QEMU is available
where qemu-system-x86_64.exe >nul 2>&1
if %errorlevel% equ 0 (
    echo QEMU found in PATH
    set QEMU=qemu-system-x86_64.exe
) else if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
    echo Found QEMU in Program Files
    set QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
) else if exist "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe" (
    echo Found QEMU in Program Files (x86)
    set QEMU="C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
) else (
    echo QEMU not found!
    echo Please install QEMU or add it to PATH
    echo Download from: https://www.qemu.org/download/
    pause
    exit /b 1
)

echo.
echo Using QEMU: %QEMU%
echo.

REM Check for boot components
set KERNEL=D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\vmlinuz
set INITRD=D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\initramfs.cpio.gz
set OVMF=D:\Kurono\Kurnon OS\BootArtifacts\OVMF_CODE.fd

if exist "%KERNEL%" (
    echo ✓ Kernel found: %KERNEL%
) else (
    echo ✗ Kernel not found: %KERNEL%
    pause
    exit /b 1
)

if exist "%INITRD%" (
    echo ✓ Initramfs found: %INITRD%
) else (
    echo ✗ Initramfs not found: %INITRD%
    pause
    exit /b 1
)

if exist "%OVMF%" (
    echo ✓ UEFI firmware found: %OVMF%
) else (
    echo ⚠ UEFI firmware not found: %OVMF%
    echo Will try BIOS boot instead
)

echo.
echo Starting Kurono OS boot test...
echo Press Ctrl+C to stop
echo.

REM Try UEFI boot first
if exist "%OVMF%" (
    echo Attempting UEFI boot...
    %QEMU% -enable-kvm -m 2048 -cpu qemu64 -bios "%OVMF%" -kernel "%KERNEL%" -initrd "%INITRD%" -append "console=ttyS0 rdinit=/init quiet splash" -serial stdio -nographic
) else (
    echo Attempting BIOS boot...
    %QEMU% -enable-kvm -m 2048 -cpu qemu64 -kernel "%KERNEL%" -initrd "%INITRD%" -append "console=ttyS0 rdinit=/init quiet splash" -serial stdio -nographic
)

echo.
echo Boot test completed!
echo.
pause