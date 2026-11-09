// generated from the kurono skill text (see claude_host.cpp). laid at /home/user/.claude/skills/kurono/SKILL.md (satoru)
static const char kKuronoSkill[] = R"SKILL(
---
name: kurono
description: Use when inspecting, diagnosing or repairing Kurono OS itself, the operating system this session is running on. Covers listing, reloading and starting drivers, reading kernel logs, checking hardware, memory, network and audio, and fixing a missing network, sound or device.
---

# Kurono OS: inspect, diagnose, repair

## What Kurono is

Kurono is its own operating system with its own kernel, written from scratch for x86_64. It is not Linux. `uname` says Linux and `/proc` exists only because Kurono implements the Linux syscall ABI so that programs like you can run. When someone asks what OS this is, the answer is Kurono.

Because of that, normal Linux repair tricks do not apply: no `modprobe`, no `systemctl`, no `dmesg` from Linux, no `/sys` tuning. Kurono has its own commands, listed below.

## How to run Kurono commands

Kurono's own commands belong to its built-in shell and are not in your Bash tool's PATH. You reach them through the device `/dev/kshell`: write one command line, read back the output. Use a fresh open per command:

```
exec 3<>/dev/kshell; echo "ksys health" >&3; busybox cat <&3; exec 3>&-
```

or with busybox: `busybox sh -c 'exec 3<>/dev/kshell; echo "kdrv list" >&3; busybox cat <&3'`.

Common tools are not on PATH by name in your Bash tool. Call them through busybox: `busybox cat`, `busybox grep`, `busybox tail`, `busybox head`, `busybox ls`, `busybox find`, `busybox sed`, `busybox awk`, `busybox wc`, `busybox sort`, `busybox ps`, `busybox env`. If `busybox cat` prints nothing for a file, use the Read tool instead and mention it.

Rules of the gate: one bare command name plus arguments. No pipes, no `;`, `&&`, `>` or `$`, no path-qualified binaries. stderr is already in the output. A refused line prints the list of allowed commands.

## Commands

Diagnostics (read only):

- `ksys health` one-screen summary: uptime, cpus, memory, nic and dhcp, audio, usb, storage, fault counts, a verdict line. Start here.
- `ksys log [N]` last N lines of the kernel log (default 40, max 400). Look for `[usrflt]`, `[STKCORRUPT]`, `PANIC`, `[E1000]`, `[hda]`.
- `ksys hw` CPU, GPU, RAM, PCI and USB devices.
- `ksys procs` process list. `ksys mem` physical memory and heap. `ksys net` nic counters, ip, gateway, interfaces.
- Also allowed: `lsblk`, `blkid`, `df`, `lspci`, `lsusb`, `lsmod`, `drivers`, `dmesg`, `uptime`, `ps`, `ip`, `ifconfig`, `help`.

Drivers:

- `kdrv list` every driver with state (running, stopped, absent, active), category, allowed actions and the bound device. `kdrv list all` adds kinit services and the kdf crash sandbox.
- `kdrv info <name>` details, why something is protected, kdf crash count.
- `kdrv reload <name>` restart a driver. `kdrv unload <name>` stop it. `kdrv start <name>` bring up a stopped one.
- `nic status|restart` and `audio status|restart|tone` are the older per-subsystem commands and still work.

Examples:

```
kdrv list
kdrv reload nic
kdrv info storage
ksys log 60
audio restart
```

## What you can and cannot change

- Reloadable: `nic` (ethernet), `audio` (Intel HDA or USB audio), `usb-hid` (USB keyboard and mouse input).
- `kdrv unload nic` and `kdrv unload usb-hid` are refused for you on purpose. They would cut your own network or input and you could not undo it. Use `reload`. The operator can unload them at the console.
- Protected, always refused: `storage` (NVMe and SATA holding KURONO.LOG and KURONO.KFS), `display`, `timer`, `interrupts`, `usb-host`, `ps2-input`. Do not look for a way around this.
- There is no loader for new driver binaries. `start` only brings up a built-in driver that is present but stopped.
- Nothing here writes to disks, NIC firmware or NVM, or reformats. On real hardware a write guard blocks writes outside the KURONO partition.
- If a command has no effect, say so plainly. Do not retry in a loop.

## Troubleshooting playbook

No network:
1. `ksys net` and `ksys health`. Look at link, ip, and whether dhcp is bound.
2. Link down or no ip: `kdrv reload nic` (or `nic restart`), then `ksys net` again.
3. Still nothing: `ksys log 80` and look for `[E1000]` or `[igc]` lines, and report them.

No sound:
1. `audio status`. Is a backend named? If `none`, check `ksys hw` for an audio controller.
2. `audio restart` (or `kdrv reload audio`), then `audio tone`.
3. USB audio device? `audio use usb` or `audio use hda` picks the backend.

A device is missing:
1. `kdrv list` and `lspci` / `lsusb`. Is the driver absent, or stopped?
2. Stopped: `kdrv start <name>`. Absent: the hardware was not found, check `ksys log 80`.

An app crashed or the desktop is slow:
1. `ksys health` for fault counts. `ksys log 80` and find `[usrflt]` (a ring-3 fault was contained) or `[STKCORRUPT]`.
2. `ksys procs` and `ksys mem` for a runaway process or low memory.
3. Report the exact log lines. A contained ring-3 fault does not mean the kernel is broken.

## Style

Check state before and after any change (`kdrv list`, `ksys health`). Say what you changed and what happened. If the kernel itself looks wrong, describe it and stop; do not guess at fixes you cannot make.
)SKILL";

// end (satoru)
