# Build and run

How it actually builds here. The full version is `../developers/build.md`.

## The easy way

Main dev host is Linux with KVM, so use the launcher from the repo root:

```bash
./start.sh                 # build the ISO and boot it (KVM, virtio-gpu, audio)
./start.sh --no-build      # boot the existing ISO
./start.sh --clean         # clean rebuild then boot
./start.sh --std           # plain framebuffer instead of virtio-gpu
./start.sh --uefi          # UEFI boot path
./start.sh --debug         # gdb stub on :1234
```

On Windows `start.ps1` drives `make iso` through WSL. Needs a WSL distro installed.

## By hand

```bash
cd src
make iso -j$(nproc) KURONO_KERNEL_BASE=0x14000000
```

You need `musl-gcc` (Debian: `apt install musl-tools`), NASM, a native g++ (or an
`x86_64-elf` cross toolchain), `grub-mkrescue`, `xorriso` and `mtools`. The
`KURONO_KERNEL_BASE` value links the kernel higher in memory so big fixed-address Linux
binaries don't collide with it. I always set it.

## Output

1. `build/kurono.elf` full kernel
2. `build/kurono_base.elf` the kernel the GRUB entries load
3. `build/kurono.iso`
4. `build/boot/kurono_efi.efi` and `build/boot/kurono_emergency.efi`
5. `build/boot/kurono_efi_gpu.efi` only if you ran `make gpu-efi`

## Don't rebuild the ISO in parallel with itself

If an ISO build fails late, run it again with `-j1`. Parallel ISO steps have bitten me:
boot artifacts can change while the image is being assembled.

## First tests, in order

1. does the kernel and ISO build
2. does the ISO boot in QEMU
3. does serial look sane if the display acts weird
4. only then risk real hardware (`boot-on-hardware.md`)

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
3. `src/drivers/keyboard.cpp`, `src/drivers/mouse.cpp`
4. `src/drivers/usb.cpp` (USB keyboards and mice, including hubs)
5. `src/ui/desktop.cpp`

## Shell command misbehaving

1. `src/shell/shell.cpp`
2. `src/shell/linux_cmds.cpp`
3. whatever subsystem owns the behaviour you expected

## VM (hypervisor) boot fails

1. `src/virt/hypervisor.cpp`
2. `src/virt/vmm.cpp`
3. `src/virt/vmexit.cpp`
4. `src/virt/linux_boot.cpp`
5. `src/shell/linux_cmds.cpp`

The hypervisor needs VT-x. Under QEMU that means nested virtualization.

## One thing

Don't treat every failure as a random regression. This is structured enough that
almost everything lands on one route fast, and then the list of files to look at is
small.
