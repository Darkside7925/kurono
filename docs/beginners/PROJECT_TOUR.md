# Project Tour

This document is the friendly source tree walk.

## 1. Top level

At the repository root, the files that matter most to a newcomer are these.

1. `README.md`
2. `STATUS.md`
3. `start.ps1`
4. `docs/`
5. `src/`

`archive/` exists for history and recovery, not for day to day development.

## 2. The source tree at a glance

### `src/boot`

This folder gets the machine into the right execution state.

### `src/kernel`

This folder owns boot order, crash handling, core types, time, and low level memory helpers.

### `src/hal`

This folder owns interrupts, reboot, and the low level CPU and PIC facing surface.

### `src/drivers`

This folder owns hardware.

### `src/ui`

This folder owns the desktop shell, window system, and the in-kernel Wayland compositor.

### `src/apps`

This folder owns the built in programs.

### `src/shell`

This folder owns command parsing and command execution routing.

### `src/fs`

This folder owns the virtual filesystem model.

### `src/net`

This folder owns the TCP IP stack.

### `src/linux`

This folder owns the Linux style subsystem inside Kurono.

### `src/virt`

This folder owns the hypervisor and virtual machines.

### `src/system`

This folder owns glue services such as input management, installer logic, and runtime logging.

## 3. The files most people actually live in

For a lot of work, the real center of gravity is this smaller set.

1. `src/kernel/kurono_kernel.cpp`
2. `src/kernel/panic.cpp`
3. `src/hal/hal.cpp`
4. `src/drivers/graphics.cpp`
5. `src/drivers/keyboard.cpp`
6. `src/drivers/mouse.cpp`
7. `src/ui/desktop.cpp`
8. `src/ui/window_manager.cpp`
9. `src/shell/shell.cpp`
10. `src/system/installer.cpp`
11. `src/virt/hypervisor.cpp`
12. `src/linux/linux_syscall.cpp`

## 4. If the goal is desktop work

Read these first.

1. `src/ui/desktop.cpp`
2. `src/ui/window_manager.cpp`
3. `src/ui/wayland_server.cpp`
4. `src/apps/terminal.cpp`
5. `src/apps/settings.cpp`
6. `src/system/input_manager.cpp`

## 5. If the goal is shell work

Read these first.

1. `src/shell/shell.cpp`
2. `src/shell/linux_cmds.cpp`
3. `src/shell/windows_cmds.cpp`
4. `src/kcl/kcl.cpp`
5. `src/system/installer.cpp`

## 6. If the goal is boot or hardware work

Read these first.

1. `src/boot/kurono_boot.asm`
2. `src/kernel/kurono_kernel.cpp`
3. `src/kernel/panic.cpp`
4. `src/hal/hal.cpp`
5. `src/drivers/display.cpp`
6. `src/drivers/graphics.cpp`

## 7. If the goal is virtualization work

Read these first.

1. `src/virt/hypervisor.cpp`
2. `src/virt/vmm.cpp`
3. `src/virt/vmexit.cpp`
4. `src/virt/vdevices.cpp`
5. `src/virt/linux_boot.cpp`

## 8. If the goal is storage or installation work

Read these first.

1. `src/system/installer.cpp`
2. `src/fs/vfs.cpp`
3. `src/fs/kvfs.cpp`
4. `src/fs/fat32.cpp`
5. `src/linux/ext4.cpp`
6. `src/drivers/nvme.cpp`

## 9. The project personality

Kurono is not pretending to be a tiny teaching kernel anymore.

It behaves more like an operating system laboratory where the desktop, shell, driver, installer, Linux personality, and hypervisor all coexist in one codebase.

That is why the docs are split by routes and audience instead of by one linear manual.
