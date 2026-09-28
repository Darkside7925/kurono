# VFS

`src/fs/vfs.cpp` and `vfs.h`. The routing layer in front of the filesystem backends.

One path-based interface that routes to KVFS, FAT32 or ext4, so callers don't have to
know which filesystem they're on.

## Mounts

Path prefix to backend, longest prefix wins:

| Mount | Backend |
| --- | --- |
| `/` | KVFS (default) |
| `/mnt/fat` | FAT32, if an NVMe or USB volume is mounted |
| `/mnt/ext4` | ext4, if a Linux partition is mounted |

## Interface

Same operations as KVFS, routed through the mount table:

```cpp
VFS::Open(path, flags)
VFS::Read(handle, buf, len)
VFS::Write(handle, buf, len)
VFS::Close(handle)
VFS::Stat(path)
VFS::List(path)
VFS::Mkdir(path)
VFS::Delete(path)
```

## Honest status

Most of the OS still calls KVFS directly. This layer went in for future expansion, so
new code should go through VFS and get the mount handling for free.

## Files

- `src/fs/kvfs.cpp` the primary backend
- `src/fs/fat32.cpp` FAT32 backend
- `src/linux/ext4.cpp` ext4 backend for Linux partitions
