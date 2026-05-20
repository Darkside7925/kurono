# GPU Probe

`src/drivers/gpu_probe.cpp` and `gpu_probe.h` scan the PCI bus for GPU devices, detect hybrid topologies, and validate framebuffer addresses.

## 1. What it does

GPU probing is the first step in display initialization on real hardware. The probe:

1. Iterates PCI devices looking for class code 0x03 (display controller).
2. For each found device, matches the vendor ID against known vendors.
3. Reads PCI BAR0 (MMIO registers) and BAR2 (VRAM aperture) for each GPU.
4. Classifies the system topology as single GPU, Optimus (muxless or MUX), PowerXpress, dual discrete, or virtual.
5. Assigns roles (primary, secondary, virtual) to each GPU based on topology.
6. On Intel iGPU systems, reads the DSPSURF (display surface) register to validate the active framebuffer address.
7. Delegates to the appropriate vendor driver: NVIDIA, AMD, or Intel.
8. Falls back to the BGA or VirtIO GPU path on virtual hardware.

## 2. Why a probe layer

Without a probe step, the kernel would need to try every vendor's initialization sequence blindly. The probe gives the system a chance to identify what is present before doing anything potentially destructive to the hardware.

On hybrid GPU laptops (Optimus, PowerXpress), the probe is critical because the GRUB-reported framebuffer address may point to the wrong GPU. The probe validates the address by reading the Intel DSPSURF register and corrects it if necessary.

## 3. Detection table

| Vendor ID | Vendor | Driver |
| --- | --- | --- |
| 0x10DE | NVIDIA | `nvidia_gpu.cpp` |
| 0x1002 | AMD/ATI | `amd_gpu.cpp` |
| 0x8086 | Intel | `intel_gpu.cpp` |
| 0x1234 | QEMU/Bochs | `bga.cpp` |
| 0x1AF4 | VirtIO | `virtio_gpu.cpp` |
| 0x15AD | VMware | `bga.cpp` |
| 0x1B36 | Red Hat QXL | `bga.cpp` |

## 4. Topology classification

The probe classifies the system into one of these topologies:

- **GPU_TOPO_SINGLE**: Single GPU (desktop or single-GPU laptop)
- **GPU_TOPO_OPTIMUS_MUXLESS**: Intel iGPU + NVIDIA dGPU, no mux switch (panel wired to iGPU only)
- **GPU_TOPO_OPTIMUS_MUX**: Intel iGPU + NVIDIA dGPU with MUX switch (either GPU can drive panel)
- **GPU_TOPO_POWERXPRESS**: AMD APU + AMD/NVIDIA discrete GPU
- **GPU_TOPO_DUAL_DISCRETE**: Two discrete GPUs (workstation)
- **GPU_TOPO_VIRTUAL**: Running under hypervisor with virtual GPU

On muxless Optimus systems, the NVIDIA dGPU is class 0x0302 (3D controller, not VGA) and cannot drive the display. The probe identifies the Intel iGPU as the primary display device.

## 5. Framebuffer validation

On systems with an Intel iGPU, the probe reads the DSPSURF register from BAR0 to determine the actual active framebuffer address. If this differs from the GRUB-reported address (which can happen on Optimus laptops), the kernel corrects it before initializing graphics.

The relevant Intel register offsets are:
- `INTEL_DSPSURF_A` (0x7019C) for pipe A
- `INTEL_DSPSURF_B` (0x7119C) for pipe B
- `INTEL_PLANE_SURF_A` (0x7019C) for Skylake+ universal plane

## 6. Limitations

The GPU probe can identify hardware but cannot perform a full modesetting (KMS-style) initialization. The focus is on getting a usable framebuffer rather than full GPU feature access. 3D acceleration is not implemented.

## 7. Related files

- `src/drivers/nvidia_gpu.cpp`  -  NVIDIA support
- `src/drivers/amd_gpu.cpp`  -  AMD support
- `src/drivers/intel_gpu.cpp`  -  Intel support
- `src/drivers/virtio_gpu.cpp`  -  VirtIO GPU path
- `src/drivers/bga.cpp`  -  BGA fallback for QEMU
- `src/drivers/display_mgr.cpp`  -  backend selection based on probe results
