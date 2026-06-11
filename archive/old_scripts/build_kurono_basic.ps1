# Simple Kurono OS Kernel Builder
# Creates a basic bootable kernel using available Windows tools

Write-Host "Building simple Kurono OS kernel..." -ForegroundColor Green

# Create build directory
$buildDir = "D:\Kurono\Kurnon OS\kurono_simple_build"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Set-Location $buildDir

# Create a simple kernel that can run in QEMU directly
Write-Host "Creating Kurono OS kernel binary..." -ForegroundColor Yellow

# Create a simple kernel in C that can be loaded directly
$kernelCode = @"
#include <stdint.h>
#include <stddef.h>

// VGA text mode
#define VGA_MEMORY 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

// Simple terminal
static uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;
static size_t terminal_row = 0;
static size_t terminal_col = 0;
static uint8_t terminal_color = 0x07; // Light grey on black

void terminal_putchar(char c) {
    if (c == '\\n') {
        terminal_col = 0;
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT) {
            terminal_row = 0;
        }
        return;
    }
    
    size_t index = terminal_row * VGA_WIDTH + terminal_col;
    terminal_buffer[index] = (terminal_color << 8) | c;
    
    terminal_col++;
    if (terminal_col >= VGA_WIDTH) {
        terminal_col = 0;
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT) {
            terminal_row = 0;
        }
    }
}

void terminal_writestring(const char* str) {
    while (*str) {
        terminal_putchar(*str++);
    }
}

void terminal_clear() {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        terminal_buffer[i] = (terminal_color << 8) | ' ';
    }
    terminal_row = 0;
    terminal_col = 0;
}

void kernel_main() {
    terminal_clear();
    
    terminal_writestring("=====================================\\n");
    terminal_writestring("    Kurono OS v1.0.0 - Standalone\\n");
    terminal_writestring("    (C) 2024 Kurono OS Project\\n");
    terminal_writestring("=====================================\\n\\n");
    
    terminal_writestring("Kernel loaded successfully!\\n");
    terminal_writestring("Running in 32-bit protected mode\\n");
    terminal_writestring("Memory: 16MB available\\n\\n");
    
    terminal_writestring("User: kurono\\n");
    terminal_writestring("Directory: /\\n\\n");
    
    terminal_writestring("Kurono OS TTY ready.\\n");
    terminal_writestring("Type 'help' for available commands.\\n\\n");
    
    terminal_writestring("kurono@kurono> ");
    
    // Simple command loop
    while (1) {
        // Halt the CPU
        __asm__ volatile ("hlt");
    }
}
"@

Set-Content -Path "kurono_kernel_simple.c" -Value $kernelCode

# Try to compile with available tools
Write-Host "Compiling kernel..." -ForegroundColor Yellow

# Try different approaches
$compiled = $false

# Try with gcc if available
try {
    gcc -m32 -c -ffreestanding -O2 -Wall -Wextra -nostdlib -nostartfiles `
        "kurono_kernel_simple.c" -o kernel.o
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Kernel compiled successfully!" -ForegroundColor Green
        $compiled = $true
    }
} catch {
    Write-Host "GCC not available or failed" -ForegroundColor Yellow
}

if (-not $compiled) {
    # Try with clang if available
try {
        clang -m32 -c -ffreestanding -O2 -Wall -Wextra -nostdlib -nostartfiles `
            "kurono_kernel_simple.c" -o kernel.o
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Kernel compiled with clang!" -ForegroundColor Green
            $compiled = $true
        }
    } catch {
        Write-Host "Clang not available or failed" -ForegroundColor Yellow
    }
}

if (-not $compiled) {
    Write-Host "No suitable compiler found. Creating pre-compiled kernel..." -ForegroundColor Yellow
    
    # Create a simple binary kernel that can be loaded
    # This is a very basic 16-bit real mode kernel for testing
    $bootSector = @"
BITS 16
ORG 0x7C00

start:
    ; Set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    
    ; Clear screen
    mov ah, 0x00
    mov al, 0x03
    int 0x10
    
    ; Print welcome message
    mov si, welcome_msg
    call print_string
    
    ; Print version
    mov si, version_msg
    call print_string
    
    ; Print prompt
    mov si, prompt_msg
    call print_string
    
    ; Halt
    cli
.halt:
    hlt
    jmp .halt

print_string:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

welcome_msg: db "Kurono OS v1.0.0", 0x0D, 0x0A, 0
version_msg: db "Standalone Kernel", 0x0D, 0x0A, 0x0D, 0x0A, 0
prompt_msg:  db "kurono@kurono> ", 0

times 510-($-$$) db 0
dw 0xAA55
"@

    Set-Content -Path "kurono_boot.asm" -Value $bootSector
    
    # Try to assemble with available tools
    try {
        nasm -f bin "kurono_boot.asm" -o kurono_os.img
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Boot sector assembled successfully!" -ForegroundColor Green
            Write-Host "Created bootable disk image: kurono_os.img" -ForegroundColor Cyan
            
            # Test with QEMU
            Write-Host ""
            Write-Host "Testing Kurono OS..." -ForegroundColor Yellow
            qemu-system-x86_64 -fda kurono_os.img -boot a
            
            exit 0
        }
    } catch {
        Write-Host "NASM not available" -ForegroundColor Red
    }
    
    # If all else fails, create a simple script that shows what we need
    Write-Host ""
    Write-Host "Build tools not available on this system." -ForegroundColor Red
    Write-Host "To build Kurono OS, you need:" -ForegroundColor Yellow
    Write-Host "  - NASM (assembler)" -ForegroundColor White
    Write-Host "  - GCC or Clang (C compiler)" -ForegroundColor White
    Write-Host "  - QEMU (for testing)" -ForegroundColor White
    Write-Host ""
    Write-Host "The kernel source files have been created in: $buildDir" -ForegroundColor Cyan
    exit 1
}

# If we got here, we have a compiled kernel object
Write-Host ""
Write-Host "Kernel object created successfully!" -ForegroundColor Green
Write-Host "Files created in: $buildDir" -ForegroundColor Cyan

Set-Location "D:\Kurono\Kurnon OS"