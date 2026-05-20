#pragma once
//  Embedded user-mode test program(s).  Linked into the kernel image via
//  objcopy from src/userprogs/<name>.asm \u2192 elf64 \u2192 binary blob.

#include "../kernel/types.h"

extern "C" {
    extern const uint8_t _binary_hello_elf_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_hello_elf_bin_end[]   __attribute__((weak));
    extern const uint8_t _binary_hello_x64_elf_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_hello_x64_elf_bin_end[]   __attribute__((weak));
    extern const uint8_t _binary_kpython_elf_bin_start[]   __attribute__((weak));
    extern const uint8_t _binary_kpython_elf_bin_end[]     __attribute__((weak));
}

namespace EmbeddedUserprogs {

inline bool HasHello() {
    return _binary_hello_elf_bin_start != nullptr &&
           _binary_hello_elf_bin_end   != nullptr &&
           +_binary_hello_elf_bin_end > +_binary_hello_elf_bin_start;
}

inline const uint8_t* HelloData() {
    return _binary_hello_elf_bin_start;
}

inline uint32_t HelloSize() {
    return (uint32_t)((uintptr_t)_binary_hello_elf_bin_end -
                      (uintptr_t)_binary_hello_elf_bin_start);
}

inline bool HasHelloX64() {
    return _binary_hello_x64_elf_bin_start != nullptr &&
           _binary_hello_x64_elf_bin_end   != nullptr &&
           +_binary_hello_x64_elf_bin_end > +_binary_hello_x64_elf_bin_start;
}

inline const uint8_t* HelloX64Data() {
    return _binary_hello_x64_elf_bin_start;
}

inline uint32_t HelloX64Size() {
    return (uint32_t)((uintptr_t)_binary_hello_x64_elf_bin_end -
                      (uintptr_t)_binary_hello_x64_elf_bin_start);
}

inline bool HasKpython() {
    return _binary_kpython_elf_bin_start != nullptr &&
           _binary_kpython_elf_bin_end   != nullptr &&
           +_binary_kpython_elf_bin_end > +_binary_kpython_elf_bin_start;
}
inline const uint8_t* KpythonData() { return _binary_kpython_elf_bin_start; }
inline uint32_t KpythonSize() {
    return (uint32_t)((uintptr_t)_binary_kpython_elf_bin_end -
                      (uintptr_t)_binary_kpython_elf_bin_start);
}

} // namespace EmbeddedUserprogs
