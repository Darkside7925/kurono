// bga.cpp - bochs graphics adapter implementation
// works with qemu -vga std, providing direct framebuffer access
#include "bga.h"
#include "serial.h"

// when booting via multiboot (-kernel), seabios does not enumerate pci.
// we must program the vga bar0 ourselves so the framebuffer is accessible.
static inline void pci_outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %w1" : : "a"(val), "Nd"(port));
}
static inline uint32_t pci_inl(uint16_t port) {
    uint32_t val;
    __asm__ __volatile__("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | ((uint32_t)(reg & 0xFC));
    pci_outl(0xCF8, addr);
    return pci_inl(0xCFC);
}

static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | ((uint32_t)(reg & 0xFC));
    pci_outl(0xCF8, addr);
    pci_outl(0xCFC, val);
}

// program bar0 for pci device at (bus, dev, func) and enable memory access
static uint32_t pci_program_bar0(uint8_t bus, uint8_t dev, uint8_t func, uint32_t desired_addr) {
    // write desired address to bar0 (offset 0x10)
    pci_cfg_write(bus, dev, func, 0x10, desired_addr);

    // read it back to confirm
    uint32_t bar0 = pci_cfg_read(bus, dev, func, 0x10);
    bar0 &= 0xFFFFFFF0u; // mask type bits

    // enable memory space + bus master in command register (offset 0x04)
    uint32_t cmd = pci_cfg_read(bus, dev, func, 0x04);
    cmd |= 0x03; // bit 0 = i/o space, bit 1 = memory space
    pci_cfg_write(bus, dev, func, 0x04, cmd);

    return bar0;
}

// static members
bool BGA::available = false;
bool BGA::bar_programmed = false;
uint32_t BGA::width = 0;
uint32_t BGA::height = 0;
uint32_t BGA::bpp = 0;
uint32_t BGA::pitch = 0;
uint8_t* BGA::framebuffer = (uint8_t*)0;

void BGA::outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint16_t BGA::inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void BGA::WriteReg(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

uint16_t BGA::ReadReg(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

bool BGA::IsAvailable() {
    // try each known bga id
    uint16_t id = ReadReg(VBE_DISPI_INDEX_ID);
    SerialLogger::Log("BGA: Probe ID = 0x");
    SerialLogger::LogHex(id);
    SerialLogger::Log("\r\n");
    return (id >= VBE_DISPI_ID0 && id <= VBE_DISPI_ID5);
}

// ensure pci bar0 is programmed to bga_framebuffer_addr
void BGA::EnsureBAR() {
    if (bar_programmed) return;
    // scan pci bus 0 for vendor 1234:1111 (bochs vga)
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read(0, dev, 0, 0x00);
        if (id == 0x11111234u) {
            uint32_t bar = pci_program_bar0(0, dev, 0, BGA_FRAMEBUFFER_ADDR);
            SerialLogger::Log("BGA: EnsureBAR - dev=");
            SerialLogger::LogDec(dev);
            SerialLogger::Log(" BAR0=0x");
            SerialLogger::LogHex(bar);
            SerialLogger::Log("\r\n");
            bar_programmed = true;
            return;
        }
    }
    SerialLogger::Log("BGA: EnsureBAR - PCI device not found, using default\r\n");
    bar_programmed = true; // don't retry
}

bool BGA::SetMode(uint32_t w, uint32_t h, uint32_t b) {
    if (w == 0 || h == 0 || (b != 8 && b != 16 && b != 24 && b != 32)) {
        SerialLogger::Log("BGA: SetMode rejected invalid args\r\n");
        return false;
    }

    EnsureBAR();

    // snapshot current mode so a failed switch can roll back rather than
    // leaving the device disabled (= black screen).
    uint16_t prev_enable = ReadReg(VBE_DISPI_INDEX_ENABLE);
    uint16_t prev_w      = ReadReg(VBE_DISPI_INDEX_XRES);
    uint16_t prev_h      = ReadReg(VBE_DISPI_INDEX_YRES);
    uint16_t prev_b      = ReadReg(VBE_DISPI_INDEX_BPP);
    uint16_t prev_vw     = ReadReg(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t prev_vh     = ReadReg(VBE_DISPI_INDEX_VIRT_HEIGHT);

    WriteReg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    WriteReg(VBE_DISPI_INDEX_XRES,        (uint16_t)w);
    WriteReg(VBE_DISPI_INDEX_YRES,        (uint16_t)h);
    WriteReg(VBE_DISPI_INDEX_BPP,         (uint16_t)b);
    WriteReg(VBE_DISPI_INDEX_VIRT_WIDTH,  (uint16_t)w);
    WriteReg(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);
    WriteReg(VBE_DISPI_INDEX_X_OFFSET,    0);
    WriteReg(VBE_DISPI_INDEX_Y_OFFSET,    0);

    WriteReg(VBE_DISPI_INDEX_ENABLE,
             (uint16_t)(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));

    uint16_t xr = ReadReg(VBE_DISPI_INDEX_XRES);
    uint16_t yr = ReadReg(VBE_DISPI_INDEX_YRES);
    uint16_t br = ReadReg(VBE_DISPI_INDEX_BPP);

    SerialLogger::Log("BGA: Mode verify: ");
    SerialLogger::LogDec(xr);
    SerialLogger::Log("x");
    SerialLogger::LogDec(yr);
    SerialLogger::Log("@");
    SerialLogger::LogDec(br);
    SerialLogger::Log("\r\n");

    if (xr != (uint16_t)w || yr != (uint16_t)h || br != (uint16_t)b) {
        SerialLogger::Log("BGA: Mode set FAILED - restoring previous mode\r\n");
        // restore previous mode
        WriteReg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        WriteReg(VBE_DISPI_INDEX_XRES,        prev_w);
        WriteReg(VBE_DISPI_INDEX_YRES,        prev_h);
        WriteReg(VBE_DISPI_INDEX_BPP,         prev_b);
        WriteReg(VBE_DISPI_INDEX_VIRT_WIDTH,  prev_vw);
        WriteReg(VBE_DISPI_INDEX_VIRT_HEIGHT, prev_vh);
        WriteReg(VBE_DISPI_INDEX_X_OFFSET,    0);
        WriteReg(VBE_DISPI_INDEX_Y_OFFSET,    0);
        WriteReg(VBE_DISPI_INDEX_ENABLE,      prev_enable);
        return false;
    }

    width       = w;
    height      = h;
    bpp         = b;
    pitch       = w * (b / 8);
    framebuffer = (uint8_t*)BGA_FRAMEBUFFER_ADDR;

    SerialLogger::Log("BGA: Framebuffer at 0xE0000000, pitch=");
    SerialLogger::LogDec((int)pitch);
    SerialLogger::Log("\r\n");
    return true;
}

bool BGA::Init(uint32_t w, uint32_t h, uint32_t b) {
    SerialLogger::Log("BGA: Detecting Bochs Graphics Adapter...\r\n");

    if (!IsAvailable()) {
        SerialLogger::Log("BGA: Not found (not a BGA device)\r\n");
        available = false;
        return false;
    }

    SerialLogger::Log("BGA: Detected! Programming PCI BAR...\r\n");

    // scan pci bus 0 for vendor 1234:1111 (bochs vga)
    bool pci_found = false;
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read(0, dev, 0, 0x00);
        if (id == 0x11111234u) {
            // found bga device - program bar0
            uint32_t bar = pci_program_bar0(0, dev, 0, BGA_FRAMEBUFFER_ADDR);
            SerialLogger::Log("BGA: PCI dev=");
            SerialLogger::LogDec(dev);
            SerialLogger::Log(" BAR0 programmed to 0x");
            SerialLogger::LogHex(bar);
            SerialLogger::Log("\r\n");
            pci_found = true;
            bar_programmed = true;
            break;
        }
    }
    if (!pci_found) {
        SerialLogger::Log("BGA: WARNING - PCI device 1234:1111 not found, using default BAR\r\n");
    }

    SerialLogger::Log("BGA: Setting mode...\r\n");

    if (!SetMode(w, h, b)) {
        available = false;
        return false;
    }

    available = true;
    SerialLogger::Log("BGA: Init complete.\r\n");
    return true;
}
