// kurono os  -  kvid container
#include "kvid.h"

namespace KVID {

static uint32_t LE32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool Open(const uint8_t* data, uint32_t size, File& out) {
    out = File{};
    if (!data || size < sizeof(Header)) return false;
    // magic stored as ascii "KVID" at byte offset 0; little-endian read
    // gives 'K' | 'V'<<8 | 'I'<<16 | 'D'<<24
    constexpr uint32_t expected = ('K') | ('V' << 8) | ('I' << 16) | ('D' << 24);
    uint32_t magic = LE32(data);
    if (magic != expected) return false;
    // safe copy via memcpy to avoid alignment trouble on packed layout
    memcpy(&out.hdr, data, sizeof(Header));
    if (out.hdr.version != kVersion) return false;
    if (out.hdr.frame_count == 0) return false;
    uint64_t idx_bytes = (uint64_t)out.hdr.index_count * sizeof(IndexEntry);
    if (out.hdr.index_offset + idx_bytes > size) return false;
    out.data  = data;
    out.size  = size;
    out.index = (const IndexEntry*)(data + out.hdr.index_offset);
    return true;
}

uint32_t DurationMs(const File& f) {
    if (f.hdr.fps_num == 0) return 0;
    // duration = frame_count * 1000 * fps_den / fps_num
    return (uint32_t)(((uint64_t)f.hdr.frame_count * 1000ull *
                       (uint64_t)f.hdr.fps_den) / (uint64_t)f.hdr.fps_num);
}

const uint8_t* GetFrameJpeg(const File& f, uint32_t i, uint32_t* sz) {
    if (i >= f.hdr.frame_count) { *sz = 0; return nullptr; }
    const IndexEntry& e = f.index[i];
    if (e.offset + e.video_size > f.size) { *sz = 0; return nullptr; }
    *sz = e.video_size;
    return f.data + e.offset;
}

const uint8_t* GetFrameAudio(const File& f, uint32_t i, uint32_t* sz) {
    if (i >= f.hdr.frame_count) { *sz = 0; return nullptr; }
    const IndexEntry& e = f.index[i];
    if (e.audio_size == 0) { *sz = 0; return nullptr; }
    if (e.audio_offset + e.audio_size > f.size) { *sz = 0; return nullptr; }
    *sz = e.audio_size;
    return f.data + e.audio_offset;
}

uint32_t FrameAtMs(const File& f, uint32_t ms) {
    if (f.hdr.frame_count == 0) return 0;
    uint64_t target_us = (uint64_t)ms * 1000ull;
    // binary search on dts_us
    uint32_t lo = 0, hi = f.hdr.frame_count - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (f.index[mid].dts_us <= target_us) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

} // namespace KVID
