#pragma once
//  kurono os - on-demand assets
//
//  the big optional payloads (alpine guest, recovery tools, ffmpeg, kpython,
//  the kld e1000e driver, demo videos) used to be objcopy'd into the kernel
//  image, which made the kernel ~106 mb for ~2.5 mb of code. they now live as
//  root files on the KURONO fat32 partition (next to FIREFOX.TAR) and load the
//  first time something asks for their data. a build that still embeds one
//  (KURONO_EMBED_ALL=1) keeps using the linked copy. (satoru)
//
//  names are fat 8.3 names, 11 chars, space padded: "ALPKERN BIN".
#include "../kernel/types.h"

namespace KAsset {
    // true if the asset is linked in or present on the partition (cheap: a
    // directory lookup, cached; never reads the payload). (satoru)
    bool Has(const char* name83, const uint8_t* emb_start, const uint8_t* emb_end);
    // the payload, loading it from the partition once if needed. nullptr if
    // missing or out of memory. (satoru)
    const uint8_t* Data(const char* name83, const uint8_t* emb_start, const uint8_t* emb_end);
    // payload size in bytes (0 if missing). (satoru)
    uint32_t Size(const char* name83, const uint8_t* emb_start, const uint8_t* emb_end);
    // true only when the payload is linked into this kernel (no disk read) (satoru)
    inline bool Linked(const uint8_t* s, const uint8_t* e) { return s && e && e > s; }
    // drop a loaded payload once nothing needs it any more (e.g. after unpacking) (satoru)
    void Release(const char* name83);
    // make sure a vfs path holds the asset, loading it on first use; the file
    // references the loaded buffer (no copy). false if unavailable. (satoru)
    bool EnsureVfsFile(const char* path, const char* name83, const uint8_t* emb_start, const uint8_t* emb_end);
}

// end (satoru)
