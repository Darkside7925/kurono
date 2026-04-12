# Kurono OS

A bare-metal x86_64 operating system with a hybrid kernel that unifies Linux, Windows, and native Kurono command environments into a single graphical desktop OS.

Built from scratch in C++ and x86 assembly - no libc, no POSIX runtime, no existing kernel. Runs directly on hardware and QEMU with hardware-accelerated virtualization, standalone EFI boot, emergency recovery boot, and installable disk layouts.

> **19 hardware drivers**  -  TCP/IP stack  -  Type 1 hypervisor (boots Alpine Linux)  -  76+ shell commands  -  emergency EFI boot  -  installer with GPT/MBR + FAT32/ext4 support

---

## Quick Start

```powershell
# Build and launch (requires WSL + cross-compiler + QEMU)
.\start.ps1

# Build only, no launch
.\start.ps1 -NoBuild

# Clean build
.\start.ps1 -Clean

# Without hardware acceleration
.\start.ps1 -NoAccel

# Debug mode (GDB on localhost:1234)
.\start.ps1 -Debug
```

---

## Features

### Hybrid Shell with 76 Commands

- **Linux commands (58)** - `ls`, `cd`, `cat`, `grep`, `find`, `ps`, `kill`, `ifconfig`, `ping`, `lspci`, `lsusb`, `lsblk`, `lscpu`, `top`, `df`, `du`, `dmesg`, `modprobe`, `wget`, `curl`, and more
- **Windows commands (18)** - `dir`, `copy`, `del`, `type`, `tasklist`, `ipconfig`, `systeminfo`, `tree`, `findstr`, and more
- **Kurono native** - `help`, `version`, `sysinfo`, `switch`, `clear`, KCL scripting, `kpkg` package manager
- **Cross-environment piping** - `linux:ls /home | windows:findstr user`
- **Command conflict resolution** - detects commands in multiple environments, presents a numbered picker
- **Environment switching** - `switch linux`, `switch windows`, `switch kurono`, `bash`, `cmd`
- **PS1 prompt** - `User@host:cwd$` (Linux), `C:\>` (Windows)

All commands pull real data from kernel drivers and subsystems - no stubs.

### Graphical Desktop Environment

- BGA display with double buffering, up to 3840x2160 (4K), VSync support
- Lock screen with password entry and setup wizard
- Desktop with wallpaper, icons (double-click launch, right-click context menu)
- Full window manager: drag, resize, minimize, maximize, close, z-order focus
- Opaque rendering pipeline - zero alpha blending for maximum framebuffer performance

### Taskbar

- **K start button** - left-aligned logo at x = 6
- **Search bar** - live substring search with blinking cursor, clickable results, Enter to launch
- **Task icons** - centered in screen, colored circles with app initials
- **System tray** - dynamically positioned WiFi signal bars, volume icon with arcs, battery indicator
- **Volume popup** - vertical slider with draggable thumb, mute toggle (SB16)
- **Clock** - far-right, 12-hour AM/PM time on top, M/D/YYYY date below, driven by RTC
- **Start menu** - left-aligned launcher with 11 items and accent color bars

### Built-in Applications

| App | Description |
|-----|-------------|
| Terminal v2.0 | ANSI codes 30-97, cwd-aware prompt, Tab completion, Ctrl+A/E/K/U/W, 1024-entry dedup history, 500ms blinking cursor |
| File Manager | Browse KVFS virtual filesystem with icon/list views |
| Calculator | Basic arithmetic with GUI keypad |
| Text Editor | Create/edit/save files on KVFS |
| ~~Browser~~ | **Removed**  -  No C++ browser on GitHub works bare-metal (NetSurf/Dillo/Ladybird need libc/POSIX). Use `curl` in terminal. |
| Media Player v2.0 | Video viewport rendering (data visualization + FPS counter), 256KB decode buffer, correct file_size, codec badges, SB16+AC97+HDA |
| Settings | 10-tab settings: resolution (apps survive switch), wallpaper, display options with deferred apply |
| Task Manager | Process list, CPU/memory stats, auto-refresh |

### Kernel and Drivers (19 Drivers)

| Driver | Description |
|--------|-------------|
| BGA | Bochs Graphics Adapter, multi-resolution up to 4K, 32bpp |
| Display Manager | 10 predefined modes (640x480 to 3840x2160), VSync, DPI scaling, EDID |
| NVMe | NVMe 1.4 SSD driver with admin + I/O queues, read/write/flush/identify |
| USB / xHCI | USB 3.0/2.0/1.1 host controller, port enumeration, control/bulk transfers |
| Intel HD Audio | HDA codec probing, CORB/RIRB, stream playback (44.1/48/96 kHz, 16/24/32-bit) |
| VirtIO GPU | 2D/3D virtqueue transport, resource management, scanout, cursor |
| NVIDIA GPU | PCI detection + BAR mapping for GeForce RTX 30xx/40xx/50xx, VRAM query, passthrough prep |
| **AMD GPU** | PCI scan (vendor 0x1002), MMIO BAR0 mapping, VRAM detection, arch ID (GCN/RDNA1/2/3), temp/fan/power |
| SB16 Audio | Sound Blaster 16, ISA DMA, PlayTone/Beep/GenerateBuffer, master volume |
| **AC97 Audio** | PCI bus master DMA, NAM/NABM register I/O, 48 kHz PCM, volume control, parallel with SB16 |
| **CPU Detect** | CPUID vendor/brand/family/model, feature flags (SSE4.2/AVX/AVX-512/FMA/AES-NI) |
| Intel E1000 | 82540EM NIC, PCI MMIO, TX/RX descriptor rings, MAC address |
| PS/2 Keyboard | Full scancode handling with drain-all-chars-per-frame input loop |
| PS/2 Mouse | 1000 Hz polling, 1600 DPI scaling, auto-draw gate |
| PIT Timer | 1000 Hz real-time polling with PIT-based frame pacing |
| Serial | COM1 UART kernel logging |
| RTC | Real-time clock for desktop clock and timestamps |
| Graphics | Optimized DrawPixel fast path (opaque bypass), FillRectRounded corner arcs |
| Display | Core framebuffer primitives |

### Networking (Full TCP/IP Stack)

- **Protocols** - Ethernet II, ARP, IPv4, ICMP, UDP, TCP
- **Socket API** - `Socket()`, `Bind()`, `Connect()`, `Listen()`, `Accept()`, `Send()`, `Recv()`, `Close()`
- **Socket types** - `SOCK_STREAM` (TCP), `SOCK_DGRAM` (UDP), `SOCK_RAW`
- **TCP state machine** - all 11 standard states (CLOSED to TIME_WAIT)
- **ARP cache** - 32 entries with resolution
- **ICMP** - `Ping(ip, timeout, &rtt)` with round-trip measurement
- **UDP** - `SendTo()` / `RecvFrom()` convenience API
- **Configuration** - `SetIP()`, `SetSubnetMask()`, `SetGateway()`, `SetDNS()`
- **Statistics** - packets/bytes RX/TX, per-protocol counters, errors, drops
- **Max 16 sockets**, 8 KB RX/TX buffers, 1460 MSS

### Virtualization (Type 1 Hypervisor)

- **VMM** - Intel VT-x and AMD-V with VMCS/VMCB management
- **EPT / NPT** - Extended Page Tables (Intel) + Nested Page Tables (AMD), 2 MB / 1 GB large pages
- **VM lifecycle** - `CreateVM`, `RunVM`, `PauseVM`, `ResumeVM`, `DestroyVM`
- **VM exit handler** - 56 Intel + 9 AMD exit reasons (CPUID, I/O, MSR, EPT violation, HLT, VMCALL)
- **Virtual devices** - PIC 8259A, APIC (MMIO), PIT 8254, HPET, serial COM1, IDE disk
- **Guest memory** - E820 table, MMIO regions, BDA/IVT setup, 4-128 MB RAM
- **Linux boot protocol** - v2.15 bzImage parser, kernel loader at 0x100000, boot_params + cmdline
- **IOMMU** - Intel VT-d / AMD-Vi for PCI device passthrough (GPU passthrough support)
- **Shell command** - `vm create`, `vm run`, `vm pause`, `vm resume`, `vm destroy`, `vm serial`, `vm regs`, `vm info`, `vm boot-alpine`
- **Alpine Linux VM** - pre-compiled Alpine Linux embedded via objcopy; boots on demand with `vm boot-alpine`
- **VMCALL hypercalls** - NOP, info, shutdown, reboot
- **Bare-metal VT-x fixes** - corrected VMCS link pointer, EPT pointer width, host 64-bit exit controls, guest CR0 fixed-bit handling, and aligned VMX allocations for real hardware execution

### Recovery and Installation

- **Emergency boot mode** - dedicated GRUB entries plus a standalone emergency EFI loader using the same EFI boot path
- **Emergency shell** - minimal VGA text recovery shell for command access and log inspection when the desktop path is unavailable
- **Installer command** - `installer` shell workflow for disk scan, partition inspection, ESP detection, planning, and deployment
- **Partition support** - GPT and MBR detection over NVMe-backed disks
- **Filesystem targets** - FAT32 ESP detection/write support and ext4 install layout/write support
- **Install layout** - deploys `/system`, `/etc`, `/boot`, `/apps` plus packaged boot payloads and manifests
- **Embedded payload deployment** - kernel, normal EFI loader, and emergency EFI loader are embedded into the final kernel for installer-driven disk installs

### Linux Subsystem

- **67 syscall numbers defined**, 35+ implemented handlers
- **Real syscalls** - `read`, `write`, `open`, `close`, `lseek`, `brk`, `getpid`, `getuid`, `stat`, `fstat`, `uname`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink`, `access`, `dup`, `dup2`, `ioctl`, `writev`, `mmap`, `munmap`, `nanosleep`, `getdents64`, `clock_gettime`, and more
- **Process model** - up to 16 Linux processes, 64 file descriptors per process, PID/FD table, brk heap
- **Signal handling** - 30 POSIX signals (SIGHUP to SIGPWR)
- **Device nodes** - `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/tty`
- **Kernel identity** - reports as "Linux 6.8.0-kurono" to userspace
- **`linux-exec` command** - run programs through real Linux syscalls
- **`syscall` command** - direct syscall test interface (call any syscall by number)
- **ext4** - superblock, inode, directory parsing support

### Security and Scripting

- **SUPR** - Privilege escalation (sudo equivalent) with timeout, audit logging, permission levels (Guest/User/Admin/Root)
- **User management** - password hashing with salt, multi-user support, groups, home directories
- **KCL** - Kurono Command Language: variables, functions, if/else/while/for, import, math (`sqrt`, `rand`), `print`, `set`
- **Package manager (In Development, stub commands)** - `kpkg install`, `kpkg remove`, `kpkg search`, `kpkg list` with dependency support

### Filesystem

- **KVFS** - In-memory virtual filesystem with directories, files, symlinks, devices, pipes, mountpoints
- **POSIX semantics** - Unix permissions (rwxrwxrwx), 64 KB per-file content
- **Operations** - `mkdir`, `rmdir`, `touch`, `cat`, `cp`, `mv`, `rm`, `chmod`, `stat`, `find`, `grep`
- **Pre-populated** - `/home/user`, `/etc`, `/tmp`, `/var/log`, `/usr/bin`, sample KCL scripts
- **Installer storage path** - ext4 target layout generation/write support plus FAT32 ESP file creation for boot deployment

---

## Architecture

```
Kurono OS
+-- Desktop Environment       (GUI, window manager, 8 apps)
+-- Hybrid Shell               (58 Linux + 18 Windows + Kurono native)
+-- KCL Interpreter            (native scripting language)
+-- TCP/IP Stack               (Ethernet/ARP/IPv4/ICMP/UDP/TCP)
+-- Hypervisor Layer           (VT-x + AMD-V, EPT/NPT, virtual devices)
|       +-- Alpine Linux VM    (on-demand: vm boot-alpine)
|       +-- Generic Linux VM   (bzImage boot protocol)
+-- Linux Subsystem            (35+ real syscalls, 67 defined)
+-- 19 Hardware Drivers        (NVMe, USB, HDA, VirtIO GPU, E1000, BGA, AMD GPU, AC97, CPU Detect, ...)
+-- Media Codecs               (WAV, MP3, AAC-LC, FLAC, MP4/H.264 via CodecRegistry)
+-- Security (SUPR)            (privilege escalation, user management)
+-- KVFS Filesystem            (in-memory, POSIX semantics)
+-- Package Manager (kpkg)     (install, remove, search, update)
```

### Source Layout

```
src/
  boot/         x86_64 Multiboot entry, GDT, IDT, linker script
  kernel/       Main kernel, heap, time, types, system management
  hal/          Hardware abstraction layer (IDT, PIC, ISR stubs)
  drivers/      BGA, NVMe, USB/xHCI, HDA, VirtIO GPU, NVIDIA, E1000, SB16, ...
  ui/           Desktop, taskbar, window manager, lock screen, fonts, GUI
  apps/         Terminal, calculator, file manager, editor, media player, settings, ...
  shell/        Hybrid shell, 58 Linux commands, 18 Windows commands
  fs/           VFS + KVFS in-memory filesystem
  net/          TCP/IP stack (ARP, IPv4, ICMP, UDP, TCP, sockets)
  linux/        Linux subsystem (35+ syscalls, signals, device nodes, ext4)
  virt/         VMM, EPT/NPT, virtual devices, hypervisor, IOMMU, Linux boot
  security/     SUPR privilege system, user management
  kcl/          Kurono Command Language interpreter
  packages/     kpkg package manager
  proc/         Round-robin scheduler
  media/        Image decoder (PNG, JPEG, WebP via stb_image)
  system/       Input manager, user management, installer subsystem
  tests/        Kernel test suite
  third_party/  stb_image, stb_truetype
```

---

## Building from Source

### Prerequisites

- **WSL** with Ubuntu (for cross-compilation)
- **x86_64-elf cross toolchain** (falls back to native `g++ -m64`)
- **NASM** assembler
- **QEMU** for Windows or WSL

### Build Commands (from WSL, inside `src/`)

```bash
make              # Build kernel ELF
make clean        # Clean build artifacts
make run          # Build + launch with WHPX acceleration
make run-noaccel  # Build + launch without acceleration
make run-kvm      # Build + launch with KVM (Linux host)
make debug        # Build + launch with GDB stub (port 1234)
make iso          # Build bootable ISO (requires grub-mkrescue)
```

### Output

- `build/kurono.elf` - final Multiboot ELF kernel with embedded installer payloads
- `build/kurono_base.elf` - base kernel used to generate EFI installer payloads
- `build/kurono.iso` - bootable ISO image
- `build/kurono_efi.efi` - standalone EFI loader
- `build/kurono_emergency.efi` - emergency EFI loader

### Project Layout

```
OS/
  src/          Source code (~65 C++ files + assembly)
  build/        Build output (kurono.elf)
  tools/        Utility scripts (cleanup.ps1)
  archive/      Old files from previous iterations
  start.ps1     One-command build + launch script
  README.md     This file
  STATUS.md     Development status
  LICENSE       MIT License
```

---

## QEMU Configuration

- **RAM:** 10 GB
- **CPU:** 4 SMP cores
- **Acceleration:** WHPX (Windows), KVM (Linux)
- **Audio:** SB16 sound card
- **Network:** Intel E1000 NIC with port forwarding (8080 to 80)
- **VirtIO:** Balloon device
- **GPU passthrough:** Available via `make run-passthrough` (VFIO)

---

## Known Limitations

- Runtime user storage is still KVFS-first and in-memory; the installer can deploy to ext4/FAT32 targets, but the live desktop session is not yet a full persistent root filesystem
- No real ELF binary loading from disk (Linux subsystem uses built-in programs)
- USB keyboard discovery is wired to xHCI, but full USB HID interrupt transfer polling is still incomplete
- NVIDIA GPU detects hardware but does not drive the display (BGA is primary)
- ext4 write support is partial (sparse block allocation and directory expansion are not fully implemented)
- FAT32 support is currently focused on ESP-safe installer writes rather than a full general-purpose long-filename desktop filesystem
- Emergency mode is intentionally minimal and recovery-focused, not a full graphical session

---

## License

GPL V2 License. See [LICENSE](LICENSE) for details.
