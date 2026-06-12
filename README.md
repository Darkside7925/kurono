# Kurono OS

A bare-metal x86_64 operating system with a hybrid kernel that combines a native desktop OS, a Linux-compatible syscall/runtime layer, a Windows command environment, a package/update pipeline, and a Type 1 hypervisor into one bootable image.

Built from scratch in freestanding C++17 and x86 assembly - no libc in the kernel, no borrowed POSIX runtime under the kernel, and no existing host kernel reused as the core OS. Kurono boots as a Multiboot2 ELF kernel, also produces standalone EFI loaders, runs in QEMU with WHPX or KVM, supports emergency recovery boot, can deploy onto FAT32/ext4 installer targets, and ships both Alpine and Debian guest paths.

> **19 hardware drivers - 76+ compatibility commands plus Kurono-native commands - full TCP/IP stack - in-kernel ELF64 dynamic linker - in-kernel Wayland compositor - PulseAudio/D-Bus runtime services - multi-backend display manager (BGA, VirtIO GPU, Intel, NVIDIA, AMD) - hybrid GPU topology detection (Optimus, PowerXpress) - emergency EFI boot - installer stack - Alpine VM - Debian on-demand rootfs and update pipeline**

---

## Quick Start

I moved the whole thing onto Linux (KVM), so this is the path I actually use day to day:

```bash
# build the iso + boot it (KVM, virtio-gpu, audio, USB)  -  one command
./start.sh

# don't rebuild, just boot the existing iso
./start.sh --no-build

# clean rebuild then boot
./start.sh --clean

# plain framebuffer instead of the accelerated virtio-gpu
./start.sh --std

# boot the UEFI path instead of BIOS
./start.sh --uefi

# gdb stub on localhost:1234
./start.sh --debug
```

> multi-core: boot **`-smp 4`** (what `start.sh` defaults to). all cores boot and run kernel code in parallel  -  real AP bring-up (INIT-SIPI-SIPI), a per-CPU swapgs SYSCALL path, per-CPU GDT/TSS, and a per-CPU current task are in. the old ">1 core deadlocks the desktop" bug is gone. the remaining piece is actually scheduling **user** threads onto the secondary cores (the kernel runs on all of them today; user threads still run on the boot core).

If you're still on Windows the old WHPX launcher works too:

```powershell
# Build ISO and launch with the default QEMU profile
.\start.ps1

# Launch the existing ISO without rebuilding
.\start.ps1 -NoBuild

# Clean, rebuild, and launch
.\start.ps1 -Clean

# Launch with a GDB stub on localhost:1234
.\start.ps1 -Debug

# Boot with OVMF UEFI firmware instead of SeaBIOS
.\start.ps1 -UEFI

# Use KVM on Linux hosts
.\start.ps1 -KVM

# Bare-metal-style host CPU pass-through path (Linux/KVM hosts)
.\start.ps1 -BareMetal

# Override guest RAM
.\start.ps1 -Memory 12G
```

```bash
# Build only from WSL
wsl bash -lc 'cd /mnt/c/Users/genie/OS/src && make'

# Build the bootable ISO only
wsl bash -lc 'cd /mnt/c/Users/genie/OS/src && make iso'

# Fallback run path without hardware acceleration
wsl bash -lc 'cd /mnt/c/Users/genie/OS/src && make run-noaccel'
```

`start.ps1` currently builds by invoking `make iso` inside WSL from `src/`, then launches the generated `build/kurono.iso`. There is no `-NoAccel` switch in the current PowerShell launcher; the non-accelerated path is the `make run-noaccel` target.

---

## Current Snapshot

- **Architecture:** x86_64 bare metal, Multiboot2 kernel, standalone EFI loader, emergency EFI loader
- **Implementation:** freestanding C++17 + NASM assembly
- **Kernel model:** custom kernel, HAL, drivers, filesystem, shell, Linux runtime, hypervisor, and in-kernel Wayland compositor
- **Primary build artifacts:**
  - `build/kurono.elf`
  - `build/kurono_base.elf`
  - `build/kurono.iso`
  - `build/boot/kurono_efi.efi`
  - `build/boot/kurono_emergency.efi`
  - `iso/kurono.iso`
- **Default QEMU profile:** 10 GB RAM, 4 vCPUs, `-serial stdio`, Intel E1000 networking, SB16 + AC97 + Intel HDA audio, WHPX on Windows, KVM on Linux
- **What boots today:** graphical desktop with compositing window manager, lock screen, package manager, updater UI, Linux subsystem, Alpine guest boot, Debian rootfs staging and boot-time setup, installer, in-kernel Wayland compositor accepting real Wayland clients, and emergency recovery paths

---

## Recent Updates

### June 2026  -  moved to Linux + made the desktop actually usable

Ditched Windows/WHPX and got Kurono building and booting on Linux under KVM. It's way faster and it quietly fixed a pile of networking + input pain that WHPX was causing. Wrote a `start.sh` so booting is one command now.

- **Boot doesn't hang anymore.** The desktop was coming up black  -  turned out the large-model BSS section wasn't being zeroed, so a bunch of pointers were straight-up garbage. Fixed it in the linker script. The old SMP race that froze the desktop ~8s in is also fixed  -  `-smp 4` is the default now and all cores boot and run kernel code in parallel.
- **Reboot persistence on a real filesystem + multi-core bring-up.** KVFS state now survives a reboot through **KFS**  -  a from-scratch inode-based on-disk filesystem (the NVMe driver was dead until a missing `volatile` on the completion poll was fixed; it now does multi-page DMA via a PRP list). The user-data tree is stored as real files + dirs, not a blob. And the secondary cores are genuinely up: INIT-SIPI-SIPI bring-up, a per-CPU swapgs SYSCALL path, per-CPU GDT/TSS, per-CPU current task. Scheduling user threads onto the APs is the next step.
- **Networking actually works.** curl/HTTP goes end to end now  -  fixed a recv loop that gave up way too early, the FIN_WAIT half-close path, and ephemeral port selection. Pulled real pages off example.com and wikipedia over tap+NAT.
- **USB works over xHCI.** keyboard, mouse, and tablet all enumerate and report now (the DMA structs just needed proper page alignment). The tablet gives accurate absolute cursor positioning, which is also how I drive it headless.
- **Killed the bugs that made the desktop unusable:**
  - top taskbar was eating *every* click  -  you literally couldn't use anything. Now only the bar itself grabs clicks; everything else passes through.
  - Sign Out / Lock froze the whole machine (the lock screen never yielded, so USB input went dead). Fixed.
  - dragging a window left ghost trails  -  added a real clip stack so nothing paints outside its own window.
  - calculator clicks did nothing and right-click in Files was broken  -  both were window-local vs global coordinate mixups. Fixed.
- **UI glow-up.** Built a little theme system (KSS) so everything themes the same way instead of every app hardcoding its own colors. Settings and the Control Center are black/grey + modern now, the start button uses the actual boot logo instead of a hand-drawn "K", and a bunch of off-center text (it assumed 8px-per-char on a proportional font) got fixed.

### May 2026

- **Runtime stability and diagnostics**
  - Fixed Internet checksum generation so IPv4/TCP checksums are calculated from network-order bytes instead of host-order 16-bit words.
  - Added inbound IPv4 and TCP checksum validation with explicit serial drop logs, which is the right fix for the earlier "ARP works but TCP stalls" behavior.
  - Fixed TCP checksum coverage to include the real TCP header length, including TCP options.
  - Replaced the hottest 32bpp video render path with direct active-framebuffer writes instead of per-pixel `DrawPixel()` calls.
  - Added bounded `VideoPlayer` decode and frame-advance logs for Denji/KVID debugging.
  - Improved CPU topology fallback so multi-vCPU QEMU guests keep reporting multiple cores even when richer topology leaves are missing.
  - `/proc/cpuinfo` is now synthesized from `CPUDetect::GetInfo()` rather than stale hardcoded placeholders.
  - The legacy headless QEMU boot helper now also launches with 4 vCPUs to match the main launcher.

- **Debian delivery and update pipeline**
  - The large Debian rootfs is no longer embedded in the default ISO.
  - `kpkg install debian [nvidia|amd|auto|none]` downloads `debian-minbase.ext4` from the package server, stages it to disk, writes a pending-update marker, and prompts for reboot.
  - The boot-time `SystemUpdate` screen verifies the staged rootfs, boots Debian inside the hypervisor, updates apt sources, runs `apt-get update`, optionally installs vendor GPU drivers, and then continues to the desktop.
  - The default build keeps `EMBED_DEBIAN=0`; offline embedding remains available via `make EMBED_DEBIAN=1`.

- **Distribution and export changes**
  - `make iso` now produces both `build/kurono.iso` and the copied export artifact `iso/kurono.iso`.
  - The default ISO remains much smaller by moving bulky Debian payloads to runtime download/install flow.

### April 2026

- **In-kernel ELF64 dynamic linker (`ld-kurono`)**
  - PT_INTERP handoff from the ELF loader
  - PIE ASLR in the user address range
  - recursive `DT_NEEDED` resolution with SONAME deduplication
  - GNU hash and SYSV hash lookup
  - 23 x86_64 relocation types
  - `PT_GNU_RELRO` enforcement
  - static TLS support
  - vDSO mapping at `0x7FFFF7FFC000`
  - full SysV auxv construction
  - constructor trampoline for `DT_INIT` / `DT_INIT_ARRAY`
  - `dlopen`, `dlsym`, `dlclose`, `dlvsym`, `dladdr`, `dlerror`
  - `LD_DEBUG`, `LD_PRELOAD`, and `r_debug` rendezvous support

- **Linux userspace runtime plumbing**
  - Runtime layout seeder under `/system`
  - Wayland compositor socket at `/system/run/user/1000/wayland-0`
  - PulseAudio-compatible server at `/system/run/user/1000/pulse/native`
  - D-Bus session bus at `/system/run/user/1000/bus`
  - AF_UNIX sockets with `SCM_RIGHTS`, abstract namespace, and peer credentials
  - Firefox launcher path built on top of the Linux exec/syscall/runtime stack

- **Kernel platform services**
  - cgroups v2 hierarchy
  - TPM 2.0 support over CRB and FIFO interfaces
  - netfilter hook pipeline wired into the TCP/IP stack
  - CPUFreq governors and P-state control
  - demand paging, copy-on-write, and scheduler-backed process cloning
  - real `fork`, `waitpid`, and `execve` flow

- **Recovery and deployment**
  - Emergency EFI boot flow and GRUB recovery entries
  - Installer subsystem for GPT/MBR inspection and deployment
  - FAT32 ESP-safe write path
  - ext4 target layout generation and payload staging

---

## Feature Overview

### Boot and Kernel Foundation

- Multiboot2 boot path with long-mode x86_64 bring-up
- GDT, IDT, PIC remap, ISR stubs, syscall entry, and userspace return stubs
- Physical memory manager, virtual memory manager, buddy allocator, slab allocator, and large static kernel heap
- Round-robin scheduler and kernel task/process tracking
- PIT-backed timing at 1000 Hz with wait helpers and frame pacing
- Kernel panic path and boot-time kernel test suite
- Ring-3 entry support, per-process kernel stacks, saved user interrupt frames, and `int 0x80` syscall dispatch
- Demand-zero heap and `mmap` regions, page-fault recovery, copy-on-write fork cloning, and `munmap` region splitting
- User/kernel handoff for built-in user programs and PIE binaries launched through `ld-kurono`

### Display, Desktop, and Input

- **Display backends:** BGA (QEMU/Bochs), VirtIO GPU (2D resource management, scanout), Intel iGPU, NVIDIA dGPU, AMD GPU  -  selected via GPU probe at boot
- **GPU probe:** early PCI scan for all display controllers (class 0x03), hybrid topology detection (Optimus muxless/MUX, PowerXpress, dual discrete, virtual), framebuffer address validation via Intel DSPSURF register read
- **Display manager:** 10 predefined modes from 640x480 through 3840x2160, runtime mode switching, EDID reading, DPI scaling, gamma/brightness, VSync modes (off/on/adaptive)
- **Graphics driver:** double/triple buffering with SSE2 non-temporal store swap path, write-combining framebuffer remap via PAT, dirty region tracking (16 rectangles), frame pacing with FPS measurement, blend modes (alpha/additive/multiply), accessibility color-blindness filters
- **Wayland compositor:** in-kernel server listening at `/system/run/user/1000/wayland-0`, speaks the real libwayland wire protocol, advertises `wl_compositor` v5, `xdg_wm_base` v3, `wl_seat` v7, `zwp_linux_dmabuf_v1` v3, and 6 other globals  -  bridges `xdg_toplevel` surfaces into the Kurono window manager
- **Window manager:** compositing WM with z-ordering, server-side decorations (titlebar, 1px border, 10px corner radius, drop shadows), 8-direction resize, drag, minimize/maximize/close, per-window alpha, smooth-step animation (open/close/minimize/restore with taskbar fly-to effect), configurable shadow and animation settings
- **Desktop environment:** wallpaper (image or midnight-blue gradient), desktop icons with context menus, taskbar with start button/search/task icons/system tray/audio popup/clock, Alt-Tab window cycling
- Boot splash with logo and animated loading feedback
- 7 virtual consoles with the GUI running on `tty7`
- Lock screen with password flow and first-run setup wizard
- PS/2 keyboard and PS/2 mouse input paths, including packet resync and VirtualBox-friendly handling improvements

### Built-in Apps and Interactive Tools

| Component | Surface | Description |
|-----------|---------|-------------|
| Terminal v2.0 | GUI app | ANSI colors, cwd-aware prompt, Tab completion, 1024-entry deduplicated history, Ctrl+A/E/K/U/W, blinking cursor, scrollback |
| File Manager | GUI app | KVFS browser with icon and list views |
| Calculator | GUI app | Basic arithmetic keypad |
| Text Editor | GUI app | KVFS file create/load/save |
| Media Player v2.0 | GUI app | video/audio playback UI, codec badges, metadata caching, decode buffer, FPS overlay, audio routing across SB16/AC97/HDA |
| Denji | GUI app / shell command | windowed KVID/JPEG playback wrapper around `VideoPlayer` for the bundled Denji media asset |
| Settings | GUI app | 10-tab settings UI including display, wallpaper, system, accessibility, and a real Updates tab |
| Task Manager | GUI app | process list, CPU/memory stats, auto-refresh |
| Firefox Launcher | runtime app path | launches a Linux userspace Firefox-style workload through the syscall/runtime layer and seeded `/system` layout |
| Conduit | GUI app | event-dialogue viewer backed by `ConduitBridge` telemetry for system, package, GPU, guest, and command events |
| Mini Python 3 | shell/runtime | built-in `python` / `python3` interpreter with file, `-c`, and `-e` execution |
| Browser stub | GUI app | intentionally reduced to a removal notice because current freestanding browser candidates still require libc/POSIX stacks Kurono does not provide |

### Hybrid Shell and Command Environments

KuronoShell is a registry-driven shell that can execute native Kurono commands, Linux-style commands, Windows-style commands, KCL scripts, and guest-management commands from one terminal surface.

- **Environment model**
  - `switch kurono`
  - `switch linux`
  - `switch windows`
  - `bash`
  - `cmd`

- **Conflict handling**
  - When a command exists in more than one environment, the shell presents a numbered resolver instead of silently choosing the wrong implementation.

- **Cross-environment piping**
  - Example: `linux:ls /home | windows:findstr user`

- **Prompt model**
  - Linux-style `User@host:cwd$`
  - Windows-style `C:\>`
  - Kurono-native interactive prompt

#### Kurono-native commands

- Core shell:
  - `help`, `version`, `env`, `switch`, `clear`, `echo`, `set`, `alias`, `history`, `exit`
- System control:
  - `reboot`, `shutdown`, `restart`, `sysinfo`, `crash`, `kurono`
- Identity and session:
  - `whoami`, `uname`, `hostname`, `date`, `uptime`, `pwd`
- Runtime and advanced:
  - `usermode`, `gpu`, `vgpu`, `denji`, `codecs`
- Linux and Windows bridging:
  - `bash`, `linux`, `cmd`
- Guest and toolchain bridging:
  - `alpine`, `ffmpeg`, `ffprobe`, `apk`, `pwsh-setup`, `pwsh`, `powershell`
- Package and language commands:
  - `kpkg`, `install`, `remove`, `update`, `search`, `list`, `pkginfo`, `python`, `python3`

#### Linux command surface

The Linux environment exposes a real command set backed by kernel drivers and live runtime data rather than fixed strings.

- Filesystem and text:
  - `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `touch`, `cat`, `head`, `tail`, `wc`, `chmod`, `stat`, `df`, `du`, `ln`, `find`, `grep`, `which`, `tee`, `sort`, `uniq`, `tr`
- System and diagnostics:
  - `ps`, `kill`, `free`, `mount`, `dmesg`, `lspci`, `lsmod`, `drivers`, `lsblk`, `lsusb`, `lscpu`, `modprobe`, `modinfo`, `insmod`, `rmmod`, `dmidecode`, `hwinfo`, `top`, `iotop`, `uname`, `uptime`, `whoami`, `hostname`, `date`
- Networking:
  - `ifconfig`, `ip`, `ss`, `ping`, `wget`, `curl`
- Linux runtime and guests:
  - `linux-exec`, `syscall`, `vm`, `alpine`, `apk`, `debian`, `apt`

#### Windows command surface

- `dir`, `copy`, `move`, `del`, `type`, `md`, `rd`, `ren`, `cls`
- `findstr`, `tasklist`, `taskkill`, `systeminfo`, `ipconfig`, `ver`, `tree`, `attrib`, `chkdsk`

### Package Management and Update Flow

Kurono ships both a native package manager and guest-aware update flows.

- **Native package commands**
  - `kpkg install <package>`
  - `kpkg remove <package>`
  - `kpkg update`
  - `kpkg search <term>`
  - `kpkg list`
  - `pkginfo <package>`

- **Repository sync**
  - HTTP-backed sync against `kurono.satorut.com`
  - Settings > Updates can perform real repository fetches and show pending update counts

- **Debian guest provisioning**
  - `kpkg install debian`
  - Optional GPU hint: `nvidia`, `amd`, `auto`, or `none`
  - Download target: `/var/lib/kurono/debian-rootfs.ext4`
  - Pending marker: `/var/lib/kurono/pending-update`
  - Boot-time updater verifies the rootfs, boots Debian, refreshes apt sources, runs `apt-get update`, optionally installs guest GPU drivers, then returns to the desktop

- **GPU setup path**
  - `kpkg setup <target>` hands off to the guest GPU driver installer
  - Installer UI can kick off Alpine guest driver setup immediately or defer it to Settings > Updates or `kpkg setup alpine-auto`

- **Build-time guest payload policy**
  - Alpine guest kernel/initramfs are embedded when present
  - Debian rootfs is external by default and only embedded when `EMBED_DEBIAN=1`

### Media and Codec Stack

- Image decoding via stb-based PNG/JPEG/WebP glue
- Audio codec registry with WAV, MP3, AAC-LC, FLAC, MP4, and H.264 parse surfaces
- MP4 demuxing and H.264/AAC parser plumbing
- KVID container/player path for bundled video playback
- Cached video metadata to avoid repeated file reads during playback
- Enlarged decode buffers and faster direct blit path for 32bpp targets
- Audio routing across SB16, AC97, HDA, and fallback backends
- `ffmpeg` and `ffprobe` helper commands through Alpine where heavier media tooling is needed

### Hardware Drivers

Kurono currently ships 19 core hardware drivers plus additional kernel services layered on top of them.

| Driver / Subsystem | Details |
|--------------------|---------|
| GPU Probe | early PCI scan, hybrid topology (Optimus/PowerXpress), framebuffer address validation |
| BGA | Bochs Graphics Adapter, PCI BAR0 programming, 32bpp LFB, multi-resolution |
| VirtIO GPU | 2D resource management, scanout, virtqueue control/cursor queues, B8G8R8X8 format |
| Display Manager | 10-mode table, multi-backend routing (BGA/VirtIO/Intel/NVIDIA/AMD), EDID, DPI scaling |
| Graphics | double/triple buffering, NT-store swap, WC remap, dirty rects, blend modes, color filters |
| Wayland Server | in-kernel compositor, libwayland wire protocol, 9 globals, xdg_toplevel bridge |
| NVIDIA GPU | PCI detection, BAR mapping, VRAM query, passthrough preparation |
| AMD GPU | vendor detection, BAR0 MMIO, VRAM, arch ID, CU/clock/temp/fan/power reporting |
| Intel GPU | iGPU detection, generation identification, display surface register read |
| NVMe | admin + I/O queues, identify, read/write/flush |
| USB / xHCI | USB 3/2/1 host control, port enumeration, control/bulk transfers |
| Intel HD Audio | codec probing, CORB/RIRB, DMA playback |
| SB16 | ISA DMA audio, tones, buffers, master volume |
| AC97 | PCI DMA audio, NAM/NABM control, 48 kHz PCM, parallel operation with SB16 |
| CPU Detect | CPUID vendor/brand/model/family/features/topology reporting |
| Intel E1000 | PCI NIC init, descriptor rings, MAC readout, link status |
| PS/2 Keyboard | scancodes, compatibility init, event drain |
| PS/2 Mouse | polling, DPI scaling, packet re-sync, burst compatibility |
| PIT Timer | 1000 Hz timing and wait helpers |
| Serial | COM1 kernel logging |
| RTC | time/date for shell and taskbar |

Additional kernel platform services wired into the current tree include TPM 2.0, CPUFreq, netfilter, AF_UNIX sockets, PulseAudio server, D-Bus session bus, and the in-kernel Wayland compositor.

### Networking

Kurono includes a real custom TCP/IP stack instead of a guest-host shortcut.

- **Link layer**
  - Ethernet II framing
  - ARP request/reply and 32-entry cache

- **Internet layer**
  - IPv4 header build and parse
  - Receive-side checksum validation
  - Routing configuration for IP, subnet, gateway, and DNS
  - IPv6 stack with echo and neighbor-discovery plumbing

- **Transport layer**
  - ICMP ping with RTT measurement
  - UDP send/receive and `SendTo` / `RecvFrom`
  - TCP with all 11 classic states from `CLOSED` through `TIME_WAIT`
  - Correct TCP pseudo-header checksum generation and validation

- **Socket API**
  - `Socket()`, `Bind()`, `Connect()`, `Listen()`, `Accept()`, `Send()`, `Recv()`, `Close()`
  - Socket types: `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_RAW`

- **Limits and telemetry**
  - Up to 16 sockets
  - 8 KB RX/TX buffers
  - 1460-byte MSS
  - Per-protocol RX/TX/error/drop statistics

- **Policy and filtering**
  - 5-hook netfilter pipeline with `PRE_ROUTING`, `LOCAL_IN`, `FORWARD`, `LOCAL_OUT`, `POST_ROUTING`
  - Rule matching by IPv4 src/dst, ports, protocol, and interface

- **Guest and bridge support**
  - E1000-backed `eth0`
  - Tun/Tap subsystem initialization path
  - QEMU SLIRP host forwarding from host port 8080 to guest port 80

### Linux Subsystem and Userspace Runtime

This is more than a command shim. Kurono has an in-kernel Linux compatibility/runtime layer with real process, memory, fd, and syscall handling.

- **Syscall layer**
  - 67 syscall numbers defined
  - 35+ implemented handlers, including `read`, `write`, `open`, `close`, `lseek`, `brk`, `fork`, `waitpid`, `execve`, `stat`, `fstat`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink`, `dup`, `dup2`, `ioctl`, `writev`, `mmap`, `munmap`, `nanosleep`, `getdents64`, `clock_gettime`, and more

- **Process model**
  - Up to 16 Linux processes
  - 64 file descriptors per process
  - Parent/child tracking
  - Saved interrupt-frame and user-context state
  - Scheduler-backed ring-3 tasks

- **Memory model**
  - Lazy `brk`
  - Anonymous `mmap`
  - Copy-on-write address-space cloning
  - Recoverable user page faults
  - Mappings currently constrained below 4 GB because the syscall ABI layer still follows a 32-bit-style pointer convention

- **Proc/sys identity**
  - Reports as `Linux 6.8.0-kurono`
  - `/proc`, `/sys`, TTY/PTY, and proc-style generated files

- **Runtime layout seeding**
  - Approximately 120 directories under `/system`
  - Generated config files under `/system/etc`
  - Generated proc/sys content under `/system/proc` and `/system/sys`
  - Per-user runtime directories under `/system/run/user/1000`

- **IPC and session services**
  - AF_UNIX socket table with `STREAM`, `DGRAM`, and `SEQPACKET`
  - `SCM_RIGHTS` file descriptor passing
  - D-Bus session bus
  - PulseAudio-compatible server
  - Wayland compositor socket

- **Dynamic linking**
  - `ld-kurono` interpreter path for PIE userspace binaries
  - `dlopen` family support
  - TLS, RELRO, auxv, vDSO, and constructor handling

- **Commands and launchers**
  - `linux-exec`
  - `syscall`
  - `usermode`
  - Firefox launcher runtime path

### Virtualization

Kurono includes a Type 1 hypervisor stack designed for Linux guest boot and device virtualization.

- **CPU virtualization**
  - Intel VT-x and AMD-V detection
  - VMXON / VMXOFF and SVM enable/disable
  - VMCS and VMCB management

- **Memory virtualization**
  - EPT on Intel and NPT on AMD
  - 2 MB and 1 GB large-page support
  - Guest physical memory layout, E820 tables, BDA/IVT setup, and MMIO regions

- **Device virtualization**
  - Virtual PIC, APIC, PIT, HPET, serial, IDE disk, and guest memory plumbing
  - Guest serial bridge into the Kurono shell

- **Guest lifecycle**
  - `CreateVM`, `RunVM`, `PauseVM`, `ResumeVM`, `DestroyVM`
  - Per-cycle VM run helpers
  - VMCALL support for NOP, info, shutdown, and reboot

- **Linux guest boot**
  - bzImage parser and Linux boot protocol v2.15
  - boot params, cmdline, and initrd load support

- **Shipped guest profiles**
  - Alpine Linux guest boot through embedded `vmlinuz-virt` and `initramfs-virt`
  - Debian rootfs staging and post-install/update boot flow

- **IOMMU and passthrough**
  - Intel VT-d and AMD-Vi plumbing
  - PCI passthrough support and GPU passthrough-oriented paths

- **Correctness work already landed**
  - VMCS link pointer fixes
  - 64-bit EPT pointer writes
  - VM-exit control correctness
  - guest CR0 fixed-bit compliance
  - aligned allocation cleanup
  - 64-bit I/O and MSR bitmap addressing

### Recovery, Installation, and Deployment

- GRUB ISO boot with multiple entries, including normal, debug, text-mode, and emergency paths
- Standalone normal EFI and emergency EFI loaders
- Emergency shell for minimal recovery when the desktop path is unavailable
- Installer shell/UI workflow for disk scan, partition inspection, and deployment planning
- GPT and MBR partition parsing over NVMe-backed targets
- FAT32 EFI System Partition detection and safe file deployment
- ext4 target layout generation for `/system`, `/etc`, `/boot`, and `/apps`
- Embedded deployment payloads:
  - kernel
  - normal EFI loader
  - emergency EFI loader
  - fallback `BOOTX64.EFI`
- Two-stage build (`kurono_base.elf` then final `kurono.elf`) so installable payloads can be embedded in the shipping kernel

### Security, Users, Scripting, and Filesystem

- **SUPR privilege system**
  - privilege escalation with timeout
  - audit logging
  - Guest/User/Admin/Root roles
  - salted password hashing

- **User management**
  - multiple users
  - groups
  - home directories
  - runtime session tracking

- **KCL**
  - variables
  - functions
  - `if` / `else`
  - `while` / `for`
  - imports
  - math helpers like `sqrt` and `rand`
  - `print` and `set`

- **KVFS**
  - in-memory virtual filesystem
  - directories, files, symlinks, devices, pipes, mountpoints
  - POSIX-style permissions
  - common file operations across shell and apps
  - pre-seeded `/home`, `/etc`, `/tmp`, `/var/log`, `/usr/bin`

- **ext4 and FAT32**
  - ext4 parsing for the Linux layer and installer targets
  - FAT32 write path focused on ESP deployment rather than general desktop storage

---

## Architecture

```text
Kurono OS
+-- Boot and Kernel Core
|   +-- Multiboot2 / EFI loaders
|   +-- PMM / VMM / scheduler / syscall entry
|   +-- Ring-3 userspace path
|   +-- Demand paging + copy-on-write
|
+-- Desktop and Native UX
|   +-- GPU probe / display manager / graphics driver
|   +-- Wayland compositor (in-kernel, wire protocol)
|   +-- Window manager (compositing, animations, shadows)
|   +-- Lock screen / desktop / taskbar / settings
|   +-- Terminal / media / file manager / task manager
|
+-- Shell and Runtime Surfaces
|   +-- Kurono-native command set
|   +-- Linux command environment
|   +-- Windows command environment
|   +-- KCL scripting
|   +-- Mini Python 3
|
+-- Networking
|   +-- Ethernet / ARP / IPv4 / IPv6
|   +-- ICMP / UDP / TCP
|   +-- Netfilter hooks
|   +-- AF_UNIX sockets
|
+-- Linux Compatibility Runtime
|   +-- Syscall layer
|   +-- /system runtime layout
|   +-- Wayland / Pulse / D-Bus
|   +-- ld-kurono dynamic linker
|
+-- Virtualization
|   +-- VT-x / AMD-V
|   +-- EPT / NPT
|   +-- Virtual devices
|   +-- Alpine guest boot
|   +-- Debian guest staging/update path
|
+-- Installation and Recovery
|   +-- Emergency EFI boot
|   +-- Installer
|   +-- FAT32 ESP deployment
|   +-- ext4 target layout generation
|
+-- Storage and Security
    +-- KVFS
    +-- ext4 parser
    +-- SUPR
    +-- package manager
```

### Source Layout

```text
src/
  boot/         x86_64 Multiboot entry, EFI loader, linker scripts, early boot
  kernel/       heap, PMM, VMM, buddy/slab allocators, ELF loader, panic, time
  hal/          interrupts, syscall entry, cpufreq hooks, architecture glue
  drivers/      display, audio, storage, NIC, TPM, GPU, input, runtime daemons
  ui/           desktop, taskbar, window manager, font, lock screen, Wayland compositor
  apps/         terminal, media player, file manager, settings, task manager, etc.
  shell/        KuronoShell plus Linux and Windows command environments
  fs/           VFS, KVFS, FAT32
  net/          TCP/IP, netfilter, IPv6, Tun/Tap, AF_UNIX
  linux/        syscall layer, ext4, init, signals, ld-kurono, dual-boot runtime
  virt/         VMM, EPT/NPT, guest memory, vdevices, guest boot, passthrough
  security/     SUPR privilege system
  kcl/          Kurono Command Language
  packages/     kpkg package manager
  proc/         scheduler and cgroups
  media/        codecs, demuxers, parsers, video player
  system/       installer, update UI, runtime layout, logging, user management
  tests/        kernel test suite
  third_party/  stb_image, stb_truetype glue
```

### Top-Level Layout

```text
OS/
  Alpine/       embedded Alpine guest assets
  build/        kernel, ISO, EFI, and intermediate outputs
  iso/          copied ISO export artifact
  src/          full kernel and subsystem source tree
  tools/        helper scripts and QEMU utilities
  start.ps1     Windows/WSL build-and-launch entry point
  STATUS.md     detailed implementation status log
  README.md     project overview
  LICENSE       GNU GPL v2
```

---

## Building from Source

### Prerequisites

- Windows host with WSL, or a Linux host directly
- x86_64-elf cross toolchain, or native `g++ -m64` fallback
- NASM
- `grub-mkrescue` and `xorriso` for ISO creation
- `gnu-efi` headers and libraries for EFI loader builds
- QEMU

### Common Make Targets

Run these from `src/` in an environment that has the required toolchain and matching QEMU backend available:

```bash
make                 # Build the kernel ELF
make iso             # Build ISO + copy to ../iso/kurono.iso
make run             # ISO boot under QEMU with the WHPX-oriented profile
make run-noaccel     # Kernel boot under QEMU without acceleration
make run-kvm         # Kernel boot under QEMU with KVM
make run-passthrough GPU_BDF=01:00.0
make debug           # Kernel boot with GDB stub on :1234
make clean           # Remove build output
```

### Build Outputs

- `build/kurono.elf`
- `build/kurono_base.elf`
- `build/kurono.iso`
- `iso/kurono.iso`
- `build/boot/kurono_efi.efi`
- `build/boot/kurono_emergency.efi`

### Guest Payload Options

```bash
# Default: Debian rootfs is NOT embedded
make iso

# Offline / self-contained ISO with Debian rootfs embedded
make EMBED_DEBIAN=1 iso
```

Alpine guest assets are linked when `Alpine/vmlinuz-virt` and `Alpine/initramfs-virt` are present. Debian rootfs embedding is opt-in because it significantly increases the ISO size.

---

## QEMU Profile

### Default launcher behavior (`start.ps1`)

- Builds by calling `make iso` in WSL
- Launches `build/kurono.iso`
- Allocates 10 GB RAM by default
- Exposes 4 vCPUs
- Uses `-serial stdio`
- Attaches:
  - SB16
  - Intel HDA + HDA duplex codec
  - AC97
  - Intel E1000
- Uses SLIRP networking with `hostfwd=tcp::8080-:80`
- Uses DirectSound on native Windows QEMU or PulseAudio on WSLg-backed QEMU
- Selects:
  - `qemu64,+vmx` on Intel WHPX hosts
  - `qemu64,+svm` on AMD WHPX hosts
  - `-cpu host` on KVM paths
- Supports OVMF UEFI via `-UEFI`

### Makefile run targets

- `make run`
  - ISO boot
  - WHPX-oriented accelerator profile
  - 4 vCPUs
  - VGA std
  - E1000 + SB16 + virtio-balloon
- `make run-noaccel`
  - direct kernel boot
  - no accelerator
- `make run-kvm`
  - direct kernel boot
  - `-cpu host`
  - KVM
  - Q35 machine
- `make run-passthrough`
  - direct kernel boot
  - VFIO GPU passthrough path
  - `GPU_BDF` override

---

## Known Limitations

- The Wayland compositor is functional at the wire-protocol level (registry, globals, surface creation, xdg_toplevel configure) but the surface-to-framebuffer blit bridge and input event forwarding are still stubbed.
- Live desktop storage is KVFS-first and RAM-backed, but it now **persists across reboot through KFS**  -  a from-scratch inode-based on-disk filesystem on the NVMe data disk. The user-data subtrees (`/home`, `/etc`, `/root`) are stored as real files + directories and restored at boot. KVFS stays the runtime fs; KFS is the on-disk persistence layer (not yet a fully mounted root).
- The Linux syscall layer still uses a 32-bit-style pointer ABI constraint, so user mappings and pointers currently need to remain below 4 GB.
- General ELF loading from disk is still limited; the Linux subsystem primarily executes built-in or staged runtime programs rather than arbitrary disk-resident binaries.
- USB host-controller work is present, but full USB HID interrupt transfer polling is not complete.
- NVIDIA GPU detection and passthrough preparation exist, but BGA remains the primary display device and NVIDIA is not yet the default native desktop renderer.
- ext4 write support is still partial; sparse allocation and full directory-growth handling are not complete.
- FAT32 support is focused on EFI System Partition deployment rather than full long-filename desktop storage workloads.
- Nested or hardware-assisted virtualization still depends on host support for VT-x or AMD-V through QEMU, WHPX, or KVM.
- Browser functionality is intentionally not shipped as a real native browser because current C/C++ browser engines still assume libc, POSIX, X11, SDL, Qt, or similar host stacks that Kurono does not provide inside its freestanding kernel environment.

---

## License

Kurono OS is licensed under the GNU General Public License v2. See [LICENSE](LICENSE) for the full text.