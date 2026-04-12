# Installer

`src/system/installer.cpp` and `installer.h` implement the Kurono OS installer.

## 1. What it does

The installer provides a step-by-step flow to install Kurono to a real disk. It is accessible from the desktop via the Install icon or from the shell with:

```
installer
```

## 2. Installation steps

1. **Disk selection**  -  enumerate NVMe/SATA disks, let the user pick a target.
2. **Partition scheme**  -  create GPT with an ESP (FAT32, 512 MB), a root partition (Kurono), and optionally a Linux partition.
3. **File copy**  -  extract the kernel image, GRUB, EFI loader, and base rootfs to the target.
4. **GRUB install**  -  write GRUB to the ESP, configure `grub.cfg` with boot entries for Kurono and (optionally) the embedded Debian guest.
5. **Config write**  -  write initial `/etc/kurono/ui.conf` to the installed root.
6. **Done**  -  report success and offer to reboot.

## 3. Dual boot awareness

The installer detects existing OS installations on the target disk and adds GRUB entries for them. It also offers to install the Debian rootfs for the integrated Linux boot feature.

## 4. Shell integration

The `installer` shell command registered in `RegisterBuiltins()` runs the installer in a terminal-driven text mode. The graphical mode runs the same logic through the installer app window.

## 5. Related files

- `src/linux/dual_boot.cpp`  -  dual boot configuration written during install
- `src/drivers/nvme.cpp`  -  disk access for installation
- `src/fs/fat32.cpp`  -  FAT32 ESP creation
