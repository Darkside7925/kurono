#!/bin/bash
# Create bootable ISO for Kurono OS

# Setup directories
mkdir -p isodir/boot/grub

# Copy kernel
cp build_cpp_kernel/kurono_kernel.elf isodir/boot/

# Create GRUB config
cat > isodir/boot/grub/grub.cfg << EOF
set timeout=0
set default=0

menuentry "Kurono OS" {
    multiboot /boot/kurono_kernel.elf
    boot
}
EOF

# Generate ISO
grub-mkrescue -o kurono.iso isodi

# Cleanup (optional)
# rm -rf isodi
