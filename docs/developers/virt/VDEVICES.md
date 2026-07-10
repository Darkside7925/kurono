# Virtual Devices

`src/virt/vdevices.cpp` and `vdevices.h` implement the virtual hardware devices presented to the Linux guest VM.

## 1. Device list

| Device | Description |
| --- | --- |
| **VirtIO NIC** | Virtual Ethernet, bridged to Kurono's E1000 via LinuxNetBridge |
| **VirtIO Block** | Virtual disk backed by `src/virt/vdisk.cpp` |
| **VirtIO Console** | Serial terminal device, backed by `src/virt/vserial.cpp` |
| **VirtIO 9P** | Shared filesystem, backed by `src/virt/v9fs.cpp` |
| **i8042 stub** | Keyboard/mouse input for the guest |
| **PIT stub** | Timer interrupt for the guest |
| **VGA/VBE** | Framebuffer for the guest display if needed |

## 2. Device registration

Each device registers its I/O port range and/or MMIO range at init time. When the guest performs an I/O access that triggers a VM exit, `vmexit.cpp` calls `VDevices::HandleIO(port, dir, value)` which dispatches to the matching device handler.

## 3. VirtIO protocol

VirtIO devices communicate with the guest driver via descriptor rings (virtqueues). The host (Kurono) processes virtqueue entries and returns results. The standard VirtIO 1.0 transport over MMIO or PCI is used.

## 4. Related files

- `src/virt/vmexit.cpp` - I/O dispatch into this module
- `src/virt/vdisk.cpp` - block device backend
- `src/virt/vserial.cpp` - serial backend (Conduit terminal)
- `src/virt/v9fs.cpp` - filesystem sharing backend
- `src/linux/linux_netbridge.cpp` - NIC bridge
