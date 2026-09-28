# VirtIO GPU

`src/drivers/virtio_gpu.cpp` and `virtio_gpu.h` are the guest driver.
`src/virt/virtio_gpu_host.cpp` and `virtio_gpu_host.h` are the host side, for the
hypervisor.

VirtIO GPU is the virtualisation standard for GPU passthrough. Guest and host talk over
virtqueues, which gets you 2D resources, scanout and cursor without full 3D.

## Guest driver

Provides:

- PCI detection (vendor 0x1AF4, device 0x1050)
- VirtIO modern device init, BAR4 MMIO region
- virtqueue setup for the control and cursor queues
- `CreateResource2D()`
- `AttachBacking()`
- `SetScanout()`
- `PresentFramebuffer()`
- `GetDisplayInfo()`

Wire format: each command goes over the control virtqueue as a descriptor chain, one
device-readable command descriptor then one device-writable response descriptor. The
resource format is `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM`, 32-bit BGRA with no alpha.

## Host emulation

A VPCI device that:

- registers with vendor 0x1AF4, device 0x1050
- exposes BAR4 as the VirtIO modern region (64KB, MMIO, 64-bit, prefetchable)
- implements common config, device config, ISR status and notify regions
- keeps up to `VIRTIO_GPU_MAX_RES` resources and `VIRTIO_GPU_MAX_SCANOUTS` scanouts
- processes commands off the guest control queue
- moves guest backing bytes into host pixels with `TransferToHost()`
- presents scanout 0 to the Kurono compositor with `PresentScanout0()`

Commands it handles: `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`,
`VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`, `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`,
`VIRTIO_GPU_CMD_SET_SCANOUT`, `VIRTIO_GPU_CMD_RESOURCE_FLUSH`,
`VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`, `VIRTIO_GPU_CMD_RESOURCE_UNREF`.

`PresentIfDirty()` is called from the hypervisor tick loop. If the scanout changed it
presents it to the Kurono compositor, which is the bridge from the virtual GPU to the
native desktop.

## Virtqueues

Descriptor table (`VIRTQ_SIZE` entries), available ring, used ring. Descriptors chain
with `VIRTQ_DESC_F_NEXT`.

## When it breaks

| Symptom | Cause |
| --- | --- |
| VirtIO GPU not detected | PCI scan missed it, or BAR4 isn't mapped |
| commands time out | virtqueue not notified, or the host isn't processing the queue |
| scanout blank | resource not attached, or scanout not set |
| pixels never appear | `PresentIfDirty()` isn't being called, or the transfer didn't run |

## Files

- `src/drivers/display_mgr.cpp` uses it as a backend
- `src/drivers/graphics.cpp` the scanout target
- `src/virt/hypervisor.cpp` tick loop calls `PresentIfDirty()`
- `src/virt/vpci.cpp` virtual PCI registration
