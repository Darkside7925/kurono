# Virtual Memory Manager

`src/kernel/vmm.cpp` and `vmm.h` handle virtual address space mappings at the kernel level.

## 1. What it does

The VMM provides page table management for the kernel. It maps physical pages (obtained from the PMM) into virtual address ranges and handles identity mappings, kernel space protection, and any explicit virtual-to-physical translations needed by drivers or security features.

Note that `src/virt/vmm.cpp` is a separate file - that one belongs to the hypervisor subsystem and handles VM guest memory backends. The kernel VMM here is `src/kernel/vmm.cpp`.

## 2. Relationship with PMM

The VMM calls the PMM to get physical pages when it needs to back a new page table entry. The VMM does not manage physical page lifetimes itself - the PMM owns that. The VMM only manages the mapping relationship.

## 3. Initialization

The VMM is initialized during the core kernel bring-up phase after the PMM is ready. The early boot code in `kurono_boot.asm` establishes a minimal identity-mapped configuration to reach `kernel_main`. After that, the VMM in `vmm.cpp` takes over and sets up the final kernel page tables.

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| Page fault in kernel code | Missing or incorrect mapping |
| DMA address collision | Driver used virtual address instead of physical |
| Corruption in high memory | Kernel image mapping overlapping a driver region |

## 5. Related files

- `src/kernel/pmm.cpp` - physical pages backing the mappings
- `src/kernel/kurono_kernel.cpp` - initialization order
- `src/virt/vmm.cpp` - separate hypervisor VMM backend (do not confuse)
