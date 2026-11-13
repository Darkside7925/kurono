# Dual boot

`src/linux/dual_boot.cpp` and `dual_boot.h`. This is an older module: the dual-boot
manager. It's bookkeeping and shell commands, and it does not boot Debian. The Debian
guest that actually runs is the VM host, see the end of this page.

## What it does

It tracks a boot mode (`dual_boot.h`):

| Mode | Meaning |
| --- | --- |
| 0 integrated (default) | Kurono boots, Linux programs run through the Linux layer |
| 1 dual active | Kurono and a Linux install on their own partitions |
| 2 standalone Linux | plain Linux boot, Kurono not loaded |
| 3 standalone Kurono | Kurono with no Linux layer |

Only the integrated mode is what Kurono really runs. The others are just recorded.

It also looks for partitions, can mount a Linux ext4 partition, and generates a GRUB config
as text (`dualboot grub`). The kernel names in that config (`vmlinuz-6.8.0-kurono`,
"Kurono Linux 6.8") and the Debian 12 Bookworm apt sources that `dualboot install` writes
into `/linux/etc` are placeholders. There's no 6.8 kernel or Bookworm install behind them.

## Shell commands

- `dualboot` / `dualboot status`: the current mode
- `dualboot mode <n>`: set the mode (0 to 3)
- `dualboot grub`: print the generated GRUB config
- `dualboot install`: lay the placeholder `/linux` files into KVFS
- `lsblk`, `fdisk`, `mount-linux`: list partitions, show the table, mount an ext4 partition

## The Debian that really boots

Debian 13 runs as a guest in Kurono's hypervisor, from `DEBKERN.BIN`, `DEBINIT.BIN` and
`DEBIAN.IMG` on the KURONO partition (`DEBIAN.IMG` has to be one contiguous file). Pick
Debian in Settings > Linux, or use the `vm` commands. Packages install with apt, and guest
RAM and the disk grow as they're used. See `../virt/HYPERVISOR.md` and
`../virt/DYNAMIC_VM.md`.

## Files

- `src/linux/dual_boot.cpp` / `.h`: this module
- `src/virt/vm_host.cpp`: the VM host that boots the Alpine and Debian guests
- `src/virt/linux_boot.cpp`: the guest Linux kernel loader
