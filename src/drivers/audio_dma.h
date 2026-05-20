#pragma once
//  kurono os  -  low-memory ISA DMA allocator
//
//  ISA DMA (used by Sound Blaster 16, AC97 BDL, floppy, etc.) requires
//  buffers that:
//    * live in physical memory below 16 MB
//    * never cross a 64 KB physical page boundary
//    * are 32-bit-addressable (so even AHCI/HDA on early machines work)
//
//  We carve out a fixed, page-aligned region of low conventional memory at
//  boot and hand out aligned chunks.  All audio drivers (SB16, AC97, HDA
//  optionally) share this allocator instead of fighting over hardcoded
//  physical addresses.
//
//  Layout (statically partitioned for determinism  -  no fragmentation):
//
//    0x00060000 .. 0x00067FFF   32 KB   SB16 single 8-bit DMA buffer
//    0x00068000 .. 0x0006FFFF   32 KB   SB16 spare / 16-bit channel
//    0x00070000 .. 0x00077FFF   32 KB   AC97 buffer descriptor list region
//                                       (BDL = first 256 B, rest is a
//                                        small staging area for tones)
//    0x00078000 .. 0x000BFFFF  288 KB   AC97 PCM ring (32 × 8 KB chunks)
//    0x000C0000 .. 0x000FFFFF  256 KB   reserved for ROM / EBDA  -  DO NOT USE
//
//  Anything above 0x100000 belongs to the kernel image, the heap, etc.
//  The reservation map is exposed via Mark*() helpers so the PMM can be
//  told never to re-hand these pages out to the buddy allocator.

#include "../kernel/types.h"

namespace AudioDMA {

// Region IDs.  Each region is contiguous, 64 KB-bounded, and reserved at
// boot so the PMM never touches it.
enum Region : uint8_t {
    REGION_SB16_PRIMARY  = 0,   // 0x60000, 32 KB
    REGION_SB16_SECONDARY= 1,   // 0x68000, 32 KB
    REGION_AC97_BDL      = 2,   // 0x70000, 32 KB (BDL + scratch)
    REGION_AC97_PCM      = 3,   // 0x78000, 288 KB
    REGION_COUNT
};

struct RegionInfo {
    uint32_t phys_base;   // 32-bit physical base address
    uint32_t size;        // bytes
    bool     in_use;      // for diagnostics only  -  regions are statically owned
    const char* owner;    // debug label
};

// One-time setup.  Zero-fills every region and marks each as "free".
// Idempotent: safe to call from multiple init paths.
void Init();

// Take exclusive ownership of a region.  Returns the physical base or
// nullptr if the region is already owned by another driver.  Marks the
// region as "in_use" with the given owner label.
void* Acquire(Region r, const char* owner);

// Release ownership.  After Release(), Acquire() may succeed again.
void  Release(Region r);

// Pure read of a region's metadata.  Useful for owners that want to know
// the size up front (e.g., BDL needs an aligned base inside the region).
const RegionInfo& GetRegion(Region r);

// Helpers for the 64 KB DMA-page rule.  ISA DMA latches the 16-bit page
// register once per transfer; if the buffer crosses a 64 KB boundary, the
// second half wraps back to the start of the same page.
//
//   PageOf(phys)  -> the 64 KB page index containing phys
//   FitsInPage(phys, len) -> true if [phys, phys+len) lies within one page
static inline uint32_t PageOf(uint32_t phys)            { return phys >> 16; }
static inline bool     FitsInPage(uint32_t phys, uint32_t len) {
    return PageOf(phys) == PageOf(phys + len - 1);
}

// 16-bit DMA (channel 5/6/7) addresses memory in 16-bit *words* relative
// to a 128 KB page.  This helper converts a byte address into the
// (page, word_offset, word_count) triple expected by the DMA controller.
struct Dma16Layout {
    uint8_t  page;          // bits 17..23, must be 128 KB aligned
    uint16_t word_offset;   // bits 1..16
    uint16_t word_count;    // length / 2 - 1
    bool     valid;
};
Dma16Layout SplitForDMA16(uint32_t phys, uint32_t length_bytes);

// Diagnostic dump: writes a multi-line summary to the serial log.
void Dump();

} // namespace AudioDMA
