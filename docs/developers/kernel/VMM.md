# VMM (kernel side)

`src/kernel/vmm.cpp` and `vmm.h`. Page tables for the kernel. Maps PMM pages into
virtual ranges, handles identity maps, kernel space protection, and the
virt-to-phys translations drivers and security code ask for.

Not `src/virt/vmm.cpp`. That one is the hypervisor's guest memory backend. Different
file, easy to open the wrong one.

## With the PMM

The VMM asks the PMM for pages to back new page table entries. It doesn't own page
lifetimes, the PMM does. The VMM owns the mapping, nothing else.

## Init

During core bring-up, after the PMM is up. `kurono_boot.asm` throws together a
minimal identity map just to reach `kernel_main`, then `vmm.cpp` takes over and
builds the real kernel page tables.

## When it breaks

| Symptom | Cause |
| --- | --- |
| page fault in kernel code | missing or wrong mapping |
| DMA address collision | driver used a virtual address where a physical one was needed |
| corruption in high memory | kernel image mapping overlapping a driver region |

## Files

- `src/kernel/pmm.cpp` the physical pages behind the mappings
- `src/kernel/kurono_kernel.cpp` init order
- `src/virt/vmm.cpp` the hypervisor VMM, don't mix them up
