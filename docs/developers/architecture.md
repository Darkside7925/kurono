# Architecture overview

This is the one-page map. Each box links to the per-file docs that go deeper. When a
detail here disagrees with the source, trust the source.

```
 ring 3                                                         
 +------------------+ +----------------+ +------------------+ +--------------+
 | kui apps         | | Linux programs | | KLD driver       | | Alpine guest |
 | (editor, files,  | | (Firefox,      | | containers       | | (own VM,     |
 | settings, ...)   | | BusyBox, ...)  | | (e1000e)         | | via hypervisor)
 +--------+---------+ +-------+--------+ +--------+---------+ +------+-------+
          | wayland / unix     | linux syscalls    | broker call      | vmexit
 ---------+--------------------+-------------------+------------------+--------
 ring 0   v                    v                   v                  v
 +----------------------------------------------------------------------------+
 | Wayland server + window manager + desktop   | Linux ABI layer (KLS)        |
 | clipboard, settings service, pulse server   | ld-kurono, /proc, sockets    |
 +----------------------------------------------------------------------------+
 | scheduler (sched=2 SMP) | KEN / KSA / admin gate | KLD broker | hypervisor  |
 +----------------------------------------------------------------------------+
 | PMM / VMM / heap | KVFS + KFS | TCP/IP | drivers (NVMe AHCI xHCI NIC GPU)  |
 +----------------------------------------------------------------------------+
 | Multiboot2 or EFI loader  ->  long mode  ->  kurono_kernel.cpp              |
 +----------------------------------------------------------------------------+
```

## Kernel core

`src/kernel`, `src/hal`, `src/proc`, `src/boot`.

- Boot: GRUB (Multiboot2) or my own EFI loader gets the machine into long mode and calls
  `kurono_kernel.cpp`, which brings the pieces up in a long fixed order. See
  `how-it-works/BOOT_TO_DESKTOP.md`.
- Memory: physical memory manager (`pmm.cpp`), kernel heap, per-process address spaces
  (`vmm`). Boots to a desktop on 512 MB and manages large RAM sizes. `kmemx` (memory
  compression) is present but does not compress pages in a real boot yet.
- Scheduling: kernel processes are cooperative; ring-3 tasks are preemptive.
  `kurono.sched=2` is the full-SMP mode where user tasks run on all online CPUs. The
  default mode is the older one with fewer cores in play. See `proc/SCHEDULER.md` and
  `proc/SMP.md`.
- `/proc`: real per-task directories for user and kernel tasks (status, stat, task/<tid>),
  `/proc/stat` and `/proc/uptime`, so `ps` and `top` work and the Task Manager reads them.
- Rule I follow: changes to `struct Process` are dangerous (adding fields once broke
  AP startup). Extra bookkeeping goes in side tables.

## Linux ABI layer (KLS)

`src/linux`. A Linux syscall implementation inside the kernel, plus `ld-kurono`, an
ELF64 dynamic linker. It's what lets unmodified binaries run: Firefox, BusyBox (a stock
static build), the recovery tools, musl- and glibc-static programs.

It covers processes and `CLONE_THREAD` threads, `futex`, `epoll`/`poll`, `mmap` (file,
anonymous, `memfd`), unix sockets with `SCM_RIGHTS`, pipes, ptys, signals, and AF_INET
sockets bridged into Kurono's TCP/IP stack. Devices appear as nodes (`/dev/sd*` with
partition windows, `/dev/kshell`, `/dev/kexec`). Linux paths map onto KVFS (`/etc/x` is
`/system/etc/x`). See `linux/LINUX_SYSCALL.md`, `linux/LD_KURONO.md`, `linux/SYSCALLS.md`.

Lessons that shaped it: sockets are non-blocking (EINPROGRESS/EAGAIN must work), and the
kernel must be event-driven about wakeups, not timer-driven.

## Wayland server

`src/ui/wayland_server.cpp`. A real Wayland compositor inside the kernel. Clients talk to
it over unix sockets and `wl_shm` buffers. It does xdg toplevels and popups,
`wl_subsurface` composition (Firefox needs that), and `wl_data_device` plus
primary-selection for the clipboard. Windows go to the same window manager as native
ones, so they get the same title bars, motion, snapping, dock and Alt+Tab.

The clipboard (`system/clipboard.cpp`) is one store: native apps, Wayland clients and the
Alpine guest all read and write it, text history is 16 entries, and the copy survives its
owner exiting. See `ui/WAYLAND_SERVER.md`, `ui/WINDOW_MANAGER.md`.

## Desktop and window manager

`src/ui`. Compositing window manager, menu bar, dock, lock screen, control center,
notifications, spring-based window motion. Window management includes snap (keyboard and
drag-to-edge with a preview), an Alt+Tab switcher with live thumbnails, Super+D show
desktop, per-monitor maximize, and remembered per-app window geometry
(`/etc/kurono/wm_geometry.conf`). Multi-monitor is handled by the display manager.

## kui apps

`src/kui`, `src/apps`. The app toolkit. One source file builds either as a window drawn
by the kernel, or as a ring-3 musl-static program that is a Wayland client. See `kui.md`.

## Security: KEN, KSA, admin gate

`src/security`.

- **KEN** is the privilege engine and the `ken <cmd>` command (sudo-like; `sudo` and
  `supr` are hidden aliases). Roles: Guest, User, Admin, Root, Sovereign.
- **KSA** is the approval prompt. It is rendered and decided inside an isolated
  hypervisor context (EPT-isolated), and only a verdict comes back over a read-only
  channel. The point: a compromised main kernel can't forge a click. The prompt keeps the
  compositor alive, polls USB HID, and can be answered on a machine that only has USB
  input.
- **Admin gate** is where the policy is enforced. Ring-3 programs run as uid 1000, not
  root. These need approval (cached for 15 seconds after): writes to system paths,
  opening raw block devices, signalling another user's process, `setuid`-style identity
  changes, cross-process memory access, `sethostname`, the KLD broker, privileged
  `/dev/kexec` builtins, and launching `.kcl` scripts from kui apps. `kurono.ken.enforce=0`
  turns the gate off for debugging.

See `security/KEN.md` and `security/KSA.md`.

## Hypervisor

`src/virt`. Intel VT-x (EPT), with AMD paths written. It runs an **Alpine Linux** guest
with virtio devices (network, block, console, 9p, audio, GPU host). A Wayland bridge
(`wlbridge`) carries guest Wayland over console channels so guest apps show up as
windows on the Kurono desktop, and carries clipboard and audio. Debian is still being
tested. Needs VT-x, so under QEMU it needs nested virtualization. See `virt/`.

## KLD: Linux drivers in a sandbox

`src/kernel/kld_broker.cpp`. KLD loads unmodified upstream Linux driver code (e1000e
today) in an untrusted ring-3 container. The container can't touch hardware directly.
Every PCI, BAR, DMA, MSI and ACPI operation goes through the kernel broker, which grants
access per device and per address space, and refuses config writes that would move a BAR
or reprogram MSI. The kernel links no Linux code. Honest limit: without an IOMMU a
granted device could still DMA anywhere, so this is fault isolation, not full security
isolation.

## Drivers

`src/drivers`: NVMe, AHCI/SATA, xHCI USB (HID, mass storage, audio, and hubs with
transaction translators; hub support has not been tried on real hardware yet), Intel
igc/e1000 NICs, Wi-Fi (iwl, ath, brcm, rtw, to varying degrees), Intel display with
modesetting and multi-monitor, HDA and USB audio, virtio-gpu and BGA for VMs, and the
experimental Intel acceleration module (`intel_accel`, flags in `boot-flags.md`).

## Filesystems and persistence

KVFS is the in-memory runtime filesystem. KFS is the on-disk persistence layer. FAT32
and ext4 read paths exist for installing and for guest images. See `fs/`.

## Networking

TCP/IP over Ethernet and Wi-Fi: ARP, DHCP with a proper lease state machine (renew,
rebind, expiry), DNS with server fallback, TCP with an RTT-based retransmit timer. See
`net/NETWORK.md`.
