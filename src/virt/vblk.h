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
    bool Init(uint64_t lba, uint64_t bytes, uint8_t irq, const char* id);
    bool HasBacking();
    uint64_t Sectors();
    void Stats(uint32_t* reads, uint32_t* writes, uint32_t* flushes, uint32_t* errors);
    uint64_t BytesRead();
    uint64_t BytesWritten();
}

// end (satoru)
