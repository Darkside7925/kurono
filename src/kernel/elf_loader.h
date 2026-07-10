#pragma once
//  ELF64 loader for static x86_64 user binaries.
//  Parses PT_LOAD segments, allocates physical pages, copies file
//  data, and maps them into a fresh user address space.  The created
//  Process is enqueued and ready for Userspace::RunProcess().

#include "types.h"

struct Process;

class ElfLoader {
public:
    // Load a static ELF64 binary from a flat in-memory buffer and create
    // a runnable user Process.  Returns nullptr on any failure.
    //
    //   data  : pointer to the ELF file bytes
    //   size  : length of the ELF buffer
    //   name  : process name (truncated to Process::name capacity)
    static Process* LoadELF64(const uint8_t* data, uint64_t size, const char* name);

    // Convenience: read the file from KVFS, then call LoadELF64.
    static Process* LoadELF64FromVFS(const char* path, const char* name);
};
