# Dual boot

`src/linux/dual_boot.cpp` and `dual_boot.h`. Kurono's integrated Linux boot coordination.

From the shell or a start menu entry you can switch to a real or embedded Debian install
without a full hardware reboot.

## How the switch goes

1. load a Debian root filesystem image, in KVFS or on a partition
2. set up a minimal boot environment using the hypervisor's guest Linux loader
3. the Debian kernel is `vmlinuz-6.8.0-kurono`, and GRUB labels the entry "Kurono Linux 6.8"
4. the guest's apt sources point at `deb.debian.org/debian bookworm main`
5. the guest boots inside the Kurono hypervisor and shows up in the Conduit app

## Debian version

Configured for Debian 12 (Bookworm). `dual_boot.cpp` writes `/linux/etc/debian_version` as
`12.0`:

- `debian_version = 12.0`
- apt sources: `deb http://deb.debian.org/debian bookworm main contrib non-free`, plus
  `bookworm-security` and `bookworm-updates`
- kernel: `vmlinuz-6.8.0-kurono`
- initrd: `initrd.img-6.8.0-kurono`

## Who actually boots it

`src/virt/linux_boot.cpp` does the boot. This module prepares the metadata and the command
line, the hypervisor does the loading.

## Files

- `src/virt/linux_boot.cpp` the real guest kernel loader
- `src/virt/hypervisor.cpp` VM infrastructure
- `src/linux/kls.cpp` the Linux personality for the native shell layer
- `src/system/conduit.cpp` the telemetry bridge and app surface
