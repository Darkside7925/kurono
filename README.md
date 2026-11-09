# Kurono

An x86_64 operating system I'm writing from scratch in freestanding C++17 and assembly. It has its own kernel, drivers, filesystem, TCP/IP stack, compositing desktop and scripting language, plus a Linux syscall layer good enough to run real, unmodified Linux programs. There's no Linux kernel underneath and no libc in the kernel.

The two programs I judge it by are Firefox 140 and the Claude Code CLI. Both are the stock binaries, and both run on it in QEMU. On my actual laptop (Legion Pro 7, Core Ultra 7 255HX) both start, but they're slow and they stall, and I'm working on that.

> **Status:** this is part 2 of the Kurono Kintsugi update, and it isn't the last part. I'm pushing what works now. Some of it is held back because it didn't go as planned, mainly GPU acceleration, which is still off by default while I test it on real hardware. It's only been tested on one real machine. Don't run it on a computer you care about without reading [the write guard](#the-write-guard).

---

## What it can do

| | In QEMU | On my laptop |
| --- | --- | --- |
| UEFI boot to desktop | yes | yes |
| SMP | up to 20 CPUs tested | 8 cores by default, all 20 with `kurono.sched=2` |
| USB keyboard and mouse | yes, also behind hubs | yes (hub support is new, still testing) |
| NVMe | yes | yes |
| Ethernet | e1000, 82576, I225/I226 | I226-V at 1 Gbps with DHCP |
| Intel HD Audio | yes | ALC287, but the speakers need an amp driver Kurono doesn't have |
| USB audio | UAC1 | AirPods Max over USB-C (UAC2) |
| Files that survive a reboot | yes | yes |
| Claude Code, full interface | yes | answers, but stalls for minutes |
| Firefox 140 | painted in every recent test boot | works, loads sites, still slow on heavy ones |
| Multiple monitors | yes | internal panel + DP monitor on the Intel GPU |
| Wi-Fi | no | no |
| GPU acceleration | flag only, off by default | held back, still testing |

### New in Kintsugi part 2

- **Apps as separate programs.** Calculator, Text Editor, Files, Task Manager, Media Player, the video player, Terminal and Settings can run as their own ring-3 programs (`kurono.r3apps=...`), so a crashing app can't take the OS down with it. The in-kernel versions are still there as a fallback.
- **New apps and UI:** a rewritten Text Editor (tabs, syntax highlighting, regex find), a new Control Center, a better Files app, window snapping and Alt+Tab.
- **One clipboard everywhere:** Kurono apps, Firefox and other Wayland apps, XWayland and Alpine apps, with a Super+V history.
- **`ken` and KSA:** `ken` is Kurono's sudo. Admin actions go through KSA, an approval prompt that runs in an isolated hypervisor context so other programs can't fake or click it.
- **All cores:** `kurono.sched=2` runs the scheduler across every CPU.
- **Real `/proc`** for every task, kernel ones included, so `ps`, `top` and the Task Manager show real numbers.
- **USB hubs**, network reliability fixes (DHCP renew, DNS fallback, TCP retransmit) and a lot of bug fixes.

### The desktop

Kurono has its own compositor and window manager, with animated windows, a dock, a menu bar, and an original icon set. It also has an in-kernel Wayland compositor, which is how Firefox gets on screen.

Built-in apps: Terminal, File Manager, Text Editor, Calculator, Media Player, Task Manager, and Settings. The terminal is a real vt100 emulator on a real pty, so full-screen programs work in it: colors, alternate screen, scroll regions, resize and all.

### Running Linux programs

Kurono implements the Linux syscall ABI in the kernel and has its own ELF64 dynamic linker, `ld-kurono`. Threads and futexes, epoll, pidfd, inotify, ptys, Unix sockets with fd passing, and close-on-exec all behave enough like Linux for real software:

- **Firefox 140 ESR** (musl + Wayland, 174 MB `libxul.so`), with content processes, across several cores.
- **Claude Code**, the stock `linux-x64-musl` build with no patches. It's a 300 MB fixed-address executable that sits right where the kernel used to live, so the kernel now links itself above it.
- **Stock Debian `busybox-static`** and other prebuilt glibc and musl binaries.

Getting there is mostly finding everything your kernel gets subtly wrong. See [bugs worth reading about](#bugs-worth-reading-about).

### Everything else in the box

- **Kernel:** hybrid design, a driver framework (KDF), an SMP scheduler (8 cores by default, all of them with `kurono.sched=2`) with per-CPU timers, and KMemX, a memory compressor (built and tested, but real boots don't compress anything yet).
- **Storage:** KFS, my own on-disk filesystem, plus ext4 and FAT32 support, NVMe, AHCI and USB mass storage.
- **Networking:** a from-scratch TCP/IP stack with DHCP, window scaling and out-of-order reassembly.
- **Audio:** a mixer and audio server, a PulseAudio-compatible server for Linux apps, HD Audio, USB audio.
- **Languages:** KCL, Kurono's scripting language, plus small built-in Python and JavaScript interpreters.
- **Shell:** 150+ commands across Kurono, Linux and Windows-flavored environments.
- **Packages:** `kpkg`, with a boot-time update flow.
- **Hypervisor:** a Type 1 stack with VT-x and AMD-V support. It boots Alpine Linux, and Alpine apps can show up on Kurono's desktop. Debian is still being tested. Inside most VMs it needs nested virtualization.

---

## Building and running

Everything builds on Linux. I use Debian 13, and WSL Debian on Windows works too. You need a C++ toolchain, NASM, `musl-gcc` (`musl-tools`), `gnu-efi`, `grub-mkrescue` and `xorriso`.

```bash
cd src && make iso -j8 KURONO_KERNEL_BASE=0x14000000
```

That gives you `build/kurono.iso` and the UEFI loader, `build/isodir/boot/kurono_efi.efi`.

`KURONO_KERNEL_BASE` links the kernel above Claude Code's fixed load address. Leave it off for the classic low link, which still boots on 512 MB of RAM. Make doesn't notice when only this flag changes, so delete `build/kurono_base.elf` and `build/kurono.elf` when you switch.

Run it in QEMU:

```bash
qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 8G -boot d \
  -cdrom build/kurono.iso \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -device qemu-xhci -device usb-kbd -device usb-mouse
```

Keep `-boot d`: with a second disk attached, QEMU otherwise tries to boot the disk and you get a black screen. Kurono doesn't boot on `-M q35`.

### On real hardware

This isn't an installer. It's how I run it, and it assumes you're comfortable with partitions and a boot manager.

1. Make a FAT32 partition and name it `KURONO` in the GPT. Kurono looks for exactly one partition with that name.
2. Copy `kurono_efi.efi` to `EFI\BOOT\BOOTX64.EFI` on it.
3. Put these in its root:

| File | What | Size |
| --- | --- | --- |
| `KURONO.LOG` | zero-filled, the boot log streams into it | 16 MB |
| `KURONO.KFS` | zero-filled, your persistent files; must be contiguous | 3 GB is plenty |
| `FIREFOX.TAR` | optional, the Firefox package, so it installs with no network | about 350 MB |
| `CLAUDE` | optional, `package/claude` from `@anthropic-ai/claude-code-linux-x64-musl` | about 300 MB |
| `CLAUDE.ENV` | optional, `CLAUDE_CODE_OAUTH_TOKEN=...` or `ANTHROPIC_API_KEY=...` | small |

`CLAUDE.ENV` is plain text, and anything that can read that partition can read it. You can `export` the variable in Kurono's terminal instead.

Boot it from your firmware's menu or a boot manager. If something breaks, the log is sitting in `KURONO.LOG`, and you can read it from any other OS.

### The write guard

On real hardware, Kurono refuses every disk write outside the files on its own `KURONO` partition, on NVMe, SATA and USB storage alike. It turns on by itself for UEFI boots and for any disk that isn't QEMU's. The only way to turn it off is `kurono.diskguard=0`. It exists because a test build could otherwise have formatted my real SSD.

---

## Bugs worth reading about

- **The disk driver gave up too early.** NVMe reads timed out after a few tens of milliseconds. The late completion stayed in the queue, the next read took it as its own and returned before its data arrived, and the queue stayed off by one forever. It showed up as a "broken FAT chain" on a healthy disk.
- **USB input only worked when nothing was running.** A background kernel task read USB keyboards and mice, and it starved the moment a program took over the CPU. All my tests typed through PS/2, so it only showed up on the real laptop, as a frozen OS.
- **One sleep loop broke sound and typing.** A kernel task sleeping for 5 seconds held up every other task that came due. That overflowed the keyboard queue and ran audio at 7% speed.
- **Exiting a process corrupted memory.** Teardown freed the shared vDSO time page, so everything after the first exit was running on borrowed time. That was Bun's random crashes.
- **`sysinfo` used the 32-bit layout on x64.** Total RAM read as 0, so Bun's garbage collector ran nonstop. Fixing it took startup from 3 minutes to 6 seconds.
- **A kernel pointer leaked in `%rdx`.** The SysV ABI says that register is the exit handler, so every glibc binary called into kernel memory on exit.
- **`socketpair()` gave both ends the same fd,** which is why Firefox's content process kept dying after navigation.
- **A missing `volatile` meant no NVMe command ever completed.** The compiler hoisted the completion check out of the polling loop.

---

## What's next

- GPU acceleration on Intel. It's in this push behind flags (`kurono.igpu.accel=1`, `kurono.igpu.zerocopy=1`, `kurono.wpdamage=1`), but it's off by default until it's solid on real hardware
- Moving more of the system out of the kernel: audio and networking next
- KLD, so more unmodified Linux drivers run in sandboxes
- KMemX actually compressing memory in real boots, and a much lower idle RAM use
- Faster Firefox startup. It's about 30 s in a VM, and most of that is Firefox waiting on something
- Wi-Fi. The BE200 needs firmware and a huge stack, so this is a long way off

## Layout

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

## License

GPL v2. See [LICENSE](LICENSE).
