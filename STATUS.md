# Kurono OS -- Development Status

**Last Updated:** June 2026

## What's New (June 2026)

Big one this month: I moved the whole dev setup off Windows/WHPX onto **Linux + KVM**. Faster, and it fixed a stack of networking/input weirdness WHPX was causing. There's a `./start.sh` now that builds + boots in one shot.

- **KSA  -  hypervisor-backed privilege prompts (our UAC).** Privilege escalation can now require a prompt that's rendered and arbitrated inside a hypervisor-isolated memory region the main OS has **no page-table mapping into**  -  so ring-0 malware in the main OS can't read it or auto-approve it. The verdict (approve/deny + a salted credential hash) crosses back through a single **read-only** VMCALL channel (`0x4B`); there's no path to inject a forged "yes". Isolation is done by carving an exactly-2 MB physical region, mapping it into a dedicated EPT, then *unmapping it from the kernel's identity map* (demoting the covering 2 MB huge page and zeroing the leaf PTEs)  -  after which `QueryMapping()` returns 0 for the region. Auth policy via `supr policy --auth=passwd|kvault|both`; disabling a factor needs `--acknowledge-risk`, and disabling *both* needs `--sovereign-override` (a new **Sovereign** role). All policy changes/prompts/denials/overrides are audited to `/kurono/var/log/security.log` even when KSA is off. **Honest caveat:** under nested KVM (Kurono itself a guest), true nested VMX for an *inner* VM isn't available, so the prompt runs as an **EPT-isolated guest context** rather than a separately `VMLAUNCH`'d VM  -  `IsRealNestedVM()` and the boot log report which path is live, and on a host with nested VMX it upgrades to a real inner VM automatically. **Verified at runtime** (`kurono.ksa.test=1`): the self-test proved `main-os reach base=no mid=no verdict=no` (region truly unmapped from the main OS), the read-only channel carried the verdict as a copy, and a 10-assertion policy/loophole state-machine test all passed  -  booting `-smp 4` under KVM with the desktop coming up faultless. See `docs/developers/security/KSA.md`.
- **The desktop survives a reboot now, on a real filesystem.** KVFS state persists to a dedicated NVMe disk between boots. The NVMe driver was completely dead  -  a missing `volatile` on the completion read meant *no* command ever finished; fixed that. Persistence now goes through **KFS (Kurono File System)**  -  a small from-scratch inode-based on-disk filesystem (superblock / block bitmap / inode table / data; 13 direct + 1 indirect block per inode). The user-data subtrees (`/home`, `/etc`, `/root`) are stored as **real files + directories** (browsable, fuse-mountable later  -  see `docs/developers/fs/KFS.md` §7 for the FUSE-compatible on-disk spec), not an opaque blob. Verified two-boot headless (`kurono.kfstest`): boot 1 writes a marker, a deep nested path, and a 256 KB byte-pattern file; boot 2 restores the volume and every byte matches  -  `[KFSTEST] PASS`.
- **NVMe multi-page DMA.** The driver was capped at 4 KB per command (one `prp1` page), so a 1.8 MB persist became ~450 commands  -  that's why persistence crawled. Now it builds a `prp2`/PRP-list (up to ~2 MB/command) and chunks internally at the controller's MDTS, so the whole tree goes out in a handful of commands. Measured: the ~0.95 MB user-data save goes out in **46 commands / ~5 ms** (the old 4 KB/command path would have needed **237**), and restore takes **~4 ms**.
- **Lightweight system logging + clean paths.** Logs were triple-homed across `/system/logs`, `/kurono/logs`, and `/var/log`. Now there's one canonical `/kurono/var/log/` (boot / system / serial / **network** / **security** / **crash**), wired to the TCP stack, the SUPR security audit, and the panic recovery. The whole on-disk layout is centralized in one `kpaths.h`.
- **One canonical filesystem tree under `/kurono`.** The old mess where `/system/` and `/kurono/system/` coexisted (plus loose `/etc /home /usr /bin /lib /tmp /proc /dev /var /apps` at the root) is gone. Everything Kurono-native now lives under **`/kurono/{system,linux,windows,apps,user,packages,runtime,var}`**, and the old top-level names are **compat symlinks** into it (`/system -> /kurono/system`, `/home -> /kurono/user/home`, `/etc -> /kurono/system/config`, `/bin -> /kurono/system/bin`, `/lib -> /kurono/linux/compat`, `/usr/{bin,lib} -> /kurono/linux/compat/{bin,lib}`, `/tmp -> /kurono/runtime/tmp`, `/proc -> /kurono/runtime/proc`, `/dev -> /kurono/runtime/dev`, `/var -> /kurono/var`, `/apps -> /kurono/apps`, `/windows -> /kurono/windows`). Rather than rewrite the ~800 hardcoded path references, KVFS got a real `Symlink()` node-creator and follows symlinks in intermediate path components; the whole overlay is installed by `KVFS::InstallCanonicalLayout()` at the earliest fs init (inside `KVFS::Init`, before anything else touches a path) and re-asserted after a persistent-tree restore. **Verified at runtime** booting `-smp 4` under KVM: the desktop comes up faultless (`Entering main loop`, GUIProcess/WindowManager/watchdog online), and `kurono.logcheck`, `kurono.dyntest` (`rc=0`  -  the dynamic linker resolves musl libc through `/system/lib -> /kurono/system/lib`), `kurono.kcltest` (11/11) and `kurono.ksa.test` (overall + policy PASS) all still pass with the tree moved. The canonical roots live in `src/system/kpaths.h`.
- **Audio + video are smooth now.** Per-stream mixer ring 341 ms → 683 ms + a clean-silence HDA underrun pad (no more stale-sample clicks); video gained a decode-ahead so frames don't stall the compositor at each boundary.
- **Multi-core is real now.** All cores boot and run kernel code in parallel (verified `-smp 4`): true application-processor bring-up via INIT-SIPI-SIPI, a per-CPU SYSCALL fast-path (swapgs), per-CPU GDT/TSS, and a per-CPU current task. The old ">1 core deadlocks the desktop" bug is gone.
- **SMP per-CPU rewrite foundation (toward user threads on the APs).** The hard, race-prone core of running ring-3 user threads on the secondary cores is now in and verified non-regressing under `-smp 4` (desktop renders, no faults): a **cross-core ready-queue lock** with an atomic per-CPU claim (`_nolock` discipline  -  every state transition under the lock, so two cores can't double-claim a thread; plus a CPU-affinity gate); a **per-CPU user-execution context** that replaces the old single-active-process model (each core runs its own active process and longjmps back to its own frame on exit/fault); and **per-CPU syscall-entry state**. On the BSP it all runs as slot 0, identical to before. **And the APs now run it**: with the gate on (`./start.sh --apsched`), an application processor was shown claiming `/usr/bin/mhello`, launching it in ring-3 via the per-CPU `RunProcessWithArgs`, running its SYSCALLs, and exiting 0  -  in parallel with the desktop, which rendered untouched. A `ClaimFreshUserForCpu` race fix (claim only an explicitly-pinned, fully-loaded process) closes a load race where a spinning AP grabbed a process mid-ELF-load. Default boot is unchanged (APs park). **Phase 4 part 1** then landed: each AP arms a per-CPU **LAPIC timer** (~100 Hz, TSC-calibrated) that drives the proven per-CPU preempt path, so user threads on an AP are time-sliced rather than cooperative (verified booting `-smp 4` with the timer live, an AP running a user program to exit 0, desktop untouched). `clone` sibling-thread placement across cores + load balancing remain. See `docs/developers/proc/SMP.md`.
- **A real Linux GUI client renders through the in-kernel Wayland compositor.** The userspace runtime got the pieces a real GUI app blocks on  -  preemptive concurrent threads (`clone`/`CLONE_THREAD`) with a real `futex` (the multithreaded pthread gate passes, counter=2000), `epoll`/`poll`, `mprotect` (W^X + region splitting), `memfd`/file-backed `mmap`, and `SCM_RIGHTS` fd-passing end to end. A Wayland client compiled with **musl-gcc** (`src/userprogs/wl_shm_test.c`, embedded in the kernel, launched by the `wltest` command) connects, fd-passes a `wl_shm` buffer, and the compositor blits its pixels into a real WM window with damage tracking and forwards pointer input  -  the whole `wl_shm` + `xdg-shell` render path, not just the wire protocol. Keyboard-to-client forwarding is implemented but not yet called from the input loop.
- **Firefox is cross-compiled for Kurono.** Real **Firefox 140.11.0esr** built against **musl** (clang/lld, `--disable-jit`, `--disable-sandbox`, `cairo-gtk3-wayland`) → `firefox`/`firefox-bin` + a 174 MB `libxul.so` Gecko engine, build rc=0, all 12 Alpine musl portability patches applied. The launcher (real `execve` of `/apps/firefox/firefox`) and the syscall/IPC/Wayland surface it targets are in place. Remaining to run it on-device: gather libxul's `.so` dependency closure onto the OS, load via `ld-kurono`, and lift the <4 GB user-pointer ABI limit. This is the concrete rebuttal to "a freestanding OS can't have a browser."
- Boot no longer hangs black  -  the large-model BSS wasn't getting zeroed (garbage pointers). Fixed in the linker script.
- curl/HTTP works end to end (recv-loop + FIN_WAIT half-close + ephemeral ports). Fetched real pages off example.com / wikipedia over tap+NAT.
- USB keyboard/mouse/tablet work over xHCI (DMA alignment was the fix); tablet does accurate absolute positioning.
- Fixed the desktop-breaking bugs: top-taskbar swallowing all input, Sign Out/Lock freezing the machine, window-drag ghost trails, dead calculator clicks, broken Files right-click.
- Started the UI pass: a small theme system (**KSS**), black/grey + modern **Settings** and **Control Center**, the start button now uses the real boot logo, and a bunch of off-center text got fixed.
- **KSS grew into a real styling + animation layer, and the desktop got a scripting language (KJ).** KSS now has a `Sheet` layer on top of the theme tokens: named style rules (selectors like `button:hover`) with a property bag (colors + radius/pad/border/font/opacity/scale/translate), per-property **transitions** that ease automatically when a value changes, and named **keyframe** tracks the compositor samples per frame (fixed-size tables, no heap). Window **open/close now fade *and* scale** (a "pop/zoom" via scaled chrome), on top of the existing notification slide and control-center panel slide; KSS button widgets ease their hover/focus color. **KJ (Kurono JavaScript)** is a new freestanding JS-subset interpreter (`src/apps/kj.cpp`, modeled on KCL): lexer + recursive-descent parser → AST + tree-walking evaluator over a tagged value type (undefined/null/bool/number/string/array/object/function-closure). It does var/let/const, functions + **closures**, objects, arrays (`.push`/`.length`/`for..of`), arithmetic/strings, `if`/`while`/`for`, ternary, `typeof`, `Math.*`, `console.log`, and `// /* */` comments  -  no libc, no STL. **Host bindings let scripts drive the live UI**: `kss.set/get/transition/keyframes/play` mutate the KSS sheet, `ui.notify` posts a toast. Usable from the shell as `kj` / `node`. **Verified headless:** `kurono.kjtest` 11/11 ALL PASS (every language feature + two KSS-binding tests that mutate the sheet from a script and read the result back through C++); `kurono.kcltest` still 11/11; desktop boots faultless under `-smp 4` KVM; window open fade+scale captured via QMP screendump mid-animation.

## Current State

The OS builds successfully as an x86_64 Multiboot ELF kernel and bootable ISO, and runs in QEMU with a full graphical desktop, 19 hardware drivers, a complete TCP/IP stack, 76+ shell commands, emergency recovery boot paths, an installer stack, and a Type 1 hypervisor with an Alpine/Debian guest-boot path. **Honest caveat:** booting a Linux guest needs the host to expose *nested* VT-x to Kurono; under plain nested KVM (Kurono itself a guest) the guest VM-entry fails with a VMX entry error, so guest boot is confirmed only where nested VMX is available.

### Build Info
- **Architecture:** x86_64 bare-metal (no libc)
- **Language:** C++17 + NASM assembly
- **Toolchain:** x86_64-elf cross-compiler (fallback: native g++ -m64)
- **Source files:** ~65 C++ + 1 ASM
- **Primary outputs:** `build/kurono.elf`, `build/kurono.iso`, `build/kurono_efi.efi`, `build/kurono_emergency.efi`
- **Build errors:** 0
- **QEMU target:** `qemu-system-x86_64` with KVM on Linux (WHPX on Windows), 4 GB RAM, **`-smp 4`** (all cores boot + run kernel code in parallel; `start.sh` defaults to 4), virtio-gpu + tap/NAT networking + xHCI USB

---

## What Has Been Built

### Core Kernel
- [x] Multiboot-compliant x86_64 boot (GDT, IDT, long mode)
- [x] x86_64 IDT with 256 entries (16-byte format), PIC 8259A remapping
- [x] ISR stubs for timer (IRQ0), keyboard (IRQ1), mouse (IRQ12), spurious, default
- [x] 2 GB kernel heap (static bump allocator)
- [x] Physical memory manager
- [x] Round-robin process scheduler
- [x] PIT timer at 1000 Hz with real-time millisecond polling
- [x] System panic handler
- [x] Kernel test suite (runs on boot)
- [x] Main loop: drains all keyboard chars per frame, polling-based ~240 FPS target

### Drivers (19)
- [x] **BGA Display** -- Bochs Graphics Adapter, multi-resolution (640x480 to 3840x2160), 32bpp, double-buffered
- [x] **Display Manager** -- 10 predefined modes, VSync (off/on/adaptive), DPI scaling (1.0-2.0x), EDID detection, gamma/brightness, multi-backend (BGA, VirtIO GPU, NVIDIA, Intel, AMD)
- [x] **NVMe** -- NVMe 1.4 SSD driver, admin + I/O queue pairs (up to 4 queues, 64-depth), read/write/flush/identify, stats tracking. Read/write verified end-to-end (the completion poll needed a `volatile`); **multi-page DMA** via PRP list (offset-aware) with internal MDTS chunking; backs the KFS persistence layer
- [x] **USB / xHCI** -- USB 3.0/2.0/1.1 host controller, port enumeration (16 ports), control/bulk transfers, device descriptors (16 devices)
- [x] **Intel HD Audio** -- HDA codec probing (4 codecs, 32 nodes), CORB/RIRB command transport, stream format (44.1/48/96 kHz, 16/24/32-bit), BDL-based DMA playback
- [x] **VirtIO GPU** -- 2D/3D support, virtqueue transport, resource management, scanout, cursor, framebuffer present, 8 pixel formats, 16 scanouts
- [x] **NVIDIA GPU** -- PCI detection + BAR mapping for GeForce RTX 30xx/40xx/50xx (Ampere, Ada Lovelace, Blackwell), VRAM query, passthrough prep
- [x] **AMD GPU** -- PCI scan (vendor 0x1002), BAR0 MMIO mapping, VRAM size detection, GPU arch identification (GCN/RDNA1/RDNA2/RDNA3), compute unit + clock reporting, temperature/fan/power monitoring
- [x] **AC97 Audio** -- PCI bus master DMA controller, NAM/NABM register I/O, codec reset, BDL double-buffer setup, 48 kHz PCM playback, volume control (SetMasterVolume/SetPCMVolume), parallel operation alongside SB16
- [x] **CPU Detect** -- x86 CPUID vendor/brand/family/model/stepping, feature flag extraction (SSE4.2, AVX, AVX-512, FMA, AES-NI, SHA-NI), log output on kernel init
- [x] **SB16 Audio** -- Sound Blaster 16, ISA DMA, PlayTone/Beep/GenerateBuffer, master volume, 22050 Hz, 32 KB DMA buffer
- [x] **Intel E1000** -- 82540EM NIC, PCI MMIO BAR mapping, TX/RX descriptor rings (32 each), MAC address read, stats
- [x] **PS/2 Keyboard** -- Full scancode handling, drain-all-chars-per-frame input loop
- [x] **PS/2 Mouse** -- 1000 Hz polling, 1600 DPI scaling, auto-draw gate (prevents double cursor)
- [x] **PIT Timer** -- Real-time polling, WaitMs(), interrupt-driven frame pacing
- [x] **Serial (COM1)** -- Kernel logging over UART
- [x] **RTC** -- Real-time clock for desktop clock and timestamps
- [x] **Graphics** -- DrawPixel opaque fast path (bypasses alpha blend), FillRectRounded with corner-arc-only pixel loop, FillRect volatile writes
- [x] **Display** -- Core framebuffer primitives, back buffer swap

### Display and UI
- [x] Double-buffered rendering (back buffer swap)
- [x] 10 resolution modes: 640x480 to 3840x2160 (4K)
- [x] VSync: off, on, adaptive
- [x] DPI scaling: 1.0x to 2.0x
- [x] Refresh rate matching: 60/120/144/240 Hz with proper frame target
- [x] Resolution-aware text scaling (16px at 1080p or below, 20px at 1440p+)
- [x] FPS counter overlay (top-right rounded pill)
- [x] Boot splash: logo + animated loading bar + pulsing dots
- [x] Built-in VGA 8x16 bitmap font + stb_truetype TTF support
- [x] Rounded rectangle rendering
- [x] Opaque rendering pipeline -- all colors 0xFF prefix, zero alpha blending overhead

### Desktop Environment
- [x] **Lock screen** with password entry and setup wizard (opaque panels)
- [x] **Desktop** with gradient wallpaper (5 color orbs, vignette, dither noise)
- [x] **Desktop icons** -- Terminal, File Manager, Calculator, Editor, Settings, Media Player, Home (single selection highlight, opaque label pills, no alpha glow)
- [x] **Context menu** -- Right-click: Open, New Folder, New File, Refresh, Settings (opaque shadow and background)
- [x] **Window manager** -- Drag, resize, minimize, maximize, close, z-order focus (opaque shadow, opaque button icon colors)

### Taskbar (Redesigned)
- [x] **K start button** -- Left-aligned at x=6, white K logo with gradient arm colors, 44x34 button
- [x] **Search bar** -- At x=58, width 220, magnifier icon, blinking cursor when active, live substring filtering, clickable results, Enter to launch, Escape to close, Backspace works
- [x] **Task icons** -- Centered in screen, colored circles with app first letter
- [x] **System tray** -- Dynamic x position (screen_width - clock_width - 80), WiFi signal bars, speaker icon with sound arcs, battery indicator
- [x] **Volume popup** -- Vertical slider with draggable thumb, dynamic position matching tray
- [x] **Clock** -- Far-right, 12-hour AM/PM time on top, M/D/YYYY date below, from RTC
- [x] **Start menu** -- Left-aligned above K button (mx0=6), 11 items with colored accent bars
- [x] **Search results** -- Positioned above search bar at x=58, proper case-insensitive substring matching, "No results" message when empty

### Applications (7)
- [x] **Terminal v2.0** -- 80x25 cells, 2048-line scrollback, ANSI codes 30-97 (fg/bg/bold/bright), cwd-aware `user@kurono:/path$` prompt via KVFS::GetCwd(), Tab completion (KVFS::Listdir-based, cycles on repeated Tab), 1024-entry deduplicated history with in-progress save/restore, Ctrl+A/E/K/U/W shortcuts, word-left/right navigation, 500ms blinking cursor via Timer::GetRealMs()
- [x] **File Manager** -- Browse KVFS, icon and list views
- [x] **Calculator** -- Basic arithmetic with GUI keypad
- [x] **Text Editor** -- Create/save/load files on KVFS
- [~] **Browser**  -  the GUI tile is a deliberate placeholder (use `curl <url>` today). The real browser path is **Firefox on the Linux runtime**: Firefox 140.11.0esr is cross-compiled against musl + Wayland (see *Firefox-Class Userspace Runtime* below), and the OS provides the syscalls/IPC/Wayland it targets. Bringing libxul on-device + lifting the <4 GB pointer ABI is what's left to run it.
- [x] **Media Player v2.0** -- Real video viewport rendering (raw data visualization with FPS counter, scanline effect, cinematic vignette), heap-allocated 256KB decode buffer (was 32KB), correct file_size via KVFS::GetFileSize(), cached video metadata (no per-frame file reads), codec badge + backend indicator + volume slider, audio SB16→AC97→HDA routing
- [x] **Settings** -- 10-tab settings with real resolution change via BGA::SetMode, deferred apply, apps reopen after switch
- [x] **Task Manager** -- Process list, CPU/memory stats, auto-refresh

### Hybrid Shell (76 Commands)
- [x] **KuronoShell** -- Command registry (256 max), environment switching, variable expansion, aliases, history (128 entries)
- [x] **Linux commands (58)** -- ls, cd, cat, grep, find, sort, uniq, head, tail, wc, chmod, stat, df, du, ln, ps, kill, free, mount, dmesg, ifconfig, ping, wget, curl, lspci, lsusb, lsblk, lscpu, modprobe, modinfo, insmod, rmmod, dmidecode, hwinfo, top, iotop, ip, ss, tr, tee, uname, uptime, whoami, hostname, date, which, linux-exec, syscall, vm, and more
- [x] **Windows commands (18)** -- dir, copy, move, del, type, md, rd, ren, cls, findstr, tasklist, taskkill, systeminfo, ipconfig, ver, tree, attrib, chkdsk
- [x] **All commands pull real driver data** -- lspci reads PCI, lsusb reads USB, ifconfig reads E1000, free reads heap, ps reads scheduler, etc.
- [x] **Environment switching** -- `switch linux`, `switch windows`, `switch kurono`, `bash`, `cmd`
- [x] **Cross-env piping** -- `linux:ls | windows:findstr`
- [x] **PS1 prompt** -- User@host:cwd$ (Linux), C:\> (Windows)
- [x] **Command conflict detection** -- Shows numbered selector when command exists in multiple environments

### Filesystem
- [x] **KVFS** -- In-memory virtual filesystem with directories, files, symlinks, devices, pipes, mountpoints
- [x] **POSIX semantics** -- Unix permissions (rwxrwxrwx), 64 KB per-file content
- [x] **Operations** -- mkdir, rmdir, touch, cat, cp, mv, rm, chmod, stat, find, grep
- [x] **Pre-populated** -- /home/user, /etc, /tmp, /var/log, /usr/bin, sample KCL scripts
- [x] **FAT32 ESP write path** -- minimal mount, directory creation, and file deployment for installer boot files
- [x] **ext4 installer write path** -- install layout creation and payload staging for target partitions

### Networking (Full TCP/IP Stack)
- [x] **E1000 NIC driver** -- PCI enumeration, MMIO mapping, TX/RX descriptor rings (32 each)
- [x] **Ethernet II** -- Frame encapsulation and parsing
- [x] **ARP** -- 32-entry cache, request/reply
- [x] **IPv4** -- Header construction, checksum, routing
- [x] **ICMP** -- Ping with round-trip timing
- [x] **UDP** -- Datagram send/receive, SendTo/RecvFrom
- [x] **TCP** -- Full 11-state machine (CLOSED through TIME_WAIT), 3-way handshake, FIN teardown
- [x] **Socket API** -- Socket(), Bind(), Connect(), Listen(), Accept(), Send(), Recv(), Close()
- [x] **Socket types** -- SOCK_STREAM (TCP), SOCK_DGRAM (UDP), SOCK_RAW
- [x] **Configuration** -- SetIP(), SetSubnetMask(), SetGateway(), SetDNS()
- [x] **Statistics** -- NetStats struct with per-protocol counters, errors, drops
- [x] **Limits** -- 16 sockets, 8 KB RX/TX buffers, 1460 MSS

### Linux Subsystem
- [x] **67 syscall numbers** defined (LSYS_*)
- [x] **35+ real syscall handlers** -- exit, read, write, open, close, lseek, brk, getpid, getuid, getgid, stat, fstat, uname, getcwd, chdir, mkdir, rmdir, unlink, access, dup, dup2, ioctl, writev, mmap, munmap, nanosleep, getdents64, clock_gettime, set_thread_area, exit_group, and more
- [x] **Process management** -- up to 16 Linux processes, 64 FDs per process, brk heap (0x08100000-0x0C000000), cwd tracking
- [x] **Signal handling** -- 30 POSIX signals (SIGHUP through SIGPWR)
- [x] **Device nodes** -- /dev/null, /dev/zero, /dev/random, /dev/tty
- [x] **Linux kernel layer** -- reports as "Linux 6.8.0-kurono", /proc filesystem, threads, TTY/PTY, /sys virtual files
- [x] **linux-exec command** -- Run programs through real Linux syscalls (hello, uname, ls, cat, mkdir, stat, sleep, echo, write, id, pwd, getpid)
- [x] **syscall command** -- Direct syscall test interface
- [x] **ext4** -- Superblock, inode, directory parsing support
- [x] **Console I/O capture** -- sys_write to stdout/stderr captures to ring buffer
- [x] **Stdin injection** -- Shell can push input data into Linux process stdin

### Virtualization (Type 1 Hypervisor)
- [x] **VMM** -- Intel VT-x and AMD-V detection, VMXON/VMXOFF, SVMEnable/Disable, VMCS/VMCB management
- [x] **EPT** -- Extended Page Tables (Intel) + Nested Page Tables (AMD), 2 MB/1 GB large pages, R/W/X control, memory types (UC/WC/WT/WP/WB)
- [x] **VM exit handler** -- 56 Intel + 9 AMD exit reasons (CPUID, I/O, MSR, EPT violation, HLT, VMCALL hypercalls)
- [x] **Virtual devices** -- PIC 8259A dual, APIC (MMIO at 0xFEE00000), PIT 8254, HPET, serial COM1 (16550A UART, 4 KB ring buffer, DLAB, loopback), IDE disk (sector read/write)
- [x] **Guest memory** -- RAM allocation (low 640 KB + high), E820 table, MMIO regions (VGA, I/O APIC, HPET, Local APIC), 4-128 MB RAM, BDA/IVT setup
- [x] **Linux boot protocol** -- v2.15 bzImage parser, kernel loader to 0x100000, boot_params + cmdline, initrd loading
- [x] **Hypervisor lifecycle** -- CreateVM, LoadLinuxKernel, ConfigureGuestProtectedMode, RunVM, RunOneCycle, PauseVM, ResumeVM, DestroyVM
- [x] **vm shell command** -- Full VM management from terminal (create, run, pause, resume, destroy, serial, regs, info, boot-test, boot-alpine)
- [~] **Alpine Linux VM** -- Pre-compiled Alpine Linux (vmlinuz-virt + initramfs-virt) embedded via objcopy as weak symbols; launched on-demand with `vm boot-alpine`; boots through the Type 1 hypervisor's BootAlpineWithExtraction() using the Linux boot protocol. **Requires nested VT-x exposed to Kurono**  -  under plain nested KVM the guest VM-entry fails (VMX entry error); the path is verified only where nested VMX is available
- [x] **Guest serial bridge** -- Guest COM1 output captured by VirtualSerial and piped into Kurono shell
- [x] **VMCALL hypercalls** -- NOP, info ("KURO"/"NO S"), shutdown, reboot
- [x] **IOMMU** -- Intel VT-d / AMD-Vi, DMA remapping, root/context tables, DRHD parsing, PCI device passthrough (GPU passthrough support)
- [x] **I/O bitmaps** -- 8 KB I/O permission bitmaps + 4 KB MSR bitmaps
- [x] **Bare-metal VT-x correctness fixes** -- VMCS link pointer writes, 64-bit EPT pointer writes, host address-space VM-exit control, guest CR0 fixed-bit compliance, aligned allocation cleanup, and 64-bit IO/MSR bitmap addressing

### Recovery and Installation
- [x] **Emergency EFI boot** -- separate emergency EFI loader and GRUB entries using the same EFI boot flow
- [x] **Kurono Setup boot entry** -- dedicated GRUB menu entry (`kurono.setup=1`) that runs the graphical installer / first-setup wizard; the main/default entry still boots straight to the desktop (`kurono.autologin=1`). `boot_setup` token parsed in `kurono_kernel.cpp`; optional `kurono.setup.screen=N` opens the wizard on a specific screen
- [x] **Setup wizard screens** -- welcome, language, keyboard, network (wired status + Wi-Fi config), disk, partition, filesystem, administrator account, hostname + timezone + preferences, and optional Linux guests (Debian/Alpine/Python). Debian reuses the `kpkg install debian` + system-update reboot flow; provisioning (user, hostname, prefs, `/etc/network`, guest queue) is written via KVFS so the wizard works even with no ext4 target
- [x] **Network/Wi-Fi setup** -- wired link probed live (carrier + DHCP address shown); Wi-Fi screen records SSID/password to `/etc/network` and is honest that there is no radio driver yet (e1000 / virtio-net wired only)
- [x] **Emergency shell** -- minimal recovery environment with VGA text console and command access
- [x] **Disk enumeration** -- NVMe-backed disk scan integrated into installer subsystem
- [x] **GPT/MBR parsing** -- partition table inspection for installer workflows
- [x] **ESP detection** -- FAT32 EFI System Partition detection and mount/write path
- [x] **Install layout generation** -- `/system`, `/etc`, `/boot`, `/apps` deployment plan and manifest generation
- [x] **Embedded payload deployment** -- final kernel embeds installable kernel + EFI payloads for installer-driven deployment
- [x] **Installer shell command** -- `installer` command registered in the shell for scan, plan, and install actions
- [x] **ISO build validation** -- `make iso` completed successfully after the installer payload pipeline changes

### Security and Scripting
- [x] **SUPR** -- Privilege escalation with timeout, audit logging, permission levels (Guest/User/Admin/Root), password hashing with salt
- [x] **User management** -- Multi-user, groups, home directories, session tracking
- [x] **KCL (Kurono Command Language)** -- complete tree-walking scripting language: lexer + recursive-descent parser + interpreter over a tagged value type (int/float/string/bool/list/none). Variables (`set x = expr`), arithmetic + string concat, comparisons, boolean logic (and/or/not), `if`/`elif`/`else`/`end`, `while`, `for x in a..b`/list/string, `func`/`return` with recursion, lists (literals/index/append/remove/len), `import`, `#` comments + shebang. Stdlib: print, input, len, str, int, float, sqrt, rand, abs, min, max, type, read, write, exists, exec, sleep, upper, lower. Errors carry line numbers and never crash the OS. Run via `kcl file.kcl` / `kcl -c "code"` / `.kcl` shebang / File Manager double-click. Verified by an 11-test self-test suite (`kurono.kcltest`, 11/11 PASS headless).
- [x] **Package manager (kpkg)** -- install, remove, update, search, list, dependency support, 64 max packages

---

## Recent Changes (This Session)

### Dynamic Linker (ld-kurono)  -  Phase 10
- **In-kernel ELF64 dynamic linker** in `src/linux/ld_kurono.h/cpp`. ~1,250 LOC, no libc, no external deps.
- **PT_INTERP detection** in `src/kernel/elf_loader.cpp` -- when an ET_DYN binary declares an interpreter (`/lib64/ld-linux-x86-64.so.2`, `/lib/ld-linux.so.2`, etc.) the loader hands the image to `LdKurono::ExecPIE()` instead of trying to map it as a static binary. Static binaries continue through the legacy fast path.
- **Per-segment ASLR** with RDTSC-derived entropy in the user range `0x500000_0000` - `0x700000_0000`, page-aligned, span-clamped.
- **Recursive DT_NEEDED resolution** with circular-dep tracking and SONAME-based deduplication. Search path: `LD_LIBRARY_PATH` (ignored if setuid) → `/system/lib` → `/system/lib/kurono` → `/system/lib/x86_64-linux-gnu` → `/apps/lib` → `/system/local/lib` → `/home/user/.local/lib`. Internal `ld-linux*` and `ld-kurono.so` references are short-circuited (the linker IS the kernel).
- **Symbol lookup**: GNU_HASH primary path (bloom filter + bucket + chain), SYSV hash fallback, linear scan as a last resort. `STB_GLOBAL` / `STB_WEAK` / `STB_LOCAL` and `STV_DEFAULT` / `STV_HIDDEN` / `STV_PROTECTED` / `STV_INTERNAL` honoured. Versioned lookup via `DT_VERSYM` / `DT_VERDEF` / `DT_VERNEED`.
- **Full x86_64 relocation set** -- 23 types: `NONE`, `64`, `PC32`, `PC64`, `PLT32`, `GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`, `32`, `32S`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`, `IRELATIVE`, `COPY`, `TPOFF32/64`, `DTPMOD64`, `DTPOFF32/64`, `TLSDESC`, `TLSGD`, `TLSLD`, `GOTTPOFF`. Eager binding (matches `DT_BIND_NOW` / `LD_BIND_NOW=1` semantics).
- **PT_GNU_RELRO enforcement** -- after relocations the linker re-protects the relro region as `PTE_USER | PTE_NX` (read-only).
- **Static TLS** -- variant-2 layout with monotonic offset assignment, per-module ID. `arch_prctl(ARCH_SET_FS)` already wired in the syscall layer.
- **vDSO** -- 4 KB ELF64 stub mapped at `0x7FFFF7FFC000` exporting `__vdso_clock_gettime` (NR 228), `__vdso_gettimeofday` (NR 96), `__vdso_time` (NR 201), `__vdso_getcpu` (NR 309) as `mov rax, NR; syscall; ret` trampolines. Surfaced via `AT_SYSINFO_EHDR`.
- **Auxv builder** -- pushes `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_FLAGS`, `AT_ENTRY`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_RANDOM` (16 bytes of RDTSC entropy on the stack), `AT_HWCAP` (`0x0001f8bb`), `AT_HWCAP2`, `AT_CLKTCK=100`, `AT_PLATFORM="x86_64"`, `AT_EXECFN`, `AT_SYSINFO_EHDR`. Full SysV stack frame: `argc / argv[]+NULL / envp[]+NULL / auxv[]+AT_NULL / strings`.
- **Constructor trampoline** -- `build_init_trampoline()` emits a real user-mode bootstrap shim (one page, hand-encoded x86_64 machine code) that walks every queued `DT_INIT` and `DT_INIT_ARRAY` entry in dependency order, preserves SysV `(rdi=argc, rsi=argv, rdx=envp)` between calls (re-materialised from `r12`/`r14` via `lea rdx, [r14 + r12*8 + 8]`), then tail-jumps to the program's real entry. The kernel sets RIP to this trampoline so ctors run with the correct user CR3, FS, and SS.
- **Public API** -- `Init`, `MapVDSO`, `ExecPIE`, `Dlopen`, `Dlclose`, `Dlsym`, `Dlvsym`, `Dladdr`, `Dlerror`, `IsLoaded`, `LoadedCount`, `DumpMaps`, `DlDebugStateNotify`. All `RTLD_*` flags (`LAZY` / `NOW` / `GLOBAL` / `LOCAL` / `NOLOAD` / `DEEPBIND` / `NODELETE`) plus `RTLD_DEFAULT` and `RTLD_NEXT`.
- **LD_DEBUG** -- `all` / `libs` / `symbols` / `reloc` / `files` / `versions` / `bindings` channels. Output goes to serial AND `/system/log/ldso.log` (append, bounded). `LD_PRELOAD` honoured (silently dropped for setuid/setgid binaries).
- **r_debug rendezvous** -- `_dl_debug_state` notification hook so a future GDB attach can rescan the loaded library list on every `Dlopen` / `Dlclose`.
- **Wired** -- `LdKurono::Init()` invoked from `kurono_kernel.cpp` after `WaylandServer::Init()`. Marker file dropped at `/system/lib/ld-kurono.so` so `file(1)` and `ldd` style scanners can see it. `ld_kurono.cpp` added to `CXX_SRCS` in `src/Makefile`.

### Firefox-Class Userspace Runtime  -  Phase 9
- **Path translation** rewritten in `src/linux/linux_syscall.cpp::ResolvePath()` -- complete Linux→Kurono prefix table. `/usr/lib*` → `/system/lib`, `/etc` → `/system/etc`, `/proc` → `/system/proc`, `/dev` → `/system/dev`, `/run` → `/system/run`, `/tmp` → `/system/tmp`, `/var/log` → `/system/log`, `/usr/share` → `/system/share`, `/usr/local` → `/system/local`, `/usr/libexec` → `/system/libexec`. `/system`, `/home`, `/apps`, `/linux`, `/boot` pass through unchanged. `LFD_PROC` reads strip the `/system` prefix so `/proc/self/*` resolves uniformly.
- **Runtime layout seeder** -- `src/system/runtime_layout.h/cpp` (NEW). Seeds ~120 directories under `/system` and writes `/system/etc/{hostname, hosts, resolv.conf, nsswitch.conf, machine-id, os-release, shells, ld.so.conf, fonts/fonts.conf, ssl/certs/ca-certificates.crt}`, full `/system/proc` sysctls plus `cmdline / version / uptime / cpuinfo / meminfo / stat / loadavg / mounts / filesystems / swaps / vmstat / partitions`, and `/system/sys/class` skeleton. Drops `firefox-deps.manifest` listing 80+ shared libraries Firefox links against. `/home/user/.config/kurono/firefox.env` carries 18 environment variables (`MOZ_ENABLE_WAYLAND=1`, `LIBGL_ALWAYS_SOFTWARE=1`, `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/system/run/user/1000`, `DBUS_SESSION_BUS_ADDRESS=unix:path=/system/run/user/1000/bus`, ...). `EnsureUserRuntime()`, `RefreshProc()`, `LibrarySearchPath()` helpers exposed.
- **AF_UNIX** -- `src/net/unix_socket.h/cpp` (NEW). 64-slot socket table, `STREAM` / `DGRAM` / `SEQPACKET` types (renamed `UNIX_SOCK_*` to dodge a `tcpip.h` macro collision), bind/listen/accept/connect, `sendmsg` / `recvmsg` with `SCM_RIGHTS` fd passing, abstract namespace (paths starting with `\0`), `SO_PEERCRED`. Ring buffers per peer.
- **Wayland compositor** -- `src/ui/wayland_server.h/cpp` (NEW). Listens on `/system/run/user/1000/wayland-0`. `wl_display`, `wl_compositor`, `wl_shm`, `wl_seat`, `wl_output`, `wl_surface`, `xdg_wm_base`, `xdg_surface`, `xdg_toplevel` minimal-but-real. Damage tracking, keyboard/pointer enter/leave/motion/button events, frame callbacks paced from the desktop redraw.
- **PulseAudio daemon** -- `src/drivers/pulse_server.h/cpp` (NEW). Listens on `/system/run/user/1000/pulse/native`. Authenticates clients with the cookie at `/home/user/.config/pulse/cookie`. Sink `kurono_hda` routes to `Audio::SubmitSamples()`. Source `kurono_input` mirrors a silent capture stream. `pa_stream_write` / `pa_stream_drain` / `pa_context_subscribe`.
- **D-Bus session bus** -- `src/system/dbus_server.h/cpp` (NEW). Listens on `/system/run/user/1000/bus`. `org.freedesktop.DBus` (`Hello`, `RequestName`, `ListNames`, `AddMatch`, `RemoveMatch`), `Properties.Get/Set/GetAll`, `Introspectable.Introspect`. Match-rule dispatch.
- **Firefox launcher** -- `src/apps/firefox_launcher.h/cpp` (NEW). `Launch()` resolves the rootfs Firefox binary, ensures the user runtime directory exists, exports the `firefox.env` block, then dispatches `LSYS_EXECVE` through `LinuxSyscall::Dispatch()` (the underlying `sys_execve` is private). Uses `RuntimeLayout::LibrarySearchPath()` for `LD_LIBRARY_PATH`.
- **Firefox-shaped syscalls added** in `src/linux/linux_syscall.h` -- `LSYS_STATX`, `LSYS_RSEQ`, `LSYS_CLOSE_RANGE`, `LSYS_OPENAT2`, `LSYS_FACCESSAT2`, `LSYS_PROCESS_MRELEASE`, `LSYS_LANDLOCK_*`, `LSYS_MEMFD_SECRET`, plus `LFD_SOCKET` fd type and memfd seal constants. `LinuxStatx` struct laid out per ABI. x86_64 numeric synonyms (288, 425-427, 435-446) accepted alongside their `LSYS_*` names.

#### Runtime brought up to actually host a GUI app

- **Concurrency + memory protection (what a GUI app blocks on)** -- real `futex` (FUTEX_WAIT/WAKE) in `src/linux/linux_syscall.cpp`, `clone`/`CLONE_THREAD` threads sharing one address space with preemptive switching on the user-thread path (the multithreaded pthread gate test passes  -  a two-thread counter reliably reaches 2000), `epoll_create1`/`epoll_ctl`/`epoll_wait` + `poll`/`ppoll` over eventfd/timerfd/sockets, and `mprotect` with W^X enforcement and region splitting. `memfd_create` + file-backed `mmap` provide the shared-memory backing the `wl_shm` path needs.
- **SCM_RIGHTS fd-passing end to end** -- `recvmsg` parses `msghdr` and receives passed fds; a passed `memfd` is resolved to a kernel-visible mapping, so a client can hand the compositor a buffer it actually reads from. This is the mechanism the Wayland render path rides on.
- **Wayland client renders end to end** -- `src/ui/wayland_server.cpp` now blits a client's `wl_shm` buffer (fd-passed via `SCM_RIGHTS`) into a real window-manager window with damage tracking (`commit_surface` → `blit_surface_region` → `blit_argb` to the graphics back buffer), bridges `xdg_toplevel`→window (`bridge_surface_window` → `WindowManager::CreateWindow`), and forwards pointer enter/leave/motion/button (`ForwardPointerMotion/Button/Axis`, called every frame from `desktop.cpp`). Proven with `src/userprogs/wl_shm_test.c`  -  a **musl-gcc**-compiled Wayland client embedded in the kernel image and launched by the `wltest` shell command (commit `df355ca`: "exercises the whole shm render path end to end"). Keyboard-to-client forwarding (`ForwardKey`) is implemented but not yet called from the input loop.
- **Firefox actually cross-compiled** -- real **Firefox 140.11.0esr** built against **musl** on the build server (Alpine-edge `ff-musl-builder`, clang/lld 22, rustc 1.96, wasi-sdk 27; mozconfig `--disable-jit` / `--disable-sandbox` / `cairo-gtk3-wayland`; all 12 Alpine musl portability patches): `firefox` + `firefox-bin` + a 174 MB `libxul.so`, build rc=0. It is a dynamic musl build. Remaining to run on-device: gather libxul's `.so` dependency closure onto Kurono, load via `ld-kurono`, and lift the <4 GB user-pointer ABI limit (the in-kernel runtime already provides the futex/epoll/mprotect/SCM_RIGHTS/AF_UNIX/Wayland/clock_gettime surface it needs).

### Cgroups, TPM, Netfilter, CPUFreq  -  Phase 8
- **Cgroups v2** -- `src/proc/cgroup.h/cpp`. Real hierarchy under `/system/sys/fs/cgroup/`, controllers: `cpu` (weight, max), `memory` (current, high, max, swap), `pids` (current, max), `io` (read/write bandwidth caps). `cgroup.procs` writes move processes between groups; `Process::cgroup_id` plumbed through scheduler.
- **TPM 2.0** -- `src/drivers/tpm.h/cpp`. CRB and FIFO interface auto-detect, locality 0 acquire, `TPM2_GetCapability` (manufacturer, family, vendor string), `TPM2_GetRandom` (entropy injected into the kernel pool), PCR extend/read for `SHA-1` / `SHA-256` / `SHA-384` banks. Used by the secure-boot path to attest `/boot/kurono.elf`.
- **Netfilter** -- `src/net/netfilter.h/cpp`. Five hooks (`PRE_ROUTING`, `LOCAL_IN`, `FORWARD`, `LOCAL_OUT`, `POST_ROUTING`), table+chain+rule model, `ACCEPT` / `DROP` / `REJECT` verdicts, match on src/dst IPv4, port ranges, protocol, interface. Wired into `tcpip.cpp` send/receive paths.
- **CPUFreq governors** -- `src/hal/cpufreq.h/cpp`. `performance` / `powersave` / `ondemand` / `userspace` governors, P-state writes via `MSR_IA32_PERF_CTL` on Intel and `MSR_AMD_PSTATE_*` on AMD. CPUID-detected min/max P-state range. `/sys/devices/system/cpu/cpu0/cpufreq/*` reflects the live state.

### Emergency Boot + Installer Stack
- Added emergency boot flow with dedicated normal/emergency EFI artifacts and matching GRUB entries
- Added installer subsystem in `src/system/installer.h` and `src/system/installer.cpp`
- Implemented NVMe disk enumeration, GPT/MBR partition detection, FAT32 ESP detection, and ext4 target layout generation
- Added minimal FAT32 write support in `src/fs/fat32.h` and `src/fs/fat32.cpp` for ESP deployment
- Added deployment of embedded installer payloads to `/boot/kurono.elf`, `/EFI/KURONO/KURONO.EFI`, `/EFI/KURONO/KEMERG.EFI`, `/EFI/BOOT/BOOTX64.EFI`, and recovery helper files
- Reworked `src/Makefile` into a two-stage build (`kurono_base.elf` then final `kurono.elf`) so installer kernel/EFI payloads can be embedded cleanly
- Final ISO build completed successfully with the new payload-embedding pipeline

### Bare-Metal VMX Fixes
- Corrected VMCS link pointer handling to use proper 64-bit writes
- Corrected EPT pointer writes to avoid truncation on real hardware
- Added required host 64-bit VM-exit control handling
- Fixed guest `CR0` setup to satisfy VMX fixed-bit MSRs
- Fixed aligned allocation/free handling used by VMX/EPT code
- Fixed 64-bit IO/MSR bitmap address handling

### New Hardware Drivers
- **AMD GPU driver** -- `src/drivers/amd_gpu.h/cpp`: PCI scan for vendor 0x1002, full MMIO BAR0 mapping, VRAM size from BAR2, GPU arch detection (GCN/RDNA1/RDNA2/RDNA3), compute unit count, engine clock, temperature/fan/power. `AmdGPU::Init()` called from kernel_main; `AmdGPU::IsAvailable()` + `AmdGPU::GetInfo()` API.
- **AC97 Audio Controller** -- `src/drivers/ac97.h/cpp`: PCI bus master DMA audio, NAM/NABM register-based codec control, double-buffered BDL playback at 48 kHz, `AC97::Init()` + `AC97::Tick()` wired into kernel main loop, `AC97::SetMasterVolume()` / `AC97::SetPCMVolume()`, `AC97::GetState()` parallel to SB16.
- **CPU Feature Detection** -- `src/drivers/cpu_detect.h/cpp`: CPUID leaf 0/1/2/7 query, vendor/brand/family/model/stepping extraction, feature flags (SSE4.2/AVX/AVX-512/FMA/AES-NI/SHA-NI), `CPUDetect::Init()` + `CPUDetect::PrintInfo()` from kernel_main.
- **Kernel init wiring** -- `src/kernel/kurono_kernel.cpp`: added `AmdGPU::Init()`, `AC97::Init()`, `CPUDetect::Init() + PrintInfo()` after IOMMU init; `AC97::Tick()` added alongside `Audio::Tick()` in the main polling loop.

### Audio Codec Registry (Previous Session, now wired in)
- `src/media/codec.h/cpp`: CodecRegistry with WAV/MP3/AAC-LC/FLAC/MP4/H.264 decoders. `Detect()` (magic bytes), `DetectByExtension()`, `DecodeAudio()`, `ExtractMP4Audio()`, `ParseFLACStreamInfo()`, `ParseAACHeader()`, `ParseMP4()` APIs.

### Browser Removed
- **Deleted** 1,260-line KBrowse implementation from `src/apps/browser.cpp`
- Replaced with minimal 75-line stub showing "Browser Removed" notice
- Reason: No C++ browser on GitHub compiles for bare-metal freestanding OS
  - NetSurf requires libc, POSIX, GTK/SDL
  - Dillo requires libc, POSIX, FLTK
  - Ladybird requires libc, POSIX, Qt/Wayland
  - Links2/ELinks require libc, POSIX, ncurses
- Desktop icon removed; start menu stub kept
- Use `curl <url>` in terminal for HTTP requests
- **Superseded (read this):** the "no C++ browser compiles for a freestanding OS" reasoning above is *historical* and no longer the project's stance. The browser answer is now **Firefox on the Linux runtime**  -  Firefox 140.11.0esr is cross-compiled against musl + Wayland (see *Firefox-Class Userspace Runtime* / *What's New*), and the engine builds fine. The GUI tile stays a placeholder only until libxul is loaded on-device via `ld-kurono` and the <4 GB pointer ABI is lifted.

### Terminal v2.0
- Extended ANSI: codes 30-37 (fg), 90-97 (bright fg), 40-47 (bg), 1 (bold), 0 (reset), 2J (clear screen), semicolon-chained multi-params
- New prompt: live CWD from `KVFS::GetCwd()`, rendering `user@kurono:/cwd$` in green/blue/cyan
- Tab completion: `KVFS::Listdir()` scan, unique→auto-complete, ambiguous→list+cycle on repeated Tab
- History: 1024 entries, dedup (skip if same as last), in-progress text saved via `hist_saved[]` before Up-arrow navigation
- Cursor: 500ms blink via `Timer::GetRealMs()`
- Shortcuts: Ctrl+A (line start), Ctrl+E (line end), Ctrl+K (kill to end), Ctrl+U (kill to start), Ctrl+W (delete word), Page Up/Down (scroll)
- Bold renders by +50 per channel lightening in `RenderCell()`

### Media Player v2.0  -  Video Fix
- **Fixed video playback**: Video viewport now renders actual frame data as pixel blocks with cinematic vignette, scanline overlay, and FPS counter
- **Fixed file_size bug**: Was using header read count (2KB max) instead of `KVFS::GetFileSize()`  -  broke all duration estimates for MP3/MP4
- **Fixed FPS**: Removed `KVFS::ReadFile()` from render path (was reading file every frame!), replaced with one-time `CacheVideoInfo()` call
- **256KB decode buffer**: Playback buffer increased from 32KB to 256KB via `KernelHeap::Alloc()` (was static array)
- **Video metadata caching**: `PlaylistEntry` now stores `video_width/height/codec/has_video/has_audio` from MP4 parse, cached per-track
- Duration per format (unchanged): WAV RIFF, MP3 ID3+bitrate, FLAC StreamInfo, AAC ADTS, MP4 mvhd
- Codec info bar + backend indicator + volume slider + playlist badges unchanged

### Resolution Switch Fix
- **Fixed apps not opening after resolution change**: `DoApplyResolution()` now resets `MediaPlayerApp::win_id` and `KBrowse::win_id` to -1 after `WindowManager::CloseAll()`  -  apps with `if(win_id >= 0) return` guard previously refused to reopen
- **Fixed WM desktop area**: Added `WindowManager::SetDesktopArea(0, 0, w, h-44)` after resolution change
- **Auto-reopen Settings**: Settings app reopens automatically after resolution switch so user sees the result

### Alpine Linux VM
- Confirmed loads on-demand via `vm boot-alpine` shell command
- Does NOT auto-boot at kernel start (correct design  -  user-initiated)
- `BootAlpineWithExtraction()` in `src/virt/hypervisor.cpp` handles extraction from embedded objcopy symbols

---

## Performance Optimizations (Previous Session)

1. **Opaque rendering pipeline** -- Converted ALL semi-transparent colors to fully opaque (0xFF prefix) across desktop.cpp, lockscreen.cpp, window_manager.cpp, media_player.cpp. This eliminates per-pixel ReadPixel + BlendColors + SetPixel and allows FillRect fast path (direct memory writes).
2. **DrawPixel fast path** -- When alpha == 0xFF, bypasses alpha blending and calls DrawPixelUnsafe directly.
3. **FillRectRounded optimization** -- Uses FillRect for the 3 rectangular body regions + per-pixel DrawPixelUnsafe only for corner arcs.
4. **FillRect volatile writes** -- Direct volatile framebuffer writes for maximum throughput.
5. **Keyboard input drain** -- Main loop drains ALL pending keyboard chars per frame (while loop) instead of one char per frame.
6. **Mouse auto-draw gate** -- Mouse::Poll() only auto-draws cursor when auto_draw is true; main loop draws cursor once after all rendering.

---

### Taskbar Complete Redesign
- Deleted and rewrote all 7+ taskbar render methods + HandleClick from scratch
- K start button: left-aligned at x=6 (was centered)
- Search bar: at x=58 with live substring matching (was first-char only)
- Task icons: centered in screen (was offset)
- System tray: dynamic position based on clock width (was hardcoded)
- Clock: far-right with 12h AM/PM format
- Start menu: left-aligned above K button
- Volume popup: dynamic position matching tray
- All colors converted to opaque (0xFF prefix)

### Media Player Fixes
- Replaced PlayLoopTone (continuous screeching) with PlayTone(440, 200, 40) short confirmation beep
- Progress bar now duration-aware: fpp = (duration * 60) / 100 frames per percent
- Play circle overlay converted to opaque

### Lock Screen Fixes
- Login and setup panel backgrounds converted from 0xD0 alpha to 0xFF opaque

### Window Manager Fixes
- Shadow color: 0xFF08080C (was 0x30000000 alpha)
- Close button X: 0xFF401010 (was 0x80000000)
- Minimize dash: 0xFF403010 (was 0x80000000)
- Maximize arrows: 0xFF103010 (was 0x80000000)

### Desktop Icon Fixes
- Removed ic_glow variable entirely (was alpha-blended glow)
- Removed glossy shine overlay (was 0x40/0x20 alpha)
- Single FillRoundedRect selection highlight (was 3-layer alpha)
- Label pill: 0xFF080812 (was alpha)
- Context menu: opaque shadow (0xFF060610) and background (0xFF121220)

---

## Known Limitations

- Live desktop storage is KVFS-first and in-memory, but it now **persists across reboot through a real on-disk filesystem**: the user-data subtrees are mirrored to **KFS** (`src/fs/kfs.cpp`)  -  a from-scratch inode-based filesystem on the NVMe data disk  -  as actual files + dirs, and restored at boot (`PersistStore::SaveTree`/`LoadTree`, `src/fs/persist.cpp`). Installer deployment to ext4/FAT32 also exists. KFS is the on-disk persistence layer, not yet a fully mounted root (KVFS stays the runtime fs)
- ELF loading from disk is improving but limited: static built-in programs run from the program table, and **dynamic musl PIEs now load through ld-kurono** (the `dyntest` PIE resolves `libc.musl-x86_64.so.1` and exits `rc=0`; file-backed `mmap` of `.so` segments landed). Running an arbitrary large closure on-device (e.g. Firefox's libxul) still needs its full `.so` set staged and the <4 GB user-pointer ABI lifted
- USB keyboard discovery is wired to xHCI, but full USB HID interrupt transfer polling is still incomplete
- NVIDIA GPU detects hardware but bugs out during intensive tasks taking more then 50mb of vram
- ext4 write support is partial (sparse block allocation and directory expansion are not fully implemented)
- FAT32 support is currently scoped to installer/ESP deployment rather than a full long-filename general-purpose filesystem
- Hypervisor requires Intel VT-x or AMD-V exposed to Kurono. Booting an Alpine/Debian **guest** additionally needs *nested* VMX: under plain nested KVM (Kurono itself a guest) the guest VM-entry fails (VMX entry error), so guest boot is confirmed only where nested VMX is available. (Same constraint behind KSA's nested-VM-vs-EPT-isolated-context fallback)
