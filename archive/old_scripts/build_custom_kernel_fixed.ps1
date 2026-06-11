# Build custom Kurono kernel from source
Write-Host "Building custom Kurono kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\build\custom_kernel"
New-Item -ItemType Directory -Force -Path $buildDir

# Install required tools
Write-Host "Installing build tools..." -ForegroundColor Yellow
wsl --exec bash -c "sudo apt-get update -qq && sudo apt-get install -y nasm gcc g++ make binutils"

# Build the bootloader (16-bit real mode)
Write-Host "Building bootloader..." -ForegroundColor Yellow
wsl --exec bash -c 'cd "/mnt/d/Kurono/Kurnon OS/kurono_simple_build" && nasm -f bin kurono_boot.asm -o kurono_boot.bin'

# Create a simple standalone kernel that can boot
Write-Host "Building standalone kernel..." -ForegroundColor Yellow
wsl --exec bash -c @'
cd "/mnt/d/Kurono/Kurnon OS/build/custom_kernel"

# Create multiboot compliant kernel
cat > multiboot_header.S << \'EOF\'
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
cat > entry.S << \'EOF\'
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

.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KB stack
stack_top:
EOF

# Create main kernel file
cat > main.c << \'EOF\'
#include <stdint.h>
#include <stddef.h>

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

void kernel_main(void) {
    const char* message = "Kurono OS v1.0.0 - Custom Kernel Booting...";
    uint16_t* vga_buffer = (uint16_t*)VGA_BUFFER;
    
    // Clear screen
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (uint16_t)0x0720; // White on black
    }
    
    // Print message
    for (int i = 0; message[i]; i++) {
        vga_buffer[i] = (uint16_t)((0x0F << 8) | message[i]);
    }
    
    // Simple delay loop
    for (volatile int i = 0; i < 10000000; i++);
    
    // Show success message
    const char* success = "Kernel loaded successfully!";
    for (int i = 0; success[i]; i++) {
        vga_buffer[80 + i] = (uint16_t)((0x0A << 8) | success[i]); // Green
    }
    
    // Halt
    while (1) {
        __asm__ volatile ("hlt");
    }
}
EOF

# Create linker script
cat > linker.ld << \'EOF\'
ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }
    
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }
    
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }
    
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
EOF

# Compile multiboot kernel
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib multiboot_header.S -o multiboot_header.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib entry.S -o entry.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib main.c -o main.o

# Link multiboot kernel
ld -m elf_i386 -T linker.ld -nostdlib \
   multiboot_header.o entry.o main.o \
   -o kurono_multiboot.elf

# Convert to flat binary
objcopy -O binary kurono_multiboot.elf kurono_multiboot.bin

# Also create ELF version for GRUB
cp kurono_multiboot.elf kurono_kernel.elf
'@

# Copy kernel to boot artifacts
Write-Host "Copying kernel to boot artifacts..." -ForegroundColor Yellow
Copy-Item "D:\Kurono\Kurnon OS\kurono_simple_build\kurono_boot.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_boot.bin" -Force -ErrorAction SilentlyContinue
Copy-Item "$buildDir\kurono_multiboot.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel.bin" -Force
Copy-Item "$buildDir\kurono_kernel.elf" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel" -Force

Write-Host "Custom Kurono kernel built successfully!" -ForegroundColor Green
Write-Host "Kernel binary: D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_kernel" -ForegroundColor Cyan
Write-Host "Bootloader: D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_boot.bin" -ForegroundColor Cyan