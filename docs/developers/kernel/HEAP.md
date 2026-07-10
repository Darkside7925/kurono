# Heap Allocator

`src/kernel/heap.cpp` and `heap.h` implement the kernel heap that provides `malloc`/`free` style dynamic allocation throughout Kurono.

## 1. Role

The heap is the memory layer that all kernel C++ code uses when it calls `new`, `delete`, or any routine that needs variable-sized allocation at runtime. It sits between the PMM (which gives out raw pages) and all higher-level code.

## 2. Initialization

The heap is initialized after PMM in `kurono_kernel.cpp`. It requests an initial region of physical pages from the PMM and manages them internally with a free-list or bookkeeping structure.

If the heap runs out of its initial region it can request additional pages from the PMM. The PMM must still be functional at that point.

## 3. Usage in the codebase

- Driver data structures that are not known at compile time.
- Window manager window allocations.
- File manager and terminal buffers.
- Any STL-like container used by the OS code.

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| Triple fault after heap init | PMM not initialized before heap |
| `new` returns null | Heap exhausted; increase initial region |
| Corruption after large alloc | Heap bookkeeping overwritten by caller |

## 5. Related files

- `src/kernel/pmm.cpp` - raw page source for the heap
- `src/kernel/memory_mgr.h` - higher level memory manager declarations
