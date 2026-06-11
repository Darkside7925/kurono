/**
 * Simple bootable test kernel for enhanced 180Hz system
 * Tests multiboot detection and basic functionality
 */

// Multiboot magic numbers
#define MULTIBOOT_MAGIC 0x2BADB002

extern "C" void kernel_main(unsigned int magic, unsigned int mb_addr) {
    // Simple test - just halt if multiboot magic is correct
    if (magic == MULTIBOOT_MAGIC) {
        // Enhanced graphics system would initialize here
        // For now, just loop to show it's working
        while(1) {
            // Enhanced 180Hz rendering loop would be here
            __asm__ volatile("pause"); // Light CPU usage
        }
    }
    
    // If wrong magic, halt
    while(1) {
        __asm__ volatile("hlt");
    }
}