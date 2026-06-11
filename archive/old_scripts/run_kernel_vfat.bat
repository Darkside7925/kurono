@echo off
set BOOT_DIR=D:\Kurono\Kurnon OS\BootArtifacts
set OVMF=%BOOT_DIR%\OVMF_CODE.fd

if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
    set QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
) else (
    set QEMU=qemu-system-x86_64
)

echo Starting QEMU (UEFI VFAT Boot)...
%QEMU% -bios "%OVMF%" -drive file=fat:rw:"%BOOT_DIR%",format=raw -net none -m 1024 -vga std -serial stdio
