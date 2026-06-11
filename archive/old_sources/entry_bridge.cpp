/**
 * Simple C entry point for enhanced Kurono OS kernel
 * Acts as bridge between multiboot and C++ kernel
 */

extern "C" void kernel_main(unsigned int magic, unsigned int mb_addr);

extern "C" void _start(unsigned int magic, unsigned int mb_addr) {
    // Call the actual kernel main function
    kernel_main(magic, mb_addr);
    
    // If kernel returns, halt
    while(1) {
        asm volatile("hlt");
    }
}