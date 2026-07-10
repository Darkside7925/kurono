#pragma once
#include "types.h"

//  kmemx lz4 - freestanding lz4 block-format codec for the kurono memory
//  compression engine (KMemX). no libc, no stl, zero heap allocation: the
//  caller supplies every buffer (the compressor needs a hash-table scratch
//  region, the decompressor needs none). this is a clean-room implementation
//  of the lz4 block format (the lz4 block spec), chosen for its ~2.5-3:1 ratio
//  on typical pages and multi-gb/s decompress. (satoru)
//
//  the codec is page-oriented: kmemx only ever compresses/decompresses whole
//  4 kb pages, but the api takes an explicit length so the in-kernel self-test
//  can exercise arbitrary sizes and the dedup/guest paths can reuse it. (satoru)

namespace KMemXLZ4 {

//  the worst-case compressed size for an input of `n` bytes. lz4 can expand
//  incompressible data slightly; a compressor MUST be handed a destination at
//  least this large or it reports failure. matches LZ4_COMPRESSBOUND. (satoru)
static inline int CompressBound(int n) {
    return n + (n / 255) + 16;
}

//  scratch bytes the compressor needs for its hash table. a 12-bit hash log
//  (4096 entries x 4-byte offsets) is the sweet spot for 4 kb inputs. (satoru)
constexpr int HASH_LOG     = 12;
constexpr int HASH_SIZE    = 1 << HASH_LOG;          // 4096 entries (satoru)
constexpr int SCRATCH_BYTES = HASH_SIZE * (int)sizeof(uint32_t);  // 16 kb (satoru)

//  compress `src_len` bytes from `src` into `dst` (capacity `dst_cap`). `scratch`
//  must be at least SCRATCH_BYTES and is used as the hash table (caller-owned, so
//  the hot path never allocates). returns the compressed byte count, or 0 if the
//  data did not fit in `dst_cap` (the caller then stores the page uncompressed).
//  (satoru)
int Compress(const uint8_t* src, int src_len,
             uint8_t* dst, int dst_cap, void* scratch);

//  decompress exactly `dst_len` bytes (the known original size) from `src`
//  (`src_len` compressed bytes) into `dst`. returns the number of bytes written
//  (== dst_len on success) or -1 on a malformed/overflowing stream. bounds are
//  checked on every token so a corrupt pool entry can never run off the end of
//  either buffer (it returns -1 and the caller panics). (satoru)
int Decompress(const uint8_t* src, int src_len,
               uint8_t* dst, int dst_len);

//  crc32 (ieee 802.3 polynomial, reflected) over `len` bytes. used to fingerprint
//  every page before compression and verify it after decompression: a mismatch is
//  silent memory corruption and triggers a kernel panic. slice-by-one with a
//  lazily-built 256-entry table. (satoru)
uint32_t Crc32(const uint8_t* data, int len);

}  // namespace KMemXLZ4

// end (satoru)
