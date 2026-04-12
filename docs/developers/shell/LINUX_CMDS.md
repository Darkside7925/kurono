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
| `apt` | Package manager frontend |
| `sudo` | Privilege escalation stub |

## 2. KVFS backing

All file operations work against the KVFS virtual filesystem. `ls`, `cat`, `mkdir`, `rm`, and similar commands call `KVFS::List()`, `KVFS::ReadString()`, `KVFS::Mkdir()`, `KVFS::Delete()`.

## 3. Registration

All commands in this file are registered in bulk during `RegisterBuiltins()` inside `shell.cpp`. The category for these commands is `"linux"` or the more specific `"files"`, `"network"`, etc.

## 4. Related files

- `src/shell/shell.cpp`  -  registration and dispatch
- `src/fs/kvfs.cpp`  -  filesystem operations
- `src/net/network.cpp`  -  used by `ping` and `ifconfig`
- `src/packages/pkgmgr.cpp`  -  used by `apt`
