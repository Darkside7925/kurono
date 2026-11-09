# Beginners

So you want to understand this thing, or just try it, without drowning in hardware
details on day one. Good. Here is the order I'd go in.

## Read in this order

1. `what-is-kurono.md` what it is, what works, what doesn't
2. `try-in-a-vm.md` run it in QEMU (zero risk to your machine)
3. `boot-on-hardware.md` only after the VM works, and only if you want to
4. `faq.md` quick answers
5. `FIRST_STEPS.md` and `PROJECT_TOUR.md` if you want to read the source
6. `BUILD_AND_RUN.md` building it yourself

## Short version

Kurono is a bare-metal x86_64 OS with its own desktop, its own apps, a shell, a lot of
drivers, a Linux-compatible syscall layer (it runs unmodified Firefox and BusyBox), and a
small hypervisor that runs Alpine Linux. It boots on a real laptop, not just in an
emulator.

It sounds huge. It gets a lot less scary once you cut it into routes:

1. how it boots
2. how the desktop shows up
3. how the shell is wired
4. how Linux programs run on top
5. how apps are built (the kui toolkit)

## Four files that explain most of it

- `src/kernel/kurono_kernel.cpp` for the boot story
- `src/shell/shell.cpp` for the command story
- `src/ui/desktop.cpp` and `src/ui/window_manager.cpp` for the UI story

Get those and the rest of the tree stops looking random.
