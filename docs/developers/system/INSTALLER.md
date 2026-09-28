# Installer

`src/system/installer.cpp` / `installer.h` is the disk-installer engine: disk scan, partition
inspection, deployment planning, FAT32/ext4 staging. `src/system/installer_gui.cpp` /
`installer_gui.h` is the graphical installer and first-setup wizard drawn on top of it.

## Three ways in

- the "Kurono Setup" GRUB entry. A dedicated boot menu entry, generated in `src/Makefile`, that
  boots `kurono.setup=1` and goes straight into the wizard. `kurono_kernel.cpp` parses the token
  (plus an optional `kurono.setup.screen=N` to open on a specific screen) and calls
  `InstallerGUI::Run()`. The installer is never the default boot: the normal "Kurono OS
  (Multiboot2)" entry boots to the desktop with `kurono.autologin=1`, so a first boot can't
  strand you on a black installer screen before input is up.
- the desktop "Install Kurono" shortcut, which launches the same `InstallerGUI`
- the shell `installer` command, which runs the engine's text-mode scan, plan and install path

```
installer
```

## Wizard screens

The `SCR_*` enum in `installer_gui.h`, in order:

1. Welcome
2. Language
3. Keyboard
4. Network: the wired link gets probed live (carrier and DHCP address shown), and it offers
   Wi-Fi config
5. Wi-Fi: records SSID and password into `/etc/network`. Honest caveat: there is no radio
   driver in this build, only e1000 and virtio-net wired links work. The Wi-Fi screen is a
   config UI with nothing driving it yet.
6. Disk: enumerate NVMe disks, pick a target
7. Partition mode: GPT/ESP layout planning
8. Filesystem
9. User: administrator account
10. Hostname: hostname plus basic preferences
11. Guests: optional Linux guests and packages, Debian (minbase), Alpine Linux, Python 3. The
    Debian and Alpine picks reuse the `kpkg install debian` plus boot-time system-update reboot
    flow.
12. Summary, Confirm, Progress, Drivers, Success, Live-exit

Provisioning (user, hostname, prefs, `/etc/network`, the guest queue) is written through KVFS,
so the wizard works even with no ext4 target. It can run as a first-setup pass on the live
system, not only as a disk install.

## Dual boot

The disk installer detects existing OS installations on the target and can add GRUB entries for
them. It also offers to install the Debian rootfs for the integrated Linux boot feature, which is
the `kpkg install debian` plus system-update path.

## Shell command

`installer` is registered in `src/system/installer.cpp`, not in `shell.cpp`'s
`RegisterBuiltins()`, and runs the engine's terminal-driven scan, plan and install mode. The
graphical wizard runs the same engine through `InstallerGUI`.

## Files

- `src/system/installer_gui.cpp` / `.h` the wizard, `SCR_*` screens and `Run()`
- `src/system/installer.cpp` / `.h` the engine and the `installer` command
- `src/kernel/kurono_kernel.cpp` the `kurono.setup` and `kurono.setup.screen` gate
- `src/Makefile` `grub.cfg` generation, including the "Kurono Setup" entry
- `src/linux/dual_boot.cpp` dual boot config written during install
- `src/drivers/nvme.cpp` disk access for installation
- `src/fs/fat32.cpp` FAT32 ESP creation
