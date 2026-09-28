# GPU probe

`src/drivers/gpu_probe.cpp` and `gpu_probe.h`. Scans PCI for GPUs, works out the hybrid
topology, validates the framebuffer address.

This is step one of display init on real hardware:

1. walk PCI looking for class code 0x03 (display controller)
2. match the vendor ID against known vendors
3. read BAR0 (MMIO registers) and BAR2 (VRAM aperture)
4. classify the topology: single, Optimus muxless, Optimus MUX, PowerXpress, dual
   discrete, or virtual
5. hand out roles (primary, secondary, virtual)
6. on Intel iGPU systems, read DSPSURF to validate the active framebuffer address
7. hand off to the vendor driver: NVIDIA, AMD or Intel
8. fall back to BGA or VirtIO GPU on virtual hardware

## Why a probe layer

Without it, the kernel would blindly try every vendor's init sequence. The probe works out
what's there before anything touches hardware.

On Optimus and PowerXpress laptops this matters, because the framebuffer address GRUB
reported can point at the wrong GPU. The probe reads Intel's DSPSURF register and fixes
it.

## Vendors

| Vendor ID | Vendor | Driver |
| --- | --- | --- |
| 0x10DE | NVIDIA | `nvidia_gpu.cpp` |
| 0x1002 | AMD/ATI | `amd_gpu.cpp` |
| 0x8086 | Intel | `intel_gpu.cpp` |
| 0x1234 | QEMU/Bochs | `bga.cpp` |
| 0x1AF4 | VirtIO | `virtio_gpu.cpp` |
| 0x15AD | VMware | `bga.cpp` |
| 0x1B36 | Red Hat QXL | `bga.cpp` |

## Topologies

- `GPU_TOPO_SINGLE`: one GPU
- `GPU_TOPO_OPTIMUS_MUXLESS`: Intel iGPU plus NVIDIA dGPU, no mux, the panel is wired to
  the iGPU only
- `GPU_TOPO_OPTIMUS_MUX`: same but with a mux, either GPU can drive the panel
- `GPU_TOPO_POWERXPRESS`: AMD APU plus an AMD or NVIDIA discrete GPU
- `GPU_TOPO_DUAL_DISCRETE`: two discrete GPUs
- `GPU_TOPO_VIRTUAL`: hypervisor with a virtual GPU

On muxless Optimus the NVIDIA part is class 0x0302 (3D controller, not VGA) and can't
drive the display, so the Intel iGPU is the primary display device.

## Framebuffer validation

On Intel systems the probe reads DSPSURF out of BAR0 for the real active framebuffer
address, and corrects the GRUB value if they disagree, which is the Optimus case.

Register offsets:
- `INTEL_DSPSURF_A` (0x7019C), pipe A
- `INTEL_DSPSURF_B` (0x7119C), pipe B
- `INTEL_PLANE_SURF_A` (0x7019C), Skylake and later universal plane

## Limits

It can identify hardware. It can't do full KMS-style modesetting. The goal is a usable
framebuffer, not full GPU access. No 3D.

## Files

- `src/drivers/nvidia_gpu.cpp`
- `src/drivers/amd_gpu.cpp`
- `src/drivers/intel_gpu.cpp`
- `src/drivers/virtio_gpu.cpp`
- `src/drivers/bga.cpp`
- `src/drivers/display_mgr.cpp` picks a backend from the probe results
