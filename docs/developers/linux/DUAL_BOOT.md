# Dual Boot

`src/linux/dual_boot.cpp` and `dual_boot.h` implement Kurono's integrated Linux boot coordination feature.

## 1. What it does

The dual boot module provides a path to boot a real or embedded Debian Linux installation alongside Kurono. From the Kurono shell or a start menu entry, the user can trigger a switch to Linux without a full hardware reboot.

## 2. How it works

1. The module loads a Debian root filesystem image (stored in KVFS or on a partition).
2. It sets up a minimal boot environment using the hypervisor's guest Linux loader.
3. The Debian vmlinuz kernel is `vmlinuz-6.8.0-kurono` (the GRUB entry is labelled "Kurono Linux 6.8").
4. The APT source list in the guest points at `deb.debian.org/debian bookworm main`.
5. The guest Linux boots inside the Kurono hypervisor and appears in the Conduit app.

## 3. Debian version

The module is configured for **Debian 12 (Bookworm)** (`/linux/etc/debian_version`
is written as `12.0` in `dual_boot.cpp`):

- `debian_version = 12.0`
- apt sources: `deb http://deb.debian.org/debian bookworm main contrib non-free`
  (plus `bookworm-security` and `bookworm-updates`)
- boot kernel: `vmlinuz-6.8.0-kurono`
- initrd: `initrd.img-6.8.0-kurono`

## 4. Integration with the virtual machine layer

The actual Linux boot execution is delegated to `src/virt/linux_boot.cpp`. The dual boot module prepares the metadata and command line; the hypervisor does the loading.

## 5. Related files

- `src/virt/linux_boot.cpp` - actual guest kernel loader
- `src/virt/hypervisor.cpp` - VM infrastructure
- `src/linux/kls.cpp` - Linux personality for the native shell layer
- `src/system/conduit.cpp` - the Conduit telemetry bridge / app surface
