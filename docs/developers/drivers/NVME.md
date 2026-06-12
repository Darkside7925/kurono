# NVMe Driver

`src/drivers/nvme.cpp` and `nvme.h` implement an NVMe storage driver for PCIe SSD access.

## 1. What it does

NVMe (Non-Volatile Memory Express) is the standard interface for modern PCIe SSDs. The driver:

1. Detects NVMe controllers on the PCI bus (class 0x01, subclass 0x08).
2. Maps the controller's MMIO BAR.
3. Creates admin and I/O submission/completion queue pairs.
4. Issues Identify commands to discover namespaces.
5. Exposes `ReadSectors` and `WriteSectors` for the filesystem layer.

## 2. Queue model

NVMe uses paired submission and completion queues (SQ/CQ). The driver places commands on the SQ, rings the doorbell register, and polls the CQ for completions. This is simpler than interrupt-driven completion but works for the current polling architecture.

## 3. Use in the OS

The NVMe driver provides block device access for the FAT32 / ext4 layers and backs **reboot persistence** (it's the block device under **KFS**  -  see [KFS.md](../fs/KFS.md)). Read/write are verified end-to-end.

`NVMe::Read`/`Write` do **multi-page DMA**: for a contiguous buffer they build `prp2` (the 2nd page directly, or a PRP-list page naming the 2nd..Nth pages), lifting the old 4 KB-per-command cap to ~2 MB. The PRP builder is offset-aware, so any identity-mapped buffer (stack / heap / PMM) can be a DMA target, not just page-aligned ones. Read/Write also **chunk internally** at `MaxTransferBytes() = min(PRP-list cap, controller MDTS)`  -  exceeding the controller's MDTS is rejected with `sc=0x02` (invalid field), so larger transfers are split transparently.

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| Controller not found | PCI scan missing NVMe class code |
| `Version 0.0.0` / never enables | 64-bit BAR base is in BAR1 (qemu puts it above 4 GB under `-m 4G`); read BAR1's high bits + map the register window |
| Enable timeout | admin/IO queues + DMA buffers must be 4 KB page-aligned (`PMM::AllocBytes`), not 16-byte `KernelHeap` |
| **Every command times out** | the completion-phase read must be `volatile`  -  the controller DMA-writes the CQE, and a plain read gets hoisted out of the poll loop. This one was why nothing ever completed. |
| Commands time out | Queue doorbell not written correctly |
| Data corruption | Cache coherence issue; flush before read-back |

## 5. Related files

- `src/fs/fat32.cpp`  -  filesystem that uses block device reads
- `src/fs/vfs.cpp`  -  VFS layer that routes to block devices
