# System Helpers

`src/kernel/system.cpp` and `system.h` provide kernel-level utility functions that are too fundamental for a single module but do not belong in the HAL, PMM, or heap.

## 1. What is here

Typical contents include:

- Low-level memory copy and fill (`memcpy`, `memset`, `memmove`) that the kernel uses before libc is available.
- String manipulation helpers (`strlen`, `strcpy`, `strcmp`, `snprintf`-style formatting) that avoid pulling in the full C standard library.
- Any other utility that multiple unrelated subsystems need.

## 2. Why not use libc

The kernel does not link a standard C library. Any function the OS needs that would normally come from libc must either be written here or in a similar utility module. The implementations in `system.cpp` are intentionally minimal - just enough to work correctly on the known target architecture.

## 3. Rules

- Do not call OS services like the heap or KVFS from inside `system.cpp`. These helpers must work before those subsystems exist.
- Do not duplicate functions already in `types.h` or `io.h`.

## 4. Related files

- `src/kernel/types.h` - primitive types used by these helpers
- `src/kernel/heap.cpp` - heap allocator that relies on `memset`/`memcpy` from here at initialization time
