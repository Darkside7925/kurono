# Package manager

`src/packages/pkgmgr.cpp` and `pkgmgr.h`.

Keeps a local package catalog and can sync repository metadata over plain HTTP from
`kurono.satorut.com`. Each package carries a version, size, dependency list and remote payload
path, and install/remove/update requests run against that.

## The database

Populated at boot from an embedded list, refreshable from a remote `Packages` index. An entry
has name, installed version, latest repo version, installed size in KB, dependency list,
description and repo payload path.

The boot-time list gives sane defaults for the core packages. After a successful sync, the
remote index overwrites `latest_version`, `Filename` and the description.

## apt

The `apt` command delegates here:

| Command | Does |
| --- | --- |
| `apt install <pkg>` | mark installed, resolve deps |
| `apt remove <pkg>` | mark removed |
| `apt update` | refresh the package list |
| `apt upgrade` | upgrade everything installed |
| `apt list` | list known packages |
| `apt search <term>` | search by name |
| `apt show <pkg>` | package details |

## Network

`update` does a real HTTP GET against `kurono.satorut.com`, trying a small set of `Packages`
index paths. If the sync works, installed packages get updated when the repository advertises
a newer version and a payload `Filename`.

## Limits

Install and update fetch the remote payload and store it under
`/kurono/packages/<name>/payload.pkg`, but they don't unpack archives or run filesystem-level
post-install scripts yet. It's a real network-backed fetch path, not a full archive and
extraction pipeline.

## Files

- `src/packages/pkgmgr.cpp` repo sync, manifest parsing, payload fetch
- `src/net/tcpip.cpp` the TCP transport the HTTP client uses
- `src/kernel/kurono_kernel.cpp` boot-time TCP stack init and ticking
