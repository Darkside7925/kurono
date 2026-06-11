# Kurono OS -- Development Status

**Last Updated:** March 30, 2026

## Current State

The OS builds successfully as a 2.2 MB x86_64 Multiboot ELF kernel and runs in QEMU with full graphical desktop.

### Build Info
- **Architecture:** x86_64 bare-metal (no libc)
- **Language:** C++17 + NASM assembly
- **Toolchain:** x86_64-elf cross-compiler (fallback: native g++ -m64)
- **Output:** `build/kurono.elf` (~2.2 MB)
- **Build errors:** 0
- **QEMU target:** `qemu-system-x86_64` with WHPX acceleration

## What Has Been Built

### Core Kernel
- [x] Multiboot-compliant x86_64 boot (GDT, IDT, long mode)
- [x] x86_64 IDT with 256 entries (16-byte format), PIC 8259A remapping
- [x] ISR stubs for timer (IRQ0), keyboard (IRQ1), mouse (IRQ12), spurious, default
- [x] 64 MB kernel heap allocator
- [x] Physical memory manager
- [x] Round-robin process scheduler
- [x] PIT timer at 1000 Hz with real-time millisecond polling
- [x] System panic handler
- [x] Kernel test suite (runs on boot)

### Drivers
- [x] **BGA Display** -- Bochs Graphics Adapter, multi-resolution (1024x768, 1920x1080, 2560x1440), 32bpp, double-buffered
- [x] **PS/2 Keyboard** -- Full scancode handling, USB stubs
- [x] **PS/2 Mouse** -- High-precision 1000 Hz polling, 1600 DPI scaling
- [x] **PIT Timer** -- Real-time polling, WaitMs(), frame pacing
- [x] **Serial (COM1)** -- Kernel logging over UART
- [x] **SB16 Audio** -- Sound Blaster 16 driver, master volume, mute
- [x] **Intel E1000 NIC** -- PCI detection, MMIO BAR mapping, TX/RX ring buffers
- [x] **RTC** -- Real-time clock reading
- [x] **NVIDIA GPU** -- PCI detection, BAR mapping, info reporting (stub)

### Display & UI
- [x] Double-buffered rendering (back buffer swap)
- [x] Multi-resolution support: 1024x768, 1920x1080 (Full HD), 2560x1440 (QHD)
- [x] Refresh rate matching: 60/120/144/240 Hz with proper frame target
- [x] Resolution-aware text scaling (16px ≤1080p, 20px 1440p+)
- [x] 180 FPS target with PIT-based frame pacing (interrupt-driven hlt)
- [x] FPS counter overlay (top-right rounded pill)
- [x] Boot splash: logo + animated loading bar + pulsing dots
- [x] Built-in bitmap font + stb_truetype TTF support
- [x] Rounded rectangle rendering
- [x] Alpha blending support

### Desktop Environment
- [x] **Lock screen** with password entry
- [x] **Desktop** with wallpaper (embedded PNG or gradient)
- [x] **Desktop icons** -- Terminal, File Manager, Calculator, Editor, Settings, Browser, Media Player
- [x] **Taskbar** -- Windows 11-style, centered start button, functional search with app filtering, task icons, acrylic effect
- [x] **Start menu** -- App launcher with all built-in apps + shutdown
- [x] **System tray** -- Dynamic clock/date from RTC, WiFi signal bars, volume icon, battery indicator
- [x] **Volume popup** -- Vertical slider with draggable thumb, mute toggle
- [x] **Context menu** -- Right-click on desktop: Open, New Folder, New File, Refresh, Settings
- [x] **Window manager** -- Drag, resize, minimize, maximize, close, z-order focus

### Applications
- [x] **Terminal** -- 80x25 cells, 512-line scrollback, ANSI color codes, command history, Ctrl+L/Ctrl+C
- [x] **File Manager** -- Browse KVFS, icon and list views
- [x] **Calculator** -- Basic arithmetic with GUI keypad
- [x] **Text Editor** -- Create/save/load files on KVFS
- [x] **Browser** -- Simple HTML rendering engine
- [x] **Media Player** -- Audio playback via SB16
- [x] **Settings** -- Resolution, wallpaper, display options with deferred apply
- [x] **Task Manager** -- Process list, CPU/memory stats, auto-refresh

### Hybrid Shell
- [x] **KuronoShell** -- Command registry, environment switching, variable expansion, aliases, history
- [x] **Linux commands (40+)** -- ls, cd, cat, grep, find, sort, uniq, head, tail, wc, chmod, stat, df, du, ln, ps, kill, free, mount, dmesg, ifconfig, ping, wget, curl, etc.
- [x] **Windows commands (18+)** -- dir, copy, move, del, type, md, rd, ren, cls, findstr, tasklist, taskkill, systeminfo, ipconfig, ver, tree, attrib, chkdsk
- [x] **Environment switching** -- `switch linux`, `switch windows`, `switch kurono`, `bash`, `cmd`
- [x] **Cross-env piping** -- `linux:ls | windows:findstr`
- [x] **PS1 prompt** -- User@host:cwd$ (Linux), C:\> (Windows)
- [x] **Command conflict detection** -- Shows selector when a command exists in multiple environments

### Filesystem
- [x] **KVFS** -- In-memory virtual filesystem with directories, files, symlinks
- [x] **Operations** -- mkdir, rmdir, touch, cat, cp, mv, rm, chmod, stat, find, grep
- [x] **Pre-populated** -- /home/user, /etc, /tmp, /var/log, /usr/bin, sample KCL scripts

### Networking
- [x] **E1000 NIC driver** -- PCI enumeration, MMIO mapping, TX/RX descriptor rings
- [x] **Network stack** -- Basic initialization, WiFi/BT stubs
- [x] **Linux net bridge** -- Network namespace bridging stubs

### Linux Subsystem
- [x] **Dual-boot manager** -- Integrated mode (Linux runs alongside Kurono)
- [x] **Linux driver framework** -- 30+ driver modules (GPU/DRM, input, net, sound, FS, bus, power)
- [x] **Linux init** -- /sbin/init, runlevel management, shell command registration
- [x] **Syscall ABI layer** -- 30+ real syscall handlers (read, write, open, close, stat, mkdir, brk, getpid, uname, getcwd, chdir, getdents64, clock_gettime, mmap, dup/dup2, ioctl, writev, nanosleep, etc.)
- [x] **Console I/O capture** -- sys_write to stdout/stderr captures to ring buffer; shell can read it back
- [x] **Stdin injection** -- Shell can push input data into Linux process stdin
- [x] **linux-exec command** -- Run programs through real Linux syscalls (hello, uname, ls, cat, mkdir, stat, sleep, echo, write, id, pwd, getpid)
- [x] **syscall command** -- Direct syscall test interface (call any syscall by number)
- [x] **Process management** -- PIDs, FD table (64 fds), brk heap (0x08100000-0x0C000000), cwd tracking
- [x] **Signal handling** -- SIGKILL, SIGTERM, SIGINT, SIGSTOP, etc.
- [x] **Device nodes** -- /dev/null, /dev/zero, /dev/random, /dev/tty
- [x] **ext4 stubs** -- Superblock, inode, directory parsing stubs

### Virtualization
- [x] **VMM** -- Intel VT-x / AMD-V detection, VMXON/VMXOFF, real inline asm (vmlaunch/vmresume/vmrun)
- [x] **EPT** -- Extended Page Tables (Intel EPT + AMD NPT) with 2MB large page mapping
- [x] **Virtual serial** -- Full 16550A UART emulation with 4KB output capture ring buffer, DLAB, loopback, interrupt priority
- [x] **Virtual disk** -- IDE/ATA emulation with sector read/write
- [x] **VM exit handler** -- 22 Intel + 6 AMD VM-exit reasons (CPUID, I/O, MSR, EPT violation, HLT, VMCALL hypercalls)
- [x] **Guest memory** -- RAM allocation (low 640KB + high), E820 table, BDA/IVT setup
- [x] **Linux boot protocol** -- bzImage parser, kernel loader to 0x100000, boot_params + cmdline setup
- [x] **Hypervisor lifecycle** -- CreateVM, LoadLinuxKernel, ConfigureGuestProtectedMode, RunVM, PauseVM, ResumeVM, DestroyVM
- [x] **vm shell command** -- Full VM management from terminal (create, run, pause, resume, destroy, serial, regs, info, boot-test)
- [x] **Guest serial bridge** -- Guest COM1 output captured by VirtualSerial and piped into Kurono shell via `vm serial` / `vm run`
- [x] **VMCALL hypercalls** -- NOP, info ("KURO"/"NO S"), shutdown, reboot
- [x] **IOMMU** -- VT-d / AMD-Vi detection for device passthrough

### Security & Scripting
- [x] **SUPR** -- Privilege escalation with timeout protection
- [x] **KCL** -- Variables, loops (for..end), math (sqrt, rand), print, set
- [x] **Package manager** -- install, remove, search, list commands

## Recent Changes (This Session)

1. **CRITICAL FIX: IDT + PIC implementation** -- HAL::Init() was empty (no IDT, no PIC remapping, no IRQ handlers). When the frame pacer executed `sti; hlt`, the CPU halted and never woke because PIT IRQ0 had no handler. Implemented complete x86_64 IDT (256 entries, 16 bytes each), PIC 8259A remapping (IRQ0-15 → vectors 32-47), and ISR stubs for timer (IRQ0), keyboard (IRQ1), mouse (IRQ12), spurious, and default interrupts. Timer ISR increments HAL::pit_ticks and sends EOI. 

2. **Frame pacer fix** -- Changed single `hlt` to proper interrupt-driven loop: `while(Timer::GetRealMs() < wait_until) { sti; hlt; }`. CPU now efficiently sleeps between frames and wakes on every PIT tick (~1ms).

3. **Desktop/Taskbar UI overhaul:**
   - K start button properly centered at `screen_width/2 - btn_w/2`
   - Search bar widened to 200px with functional keyboard input, search results dropdown
   - Clock shows dynamic date/time from TimeManager instead of hardcoded "3/29/2026"
   - WiFi indicator defaults to connected with 3-bar signal strength
   - System tray modernized (better battery, volume, WiFi rendering with rounded rects)
   - Taskbar gets acrylic-like texture effect
   - Search box: typing filters apps, Enter launches match, Escape closes, Backspace works

4. **Multi-resolution support** -- BGA SetMode supports 1024x768, 1920x1080 (1080p), and 2560x1440 (1440p). Settings Display tab allows runtime resolution switching with deferred apply (between frames). Refresh rate selection: 60/120/144/240 Hz with proper frame target update.

5. **Resolution-aware text rendering** -- DrawString scales font size based on screen height: 16px for ≤1080p, 20px for 1440p+. VGA 8x16 bitmap font with proportional scaling at higher resolutions.

6. **Linux driver enhancements** -- Added DRM KMS (Kernel Mode Setting) driver and framebuffer console driver (fbcon). Updated GPU driver category now includes bochs_drm, drm_kms, fbcon, and vgacon.

7. **Updated About page** -- Architecture corrected from "x86 (i686)" to "x86_64", Display from "BGA 1024x768" to "BGA (adaptive)".

## Previous Changes (Last Session)

### Session: Shell Polish, UI, Cleanup

1. **Created `start.ps1`** -- One-command build + launch script.

2. **Cleaned up project directory** -- 150+ loose files organized:
   - Stale `.o` files, build directories, binaries, logs **deleted**
   - Old scripts (45+) moved to `archive/old_scripts/`
   - Old sources (30+) moved to `archive/old_sources/`
   - Old Python files moved to `archive/old_python/`
   - Old boot artifacts moved to `archive/old_boot/`
   - Old docs moved to `archive/old_docs/`
   - Created `tools/cleanup.ps1` for repeatable cleanup

3. **Updated README.md** -- Rewrote to reflect actual codebase with accurate build instructions, feature list, and architecture.

4. **Created STATUS.md** -- This file.

5. **Implemented command conflict selector** -- Shell now detects when a typed command exists in multiple environments and presents a numbered menu to pick which one to run (e.g., `ls` exists in both Linux and Kurono). Added `FindAllCommands()`, `ResolveConflict()`, and conflict state machine to shell.

6. **Optimized kernel main loop:**
   - E1000 NIC poll reduced to every 4th frame
   - TaskManager refresh interval increased (300 frames / ~2s)
   - Settings deferred actions polled every 8th frame
   - Frame pacer uses `hlt` instruction for CPU-efficient sleep + `pause` for fine-tuning
   - Removed unused `needs_redraw` tracking code

7. **Improved UI visuals across the board:**
   - **Wallpaper:** 5 color orbs (amber, deep blue, magenta, teal aurora, warm accent) with vignette and dither noise
   - **Taskbar:** Deeper glass effect (0xE8080812), accent top line, inner glow, bottom shadow
   - **Start menu:** Accent underline, colored left indicator bars per item, deeper shadows, richer dark background
   - **Start button:** Glow aura behind button, brighter K logo in white/blue tones
   - **Desktop icons:** Soft color glow beneath each icon, improved gradient blend zone, better glossy shine, label background pills. Added Browser (blue) and Media (pink) icon colors.
   - **Context menu:** Rounded corners, proper shadows, accent border, item dividers
   - **Clock:** Brighter time (0xFFF0F0FF), softer date (0xFF8888AA)
   - **FPS overlay:** Rounded pill shape instead of rectangle, green accent text

8. **Restored `logo.h`** -- Boot splash logo file restored to project root from archive.

## Previous Sessions

### Session: x86_64 Migration + Major Features
- Migrated entire OS from 32-bit (i386) to 64-bit (x86_64)
- Implemented real PIT timing (replaced fake incrementing timer)
- Fixed FPS counter to use real elapsed time
- Added QEMU WHPX acceleration support
- Built Intel E1000 NIC driver
- Built WiFi/BT networking stubs
- Created Linux driver framework (28 drivers)
- Implemented hardware virtualization (VMM, EPT, IOMMU)
- Deep Linux integration (dual-boot, syscalls, device nodes)

## Known Limitations

- No persistent storage (KVFS is in-memory, lost on reboot)
- Network commands are simulated (E1000 driver initializes but TCP/IP stack is stub)
- Linux syscalls execute real operations but no actual ELF binary loading from disk yet (uses built-in programs)
- Hypervisor requires Intel VT-x or AMD-V hardware; falls back to simulated serial bridge without it
- No USB support (PS/2 only for real input)
- NVIDIA GPU driver detects but doesn't drive display
- ext4 filesystem is stub-level parsing only
- No multi-user login (single user "user")
