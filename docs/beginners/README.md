# Beginners

So you want to understand this thing without drowning in hardware details on day
one.

## Read in this order

1. `FIRST_STEPS.md`
2. `PROJECT_TOUR.md`
3. `BUILD_AND_RUN.md`

## What this is

A bare-metal x86_64 OS with a desktop, built-in apps, a mixed shell (Kurono, Linux,
Windows), a Linux-compatible syscall layer, a VM stack, an installer, and a lot of
drivers.

Sounds huge. It isn't, once you cut it into routes:

1. how it boots
2. how the desktop shows up
3. how the shell is wired
4. which folders matter first
5. how to build and test it without wrecking your machine

## Four files that explain most of it

- `src/kernel/kurono_kernel.cpp` for the boot story
- `src/shell/shell.cpp` for the command story
- `src/ui/desktop.cpp` and `src/ui/window_manager.cpp` for the UI story

Get those four and the rest of the tree stops looking random.
