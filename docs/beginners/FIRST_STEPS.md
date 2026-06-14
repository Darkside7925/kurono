# First Steps

This is the shortest practical introduction to the repository.

## 1. Know the project size

The live repository is not small.

The source tree under `src/` is approximately 130,000 lines of hand-written freestanding C++17, NASM assembly, and C. (A raw `wc -l` reports far more  -  around 337,000  -  but ~207,000 of that is the two pre-decoded raw-RGBA wallpaper headers in `src/ui/`, which are embedded image data, not code.)

That means the right strategy is not reading everything in random order. The right strategy is reading the control files first.

## 2. Read these files first

1. `README.md`
2. `docs/README.md`
3. `src/kernel/kurono_kernel.cpp`
4. `src/shell/shell.cpp`
5. `src/ui/desktop.cpp`
6. `src/ui/window_manager.cpp`
7. `src/kernel/panic.cpp`

## 3. Understand the project as five routes

### Route 1

Boot route.

Firmware or bootloader state enters the kernel, core services come online, display is accepted, input is attached, and the main loop starts.

### Route 2

Desktop route.

The desktop shell, taskbar, window manager, and applications render into the framebuffer.

### Route 3

Command route.

The shell parses a line, decides which environment owns it, and forwards execution to the correct subsystem or guest backend.

### Route 4

Storage route.

The runtime filesystem is built in memory, while the installer can inspect real disks and deploy boot payloads and layouts to supported partitions.

### Route 5

Virtualization route.

A shell command can build a virtual machine, load an embedded guest kernel, and enter the hypervisor execution loop.

## 4. What not to do on day one

1. Do not start by reading every driver file.
2. Do not start in archived folders.
3. Do not assume a familiar Unix or Windows process model everywhere; some systems are lighter weight or polling based.
4. Do not assume a graphics bug is inside the app layer; many display issues begin in boot, framebuffer ownership, or cache policy.

## 5. What to do on day one

1. Build the project once.
2. Read the developer boot document once.
3. Keep the file map open.
4. Pick one route and stay inside it until it makes sense.

## 6. Good first investigation topics

1. How the shell chooses between Linux, Windows, and Kurono commands.
2. How the desktop search box launches apps.
3. How the panic screen gets a framebuffer even when the desktop is dead.
4. How the GPU probe detects hybrid graphics and validates the framebuffer address.
5. How the Wayland compositor speaks the wire protocol and bridges to the window manager.
6. How the installer discovers disks and partitions.
7. How `vm boot-alpine` crosses from shell command into the hypervisor.
