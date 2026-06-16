#pragma once
#include "types.h"

//  KMemX pool  -  the fixed physical arena that holds compressed page blobs, plus
//  its variable-size slot allocator. split out of kmemx.cpp so the allocator can
//  be unit-tested in isolation (the stage-2 self-test exercises it directly).
//  (satoru)
//
//  the arena is a set of 2mb chunks pulled from the pmm at init (a single giant
//  contiguous reservation would fragment / fail; 2mb chunks are reliable and
//  each comfortably holds hundreds of <=4kb blobs). a blob never straddles a
//  chunk boundary, so a chunk is a self-contained heap. allocation is best-fit
//  over a per-arena free-list with coalescing on free. all sizes are rounded up
//  to KMEMX_POOL_GRAIN so the free-list stays coarse and metadata stays small.
//  (satoru)

namespace KMemXPool {

constexpr uint64_t CHUNK_BYTES = 2ULL * 1024 * 1024;   // 2mb pool chunk (satoru)
constexpr uint32_t GRAIN       = 16;                    // allocation granularity (satoru)
constexpr uint32_t MAX_CHUNKS  = 512;                   // up to 1gb pool (satoru)

// reserve `want_bytes` of pool from the pmm, rounded up to whole 2mb chunks
// (capped at MAX_CHUNKS). returns the bytes actually reserved (0 on failure).
// idempotent-ish: a second call with a larger size adds chunks; smaller is a
// no-op here (SetPoolPct handles safe shrink). (satoru)
uint64_t Reserve(uint64_t want_bytes);

// total reserved + currently-allocated byte counts. (satoru)
uint64_t TotalBytes();
uint64_t UsedBytes();
uint32_t ChunkCount();

// allocate `n` bytes (1..CHUNK headroom) from the arena. returns a POOL OFFSET
// (a stable handle: chunk_index * CHUNK_BYTES + intra-chunk offset) or
// KMEMX_POOL_NULL on failure. the offset maps to a real pointer via Ptr(). the
// allocator never crosses a chunk boundary. (satoru)
uint32_t Alloc(uint32_t n);

// free a previous Alloc() result of size `n` (the caller tracks the size in the
// page metadata, so the allocator header stays out of the hot blob bytes). (satoru)
void Free(uint32_t pool_off, uint32_t n);

// translate a pool offset to a directly-usable kernel pointer (identity-mapped).
// returns nullptr for KMEMX_POOL_NULL. (satoru)
void* Ptr(uint32_t pool_off);

// grow the arena by reserving more chunks up to `want_bytes` total. (satoru)
uint64_t GrowTo(uint64_t want_bytes);

// largest single allocation the arena can currently satisfy (diagnostic). (satoru)
uint32_t LargestFree();

// reset the allocator to empty (frees nothing back to the pmm; just marks every
// chunk wholly free). used only by the self-test between cases. (satoru)
void ResetForTest();

}  // namespace KMemXPool

// the sentinel "no allocation" pool offset. 0 is a valid offset (start of chunk
// 0), so the null handle is all-ones. (satoru)
constexpr uint32_t KMEMX_POOL_NULL = 0xFFFFFFFFu;

// end (satoru)
