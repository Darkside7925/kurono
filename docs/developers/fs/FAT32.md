# FAT32

`src/fs/fat32.cpp` and `fat32.h`. FAT32 partitions.

Reads and writes FAT32 volumes through a block-device-agnostic interface, so the backing
device can be NVMe or USB mass storage.

## Operations

- `Mount(block_device)` parse the boot sector and the FAT
- `ReadFile(path, buf, maxlen)` walk directory entries, follow clusters
- `WriteFile(path, buf, len)` allocate clusters, update FAT entries
- `ListDir(path)`
- `MakeDir(path)`

## Cluster chains

Files are chains of clusters. Each FAT entry points at the next cluster or an end-of-chain
marker. Reading a multi-cluster file means following the chain.

## Long file names

LFN entries are supported for reading, so names longer than 8.3 come through correctly.
LFN creation works for writing too.

## Status

The installer uses the ESP write path, so FAT32 is live there. For general filesystem work
the OS mostly goes through ext4 and KVFS; the FAT32 read path isn't part of the VFS live
routing yet.

## What it's used for

- reading persistent config off a real partition
- writing kernel logs to a FAT32 partition that Windows and Linux can also see
- boot media: the ISO uses FAT32 for the ESP

## When it breaks

| Symptom | Cause |
| --- | --- |
| mount fails | wrong BPB signature, or the partition offset is off |
| files truncated | the chain follower stopped at the wrong EOC value |
| write corruption | FAT table update out of sync with the data write |

## Files

- `src/drivers/nvme.cpp` block device underneath
- `src/fs/vfs.cpp` routes FAT32 requests from VFS
