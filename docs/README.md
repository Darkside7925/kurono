# Kurono OS Documentation

Kurono OS now has a docs tree arranged by audience.

## Documentation Scope

The live source tree under `src/` is approximately 130,000 lines of hand-written freestanding C++17, NASM assembly, and C. (The raw `wc -l` over `src/` is much larger - around 337,000 lines - but roughly 207,000 of those are the two pre-decoded raw-RGBA wallpaper headers, `src/ui/wallpaper.h` and `src/ui/wallpaper2.h`, which are embedded pixel data, not code; the vendored `src/third_party/` stb glue adds another ~13,000.) The full repository text and documentation footprint is larger.

## Reading Order

### If the reader is new to the project

1. See `beginners/README.md`
2. Read `beginners/FIRST_STEPS.md`
3. Read `beginners/PROJECT_TOUR.md`
4. Read `beginners/BUILD_AND_RUN.md`

### If the reader is working on the codebase

1. See `developers/README.md`
2. Read `developers/how-it-works/BOOT_TO_DESKTOP.md`
3. Read `developers/how-it-works/SUBSYSTEMS.md`
4. Read `developers/routing/COMMAND_ROUTING.md`
5. Read `developers/routing/UI_INPUT_ROUTING.md`
6. Use `developers/reference/FILE_MAP.md` as the working index

### If the reader is working on display or graphics

1. Read `developers/drivers/GPU_PROBE.md` - GPU detection and hybrid topology
2. Read `developers/drivers/DISPLAY_MGR.md` - multi-backend display manager
3. Read `developers/drivers/GRAPHICS.md` - framebuffer drawing primitives
4. Read `developers/drivers/BGA.md` - Bochs Graphics Adapter
5. Read `developers/drivers/VIRTIO_GPU.md` - VirtIO GPU driver
6. Read `developers/ui/WAYLAND_SERVER.md` - in-kernel Wayland compositor
7. Read `developers/ui/WINDOW_MANAGER.md` - compositing window manager

### If the reader is working on storage, persistence, or paths

1. Read `developers/fs/KVFS.md` - the in-memory runtime filesystem
2. Read `developers/fs/KFS.md` - the on-disk persistence filesystem (extents, no limits)
3. Read `developers/system/LOGGING.md` - the canonical `/kurono` path layout + logging
4. Read `developers/drivers/NVME.md` - the block device under KFS

### If the reader is working on the Linux runtime

1. Read `developers/linux/LINUX_SYSCALL.md` - the in-kernel Linux syscall runtime
2. Read `developers/linux/LD_KURONO.md` - the in-kernel ELF64 dynamic linker
3. Read `developers/ui/WAYLAND_SERVER.md` - the compositor real clients render through
4. Read `developers/net/NETWORK.md` - TCP/IP + AF_UNIX sockets

### If the reader is working on security or multi-core

1. Read `developers/security/SUPR.md` - the privilege engine + roles
2. Read `developers/security/KSA.md` - hypervisor-backed authorization prompts
3. Read `developers/proc/SCHEDULER.md` - the cooperative + preemptive scheduler
4. Read `developers/proc/SMP.md` - multi-core bring-up + the per-CPU rewrite

## Existing Specialized Guides

The original deep technical guide remains useful and should stay in circulation.

1. `HYBRID_GPU_OPTIMUS_GUIDE.md`

That guide is still the best place for hybrid graphics, GOP framebuffer ownership, muxless laptop realities, and the display side of bare metal bring-up.

## What This Documentation Set Covers

### Beginners

The beginner set explains what Kurono OS is, how to build it, what to test first, and how to navigate the source tree without getting lost.

### Developers

The developer set explains boot flow, runtime architecture, shell and UI routing, subsystem responsibilities, the display and graphics stack, and the practical file layout of the repository.

## Documentation Style Notes

This set follows the same basic spirit as the hybrid GPU guide.

1. Sections are explicit.
2. The writing stays technical.
3. The explanations focus on what the code does now.
4. The text is meant to be read while the source is open next to it.
