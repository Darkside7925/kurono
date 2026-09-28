# NVMe

`src/drivers/nvme.cpp` and `nvme.h`. PCIe SSD storage.

1. finds controllers on PCI (class 0x01, subclass 0x08)
2. maps the MMIO BAR
3. creates admin and I/O submission/completion queue pairs
4. Identify commands to discover namespaces
5. `ReadSectors` and `WriteSectors` for the filesystem layer

## Queues

Paired SQ/CQ. Commands go on the SQ, the doorbell gets rung, the CQ gets polled for
completions. Simpler than interrupt-driven completion, and it fits the polling
architecture.

## Where it's used

Block access for the FAT32 and ext4 layers, and it's the device under KFS reboot
persistence (see [KFS.md](../fs/KFS.md)). Read and write are verified end to end.

`NVMe::Read` and `Write` do multi-page DMA. For a contiguous buffer they build `prp2`:
the second page directly, or a PRP-list page naming pages 2 to N. That lifts the old 4KB
per command cap to about 2MB. The PRP builder is offset-aware, so any identity-mapped
buffer works as a DMA target (stack, heap, PMM), not only page-aligned ones. Read and
Write also chunk internally at `MaxTransferBytes()`, which is `min(PRP-list cap,
controller MDTS)`. Going past the controller's MDTS gets rejected with `sc=0x02` (invalid
field), so bigger transfers get split without the caller knowing.

## When it breaks

| Symptom | Cause |
| --- | --- |
| controller not found | PCI scan missing the NVMe class code |
| `Version 0.0.0`, never enables | the 64-bit BAR base is in BAR1 (qemu puts it above 4GB under `-m 4G`), so read BAR1's high bits and map the register window |
| enable timeout | admin/I/O queues and DMA buffers must be 4KB page aligned (`PMM::AllocBytes`), not 16-byte `KernelHeap` |
| every command times out | the completion-phase read must be `volatile`. The controller DMA-writes the CQE and a plain read gets hoisted out of the poll loop. This one is why nothing ever completed. |
| commands time out | doorbell not written correctly |
| data corruption | cache coherence, flush before read-back |

## Files

- `src/fs/fat32.cpp` reads blocks through this
- `src/fs/vfs.cpp` routes to block devices
