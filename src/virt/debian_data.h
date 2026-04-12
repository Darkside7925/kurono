//  kurono os  -  embedded debian guest rootfs data
#pragma once
#include <stdint.h>

extern "C" {
    extern const uint8_t _binary_debian_minbase_ext4_start[] __attribute__((weak));
    extern const uint8_t _binary_debian_minbase_ext4_end[]   __attribute__((weak));
}

static inline uint32_t debian_rootfs_size() {
    return (uint32_t)((uintptr_t)_binary_debian_minbase_ext4_end -
                      (uintptr_t)_binary_debian_minbase_ext4_start);
}

static inline const uint8_t* debian_rootfs_data() {
    return _binary_debian_minbase_ext4_start;
}

static inline bool debian_rootfs_available() {
    return _binary_debian_minbase_ext4_start != nullptr &&
           _binary_debian_minbase_ext4_end != nullptr;
}
