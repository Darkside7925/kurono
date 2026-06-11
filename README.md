# Kurono OS

A bare-metal x86_64 operating system with a hybrid kernel that unifies Linux, Windows, and native Kurono command environments into a single graphical desktop OS.

Built from scratch in C++ and x86 assembly — no libc, no POSIX runtime, no existing kernel. Runs directly on hardware and QEMU with hardware-accelerated virtualization.

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

### Hybrid Shell with Multi-Environment Commands

- **Kurono native** — `help`, `version`, `sysinfo`, `switch`, `clear`, KCL scripting
- **Linux bridge** — `ls`, `cd`, `cat`, `grep`, `find`, `ps`, `kill`, `ifconfig`, `ping`, 40+ POSIX commands
- **Windows bridge** — `dir`, `copy`, `del`, `type`, `tasklist`, `ipconfig`, `systeminfo`, `tree`, 18+ NT commands
- **Cross-environment piping** — `linux:ls /home | windows:findstr user`
- **Command conflict resolution** — when a command exists in multiple environments, prompts to choose:

```
> dir
[System Alert] Command 'dir' exists in multiple environments:
1) /bin/dir          (Linux)
2) dir.ps1           (PowerShell)
3) dir.kc            (Kurono)
Enter selection (1-3):
```

- **Environment switching** — `switch linux`, `switch windows`, `switch kurono`, `bash`, `cmd`
- **PS1 prompt** — `User@host:cwd$` (Linux style), `C:\>` (Windows style)

### Graphical Desktop Environment

- 1024x768x32 BGA display with double buffering at 144 FPS target
- Windows 11-style taskbar: centered start button, search box, app icons, system tray
- Volume popup with draggable slider (SB16 audio driver)
- Lock screen with password entry
- Desktop icons with double-click launch and right-click context menu
- Full window manager: drag, resize, minimize, maximize, close, focus/z-order management

### Built-in Applications

| App | Description |
|-----|-------------|
| Terminal | Shell emulator with 512-line scrollback, ANSI colors, command history |
| File Manager | Browse KVFS virtual filesystem with icon/list views |
| Calculator | Basic arithmetic with GUI keypad |
| Text Editor | Create/edit files on KVFS |
| Browser | Simple HTML rendering engine |
| Media Player | Audio playback via SB16 |
| Settings | Resolution, wallpaper, display options with deferred apply |
| Task Manager | Process list, CPU/memory stats, auto-refresh |

### Kernel and Drivers

- **x86_64 bare-metal** — Multiboot-compliant ELF, boots via QEMU `-kernel` or real hardware
- **Memory** — 64 MB kernel heap, physical memory manager
- **Timer** — PIT at 1000 Hz with real-time millisecond polling
- **Input** — PS/2 keyboard + mouse with 1000 Hz polling, USB stubs
- **Display** — Bochs BGA driver, VBE framebuffer fallback, double-buffered rendering
- **Audio** — SB16 sound card driver with master volume and mute control
- **Network** — Intel E1000 NIC driver (PCI enumeration, MMIO, TX/RX rings), WiFi/BT stubs
- **GPU** — NVIDIA/AMD PCI detection and BAR mapping (stub)
- **Storage** — KVFS in-memory virtual filesystem with POSIX-like operations
- **RTC** — Real-time clock reading

### Security and Scripting

- **SUPR** — Privilege escalation (sudo equivalent) with 15-minute timeout and audit logging
- **User management** — SHA-256 password hashing, multi-user support
- **KCL** — Kurono Command Language: variables, loops (`for..end`), math (`sqrt`, `rand`), `print`, `set`
- **Package manager** — `install`, `remove`, `search`, `list` commands

### Filesystem

- **KVFS** — In-memory virtual filesystem with directories, files, and symlinks
- **Operations** — `mkdir`, `rmdir`, `touch`, `cat`, `cp`, `mv`, `rm`, `chmod`, `stat`, `find`, `grep`
- **Pre-populated** — `/home/user`, `/etc`, `/tmp`, `/var/log`, `/usr/bin`, sample KCL scripts

### Virtualization (In Progress)

- **VMM** — Intel VT-x / AMD-V detection, VMXON/VMXOFF, vendor abstraction layer
- **EPT/NPT** — Extended/Nested Page Tables with 2MB large page support
- **Virtual devices** — vSerial, vDisk, vBalloon
- **VM exit handler** — CPUID/IO port interception
- **IOMMU** — VT-d / AMD-Vi detection for device passthrough
- **Hypervisor framework** — Guest memory setup, Linux boot protocol stubs

### Linux Subsystem (In Progress)

- Dual-boot manager (integrated mode — Linux runs alongside Kurono)
- Linux driver framework with 28 driver modules (AHCI, NVMe, USB, GPU, etc.)
- Linux init stubs (`/sbin/init`, runlevel management)
- Syscall stubs: `fork`, `exec`, `open`, `read`, `write`, `close`, etc.
- Signal handling: `SIGKILL`, `SIGTERM`, `SIGINT`, `SIGSTOP`, etc.
- Device nodes: `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/tty`
- ext4 filesystem stubs (superblock, inode, directory parsing)
- Network namespace bridging stubs

---

## Roadmap (Pending)

### High Priority

- [ ] **Real Linux guest execution** — Make syscall stubs execute actual Linux code via the VMM layer instead of logging only
- [ ] **Full SVM/VT-x hypervisor** — Complete AMD-V (primary) and Intel VT-x backend so Linux boots as a real guest kernel
- [ ] **Linux app execution** — Route real Linux binaries through the guest kernel; surface output back to KuronoShell
- [ ] **Real TCP/IP network stack** — Replace E1000 stub with a working IP/TCP/UDP implementation
- [ ] **Persistent storage** — Replace in-memory KVFS with real disk-backed filesystem (ext4 or custom)

### Medium Priority

- [ ] **Native PE / .exe execution** — Run Windows executables directly without a translation layer
- [ ] **Full PowerShell integration** — Real PS cmdlet execution, not just command emulation
- [ ] **GPU driver** — Real display output via AMDGPU or Nouveau (likely via Linux guest driver passthrough)
- [ ] **USB support** — Replace PS/2-only input with real USB HID stack
- [ ] **Windows registry simulation** — Persistent registry for Windows subsystem compatibility

### Long Term

- [ ] **Deep Linux/Kurono state sync** — Unified user, process, and filesystem state across both environments in real time
- [ ] **Cross-env conflict resolution v2** — Namespace prefixing (`linux::cmd`, `win::cmd`) to bypass the selector prompt
- [ ] **Multi-user login** — Full multi-seat support beyond single user
- [ ] **Package repository** — Remote package hosting and dependency resolution
- [ ] **Real hardware support** — Expand beyond QEMU: NVMe, modern GPU, USB, WiFi on bare metal

---

## Architecture

```
Kurono OS
├── KCL Interpreter          (native scripting)
├── Hybrid Shell             (Linux + Windows + Kurono commands unified)
├── Desktop Environment      (GUI, window manager, apps)
├── Hypervisor Layer         (SVM + VT-x backends)
│       └── Linux Guest      (hardware drivers, real app execution) [Pending]
├── Security (SUPR)
└── KVFS Filesystem
```

### Source Layout

```
src/
  boot/         x86_64 Multiboot entry, GDT, IDT, linker script
  kernel/       Main kernel, heap, time, types, system management
  hal/          Hardware abstraction layer
  drivers/      BGA, keyboard, mouse, timer, serial, audio, E1000, NVIDIA
  ui/           Desktop, taskbar, window manager, lock screen, fonts, GUI
  apps/         Terminal, calculator, file manager, editor, browser, media player
  shell/        Hybrid shell, Linux commands, Windows commands
  fs/           VFS + KVFS in-memory filesystem
  net/          Network stack, WiFi/BT stubs
  linux/        Linux subsystem (syscalls, drivers, dual-boot, ext4)
  virt/         VMM, EPT, virtual devices, hypervisor, IOMMU
  security/     SUPR privilege system
  kcl/          Kurono Command Language interpreter
  packages/     Package manager
  proc/         Round-robin scheduler
  media/        Image/audio decoder (stb_image, stb_truetype)
  system/       Input manager, user management
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

`build/kurono.elf` — approximately 2.2 MB Multiboot ELF kernel

### Project Layout

```
OS/
  src/          Source code
  build/        Build output (kurono.elf)
  tools/        Utility scripts (cleanup.ps1)
  archive/      Old files from previous iterations
  start.ps1     One-command build + launch script
  README.md     This file
  LICENSE       MIT License
```

---

## Known Limitations

- No persistent storage (KVFS is in-memory, lost on reboot)
- Network commands are simulated (E1000 driver initializes but TCP/IP stack is stub)
- Linux subsystem is stub-level (syscalls log but do not execute real Linux code)
- No USB support (PS/2 only for real input)
- GPU driver detects hardware but does not drive the display
- ext4 filesystem is stub-level parsing only
- Single user only ("user")

---

## License

MIT License. See [LICENSE](LICENSE) for details.