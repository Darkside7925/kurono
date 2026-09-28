# PMM

`src/kernel/pmm.cpp` and `pmm.h`. Physical frame allocator. Tracks which page
frames are free and hands them out. Everything above it needs it: the heap, the
VMM, any DMA driver.

## Init

Right after HAL bring-up in `kurono_kernel.cpp`. Reads the Multiboot memory map,
builds a free-page bitmap (or list) over the usable RAM. The kernel image, the
Multiboot info region and anything else the bootloader marked reserved get excluded
first.

## Interface

Alloc and free physical pages. You get a physical address back, and you map it
yourself before you touch it.

It does not zero pages. Want zeroed memory? Do it yourself or go through the heap.

## Constraints

- no virtual memory dependency. Physical addresses only, because the VMM depends on
  it.
- no heap dependency. Its own structures live in physical memory reserved at init.
- has to be up before any DMA driver, or anything that allocates.

## When it breaks

| Symptom | Cause |
| --- | --- |
| heap alloc fails at boot | not enough reserved pages, or a bad Multiboot map |
| crash in driver init | driver asked for memory before kernel memory was ready |
| physical address conflicts | exclusion logic missed a reserved area |

## Files

- `src/kernel/heap.cpp` the heap on top of this
- `src/kernel/vmm.cpp` maps the pages this hands out
- `src/kernel/kurono_kernel.cpp` init order
