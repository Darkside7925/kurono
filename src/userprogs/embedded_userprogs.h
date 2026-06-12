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
    // musl-static hello blob (satoru)
    extern const uint8_t _binary_hello_musl_elf_start[]    __attribute__((weak));
    extern const uint8_t _binary_hello_musl_elf_end[]      __attribute__((weak));
    // musl-static ffmpeg blob (satoru)
    extern const uint8_t _binary_ffmpeg_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_ffmpeg_elf_end[]          __attribute__((weak));
    // raw-protocol wl_shm wayland test client blob (satoru)
    extern const uint8_t _binary_wl_shm_test_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_wl_shm_test_elf_end[]     __attribute__((weak));
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

// musl-static hello accessors (satoru)
inline bool HasMuslHello() {
    return _binary_hello_musl_elf_start != nullptr &&
           _binary_hello_musl_elf_end   != nullptr &&
           +_binary_hello_musl_elf_end > +_binary_hello_musl_elf_start;
}
inline const uint8_t* MuslHelloData() { return _binary_hello_musl_elf_start; }
inline uint32_t MuslHelloSize() {
    return (uint32_t)((uintptr_t)_binary_hello_musl_elf_end -
                      (uintptr_t)_binary_hello_musl_elf_start);
}

// musl-static ffmpeg accessors (satoru)
inline bool HasFfmpeg() {
    return _binary_ffmpeg_elf_start != nullptr &&
           _binary_ffmpeg_elf_end   != nullptr &&
           +_binary_ffmpeg_elf_end > +_binary_ffmpeg_elf_start;
}
inline const uint8_t* FfmpegData() { return _binary_ffmpeg_elf_start; }
inline uint32_t FfmpegSize() {
    return (uint32_t)((uintptr_t)_binary_ffmpeg_elf_end -
                      (uintptr_t)_binary_ffmpeg_elf_start);
}

// raw-protocol wl_shm wayland test client accessors (satoru)
inline bool HasWlShmTest() {
    return _binary_wl_shm_test_elf_start != nullptr &&
           _binary_wl_shm_test_elf_end   != nullptr &&
           +_binary_wl_shm_test_elf_end > +_binary_wl_shm_test_elf_start;
}
inline const uint8_t* WlShmTestData() { return _binary_wl_shm_test_elf_start; }
inline uint32_t WlShmTestSize() {
    return (uint32_t)((uintptr_t)_binary_wl_shm_test_elf_end -
                      (uintptr_t)_binary_wl_shm_test_elf_start);
}

} // namespace EmbeddedUserprogs
