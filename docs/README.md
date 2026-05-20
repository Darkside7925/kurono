# Kurono OS Documentation

Kurono OS now has a docs tree arranged by audience.

## Documentation Scope

The live source tree under `src/` is approximately 137,000 lines of freestanding C++17, NASM assembly, and C. The full repository text and documentation footprint is larger.

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

1. Read `developers/drivers/GPU_PROBE.md`  -  GPU detection and hybrid topology
2. Read `developers/drivers/DISPLAY_MGR.md`  -  multi-backend display manager
3. Read `developers/drivers/GRAPHICS.md`  -  framebuffer drawing primitives
4. Read `developers/drivers/BGA.md`  -  Bochs Graphics Adapter
5. Read `developers/drivers/VIRTIO_GPU.md`  -  VirtIO GPU driver
6. Read `developers/ui/WAYLAND_SERVER.md`  -  in-kernel Wayland compositor
7. Read `developers/ui/WINDOW_MANAGER.md`  -  compositing window manager

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
