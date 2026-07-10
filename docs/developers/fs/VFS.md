# VFS - Virtual Filesystem Interface

`src/fs/vfs.cpp` and `vfs.h` implement the generic filesystem routing layer.

## 1. What it does

The VFS provides a unified path-based interface that routes I/O requests to the appropriate backend: KVFS, FAT32, or ext4.

Without the VFS, every piece of code that does file I/O would need to know which filesystem to call directly. The VFS removes that coupling and lets the OS support multiple filesystems simultaneously.

## 2. Mount points

The VFS uses mount points to map path prefixes to filesystem backends:

| Mount path | Backend |
| --- | --- |
| `/` | KVFS (default) |
| `/mnt/fat` | FAT32 (if NVMe/USB mounted) |
| `/mnt/ext4` | ext4 (if Linux partition mounted) |

Route selection is done by longest prefix match.

## 3. Interface

The VFS exposes the same operations as KVFS but routes them through the mount table:

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

## 4. Current usage

Most of the OS still calls KVFS directly because the VFS layer was added to support future expansion. New code should prefer the VFS interface so it benefits from the mount system automatically.

## 5. Related files

- `src/fs/kvfs.cpp` - primary backend
- `src/fs/fat32.cpp` - FAT32 backend
- `src/linux/ext4.cpp` - ext4 backend for Linux partitions
