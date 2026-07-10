//  kurono os - low-memory ISA DMA allocator (implementation)
//
//  See audio_dma.h for the layout description.  This file is intentionally
//  small: the allocation policy is "fixed regions, take-or-fail".  No
//  fragmentation, no metadata persistence, no locking - audio drivers
//  acquire their region at boot and never release it during normal
//  operation.
#include "audio_dma.h"
#include "serial.h"
#include "../kernel/types.h"

namespace AudioDMA {

static RegionInfo g_regions[REGION_COUNT] = {
    { 0x00060000, 32u  * 1024,        false, nullptr },  // SB16_PRIMARY
    { 0x00068000, 32u  * 1024,        false, nullptr },  // SB16_SECONDARY
    { 0x00070000, 32u  * 1024,        false, nullptr },  // AC97_BDL
    { 0x00078000, 288u * 1024,        false, nullptr },  // AC97_PCM
};
static bool g_initialised = false;

static const char* RegionName(Region r) {
    switch (r) {
        case REGION_SB16_PRIMARY:  return "SB16_PRIMARY";
        case REGION_SB16_SECONDARY:return "SB16_SECONDARY";
        case REGION_AC97_BDL:      return "AC97_BDL";
        case REGION_AC97_PCM:      return "AC97_PCM";
        default:                   return "INVALID";
    }
}

void Init() {
    if (g_initialised) return;
    for (int i = 0; i < REGION_COUNT; i++) {
        // zero-fill the region so any subsequent DMA reads garbage-free.
        // 8-bit unsigned PCM "silence" is 0x80; 16-bit signed silence is 0.
        // We pick 0x00 here and let the owning driver re-paint with its
        // own silence pattern at acquire time.
        memset(reinterpret_cast<void*>(static_cast<uintptr_t>(g_regions[i].phys_base)),
               0, g_regions[i].size);
        g_regions[i].in_use = false;
        g_regions[i].owner  = nullptr;
    }
    g_initialised = true;
    SerialLogger::Log("[AudioDMA] Initialised 4 regions in low memory\r\n");
}

void* Acquire(Region r, const char* owner) {
    if (!g_initialised) Init();
    if (r >= REGION_COUNT) {
        SerialLogger::Log("[AudioDMA] Acquire(): invalid region\r\n");
        return nullptr;
    }
    if (g_regions[r].in_use) {
        SerialLogger::Log("[AudioDMA] Acquire(): region ");
        SerialLogger::Log(RegionName(r));
        SerialLogger::Log(" already owned by ");
        SerialLogger::Log(g_regions[r].owner ? g_regions[r].owner : "?");
        SerialLogger::Log(", refusing\r\n");
        return nullptr;
    }
    g_regions[r].in_use = true;
    g_regions[r].owner  = owner;
    SerialLogger::Log("[AudioDMA] ");
    SerialLogger::Log(owner);
    SerialLogger::Log(" acquired ");
    SerialLogger::Log(RegionName(r));
    SerialLogger::Log("\r\n");
    return reinterpret_cast<void*>(static_cast<uintptr_t>(g_regions[r].phys_base));
}

void Release(Region r) {
    if (r >= REGION_COUNT) return;
    g_regions[r].in_use = false;
    g_regions[r].owner  = nullptr;
}

const RegionInfo& GetRegion(Region r) {
    static const RegionInfo invalid = { 0, 0, false, "INVALID" };
    if (r >= REGION_COUNT) return invalid;
    return g_regions[r];
}

Dma16Layout SplitForDMA16(uint32_t phys, uint32_t length_bytes) {
    Dma16Layout out{};
    out.valid = false;
    if ((length_bytes & 1) != 0) return out;          // must be 16-bit aligned
    if (length_bytes == 0)        return out;
    if (length_bytes > 0x20000)   return out;          // > 128 KB doesn't fit
    // 16-bit DMA latches a 7-bit page register that selects a 128 KB
    // window in physical memory.  The buffer must lie wholly inside that
    // window.  Compute (page, word_offset, word_count).
    uint32_t page128k_base = phys & ~0x1FFFFu;        // 128 KB aligned
    uint32_t end           = phys + length_bytes - 1;
    if ((end & ~0x1FFFFu) != page128k_base) return out; // crosses 128 KB
    out.page        = static_cast<uint8_t>((page128k_base >> 17) & 0x7F);
    out.word_offset = static_cast<uint16_t>(((phys - page128k_base) >> 1) & 0xFFFF);
    out.word_count  = static_cast<uint16_t>((length_bytes / 2) - 1);
    out.valid       = true;
    return out;
}

void Dump() {
    SerialLogger::Log("[AudioDMA] Region map:\r\n");
    for (int i = 0; i < REGION_COUNT; i++) {
        SerialLogger::Log("  ");
        SerialLogger::Log(RegionName(static_cast<Region>(i)));
        SerialLogger::Log(" @ 0x");
        SerialLogger::LogHex(g_regions[i].phys_base);
        SerialLogger::Log(" size=");
        SerialLogger::LogDec(g_regions[i].size);
        SerialLogger::Log(g_regions[i].in_use ? " (in use by " : " (free)");
        if (g_regions[i].in_use) {
            SerialLogger::Log(g_regions[i].owner ? g_regions[i].owner : "?");
            SerialLogger::Log(")");
        }
        SerialLogger::Log("\r\n");
    }
}

} // namespace AudioDMA
