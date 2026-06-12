# System Logging & Path Layout

`src/system/logging.cpp` / `logging.h` implement Kurono's runtime logging, and
`src/system/kpaths.h` defines the canonical on-disk path layout the whole tree
agrees on. They ship together because the log files are just one part of that
layout.

## 1. Canonical paths (`kpaths.h`)

Before this, logs were triple-homed across `/system/logs`, `/kurono/logs`, and
`/var/log`, and "system" files were split between `/system` and `/kurono`.
`kpaths.h` is now the **single source of truth**: change a root there and the
rest of the kernel follows.

Everything Kurono-native lives under **`/kurono`**; the bare Linux-compat dirs
(`/home`, `/etc`, `/usr`, `/proc`, `/dev`, `/sys`, `/tmp`, `/boot`) stay at the
root so the Kurono Linux Subsystem (KLS) keeps working.

| Macro | Path | Contents |
| --- | --- | --- |
| `KP_ROOT` | `/kurono` | Kurono-native namespace root |
| `KP_LOG_DIR` | `/kurono/var/log` | all logs (one canonical home) |
| `KP_VAR_LIB` | `/kurono/var/lib` | persistent state (KFS/persist image, kpkg db) |
| `KP_ETC` | `/kurono/etc` | Kurono config |
| `KP_SYSTEM` | `/kurono/system` | Kurono system files |

## 2. Logging design

Lightweight by design  -  **no syslog daemon**, just clean structured files, one
per category, written through KVFS. The API is the `RuntimeLog` namespace:

| Function | Target | When |
| --- | --- | --- |
| `LogBoot` | `boot.log` | boot milestones |
| `LogSystem(component, msg)` | `system.log` | general system events |
| `MirrorSerial` | `serial.log` | mirror of the serial console |
| `LogNetwork(event, detail)` | `network.log` | TCP connect / disconnect / RST / errors |
| `LogSecurity(event, detail)` | `security.log` | SUPR escalations, KSA prompts, grants/denials |
| `LogCrash(summary, detail)` | `crash/<n>.log` | kernel panics + minidumps |
| `LogAppEvent` | `apps/<app>` | per-app logs |
| `LogProcessEvent` | `processes/<name>` | per-kernel-process logs |

`InitFilesystem()` creates the directory tree at boot.

## 3. Where the events come from

- **Network**  -  `src/net/tcpip.cpp` calls `LogNetwork` when a TCP connection
  reaches ESTABLISHED and when it sees a RST.
- **Security**  -  `src/security/supr.cpp::SUPR::Log` mirrors every privilege
  decision into `LogSecurity` (with the requesting username).
- **Crash**  -  the kernel's panic / crash-recovery path (`src/kernel/
  kurono_kernel.cpp`) writes a `LogCrash` record and drops an `emergency.txt`
  under `KP_LOG_DIR`.
- **Boot / system**  -  milestones throughout bring-up.

## 4. Inspecting logs

From the Kurono shell, `dmesg` prints `/kurono/var/log/serial.log`; the other
files can be read with the normal file tools or browsed in the file manager.
After a reboot they survive via [KFS](../fs/KFS.md) persistence (the user-data
subtrees are mirrored to disk).

## 5. Related files

- `src/system/kpaths.h`  -  the path layout (authoritative)
- `src/system/logging.cpp` / `.h`  -  the logging implementation
- `src/net/tcpip.cpp`, `src/security/supr.cpp`, `src/kernel/kurono_kernel.cpp`  -  log producers
