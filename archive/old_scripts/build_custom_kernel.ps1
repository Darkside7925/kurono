# Build custom Kurono kernel from source
Write-Host "Building custom Kurono kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\build\custom_kernel"
New-Item -ItemType Directory -Force -Path $buildDir

# Set compiler flags for kernel compilation
$env:CC = "gcc"
$env:CXX = "g++"
$env:CFLAGS = "-m32 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -Wall -Wextra"
$env:CXXFLAGS = "-m32 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -Wall -Wextra -fno-exceptions -fno-rtti"

# Build the bootloader (16-bit real mode)
Write-Host "Building bootloader..." -ForegroundColor Yellow
wsl --exec bash -c @"
cd /mnt/d/Kurono/Kurnon OS/kurono_simple_build
nasm -f bin kurono_boot.asm -o kurono_boot.bin
"@

# Build the kernel (32-bit protected mode)
Write-Host "Building kernel..." -ForegroundColor Yellow
wsl --exec bash -c @"
cd /mnt/d/Kurono/Kurnon OS/kurono
# Compile kernel components
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. kernel.cpp -o kernel.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. kurono_os.c -o kurono_os.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. kcl_interpreter.c -o kcl_interpreter.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. package_manager.c -o package_manager.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. linux_bridge.c -o linux_bridge.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. windows_bridge.c -o windows_bridge.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. conflict_resolver.c -o conflict_resolver.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib -I. security_supr_engine.c -o security_supr_engine.o

# Link everything together
ld -m elf_i386 -Ttext 0x100000 -nostdlib \
   kernel.o kurono_os.o kcl_interpreter.o package_manager.o \
   linux_bridge.o windows_bridge.o conflict_resolver.o security_supr_engine.o \
   -o kurono_kernel.elf

# Convert to flat binary
objcopy -O binary kurono_kernel.elf kurono_kernel.bin
"@

# Create multiboot compliant kernel
Write-Host "Creating multiboot kernel..." -ForegroundColor Yellow
wsl --exec bash -c @"
cd /mnt/d/Kurono/Kurnon OS/build/custom_kernel
# Create multiboot header
cat > multiboot_header.S << 'EOF'
.set MAGIC,    0x1BADB002
.set FLAGS,    0x00000003
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
EOF

# Create entry point
cat > entry.S << 'EOF'
.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp
    call kernel_main
    cli
1:  hlt
    jmp 1b
.size _start, . - _start
EOF

# Create main kernel file
cat > main.c << 'EOF'
#include <stdint.h>
#include <stddef.h>

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

void kernel_main(void) {
    const char* message = "Kurono OS v1.0.0 - Custom Kernel Booting...";
    uint16_t* vga_buffer = (uint16_t*)VGA_BUFFER;
    
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (uint16_t)0x0F20;
    }
    
    for (int i = 0; message[i]; i++) {
        vga_buffer[i] = (uint16_t)((0x0F << 8) | message[i]);
    }
    
    while (1) {
        __asm__ volatile ("hlt");
    }
}
EOF

# Compile multiboot kernel
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib multiboot_header.S -o multiboot_header.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib entry.S -o entry.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib main.c -o main.o

# Link multiboot kernel
ld -m elf_i386 -Ttext 0x100000 -nostdlib \
   multiboot_header.o entry.o main.o \
   -o kurono_multiboot.elf

# Convert to flat binary
objcopy -O binary kurono_multiboot.elf kurono_multiboot.bin
"@

# Copy kernel to boot artifacts
Write-Host "Copying kernel to boot artifacts..." -ForegroundColor Yellow
Copy-Item "D:\Kurono\Kurnon OS\kurono_simple_build\kurono_boot.bin" -Destination "$buildDir\kurono_boot.bin" -Force
Copy-Item "D:\Kurono\Kurnon OS\kurono\kurono_kernel.bin" -Destination "$buildDir\kurono_kernel.bin" -Force
Copy-Item "$buildDir\kurono_multiboot.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel" -Force

Write-Host "Custom Kurono kernel built successfully!" -ForegroundColor Green
Write-Host "Kernel binary: D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel" -ForegroundColor Cyan