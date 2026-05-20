# Boot to Desktop

This document explains the live path from firmware handoff to the Kurono desktop.

## 1. Boot entry and early machine state

The boot chain begins in `src/boot`.

`kurono_boot.asm` and `multiboot_header.S` establish the loader contract. The linker script in `kurono_linker.ld` shapes the kernel image. The assembly side prepares the early environment and hands off into the C++ kernel.

Kurono supports a Multiboot path and an EFI loader path. The important point for the rest of the system is simple: by the time `kernel_main()` starts, the kernel expects to have a valid boot information structure and to be in 64 bit mode.

## 2. The real boot coordinator

The entire high level boot sequence lives in `src/kernel/kurono_kernel.cpp`.

That file is not a stub and not a placeholder. It is the project’s boot script in C++ form.

The kernel starts by doing four jobs immediately.

1. It brings up serial logging.
2. It normalizes Multiboot2 input into the older Multiboot1 compatible structure when needed.
3. It initializes the panic subsystem early so crash rendering has a framebuffer path.
4. It attempts early VGA text and early framebuffer diagnostics before the larger runtime is online.

This is why `kurono_kernel.cpp` is the first file to read when something fails before the desktop appears.

## 3. Core system bring up

After boot validation, the kernel initializes the core services in a practical order.

1. HAL
2. Memory manager
3. Scheduler
4. VFS
5. Test suite
6. Timer and timekeeping
7. Display path
8. Input path
9. Filesystem population and subsystem registration
10. Desktop and main loop

The project is intentionally conservative here. Many later systems depend on earlier ones already working, so the sequence matters.

## 4. Panic system placement

The panic system is initialized before the main display stack is fully online.

That decision is important.

If graphics fails later, the panic subsystem still has a better chance of drawing a crash screen because it already learned the framebuffer details from boot services. The panic path also keeps a VGA fallback and a hard reboot path.

## 5. Display path

Kurono tries the display in tiers.

1. It prefers the bootloader or GOP supplied framebuffer.
2. It can fall back to VBE details when available.
3. It can use BGA in virtual hardware cases.

Before accepting any framebuffer, the kernel runs `GpuProbe::ScanAll()` to detect all display controllers on the PCI bus. This is critical for hybrid GPU laptops. The probe identifies Intel iGPU, NVIDIA dGPU, AMD APU/dGPU, and virtual GPUs (QEMU, VMware, VirtIO). It classifies the topology as Optimus (muxless or MUX), PowerXpress, dual discrete, or single GPU.

On Optimus laptops, the GRUB-reported framebuffer address may point to the wrong GPU. The GPU probe validates the address by reading the Intel DSPSURF (display surface) register from MMIO BAR0. If the address does not match the actual active surface, the kernel corrects it before initializing graphics.

Once a display path is accepted, the display manager selects the backend (BGA, VirtIO GPU, Intel, NVIDIA, or AMD) based on the GPU probe results. The graphics layer takes over framebuffer operations with write-combining remap via PAT to ensure cached writes do not disappear on real hardware. The panic framebuffer metadata is updated so crash rendering tracks the live mode.

The Wayland compositor initializes after the graphics layer is ready. It listens at `/system/run/user/1000/wayland-0` and advertises the standard libwayland globals. The compositor runs in-kernel rather than as a separate user-space process.

This layered approach is why the project can show both an early boot image and a later desktop on the same hardware without a complete modesetting implementation.

## 6. Input path

The boot code brings up the keyboard and mouse around the display phase because the desktop and emergency shells both depend on them.

The important operational detail is that laptop hardware is touchy. The keyboard and touchpad often share a fragile 8042 or embedded controller path. That is why the initialization order has been treated as a real compatibility concern rather than a cosmetic preference.

The current flow initializes keyboard support, initializes the PS/2 auxiliary path, then re-arms the keyboard before the input manager is attached.

## 7. Filesystem and runtime layout

Once the machine is interactive, the kernel builds the live runtime layout.

`KVFS::Init()` creates the in memory filesystem tree used by the desktop, shell, logs, and demo files. Runtime logging mirrors boot and system events into the filesystem so the machine can inspect itself after boot.

The kernel then populates familiar locations such as `/home/user`, `/etc`, `/tmp`, `/var/log`, and `/usr/bin`.

## 8. Shell and subsystem registration

The shell is initialized before the desktop begins normal life.

The kernel registers Linux commands, Windows commands, installer commands, package manager commands, KCL, Linux integration pieces, and other subsystem surfaces while it is still in the boot phase. In other words, the shell is not a late plugin. It is part of the system bring up process.

## 9. Linux and virtualization policy

Kurono’s Linux subsystem and Kurono’s hypervisor are related but not identical.

The Linux subsystem inside `src/linux` is a personality and syscall environment implemented by the native kernel.

The hypervisor inside `src/virt` is a separate machine route that can boot embedded Linux guests.

Normal boot defers risky virtualization hardware bring up until the user explicitly asks for VM features. This is a deliberate safety choice for real laptops and not an omission.

## 10. Desktop bring up

After the system services are ready, `DesktopEnvironment::Init()` sets up the desktop, taskbar, icons, windows, and launch surfaces. The applications themselves live in `src/apps`, but the desktop owns the interaction model that decides when they open and how they receive events.

## 11. The main loop

The main loop in `kurono_kernel.cpp` is the live heartbeat of the operating system.

Every iteration performs a practical set of work.

1. Advance time from PIT measurements.
2. Poll deferred settings work.
3. Tick audio.
4. Poll the NIC when present.
5. Poll input.
6. Tick the scheduler.
7. Drain mouse events.
8. Drain keyboard characters.
9. Hand both into the desktop environment.
10. Update and render the desktop.
11. Refresh task manager and other periodic views.

The kernel stays mostly polling based. That design decision shows up everywhere from timer handling to input and is one of the strongest themes in the entire repository.

## 12. Emergency mode

The same kernel can boot into an emergency recovery profile.

In that mode, the system prepares a smaller runtime, uses a VGA oriented shell path, registers the minimal services needed for diagnosis, and keeps command access available even if the desktop route is not safe.

## 13. Practical debugging advice

When the boot path fails, the fastest inspection order is usually the following.

1. `src/kernel/kurono_kernel.cpp`
2. `src/kernel/panic.cpp`
3. `src/hal/hal.cpp`
4. `src/drivers/display.cpp`
5. `src/drivers/graphics.cpp`
6. `src/drivers/keyboard.cpp`
7. `src/drivers/mouse.cpp`

That order matches the real bring up chain and usually saves time.
