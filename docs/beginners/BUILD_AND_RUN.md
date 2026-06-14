# Build and Run

This document describes the practical build path that matches the current repository.

## 1. The easiest entry point

The project's primary dev host is now **Linux + KVM**, so the easiest first
command is the Linux launcher at the repository root:

```bash
./start.sh                 # build the ISO and boot it (KVM, virtio-gpu, audio)
./start.sh --no-build      # boot the existing ISO without rebuilding
./start.sh --clean         # clean rebuild, then boot
./start.sh --std           # plain framebuffer instead of virtio-gpu
./start.sh --uefi          # UEFI boot path
./start.sh --debug         # gdb stub on :1234
```

On a Windows host the older PowerShell launcher `start.ps1` (WHPX) still works
and drives `make iso` inside WSL.

## 2. The build reality in this workspace

Builds run from the `src/` folder with `make` on a Linux toolchain (on Windows
the same `make` is driven through WSL). The repository also contains a VS Code
task named `Build OS`.

## 3. The important output files

1. `build/kurono.elf`
2. `build/kurono.iso`
3. `iso/kurono.iso`
4. `build/boot/kurono_efi.efi`
5. `build/boot/kurono_emergency.efi`

## 4. Why single job ISO builds matter

For this project, `make -j1 iso` has proven safer than a more parallel ISO step because some boot artifacts can change while the ISO is being assembled. When that happens, image creation can fail late even though most of the compile finished correctly.

## 5. Good first test sequence

### First test

Confirm the kernel and ISO build.

### Second test

Boot the ISO in QEMU.

### Third test

Check serial logging if the display path behaves strangely.

### Fourth test

Only after the virtual path looks sane should a risky hardware test follow.

## 6. When the screen stays black

A black screen does not automatically mean the kernel is dead.

The likely places to check are these.

1. `src/kernel/kurono_kernel.cpp`
2. `src/drivers/display.cpp`
3. `src/drivers/display_mgr.cpp`
4. `src/drivers/graphics.cpp`
5. `src/drivers/gpu_probe.cpp`
6. `src/kernel/panic.cpp`
7. `docs/HYBRID_GPU_OPTIMUS_GUIDE.md`

## 7. When the keyboard or touchpad breaks

The likely places to check are these.

1. `src/kernel/kurono_kernel.cpp`
2. `src/system/input_manager.cpp`
3. `src/drivers/keyboard.cpp`
4. `src/drivers/mouse.cpp`
5. `src/ui/desktop.cpp`

## 8. When a shell command behaves wrong

The likely places to check are these.

1. `src/shell/shell.cpp`
2. `src/shell/linux_cmds.cpp`
3. `src/shell/windows_cmds.cpp`
4. the subsystem file that owns the target behavior

## 9. When a VM boot fails

The likely places to check are these.

1. `src/virt/hypervisor.cpp`
2. `src/virt/vmm.cpp`
3. `src/virt/vmexit.cpp`
4. `src/virt/linux_boot.cpp`
5. `src/shell/linux_cmds.cpp`

## 10. Practical advice

Do not treat every failure as a random regression.

Kurono is structured enough that most failures can be placed onto one route quickly. Once the route is known, the candidate file set gets much smaller.
