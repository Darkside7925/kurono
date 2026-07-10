// bga.h - bochs graphics adapter driver for qemu (-vga std)
// provides a linear framebuffer without needing bios vbe calls
#pragma once
#include "../kernel/types.h"

// bga port addresses
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

// bga register indices
#define VBE_DISPI_INDEX_ID           0
#define VBE_DISPI_INDEX_XRES         1
#define VBE_DISPI_INDEX_YRES         2
#define VBE_DISPI_INDEX_BPP          3
#define VBE_DISPI_INDEX_ENABLE       4
#define VBE_DISPI_INDEX_BANK         5
#define VBE_DISPI_INDEX_VIRT_WIDTH   6
#define VBE_DISPI_INDEX_VIRT_HEIGHT  7
#define VBE_DISPI_INDEX_X_OFFSET     8
#define VBE_DISPI_INDEX_Y_OFFSET     9

// enable flags
#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40   // linear framebuffer mode
#define VBE_DISPI_NOCLEARMEM   0x80

// bga framebuffer physical address (qemu maps it here with -vga std)
#define BGA_FRAMEBUFFER_ADDR   0xE0000000u

// valid bga ids
#define VBE_DISPI_ID0  0xB0C0
#define VBE_DISPI_ID1  0xB0C1
#define VBE_DISPI_ID2  0xB0C2
#define VBE_DISPI_ID3  0xB0C3
#define VBE_DISPI_ID4  0xB0C4
#define VBE_DISPI_ID5  0xB0C5

class BGA {
public:
    static bool available;
    static uint32_t width;
    static uint32_t height;
    static uint32_t bpp;
    static uint32_t pitch;
    static uint8_t* framebuffer;

    // detect and initialize bga at given resolution
    static bool Init(uint32_t w, uint32_t h, uint32_t b);

    // check if bga is present
    static bool IsAvailable();

    // set display mode (ensures pci bar is configured)
    static bool SetMode(uint32_t w, uint32_t h, uint32_t b);

    // get framebuffer pointer
    static uint8_t* GetFramebuffer() { return framebuffer; }

    // low-level register access
    static void WriteReg(uint16_t index, uint16_t value);
    static uint16_t ReadReg(uint16_t index);

private:
    static bool bar_programmed;        // true once bar0 has been set to bga_framebuffer_addr
    static void EnsureBAR();           // program bar0 if needed
    static void outw(uint16_t port, uint16_t value);
    static uint16_t inw(uint16_t port);
};
