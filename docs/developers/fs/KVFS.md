# KVFS

`src/fs/kvfs.cpp` and `kvfs.h`. The in-memory filesystem, and the main storage thing in
Kurono.

A path tree living in RAM, rebuilt fresh every boot. User-data subtrees persist anyway,
because they get mirrored to KFS, a real on-disk filesystem (see [KFS.md](KFS.md)).
Ramdisk with a POSIX-path interface, KFS for the backing store.

The shell, desktop, settings, text editor and config system all use it. Unless something
explicitly wants FAT32 or ext4, file access is KVFS.

## API

```cpp
// create and write
KVFS::Mkdir(path)                          // create directory
KVFS::Mkdirs(path)                         // create directory tree (follows symlinks)
KVFS::Symlink(path, target)                // create/repoint a symlink node
KVFS::InstallCanonicalLayout()             // lay the /kurono tree + compat symlinks
KVFS::WriteString(path, content)           // write file from string
KVFS::WriteBytes(path, buf, len)           // write file from buffer

// read
KVFS::ReadString(path, buf, maxlen)        // read to buffer, returns bytes read
KVFS::ReadBytes(path, buf, maxlen)         // binary read

// query
KVFS::Exists(path)
KVFS::IsDir(path)
KVFS::List(path)                           // directory entries

// delete
KVFS::Delete(path)                         // file or empty directory
```

## The canonical tree

One real tree at `/kurono`, plus a thin compat-symlink overlay at the old top-level names.
Kurono-native things live under `/kurono`, the bare names are symlinks into it. That keeps the
~800 hardcoded path references in the codebase resolving while collapsing the old `/system`
vs `/kurono/system` split.

```
/
└── kurono/
    ├── system/    core os (bin, lib, drivers, config, boot, security, themes)
    ├── linux/     linux compat (compat/{bin,lib}, linker, bridge, drivers)
    ├── windows/   windows compat environment
    ├── apps/      installed native kurono apps (bin, lib)
    ├── user/      all user data (home/user/{Desktop,Documents,...}, shared)
    ├── packages/  kpkg state
    ├── runtime/   live state (proc, dev, tmp, sockets), conceptually cleared on boot
    └── var/{log,lib,updates,state}

compat symlinks at the root (resolved through intermediate components):
  /system  -> /kurono/system           /home    -> /kurono/user/home
  /etc     -> /kurono/system/config    /bin     -> /kurono/system/bin
  /lib     -> /kurono/linux/compat     /usr/bin -> /kurono/linux/compat/bin
  /usr/lib -> /kurono/linux/compat/lib /tmp     -> /kurono/runtime/tmp
  /proc    -> /kurono/runtime/proc     /dev     -> /kurono/runtime/dev
  /var     -> /kurono/var              /apps    -> /kurono/apps
  /windows -> /kurono/windows
```

The roots are macros in `src/system/kpaths.h` (`KP_SYSTEM_ROOT`, `KP_LINUX_COMPAT`,
`KP_USER_HOME`, `KP_RUNTIME`, and the rest). `KVFS::InstallCanonicalLayout()` installs the
tree and the symlinks. It runs at the very start of `BuildDefaultTree()`, so inside
`KVFS::Init()` before anything else touches a path, and again after a persistent-tree
restore.

`KVFS::Resolve` and `KVFS::Mkdirs` follow symlinks in intermediate path components, 16 hops
max. So a later `Mkdirs("/system/lib/x")` or `WriteFile("/etc/hostname")` lands in the real
`/kurono` directory instead of materialising a stray real `/system` or `/etc` at the root.

Shell commands, the desktop icons, the config system, ld-kurono (it searches `/system/lib`)
and logging (`/kurono/var/log`) all lean on this layout.

## Persistence

Across reboots it goes through KFS (see [KFS.md](KFS.md)), via `PersistStore::SaveTree` and
`LoadTree` in `src/fs/persist.cpp`. On a clean shutdown or reboot, and on demand from the
`persisttest` shell command, `SaveTree` formats a fresh KFS volume on the NVMe data disk and
mirrors the user-data subtrees (`/home`, `/etc`, `/root`) into it as real files and
directories. At boot, `LoadTree` mounts the volume and walks it back into KVFS, before the
boot seeding refills the big `/usr` binaries.

KFS v2, the extent-based one, killed the old per-file cap (`KFS_MAX_FILE`). `SaveTree` now
mirrors whatever KVFS holds, limited only by free disk space (see [KFS.md](KFS.md)). The
re-seeded `/usr` binaries are still skipped on save because boot seeding recreates them, not
because of a size limit. Since the snapshot is a real filesystem, a future `kfs-fuse` driver
could mount the volume on Linux and browse the files directly. (The older opaque raw-sector
blob store still exists as `PersistStore::Save` and `Load`, but nothing takes that path any
more.)

## Limitations

| Limitation | Note |
| --- | --- |
| permissions are advisory | mode bits are stored, but the single-address-space kernel doesn't enforce them across all callers |
| no hard links | the path is the only way to identify a file, symlinks are supported |
| live tree lives in RAM | total live size is bounded by the kernel heap |
| selective persistence | the runtime tree is rebuilt every boot, but `/home`, `/etc` and `/root` come back through KFS and the `/usr` binaries are regenerated. No per-file cap any more |

## Files

- `src/fs/vfs.cpp` the routing layer in front of KVFS or FAT32
- `src/system/ui_config.cpp` reads `/etc/kurono/ui.conf` out of KVFS
- `src/ui/desktop.cpp` reads and writes `/home/user/Desktop/`
