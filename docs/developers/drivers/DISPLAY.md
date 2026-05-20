# Display Setup

`src/drivers/display.cpp` and `display.h` handle display mode selection and framebuffer acquisition. `src/drivers/display_mgr.cpp` provides the multi-backend display manager.

## 1. Role

The display driver is responsible for finding a usable framebuffer and handing it to the graphics backend. It does not draw anything itself. Its job is to figure out what hardware is present and get a pixel buffer the rest of the system can use.

The display manager (`display_mgr.cpp`) provides a multi-backend abstraction that routes to BGA, VirtIO GPU, Intel, NVIDIA, or AMD based on GPU probe results. It exposes runtime mode switching, EDID reading, DPI scaling, gamma/brightness, and VSync control.

## 2. Priority order

The driver tries framebuffer sources in priority order:

1. **Multiboot framebuffer**  -  provided by GRUB via EFI GOP on real hardware. The GPU probe validates the address on hybrid GPU laptops.
2. **VBE linear framebuffer**  -  provided by BIOS-era bootloaders. Works on most legacy hardware.
3. **BGA (Bochs Graphics Adapter)**  -  used in virtual machines (QEMU, VirtualBox).

Once a source is accepted, the display manager selects the appropriate backend based on the GPU probe results and hands the framebuffer address, dimensions, and pitch to the graphics driver.

## 3. Backend selection

The display manager tries backends in this order:

1. **VirtIO GPU**  -  if detected, provides 2D resource management and scanout (highest quality in VMs)
2. **BGA**  -  Bochs Graphics Adapter fallback for QEMU/Bochs
3. **Native**  -  Intel, NVIDIA, or AMD based on GPU probe primary GPU selection

## 4. Panic framebuffer update

After the display driver establishes a mode, it informs the panic subsystem of the framebuffer parameters. This keeps the panic crash renderer synchronized so that if the system crashes later, the crash screen draws to the correct location.

## 5. Mode switching

The display manager provides 10 predefined modes from 640x480 through 3840x2160. Runtime mode switching is supported via `SetResolution()`. When the OS needs to change resolution after boot (for example, from a settings change), the display manager handles the re-initialization and notifies the rest of the system.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Screen stays black | No framebuffer source found; check bootloader config or GPU probe |
| Correct boot but wrong resolution | VBE mode selection picking wrong index or EDID not read |
| Works in VM but not real hardware | BGA only; GPU probe and VBE not set up |
| Black screen on Optimus laptop | GRUB framebuffer points to wrong GPU; GPU probe validation failed |
| Resolution changes crash | Panic framebuffer not updated after mode switch |

## 7. Related files

- `src/drivers/display_mgr.cpp`  -  multi-backend display manager
- `src/drivers/gpu_probe.cpp`  -  GPU detection and hybrid topology
- `src/drivers/graphics.cpp`  -  receives framebuffer parameters
- `src/drivers/bga.cpp`  -  BGA path for virtual machines
- `src/drivers/virtio_gpu.cpp`  -  VirtIO GPU path
- `src/kernel/panic.cpp`  -  framebuffer update target
