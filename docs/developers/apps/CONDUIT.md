# Conduit

`src/system/conduit.cpp` and `conduit.h`. Note it lives in `src/system/`, there is no
`src/apps/conduit.*`.

Conduit is an event and telemetry bridge (`ConduitBridge`) plus the GUI viewer that
shows the events. Not a guest terminal. It watches the live system and surfaces a
running dialogue: boot and shutdown, package operations, GPU rendering, guest-OS
switches, driver activity, plain shell commands.

## Event model

`ConduitBridge` keeps a fixed ring of `ConduitEvent` records (`CONDUIT_MAX_EVENTS`).
Each has a sequence number, a `ConduitEventType`, the Linux guest profile it relates
to, and a short summary and detail.

Event types (`ConduitEventType` in `conduit.h`): `SYSTEM_BOOT`, `BOOT_SEQUENCE`,
`SHUTDOWN`, `PACKAGE_INSTALL`, `PACKAGE_UPDATE`, `PACKAGE_REMOVE`, `GPU_RENDER`,
`WIFI_DRIVER`, `AUDIO_DRIVER`, `RAM_WARNING`, `NVIDIA_FAULT`, `GUEST_SWITCH`,
`GENERIC_COMMAND`.

## API

| Function | Role |
| --- | --- |
| `ConduitBridge::Init()` | set up the ring, push the first boot event |
| `PollSystemState()` | sample live state (guest boot status and so on), emit change events |
| `RecordCommand(cmdline)` | classify a shell command with `classify_command`, record it |
| `Consume(after_seq, out, max)` | pull events newer than a sequence number, the viewer's read path |
| `GetLatestSeq()` | latest sequence number |

`classify_command` reads a command line and maps it to an event type. `kpkg` and `apt`
become package events, GPU commands become `GPU_RENDER`, a guest launch becomes
`GUEST_SWITCH`.

## Files

- `src/system/conduit.cpp` / `.h` the `ConduitBridge` ring and classifier
- `src/virt/hypervisor.cpp` guest boot state, polled for `GUEST_SWITCH`
- `src/packages/pkgmgr.cpp` package operations that raise events
- `src/shell/shell.cpp` calls `RecordCommand` for executed commands
