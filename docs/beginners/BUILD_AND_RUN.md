# Build and Run

This document describes the practical build path that matches the current repository.

## 1. The easiest entry point

From Windows PowerShell, the easiest first command is the helper script at the repository root.

`start.ps1` is the normal entry point for build and launch flow.

## 2. The build reality in this workspace

The repository contains a VS Code task named `Build OS`, but in the current environment plain `make` from PowerShell is not enough by itself because the useful build path is the WSL based one.

The working build route used in this workspace is the `src` folder under WSL.

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
3. `src/drivers/graphics.cpp`
4. `src/kernel/panic.cpp`
5. `docs/HYBRID_GPU_OPTIMUS_GUIDE.md`

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
