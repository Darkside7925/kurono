# Display Setup

`src/drivers/display.cpp` and `display.h` handle display mode selection and framebuffer acquisition.

## 1. Role

The display driver is responsible for finding a usable framebuffer and handing it to the graphics backend. It does not draw anything itself. Its job is to figure out what hardware is present and get a pixel buffer the rest of the system can use.

## 2. Priority order

The driver tries framebuffer sources in priority order:

1. **GOP (Graphics Output Protocol)**  -  provided by the EFI bootloader. Preferred on modern hardware.
2. **VBE linear framebuffer**  -  provided by BIOS-era bootloaders. Works on most legacy hardware.
3. **BGA (Bochs Graphics Adapter)**  -  used in virtual machines (QEMU, VirtualBox).

Once a source is accepted the framebuffer address, dimensions, and pitch are handed to the graphics driver and the display driver's job is done until a mode change is requested.

## 3. Panic framebuffer update

After the display driver establishes a mode, it informs the panic subsystem of the framebuffer parameters. This keeps the panic crash renderer synchronized so that if the system crashes later, the crash screen draws to the correct location.

## 4. Mode switching

`display_mgr.cpp` extends the display driver with runtime mode switching. If the OS needs to change resolution after boot (for example, from a settings change), the display manager handles the re-initialization and notifies the rest of the system.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| Screen stays black | No framebuffer source found; check bootloader config |
| Correct boot but wrong resolution | VBE mode selection picking wrong index |
| Works in VM but not real hardware | BGA only; GPU probe and VBE not set up |
| Resolution changes crash | Panic framebuffer not updated after mode switch |

## 6. Related files

- `src/drivers/display_mgr.cpp`  -  runtime mode switching
- `src/drivers/graphics.cpp`  -  receives framebuffer parameters
- `src/drivers/bga.cpp`  -  BGA path for virtual machines
- `src/kernel/panic.cpp`  -  framebuffer update target
