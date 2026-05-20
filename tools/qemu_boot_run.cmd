@echo off
cd /d C:\Users\genie\OS
"C:\Program Files\qemu\qemu-system-x86_64.exe" ^
  -accel tcg ^
  -cpu qemu64 -m 4G -smp 4 ^
  -cdrom .\build\kurono.iso -boot d ^
  -display none ^
  -serial stdio ^
  -no-reboot -no-shutdown ^
  > .\qemu_boot_test.log 2>&1
