# Developer Documentation

This branch of the docs tree is for people changing the kernel, the desktop, the shell, the virtual machine stack, the installer, or the hardware drivers.

## The Short Version

Kurono OS is a single address space, polling heavy, bare metal operating system that starts from a Multiboot or EFI path, brings up a framebuffer driven desktop, exposes a mixed shell model, and layers in a Linux personality plus a type 1 hypervisor stack.

The project is broad, but the actual top level shape is easy to hold in the head once the major routes are clear.

1. `src/boot` gets execution into long mode and hands control to the kernel.
2. `src/kernel/kurono_kernel.cpp` performs the long boot sequence.
3. `src/hal` owns interrupts, reboot, low level port access, and exception dispatch.
4. `src/drivers` talks to hardware and firmware provided framebuffers.
5. `src/ui` owns the desktop, taskbar, window manager, and presentation path.
6. `src/apps` supplies the built in programs.
7. `src/shell` owns command parsing, command lookup, environment switching, and cross environment execution.
8. `src/fs`, `src/system`, `src/packages`, `src/security`, `src/kcl`, `src/net`, `src/linux`, and `src/virt` provide the operating system services layered above the core bring up path.

## Documents in This Section

### How it works

- `how-it-works/BOOT_TO_DESKTOP.md`  -  full boot path from firmware to desktop
- `how-it-works/SUBSYSTEMS.md`  -  subsystem overview

### Routing

- `routing/COMMAND_ROUTING.md`  -  how shell commands reach subsystems
- `routing/UI_INPUT_ROUTING.md`  -  how input events reach applications

### Reference

- `reference/FILE_MAP.md`  -  every source file with its role

---

### Kernel (`kernel/`)

- `kernel/KURONO_KERNEL.md`  -  boot coordinator and main loop
- `kernel/PMM.md`  -  physical memory manager
- `kernel/HEAP.md`  -  kernel heap allocator
- `kernel/VMM.md`  -  virtual memory mapping
- `kernel/PANIC.md`  -  crash screen and bugcheck
- `kernel/TIME.md`  -  timekeeping and wall clock
- `kernel/TYPES.md`  -  core type aliases
- `kernel/SYSTEM.md`  -  utility functions (memcpy, strlen, etc.)

### Boot (`boot/`)

- `boot/BOOT_ASM.md`  -  early assembly boot path
- `boot/LINKER.md`  -  kernel linker script
- `boot/EFI_LOADER.md`  -  standalone EFI loader

### HAL (`hal/`)

- `hal/HAL.md`  -  IDT, PIC, interrupts, reboot

### Drivers (`drivers/`)

- `drivers/KEYBOARD.md`  -  PS/2 keyboard
- `drivers/MOUSE.md`  -  PS/2 mouse and touchpad
- `drivers/GRAPHICS.md`  -  framebuffer drawing primitives
- `drivers/DISPLAY.md`  -  display mode selection
- `drivers/DISPLAY_MGR.md`  -  multi-backend display manager
- `drivers/BGA.md`  -  Bochs Graphics Adapter
- `drivers/VIRTIO_GPU.md`  -  VirtIO GPU driver
- `drivers/SERIAL.md`  -  COM1 debug serial logger
- `drivers/TIMER.md`  -  PIT timer
- `drivers/RTC.md`  -  real time clock
- `drivers/E1000.md`  -  Intel E1000 NIC driver
- `drivers/GPU_PROBE.md`  -  PCI GPU detection and hybrid topology
- `drivers/AUDIO.md`  -  audio services (AC97 / HDA)
- `drivers/NVME.md`  -  NVMe storage driver
- `drivers/CPU_DETECT.md`  -  CPUID and feature detection

### UI (`ui/`)

- `ui/DESKTOP.md`  -  desktop, taskbar, icons, context menu
- `ui/WINDOW_MANAGER.md`  -  floating window system
- `ui/GUI.md`  -  higher-level drawing helpers
- `ui/WAYLAND_SERVER.md`  -  in-kernel Wayland compositor

### Apps (`apps/`)

- `apps/TASK_MANAGER.md`  -  task manager with kill/restart
- `apps/SETTINGS.md`  -  settings application
- `apps/TERMINAL.md`  -  terminal emulator
- `apps/TEXT_EDITOR.md`  -  text editor
- `apps/CALCULATOR.md`  -  calculator
- `apps/BROWSER.md`  -  HTTP browser
- `apps/MEDIA_PLAYER.md`  -  MP3 and image media player
- `apps/CONDUIT.md`  -  Linux guest integration app

### App Development (`apps/`, `kcl/`)
- `apps/KURONO_APP_DEV_GUIDE.md`  -  **how to create Kurono apps (.kro)**
- `kcl/KCL_REFERENCE.md`  -  KCL scripting language reference

### Shell (`shell/`)

- `shell/SHELL.md`  -  shell core, command registry, `kurono` command
- `shell/LINUX_CMDS.md`  -  POSIX/Linux commands

### Filesystem (`fs/`)

- `fs/KVFS.md`  -  in-memory virtual filesystem (runtime fs)
- `fs/KFS.md`  -  Kurono File System (on-disk persistence layer)
- `fs/VFS.md`  -  filesystem routing layer
- `fs/FAT32.md`  -  FAT32 disk filesystem

### System (`system/`)

- `system/UI_CONFIG.md`  -  runtime UI configuration system
- `system/INPUT_MANAGER.md`  -  input event routing
- `system/INSTALLER.md`  -  disk installer
- `system/LOGGING.md`  -  runtime logging + canonical path layout (`kpaths.h`)

### Network (`net/`)

- `net/NETWORK.md`  -  Ethernet, ARP, TCP/IP, DHCP

### Process (`proc/`)

- `proc/SCHEDULER.md`  -  process scheduler

### Security (`security/`)

- `security/SUPR.md`  -  privilege system

### Packages (`packages/`)

- `packages/PKGMGR.md`  -  apt-compatible package manager

### KCL (`kcl/`)

- `kcl/KCL.md`  -  Kurono Configuration Language

### Linux Integration (`linux/`)

- `linux/KLS.md`  -  Kurono Linux Shell (Trixie personality)
- `linux/DUAL_BOOT.md`  -  integrated Debian Linux boot
- `linux/LINUX_SYSCALL.md`  -  Linux syscall compatibility layer
- `linux/LINUX_NETBRIDGE.md`  -  guest network bridge

### Virtualization (`virt/`)

- `virt/HYPERVISOR.md`  -  Intel VT-x hypervisor
- `virt/EPT.md`  -  Extended Page Tables
- `virt/VMEXIT.md`  -  VM exit dispatch
- `virt/VDEVICES.md`  -  virtual devices (VirtIO NIC, disk, serial, 9P)

### Media (`media/`)

- `media/MEDIADECODER.md`  -  image and MP3 decoder

### Customization (`customization/`)

- `customization/UI_CUSTOMIZATION.md`  -  how to customize colors and layouts
- `customization/KURONO_COMMAND.md`  -  `kurono reload`, `info`, `config` commands

## Suggested Reading Order for a New Contributor

1. Start with `how-it-works/BOOT_TO_DESKTOP.md`
2. Continue with `routing/COMMAND_ROUTING.md`
3. Read `routing/UI_INPUT_ROUTING.md`
4. Keep `reference/FILE_MAP.md` open while reading code
5. For UI customization work: `customization/UI_CUSTOMIZATION.md`
6. Use `HYBRID_GPU_OPTIMUS_GUIDE.md` when working on graphics or laptop display paths

## Working Model

A good way to approach Kurono OS is to think in terms of routes.

The boot route turns firmware state into a usable kernel runtime.

The shell route turns a command line into a backend selection and then into a subsystem action.

The UI route turns mouse and keyboard state into desktop actions, window manager actions, and finally application callbacks.

The virtualization route turns a shell command into a guest configuration, then into a VM entry loop, and finally into a virtual device conversation.

The installer route turns real disk discovery into a deployment plan and then into boot payload installation.

Once those routes make sense, the rest of the codebase stops feeling random.
