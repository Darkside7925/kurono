# Package Manager

`src/packages/pkgmgr.cpp` and `pkgmgr.h` implement the Kurono package manager.

## 1. What it does

The package manager maintains a local package catalog and can now sync repository metadata over plain HTTP from `kurono.satorut.com`. It tracks known packages with version, size, dependency, and remote payload path information, and processes install/remove/update requests.

## 2. Package database

The package database is populated at boot from an embedded list and can be refreshed from a remote `Packages` index. Each package entry has:

- Name
- Installed version
- Latest repository version
- Installed size (KB)
- Dependency list
- Description
- Repository payload path

The boot-time list provides sane defaults for Kurono core packages. After a successful sync, repository metadata overwrites `latest_version`, `Filename`, and description fields from the remote index.

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

`update` performs a real HTTP GET against `kurono.satorut.com` and tries a small set of `Packages` index paths. If the sync succeeds, installed packages can be updated when the repository advertises a newer version and a payload `Filename`.

## 5. Limitations

Package install and update fetch the remote payload and store it under `/kurono/packages/<name>/payload.pkg`, but they do not yet unpack archives or apply filesystem-level post-install scripts. The current implementation is a real network-backed package fetch path, not a full archive/extraction pipeline.

## 6. Related files

- `src/packages/pkgmgr.cpp` - repository sync, manifest parsing, payload fetch
- `src/net/tcpip.cpp` - TCP transport used by the package HTTP client
- `src/kernel/kurono_kernel.cpp` - boot-time TCP stack initialization and ticking
