# kinit

`src/system/kinit.{h,cpp}`, `kinit_kservice.cpp`, `kpkg_daemon.{h,cpp}` and `kdaemons.{h,cpp}`.
kinit is the service and init manager, Kurono's answer to systemd. It isolates background work so
the desktop never blocks, sequences services by dependency through boot targets, and supervises
them with crash-restart backoff.

## What it actually is (read this first)

Kurono is not Linux. The core services already run as in-kernel kernel-processes and in-kernel
servers, brought up by the kernel before kinit exists:

| Service | What it really is | Source |
| --- | --- | --- |
| `klog` (logging) | `RuntimeLog` plus the logging kernel-process | `src/system/logging.cpp` |
| `knet` (network) | the network kernel-process plus the TCP/IP stack | `src/proc/kernel_processes.cpp`, `src/net/tcpip.cpp` |
| `kdbus` (D-Bus) | the AF_UNIX session-bus daemon | `src/system/dbus_server.cpp` |
| `kwayland` (compositor) | the Wayland server plus the GUI kernel-process | `src/ui/wayland_server.cpp` |
| `kaudio` (audio) | the audio-server and mixer pump kernel-process | `src/drivers/audio_server.cpp` |

So kinit does not rewrite those into separate Linux processes. It supervises two honestly
distinct unit kinds:

- `KUNIT_INKERNEL`: an existing in-kernel subsystem. kinit adopts it as an already-running unit,
  tracks state, runs a periodic health probe, and can restart it through a hook. It is not a
  separate address space, and neither the code nor this doc pretends otherwise.
- `KUNIT_PROCESS`: a genuine isolated Linux user process spawned through fork/exec (ld-kurono
  `ExecPIE`). Each one gets its own supervisor kernel-process. This is the only kind that is a
  real separate address space.

The split is deliberate: a working, non-regressing init today, with a clean path to fuller
process isolation as the dynamic-exec runtime matures.

## Boot targets

Five dependency-sequenced stages. Each `StartTarget` brings up that stage's services once their
`After=` dependencies are settled, meaning a daemon is `running` and a oneshot is `stopped`:

```
kernel.target   -> klog
network.target  -> knet            (After=klog)
dbus.target     -> kdbus           (After=knet, critical)
desktop.target  -> kwayland        (After=kdbus, critical)
                   kaudio          (After=kdbus)
user.target     -> kpkg-daemon     (After=kdbus knet)
                   kupdate         (After=kpkg-daemon)
                   ksecurity       (After=kdbus)
```

`KInit::Boot()` runs the five `StartTarget` calls in order, then spawns the crash monitor. It is
invoked from `kernel_main` right after `KernelProcesses::SpawnAll` and before
`Scheduler::Start()`, so every supervisor, monitor and worker kernel-process joins the scheduler.

## .kservice files

Unit files live in `/kurono/system/services/*.kservice`, INI style. The parser
(`kinit_kservice.cpp`) is allocation-free and bounds every copy. An unknown key or malformed line
gets skipped, never rejected.

```ini
[Service]
Name=kpkg-daemon
Description=kurono package install daemon
Exec=/kurono/system/bin/kpkg-daemon
Restart=on-failure          ; no | on-failure | always
RestartDelay=2000           ; base backoff in ms
After=dbus network          ; space-separated dependency names
WantedBy=user.target        ; maps to a boot target
Critical=no                 ; yes, desktop notification on failure
Type=oneshot                ; optional, the default is a long-running daemon
[Capabilities]
Network=yes
Filesystem=yes
GUI=no
```

`WantedBy` accepts `<stage>.target` or a bare `<stage>`. `LoadServiceDir()` scans the directory at
`Init()` and on `kinit reload`. Names already registered as built-ins are deduped, so the seeded
files act as documentation without double-registering.

A note on the seeded units. The seeded `kpkg-daemon.kservice` and `kupdate.kservice` describe the
eventual fully-isolated process form (`Exec=/kurono/system/bin/...`). Today those three user.target
services run as in-kernel workers, which is what kinit registers and reports. The `.kservice` files
carry a comment saying exactly this.

## Crash monitor and restart backoff

The `kinit-monitor` kernel-process calls `KInit::Tick()` about four times a second:

- process units: when a supervisor reports an exit, a nonzero code is a crash. `Restart=always`
  relaunches on any exit, `on-failure` only on a crash.
- exponential backoff: delays start at `RestartDelay`, default 2000ms, and double each crash,
  2s, 4s, 8s, capped at 60s. A manual `kinit restart` resets the backoff.
- the 5-in-60s rule: a rolling 60s window counts crashes. Five or more in the window marks the
  service `failed` and kinit stops relaunching it, with an alert logged.
- critical services: a `failed` critical service (kdbus, kwayland) raises a desktop notification
  through `NotificationManager::Post`.
- in-kernel health probes: a `KUNIT_INKERNEL` unit whose `health_fn()` returns false counts as a
  crash and goes through the same backoff machinery, with its hook re-run on relaunch.

## How a process unit is spawned

`Userspace::RunProcessWithArgs` blocks its calling kernel stack until the user process exits, and
longjmps back on exit. A long-lived daemon can't be launched inline from the monitor without
wedging it. So every process unit gets its own supervisor kernel-process, `ksup:<name>`:

1. `StartService` runs the capability gate, then sets `relaunch = true` on the unit's supervisor
   slot.
2. The supervisor loads the ELF (`ElfLoader::LoadELF64FromVFS`) and calls `RunProcessWithArgs`.
   The preemptive scheduler time-shares the user process against the GUI, so the desktop keeps
   rendering while it runs.
3. When the process exits or crashes, `RunProcessWithArgs` returns, the supervisor records the
   exit code, and `Tick()` applies the restart policy.

If the binary is missing, the supervisor logs `spawn-missing` and the unit fails out gracefully
instead of pretending to run.

## The daemons

- kpkg-daemon (`kpkg_daemon.cpp`), the concrete win. The package download and extract loop used to
  run inline on whatever thread called `kpkg install`, blocking a GUI-initiated install.
  kpkg-daemon moves it into a dedicated worker kernel-process: `RequestInstall(pkg)` is
  non-blocking (a one-slot request ring), the worker calls `PackageManager::Install`, and a polled
  `JobStatus` plus an `org.kurono.Pkg` D-Bus `Progress` signal report progress. Route to it from
  the shell with `kpkg install --daemon <pkg>`, or `--bg`. The foreground form still runs inline
  for the CLI.
- kupdate (`kdaemons.cpp`), polls `PackageManager::GetPendingUpdateCount` every 60s and toasts
  when updates appear. `kupdate check` forces a sync.
- ksecurity (`kdaemons.cpp`), re-runs `SUPR::PolicySelfTest` and `KSA::SelfTest` every 30s. A
  self-test failure alerts, logs to `security.log`, and drives the unit's health probe.

All three run as worker kernel-processes and never block the GUI. kpkg-daemon's worker is what the
GUI talks to, and the matching process-unit declaration is for the future isolated form.

## Capability enforcement at spawn

The `[Capabilities]` section (`Network`, `Filesystem`, `GUI`) is enforced for process and oneshot
units in `capability_gate()`: a unit that asks for a privileged capability (network or filesystem)
needs at least a logged-in SUPR user session (`SUPR::GetCurrentSession`, `GetLevel`). A guest or
disabled session is denied. Every decision goes to `services.log` and `security.log`. For in-kernel
units the grants are recorded but not enforced, since those already run privileged inside the
kernel.

## Audit logging

Every start, stop, crash, restart, relaunch and failure goes to `/kurono/var/log/services.log`
through `KInit::LogEvent`, also mirrored to serial, formatted
`<uptime_ms> <event> <service> <detail>`. kinit also feeds `RuntimeLog::LogCrash` and
`LogSecurity` for the canonical crash and security logs.

## Shell commands

```
kinit status                 # overview + per-target service table
kinit start    <svc>
kinit stop     <svc>
kinit restart  <svc>
kinit enable   <svc>
kinit disable  <svc>
kinit logs    [svc]          # dump services.log, optionally filtered
kinit reload                 # re-parse /kurono/system/services
```

Plus `kpkg-daemon status|install <pkg>`, `kupdate [check]` and `ksecurity`.

## On download throughput

A common misconception is that slow (~0.5 MB/s) package downloads come from the GUI or install loop
blocking. They don't. The pkgmgr download loop already gates its UI pump to about 10 fps and that
didn't change the rate. kpkg-daemon buys process isolation, meaning the GUI never blocks during a
download. It does not raise MB/s.

The real cap is the download path itself: in the QEMU dev environment most likely SLIRP user-mode
networking, plus `KVFS::WriteFile` and tar-extract speed for large artifacts. `TCPStack::Recv` in
`src/net/tcpip.cpp` drains a 64KB RX ring and the advertised window is a 16-bit field clamped to
64KB. The byte-by-byte ring copy in `Recv` is memcpy-equivalent and not itself the bottleneck. No
speedup has been measured or is claimed here. Raising the rate is a separate, measure-first task:
window size, KVFS large and incremental write behaviour, host networking.

## Files

| File | Role |
| --- | --- |
| `src/system/kinit.h` / `kinit.cpp` | core: model, targets, spawn, crash monitor, shell |
| `src/system/kinit_kservice.cpp` | `.kservice` parser and directory loader |
| `src/system/kpkg_daemon.{h,cpp}` | background package install worker plus D-Bus progress |
| `src/system/kdaemons.{h,cpp}` | kupdate and ksecurity worker daemons |
| `/kurono/system/services/*.kservice` | on-disk unit files, seeded and user-authored |
| `/kurono/var/log/services.log` | audit log |
