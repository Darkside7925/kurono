# Kernel heap

`src/kernel/heap.cpp` and `heap.h`. This is malloc/free for kernel C++ code, so it
sits behind `new`/`delete` and every runtime-sized allocation. It's sandwiched
between the PMM (raw pages) and everything above.

## Init

After the PMM, in `kurono_kernel.cpp`. Asks the PMM for a first chunk of pages,
keeps them in a free list, asks for more when it runs dry. Which means the PMM has
to still be alive later on.

## Who uses it

Driver structs that aren't known at compile time. Window manager windows. File
manager and terminal buffers. Basically every container in the tree.

## If you find a corruption here

The freelist is intrusive: free blocks hold their own list links. So one stray write
into a freed block wrecks the walk. `heap.cpp` validates every link (`link_ok`,
SLAB_FREELIST_HARDENED style). If serial prints `Heap: freelist corrupt`, a real
use-after-free writer is loose. Hunt it. Don't silence it. The big caches (KFS
bitmap and inode, around 36MB) live in the PMM on purpose, not here.

## When it breaks

| Symptom | Cause |
| --- | --- |
| triple fault right after heap init | PMM wasn't up before the heap |
| `new` returns null | heap is exhausted, raise the first region |
| corruption after a big alloc | caller wrote past its own block |

## Files

- `src/kernel/pmm.cpp` where the pages come from
- `src/kernel/memory_mgr.h` higher level memory declarations
