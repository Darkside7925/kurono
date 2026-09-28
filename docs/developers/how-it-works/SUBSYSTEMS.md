# Subsystems

What's in the tree and what each piece does.

## Boot (`src/boot`)

Get into the kernel correctly, define the image layout, bridge bootloader state into the runtime.

| File | Role |
| --- | --- |
| `efi_loader.c` | standalone EFI loader path |
| `kurono_boot.asm` | early assembly entry and setup |
| `multiboot_header.S` | Multiboot contract |
| `kurono_linker.ld` | kernel image layout |

## Kernel core (`src/kernel`)

Boot sequencing, panic flow, low-level types, time, memory helpers, PCI discovery helpers, system
coordination.

| File | Role |
| --- | --- |
| `kurono_kernel.cpp` | high-level boot sequence and main loop |
| `panic.cpp` | BSOD path, crash screen, reboot |
| `time.cpp` | timekeeping and calendar |
| `types.*` | foundational types |
| `system.*` | core system helpers |
| `pmm.cpp` | physical memory |
| `vmm.cpp` | virtual memory support |
| `heap.cpp` | heap allocator |
| `multiboot.h` | boot structure contract |
| `pci.h` | PCI probing helpers |

## HAL (`src/hal`)

Low-level interrupt setup, port I/O, reboot, PIC handling, exception dispatch. This is where the CPU
meets OS policy.

| File | Role |
| --- | --- |
| `hal.cpp` | IDT, PIC, exception route, reboot route |
| `isr_stubs.asm` | assembly stubs feeding the C handler |

## Drivers (`src/drivers`)

Real and virtual devices. Broad, because the project covers framebuffer graphics, audio, storage,
USB, NICs, GPUs, serial, timer, RTC and input.

### Display and graphics

`display.*`, `graphics.*`, `bga.*`, `display_mgr.*`, `virtio_gpu.*`, `gpu_probe.*`, `intel_gpu.*`,
`nvidia_gpu.*`, `amd_gpu.*`. Framebuffer primitives, mode handling, display detection, GPU probing,
vendor paths.

`gpu_probe` scans PCI for class 0x03 devices, identifies vendors, classifies hybrid topologies
(Optimus, PowerXpress), and validates framebuffer addresses on Optimus laptops by reading Intel
DSPSURF registers.

`display_mgr` is the multi-backend layer: BGA, VirtIO GPU, Intel, NVIDIA or AMD based on the probe
results, with 10 predefined modes, runtime switching, EDID reading, DPI scaling, gamma/brightness
and VSync.

`graphics` has the drawing primitives with double/triple buffering, SSE2 non-temporal store swaps,
write-combining remap, dirty region tracking, frame pacing, blend modes and accessibility colour
filters.

### Input

`keyboard.*`, `mouse.*`, `usb.*`. PS/2 keyboard, PS/2 mouse and touchpad-style packet handling, USB
controller support.

### Time and platform

`timer.*`, `rtc.*`, `serial.*`, `cpu_detect.*`. Periodic timing, wall clock reads, serial logging,
CPUID feature reporting.

### Storage and network

`nvme.*`, `e1000.*`. Disk and NIC transport, with the installer and the higher network stack sitting
on top.

### Audio

`audio.*`, `ac97.*`, `hda.*`. Tone generation, DMA playback routes, controller-specific logic.

## UI (`src/ui`)

Desktop shell and interaction routing at the presentation layer.

| File | Role |
| --- | --- |
| `desktop.cpp` | taskbar, desktop icons, launcher flow, input entry point |
| `window_manager.cpp` | window lifetime, focus, drag, resize, render ordering |
| `gui.cpp` | lower-level drawing and layout helpers |
| `font.cpp`, `text_layout.cpp` | text rendering |
| `lockscreen.cpp` | lock screen flow |
| `file_browser.cpp` | browser-style UI support |
| `ui_elements.cpp` | reusable UI drawing pieces |

## Apps (`src/apps`)

The built-in desktop programs.

| File pair | Role |
| --- | --- |
| `terminal.*` | shell host window |
| `file_manager.*` | filesystem browser |
| `calculator.*` | calculator |
| `text_editor.*` | editor window |
| `settings.*` | settings and resolution flow |
| `task_manager.*` | process and system inspection |
| `browser.*` | browser placeholder |
| `media_player.*` | media viewing and playback |
| `conduit.*` | system app and VM-facing UX |

## Shell (`src/shell`)

Parse commands, manage registration, resolve conflicts, switch environments, and route execution to
the right subsystem or guest backend.

| File | Role |
| --- | --- |
| `shell.cpp` | parser, registry, execution, pipe logic |
| `linux_cmds.cpp` | Linux-style commands |
| `windows_cmds.cpp` | Windows-style commands |

## Filesystems (`src/fs` plus parts of `src/system`)

The in-memory runtime filesystem, FAT32 handling, install target generation.

| File | Role |
| --- | --- |
| `vfs.*` | generic virtual filesystem surface |
| `kvfs.*` | in-memory runtime filesystem |
| `fat32.*` | ESP-facing FAT32 |
| `linux/ext4.*` | ext4 parsing and install target logic |

## Networking (`src/net`)

Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets and interface management.

| File | Role |
| --- | --- |
| `network.*` | interface model, socket surface, WiFi facade |
| `tcpip.*` | protocol machinery and packet work |

## Linux subsystem (`src/linux`)

A Linux-like execution personality inside Kurono. Not the same thing as booting a guest VM.

| File | Role |
| --- | --- |
| `linux_kernel.*` | Linux personality core |
| `linux_syscall.*` | syscall entry and dispatch |
| `linux_signals.*` | signal model |
| `linux_devices.*` | device node behaviour |
| `linux_init.*` | init and service-style logic |
| `dual_boot.*` | integrated Linux boot coordination |
| `linux_netbridge.*` | network bridge between the two worlds |
| `shared_mount.*` | shared filesystem view |
| `user_bridge.*` | user-facing bridge helpers |
| `ext4.*` | ext4 support |
| `kls.*` | Linux-side shell helpers |
| `linux_drivers.*` | Linux driver integration surface |

## Virtualization (`src/virt`)

The type-1 hypervisor, VM creation, guest memory, virtual devices, guest boot and VM exit
processing.

| File | Role |
| --- | --- |
| `hypervisor.*` | top-level VM lifecycle |
| `vmm.*` | virtualization hardware abstraction |
| `ept.*` | EPT and NPT mapping |
| `vmexit.*` | VM exit decode and policy |
| `vdevices.*` | PIC, APIC, PIT, HPET and other virtual devices |
| `vserial.*` | virtual COM1 |
| `vdisk.*` | virtual disk |
| `guest_mem.*` | guest RAM setup |
| `linux_boot.*` | guest Linux load path |
| `iommu.*` | VT-d and AMD-Vi work |
| `v9fs.*` | shared file protocol |
| `alpine_data.h`, `debian_data.h` | embedded guest payloads |

## System services (`src/system`)

Binds subsystems together into usable OS services.

| File | Role |
| --- | --- |
| `input_manager.*` | input device registration and keyboard callback route |
| `logging.*` | runtime logging: boot, system, serial, network, security, crash |
| `kpaths.h` | the canonical `/kurono` on-disk path layout, single source of truth |
| `installer.*`, `installer_gui.*` | disk discovery and deployment planning, plus the graphical wizard |
| `runtime_layout.*` | Linux runtime layout seeder, the `/system` tree and firefox.env |
| `dbus_server.*` | D-Bus session bus daemon |
| `system_update.*` | boot-time staged rootfs and update flow |
| `ui_config.*` | `/etc/kurono/ui.conf` theme tokens |
| `user_mgmt.*` | user data and account services |
| `conduit.*` | system bridge logic, also the GUI Conduit app |

## Security (`src/security`)

Privilege management and escalation policy: `supr.*` (the SUPR engine, roles, auth policy, the
`supr <cmd>` sudo-style reroute) and `ksa.*` (hypervisor-backed authorization prompts in an
EPT-isolated region, see `security/KSA.md`).

## Packages (`src/packages`)

The package manager surface: `pkgmgr.*`.

## Scripting (`src/kcl`, `src/apps`)

In-OS scripting languages:

- `kcl/kcl.*` KCL, the Kurono Command Language, a tree-walking interpreter
- `apps/kj.*` KJ, Kurono JavaScript, a freestanding JS-subset interpreter exposed as the `kj` and
  `node` commands, with `kss` and `ui` host bindings

## Process scheduling (`src/proc`)

Process objects, queue handling, tick-based progression, execution bookkeeping and multi-core
bring-up: `scheduler.*` (the cross-core ready-queue lock and per-CPU pick), `smp.*` (LAPIC, MADT
parse, AP bring-up, per-CPU blocks, see `proc/SMP.md`), `kernel_processes.*` (built-in kernel
processes), `cgroup.*` (cgroups v2).

## Media and third-party glue (`src/media`, `src/third_party`)

Image and audio parsing, glue for the embedded decoders, and the media helpers the UI and apps use.

## Tests (`src/tests`)

Boot-time validation and system sanity checks: `test_suite.*`.

## The practical rule

Most bugs here are not single-file bugs. They're route bugs crossing at least two layers:

1. a display failure usually crosses boot, graphics and panic
2. an input failure usually crosses kernel boot order, keyboard, mouse and desktop routing
3. a command failure usually crosses shell registration, subsystem registration during boot, and
   the target subsystem implementation
4. a VM boot failure usually crosses shell command plumbing, hypervisor lifecycle, guest memory,
   VMCS or VMCB setup, and virtual devices

That's why the routing docs matter as much as the folder map.
