# Hypervisor

`src/virt/hypervisor.cpp` and `hypervisor.h`, and the background VM host in `src/virt/vm_host.cpp`
and `vm_host.h`. The hypervisor runs guests on Intel VT-x (VMX). The background VM host is the part
you use day to day: it runs an Alpine or Debian guest, or a Kurono kernel, next to the desktop.

## What it needs

- Intel VT-x (`CPUDetect::HasVMX()`). The tests for part 3 of the major update ran Kurono inside QEMU under KVM, with VMX
  passed down to Kurono.
- EPT for guest memory (see [EPT.md](EPT.md)).
- Enough host RAM for the guest. Guest RAM is lazy by default, so the guest only takes host
  frames as it touches them (see [DYNAMIC_VM.md](DYNAMIC_VM.md)).
- VT-d for `kurono.vm.pt` device passthrough (see [PCI_PASSTHROUGH.md](PCI_PASSTHROUGH.md)).

The AMD-V (SVM) exit path is in `vmexit.cpp`, but the tests for part 3 of the major update only ran on VT-x.

## The background VM host

`vm_host.cpp` runs the guest from its own kernel process. The process owns the VMCS, so every
VMX operation happens on it. It runs the guest in short real-time slices, yields to the desktop
between them, and sleeps while the guest is idle in HLT. Guest time keeps going whether or not
anything is looking at the screen. A guest that crashes or powers off is torn down here, and
Kurono keeps running.

Three distros are in the table in `vm_host.cpp`:

| Distro | Files on the KURONO partition root | Default RAM |
| --- | --- | --- |
| Alpine | `ALPKERN.BIN`, `ALPINIT.BIN`, `ALPINE.IMG` | 1024 MB |
| Debian 13 | `DEBKERN.BIN`, `DEBINIT.BIN`, `DEBIAN.IMG` | 768 MB in the table |
| Kurono (nested) | any multiboot2 Kurono kernel, started by `vm kurono` only | 1024 MB |

`DEBIAN.IMG` has to be one contiguous file on the partition. The VM host reports a fragmented
image as not installed.

`linux.autostart` starts the VM a few seconds after the desktop is up (`AutostartAtBoot()`).

## Commands

The `vm` command is registered in `src/shell/linux_cmds.cpp`, category `virt`. The ones that
matter day to day:

```
vm start                 start the background guest (alpine by default)
vm stop                  clean poweroff through the guest agent, forced after a timeout
vm kill                  immediate teardown
vm state                 live status
vm log                   the guest console
vm term                  open a Linux terminal on the guest
vm exec <cmd>            run a guest command and show its output
vm run <cmd>             start a guest program (GUI apps open as Kurono windows)
vm snap <png>            save a Kurono guest's screen as a png
vm kurono <elf> [MB] [cmdline]   boot a Kurono kernel as a guest
vm kloop [clean] [secs] [MB] [cmdline]   build a kernel in the guest and boot it (see KLOOP.md)
```

There are also lower-level commands used by the test rig: `vm create`, `vm run`, `vm pause`,
`vm resume`, `vm destroy`, `vm serial`, `vm regs`, `vm info`, `vm boot-alpine`, `vm boot-debian`
and `vm boot-test`. `vm` is its own top-level command. There is no `kurono vm start`.

## Kurono as a guest

`vm kurono` boots a multiboot2 Kurono kernel (ELF64) as a guest, for testing a new build
without rebooting the machine. The source is a path in the VFS, or an 8.3 name in the KURONO
partition root (`KGUEST.ELF`). The guest has one vCPU, its RAM defaults to 1024 MB and cannot go
below 256 MB, and it has no guest agent, so `vm stop` forces it off.

The guest's com1 is its whole log. Every line lands in the host kernel's log as `[gs]` lines,
so they show up in `KURONO.LOG` too. `vm log` and `vm snap` work the same as for Alpine.

## Init is deferred

Hypervisor init does not run during normal boot. It runs when a VM is started, so boot does not
depend on VT-x working on every machine.

## VMCS

The VMCS is the structure VMX uses to describe guest and host machine state. The hypervisor
sets up:

- guest register set: RIP, RSP, CR0, CR3, CR4, segment descriptors
- host save state, the kernel registers to restore on exit
- execution controls: pin-based, processor-based, exit and entry
- the EPT pointer for guest memory

## Exits

Every exit goes to `vmexit.cpp`. See [VMEXIT.md](VMEXIT.md) for the table.

## Files

- `src/virt/vm_host.cpp` the background VM host, distro table, boot paths and the `vm` status
- `src/virt/hypervisor.cpp` VMCS setup, the run loop and the guest boot paths
- `src/virt/vmexit.cpp` exit dispatch
- `src/virt/ept.cpp` extended page tables
- `src/virt/guest_mem.cpp` guest RAM layout and lazy backing
- `src/virt/vdisk.cpp` and `src/virt/vblk.cpp` guest disks
- `src/virt/linux_boot.cpp` Linux boot protocol
- `src/virt/mb2_boot.cpp` multiboot2 guest loader
- `src/virt/kloop.cpp` the `vm kloop` dev loop
- `src/virt/vdevices.cpp` what the guest sees as hardware
