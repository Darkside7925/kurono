@echo off
cd /d C:\Users\genie\OS
"C:\Program Files\qemu\qemu-system-x86_64.exe" ^
  -accel whpx,kernel-irqchip=off ^
  -cpu qemu64 -smp 4 -m 4G ^
  -cdrom .\build\kurono.iso -boot d ^
  -vga std ^
  -device sb16 ^
  -device e1000,netdev=net0 -netdev user,id=net0 ^
  -display none ^
  -serial stdio ^
  -no-reboot -no-shutdown ^
  > .\qemu_boot_test.log 2>&1
