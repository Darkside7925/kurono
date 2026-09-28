# VM exits

`src/virt/vmexit.cpp` and `vmexit.h`. Dispatch for every VM exit.

A VM exit is what happens when the guest does something the hypervisor has to handle:
hardware saves guest state into the VMCS and transfers control to the host instruction
pointer. This file is the first code that runs at that point.

The reason comes out of the VMCS `VM_EXIT_REASON` field, and a table maps it to a handler:

| Reason | Name | Handler |
| --- | --- | --- |
| 10 | CPUID | return emulated CPU info |
| 12 | HLT | guest halted, yield to host |
| 18 | VMCALL | hypercall processing |
| 28 | control register access | emulate CR0/CR3/CR4 writes |
| 30 | I/O instruction | forward to virtual devices |
| 31 | RDMSR | return MSR value |
| 32 | WRMSR | absorb or emulate the write |
| 48 | EPT violation | map the missing guest page |

## I/O forwarding

Guest port accesses go to `vdevices.cpp`, which keeps a port-to-device table. Every virtual
device registers its ports at init and provides read/write handlers.

## Hypercalls

Guest software asks for hypervisor services with `VMCALL`. The handler reads the hypercall
number out of guest `eax`. What's recognised: NOP/info returning `"kuro"`, shutdown, reboot,
an audio PCM passthrough channel (`0x11`), network status and packet passthrough (`0x12` and
`0x14`), the 9p shared-filesystem channel (`0x20`, host KVFS to guest), and the KSA
read-only authorization-verdict channel (`0x4B`). That last one, `KSA_SUB_GET_VERDICT`,
returns a copy of the latched verdict (completed, approved, has-hash) and never a pointer.
There is no path to write an approval back into KSA memory from the main OS, see
`security/KSA.md`.

## Files

- `src/virt/hypervisor.cpp` sets the host RIP to this handler
- `src/virt/ept.cpp` for EPT_VIOLATION
- `src/virt/vdevices.cpp` the port handler registry
- `src/virt/v9fs.cpp` the file sharing hypercall
