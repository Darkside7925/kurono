# BGA Driver

`src/drivers/bga.cpp` and `bga.h` implement the Bochs Graphics Adapter driver for QEMU and Bochs emulators.

## 1. What it does

BGA is a simple framebuffer device used by QEMU and Bochs as a standard graphics adapter. The driver programs the device via I/O ports to set resolution, bit depth, and virtual width/height, then reads the linear framebuffer address from PCI BAR0.

## 2. Device detection

BGA is detected by checking for PCI vendor ID 0x1234 (QEMU/Bochs). The device ID should be 0x1111 for the standard BGA device.

## 3. I/O port interface

BGA uses two I/O ports for configuration:

- **Index port (0x01CE)**  -  write a register index
- **Data port (0x01CF)**  -  read or write the register value

### Important registers

| Index | Register | Description |
| --- | --- | --- |
| 0 | ID | Device identification (should read 0x1111) |
| 1 | XRES | Horizontal resolution |
| 2 | YRES | Vertical resolution |
| 3 | BPP | Bits per pixel (16, 24, or 32) |
| 4 | ENABLE | Enable bit (bit 0) and LFB enable (bit 6) |
| 5 | BANK | Virtual bank (for banked modes, not used in LFB mode) |
| 6 | VIRT_WIDTH | Virtual width (for scrolling) |
| 7 | VIRT_HEIGHT | Virtual height (for scrolling) |
| 8 | X_OFFSET | X offset in virtual buffer |
| 9 | Y_OFFSET | Y offset in virtual buffer |

## 4. Mode setting

To set a mode:

1. Write XRES, YRES, BPP to registers 1, 2, 3.
2. Write virtual width/height to registers 6, 7 (optional).
3. Write offset to registers 8, 9 (optional).
4. Write `ENABLE | 0x40` to register 4 to enable LFB mode.
5. Read BAR0 from PCI configuration space to get the framebuffer address.

## 5. PCI BAR0 programming

On some QEMU configurations, BAR0 may not be pre-configured. The driver can program BAR0 to a fixed physical address (e.g., 0xE0000000) via PCI configuration writes.

## 6. Framebuffer format

BGA supports multiple pixel formats:

- **16 bpp**  -  RGB 565
- **24 bpp**  -  RGB 888
- **32 bpp**  -  BGRA 8888 (most common in Kurono)

The driver prefers 32 bpp for compatibility with the graphics layer.

## 7. Common problems

| Problem | Likely cause |
| --- | --- |
| Black screen after mode set | LFB enable bit not set or BAR0 not programmed |
| Wrong colors | BPP mismatch between driver and device |
| Garbled output | Pitch calculation wrong or virtual width/height misconfigured |
| Device not detected | Wrong PCI vendor/device ID or device not present in VM config |

## 8. QEMU configuration

To use BGA in QEMU, add `-vga std` to the command line. This enables the Bochs Graphics Adapter.

## 9. Related files

- `src/drivers/display_mgr.cpp`  -  uses BGA as a display backend
- `src/drivers/graphics.cpp`  -  framebuffer operations
- `src/kernel/pci.h`  -  PCI configuration helpers
