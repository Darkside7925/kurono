# Virtual devices

`src/virt/vdevices.cpp` and `vdevices.h`. The virtual hardware the Linux guest sees.

| Device | What |
| --- | --- |
| VirtIO NIC | virtual Ethernet, bridged to Kurono's E1000 through LinuxNetBridge |
| VirtIO Block | virtual disk backed by `src/virt/vdisk.cpp` |
| VirtIO Console | serial terminal, backed by `src/virt/vserial.cpp` |
| VirtIO 9P | shared filesystem, backed by `src/virt/v9fs.cpp` |
| i8042 stub | keyboard and mouse for the guest |
| PIT stub | timer interrupt for the guest |
| VGA/VBE | framebuffer for the guest if it wants one |

## Registration

Each device registers its I/O port range and/or MMIO range at init. When a guest access
triggers a VM exit, `vmexit.cpp` calls `VDevices::HandleIO(port, dir, value)`, which
dispatches to the matching handler.

## VirtIO

Devices talk to the guest driver over descriptor rings (virtqueues). The host side
processes the entries and returns results. Standard VirtIO 1.0 transport over MMIO or PCI.

## Files

- `src/virt/vmexit.cpp` I/O dispatch into this module
- `src/virt/vdisk.cpp` block device backend
- `src/virt/vserial.cpp` serial backend, the Conduit terminal
- `src/virt/v9fs.cpp` filesystem sharing backend
- `src/linux/linux_netbridge.cpp` NIC bridge
