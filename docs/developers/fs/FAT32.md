# FAT32

`src/fs/fat32.cpp` and `fat32.h` implement the FAT32 filesystem for accessing disk partitions.

## 1. What it does

The FAT32 driver reads and writes FAT32 formatted partitions. It provides a block-device-agnostic interface  -  the underlying block device can be an NVMe drive or a USB mass storage device.

## 2. Supported operations

- `Mount(block_device)`  -  parse the FAT32 boot sector and FAT table
- `ReadFile(path, buf, maxlen)`  -  traverse directory entries and read file clusters
- `WriteFile(path, buf, len)`  -  allocate clusters and update FAT entries
- `ListDir(path)`  -  enumerate directory entries
- `MakeDir(path)`  -  create a directory entry chain

## 3. Cluster chains

FAT32 files are stored as chains of clusters. Each cluster number in the FAT points to the next cluster or to the end-of-chain marker. The driver follows these chains when reading multi-cluster files.

## 4. Long file names

FAT32 LFN (Long File Name) entries are supported for reading. Files with names longer than 8.3 characters are read correctly. LFN creation is supported for writing.

## 5. Use cases in Kurono

- Reading persistent configuration from a real disk partition.
- Writing kernel logs to a FAT32 partition that is also visible from Windows/Linux.
- Boot media reading: the ISO uses FAT32 as the ESP filesystem.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Mount fails | Wrong BPB signature or partition offset |
| Files truncated | Cluster chain follower stopping at wrong EOC value |
| Write corruption | FAT table update out of sync with data write |

## 7. Related files

- `src/drivers/nvme.cpp`  -  block device for FAT32
- `src/fs/vfs.cpp`  -  routes FAT32 requests from VFS
