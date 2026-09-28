# Build and run

How it actually builds here.

## The easy way

Main dev host is Linux with KVM, so use the launcher:

```bash
./start.sh                 # build the ISO and boot it (KVM, virtio-gpu, audio)
./start.sh --no-build      # boot the existing ISO
./start.sh --clean         # clean rebuild then boot
./start.sh --std           # plain framebuffer instead of virtio-gpu
./start.sh --uefi          # UEFI boot path
./start.sh --debug         # gdb stub on :1234
```

On Windows `start.ps1` still works and drives `make iso` through WSL. Needs a WSL
distro installed.

## Where it builds

`make` from `src/`, Linux toolchain. There's a VS Code task called `Build OS` too.

## Output

1. `build/kurono.elf`
2. `build/kurono.iso`
3. `iso/kurono.iso`
4. `build/boot/kurono_efi.efi`
5. `build/boot/kurono_emergency.efi`

## Build the ISO single job

`make -j1 iso`. Parallel ISO steps have bitten me: boot artifacts can change while the
image is being assembled, and then it fails late even though the compile was fine.

## First tests, in order

1. does the kernel and ISO build
2. does the ISO boot in QEMU
3. does serial look sane if the display acts weird
4. only then risk real hardware

## Black screen

Doesn't mean the kernel is dead. Look here:

1. `src/kernel/kurono_kernel.cpp`
2. `src/drivers/display.cpp`
3. `src/drivers/display_mgr.cpp`
4. `src/drivers/graphics.cpp`
5. `src/drivers/gpu_probe.cpp`
6. `src/kernel/panic.cpp`
7. `docs/HYBRID_GPU_OPTIMUS_GUIDE.md`

## Keyboard or touchpad dead

1. `src/kernel/kurono_kernel.cpp`
2. `src/system/input_manager.cpp`
3. `src/drivers/keyboard.cpp`
4. `src/drivers/mouse.cpp`
5. `src/ui/desktop.cpp`

## Shell command misbehaving

1. `src/shell/shell.cpp`
2. `src/shell/linux_cmds.cpp`
3. `src/shell/windows_cmds.cpp`
4. whatever subsystem owns the behaviour you expected

## VM boot fails

1. `src/virt/hypervisor.cpp`
2. `src/virt/vmm.cpp`
3. `src/virt/vmexit.cpp`
4. `src/virt/linux_boot.cpp`
5. `src/shell/linux_cmds.cpp`

## One thing

Don't treat every failure as a random regression. This is structured enough that
almost everything lands on one route fast, and then the list of files to look at is
small.
