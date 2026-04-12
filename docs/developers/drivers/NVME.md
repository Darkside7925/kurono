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

The NVMe driver provides block device access for the FAT32 and ext4 layers. When Kurono is booted on real hardware with an NVMe SSD, persistent configuration data can be stored on that device.

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| Controller not found | PCI scan missing NVMe class code |
| Commands time out | Queue doorbell not written correctly |
| Data corruption | Cache coherence issue; flush before read-back |

## 5. Related files

- `src/fs/fat32.cpp`  -  filesystem that uses block device reads
- `src/fs/vfs.cpp`  -  VFS layer that routes to block devices
