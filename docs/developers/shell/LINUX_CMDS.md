# Linux commands

`src/shell/linux_cmds.cpp` and `linux_cmds.h`. The POSIX-style command surface.

What a Linux user would expect:

| Command | Does |
| --- | --- |
| `ls` | list directory contents |
| `cat` | print file contents |
| `pwd` | print working directory |
| `cd` | change directory |
| `mkdir` | create a directory |
| `rm` | remove a file or directory |
| `cp` | copy a file |
| `mv` | move or rename |
| `echo` | print arguments |
| `grep` | search a file for a pattern |
| `ps` | list processes |
| `kill` | signal a process |
| `top` | live process view |
| `uname` | OS information |
| `whoami` | current user |
| `df` | disk usage |
| `free` | memory usage |
| `ifconfig` | network interface info |
| `ping` | ICMP echo |
| `curl` | HTTP fetch through the TCP/IP stack |
| `wget` | HTTP download |
| `apt` | Debian package manager frontend |
| `alpine`, `apk`, `debian` | guest package and management bridges |

Two things people get wrong here. `echo` is not a Linux command, it's a Kurono-native
builtin registered in `shell.cpp` with `ENV_KURONO`. And there is no `sudo`. Privilege
escalation is the native `supr` command: `supr <cmd>` runs something elevated, see
[../security/SUPR.md](../security/SUPR.md).

`linux_cmds.cpp` registers 63 distinct commands. The table above is a representative
subset, not the full list.

## Backing

File operations all go to KVFS. `ls`, `cat`, `mkdir`, `rm` and friends call `KVFS::List()`,
`KVFS::ReadString()`, `KVFS::Mkdir()` and `KVFS::Delete()`.

## Registration

Everything in this file registers in bulk through `LinuxCmds::RegisterAll(sh)`, invoked from
shell init, not from `RegisterBuiltins()` in `shell.cpp`. Categories are `filesystem`,
`text`, `system`, `network` or `package`, and most use `ENV_LINUX` (or `ENV_AUTO`) so they
show up in the Linux shell surface.

## Files

- `src/shell/linux_cmds.cpp` / `.h` the commands and `RegisterAll`
- `src/shell/shell.cpp` the registry and dispatch they hook into
- `src/fs/kvfs.cpp` filesystem operations
- `src/net/network.cpp` behind `ping` and `ifconfig`
- `src/packages/pkgmgr.cpp` behind `apt`
