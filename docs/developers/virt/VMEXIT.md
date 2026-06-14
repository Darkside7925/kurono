# VM Exit Handling

`src/virt/vmexit.cpp` and `vmexit.h` implement the dispatch logic for all VM exits.

## 1. What VM exits are

Every time the guest CPU encounters an instruction or event that the hypervisor needs to handle, the hardware performs a VM exit: it saves guest state to the VMCS and transfers control to the hypervisor's host instruction pointer. `vmexit.cpp` is the first code that runs in that situation.

## 2. Exit reason dispatch

The exit reason is read from the VMCS `VM_EXIT_REASON` field. The dispatch table maps exit reasons to handlers:

| Exit reason code | Name | Handler summary |
| --- | --- | --- |
| 10 | CPUID | Return emulated CPU info |
| 12 | HLT | Guest halted; yield to host |
| 18 | VMCALL | Hypercall processing |
| 28 | Control register access | Emulate CR0/CR3/CR4 writes |
| 30 | I/O instruction | Forward to virtual devices |
| 31 | RDMSR | Return MSR value |
| 32 | WRMSR | Absorb or emulate MSR write |
| 48 | EPT violation | Map missing guest page |

## 3. I/O forwarding

I/O port accesses from the guest are forwarded to `vdevices.cpp` which checks a port-to-device mapping table. Each virtual device registers its I/O ports at init time and provides read/write handlers.

## 4. Hypercalls

Guest software can request hypervisor services via `VMCALL` (the handler reads the
hypercall number from guest `eax`). The recognized set includes: NOP/info (returns
`"kuro"`), shutdown, reboot, an audio PCM passthrough channel (`0x11`), network
status + packet passthrough (`0x12` / `0x14`), the 9p shared-filesystem channel
(`0x20`, host KVFS ↔ guest), and the **KSA read-only authorization-verdict channel
(`0x4B`)**  -  `KSA_SUB_GET_VERDICT` returns a *copy* of the latched verdict
(completed / approved / has-hash) and never a pointer; there is no path to write an
approval back into KSA memory from the main OS (see `security/KSA.md`).

## 5. Related files

- `src/virt/hypervisor.cpp`  -  sets the host RIP to the exit handler
- `src/virt/ept.cpp`  -  called for EPT_VIOLATION exits
- `src/virt/vdevices.cpp`  -  I/O port handler registry
- `src/virt/v9fs.cpp`  -  file sharing hypercall handler
