# Hypervisor

`src/virt/hypervisor.cpp` and `hypervisor.h`. The Type-1 style hypervisor, on Intel VT-x
(VMX).

A hardware-accelerated VMM built into the kernel. It uses VMX to create and run guests, and
the main use case is booting a Debian guest inside Kurono without a reboot.

## What it needs

- Intel VMX (`CPUDetect::HasVMX()`) or AMD-V/SVM. Both detection and enable paths exist,
  EPT on Intel and NPT on AMD.
- VMXON permission, CR4.VMXE set, for the VT-x path
- enough RAM for the guest's physical memory

> The honest caveat: the VMCS/VMCB and Linux-boot path are implemented, but actually booting
> an Alpine/Debian guest needs the host to expose **nested** VT-x to Kurono. In the usual dev
> environment, Kurono itself running as a guest under nested KVM/QEMU, that layer is not
> available and VM-entry fails with a VMX entry error (`hypervisor.cpp` logs "VM-entry
> failed ... host likely doesn't support nested virt"). So guest boot is only confirmed where
> nested VMX exists. `VMM::IsNested()` reports the situation at boot. Same constraint sits
> behind KSA's nested-VM-vs-EPT-isolated-context fallback, see
> [../security/KSA.md](../security/KSA.md).

## Init is deferred

Hypervisor init does not run during normal boot. It happens when you explicitly start a VM.
On purpose: VT-x init is risky on some real laptop hardware, and deferring it keeps boot
from failing.

Start one with the `vm` command, registered in `linux_cmds.cpp` with category `virt`:

```
vm create [ram_mb]      # create a VM
vm run [max_exits]      # enter the VM run loop
vm boot-alpine          # boot the embedded Alpine guest
vm boot-test            # minimal guest that prints to serial
```

There is no `kurono vm start`. `vm` is its own top-level command with `create`, `run`,
`pause`, `resume`, `destroy`, `serial`, `regs`, `info`, `boot-test` and `boot-alpine`.

## VMCS

The VMCS is the structure VMX uses to describe guest and host machine state. The hypervisor
sets up:

- guest register set: RIP, RSP, CR0, CR3, CR4, segment descriptors
- host save state, the kernel registers to restore on exit
- execution controls: pin-based, processor-based, exit and entry
- the EPT pointer for guest memory

## Exits

Every exit goes to `vmexit.cpp`, which dispatches on the reason:

| Reason | Handler |
| --- | --- |
| CPUID | return emulated values |
| I/O port access | forward to the virtual device |
| EPT violation | map the missing guest page |
| RDMSR/WRMSR | return or absorb the MSR value |
| HLT | guest idled, yield |

## Files

- `src/virt/vmexit.cpp` exit dispatch
- `src/virt/ept.cpp` extended page tables
- `src/virt/vmm.cpp` backend glue
- `src/virt/linux_boot.cpp` loads Linux into the guest
- `src/virt/vdevices.cpp` what the guest sees as hardware
