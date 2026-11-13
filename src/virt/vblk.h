//  kurono os - virtio-blk host device for linux guests
//
//  the guest root disk is a preallocated contiguous image file on the KURONO
//  partition (ALPINE.IMG, DEBIAN.IMG ...): an nvme extent made writable for
//  the running vm by the third guard window, so packages the guest installs
//  persist across vm restarts and kurono reboots. requests are served
//  synchronously on the vm thread. (satoru)
#pragma once
#include <stdint.h>

namespace VBlk {
    // registers the device for the image at nvme `lba`, `bytes` long; `id` is
    // the serial the guest reads back (virtio GET_ID). false if no backing. (satoru)
    // chunk_prefix8 (8 space padded chars of NAME.000), img83 (the image's 8.3 name, to re-arm
    // its write window) and virt_bytes > bytes turn on the chunked tail: virt_bytes - bytes of
    // 64 mb chunk files, created on first write (satoru)
    bool Init(uint64_t lba, uint64_t bytes, uint8_t irq, const char* id,
              const char* chunk_prefix8 = nullptr, const char* img83 = nullptr, uint64_t virt_bytes = 0);
    void ChunkStats(uint32_t* present, uint32_t* total);
    bool HasBacking();
    uint64_t Sectors();
    void Stats(uint32_t* reads, uint32_t* writes, uint32_t* flushes, uint32_t* errors);
    uint64_t BytesRead();
    uint64_t BytesWritten();
}

// end (satoru)
