# Kurono OS Documentation

Kurono OS now has a docs tree arranged by audience.

## Documentation Scope

This repository currently contains two useful size numbers.

1. The full repository text and source footprint, excluding generated build trees and archived copies, is **142,242 lines**.
2. The live source tree under `src/` is **136,984 lines**.

Those counts were taken from the current workspace state and are meant to give a realistic sense of project size rather than a marketing number.

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

## Existing Specialized Guides

The original deep technical guide remains useful and should stay in circulation.

1. `HYBRID_GPU_OPTIMUS_GUIDE.md`

That guide is still the best place for hybrid graphics, GOP framebuffer ownership, muxless laptop realities, and the display side of bare metal bring-up.

## What This Documentation Set Covers

### Beginners

The beginner set explains what Kurono OS is, how to build it, what to test first, and how to navigate the source tree without getting lost.

### Developers

The developer set explains boot flow, runtime architecture, shell and UI routing, subsystem responsibilities, and the practical file layout of the repository.

## Documentation Style Notes

This set follows the same basic spirit as the hybrid GPU guide.

1. Sections are explicit.
2. The writing stays technical.
3. The explanations focus on what the code does now.
4. The text is meant to be read while the source is open next to it.
