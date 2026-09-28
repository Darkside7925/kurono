# types.h

`src/kernel/types.h` and `types.cpp`. The foundational aliases the whole tree agrees
on.

What's in it:

- fixed-width ints: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t` and friends
- `size_t`, `ptrdiff_t`, `uintptr_t` for pointer arithmetic
- `bool`, if the compiler isn't handing us one
- anything else the OS uses everywhere

## Why it exists

No libc, so `<stdint.h>` and `<stddef.h>` style definitions have to come from
somewhere in the tree. This is that somewhere. Include it anywhere you touch numeric
types, pointer widths or booleans. Most files get it transitively anyway.

## Rules

- new project-wide aliases go here
- no logic, no functions, no heavy macros. It's a type-definition file.
- don't include OS headers from here. It has to stay includable from everything,
  including assembly glue headers.

## Files

- `src/kernel/kurono_kernel.cpp` gets types transitively through nearly every header
- `src/kernel/io.h` port helpers using these types
