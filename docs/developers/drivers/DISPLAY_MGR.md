# Display Manager

`src/drivers/display_mgr.cpp` and `display_mgr.h` provide a multi-backend display manager that routes to BGA, VirtIO GPU, Intel, NVIDIA, or AMD based on GPU probe results.

## 1. What it does

The display manager abstracts away the differences between display backends. It provides a unified API for:

- Mode selection (10 predefined modes from 640x480 through 3840x2160)
- Runtime mode switching
- EDID reading
- DPI scaling
- Gamma and brightness control
- VSync control (off, on, adaptive)
- Double buffering

## 2. Backend selection

The display manager tries backends in this order:

1. **VirtIO GPU** - if detected via `VirtIOGPU::IsDetected()`, provides 2D resource management and scanout. This is the highest quality path in VMs.
2. **BGA** - Bochs Graphics Adapter fallback for QEMU/Bochs via `BGA::IsAvailable()`.
3. **Native** - Intel, NVIDIA, or AMD based on GPU probe primary GPU selection. The backend is classified from the detected GPU vendor ID.

## 3. Mode table

The display manager exposes 10 predefined modes:

| Mode | Resolution |
| --- | --- |
| 0 | 640x480 |
| 1 | 800x600 |
| 2 | 1024x768 |
| 3 | 1280x720 |
| 4 | 1280x1024 |
| 5 | 1366x768 |
| 6 | 1600x900 |
| 7 | 1920x1080 |
| 8 | 2560x1440 |
| 9 | 3840x2160 |

## 4. VirtIO GPU backend

When VirtIO GPU is selected, the display manager:

1. Calls `VirtIOGPU::GetDisplayInfo(0, &w, &h)` to get the native resolution (defaults to 1920x1080 if unavailable).
2. Allocates a framebuffer from the kernel heap.
3. Creates a 2D resource via `VirtIOGPU::CreateResource2D(w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM)`.
4. Attaches the backing store via `VirtIOGPU::AttachBacking()`.
5. Sets the scanout via `VirtIOGPU::SetScanout()`.

## 5. BGA backend

When BGA is selected, the display manager:

1. Reads BGA width, height, and BPP from the BGA driver.
2. Calculates pitch as `width * (bpp / 8)`.
3. Gets the framebuffer address from `BGA::GetFramebuffer()`.
4. Matches the resolution to the mode table.

## 6. Native backend

When a native GPU is selected (Intel, NVIDIA, AMD), the display manager uses the native graphics framebuffer provided by the kernel. The backend is classified from the GPU probe results.

## 7. Double buffering

The display manager can enable double buffering via `EnableDoubleBuffering()`. When enabled:

- A back buffer is allocated from the kernel heap.
- `SwapBuffers()` uses SSE2 non-temporal stores to blast the back buffer to the framebuffer.
- For VirtIO GPU, `VirtIOGPU::PresentFramebuffer()` is called after the swap.

## 8. Common problems

| Problem | Likely cause |
| --- | --- |
| No display backend selected | GPU probe failed or no compatible backend found |
| VirtIO GPU mode switch fails | Resource allocation failed or scanout set failed |
| BGA mode switch fails | BAR0 not programmed or I/O port write failed |
| Double buffering not working | Back buffer allocation failed or WC remap issue |

## 9. Related files

- `src/drivers/gpu_probe.cpp` - GPU detection for backend classification
- `src/drivers/graphics.cpp` - framebuffer operations and swap path
- `src/drivers/bga.cpp` - BGA driver
- `src/drivers/virtio_gpu.cpp` - VirtIO GPU driver
- `src/drivers/intel_gpu.cpp` - Intel GPU driver
- `src/drivers/nvidia_gpu.cpp` - NVIDIA GPU driver
- `src/drivers/amd_gpu.cpp` - AMD GPU driver
