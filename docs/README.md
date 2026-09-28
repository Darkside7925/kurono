# Kurono OS docs

Two kinds of people read this: people who are new here, and people who are about to
change the code.

## How much code is actually in here

`src/` is about 130,000 lines of hand-written freestanding C++17, NASM and C. A raw
`wc -l` says ~337,000, but ~207,000 of that is wallpaper pixel data sitting in
`src/ui/wallpaper.h` and `src/ui/wallpaper2.h`. Not code. The vendored stb glue in
`src/third_party/` is another ~13,000.

## New here

1. `beginners/README.md`
2. `beginners/FIRST_STEPS.md`
3. `beginners/PROJECT_TOUR.md`
4. `beginners/BUILD_AND_RUN.md`

## Here to change code

1. `developers/README.md`
2. `developers/how-it-works/BOOT_TO_DESKTOP.md`
3. `developers/how-it-works/SUBSYSTEMS.md`
4. `developers/routing/COMMAND_ROUTING.md`
5. `developers/routing/UI_INPUT_ROUTING.md`
6. `developers/reference/FILE_MAP.md`. Keep it open while you read code.

## Display or graphics

1. `developers/drivers/GPU_PROBE.md` GPU detection, hybrid topology
2. `developers/drivers/DISPLAY_MGR.md` multi-backend display manager
3. `developers/drivers/GRAPHICS.md` framebuffer drawing primitives
4. `developers/drivers/BGA.md` Bochs Graphics Adapter
5. `developers/drivers/VIRTIO_GPU.md` VirtIO GPU driver
6. `developers/ui/WAYLAND_SERVER.md` in-kernel Wayland compositor
7. `developers/ui/WINDOW_MANAGER.md` compositing window manager

## Storage, persistence, paths

1. `developers/fs/KVFS.md` the in-memory runtime filesystem
2. `developers/fs/KFS.md` the on-disk persistence filesystem (extents, no limits)
3. `developers/system/LOGGING.md` the canonical `/kurono` layout and logging
4. `developers/drivers/NVME.md` the block device under KFS

## Linux runtime

1. `developers/linux/LINUX_SYSCALL.md` the in-kernel syscall runtime
2. `developers/linux/LD_KURONO.md` the in-kernel ELF64 dynamic linker
3. `developers/ui/WAYLAND_SERVER.md` the compositor real clients talk to
4. `developers/net/NETWORK.md` TCP/IP and AF_UNIX sockets

## Security and multi-core

1. `developers/security/SUPR.md` privilege engine and roles
2. `developers/security/KSA.md` hypervisor-backed authorization prompts
3. `developers/proc/SCHEDULER.md` the cooperative plus preemptive scheduler
4. `developers/proc/SMP.md` bring-up and the per-CPU rewrite

## The old guide

`HYBRID_GPU_OPTIMUS_GUIDE.md` came before this tree and is still the best thing on
hybrid graphics, GOP framebuffer ownership, muxless laptops and the display side of
bare-metal bring-up.
