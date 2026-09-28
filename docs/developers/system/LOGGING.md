# Logging and the path layout

`src/system/logging.cpp` / `logging.h` are the runtime logging, and `src/system/kpaths.h` is the
canonical path layout the whole tree agrees on. They ship together because the log files are
just one part of that layout.

## Canonical paths

Before this, logs were triple-homed across `/system/logs`, `/kurono/logs` and `/var/log`, and
"system" files were split between `/system` and `/kurono`. `kpaths.h` is now the single source
of truth: change a root there and the rest of the kernel follows.

Everything Kurono-native lives under `/kurono`. The bare Linux-compat directories (`/home`,
`/etc`, `/usr`, `/proc`, `/dev`, `/sys`, `/tmp`, `/boot`) stay at the root so KLS keeps working.

| Macro | Path | Contents |
| --- | --- | --- |
| `KP_ROOT` | `/kurono` | Kurono-native namespace root |
| `KP_LOG_DIR` | `/kurono/var/log` | all logs, one canonical home |
| `KP_VAR_LIB` | `/kurono/var/lib` | persistent state: KFS/persist image, kpkg db |
| `KP_ETC` | `/kurono/etc` | Kurono config |
| `KP_SYSTEM` | `/kurono/system` | Kurono system files |

## Logging design

Lightweight on purpose. No syslog daemon, just structured files, one per category, written
through KVFS. The API is the `RuntimeLog` namespace:

| Function | Target | When |
| --- | --- | --- |
| `LogBoot` | `boot.log` | boot milestones |
| `LogSystem(component, msg)` | `system.log` | general system events |
| `MirrorSerial` | `serial.log` | mirror of the serial console, deferred |
| `LogNetwork(event, detail)` | `network.log` | TCP connect, disconnect, RST, errors |
| `LogSecurity(event, detail)` | `security.log` | SUPR escalations, KSA prompts, grants, denials |
| `LogCrash(summary, detail)` | `crash/<n>.log` | kernel panics and minidumps |
| `LogAppEvent` | `apps/<app>` | per-app logs |
| `LogProcessEvent` | `processes/<name>` | per-kernel-process logs |

`InitFilesystem()` creates the tree at boot.

## MirrorSerial is deferred, and that matters

`MirrorSerial` runs from any context, including timer IRQs and, critically, the `#PF`/`#GP`
exception-dump path. It used to append the line into KVFS synchronously, which meant a heap
realloc. The log, VFS and heap locks are all CPU-owner recursive, so a fault taken while one of
them was already held on the same core re-entered a half-mutated KVFS tree and heap freelist,
and wrote log text over live blocks. That was the root cause of a family of "register dump full
of `serial.l` / `/kurono/` ASCII" corruptions, and of `#PF` dump, nested fault, triple-fault
reboot cascades.

`MirrorSerial` now only stages text into a fixed static ring. No KVFS, no heap, safe from every
context. The `LoggingProcess` kernel-process is the only thing that moves the ring into KVFS,
from safe process context, via `FlushSerialMirror()` on its periodic tick.

Rule: never add serial logging to an exception path or a heap-internal path without that
deferral.

## Where the events come from

- network: `src/net/tcpip.cpp` calls `LogNetwork` when a TCP connection reaches ESTABLISHED,
  and when it sees a RST
- security: `SUPR::Log` mirrors every privilege decision into `LogSecurity`, with the
  requesting username
- crash: the panic and crash-recovery path writes a `LogCrash` record and drops an
  `emergency.txt` under `KP_LOG_DIR`
- boot and system: milestones through bring-up

## Reading them

`dmesg` in the shell prints `/kurono/var/log/serial.log`. The rest can be read with the normal
file tools or browsed in the file manager. They survive reboot through KFS persistence, since
the user-data subtrees get mirrored to disk.

## Files

- `src/system/kpaths.h` the path layout, authoritative
- `src/system/logging.cpp` / `.h` the implementation
- `src/net/tcpip.cpp`, `src/security/supr.cpp`, `src/kernel/kurono_kernel.cpp` the log producers
