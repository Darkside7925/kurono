# GPU Probe

`src/drivers/gpu_probe.cpp` and `gpu_probe.h` scan the PCI bus for GPU devices and hand off to vendor-specific drivers.

## 1. What it does

GPU probing is the first step in display initialization on real hardware. The probe:

1. Iterates PCI devices looking for class code 0x03 (display controller).
2. For each found device, matches the vendor ID against known vendors.
3. Delegates to the appropriate vendor driver: NVIDIA, AMD, or Intel.
4. Falls back to the BGA path on virtual hardware.

## 2. Why a probe layer

Without a probe step, the kernel would need to try every vendor's initialization sequence blindly. The probe gives the system a chance to identify what is present before doing anything potentially destructive to the hardware.

## 3. Detection table

| Vendor ID | Vendor | Driver |
| --- | --- | --- |
| 0x10DE | NVIDIA | `nvidia_gpu.cpp` |
| 0x1002 | AMD/ATI | `amd_gpu.cpp` |
| 0x8086 | Intel | `intel_gpu.cpp` |
| Virtual | QEMU/VBox | `bga.cpp` / `virtio_gpu.cpp` |

## 4. Limitations

The GPU probe can identify hardware but cannot perform a full modesetting (KMS-style) initialization.  The focus is on getting a usable framebuffer rather than full GPU feature access.  3D acceleration is not implemented.

## 5. Related files

- `src/drivers/nvidia_gpu.cpp`  -  NVIDIA support
- `src/drivers/amd_gpu.cpp`  -  AMD support
- `src/drivers/intel_gpu.cpp`  -  Intel support
- `src/drivers/virtio_gpu.cpp`  -  VirtIO GPU path
- `src/drivers/bga.cpp`  -  BGA fallback for QEMU
