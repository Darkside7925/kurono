# Physical Memory Manager

`src/kernel/pmm.cpp` and `pmm.h` implement the physical memory allocator that underpins everything in Kurono that needs RAM.

## 1. What it does

The PMM tracks which physical page frames are available and hands them out on demand. It is the lowest layer of the memory system. Nothing in the heap, VMM, or any allocating driver works without a functioning PMM.

## 2. Initialization

The PMM is initialized in `kurono_kernel.cpp` immediately after HAL bring-up. It reads the Multiboot memory map to identify usable RAM regions and constructs a free-page bitmap or list over them.

The kernel image itself, the Multiboot info region, and any reserved areas described by the bootloader are excluded from usable memory before the allocator opens for business.

## 3. Interface

The PMM exposes allocation and freeing of physical pages. Callers receive a physical address and are responsible for mapping that page into a virtual address space before accessing it.

The PMM does not zero pages before returning them. If a caller needs zeroed memory it must do so itself or use the heap allocator, which adds that layer.

## 4. Design constraints

- No virtual memory dependency. The PMM must work with only physical addresses because VMM depends on it.
- No heap dependency. The PMM's own data structures are placed in a region of physical memory reserved at initialization time.
- Must be initialized before any device driver that does DMA or any kernel structure that needs dynamic memory.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| Heap allocation fails at boot | PMM did not reserve enough pages or Multiboot map was wrong |
| Crash when a driver init runs | Driver called heap/PMM before kernel memory is ready |
| Physical address conflicts | PMM region exclusion logic missed a reserved area |

## 6. Related files

- `src/kernel/heap.cpp`  -  heap built on top of PMM
- `src/kernel/vmm.cpp`  -  virtual address mapping that uses PMM pages
- `src/kernel/kurono_kernel.cpp`  -  initialization order
