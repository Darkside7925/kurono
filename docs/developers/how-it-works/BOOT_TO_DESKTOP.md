# Boot to desktop

The live path from firmware handoff to the desktop.

## Entry

The boot chain starts in `src/boot`. `kurono_boot.asm` and `multiboot_header.S` establish the
loader contract, `kurono_linker.ld` shapes the image, and the assembly side prepares the early
environment and hands off to C++.

Two entry paths: Multiboot and the EFI loader. What matters for the rest of the system is
simple. By the time `kernel_main()` starts, the kernel expects a valid boot information
structure and 64-bit mode.

## kurono_kernel.cpp is the real coordinator

The whole high-level boot sequence lives in `src/kernel/kurono_kernel.cpp`. Not a stub, it's
the project's boot script written in C++.

It starts with four jobs:

1. bring up serial logging
2. normalise Multiboot2 input into the older Multiboot1-compatible structure when needed
3. init panic early, so crash rendering has a framebuffer path
4. try early VGA text and early framebuffer diagnostics before the big runtime is online

That's why this file is the first thing to read when something fails before the desktop shows
up.

## Core bring-up order

1. HAL
2. memory manager
3. scheduler
4. VFS
5. the test suite
6. timer and timekeeping
7. display path
8. input path
9. filesystem population and subsystem registration
10. desktop, then the main loop

Conservative on purpose. Plenty of later systems assume the earlier ones already work, so the
sequence matters.

## Panic sits early

Panic comes up before the display stack is fully online. That matters. If graphics fails
later, panic already learned the framebuffer details from boot services, so it has a better
chance of drawing. It also keeps the VGA fallback and the hard reboot path.

## Display, in tiers

1. the bootloader or GOP framebuffer first
2. VBE details where available
3. BGA for virtual hardware

Before accepting anything, the kernel runs `GpuProbe::ScanAll()` to find every display
controller on PCI. That's what makes hybrid laptops work: the probe spots Intel iGPU, NVIDIA
dGPU, AMD APU/dGPU and virtual GPUs (QEMU, VMware, VirtIO), and classifies the topology as
Optimus (muxless or MUX), PowerXpress, dual discrete or single.

On Optimus laptops the GRUB-reported framebuffer address can point at the wrong GPU. The probe
validates it by reading Intel's DSPSURF (display surface) register out of MMIO BAR0, and
corrects it before graphics init if it doesn't match the live surface.

Once a path is accepted, the display manager picks a backend (BGA, VirtIO GPU, Intel, NVIDIA
or AMD) from the probe results. The graphics layer takes over framebuffer operations with a
write-combining remap through PAT, so cached writes don't vanish on real hardware. Panic's
framebuffer metadata gets updated so crash rendering tracks the live mode.

The Wayland compositor comes up after graphics is ready. It listens on
`/system/run/user/1000/wayland-0` and advertises the standard libwayland globals. It runs
in-kernel, not as a separate userspace process.

That layering is why the project can show an early boot image and later a desktop on the same
hardware without a full modesetting implementation.

## Input

Keyboard and mouse come up around the display phase, because the desktop and the emergency
shell both need them.

Laptop hardware is touchy here. The keyboard and touchpad usually share a fragile 8042 or
embedded-controller path, so the init order is a real compatibility concern, not a cosmetic
preference. The flow is: init keyboard, init the PS/2 auxiliary path, re-arm the keyboard,
then attach the input manager.

## Filesystem and runtime layout

Once the machine is interactive, the kernel builds the live runtime layout. `KVFS::Init()`
creates the in-memory tree the desktop, shell, logs and demo files use. Runtime logging mirrors
boot and system events into the filesystem, so the machine can inspect itself after boot. Then
it populates the familiar locations: `/home/user`, `/etc`, `/tmp`, `/var/log`, `/usr/bin`.

## Shell and subsystem registration

The shell comes up before the desktop starts normal life. The kernel registers Linux commands,
Windows commands, installer commands, package manager commands, KCL, the Linux integration
pieces and other subsystem surfaces while it's still in the boot phase. The shell is not a late
plugin, it's part of bring-up.

## Linux and virtualization

Kurono's Linux subsystem and Kurono's hypervisor are related but not the same thing.

`src/linux` is a personality and syscall environment implemented by the native kernel. `src/virt`
is a separate machine route that boots embedded Linux guests.

Normal boot defers the risky virtualization hardware bring-up until you explicitly ask for VM
features. That's a deliberate safety choice for real laptops, not an omission.

## Desktop

Once the system services are ready, `DesktopEnvironment::Init()` sets up the desktop, taskbar,
icons, windows and launch surfaces. The apps live in `src/apps`, but the desktop owns the
interaction model that decides when they open and how they get events.

## The main loop

`kurono_kernel.cpp`'s main loop is the heartbeat. Every iteration does a practical set of work.

1. advance time from the PIT
2. drain deferred settings work
3. tick audio
4. poll the NIC if there is one
5. poll input
6. tick the scheduler
7. drain mouse events
8. drain keyboard characters
9. hand both to the desktop
10. update and render the desktop
11. refresh the task manager and other periodic views

Mostly polling. That choice shows up everywhere, from timer handling to input, and it's one of
the strongest themes in the repo.

## Emergency mode

The same kernel can boot into an emergency recovery profile: a smaller runtime, a VGA-oriented
shell path, only the services needed for diagnosis, and command access even when the desktop
route isn't safe.

## When boot fails, look here

1. `src/kernel/kurono_kernel.cpp`
2. `src/kernel/panic.cpp`
3. `src/hal/hal.cpp`
4. `src/drivers/display.cpp`
5. `src/drivers/graphics.cpp`
6. `src/drivers/keyboard.cpp`
7. `src/drivers/mouse.cpp`

That order matches the real bring-up chain and usually saves time.
