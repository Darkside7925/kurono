# Linux Commands

`src/shell/linux_cmds.cpp` and `linux_cmds.h` implement the POSIX/Linux-style commands available in the Kurono shell.

## 1. What commands are here

These are the commands a Linux user would expect. Representative list:

| Command | Behavior |
| --- | --- |
| `ls` | List directory contents |
| `cat` | Print file contents |
| `pwd` | Print working directory |
| `cd` | Change directory |
| `mkdir` | Create directory |
| `rm` | Remove file or directory |
| `cp` | Copy file |
| `mv` | Move or rename |
| `echo` | Print arguments |
| `grep` | Search for a pattern in a file |
| `ps` | List processes |
| `kill` | Send signal to process |
| `top` | Live process view |
| `uname` | Print OS information |
| `whoami` | Print current user |
| `df` | Disk usage |
| `free` | Memory usage |
| `ifconfig` | Network interface info |
| `ping` | ICMP echo |
| `curl` | HTTP fetch via TCP/IP stack |
| `wget` | HTTP download |
| `apt` | Debian package manager frontend |
| `alpine` / `apk` / `debian` | guest package/management bridges |

> `echo` is **not** a Linux command  -  it is a Kurono-native builtin (registered
> in `shell.cpp` with `ENV_KURONO`). There is **no `sudo` command**; privilege
> escalation is done with the native `supr` command (`supr <cmd>` runs a command
> elevated  -  see [../security/SUPR.md](../security/SUPR.md)).
>
> `linux_cmds.cpp` registers **63 distinct commands**  -  the table above is a
> representative subset, not the full list.

## 2. KVFS backing

All file operations work against the KVFS virtual filesystem. `ls`, `cat`, `mkdir`, `rm`, and similar commands call `KVFS::List()`, `KVFS::ReadString()`, `KVFS::Mkdir()`, `KVFS::Delete()`.

## 3. Registration

All commands in this file are registered in bulk by `LinuxCmds::RegisterAll(sh)`
(in `linux_cmds.cpp`, invoked from shell init)  -  *not* by `RegisterBuiltins()` in
`shell.cpp`. Their `category` is one of `filesystem`, `text`, `system`,
`network`, or `package`, and most use the `ENV_LINUX` (or `ENV_AUTO`)
environment flag so they appear in the Linux shell surface.

## 4. Related files

- `src/shell/linux_cmds.cpp` / `.h`  -  the commands + `RegisterAll`
- `src/shell/shell.cpp`  -  the registry + dispatch they hook into
- `src/fs/kvfs.cpp`  -  filesystem operations
- `src/net/network.cpp`  -  used by `ping` and `ifconfig`
- `src/packages/pkgmgr.cpp`  -  used by `apt`
