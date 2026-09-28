# Display manager

`src/drivers/display_mgr.cpp` and `display_mgr.h`. Routes display work to BGA, VirtIO
GPU, Intel, NVIDIA or AMD based on what the GPU probe found.

One API for mode selection, runtime mode switching, EDID reading, DPI scaling, gamma and
brightness, VSync (off, on, adaptive) and double buffering.

## Backend order

1. VirtIO GPU, if `VirtIOGPU::IsDetected()`. 2D resources and scanout, best quality in a
   VM.
2. BGA, if `BGA::IsAvailable()`.
3. Native: Intel, NVIDIA or AMD, whichever the GPU probe called primary. Classified from
   the vendor ID.

## Modes

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

## VirtIO GPU path

1. `VirtIOGPU::GetDisplayInfo(0, &w, &h)` for the native resolution, 1920x1080 if that
   fails
2. allocate a framebuffer from the kernel heap
3. `VirtIOGPU::CreateResource2D(w, h, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM)`
4. `VirtIOGPU::AttachBacking()`
5. `VirtIOGPU::SetScanout()`

## BGA path

1. read width, height and BPP from the BGA driver
2. pitch is `width * (bpp / 8)`
3. framebuffer address from `BGA::GetFramebuffer()`
4. match the resolution against the mode table

## Native path

Uses the native graphics framebuffer the kernel already has, classified from the GPU
probe results.

## Double buffering

`EnableDoubleBuffering()` allocates a back buffer from the kernel heap. `SwapBuffers()`
blasts it to the framebuffer with SSE2 non-temporal stores, and on VirtIO GPU it calls
`VirtIOGPU::PresentFramebuffer()` afterwards.

## When it breaks

| Symptom | Cause |
| --- | --- |
| no backend selected | GPU probe failed, or nothing compatible was found |
| VirtIO GPU mode switch fails | resource allocation or scanout set failed |
| BGA mode switch fails | BAR0 not programmed, or the port write failed |
| double buffering does nothing | back buffer allocation failed, or the WC remap didn't take |

## Files

- `src/drivers/gpu_probe.cpp` detection and backend classification
- `src/drivers/graphics.cpp` framebuffer operations and the swap path
- `src/drivers/bga.cpp`
- `src/drivers/virtio_gpu.cpp`
- `src/drivers/intel_gpu.cpp`
- `src/drivers/nvidia_gpu.cpp`
- `src/drivers/amd_gpu.cpp`
