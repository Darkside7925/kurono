# Installer

`src/system/installer.cpp` / `installer.h` implement the disk-installer engine
(disk scan, partition inspection, deployment planning, FAT32/ext4 staging), and
`src/system/installer_gui.cpp` / `installer_gui.h` implement the **graphical
installer / first-setup wizard** drawn on top of it.

## 1. What it does

The installer provides a step-by-step flow to install (or first-time-configure)
Kurono. There are three ways in:

- **The "Kurono Setup" GRUB entry**  -  a *dedicated* boot menu entry (generated in
  `src/Makefile`) that boots `kurono.setup=1` and drops straight into the
  graphical wizard. `kurono_kernel.cpp` parses the `kurono.setup` token (and an
  optional `kurono.setup.screen=N` to open on a specific screen) and calls
  `InstallerGUI::Run()`. **The installer is never the default boot**  -  the normal
  **"Kurono OS (Multiboot2)"** entry boots straight to the desktop
  (`kurono.autologin=1`), so a first boot can't strand the user on a black
  installer screen before input is up.
- **The desktop "Install Kurono" shortcut**  -  launches the same `InstallerGUI`.
- **The shell `installer` command**  -  runs the text-mode disk scan / plan / install
  path of the engine.

```
installer
```

## 2. Wizard screens (`InstallerGUI::Screen`)

The graphical wizard walks these screens in order (the `SCR_*` enum in
`installer_gui.h`):

1. **Welcome**
2. **Language**
3. **Keyboard**
4. **Network**  -  the wired link is probed live (carrier + DHCP address shown via
   `src/net/network.h`); offers Wi-Fi config.
5. **Wi-Fi**  -  records SSID/password into `/etc/network`. **Honest caveat:** there
   is no radio driver in this build  -  only e1000 / virtio-net *wired* links work;
   the Wi-Fi screen is an honest config UI with nothing to drive.
6. **Disk**  -  enumerate NVMe disks, pick a target.
7. **Partition mode**  -  GPT/ESP layout planning.
8. **Filesystem**
9. **User**  -  administrator account.
10. **Hostname**  -  hostname + basic preferences.
11. **Guests**  -  optional Linux guests / packages: **Debian (minbase)**, **Alpine
    Linux**, **Python 3**. The Debian/Alpine selections reuse the
    `kpkg install debian` + boot-time system-update reboot flow.
12. **Summary → Confirm → Progress → Drivers → Success → Live-exit.**

Provisioning (user, hostname, prefs, `/etc/network`, the guest queue) is written
through KVFS, so the wizard works even when there is no ext4 target  -  it can run
as a first-setup pass on the live system, not only as a disk install.

## 3. Dual boot awareness

The disk installer detects existing OS installations on the target and can add
GRUB entries for them. It also offers to install the Debian rootfs for the
integrated Linux boot feature (the `kpkg install debian` + system-update path).

## 4. Shell integration

The `installer` shell command is registered in `src/system/installer.cpp`
(`RegisterCommand("installer", cmd_installer, ...)`, *not* in `shell.cpp`'s
`RegisterBuiltins()`) and runs the engine's terminal-driven scan / plan / install
mode. The graphical wizard runs the same underlying engine through
`InstallerGUI`.

## 5. Related files

- `src/system/installer_gui.cpp` / `.h`  -  the graphical wizard (`SCR_*` screens, `Run()`)
- `src/system/installer.cpp` / `.h`  -  disk-installer engine + `installer` command
- `src/kernel/kurono_kernel.cpp`  -  `kurono.setup` / `kurono.setup.screen` token gate
- `src/Makefile`  -  `grub.cfg` generation incl. the "Kurono Setup" entry
- `src/linux/dual_boot.cpp`  -  dual boot configuration written during install
- `src/drivers/nvme.cpp`  -  disk access for installation
- `src/fs/fat32.cpp`  -  FAT32 ESP creation
