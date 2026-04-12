# KVFS  -  Kernel Virtual Filesystem

`src/fs/kvfs.cpp` and `kvfs.h` implement the in-memory virtual filesystem that is the primary storage mechanism in Kurono.

## 1. What KVFS is

KVFS is an in-memory key-value filesystem organized as a path tree. It is not backed by a disk  -  it exists entirely in RAM and is populated fresh on every boot. Think of it as a ramdisk with a POSIX-path interface.

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

KVFS does not persist across reboots. All changes are lost when the machine resets. This matches the design of a live OS image and is not a bug.

If persistence is needed, use FAT32 via the NVMe driver or write to a real mounted filesystem.

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
