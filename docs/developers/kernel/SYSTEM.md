# system.cpp

`src/kernel/system.cpp` and `system.h`. The low-level utility stuff that doesn't
belong in the HAL, the PMM or the heap.

In here:

- `memcpy`, `memset`, `memmove`. The kernel needs these before any libc exists.
- string helpers: `strlen`, `strcpy`, `strcmp`, `snprintf`-style formatting
- whatever else unrelated subsystems need

## Why not libc

We don't link a C library. Anything libc would normally give us gets written here (or
in a similar utility file), and the implementations are minimal on purpose. Just
enough to be correct on the target.

## Rules

- don't call the heap or KVFS from here. These have to work before those exist.
- don't duplicate what's already in `types.h` or `io.h`.

## Files

- `src/kernel/types.h` the primitives these helpers use
- `src/kernel/heap.cpp` heap init calls `memset`/`memcpy` from here
