# Core Types

`src/kernel/types.h` and `types.cpp` define the foundational type aliases and size-safe primitives used across the entire codebase.

## 1. What is here

The types header provides the single canonical location for:

- Fixed-width integer aliases (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t`, etc.)
- `size_t`, `ptrdiff_t`, and `uintptr_t` for pointer arithmetic
- `bool` if not available through the compiler
- Any other primitive that the OS uses everywhere without pulling in a large standard library header

## 2. Why this exists

The kernel does not link against a standard C library. That means `<stdint.h>` and `<stddef.h>` style definitions need to come from somewhere inside the project. `types.h` is that somewhere.

Every file that uses numeric types, pointer widths, or boolean values should include `types.h`. Most files get it transitively through other headers that already include it.

## 3. Rules for types.h

- Add new type aliases here when they need to be available project-wide.
- Do not add logic, functions, or heavy macros here. It is a type-definition file only.
- Do not include OS-specific headers from here. It must be includable from everything including assembly glue headers.

## 4. Related files

- `src/kernel/kurono_kernel.cpp` - includes types transitively through almost every header
- `src/kernel/io.h` - I/O port helpers that use the types defined here
