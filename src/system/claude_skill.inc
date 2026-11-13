// generated from the kurono skill text (see claude_host.cpp). laid at /home/user/.claude/skills/kurono/SKILL.md (satoru)
static const char kKuronoSkill[] = R"SKILL(
---
name: kurono
description: Use when inspecting, diagnosing or repairing Kurono OS itself, the operating system this session runs on. Covers the kernel log, drivers, services, audio, display, GPU, USB, network, storage, persistence, and how to save work.
---

# Kurono OS: inspect, diagnose, repair

## What this machine is

Kurono is its own x86_64 operating system with its own kernel and desktop, not Linux. `uname` says Linux and `/proc` exists only because Kurono implements the Linux syscall ABI so unmodified Linux programs (including you) run. When asked what OS this is, the answer is Kurono.

Your Bash tool is a normal Linux userland on top of that ABI: a real bash 5.2, busybox 1.37 (all applets on PATH), git 2.47 (static), wget with HTTPS, a curl front end, tar/gzip/unzip/diff/patch, vi, less. There is no python3, no C compiler and no package manager for Linux software.

## Running Kurono's own commands

Kurono's built-in shell commands are not Linux binaries. They run through `/dev/kshell`, and three spellings reach them:

    kcmd <command> [args...]        # generic, always works
    ksh-run <command> [args...]     # same client
    <command> [args...]             # direct name for the common ones

The direct names on PATH are: `ksys kdrv kinit kctl audio recovery persisttest gpu gpuprobe gpuaux gpumode gpuaccel display sysinfo hwinfo drivers lsblk lspci lsusb lscpu dmidecode hwfw iotop ss nic journal ver version tree fls fcat codecs vgpu screenshot help usermode ken` (`wifi` and `nmcli` exist only when a wireless driver registered them).

Names busybox also provides (`ip`, `ifconfig`, `lsmod`, `modinfo`, `mount`, `blkid`, `ping`, `wget`) keep their Linux behaviour on PATH; reach Kurono's version with `kcmd <name>`. A command may refuse shell syntax: `/dev/kshell` runs one command line, no pipes or redirection. Pipes belong in bash: `ksys log 80 | grep -i hda`.

Privileged commands ask the desktop operator for approval. In a headless session the prompt cannot be answered, so a state-changing command returns "administrator approval denied". Read-only commands do not prompt. Do not loop on a denied command.

## First look: health and logs

- `ksys health` one screen: uptime, CPUs, memory, NIC/DHCP, audio, USB, storage, fault counts, verdict.
- `ksys log [N]` last N lines of the live kernel log (default 40, max 400).
- `dmesg` the same live kernel log from Linux (`dmesg | tail -40`, `dmesg | grep -i -E 'hda|e1000|usb|panic'`).
- `journal [N] [path]` tail a KVFS log (default `/kurono/var/log/system.log`).
- `ksys hw` CPU, GPU, RAM, PCI and USB inventory. `ksys mem` physical memory and heap. `ksys net` NIC counters and IP. `ksys procs` process list. `ksys persist` shows where the KFS store lives and whether the persistence marker is present.
- `ksys log` lines are tagged: `[usrflt]` a contained ring-3 fault, `[STKCORRUPT]`, `PANIC`, `[E1000]`/`[igc]` network, `[hda]` audio, `[usbaudio]`, `[igpu]`, `[nvme]`.

Crash records: `/kurono/var/log/crash/crash.log` (append-only panic records) and `/kurono/var/crash/last.dmp` after a kernel panic (the kernel's `/var` is `/kurono/var`). Read them with `cat`/`tail` (they are inside the KURONO tree, no admin approval needed to read).

## Drivers

`kdrv list` shows every driver with state, category, allowed operations and bound device. `kdrv list all` adds kinit services and the kdf crash sandbox. `kdrv info <name>` explains a driver and its protections. `kdrv reload <name>` restarts it, `kdrv start <name>` brings up a stopped one, `kdrv unload <name>` stops one.

Reloadable: `nic`, `audio`, `usb-hid`. `kdrv unload nic` and `kdrv unload usb-hid` are refused for you on purpose: they would cut your own network or input. Protected and always refused: `storage` (NVMe/SATA holding KURONO.LOG and KURONO.KFS), `display`, `timer`, `interrupts`, `usb-host`, `ps2-input`.

## Services

`kinit status` lists units with state, kind and description. `kinit logs [svc]` tails the service log. `kinit analyze` shows boot timing and the dependency graph. `kinit restart <svc>`, `kinit stop <svc>`, `kinit start <svc>`, `kinit enable|disable <svc>` change state and need desktop approval.

`kctl` is the short service-control spelling: `kctl list`, `kctl restart audio`, `kctl stop <svc>`, `kctl logs [svc]`. It maps onto kinit.

Core services (`kernel`, `scheduler`, `klog`, `nvme`, `knet`, `gui`, `desktop`, `kvfs`) must not be stopped; killing a kernel service is refused with EPERM anyway. Restart only a leaf service you are diagnosing.

## Audio

1. `audio status` names the backend (HDA, USB or pcspk) and the codec/stream state.
2. No backend: `ksys hw` and `lspci` to see whether a controller exists. `dmesg | grep -i -E 'hda|audio'`.
3. `audio restart` re-runs controller and codec bring-up. `audio tone` plays a short 440 Hz test tone.
4. USB headset: `audio use usb`; internal codec: `audio use hda`. `kdrv list` shows the audio driver state.
If `audio status` says `absent` and `ksys hw` lists no controller, the hardware is not there; say so instead of retrying.

## Display and GPU

- `display` lists outputs, modes and the active pipe. `gpu` is the short GPU summary.
- `gpuprobe` is a read-only Intel display register report. `gpuaux` reads DPCD/EDID of attached sinks. `gpumode` controls the external DP pipe (`status`, `light [hz]`, `rate HZ`, `off`). `gpuaccel status|on|off|test` controls Intel display acceleration.
- These are hardware-specific; on a machine without the matching GPU they report absent. Do not try to force a mode on hardware that is not present.

## USB and input

`lsusb` lists devices, `kdrv info usb-host` and `kdrv info usb-hid` show the host controller and input driver. `ksys hw` includes the USB tree. If the keyboard or mouse stops, `kdrv reload usb-hid` (approval) is the safe action; do not unload `usb-host` or `ps2-input`.

## Network

1. `ksys net` shows link, IP, gateway and counters. `ifconfig`/`ip` are the Linux views (`kcmd ifconfig` is Kurono's).
2. Link down or no IP: `nic status`, then `kdrv reload nic` (or `nic restart`), then `ksys net` again.
3. `wifi status` and `nmcli` cover wireless when a wireless driver is present. `ping` and `nslookup` test reachability and DNS.
4. `dmesg | grep -i -E 'e1000|igc|wifi|dhcp'` for driver errors.
Do not run `kdrv unload nic`; it cuts your own network.

## Storage and persistence

- `lsblk`, `blkid`, `fdisk -l` (Kurono's `kcmd fdisk`) show disks and partitions. `df` and `mount` are the Linux views.
- The KURONO partition holds `KURONO.LOG` (kernel log mirror), `KURONO.KFS` (persisted `/home`, `/etc`, `/root`) and the package files. The write guard blocks writes outside the KURONO partition; a failed write to `/dev/sda1` or another partition is usually that guard, not a bad disk.
- `/home/user` survives reboots: the tree is saved to KURONO.KFS when the session ends. `persisttest` exercises the save/restore path (safe: it writes `/home/user/.persisttest` and flushes the KFS volume). `list` shows installed packages, `pkginfo <pkg>` and `search <term>` query them; `kpkg install <pkg>` needs approval.
- Never write to a raw disk, never run `mkfs`/`fsck` on a mounted or unknown partition, and never touch `/dev/nvme0n1` or `/dev/sda` directly. Recovery tools (ntfsfix, e2fsck, testdisk) exist but are operator tools; ask before running them.

## Processes and threads

`ps` is busybox ps over `/proc`; `ps -ef`, `ps -o pid,ppid,args` work. `top` is live. Kernel services appear as `[name]` with no cmdline, exactly like Linux kernel threads; `kill` on them returns EPERM, not "No such process". `kill <pid>` on a normal process works, `kill -9` is allowed for a process you own. `/proc/<pid>/status|stat|cmdline|comm|cwd|exe|fd` are real. `ksys procs` is the kernel's own view.

## Saving your work

Write files under `/home/user` (for example `/home/user/projects/<name>`), not under `/system` or `/tmp`, if they must survive a reboot. `/tmp` is scratch. Use `git init`, `git add`, `git commit` in a local directory to keep history; the git here has no HTTPS transport, so `git clone` works for local paths and file:// URLs only. Fetch remote files with `wget` (or `curl`, a small front end over it). busybox's TLS does not validate certificates: do not send credentials over an untrusted link.

## Limits

- No `systemctl`, `modprobe`, `dmesg -w`, `/sys` tuning or ALSA. Kurono has its own commands above.
- No compiler and no python3. Do not try to build or pip install anything.
- Do not launch `claude` or `firefox` from the Bash tool; you are already inside the session and the launch would block it.
- One `/dev/kshell` command per call: no `;`, `&&`, `|`, `>` or `$` inside a kcmd argument. Run the pipeline in bash and pipe the output.
- If a command has no effect, say so plainly and report the log lines. Do not retry in a loop.
)SKILL";

// end (satoru)
