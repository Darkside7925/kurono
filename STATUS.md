# Kurono status

Last updated November 2026. The README has the short version. This file is the longer one: what's in each release, what the parts of the OS can do right now, and what's still broken.

## Where it is

Kurono boots on real hardware and in QEMU. It reaches a full compositing desktop, runs Firefox 140, Claude Code and OpenCode as stock Linux binaries, keeps your files across reboots and runs Alpine and Debian guests in its own hypervisor. The only real machine it's been tested on is my laptop, a Legion Pro 7 (Core Ultra 7 255HX, 32 GB).

It still isn't a daily driver. Firefox is slow to start, GPU acceleration is behind flags, several things only work in QEMU so far, and some features are held back until their bugs are fixed.

## Part 3: infrastructure and support (November 2026)

This is the groundwork before Kurono Kintsugi, and it isn't the final version.

- OpenCode. The stock `opencode` `linux-x64-musl` build runs: the TUI, `opencode run`, and its file and shell tools. It loads from the `OPENCODE` file on the KURONO partition the same way Claude does (`src/system/opencode_host.cpp`), takes musl's `libstdc++.so.6` and `libgcc_s.so.1` from `LIBSTDC.SO` and `LIBGCCS.SO`, and reads provider keys from `OPENCODE.ENV`.
- The Claude Code freeze. JavaScriptCore stops its threads with a signal and waits on a semaphore until each one answers. A thread sleeping in `FUTEX_WAIT` was only woken by a fatal signal, so the handler never ran and the collector waited forever. A caught signal now ends the wait with `EINTR` (`kurono.futexsig=0` brings back the old behaviour). A 2 hour 50 minute run with 46 tool-heavy prompts had no faults and no freeze.
- Firefox and Claude at the same time. vfork copies a 64 KB window of the parent's stack back on exec. Each chunk was sized by the destination page only, so it read past the end of the source frame, and the window also ran past the top of the stack. Firefox's environment got garbled, ICU lost its data path and Firefox aborted. With the same kernel and only the flag changed: 4 crashes in 12 boots with `kurono.vfcopyfix=0`, 0 in 13 with the fix.
- Everyday Linux fixes. These turned up while running Claude Code on Kurono for hours.
  - sendfile and copy_file_range fall back to read/write. Before, busybox `cat` printed nothing and `cp` made empty files.
  - A blocking pipe write waits for room instead of returning `EAGAIN`, so output past 64 KB isn't lost, and a write with no reader raises `SIGPIPE`.
  - Relative symlinks resolve, and `lstat` and `readlink` see the link itself.
  - Files get real wall-clock times, and `utimensat` keeps the times it's given.
  - `getdents` lists `.` and `..`, and a directory can grow past 512 entries, up to 65,536.
  - Exit sends `SIGCHLD`, and `sigsuspend` sleeps instead of spinning, which fixes `cmd & wait`.
  - `/dev/zero`, `time()` and `gettimeofday()` work for static programs.
  - PIDs count up instead of reusing the one that just exited, and process groups keep their zombies.
  - `fcntl` locks read the whole 64-bit `struct flock` pointer. It was a `uint32`, which broke SQLite's WAL.
  - Each of these has a `kurono.*` flag that brings the old behaviour back.
- Memory. Kernel heap allocations of 256 KB and up come straight from page frames (`kurono.heapdirect=0` reverts it). Fifty minutes into a Claude run, the heap's total is 196 MB instead of 375 MB, and Firefox keeps about 470 MB more free. Task Manager's Virtual Machine row shows the guest's real RAM instead of the host task's 128 KB.
- VMs. Alpine runs on up to 4 parallel vCPUs on machines with 12 or more CPUs (`kurono.vm.smp=1` reverts it). Guest RAM is backed on first touch and handed back through free page reporting, guest disks grow in 64 MB chunk files, and an idle vCPU no longer spins. The Debian 13 guest boots.
- Building Kurono inside Kurono. `vm kloop` builds a kernel inside the Alpine guest (a repeat build takes about 76 s), boots it as a nested Kurono and streams its log to `~/vmshare/kguest.log`. `kurono.vm.pt=BB:DD.F` passes a PCI device through to that guest. That needs VT-d, and GPUs, storage and bridges are refused.
- Hardware and KLD. All 20 CPUs by default on the laptop, x2APIC bring-up for firmware that locks it, and a fix for a 20-CPU deadlock between the log lock and the file and heap locks. Intel Wi-Fi runs in a KLD container with VT-d isolation, and there's a HID-over-I2C touchpad container. `kld reload` swaps a ring-3 driver without a reboot.
- Desktop. `ken` is now `amdo` (`ken`, `supr` and `sudo` still work). KSA has an off switch in Settings > Users & Security > Approval: turning it off needs the password, turning it on doesn't. On the Intel GPU, a blit batch hazard that drew wrong pixels on the real iGPU is fixed, window drags copy about a quarter of the data they used to, and an automated test compares partial redraws with full ones.

## Kintsugi part 2 (November 2026)

I pushed what worked and held back what didn't.

- Apps as separate programs. A small UI toolkit (`src/kui/`) lets Kurono apps run as ring-3 programs in their own address space, drawing through the Wayland server like Firefox does. Calculator, Text Editor, Files, Task Manager, Media Player, the video player, Terminal and Settings all have ring-3 versions (`kurono.r3apps=...`), and the in-kernel ones stay as a fallback.
- New apps and UI. The Text Editor was rewritten with tabs, a file sidebar, syntax highlighting, regex find and replace and multi-level undo. There's a new Control Center (network, volume, brightness, media, power), a reworked Files app, window snapping, Alt+Tab and show desktop.
- One clipboard. Kurono apps, Wayland clients like Firefox, XWayland and Alpine apps share one clipboard, plus the primary selection and a Super+V history.
- `ken` and KSA. Admin actions from ring-3 apps (system paths, raw disks, killing other users' processes, the driver broker, setuid) go through the KSA approval prompt, which runs in an isolated hypervisor context.
- Scheduler. `kurono.sched=2` runs the scheduler across every CPU. poll and epoll no longer spin a whole core while idle, and they return `-EINTR` on a pending signal.
- Real `/proc`. Every task, kernel tasks included, has `/proc/<pid>/` with real CPU times, memory and threads, plus `/proc/stat` and `/proc/diskstats`. `ps`, `top` and the ring-3 Task Manager all read it.
- USB hubs. Hubs are walked now: port power, reset, hot plug, transaction translators for full-speed devices behind high-speed hubs, and nested hubs. Before this, anything behind a hub was invisible. It's new and so far only tested in QEMU.
- Network. DHCP renew and rebind, DNS fallback, TCP retransmit timeouts and a network status page.
- Multi-monitor. The internal panel and a DP monitor both light up on the Intel GPU. A GGTT setup bug had kept the second screen dark.
- Held back: GPU acceleration. A hardware cursor plane, damage-only wallpaper and zero-copy present are in, behind `kurono.igpu.accel=1`, `kurono.wpdamage=1` and `kurono.igpu.zerocopy=1`. They're all off by default. They pass in QEMU but still misbehave on real hardware.

## Earlier in November: first real hardware

- First boots on the laptop. Kurono boots through its UEFI loader. The laptop's own boot log showed the cores online, I226-V Ethernet up at 1 Gbps with a DHCP lease, the USB mouse and keyboard, NVMe, AirPods Max as a USB audio device, a 36 minute run with no panic, and Claude Code and Firefox both launching.
- Boot log and write guard. A FAT32 partition named `KURONO` holds `KURONO.LOG`, which the kernel streams its log into, so a failed boot can be read from any other OS. It also holds `KURONO.KFS`, the persistent store. On UEFI boots and on any disk that isn't QEMU's, NVMe writes outside those files are refused, and SATA and USB storage writes are refused outright. `kurono.diskguard=0` is the only way off.
- USB input. 64-bit BARs, BIOS handoff, scratchpads, correct full-speed packet sizes and intervals, and recovery for halted endpoints and a stalled ep0. USB used to be polled only by a kernel process that starved under a ring-3 program, so input died as soon as Claude started. The UI loop polls it now.
- Claude Code runs. The real, unmodified `claude` (linux-x64-musl, a 300 MiB Bun executable) runs with its full interface in the desktop terminal: streamed replies over HTTPS, Bash and Read tool calls through fork and exec, and `~/.claude` surviving reboots. It's a fixed-address executable at 0x200000, so the kernel is built with `KURONO_KERNEL_BASE=0x14000000`. Getting there took a kernel pty (`src/linux/kpty.cpp`) and a vt100 emulator (`src/apps/vt.cpp`), and it turned up real kernel bugs: process exit freed the shared vDSO page, and x64 `sysinfo` used the 32-bit layout, so Bun's GC never stopped (startup went from 3 minutes to 6 seconds).
- NVMe stale completions. A read that took longer than a few tens of milliseconds was abandoned, and its late completion was then taken by the next command, which returned before its data arrived. Waits are about 5 to 10 s now, and abandoned completions are consumed before the next submit.
- Typing and audio. `Scheduler::SleepMs` waited for its own wake-up and ignored other kernel processes that came due. Input polling stalled for up to 5 s and overflowed the PS/2 queue, and the audio pump only got 7% of real-time data. Now a 209-character typed string arrives byte-exact, and audio gets about 178 KB/s of the 176 KB/s it needs.
- New drivers. Intel I225/I226/82576 Ethernet (`igc.cpp`, which never writes the NVM) with a DHCP client, a rewritten HD Audio driver, and USB Audio Class 1 and 2 over isochronous xHCI transfers. USB audio has no feedback endpoint support yet, so asynchronous devices can drift.
- Firefox from the KURONO partition. It installs from `FIREFOX.TAR` with no network. In QEMU, 40 of 40 boots painted, about 30 s from power-on.

## October 2026: Firefox loads pages

The Firefox package is an official build, so e10s stays on and every tab lives in a forked content process. That child kept getting torn down a few seconds after launch. Reading Firefox's source for the exact release showed it was the Unix socket layer: `socketpair()` gave both ends the same fd, an empty `recv` returned 0 (EOF) instead of `EAGAIN`, stream `SCM_RIGHTS` fds landed on the wrong read, and Unix sockets always reported `EPOLLOUT`. With those fixed, pages render in the content process.

October also brought the modern desktop (`ui.style` modern or classic, Inter text, a translucent menu bar, a dock and an original icon set), spring-based window motion, and boots on 512 MB of RAM.

## July 2026: the first real web page

Firefox went from painting its own window frame to painting a real page over Kurono's TCP/IP stack. The fixes that got it there:

- A thread moving between cores could be resumed on the second core while the first was still saving its registers, so it ran on a half-saved frame. That crashed about 3 of every 4 Firefox boots.
- The compositor never sent `wl_buffer.release`, so Firefox ran out of buffers after the first frame and froze.
- A client that only repaints the changed strip of a fresh buffer wiped its window to black every frame. The compositor now keeps a per-surface shadow buffer.
- A futex abort path cleared its wait slot unconditionally, which could destroy another thread's entry and leave it blocked forever.
- TLB shootdowns gave up waiting too soon, so a core could keep running stale translations after the JIT flipped page permissions.
- Freeing a process while a CPU was still unwinding on it was a use-after-free.
- `fcntl` locks were a stub that always granted, which let two SQLite connections both own a WAL lock.

## June 2026

- I moved development from Windows and WHPX to Linux and KVM, which also fixed a pile of networking and input oddities WHPX was causing.
- KSA, the privilege prompt, renders inside a hypervisor-isolated memory region the main OS can't map, and its answer comes back through one read-only VMCALL channel.
- The desktop survives a reboot. The NVMe driver had never completed a command because a missing `volatile` let the compiler hoist the check out of the loop. Persistence goes through KFS, a small inode-based filesystem I wrote. NVMe gained multi-page DMA, so a save takes a handful of commands instead of hundreds.
- All cores boot and run kernel code in parallel, with a per-CPU syscall fast path, GDT, TSS and current task.
- A real Linux Wayland client renders through the in-kernel compositor, with threads, futexes, epoll, `mprotect`, memfd, file-backed `mmap` and `SCM_RIGHTS` working end to end.
- Firefox 140.11.0esr was built against musl, and its 174 MB `libxul.so` loads through `ld-kurono`.
- KMemX, the memory compressor, was added: inactive 4 KB pages get LZ4-compressed into a fixed pool and decompressed on the next fault.

## What's built

### Kernel

x86_64 long mode with a 256-entry IDT. The physical memory manager is sized from usable RAM, and the heap grows on demand, so it boots on 512 MB. The scheduler is CFS-style with normal, FIFO, round-robin and idle classes, per-CPU dispatch, and preemption in ring 3 and at syscall exit. SMP comes up on every core, with x2APIC support. There's a hybrid driver model (KDF, UDF, IRP, KExec), so some drivers can fail without taking the kernel down, and kinit supervises services the way systemd does.

### Drivers

NVMe, AHCI, xHCI with keyboards, mice, tablets, mass storage, audio and hubs, PS/2, Intel HD Audio, USB audio, AC97 and SB16, e1000 and Intel I225/I226/82576 Ethernet, Bochs and VirtIO GPU, Intel display with multi-monitor, NVIDIA and AMD detection, RTC, serial, CPU feature detection and a Wi-Fi hardware layer. KLD runs some unmodified Linux drivers (Intel Wi-Fi, a touchpad, i915 display) in ring-3 containers.

### Desktop

Its own compositor and window manager, with two live-switchable styles: modern, with a menu bar and dock, and classic, with a taskbar. It has resolution modes up to 4K, VSync, DPI scaling, TTF text, animated windows, a lock screen and setup wizard, and an in-kernel Wayland compositor for Linux apps.

### Apps

Terminal (vt100 on a real pty), Files, Text Editor, Calculator, Media Player with video, Task Manager with process, performance, services and details tabs, Settings, and the Firefox launcher.

### Shell

About 150 commands across Kurono, Linux and Windows-style environments, with switching (`switch linux`, `cmd`) and piping across environments. The commands read real driver data: `lspci`, `lsusb`, `ifconfig`, `ps` and `free` all show the real thing.

### Filesystems

KVFS in memory, with directories, symlinks, devices, pipes and mount points. KFS on disk for `/home`, `/etc`, `/root` and `/apps`. ext4 reads and partial writes, and FAT32 for the boot partition.

### Networking

Ethernet, ARP, IPv4, ICMP, UDP and TCP with the full state machine, window scaling, a multi-segment send window and fast retransmit. DHCP and DNS, and a socket API that Linux programs reach through an AF_INET bridge. Bulk downloads run at about 36 MB/s in QEMU.

### Linux layer

282 syscall numbers defined and 262 dispatched, an `ENOSYS` trace for anything missing, 256 process and thread slots, 256 fds per process, fork, exec, wait, signals, AF_UNIX with fd passing, epoll, inotify, pidfd, ptys, `/proc` and `/sys`. It reports itself as "Linux 6.8.0-kurono", and `ld-kurono` is its dynamic linker.

### Hypervisor

VT-x and AMD-V, EPT and NPT with large pages, virtual PIC, APIC, PIT, HPET, serial and disks, the Linux boot protocol, VT-d and AMD-Vi with PCI passthrough, virtio devices and Wayland bridging so guest windows appear on the desktop. It boots Alpine, Debian 13 and nested Kurono.

### Security and scripting

`amdo` (formerly `ken` and `supr`) for elevation, with audit logging and salted password hashes, KSA approval prompts, multiple users and groups, KCL as a full scripting language, small Python and JavaScript interpreters, and `kpkg` packages.

### Install and recovery

A UEFI loader, an emergency EFI loader and emergency shell, a graphical setup wizard, GPT and MBR parsing, and an installer that can deploy the kernel and EFI payloads.

## Known problems

- Firefox takes about 30 s to start in a VM, and much longer under TCG. Its JIT is off by default (`kurono.ffjit=1|2` turns tiers back on).
- The Linux layer doesn't track close-on-exec yet, so forked and exec'd children inherit every open fd.
- The AF_UNIX table holds 64 sockets system-wide, and Firefox uses about 45 of them.
- GPU acceleration is off by default because it still misbehaves on real hardware.
- The NVIDIA driver detects the card, but it breaks under heavy work that uses more than about 50 MB of VRAM.
- On the laptop, the HD Audio speakers need an amplifier driver I haven't written. Headphones and USB audio work.
- USB hubs and Debian on real hardware are only tested in QEMU so far.
- ext4 writes are partial: sparse allocation and directory growth aren't finished.
- FAT32 is only meant for the boot partition, not general use.
- KMemX is built and tested but doesn't compress anything on real boots yet, and idle RAM is still too high.
- Booting a guest inside Kurono needs VT-x or AMD-V exposed to it, and inside another VM that means nested virtualization.
