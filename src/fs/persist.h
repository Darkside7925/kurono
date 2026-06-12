#pragma once
#include "../kernel/types.h"

//  kurono raw-sector state store  -  persists a single binary blob (the serialized
//  kvfs tree) directly to a dedicated nvme data disk, bypassing any filesystem.
//  uses ONLY the verified nvme read/write primitives. layout: lba 0 holds a 16
//  byte header (magic + version + length + crc32), the blob follows at byte
//  offset 4096, written in 4096-byte (one-page) chunks because the nvme driver
//  does single-page prp1 dma. the data disk is ours to own  -  we do not pretend
//  it is a real filesystem. (satoru)

class PersistStore {
public:
    // true if a usable nvme block device is present. (satoru)
    static bool Available();

    // write the blob (any length up to the disk size) + a committing header.
    // returns false if no device / write failed. (satoru)
    static bool Save(const uint8_t* blob, uint32_t len);

    // read the stored blob into buf (up to maxlen) and set *out_len. returns
    // false if there is no valid store (bad magic / oversize / crc mismatch),
    // leaving the caller's tree untouched. (satoru)
    static bool Load(uint8_t* buf, uint32_t maxlen, uint32_t* out_len);

    // KFS-backed persistence  -  the real-filesystem replacement for the raw blob.
    // SaveTree formats a fresh KFS volume on the nvme data disk and mirrors the
    // kvfs user-data subtrees (/home, /etc, /root) into it as real files + dirs;
    // LoadTree mounts it and restores them. (satoru)
    static bool SaveTree();
    static bool LoadTree();
};
// end (satoru)
