# Conduit

`src/system/conduit.cpp` and `src/system/conduit.h` implement Conduit. (Note:
Conduit lives entirely under `src/system/`  -  there is no `src/apps/conduit.*`.)

## 1. What Conduit is

Conduit is an **event / telemetry bridge** (`ConduitBridge`) plus the GUI viewer
that displays its events. It is not a guest terminal  -  it observes the live
system and surfaces a running dialogue of what's happening: boot/shutdown,
package operations, GPU rendering, guest-OS switches, driver activity, and
generic shell commands.

## 2. The event model

`ConduitBridge` keeps a fixed ring of `ConduitEvent` records (`CONDUIT_MAX_EVENTS`),
each with a sequence number, a `ConduitEventType`, the relevant Linux guest
profile, and a short summary/detail. The event types (`ConduitEventType` in
`conduit.h`) are:

`SYSTEM_BOOT`, `BOOT_SEQUENCE`, `SHUTDOWN`, `PACKAGE_INSTALL`, `PACKAGE_UPDATE`,
`PACKAGE_REMOVE`, `GPU_RENDER`, `WIFI_DRIVER`, `AUDIO_DRIVER`, `RAM_WARNING`,
`NVIDIA_FAULT`, `GUEST_SWITCH`, and `GENERIC_COMMAND`.

## 3. API

| Function | Role |
| --- | --- |
| `ConduitBridge::Init()` | initialize the ring; push the first boot event |
| `PollSystemState()` | sample live system state (guest boot status, etc.) and emit change events |
| `RecordCommand(cmdline)` | classify a shell command (`classify_command`) and record it as an event |
| `Consume(after_seq, out, max)` | pull events newer than a sequence number (the viewer's read path) |
| `GetLatestSeq()` | the most recent sequence number |

`classify_command` inspects a command line and maps it onto an event type (e.g.
`kpkg`/`apt` → a package event, GPU commands → `GPU_RENDER`, a guest launch →
`GUEST_SWITCH`).

## 4. Related files

- `src/system/conduit.cpp` / `.h`  -  `ConduitBridge` event ring + classifier
- `src/virt/hypervisor.cpp`  -  guest boot state polled for `GUEST_SWITCH` events
- `src/packages/pkgmgr.cpp`  -  package operations that produce package events
- `src/shell/shell.cpp`  -  calls `RecordCommand` to log executed commands
