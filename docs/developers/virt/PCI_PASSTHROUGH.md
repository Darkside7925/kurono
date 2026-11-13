# PCI passthrough

`src/virt/pci_passthrough.cpp` and `pci_passthrough.h`. Gives a PCI function on the host to a
`vm kurono` guest.

## How to use it

```
kurono.vm.pt=BB:DD.F[,BB:DD.F]
```

The boot flag names the functions to reserve, in bus:device.function form. Reserved functions are
skipped by Kurono's own host drivers at boot, so the host never touches them. The next `vm kurono`
guest gets them.

When the guest starts, `AttachReserved` does four things for each device:

- exposes the device on the virtual PCI bus with its real IDs and BARs
- maps its BARs into the guest's EPT, uncached
- puts the device in a VT-d domain whose IOVA equals the guest physical address, so its DMA
  reaches the guest's memory
- forwards the device's INTx interrupt to the guest as an edge. INTx stays disabled on the host
  side.

Passthrough needs VT-d. Without it, the attach is refused and the guest starts without the device.
Before guest RAM is freed, `DetachGuest` turns bus mastering off and releases the domain.

Lazy guest RAM is off for a guest with reserved devices (see [DYNAMIC_VM.md](DYNAMIC_VM.md)).

In the tests for part 3 of the major update an e1000 network card was passed to a Kurono guest, and the guest got a DHCP
lease.

## What is refused

`refused()` in `pci_passthrough.cpp` turns down these, with a log line saying why:

- a function that is not there
- anything from NVIDIA (vendor `0x10DE`)
- storage controllers (class `0x01`)
- display controllers (class `0x03`)
- bridges (class `0x06`)
- three functions on the laptop the host needs: `02:00.0`, `81:00.0` and `80:14.0`

A refused function is skipped, and the boot goes on.

## The GPU handoff

The same file also has the older NVIDIA and AMD GPU handoff, `HandoffNvidiaGPU()` and
`HandoffAmdGPU()`. That path is separate from `kurono.vm.pt` and is not how a `vm kurono`
guest gets a device.

## Files

- `src/virt/pci_passthrough.cpp` and `pci_passthrough.h` the reserve list, refusal rules and attach
- `src/virt/iommu.cpp` and `iommu.h` the VT-d domains used for DMA
- `src/virt/ept.cpp` the BAR mappings in the guest's EPT
- `src/virt/vpci.cpp` the virtual PCI bus the device is shown on
- `src/kernel/kurono_kernel.cpp` the `kurono.vm.pt` boot flag
