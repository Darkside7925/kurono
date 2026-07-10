//  kurono os - debian guest rootfs accessor
//
//  on-disk fallback path: /var/lib/kurono/debian-rootfs.ext4
//
//  the rootfs may be (a) embedded into the kernel binary at link time
//  (large iso, opt-in via EMBED_DEBIAN=1 make var) or (b) downloaded at
//  runtime by `kpkg install debian` and then loaded from KVFS on demand.
//  callers should not care which.
#pragma once
#include <stdint.h>

namespace DebianRootfs {
    bool           Available();
    uint32_t       Size();
    const uint8_t* Data();
    bool           SaveDownloaded(const uint8_t* buf, uint32_t len);
    void           EvictCache();
}

// legacy inline shims - keep existing call sites compiling.
static inline uint32_t debian_rootfs_size()       { return DebianRootfs::Size(); }
static inline const uint8_t* debian_rootfs_data() { return DebianRootfs::Data(); }
static inline bool debian_rootfs_available()      { return DebianRootfs::Available(); }
