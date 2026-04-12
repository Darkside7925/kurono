# Hypervisor

`src/virt/hypervisor.cpp` and `hypervisor.h` implement the Kurono Type-1 style hypervisor using Intel VT-x (VMX).

## 1. What it is

The hypervisor is a hardware-accelerated virtual machine monitor built into the Kurono kernel. It uses Intel VT-x VMX instructions to create and run guest virtual machines. The primary use case is booting a Debian Linux guest inside Kurono without a reboot.

## 2. Prerequisites

The hypervisor requires:

- Intel VMX support (checked with `CPUDetect::HasVMX()`)
- VMXON permission (CR4.VMXE set)
- Sufficient RAM for guest physical memory regions

AMD SVM is a planned future addition. The current implementation is VT-x only.

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
