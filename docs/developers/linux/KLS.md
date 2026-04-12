# KLS  -  Kurono Linux Shell

`src/linux/kls.cpp` and `kls.h` implement the Linux compatibility personality layer that makes Kurono's shell feel like a real Debian 13 Trixie system.

## 1. What it does

KLS (Kurono Linux Shell) provides the detailed Linux-alike data layer. It supplements the basic shell commands with realistic system information that matches a Debian 13 Trixie installation:

- A full package database of 30+ packages with Trixie-era versions
- Kernel version string: `6.12.0-kurono`
- `uname -a` output matching current Trixie
- `/etc/os-release` content with `VERSION_ID=13` and `VERSION_CODENAME=trixie`
- `lsb_release -a` output
- `/proc/version` and `/proc/cpuinfo` generation
- `dpkg -l` package listing
- `dpkg --get-selections` output

## 2. Package version accuracy

All package versions in KLS were updated to Debian 13 Trixie (released 2025/2026) equivalents:

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

## 3. `GetKernelVersion()`

Returns `"6.12.0-kurono"`  -  the version string used in `uname` output and displayed in the About section of Settings.

## 4. Virtual file generation

KLS generates virtual `/proc` and `/etc` files dynamically. `cat /proc/cpuinfo` returns text derived from the live `CPUDetect` results. `cat /etc/os-release` returns Trixie-accurate content.

## 5. Related files

- `src/linux/linux_kernel.cpp`  -  calls into KLS for syscall-level Linux personality
- `src/apps/settings.cpp`  -  displays the kernel version from `GetKernelVersion()`
- `src/packages/pkgmgr.cpp`  -  uses the package database from KLS
