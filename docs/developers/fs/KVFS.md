# KVFS  -  Kernel Virtual Filesystem

`src/fs/kvfs.cpp` and `kvfs.h` implement the in-memory virtual filesystem that is the primary storage mechanism in Kurono.

## 1. What KVFS is

KVFS is an in-memory key-value filesystem organized as a path tree. It lives in RAM and is populated fresh on every boot, but its user-data subtrees now **persist across reboots** by being mirrored to **KFS**, a real on-disk filesystem (see [KFS.md](KFS.md)). Think of it as a ramdisk with a POSIX-path interface; KFS is its on-disk backing store.

KVFS is the filesystem that the shell, the desktop, the settings app, the text editor, and the config system all use. Unless explicitly using FAT32 or ext4, all file operations go through KVFS.

## 2. API

```cpp
// Create and write
KVFS::Mkdir(path)                          // create directory
KVFS::Mkdirs(path)                         // create directory tree
KVFS::WriteString(path, content)           // write file from string
KVFS::WriteBytes(path, buf, len)           // write file from buffer

// Read
KVFS::ReadString(path, buf, maxlen)        // read file to buffer, returns bytes read
KVFS::ReadBytes(path, buf, maxlen)         // binary read

// Query
KVFS::Exists(path)                         // true if file or directory exists
KVFS::IsDir(path)                          // true if directory
KVFS::List(path)                           // list directory entries

// Delete
KVFS::Delete(path)                         // delete file or empty directory
```

## 3. Initial population

The kernel populates KVFS at boot with the standard directory tree and some demonstration files:

```
/
  etc/
    kurono/
      ui.conf
  home/
    user/
      Desktop/
  tmp/
  var/
    log/
  usr/
    bin/
```

The shell commands, the desktop icon system, and the config all rely on these directories existing.

## 4. Persistence

KVFS persists across reboots through **KFS**, a real on-disk filesystem (see [KFS.md](KFS.md)), via `PersistStore::SaveTree` / `LoadTree` (`src/fs/persist.cpp`). On a clean shutdown / reboot  -  and on demand via the `persisttest` shell command  -  `SaveTree` formats a fresh KFS volume on the NVMe data disk and **mirrors the user-data subtrees (`/home`, `/etc`, `/root`) into it as real files + directories**. At boot, `LoadTree` mounts the volume and walks it back into KVFS, before the boot seeding re-fills the large `/usr` binaries.

To keep the volume small and the restore fast, only file content up to `KFS_MAX_FILE` per file is stored; the re-seeded `/usr` binaries and the >4 MB sample media are skipped (the boot seeding re-creates them). Because the snapshot is a real filesystem, a future `kfs-fuse` driver could mount the volume on Linux and browse the files directly. (The earlier opaque raw-sector blob store still exists as `PersistStore::Save`/`Load` but is no longer the path persistence takes.)

## 5. Limitations

| Limitation | Note |
| --- | --- |
| No permissions | All files accessible to all code |
| No hard links | Path is the only way to identify a file |
| No large files | Buffer backed; RAM limited |
| No persistence | Volatile; reset on reboot |

## 6. Related files

- `src/fs/vfs.cpp`  -  generic VFS layer that routes calls to KVFS or FAT32
- `src/system/ui_config.cpp`  -  reads `/etc/kurono/ui.conf` from KVFS
- `src/ui/desktop.cpp`  -  reads/writes `/home/user/Desktop/` entries
