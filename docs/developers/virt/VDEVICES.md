# Virtual devices

`src/virt/vdevices.cpp` and `vdevices.h`, with the device models next to it. The virtual hardware
a guest sees.

| Device | File | What |
| --- | --- | --- |
| Legacy PIC pair (8259) | `vdevices.cpp` | master and slave interrupt controllers, full emulation |
| PIT | `vdevices.cpp` | the 8254 timer, full emulation, counter latch included |
| HPET, APIC | `vdevices.cpp`, `vlapic.cpp` | per vCPU local APIC in xAPIC MMIO mode for SMP guests |
| virtio-blk | `vblk.cpp` | the guest disk, chunked (see [DYNAMIC_VM.md](DYNAMIC_VM.md)) |
| virtio-balloon | `vballoon.cpp` | free page reporting only |
| virtio-net | `vnet.cpp` | NAT into Kurono's network stack |
| virtio-console | `vcon.cpp` | hvc0 (login console) and hvc1 (guest agent channel) |
| 8250/16550A UART | `vserial.cpp` | the guest's serial port |
| i8042 | `vi8042.cpp` | ps/2 keyboard controller |
| 9p share | `v9fs.cpp` | host directory `/home/user/vmshare`, the only one a guest sees |
| guest audio | `vaudio.cpp` | audio bridge |
| virtual PCI bus | `vpci.cpp` | where passthrough devices are shown |
| virtio-gpu | `virtio_gpu_host.cpp` | host side of the virtio GPU |
| Wayland bridge | `wlbridge.cpp` | guest Wayland clients shown as Kurono windows |

## Registration

Each device registers its I/O port range and/or MMIO range at init. When a guest access
triggers a VM exit, `vmexit.cpp` calls `VDevices::HandleIO(port, dir, value)`, which
dispatches to the matching handler.

## PIC and PIT fixes

Two bugs in the interrupt path froze a Kurono guest about 10 seconds into its boot. Both are fixed.

- Non-specific EOI (OCW2 type 1) cleared the highest pending IRQ. It has to clear the highest
  in-service IRQ. With the next tick already pending, the in-service bit was never cleared, and
  IRQ 0 never came again. Linux uses specific EOIs and never hit this. A Kurono guest sends 0x20.
- A PIT counter-latch command overwrote channel 0's mode with 0, which turned the timer into a
  one-shot. A latch command now only latches the count.

## VirtIO

Devices talk to the guest driver over descriptor rings (virtqueues). The host side
processes the entries and returns results. The transport is VirtIO 1.0.

## Files

- `src/virt/vdevices.cpp` the PIC, APIC, PIT and HPET models and the port table
- `src/virt/vlapic.cpp` per vCPU local APIC
- `src/virt/vmexit.cpp` I/O dispatch into this module
- `src/virt/vblk.cpp`, `vballoon.cpp`, `vnet.cpp`, `vcon.cpp`, `vserial.cpp`, `vi8042.cpp`, `v9fs.cpp`
- `src/virt/virtio_dev.cpp` the shared virtqueue code
