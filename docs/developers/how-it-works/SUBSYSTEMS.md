# Subsystems

This document describes the live subsystems in the repository and the job each one performs.

## 1. Boot

Folder: `src/boot`

Purpose: enter the kernel correctly, define the image layout, and bridge bootloader state into the runtime.

Important files:

| File | Role |
| --- | --- |
| `efi_loader.c` | Standalone EFI loader path |
| `kurono_boot.asm` | Early assembly entry and setup |
| `multiboot_header.S` | Multiboot contract |
| `kurono_linker.ld` | Kernel image layout |

## 2. Kernel core

Folder: `src/kernel`

Purpose: own boot sequencing, panic flow, low level types, time, memory helpers, PCI discovery helpers, and system level coordination.

Important files:

| File | Role |
| --- | --- |
| `kurono_kernel.cpp` | High level boot sequence and main loop |
| `panic.cpp` | BSOD path, crash screen rendering, reboot path |
| `time.cpp` | timekeeping and calendar functions |
| `types.*` | foundational types |
| `system.*` | core system helper layer |
| `pmm.cpp` | physical memory management |
| `vmm.cpp` | virtual memory management support |
| `heap.cpp` | heap allocator |
| `multiboot.h` | boot structure contract |
| `pci.h` | PCI probing helpers |

## 3. HAL

Folder: `src/hal`

Purpose: low level interrupt setup, port I/O, reboot, PIC handling, and exception dispatch.

The HAL is where the CPU meets the operating system policy.

Important files:

| File | Role |
| --- | --- |
| `hal.cpp` | IDT, PIC, exception route, reboot route |
| `isr_stubs.asm` in build chain | assembly stubs feeding the C handler |

## 4. Drivers

Folder: `src/drivers`

Purpose: talk to real or virtual devices.

This folder is broad because the project covers framebuffer graphics, audio, storage, USB, NICs, GPUs, serial, timer, RTC, and input.

The main driver groups are these.

### 4.1 Display and graphics

`display.*`, `graphics.*`, `bga.*`, `display_mgr.*`, `virtio_gpu.*`, `gpu_probe.*`, `intel_gpu.*`, `nvidia_gpu.*`, `amd_gpu.*`

These files own framebuffer primitives, mode handling, display detection, GPU probing, and hardware specific paths.

GPU probe scans the PCI bus for class 0x03 devices, identifies vendors, classifies hybrid topologies (Optimus, PowerXpress), and validates framebuffer addresses on Optimus laptops by reading Intel DSPSURF registers.

Display manager provides a multi-backend abstraction that routes to BGA, VirtIO GPU, Intel, NVIDIA, or AMD based on GPU probe results. It exposes 10 predefined modes, runtime mode switching, EDID reading, DPI scaling, gamma/brightness, and VSync control.

Graphics provides the framebuffer drawing primitives with double/triple buffering, SSE2 non-temporal store swaps, write-combining remap, dirty region tracking, frame pacing, blend modes, and accessibility color filters.

### 4.2 Input

`keyboard.*`, `mouse.*`, `usb.*`

These files own PS/2 keyboard handling, PS/2 mouse and touchpad style packet handling, and USB controller support.

### 4.3 Time and platform support

`timer.*`, `rtc.*`, `serial.*`, `cpu_detect.*`

These files own periodic timing, wall clock reads, serial logging, and CPUID feature reporting.

### 4.4 Storage and network

`nvme.*`, `e1000.*`

These files own disk transport and NIC transport. Installer code and higher network stack code sit on top of them.

### 4.5 Audio

`audio.*`, `ac97.*`, `hda.*`

These files own tone generation, DMA playback routes, and controller specific audio logic.

## 5. UI

Folder: `src/ui`

Purpose: render the desktop shell and route user interaction at the presentation layer.

Important files:

| File | Role |
| --- | --- |
| `desktop.cpp` | taskbar, desktop icons, launcher flow, input entry point |
| `window_manager.cpp` | window lifetime, focus, drag, resize, render ordering |
| `gui.cpp` | lower level drawing and layout helpers |
| `font.cpp`, `text_layout.cpp` | text rendering support |
| `lockscreen.cpp` | lock screen flow |
| `file_browser.cpp` | browser style UI support |
| `ui_elements.cpp` | reusable UI drawing pieces |

## 6. Applications

Folder: `src/apps`

Purpose: built in desktop programs.

Current application set:

| File pair | Role |
| --- | --- |
| `terminal.*` | shell host window |
| `file_manager.*` | filesystem browser |
| `calculator.*` | calculator UI |
| `text_editor.*` | editor window |
| `settings.*` | settings and resolution flow |
| `task_manager.*` | process and system inspection |
| `browser.*` | browser placeholder and shell |
| `media_player.*` | media viewing and playback surface |
| `conduit.*` | special system application and VM facing UX |

## 7. Shell

Folder: `src/shell`

Purpose: parse commands, manage command registration, resolve conflicts, switch environments, and route execution to the right subsystem or guest backend.

Important files:

| File | Role |
| --- | --- |
| `shell.cpp` | parser, registry, command execution, pipe logic |
| `linux_cmds.cpp` | Linux style commands |
| `windows_cmds.cpp` | Windows style commands |

## 8. Filesystems

Folders: `src/fs` and parts of `src/system`

Purpose: expose the in memory runtime filesystem, FAT32 handling, and install target generation.

Important files:

| File | Role |
| --- | --- |
| `vfs.*` | generic virtual filesystem surface |
| `kvfs.*` | in memory runtime filesystem |
| `fat32.*` | ESP facing FAT32 support |
| `linux/ext4.*` | ext4 parsing and install target logic |

## 9. Networking

Folder: `src/net`

Purpose: provide Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets, and interface management.

Important files:

| File | Role |
| --- | --- |
| `network.*` | interface model, socket surface, WiFi facade |
| `tcpip.*` | protocol machinery and packet work |

## 10. Linux subsystem

Folder: `src/linux`

Purpose: provide a Linux like execution personality inside Kurono itself.

This is not the same thing as booting a guest VM.

Important files:

| File | Role |
| --- | --- |
| `linux_kernel.*` | Linux personality core |
| `linux_syscall.*` | syscall entry and dispatch |
| `linux_signals.*` | signal model |
| `linux_devices.*` | device node behavior |
| `linux_init.*` | init and service style logic |
| `dual_boot.*` | integrated Linux boot coordination |
| `linux_netbridge.*` | network bridge between worlds |
| `shared_mount.*` | shared filesystem view logic |
| `user_bridge.*` | user facing bridge helpers |
| `ext4.*` | ext4 support |
| `kls.*` | Linux side shell helpers |
| `linux_drivers.*` | Linux driver integration surface |

## 11. Virtualization

Folder: `src/virt`

Purpose: own the type 1 hypervisor, VM creation, guest memory, virtual devices, guest boot paths, and VM exit processing.

Important files:

| File | Role |
| --- | --- |
| `hypervisor.*` | top level VM lifecycle manager |
| `vmm.*` | virtualization hardware abstraction |
| `ept.*` | EPT and NPT mapping logic |
| `vmexit.*` | VM exit decode and policy |
| `vdevices.*` | PIC, APIC, PIT, HPET and other virtual devices |
| `vserial.*` | virtual COM1 |
| `vdisk.*` | virtual disk |
| `guest_mem.*` | guest RAM setup |
| `linux_boot.*` | guest Linux load path |
| `iommu.*` | VT d and AMD Vi related work |
| `v9fs.*` | shared file protocol work |
| `alpine_data.h`, `debian_data.h` | embedded guest payload data |

## 12. System services

Folder: `src/system`

Purpose: bind subsystems together into usable operating system services.

Important files:

| File | Role |
| --- | --- |
| `input_manager.*` | input device registration and keyboard callback route |
| `logging.*` | runtime log mirroring |
| `installer.*` | disk discovery and deployment planning |
| `user_mgmt.*` | user data and account services |
| `conduit.*` | system bridge logic |

## 13. Security

Folder: `src/security`

Purpose: privilege management and escalation policy.

Main file: `supr.*`

## 14. Packages

Folder: `src/packages`

Purpose: package manager surface.

Main file: `pkgmgr.*`

## 15. Scripting

Folder: `src/kcl`

Purpose: Kurono Command Language.

Main file: `kcl.*`

## 16. Process scheduling

Folder: `src/proc`

Purpose: process objects, queue handling, tick based progression, and execution bookkeeping.

Main file: `scheduler.*`

## 17. Media and third party glue

Folders: `src/media` and `src/third_party`

Purpose: image and audio parsing, glue code for embedded third party decoders, and media side helpers consumed by the UI and apps.

## 18. Tests

Folder: `src/tests`

Purpose: boot time validation and system sanity checks.

Main file: `test_suite.*`

## 19. The practical subsystem rule

Most bugs in Kurono are not single file bugs. They are route bugs crossing at least two layers.

Typical examples are these.

1. A display failure often crosses boot, graphics, and panic.
2. An input failure often crosses kernel boot order, keyboard, mouse, and desktop routing.
3. A command failure often crosses shell registration, subsystem registration during boot, and the target subsystem implementation.
4. A VM boot failure often crosses shell command plumbing, hypervisor lifecycle, guest memory, VMCS or VMCB setup, and virtual devices.

That is why the routing documents matter as much as the folder map.
