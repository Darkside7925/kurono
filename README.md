# Kurono

Kurono is an x86_64 operating system I'm building from scratch in freestanding C++17 and assembly. It has its own kernel, drivers, filesystem, TCP/IP stack, desktop and scripting language. It also speaks the Linux syscall ABI well enough to run real Linux programs without changes. There's no Linux kernel underneath it.

The goal is an OS that's light on RAM and fast on older hardware, and still runs the programs people actually use. The three I test it with are Firefox 140, the Claude Code CLI and OpenCode. All three are the stock Linux binaries, and all three run on Kurono, in QEMU and on my laptop (a Legion Pro 7 with a Core Ultra 7 255HX).

This is part 3 of the big update, the infrastructure work that comes before Kurono Kintsugi. It isn't the final version. Some parts are held back because they still have bugs, and it's only been tested on one real machine. Read [the write guard](#the-write-guard) before you boot it on a computer you care about.

## Why so much lands at once

Kurono is a solo project and I've been at it for a long time. Most of it was built up locally first, in a `feats/` tree on my dev machine. A lot of the feature set already exists there but isn't debugged or wired together yet. What you see in this repo is me finishing those pieces and pushing each one when it's solid, so features tend to arrive in bursts. Plenty of it works with rough edges, and it isn't a daily driver yet.

## What works

| | In QEMU | On my laptop |
| --- | --- | --- |
| UEFI boot to the desktop | yes | yes |
| SMP | tested up to 20 CPUs | all 20 cores by default |
| USB keyboard and mouse | yes, also behind hubs | yes (hub support is new) |
| NVMe | yes | yes |
| Ethernet | e1000, 82576, I225/I226 | I226-V at 1 Gbps with DHCP |
| Intel HD Audio | yes | ALC287 is detected, but the speakers need an amp driver I haven't written |
| USB audio | UAC1 | AirPods Max over USB-C (UAC2) |
| Files that survive a reboot | yes | yes |
| Claude Code with its full interface | yes, a 3 hour session with no freeze | runs |
| Firefox 140 | yes, also alongside Claude | opens and loads sites |
| OpenCode | yes, TUI and tools | not tried yet |
| Alpine and Debian guests | yes, Alpine on 4 vCPUs | Alpine yes, Debian not tried yet |
| Multiple monitors | yes | internal panel plus a DP monitor on the Intel GPU |
| Wi-Fi | no | BE200, through KLD |
| GPU acceleration | behind a flag, off by default | behind a flag, off by default |

## What's in part 3

OpenCode runs. It's the stock `linux-x64-musl` build with its TUI and its file and shell tools; type `opencode`. It needs musl's C++ runtime, which Kurono loads from two files on the KURONO partition.

Claude Code doesn't freeze anymore. Its garbage collector stops the other threads with a signal and waits for each one to answer. Kurono only woke a thread sleeping in a futex for a fatal signal, so the collector waited forever. After the fix, a 3 hour session full of tool calls ran clean.

Firefox and Claude can run at the same time. vfork copied part of the parent's stack back from the wrong memory, and once Claude had fragmented RAM, Firefox's environment got garbled. That was 4 crashes in 12 boots before the fix and 0 in 13 after.

A lot of everyday Linux behaviour got fixed. `cat` printed nothing, `cp` made empty files, pipes lost everything past 64 KB, symlinks didn't resolve, every file was dated 1970, `ls -a` had no `.` or `..`, `cmd & wait` hung, a folder couldn't hold more than 512 files, and `kill` on a finished job could hit a different program.

VMs got a lot better. Alpine runs on up to 4 cores at once, guest RAM and disks grow as they're used, and Debian 13 boots. `vm kloop` builds a kernel inside the Alpine guest and boots it as a nested Kurono, with its log on the host. A PCI device can be passed through to that guest, and `kld reload` swaps a ring-3 driver without rebooting.

Big kernel allocations now come straight from page frames. A long session keeps about 170 MB more free, and Firefox gets back around 470 MB.

Smaller things: `ken` is now called `amdo` (the old names still work), KSA has an off switch in Settings, the real Intel iGPU stopped drawing wrong pixels, window drags copy a quarter of the data they used to, and there's an automated test that compares partial redraws against full ones.

## What was in Kintsugi part 2

The built-in apps can run as their own ring-3 programs (`kurono.r3apps=...`), so a crashing app can't take the OS down. The in-kernel versions are still there as a fallback. The Text Editor was rewritten with tabs, syntax highlighting and regex find, and there's a new Control Center, a better Files app, window snapping and Alt+Tab.

There's one clipboard for Kurono apps, Firefox and other Wayland apps, XWayland and the Alpine guest, with a Super+V history. `ken` (now `amdo`) is Kurono's sudo, and admin actions go through KSA, an approval prompt that runs in an isolated hypervisor context so other programs can't fake it or click it. `kurono.sched=2` runs the scheduler on every core. `/proc` is real for every task, so `ps`, `top` and Task Manager show real numbers. USB hubs work, and DHCP renew, DNS fallback and TCP retransmit got fixed.

## The desktop

Kurono has its own compositor and window manager with animated windows, a dock, a menu bar and its own icon set. Firefox gets on screen through an in-kernel Wayland compositor.

The built-in apps are Terminal, Files, Text Editor, Calculator, Media Player, Task Manager and Settings. The terminal is a real vt100 emulator on a real pty, so full-screen programs work in it: colours, the alternate screen, scroll regions and resizing.

## Running Linux programs

The kernel implements the Linux syscall ABI, and Kurono has its own ELF64 dynamic linker, `ld-kurono`. Threads and futexes, epoll, pidfd, inotify, ptys, Unix sockets with fd passing and close-on-exec all behave close enough to Linux for real software:

- Firefox 140 ESR (musl and Wayland, with a 174 MB `libxul.so`), with its content processes, across several cores.
- Claude Code, the stock `linux-x64-musl` build with no patches. It's a 300 MB fixed-address executable that loads right where the kernel used to sit, so the kernel now links itself above it.
- Debian's stock `busybox-static` and other prebuilt glibc and musl binaries.

Most of the work is finding all the places your kernel is subtly wrong. A few of my favourites are in [bugs worth reading about](#bugs-worth-reading-about).

## Everything else

- Kernel: a hybrid design with a driver framework (KDF), an SMP scheduler with per-CPU timers, and KMemX, a memory compressor. KMemX is built and tested, but real boots don't compress anything yet.
- Storage: KFS, my own on-disk filesystem, plus ext4 and FAT32, NVMe, AHCI and USB mass storage.
- Networking: a TCP/IP stack with DHCP, window scaling and out-of-order reassembly.
- Audio: a mixer and audio server, a PulseAudio-compatible server for Linux apps, HD Audio and USB audio.
- Languages: KCL, Kurono's scripting language, plus small built-in Python and JavaScript interpreters.
- Shell: 150+ commands, with Kurono, Linux and Windows-style environments.
- Packages: `kpkg`, with an update flow at boot.
- Hypervisor: a type 1 stack for VT-x and AMD-V. It boots Alpine and Debian 13, and their windows can show up on Kurono's desktop. Inside most VMs it needs nested virtualization.

## Building it

Everything builds on Linux. I use Debian 13, and WSL Debian on Windows works too. You need a C++ toolchain, NASM, `musl-gcc` (from `musl-tools`), `gnu-efi`, `grub-mkrescue` and `xorriso`.

```bash
cd src && make iso -j8 KURONO_KERNEL_BASE=0x14000000
```

That builds `build/kurono.iso` and the UEFI loader, `build/isodir/boot/kurono_efi.efi`.

`KURONO_KERNEL_BASE` links the kernel above Claude Code's fixed load address. Leave it off for the old low link, which still boots on 512 MB of RAM. Make doesn't notice when only this flag changes, so delete `build/kurono_base.elf` and `build/kurono.elf` when you switch.

To run it in QEMU:

```bash
qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 8G -boot d \
  -cdrom build/kurono.iso \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -device qemu-xhci -device usb-kbd -device usb-mouse
```

Keep `-boot d`. With a second disk attached, QEMU tries to boot the disk otherwise and you get a black screen. Kurono doesn't boot on `-M q35`.

### On real hardware

There's no installer for this. It's how I run it, and it assumes you're comfortable with partitions and a boot manager.

1. Make a FAT32 partition and name it `KURONO` in the GPT. Kurono looks for exactly one partition with that name.
2. Copy `kurono_efi.efi` to `EFI\BOOT\BOOTX64.EFI` on it.
3. Put these files in its root:

| File | What it is | Size |
| --- | --- | --- |
| `KURONO.LOG` | zero-filled; the boot log streams into it | 16 MB |
| `KURONO.KFS` | zero-filled; your persistent files. It must be contiguous | 3 GB is plenty |
| `FIREFOX.TAR` | optional; the Firefox package, so it installs without a network | about 350 MB |
| `CLAUDE` | optional; `package/claude` from `@anthropic-ai/claude-code-linux-x64-musl` | about 300 MB |
| `CLAUDE.ENV` | optional; `CLAUDE_CODE_OAUTH_TOKEN=...` or `ANTHROPIC_API_KEY=...` | small |
| `OPENCODE` | optional; the `opencode-linux-x64-musl` binary | about 210 MB |
| `LIBSTDC.SO`, `LIBGCCS.SO` | needed by OpenCode: musl's `libstdc++.so.6` and `libgcc_s.so.1` (the Firefox package has both in `firefox/lib`) | 3 MB |
| `OPENCODE.ENV` | optional; provider keys such as `ANTHROPIC_API_KEY=...` | small |
| `ALPINE.IMG`, `DEBIAN.IMG` | optional; the Linux guests. Each must be one contiguous file | 3 to 4 GB |

`CLAUDE.ENV` and `OPENCODE.ENV` are plain text, so anything that can read that partition can read your key. You can `export` the variable in Kurono's terminal instead.

Boot it from your firmware's menu or a boot manager. If something breaks, the log is in `KURONO.LOG` and you can read it from any other OS.

### The write guard

On real hardware Kurono refuses every disk write outside the files on its own `KURONO` partition. That covers NVMe, SATA and USB storage. It turns on by itself for UEFI boots and for any disk that isn't QEMU's, and the only way to turn it off is `kurono.diskguard=0`. I added it because a test build could otherwise have formatted my real SSD.

## Bugs worth reading about

- The NVMe driver gave up too early. Reads timed out after a few tens of milliseconds, the late completion stayed in the queue, and the next read took it as its own and returned before its data arrived. The queue stayed off by one forever. It looked like a broken FAT chain on a healthy disk.
- USB input only worked when nothing else was running. A background kernel task read the keyboard and mouse, and it starved as soon as a program took over the CPU. All my tests typed through PS/2, so it only showed up on the real laptop, as a frozen OS.
- One sleep loop broke both sound and typing. A kernel task sleeping for 5 seconds held up every other task that came due, which overflowed the keyboard queue and ran audio at 7% speed.
- Exiting a process corrupted memory. Teardown freed the shared vDSO time page, so everything after the first exit was running on borrowed time. That was Bun's random crashes.
- `sysinfo` used the 32-bit layout on x64. Total RAM read as 0, so Bun's garbage collector never stopped running. Fixing it took Claude Code's startup from 3 minutes to 6 seconds.
- A kernel pointer leaked in `%rdx`. The SysV ABI says that register holds the exit handler, so every glibc program called into kernel memory when it exited.
- `socketpair()` gave both ends the same fd. That's why Firefox's content process kept dying after navigation.
- A missing `volatile` meant no NVMe command ever completed. The compiler hoisted the completion check out of the polling loop.
- One `uint32` broke SQLite. `fcntl` kept its argument in a 32-bit variable, so every file lock read its `struct flock` from a chopped-off address. OpenCode died with "locking protocol" until that became a `uint64`.
- Blocking pipe writes didn't block. A write into a full pipe returned "try again" instead of waiting, so anything past 64 KB was lost. Long Claude Code runs on Kurono were the first thing to trip over it.
- Every nested Kurono froze 10 seconds in. My virtual timer chip lost its mode whenever the guest latched the counter, which Kurono does on every poll, and the virtual interrupt controller cleared the wrong interrupt. Linux guests never hit either one.

## What's next

- GPU acceleration on Intel, on by default
- Hardware video decode
- Moving more of the system out of the kernel, audio and networking first
- More of KLD, so more unmodified Linux drivers run in sandboxes
- KMemX compressing memory on real boots, and much lower idle RAM
- Faster Firefox startup. It takes about 30 s in a VM, and most of that is Firefox waiting on something

## Where things live

| Path | What |
| --- | --- |
| `src/kernel/`, `src/proc/`, `src/hal/` | kernel core, scheduler, SMP, ACPI |
| `src/linux/` | Linux syscall layer, `ld-kurono`, pty, sockets |
| `src/drivers/` | NVMe, AHCI, xHCI, USB audio, HD Audio, I226, e1000 and more |
| `src/net/` | TCP/IP, DHCP |
| `src/fs/` | KFS, KVFS, persistence |
| `src/ui/`, `src/apps/` | compositor, window manager, terminal, apps |
| `src/boot/` | boot code and the UEFI loader |
| `docs/` | developer docs |

Where things stand in more detail is in [STATUS.md](STATUS.md).

## License

GPL v2, see [LICENSE](LICENSE). Third-party code, programs, fonts and data keep their own licenses. They're listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), with the full texts in `licenses/`.
