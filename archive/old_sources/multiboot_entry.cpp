/**
 * C-based multiboot entry point for Enhanced Kurono OS 180Hz Kernel  
 * Provides reliable boot sequence without assembly symbol issues
 */

// Multiboot header structure
struct multiboot_header {
    unsigned int magic;
    unsigned int flags;  
    unsigned int checksum;
    unsigned int header_addr;
    unsigned int load_addr;
    unsigned int load_end_addr;
    unsigned int bss_end_addr;
    unsigned int entry_addr;
    unsigned int mode_type;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
} __attribute__((packed));

// Place multiboot header in special section  
__attribute__((section(".multiboot")))
struct multiboot_header mb_header = {
    .magic = 0x1BADB002,
    .flags = 0x00010003,  // Page-aligned + memory info + video mode
    .checksum = 0xE4514FFB,  // -(0x1BADB002 + 0x00010003) as unsigned
    .header_addr = 0,
    .load_addr = 0,
    .load_end_addr = 0, 
    .bss_end_addr = 0,
    .entry_addr = 0,
    .mode_type = 0,  // Linear graphics
    .width = 1024,
    .height = 768,
    .depth = 32
};

// Forward declaration of enhanced kernel
extern "C" void kernel_main(unsigned int magic, unsigned int mb_addr);

// Entry point that multiboot will call
extern "C" void _start(unsigned int magic, unsigned int mb_addr) {
    // Call the enhanced 180Hz kernel
    kernel_main(magic, mb_addr);
    
    // If kernel returns, halt
    while(1) {
        asm volatile("hlt");
    }
}