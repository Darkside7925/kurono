# Project tour

Where things live.

## Repo root

What you actually need:

1. `README.md`
2. `STATUS.md`
3. `start.sh` (Linux/KVM, what I use) and `start.ps1` (Windows/WHPX)
4. `docs/`
5. `src/`

## What each `src/` folder owns

- `src/boot` gets the machine into a sane execution state
- `src/kernel` boot order, crash handling, core types, time, low-level memory
- `src/hal` interrupts, reboot, the low-level CPU and PIC surface
- `src/drivers` hardware
- `src/ui` desktop shell, window system, in-kernel Wayland compositor
- `src/apps` the built-in programs
- `src/shell` command parsing and routing
- `src/fs` the virtual filesystem model
- `src/net` the TCP/IP stack
- `src/linux` the Linux personality
- `src/virt` hypervisor and virtual machines
- `src/system` glue: input manager, installer logic, runtime logging

## Files people actually live in

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

## Desktop work

1. `src/ui/desktop.cpp`
2. `src/ui/window_manager.cpp`
3. `src/ui/wayland_server.cpp`
4. `src/apps/terminal.cpp`
5. `src/apps/settings.cpp`
6. `src/system/input_manager.cpp`

## Shell work

1. `src/shell/shell.cpp`
2. `src/shell/linux_cmds.cpp`
3. `src/shell/windows_cmds.cpp`
4. `src/kcl/kcl.cpp`
5. `src/system/installer.cpp`

## Boot or hardware work

1. `src/boot/kurono_boot.asm`
2. `src/kernel/kurono_kernel.cpp`
3. `src/kernel/panic.cpp`
4. `src/hal/hal.cpp`
5. `src/drivers/display.cpp`
6. `src/drivers/graphics.cpp`

## Virtualization work

1. `src/virt/hypervisor.cpp`
2. `src/virt/vmm.cpp`
3. `src/virt/vmexit.cpp`
4. `src/virt/vdevices.cpp`
5. `src/virt/linux_boot.cpp`

## Storage or installer work

1. `src/system/installer.cpp`
2. `src/fs/vfs.cpp`
3. `src/fs/kvfs.cpp`
4. `src/fs/fat32.cpp`
5. `src/linux/ext4.cpp`
6. `src/drivers/nvme.cpp`

## Not a toy

Stopped being a teaching kernel a while back. It's an OS lab: desktop, shell,
drivers, installer, Linux personality and hypervisor all in one tree. That's why the
docs are split by route instead of being one manual.
