# Package Manager

`src/packages/pkgmgr.cpp` and `pkgmgr.h` implement the Kurono package manager.

## 1. What it does

The package manager provides a simulated Debian-compatible `apt` interface. It maintains a list of known packages with version, size, and dependency information, and processes install/remove/update requests.

## 2. Package database

The package database is populated at boot from an embedded list. Each package entry has:

- Name
- Version (Debian 13 Trixie era)
- Installed size (KB)
- Dependency list
- Description

The list is representative of a real Trixie system: `libc6`, `bash`, `coreutils`, `apt`, `systemd`, `gcc-14`, `python3.13`, and many others.

## 3. `apt` interface

The `apt` command in the shell delegates to the package manager:

| Command | Action |
| --- | --- |
| `apt install <pkg>` | Mark as installed, resolve deps |
| `apt remove <pkg>` | Mark as removed |
| `apt update` | Refresh package list |
| `apt upgrade` | Upgrade all installed packages |
| `apt list` | List all known packages |
| `apt search <term>` | Search by name |
| `apt show <pkg>` | Show package details |

## 4. Network interaction

`apt update` can attempt an HTTP fetch from a Trixie mirror URL if the network stack is available. Without network, it uses the embedded package list.

## 5. Limitations

Package install does not download and extract real binaries  -  it updates the package database state. The package manager demonstrates the interface and metadata handling, not full binary delivery.

## 6. Related files

- `src/shell/linux_cmds.cpp`  -  `apt` command registration
- `src/net/tcpip.cpp`  -  HTTP fetch for `apt update`
- `src/linux/kls.cpp`  -  Trixie package version database
