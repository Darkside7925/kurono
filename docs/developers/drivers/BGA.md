# BGA

`src/drivers/bga.cpp` and `bga.h`. The Bochs Graphics Adapter, which is what QEMU and
Bochs hand you as a default display device.

BGA is programmed over I/O ports rather than MMIO: set resolution, bit depth and virtual
size, then read the linear framebuffer address out of PCI BAR0.

## Detection

PCI vendor 0x1234 (QEMU/Bochs), device 0x1111.

## Ports

- index port 0x01CE, write a register index
- data port 0x01CF, read or write that register

| Index | Register | Meaning |
| --- | --- | --- |
| 0 | ID | identification, should read 0x1111 |
| 1 | XRES | horizontal resolution |
| 2 | YRES | vertical resolution |
| 3 | BPP | bits per pixel, 16, 24 or 32 |
| 4 | ENABLE | enable bit 0, LFB enable bit 6 |
| 5 | BANK | virtual bank, unused in LFB mode |
| 6 | VIRT_WIDTH | virtual width, for scrolling |
| 7 | VIRT_HEIGHT | virtual height, for scrolling |
| 8 | X_OFFSET | x offset into the virtual buffer |
| 9 | Y_OFFSET | y offset into the virtual buffer |

## Setting a mode

1. write XRES, YRES, BPP to registers 1, 2, 3
2. optionally virtual width/height to 6, 7 and offsets to 8, 9
3. write `ENABLE | 0x40` to register 4 to turn on LFB mode
4. read BAR0 from PCI config space for the framebuffer address

On some QEMU setups BAR0 comes up unconfigured. The driver can program it to a fixed
physical address (0xE0000000) through PCI config writes.

## Pixel formats

16bpp RGB565, 24bpp RGB888, 32bpp BGRA8888. The driver prefers 32bpp to match the
graphics layer.

## When it breaks

| Symptom | Cause |
| --- | --- |
| black screen after mode set | LFB enable bit not set, or BAR0 never programmed |
| wrong colours | BPP mismatch between driver and device |
| garbled output | pitch wrong, or virtual width/height misconfigured |
| device not detected | wrong vendor/device ID, or the VM has no BGA |

## QEMU

`-vga std` gives you BGA.

## Files

- `src/drivers/display_mgr.cpp` uses BGA as a backend
- `src/drivers/graphics.cpp` framebuffer operations
- `src/kernel/pci.h` PCI config helpers
