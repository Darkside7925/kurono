# kinit - Service & Init Manager

`src/system/kinit.{h,cpp}`, `src/system/kinit_kservice.cpp`,
`src/system/kpkg_daemon.{h,cpp}`, and `src/system/kdaemons.{h,cpp}` implement
**kinit**, Kurono's service and init manager (its answer to systemd). kinit
isolates background work so the desktop never blocks, sequences services by
dependency through boot targets, and supervises them with crash-restart backoff.

## 1. What kinit actually is (read this first)

Kurono is **not** Linux. Its core services already run as in-kernel
kernel-processes and in-kernel servers, brought up by the kernel before kinit
exists:

| Service | What it really is | Source |
| --- | --- | --- |
| `klog` (logging) | `RuntimeLog` + the logging kernel-process | `src/system/logging.cpp` |
| `knet` (network) | the network kernel-process + TCP/IP stack | `src/proc/kernel_processes.cpp`, `src/net/tcpip.cpp` |
| `kdbus` (D-Bus) | the AF_UNIX session-bus daemon | `src/system/dbus_server.cpp` |
| `kwayland` (compositor) | the Wayland server + the GUI kernel-process | `src/ui/wayland_server.cpp` |
| `kaudio` (audio) | the audio-server / mixer pump kernel-process | `src/drivers/audio_server.cpp` |

So kinit does **not** rewrite these into separate Linux processes. It supervises
**two honestly-distinct unit kinds**:

- **`KUNIT_INKERNEL`** - an existing in-kernel subsystem. kinit *adopts* it as an
  already-running unit: it tracks state, runs a periodic **health probe**, and
  can restart it via a hook. It is **not** a separate address space, and the code
  + this doc never pretend otherwise.
- **`KUNIT_PROCESS`** - a genuine isolated Linux user process spawned via
  fork/exec (`ld-kurono` `ExecPIE`). Each gets its own supervisor
  kernel-process (see §5). This is the only kind that is a real separate address
  space.

This split is deliberate: it gives a working, non-regressing init *today* while
leaving a clean path to fuller process isolation as the dynamic-exec runtime
matures.

## 2. Boot targets

Five dependency-sequenced stages. Each `StartTarget` brings up that stage's
services once their `After=` dependencies are *settled* (a daemon is `running`,
a oneshot is `stopped`):

```
kernel.target   → klog
network.target  → knet            (After=klog)
dbus.target     → kdbus           (After=knet, critical)
desktop.target  → kwayland        (After=kdbus, critical)
                  kaudio          (After=kdbus)
user.target     → kpkg-daemon     (After=kdbus knet)
                  kupdate         (After=kpkg-daemon)
                  ksecurity       (After=kdbus)
```

`KInit::Boot()` runs the five `StartTarget` calls in order, then spawns the crash
monitor. It is invoked from `kernel_main` right after `KernelProcesses::SpawnAll`
and before `Scheduler::Start()`, so every supervisor/monitor/worker
kernel-process joins the scheduler.

## 3. `.kservice` files

Unit files live in **`/kurono/system/services/*.kservice`** (INI-style). The
parser (`kinit_kservice.cpp`) is allocation-free and bounds every copy; an
unknown key or malformed line is skipped, never rejected.

```ini
[Service]
Name=kpkg-daemon
Description=kurono package install daemon
Exec=/kurono/system/bin/kpkg-daemon
Restart=on-failure          ; no | on-failure | always
RestartDelay=2000           ; base backoff in ms
After=dbus network          ; space-separated dependency names
WantedBy=user.target        ; maps to a boot target
Critical=no                 ; yes → desktop notification on failure
Type=oneshot                ; optional; default is a long-running daemon
[Capabilities]
Network=yes
Filesystem=yes
GUI=no
```

`WantedBy` accepts `<stage>.target` or a bare `<stage>`. `LoadServiceDir()` scans
the directory at `Init()` and on `kinit reload`. Names already registered as
built-ins are deduped, so the seeded files act as documentation without
double-registering.

> **Honesty note on the seeded units.** The seeded `kpkg-daemon.kservice` /
> `kupdate.kservice` describe the *eventual* fully-isolated process form
> (`Exec=/kurono/system/bin/...`). Today those three user.target services run as
> in-kernel workers (§6), which is what kinit registers and reports. The
> `.kservice` files carry a comment saying exactly this.

## 4. Crash monitor & restart backoff

The `kinit-monitor` kernel-process calls `KInit::Tick()` ~4×/sec:

- **Process units:** when a supervisor reports an exit, a nonzero code is a
  crash. `Restart=always` relaunches on any exit; `on-failure` only on a crash.
- **Exponential backoff:** delays start at `RestartDelay` (default 2000 ms) and
  **double each crash - 2s, 4s, 8s ... capped at 60s**. A manual `kinit restart`
  resets the backoff.
- **5-in-60s rule:** a rolling 60s window counts crashes. **≥ 5 crashes in the
  window → the service is marked `failed`** and kinit stops relaunching it (an
  alert is logged).
- **Critical services:** a `failed` *critical* service (kdbus / kwayland) raises a
  desktop notification via `NotificationManager::Post`.
- **In-kernel health probes:** a `KUNIT_INKERNEL` unit whose `health_fn()`
  returns false is treated as a crash and run through the same backoff machinery
  (its hook re-runs on relaunch).

## 5. How a process unit is spawned (the blocking problem)

`Userspace::RunProcessWithArgs` **blocks its calling kernel stack** until the
user process exits (it longjmps back on exit). A long-lived daemon can't be
launched inline from the monitor without wedging it. So each process unit gets a
dedicated **supervisor kernel-process** (`ksup:<name>`):

1. `StartService` runs the **capability gate** (§7), then sets `relaunch = true`
   on the unit's supervisor slot.
2. The supervisor loads the ELF (`ElfLoader::LoadELF64FromVFS`) and calls
   `RunProcessWithArgs`. The preemptive scheduler time-shares the user process
   against the GUI, so **the desktop keeps rendering** while it runs.
3. When the process exits/crashes, `RunProcessWithArgs` returns; the supervisor
   records the exit code, and `Tick()` applies the restart policy.

If the binary is missing, the supervisor logs `spawn-missing` and the unit fails
out gracefully rather than pretending to run.

## 6. The service daemons

- **kpkg-daemon** (`kpkg_daemon.cpp`) - the concrete win. The package
  download+extract loop used to run inline on whatever thread invoked
  `kpkg install`, blocking a GUI-initiated install. kpkg-daemon moves it into a
  dedicated worker kernel-process: `RequestInstall(pkg)` is **non-blocking** (a
  one-slot request ring), the worker calls `PackageManager::Install`, and a
  polled `JobStatus` + a `org.kurono.Pkg` D-Bus `Progress` signal report
  progress. Route to it from the shell with **`kpkg install --daemon <pkg>`**
  (or `--bg`); the foreground form still runs inline for the CLI.
- **kupdate** (`kdaemons.cpp`) - polls `PackageManager::GetPendingUpdateCount`
  every 60s, toasts when updates appear; `kupdate check` forces a sync.
- **ksecurity** (`kdaemons.cpp`) - re-runs `SUPR::PolicySelfTest` +
  `KSA::SelfTest` every 30s; a self-test failure alerts and logs to
  `security.log`, and drives the unit's health probe.

All three run as worker kernel-processes (never blocking the GUI). kpkg-daemon's
worker is what the GUI talks to; the matching process-unit declaration is for the
future isolated form.

## 7. Capability enforcement at spawn

The `[Capabilities]` section (`Network` / `Filesystem` / `GUI`) is enforced for
process/oneshot units in `capability_gate()`: a unit that requests a privileged
capability (network or filesystem) requires at least a logged-in SUPR user
session (`SUPR::GetCurrentSession` / `GetLevel`). A guest or disabled session is
denied. Every decision is logged to `services.log` **and** `security.log`. (For
in-kernel units the grants are recorded but not enforced - those already run
privileged inside the kernel.)

## 8. Audit logging

Every start/stop/crash/restart/relaunch/failure goes to
**`/kurono/var/log/services.log`** via `KInit::LogEvent` (also mirrored to
serial), formatted `<uptime_ms> <event> <service> <detail>`. kinit also feeds
`RuntimeLog::LogCrash` / `LogSecurity` for the canonical crash/security logs.

## 9. Shell commands

```
kinit status                 # overview + per-target service table
kinit start    <svc>
kinit stop     <svc>
kinit restart  <svc>
kinit enable   <svc>
kinit disable  <svc>
kinit logs    [svc]          # dump services.log (optionally filtered)
kinit reload                 # re-parse /kurono/system/services
```

Plus `kpkg-daemon status|install <pkg>`, `kupdate [check]`, and `ksecurity`.

## 10. On download throughput (deliberately *not* claimed)

A common misconception is that slow (~0.5 MB/s) package downloads were caused by
the GUI/install loop blocking. **They were not.** The pkgmgr download loop
already gates its UI pump to ~10 fps and that did not change the rate. kpkg-daemon
buys **process isolation** (the GUI never blocks during a download) - it does
**not** raise MB/s.

The real throughput cap is the download path itself: in the QEMU dev environment
it is most likely SLIRP user-mode networking, plus `KVFS::WriteFile` / tar-extract
speed for large artifacts. `TCPStack::Recv` (`src/net/tcpip.cpp`) drains a 64 KB
RX ring and the advertised window is a 16-bit field clamped to 64 KB; the
byte-by-byte ring copy in `Recv` is a memcpy-equivalent and is not itself the
bottleneck. **No speedup has been measured or is claimed here.** Raising the rate
is a separate, measure-first task (window size, KVFS large/incremental-write
behavior, host networking).

## 11. Files

| File | Role |
| --- | --- |
| `src/system/kinit.h` / `kinit.cpp` | core: model, targets, spawn, crash monitor, shell |
| `src/system/kinit_kservice.cpp` | `.kservice` parser + directory loader |
| `src/system/kpkg_daemon.{h,cpp}` | background package install worker + D-Bus progress |
| `src/system/kdaemons.{h,cpp}` | kupdate + ksecurity worker daemons |
| `/kurono/system/services/*.kservice` | on-disk unit files (seeded + user-authored) |
| `/kurono/var/log/services.log` | audit log |
