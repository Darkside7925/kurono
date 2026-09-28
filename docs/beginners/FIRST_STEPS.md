# First steps

Shortest path to not being lost.

## The repo is not small

`src/` is about 130,000 lines of freestanding C++17, NASM and C. Raw `wc -l` says
~337,000, but ~207,000 of that is wallpaper pixel data in `src/ui/`. Not code. So
don't read it top to bottom. Read the control files, then follow one route at a time.

## Read these seven first

1. `README.md`
2. `docs/README.md`
3. `src/kernel/kurono_kernel.cpp`
4. `src/shell/shell.cpp`
5. `src/ui/desktop.cpp`
6. `src/ui/window_manager.cpp`
7. `src/kernel/panic.cpp`

## Five routes

Boot. Firmware or bootloader state goes in, core services come up, the display gets
accepted, input attaches, main loop starts.

Desktop. Desktop shell, taskbar, window manager and apps render into the framebuffer.

Command. The shell parses a line, works out which environment owns it, forwards it to
the subsystem or guest backend that implements it.

Storage. Runtime filesystem is built in memory. The installer can also read real disks
and deploy boot payloads and layouts onto partitions.

Virtualization. A shell command builds a VM, loads an embedded guest kernel, enters
the hypervisor loop.

## Don't, on day one

- don't read every driver file
- don't start in the archived folders
- don't assume a Unix or Windows process model everywhere. A lot of this is
  lighter-weight and poll-based.
- don't assume a graphics bug is in the app layer. Most display problems start in
  boot, framebuffer ownership or cache policy.

## Do, on day one

1. build it once
2. read the developer boot doc once
3. keep the file map open
4. pick one route and stay in it until it makes sense

## Good things to chase first

- how the shell picks between Linux, Windows and Kurono commands
- how the desktop search box launches apps
- how the panic screen gets a framebuffer when the desktop is already dead
- how the GPU probe detects hybrid graphics and validates the framebuffer address
- how the Wayland compositor speaks the wire protocol and bridges to the WM
- how the installer finds disks and partitions
- how `vm boot-alpine` goes from a shell command into the hypervisor
