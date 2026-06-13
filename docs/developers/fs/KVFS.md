# KVFS  -  Kernel Virtual Filesystem

`src/fs/kvfs.cpp` and `kvfs.h` implement the in-memory virtual filesystem that is the primary storage mechanism in Kurono.

## 1. What KVFS is

KVFS is an in-memory key-value filesystem organized as a path tree. It lives in RAM and is populated fresh on every boot, but its user-data subtrees now **persist across reboots** by being mirrored to **KFS**, a real on-disk filesystem (see [KFS.md](KFS.md)). Think of it as a ramdisk with a POSIX-path interface; KFS is its on-disk backing store.

KVFS is the filesystem that the shell, the desktop, the settings app, the text editor, and the config system all use. Unless explicitly using FAT32 or ext4, all file operations go through KVFS.

## 2. API

```cpp
// Create and write
KVFS::Mkdir(path)                          // create directory
KVFS::Mkdirs(path)                         // create directory tree (follows symlinks)
KVFS::Symlink(path, target)                // create/repoint a symlink node
KVFS::InstallCanonicalLayout()             // lay the /kurono tree + compat symlinks
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

## 3. Initial population & the canonical path layout

KVFS uses a **single canonical tree rooted at `/kurono`** plus a thin **compat-symlink overlay** at the old top-level names. Everything Kurono-native lives under `/kurono`; the bare names (`/system`, `/home`, `/etc`, `/bin`, `/lib`, `/tmp`, `/proc`, `/dev`, `/var`, `/apps`, `/usr/bin`, `/usr/lib`) are **symlinks** into it. This keeps the ~800 hardcoded path references across the codebase resolving while collapsing the old `/system` vs `/kurono/system` split.

```
/
└── kurono/
    ├── system/    core os (bin, lib, drivers, config, boot, security, themes)
    ├── linux/     linux compat (compat/{bin,lib}, linker, bridge, drivers)
    ├── windows/   windows compat environment
    ├── apps/      installed native kurono apps (bin, lib)
    ├── user/      all user data (home/user/{Desktop,Documents,...}, shared)
    ├── packages/  kpkg state
    ├── runtime/   live state (proc, dev, tmp, sockets)  -  conceptually cleared on boot
    └── var/{log,lib,updates,state}

compat symlinks at the root (resolved through intermediate components):
  /system  -> /kurono/system          /home    -> /kurono/user/home
  /etc     -> /kurono/system/config    /bin     -> /kurono/system/bin
  /lib     -> /kurono/linux/compat     /usr/bin -> /kurono/linux/compat/bin
  /usr/lib -> /kurono/linux/compat/lib /tmp     -> /kurono/runtime/tmp
  /proc    -> /kurono/runtime/proc     /dev     -> /kurono/runtime/dev
  /var     -> /kurono/var              /apps    -> /kurono/apps
  /windows -> /kurono/windows
```

The canonical roots are centralized as macros in `src/system/kpaths.h` (`KP_SYSTEM_ROOT`, `KP_LINUX_COMPAT`, `KP_USER_HOME`, `KP_RUNTIME`, ...). The tree and the symlinks are installed by `KVFS::InstallCanonicalLayout()`, which runs at the **very start** of `BuildDefaultTree()` (i.e. inside `KVFS::Init()`, before any other code touches a path) and again after a persistent-tree restore. Because `KVFS::Resolve` and `KVFS::Mkdirs` both follow symlinks in intermediate path components (16-hop budget), a later `Mkdirs("/system/lib/x")` or `WriteFile("/etc/hostname")` resolves *through* the symlink into the real `/kurono` dir instead of materialising a stray real `/system` or `/etc` at the root.

The shell commands, the desktop icon system, the config system, the dynamic linker (ld-kurono searches `/system/lib`), and the logging subsystem (`/kurono/var/log`) all rely on this layout.

## 4. Persistence

KVFS persists across reboots through **KFS**, a real on-disk filesystem (see [KFS.md](KFS.md)), via `PersistStore::SaveTree` / `LoadTree` (`src/fs/persist.cpp`). On a clean shutdown / reboot  -  and on demand via the `persisttest` shell command  -  `SaveTree` formats a fresh KFS volume on the NVMe data disk and **mirrors the user-data subtrees (`/home`, `/etc`, `/root`) into it as real files + directories**. At boot, `LoadTree` mounts the volume and walks it back into KVFS, before the boot seeding re-fills the large `/usr` binaries.

To keep the volume small and the restore fast, only file content up to `KFS_MAX_FILE` per file is stored; the re-seeded `/usr` binaries and the >4 MB sample media are skipped (the boot seeding re-creates them). Because the snapshot is a real filesystem, a future `kfs-fuse` driver could mount the volume on Linux and browse the files directly. (The earlier opaque raw-sector blob store still exists as `PersistStore::Save`/`Load` but is no longer the path persistence takes.)

## 5. Limitations

| Limitation | Note |
| --- | --- |
| Permissions advisory | POSIX-style mode bits are stored, but the single-address-space kernel does not yet enforce them across all callers |
| No hard links | Path is the only way to identify a file (symlinks *are* supported  -  see §3) |
| RAM-resident live tree | The live tree lives in RAM, so total live size is bounded by the kernel heap |
| Selective persistence | The runtime tree is rebuilt fresh each boot, but the user-data subtrees (`/home`, `/etc`, `/root`) persist across reboot through KFS (§4); large re-seeded `/usr` binaries and >`KFS_MAX_FILE` media are regenerated at boot rather than stored |

## 6. Related files

- `src/fs/vfs.cpp`  -  generic VFS layer that routes calls to KVFS or FAT32
- `src/system/ui_config.cpp`  -  reads `/etc/kurono/ui.conf` from KVFS
- `src/ui/desktop.cpp`  -  reads/writes `/home/user/Desktop/` entries
