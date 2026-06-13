# Hypervisor

`src/virt/hypervisor.cpp` and `hypervisor.h` implement the Kurono Type-1 style hypervisor using Intel VT-x (VMX).

## 1. What it is

The hypervisor is a hardware-accelerated virtual machine monitor built into the Kurono kernel. It uses Intel VT-x VMX instructions to create and run guest virtual machines. The primary use case is booting a Debian Linux guest inside Kurono without a reboot.

## 2. Prerequisites

The hypervisor requires:

- Intel VMX (`CPUDetect::HasVMX()`) **or** AMD-V/SVM detection (both detection +
  enable paths exist; EPT on Intel / NPT on AMD)
- VMXON permission (CR4.VMXE set) for the VT-x path
- Sufficient RAM for guest physical memory regions

> **Honest caveat  -  nested VMX for guest boot.** The VMCS/VMCB + Linux-boot path
> is implemented, but actually *booting* an Alpine/Debian guest needs the host to
> expose **nested** VT-x to Kurono. In the common dev environment  -  Kurono itself
> running as a guest under nested KVM/QEMU  -  that nested layer is not available,
> and the guest **VM-entry fails with a VMX entry error** (`hypervisor.cpp` logs
> "VM-entry failed ... host likely doesn't support nested virt"). Guest boot is
> therefore confirmed only where nested VMX is present. `VMM::IsNested()` reports
> the situation at boot. This is the same constraint behind KSA's
> nested-VM-vs-EPT-isolated-context fallback (see
> [../security/KSA.md](../security/KSA.md) §3).

## 3. Initialization

Hypervisor initialization is deferred  -  it does not run during normal Kurono boot. It is activated only when the user explicitly starts a VM. This is a deliberate decision: VT-x initialization is risky on some real laptop hardware and deferring it prevents boot failures.

To start the hypervisor:
```
kurono vm start
```
Or from the Conduit app.

## 4. VMCS setup

The VM Control Structure (VMCS) is the data structure Intel VMX uses to define guest and host machine state. The hypervisor initializes VMCS fields for:

- Guest register set (RIP, RSP, CR0, CR3, CR4, segment descriptors)
- Host save state (kernel registers to restore on VM exit)
- VM execution controls (pin-based, processor-based, exit/entry controls)
- EPT (Extended Page Tables) pointer for guest memory mapping

## 5. VM exits

Every VM exit calls `vmexit.cpp` which dispatches based on the exit reason. Common exit reasons handled:

| Exit reason | Handler |
| --- | --- |
| CPUID | Return emulated values |
| I/O port access | Forward to virtual device |
| EPT violation | Map missing guest page |
| RDMSR/WRMSR | Return/absorb MSR value |
| HLT | Guest idled; yield |

## 6. Related files

- `src/virt/vmexit.cpp`  -  VM exit dispatch
- `src/virt/ept.cpp`  -  Extended Page Tables
- `src/virt/vmm.cpp`  -  VMM backend glue
- `src/virt/linux_boot.cpp`  -  loads Linux into the guest
- `src/virt/vdevices.cpp`  -  virtual devices seen by the guest
