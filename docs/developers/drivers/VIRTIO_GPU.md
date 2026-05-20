# VirtIO GPU Driver

`src/drivers/virtio_gpu.cpp` and `virtio_gpu.h` implement the VirtIO GPU guest driver. `src/virt/virtio_gpu_host.cpp` and `virtio_gpu_host.h` implement the VirtIO GPU host emulation for the hypervisor.

## 1. What it does

VirtIO GPU is a virtualization standard for GPU passthrough. The guest driver communicates with the host device via virtqueues, enabling 2D resource management, scanout, and cursor operations without requiring full 3D acceleration.

## 2. Guest driver (`virtio_gpu.cpp`)

The guest driver provides:

- PCI device detection (vendor ID 0x1AF4, device ID 0x1050)
- VirtIO modern device initialization (BAR4 MMIO region)
- Virtqueue setup for control and cursor queues
- 2D resource creation via `CreateResource2D()`
- Backing store attachment via `AttachBacking()`
- Scanout configuration via `SetScanout()`
- Framebuffer presentation via `PresentFramebuffer()`
- Display info query via `GetDisplayInfo()`

### Wire format

The guest uses the VirtIO GPU wire protocol. Each command is sent via the control virtqueue as a descriptor chain:

1. Command descriptor (device-readable)
2. Response descriptor (device-writable)

### Resource format

The driver uses the `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` format for 32-bit color (BGRA, no alpha).

## 3. Host emulation (`virtio_gpu_host.cpp`)

The host emulation is a VPCI device that:

- Registers as a PCI device with vendor ID 0x1AF4 and device ID 0x1050
- Exposes BAR4 as the VirtIO modern region (64 KB, MMIO, 64-bit, prefetchable)
- Implements common config, device config, ISR status, and notify regions
- Maintains up to `VIRTIO_GPU_MAX_RES` resources and `VIRTIO_GPU_MAX_SCANOUTS` scanouts
- Processes commands from the guest control queue
- Transfers guest backing store bytes into host pixels via `TransferToHost()`
- Presents scanout 0 to the Kurono compositor via `PresentScanout0()`

### Command processing

The host processes these commands:

- `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`  -  returns display dimensions
- `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`  -  creates a 2D resource
- `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`  -  attaches guest memory to resource
- `VIRTIO_GPU_CMD_SET_SCANOUT`  -  sets a resource as the scanout
- `VIRTIO_GPU_CMD_RESOURCE_FLUSH`  -  flushes resource changes
- `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`  -  transfers data to host
- `VIRTIO_GPU_CMD_RESOURCE_UNREF`  -  destroys a resource

### PresentIfDirty

The hypervisor tick loop calls `VirtIOGPUHost::PresentIfDirty()` to check if the scanout has been modified and present it to the Kurono compositor. This bridges the virtual GPU to the native desktop.

## 4. Virtqueue setup

The guest driver initializes virtqueues with:

- Descriptor table (VIRTQ_SIZE entries)
- Available ring
- Used ring

Each descriptor can be chained via the `VIRTQ_DESC_F_NEXT` flag.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| VirtIO GPU not detected | PCI scan missed device or BAR4 not mapped |
| Commands timeout | Virtqueue not notified or host not processing queue |
| Scanout blank | Resource not attached or scanout not set |
| Pixels not visible | `PresentIfDirty()` not called or transfer not executed |

## 6. Related files

- `src/drivers/display_mgr.cpp`  -  uses VirtIO GPU as a display backend
- `src/drivers/graphics.cpp`  -  framebuffer target for scanout
- `src/virt/hypervisor.cpp`  -  tick loop calls `PresentIfDirty()`
- `src/virt/vpci.cpp`  -  virtual PCI device registration
