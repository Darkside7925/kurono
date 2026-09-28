# KLS

`src/linux/kls.cpp` and `kls.h`. The Linux personality layer that makes the Kurono shell look
like a Debian 13 Trixie box.

It supplies the detailed Linux-alike data on top of the basic shell commands:

- a package database of 30+ packages with Trixie-era versions
- kernel string `6.12.0-kurono`
- `uname -a` output that matches current Trixie
- `/etc/os-release` with `VERSION_ID=13` and `VERSION_CODENAME=trixie`
- `lsb_release -a` output
- `/proc/version` and `/proc/cpuinfo` generation
- `dpkg -l` and `dpkg --get-selections` output

## Package versions

All updated to Debian 13 Trixie equivalents:

| Package | Version |
| --- | --- |
| `libc6` | 2.41 |
| `bash` | 5.2.37 |
| `apt` | 3.0.3 |
| `coreutils` | 9.7 |
| `gcc-14` | 14.2 |
| `python3.13` | 3.13.3 |
| `systemd` | 257 |
| `git` | 2.47.2 |
| `curl` | 8.14.1 |
| `nano` | 8.3 |
| `wget` | 1.25.0 |

The full list is in `kls.cpp`.

## GetKernelVersion()

Returns `"6.12.0-kurono"`. That's the string in `uname` output and in Settings, About.

## Virtual files

KLS generates the `/proc` and `/etc` files dynamically. `cat /proc/cpuinfo` is built from live
`CPUDetect` results, and `cat /etc/os-release` returns Trixie-accurate content.

## Files

- `src/linux/linux_kernel.cpp` calls into KLS for syscall-level Linux personality
- `src/apps/settings.cpp` shows the kernel version from `GetKernelVersion()`
- `src/packages/pkgmgr.cpp` uses the package database from KLS
