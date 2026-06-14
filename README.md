# Kurono OS

A bare-metal x86_64 operating system with a hybrid kernel that combines a native desktop OS, a Linux-compatible syscall/runtime layer, a Windows command environment, a package/update pipeline, and a Type 1 hypervisor into one bootable image.

Built from scratch in freestanding C++17 and x86 assembly - no libc in the kernel, no borrowed POSIX runtime under the kernel, and no existing host kernel reused as the core OS. Kurono boots as a Multiboot2 ELF kernel, also produces standalone EFI loaders, runs in QEMU with WHPX or KVM, supports emergency recovery boot, can deploy onto FAT32/ext4 installer targets, and ships both Alpine and Debian guest paths.

> **19 hardware drivers - 150+ shell commands across the Kurono / Linux / Windows environments - full TCP/IP stack - in-kernel ELF64 dynamic linker (ld-kurono) - a Linux syscall runtime with real concurrent threads/futex, epoll, mprotect W^X and SCM_RIGHTS fd-passing - an in-kernel Wayland compositor that renders real musl-compiled Wayland clients on screen - PulseAudio/D-Bus runtime services - multi-backend display manager (BGA, VirtIO GPU, Intel, NVIDIA, AMD) - hybrid GPU topology detection (Optimus, PowerXpress) - emergency EFI boot - installer stack - Alpine VM - Debian on-demand rootfs and update pipeline**

> **On the browser question:** Kurono does *not* ship a cut-down toy browser, and the GUI "Browser" tile is a deliberate placeholder. The real answer is **Firefox on the Linux runtime**  -  a full Firefox 140.11.0esr is already cross-compiled against musl + Wayland (174 MB `libxul.so`), and the OS provides the syscalls, IPC and Wayland surface it targets. Loading its shared-library closure through `ld-kurono` and lifting the current <4 GB pointer-ABI limit is the active frontier, not an impossibility. `curl <url>` is the working HTTP path today.

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

> multi-core: boot **`-smp 4`** (what `start.sh` defaults to). all cores boot and run kernel code in parallel  -  real AP bring-up (INIT-SIPI-SIPI), a per-CPU swapgs SYSCALL path, per-CPU GDT/TSS, and a per-CPU current task are in. the old ">1 core deadlocks the desktop" bug is gone. And the **secondary cores now run real ring-3 Linux user processes in parallel with the desktop**: the per-CPU rewrite (cross-core ready-queue lock + atomic per-CPU claim, a per-CPU user-execution context replacing the old single-active-process model, per-CPU syscall state) is done, and an application processor has been shown launching `/usr/bin/mhello`, running it, and exiting 0 while the desktop rendered untouched. It's opt-in (`./start.sh --apsched`); the default boot parks the APs exactly as before. And the APs are now **preemptive**  -  each arms a per-CPU LAPIC timer that time-slices the user threads it runs, instead of only switching cooperatively at syscalls. Running `clone` sibling threads across cores and load balancing are the next step. See [docs/developers/proc/SMP.md](docs/developers/proc/SMP.md).

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

### GRUB boot menu

`make iso` generates the GRUB menu (defined in `src/Makefile`). On UEFI it defaults
to the EFI loader; on BIOS it defaults to the Multiboot2 kernel. The entries are:

- **Kurono OS (EFI Direct)**  -  chainload the standalone EFI loader
- **Kurono OS (Emergency EFI)**  -  chainload the emergency EFI loader
- **Kurono OS (Multiboot2)**  -  the normal kernel boot (BIOS default), straight to the desktop
- **Kurono Setup**  -  boots `kurono.setup=1` and runs the graphical installer / first-setup wizard
- **Kurono OS (Emergency Multiboot2)**  -  text-mode emergency kernel
- **Kurono OS (CLI Multiboot2)**  -  the headless/CLI boot profile (`KURONO_BOOT_PROFILE=cli`)
- **Kurono OS (Multiboot1)**  -  Multiboot1 fallback
- **Kurono OS (Debug  -  GRUB verbose)**  -  verbose GRUB diagnostics
- **Kurono OS (Text Mode  -  no framebuffer)** / **(gfxpayload=keep)**  -  display fallbacks

> **Installer / first-run setup.** The graphical installer (a real disk-install
> wizard: language → keyboard → **network/Wi-Fi** → disk → partition → filesystem →
> **guest-OS install (Debian/etc.)** → user → summary → progress) has its own
> dedicated **"Kurono Setup"** GRUB entry (`kurono.setup=1`); the normal entry
> boots straight to the desktop. It can also be launched on demand from the
> desktop **"Install Kurono"** shortcut or the shell `installer` command. It is
> never the default boot  -  so it can't strand a first-boot user on a black
> installer screen before input is up. See
> [docs/developers/system/INSTALLER.md](docs/developers/system/INSTALLER.md).

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
- **What boots today:** graphical desktop with compositing window manager, lock screen, package manager, updater UI, Linux subsystem, installer, in-kernel Wayland compositor accepting real Wayland clients, and emergency recovery paths. The hypervisor's Alpine/Debian guest-boot path is implemented, but **booting a Linux guest needs the host to expose nested VT-x to Kurono**  -  under plain nested KVM (Kurono itself a guest) the guest VM-entry currently fails (VMX entry error), so guest boot is verified only where nested VMX is available (see *Known Limitations*)

---

## Recent Updates

### June 2026  -  moved to Linux + made the desktop actually usable

Ditched Windows/WHPX and got Kurono building and booting on Linux under KVM. It's way faster and it quietly fixed a pile of networking + input pain that WHPX was causing. Wrote a `start.sh` so booting is one command now.

- **Boot doesn't hang anymore.** The desktop was coming up black  -  turned out the large-model BSS section wasn't being zeroed, so a bunch of pointers were straight-up garbage. Fixed it in the linker script. The old SMP race that froze the desktop ~8s in is also fixed  -  `-smp 4` is the default now and all cores boot and run kernel code in parallel.
- **Reboot persistence on a real filesystem + multi-core bring-up.** KVFS state now survives a reboot through **KFS**  -  a from-scratch inode-based on-disk filesystem (the NVMe driver was dead until a missing `volatile` on the completion poll was fixed; it now does multi-page DMA via a PRP list). The user-data tree is stored as real files + dirs, not a blob. And the secondary cores are genuinely up: INIT-SIPI-SIPI bring-up, a per-CPU swapgs SYSCALL path, per-CPU GDT/TSS, per-CPU current task. The **per-CPU scheduling foundation** for running user threads on them then landed  -  a cross-core ready-queue lock + atomic per-CPU claim, a per-CPU user-execution context (replacing the single-active-process model), and per-CPU syscall state, all verified non-regressing under `-smp 4` (desktop renders, no faults). And the AP dispatch loop now **enters ring-3 on the secondary cores**: opt-in via `./start.sh --apsched`, an application processor was shown launching `/usr/bin/mhello`, running its syscalls, and exiting 0 in parallel with the desktop; the APs are then made **preemptive** with a per-CPU LAPIC timer. Spreading a process's `clone` threads across multiple cores and load balancing are the remaining steps. See [docs/developers/proc/SMP.md](docs/developers/proc/SMP.md).
- **A real Linux GUI client renders through the in-kernel Wayland compositor.** This is the milestone the rest of the runtime was building toward. The userspace layer grew the pieces a real GUI app actually blocks on: preemptive concurrent threads (`clone`/`CLONE_THREAD`) backed by a real `futex` (the multithreaded pthread gate passes), `epoll`/`poll`, `mprotect` with W^X enforcement, `memfd`/file-backed `mmap`, and `SCM_RIGHTS` fd-passing end to end. With those in place, a Wayland client compiled with **musl-gcc** (`wl_shm_test`, embedded in the kernel and launched by the `wltest` command) connects to the compositor over the AF_UNIX socket, fd-passes a `wl_shm` buffer, and the compositor blits its pixels into a real window-manager window with damage tracking and forwards pointer input back to it. That's the whole `wl_shm` + `xdg-shell` render path working  -  not just the wire protocol. (Keyboard-to-client forwarding is the next wire-up.)
- **Firefox is cross-compiled for Kurono.** A real **Firefox 140.11.0esr** is built against **musl** (clang/lld, `--disable-jit`, `cairo-gtk3-wayland`)  -  `firefox`/`firefox-bin` plus a 174 MB `libxul.so` Gecko engine, build rc=0, all of Alpine's musl portability patches applied. This is the concrete rebuttal to "a freestanding OS can't have a browser": the engine targets musl + Wayland, both of which Kurono's runtime provides, and the launcher already does a real `execve`. What's left to actually run it is bringing libxul's `.so` dependency closure onto the OS and loading it through `ld-kurono`, plus lifting the <4 GB user-pointer ABI limit.
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
  - The boot-time `SystemUpdate` screen verifies the staged rootfs, boots Debian inside the hypervisor (where nested VMX is available  -  see *Known Limitations*), updates apt sources, runs `apt-get update`, optionally installs vendor GPU drivers, and then continues to the desktop.
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
  - 24 x86_64 relocation types handled
  - `PT_GNU_RELRO` enforcement
  - static TLS support
  - vDSO page built at `0x7FFFF7FFC000` (currently **not advertised** to the loader  -  `AT_SYSINFO_EHDR=0`  -  because the synthesized page lacks a musl-parseable `PT_DYNAMIC`, so musl falls back to plain `syscall`s)
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
  - Dedicated **"Kurono Setup"** GRUB entry (`kurono.setup=1`) that runs the
    graphical installer / first-setup wizard; the main entry still boots
    straight to the desktop (`kurono.autologin=1`)
  - Installer subsystem for GPT/MBR inspection and deployment
  - Guided setup wizard: language, keyboard, **network/Wi-Fi**, disk,
    filesystem, **administrator account**, **hostname + timezone + prefs**, and
    **optional Linux guests** (Debian / Alpine / Python) that reuse the
    `kpkg install debian` + system-update reboot flow
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
- **Wayland compositor:** in-kernel server listening at `/system/run/user/1000/wayland-0`, speaks the real libwayland wire protocol, advertises `wl_compositor` v5, `xdg_wm_base` v3, `wl_seat` v7, `zwp_linux_dmabuf_v1` v3, and 5 other globals (9 in total: also `wl_subcompositor`, `wl_shm`, `wl_output`, `wl_data_device_manager`, `zxdg_decoration_manager_v1`). It goes past the wire protocol: an `xdg_toplevel` surface becomes a real window-manager window, the client's `wl_shm` buffer (fd-passed via `SCM_RIGHTS`) is blitted into that window's framebuffer rect with damage tracking, and pointer enter/leave/motion/button events are forwarded back to the focused client  -  verified with a real **musl-compiled** Wayland client (`wl_shm_test`, run via `wltest`). Keyboard-to-client forwarding is implemented but not yet wired into the input loop; `zwp_linux_dmabuf` GPU buffers are advertised but only `wl_shm` software buffers are composited today
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
| Settings | GUI app | 12-tab settings UI (Display, Sound, Network, Storage, Power, Personalize, Security, Packages, Updates, System, About, Accessibility)  -  incl. a real Updates tab |
| Task Manager | GUI app | process list, CPU/memory stats, auto-refresh |
| Firefox Launcher | runtime app path | real `execve` of `/apps/firefox/firefox` through the Linux syscall/runtime layer (seeded `/system`, `firefox.env`, `LD_LIBRARY_PATH` from the runtime layout). A real Firefox 140.11.0esr is cross-compiled (musl, Wayland); shipping + loading its 174 MB libxul closure via `ld-kurono` is the remaining bring-up step |
| Conduit | GUI app | event-dialogue viewer backed by `ConduitBridge` telemetry for system, package, GPU, guest, and command events |
| Mini Python 3 | shell/runtime | built-in `python` / `python3` interpreter with file, `-c`, and `-e` execution |
| KJ (Kurono JavaScript) | shell/runtime | built-in `kj` / `node` JS-subset interpreter (var/let/const, closures, objects, arrays, `Math.*`) with file + `-c` execution; host bindings drive the KSS styling/animation layer |
| Browser (placeholder) | GUI app | a deliberate placeholder tile  -  `curl <url>` is today's working HTTP path. The real browser strategy is **Firefox on the Linux runtime** (cross-compiled against musl + Wayland, in bring-up; see the Firefox Launcher row), not a cut-down freestanding engine |

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
  - 177 `LSYS_*` syscall numbers defined
  - ~155 implemented handlers (case arms in `src/linux/linux_syscall.cpp`), including `read`, `write`, `open`, `close`, `lseek`, `brk`, `fork`, `waitpid`, `execve`, `stat`, `fstat`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink`, `dup`, `dup2`, `ioctl`, `writev`, `mmap`, `munmap`, `nanosleep`, `getdents64`, `clock_gettime`, and more
  - the syscalls a real GUI app actually blocks on are implemented for real, not stubbed: `futex` (FUTEX_WAIT/WAKE), `clone`/`CLONE_THREAD`, `epoll_create1`/`epoll_ctl`/`epoll_wait`, `poll`/`ppoll`, `mprotect` (W^X with region splitting), `memfd_create`, file-backed `mmap`, and `sendmsg`/`recvmsg` carrying `SCM_RIGHTS`

- **Process model**
  - Up to 16 Linux processes
  - 64 file descriptors per process
  - Parent/child tracking
  - Saved interrupt-frame and user-context state
  - Scheduler-backed ring-3 tasks

- **Threads and concurrency**
  - `clone`/`CLONE_THREAD` threads sharing one address space
  - a real `futex` (FUTEX_WAIT/WAKE) for blocking and wakeups
  - preemptive switching on the user-thread path  -  verified by a multithreaded pthread gate test (a two-thread counter reliably reaches 2000)
  - `epoll`/`poll` event loops backed by eventfd, timerfd, and sockets

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
  - `SCM_RIGHTS` file descriptor passing  -  exercised end to end: a Wayland client fd-passes its `wl_shm` buffer to the compositor, which maps and composites it
  - D-Bus session bus
  - PulseAudio-compatible server
  - Wayland compositor socket  -  a real musl-compiled client (`wl_shm_test`) connects, renders, and receives pointer input through it

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
  - VMCALL support for NOP, info, shutdown, reboot, GPU/audio/net/9p bridges,
    and the **KSA read-only authorization-verdict channel** (`0x4B`)

- **KSA secure-authorization context** (see Security section)
  - EPT-isolated prompt region carved from physical memory and unmapped from the
    main-OS page tables; used to back hypervisor-arbitrated privilege prompts

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

- GRUB ISO boot with multiple entries, including normal, **Kurono Setup**, debug, text-mode, and emergency paths
  - the first/default entry boots straight to the desktop (`kurono.autologin=1`)
  - the **"Kurono Setup"** entry boots with `kurono.setup=1` and runs the
    graphical installer / first-setup wizard before the desktop
- Standalone normal EFI and emergency EFI loaders
- Emergency shell for minimal recovery when the desktop path is unavailable
- Graphical setup wizard: language, keyboard, network (wired + an honest Wi-Fi
  config screen  -  no radio driver in this build, e1000/virtio-net wired only),
  disk, filesystem, administrator account, hostname/timezone/preferences, and
  optional Linux guests (Debian/Alpine/Python)
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
  - Guest/User/Admin/Root/Sovereign roles
  - salted password hashing

- **KSA  -  Kurono Secure Authorization (hypervisor-backed privilege prompts)**
  - Kurono's answer to Windows UAC, but the prompt is rendered and arbitrated
    inside a hypervisor-isolated region the main OS has **no page-table mapping
    into**  -  ring-0 malware in the main OS can't read it or auto-approve it
  - the verdict (approve/deny + salted credential hash) crosses back through a
    single **read-only** VMCALL channel (`0x4B`); there is no path to inject a
    forged approval from the main OS
  - auth policy via `supr policy --auth=passwd|kvault|both`; disabling either
    factor needs explicit acknowledgement, and disabling *both* requires
    `--sovereign-override` (Sovereign role only)  -  all changes audited even when
    KSA is off
  - on a host without nested VMX (e.g. Kurono under KVM/QEMU) the prompt runs as
    an **EPT-isolated guest context** rather than a separately launched VM; the
    memory-isolation and read-only-channel guarantees still hold and are proven
    by a runtime self-test (`kurono.ksa.test=1` / `supr selftest`). See
    `docs/developers/security/KSA.md`.

- **User management**
  - multiple users
  - groups
  - home directories
  - runtime session tracking

- **KCL** (Kurono Command Language)  -  a complete tree-walking scripting language
  - typed values: int, float, string, bool, list, none (real doubles)
  - variables (`set x = 10`), arithmetic, string concat, comparisons, boolean logic (`and`/`or`/`not`)
  - control flow: `if` / `elif` / `else` / `end`, `while`, `for x in a..b` and `for x in <list/string>`
  - functions with parameters, return values, and recursion (`func name(args) ... end`)
  - lists: literals `[1, 2, 3]`, indexing (incl. negative), `append` / `remove` / `len`
  - imports (`import lib.kcl`) and `#` comments / shebang (`#!/kcl`)
  - stdlib builtins: `print` `input` `len` `str` `int` `float` `sqrt` `rand` `abs` `min` `max` `type` `read` `write` `exists` `exec` `sleep` `upper` `lower`
  - run via `kcl script.kcl`, `kcl -c "code"`, a `.kcl` shebang, or double-click in the File Manager
  - errors are reported with line numbers and never crash the OS

- **KJ** (Kurono JavaScript)  -  a freestanding JavaScript-subset interpreter for scripting the desktop
  - lexer + recursive-descent parser → AST, then a tree-walking evaluator over a tagged value type (undefined / null / bool / number(double) / string / array / object / function-closure). No libc, no STL.
  - `var` / `let` / `const`, `function` declarations + expressions with **closures**, `return`
  - objects `{ k: v }` with dot + bracket access/assignment + nested + methods; arrays `[..]` with index, `.length`, `.push`/`.pop`, `for..of`
  - arithmetic `+ - * / %`, comparisons `== != === !== < > <= >=`, `&& || !`, ternary `?:`, prefix/postfix `++ --`, `typeof`, string concat, `// /* */` comments
  - `if`/`else`, `while`, classic `for(;;)`, `break`/`continue`; `Math.{floor,ceil,round,abs,sqrt,sin,cos,min,max,pow,random}`, `Math.PI`
  - **host bindings** so scripts drive the live UI: `console.log`, `kss.set/get/transition/keyframes/play` (drives the KSS stylesheet/animation layer), `ui.notify`
  - run via `kj file.js`, `kj -c "code"`, or `KJ::Execute()` from C++; errors are captured (never crash the OS)
  - verified by an 11-test self-test suite (`kurono.kjtest`, 11/11 PASS headless) covering every feature plus the KSS bindings

- **KSS** (Kurono Style Sheet)  -  theme tokens **plus** a scriptable styling/animation layer
  - theme token set (colors + metrics) overridable from `/etc/kurono/ui.conf`
  - a `Sheet` layer: named style rules (selectors like `button:hover`) with a property bag (bg/fg/border/accent/shadow + radius/pad/border-width/font/opacity/scale/translate), per-property **transitions** that ease automatically on value change, and named **keyframe** tracks the compositor samples per frame
  - an `Anim` tween engine (eased float/color tweens keyed by stable id) wired into the keep-rendering gate so motion never stalls mid-flight
  - live animations: window open/close **fade + scale**, notification slide-in/out, control-center panel slide, button hover/focus color transitions  -  all driven per-frame by the compositor

- **KVFS**
  - in-memory virtual filesystem
  - directories, files, symlinks, devices, pipes, mountpoints
  - POSIX-style permissions
  - common file operations across shell and apps
  - one **canonical tree under `/kurono`** (`system`, `linux`, `windows`, `apps`, `user`, `packages`, `runtime`, `var`); the old top-level names are **compat symlinks** into it (`/system -> /kurono/system`, `/home -> /kurono/user/home`, `/etc -> /kurono/system/config`, `/bin`, `/lib`, `/usr/{bin,lib}`, `/tmp`, `/proc`, `/dev`, `/var`, `/apps`, `/windows`), installed at the earliest fs init by `KVFS::InstallCanonicalLayout()` and centralized in `src/system/kpaths.h` (see `docs/developers/fs/KVFS.md`)
  - pre-seeded `/home`, `/etc`, `/tmp`, `/var/log`, `/usr/bin` (all resolving through the overlay)

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

- The Wayland compositor renders real clients  -  an `xdg_toplevel` surface becomes a managed window and its `wl_shm` buffer (fd-passed via `SCM_RIGHTS`) is blitted to the framebuffer with damage tracking, and pointer input is forwarded to the focused client (proven with a real musl-compiled client, `wl_shm_test`). The remaining gaps are **keyboard**-to-client forwarding (the handler exists but isn't wired into the input loop) and `zwp_linux_dmabuf` GPU buffers (only `wl_shm` software buffers are composited today).
- Live desktop storage is KVFS-first and RAM-backed, but it now **persists across reboot through KFS**  -  a from-scratch inode-based on-disk filesystem on the NVMe data disk. The user-data subtrees (`/home`, `/etc`, `/root`) are stored as real files + directories and restored at boot. KVFS stays the runtime fs; KFS is the on-disk persistence layer (not yet a fully mounted root).
- The Linux syscall layer still uses a 32-bit-style pointer ABI constraint, so user mappings and pointers currently need to remain below 4 GB.
- General ELF loading from disk is still limited; the Linux subsystem primarily executes built-in or staged runtime programs rather than arbitrary disk-resident binaries.
- USB host-controller work is present, but full USB HID interrupt transfer polling is not complete.
- NVIDIA GPU detection and passthrough preparation exist, but BGA remains the primary display device and NVIDIA is not yet the default native desktop renderer.
- ext4 write support is still partial; sparse allocation and full directory-growth handling are not complete.
- FAT32 support is focused on EFI System Partition deployment rather than full long-filename desktop storage workloads.
- Hardware-assisted virtualization depends on the host exposing VT-x or AMD-V to Kurono. The Type-1 hypervisor + Linux-boot-protocol path is implemented, but **booting an Alpine/Debian guest requires *nested* VMX to be exposed to Kurono**. In the common dev environment  -  Kurono itself running as a guest under nested KVM/QEMU  -  that nested layer is not available, and the guest VM-entry fails with a VMX entry error; guest boot is therefore confirmed only where nested VMX is present. (This is the same constraint behind KSA's nested-VM-vs-EPT-isolated-context fallback  -  see the Security section.)
- A native browser isn't running on the OS **yet**  -  but not for the usual "freestanding can't do it" reason. A real **Firefox 140.11.0esr** is cross-compiled against **musl** (`--disable-jit`, `cairo-gtk3-wayland`, 174 MB libxul), and the Linux runtime already provides what it targets: pthreads/`futex`, `epoll`/`poll`, `mprotect` W^X, AF_UNIX + `SCM_RIGHTS`, and a Wayland compositor that composites real `wl_shm` clients. The open work is (1) bringing libxul's shared-library closure onto the OS and loading it through `ld-kurono`, and (2) lifting the current <4 GB user-pointer ABI limit. The GUI "Browser" tile stays a placeholder until then; `curl <url>` is the working HTTP path today.

---

## License

Kurono OS is licensed under the GNU General Public License v2. See [LICENSE](LICENSE) for the full text.