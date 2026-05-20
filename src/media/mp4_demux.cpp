// kurono os  -  mp4 / isobmff demuxer implementation
// see mp4_demux.h for the full feature list and design rationale.
//
// the implementation is intentionally a single translation unit so the
// optimiser can inline the read helpers freely.  every byte read goes
// through bounds-checked Reader helpers which return false on overflow;
// callers are expected to early-exit on false to keep the code linear.
#include "mp4_demux.h"
#include "../kernel/heap.h"
#include "../system/logging.h"
#include "../drivers/serial.h"

namespace MP4 {

// =========================================================================
// big-endian integer readers (mp4 is exclusively big-endian on the wire)
// =========================================================================
namespace {

struct Reader {
    const uint8_t* base;     // start of the buffer (used for absolute offsets)
    const uint8_t* end;      // one past the last valid byte
    const uint8_t* p;        // current read cursor

    bool Remaining(uint32_t n) const { return (uint32_t)(end - p) >= n; }
    uint64_t Offset() const { return (uint64_t)(p - base); }

    bool Skip(uint64_t n) {
        if ((uint64_t)(end - p) < n) return false;
        p += n;
        return true;
    }

    bool U8(uint8_t& v) {
        if (!Remaining(1)) return false;
        v = *p++;
        return true;
    }
    bool U16(uint16_t& v) {
        if (!Remaining(2)) return false;
        v = ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        return true;
    }
    bool U24(uint32_t& v) {
        if (!Remaining(3)) return false;
        v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        p += 3;
        return true;
    }
    bool U32(uint32_t& v) {
        if (!Remaining(4)) return false;
        v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8)  | p[3];
        p += 4;
        return true;
    }
    bool U64(uint64_t& v) {
        if (!Remaining(8)) return false;
        v = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
            ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
            ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
            ((uint64_t)p[6] << 8)  | p[7];
        p += 8;
        return true;
    }
};

// box header: 4-byte size, 4-byte type.  size==1 → 8-byte large size
// follows.  size==0 → box runs to eof.  if type==uuid (we ignore those)
// 16 more bytes of extended type follow.
struct BoxHeader {
    uint64_t       payload_size;   // bytes of payload after header
    uint32_t       type;           // fourcc
    const uint8_t* payload_start;
    const uint8_t* box_end;        // one past last byte of this box
};

// parse a box header at r->p; advance r->p to the start of the payload.
// returns false on malformed header (too small, runs off end, etc).
bool ReadBoxHeader(Reader* r, BoxHeader* hdr) {
    const uint8_t* hdr_start = r->p;
    uint32_t size32; uint32_t type;
    if (!r->U32(size32)) return false;
    if (!r->U32(type))   return false;
    uint64_t total_size;
    if (size32 == 1) {
        if (!r->U64(total_size)) return false;
    } else if (size32 == 0) {
        // extends to end of file
        total_size = (uint64_t)(r->end - hdr_start);
    } else if (size32 < 8) {
        return false;
    } else {
        total_size = size32;
    }
    uint64_t header_size = (uint64_t)(r->p - hdr_start);
    if (total_size < header_size) return false;
    uint64_t payload_size = total_size - header_size;
    if ((uint64_t)(r->end - r->p) < payload_size) return false;
    hdr->payload_size  = payload_size;
    hdr->type          = type;
    hdr->payload_start = r->p;
    hdr->box_end       = r->p + payload_size;
    return true;
}

// read full box version + flags (1 + 3 bytes)
bool ReadFullBoxHeader(Reader* r, uint8_t* version, uint32_t* flags) {
    if (!r->U8(*version)) return false;
    return r->U24(*flags);
}

// trivial little helpers
uint32_t MinU32(uint32_t a, uint32_t b) { return a < b ? a : b; }

} // anon namespace

// =========================================================================
// box parsers  -  each parses one container or leaf box and updates state
// =========================================================================

namespace {

struct ParserState {
    const uint8_t* file_base;
    uint32_t       file_size;
    Movie*         mv;
    Track*         cur_track;   // null when not inside a trak
    int            depth;       // recursion guard
};

bool ParseBoxList(ParserState* st, const uint8_t* start, const uint8_t* end);

bool ParseMvhd(ParserState* st, BoxHeader& bh) {
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t version; uint32_t flags;
    if (!ReadFullBoxHeader(&r, &version, &flags)) return false;
    uint64_t creation, modification, duration;
    uint32_t timescale, rate; uint16_t volume;
    if (version == 1) {
        if (!r.U64(creation))   return false;
        if (!r.U64(modification)) return false;
        if (!r.U32(timescale))  return false;
        if (!r.U64(duration))   return false;
    } else {
        uint32_t c32, m32, d32;
        if (!r.U32(c32)) return false;
        if (!r.U32(m32)) return false;
        if (!r.U32(timescale)) return false;
        if (!r.U32(d32)) return false;
        creation = c32; modification = m32; duration = d32;
    }
    if (!r.U32(rate)) return false;
    if (!r.U16(volume)) return false;
    // remainder (reserved + matrix + pre_defined + next_track_id) is
    // ignored  -  we don't need it.
    st->mv->timescale       = timescale;
    st->mv->duration_units  = duration;
    return true;
}

bool ParseTkhd(ParserState* st, BoxHeader& bh) {
    if (!st->cur_track) return false;
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t version; uint32_t flags;
    if (!ReadFullBoxHeader(&r, &version, &flags)) return false;
    uint64_t creation, modification, duration;
    uint32_t track_id, reserved;
    if (version == 1) {
        if (!r.U64(creation))     return false;
        if (!r.U64(modification)) return false;
        if (!r.U32(track_id))     return false;
        if (!r.U32(reserved))     return false;
        if (!r.U64(duration))     return false;
    } else {
        uint32_t c32, m32, d32;
        if (!r.U32(c32))      return false;
        if (!r.U32(m32))      return false;
        if (!r.U32(track_id)) return false;
        if (!r.U32(reserved)) return false;
        if (!r.U32(d32))      return false;
        duration = d32;
    }
    st->cur_track->track_id = track_id;
    return true;
}

bool ParseMdhd(ParserState* st, BoxHeader& bh) {
    if (!st->cur_track) return false;
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t version; uint32_t flags;
    if (!ReadFullBoxHeader(&r, &version, &flags)) return false;
    uint64_t creation, modification, duration;
    uint32_t timescale;
    if (version == 1) {
        if (!r.U64(creation))     return false;
        if (!r.U64(modification)) return false;
        if (!r.U32(timescale))    return false;
        if (!r.U64(duration))     return false;
    } else {
        uint32_t c32, m32, d32;
        if (!r.U32(c32))      return false;
        if (!r.U32(m32))      return false;
        if (!r.U32(timescale)) return false;
        if (!r.U32(d32))      return false;
        duration = d32;
    }
    st->cur_track->timescale      = timescale;
    st->cur_track->duration_units = duration;
    return true;
}

bool ParseHdlr(ParserState* st, BoxHeader& bh) {
    if (!st->cur_track) return false;
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t version; uint32_t flags; uint32_t pre_defined; uint32_t handler;
    if (!ReadFullBoxHeader(&r, &version, &flags)) return false;
    if (!r.U32(pre_defined)) return false;
    if (!r.U32(handler))     return false;
    st->cur_track->handler = handler;
    if      (handler == Handler::vide) st->cur_track->kind = TRACK_VIDEO;
    else if (handler == Handler::soun) st->cur_track->kind = TRACK_AUDIO;
    else                               st->cur_track->kind = TRACK_OTHER;
    return true;
}

// scan the codec-private box (avcC, hvcC, esds, ...) inside the sample
// description entry's "extra boxes" tail and capture a borrowed pointer.
bool CaptureCodecPrivate(Track* tr, Reader r) {
    while (r.Remaining(8)) {
        BoxHeader inner;
        if (!ReadBoxHeader(&r, &inner)) return true; // tolerate trailing junk
        switch (inner.type) {
            case Box::avcC:
            case Box::hvcC:
            case Box::esds: {
                uint32_t len = (uint32_t)inner.payload_size;
                if (len > kMaxCodecPrivateBytes) len = kMaxCodecPrivateBytes;
                tr->codec_priv     = inner.payload_start;
                tr->codec_priv_len = len;
                return true;
            }
            default: break;
        }
        r.p = inner.box_end;
    }
    return true;
}

bool ParseStsd(ParserState* st, BoxHeader& bh) {
    if (!st->cur_track) return false;
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t version; uint32_t flags;
    if (!ReadFullBoxHeader(&r, &version, &flags)) return false;
    uint32_t entry_count;
    if (!r.U32(entry_count)) return false;
    if (entry_count == 0) return true;

    BoxHeader entry;
    if (!ReadBoxHeader(&r, &entry)) return false;
    st->cur_track->codec = entry.type;

    Reader er{st->file_base, entry.box_end, entry.payload_start};
    // common: 6 bytes reserved + 2 bytes data_reference_index
    if (!er.Skip(6)) return false;
    uint16_t dref_idx;
    if (!er.U16(dref_idx)) return false;

    if (st->cur_track->kind == TRACK_VIDEO) {
        // visual sample entry
        // 16 bytes pre_defined+reserved+pre_defined3
        if (!er.Skip(16)) return false;
        uint16_t w, h;
        if (!er.U16(w)) return false;
        if (!er.U16(h)) return false;
        // skip horiz/vert resolution (4+4) + reserved(4) + frame_count(2)
        // + compressorname(32) + depth(2) + pre_defined(2)
        if (!er.Skip(4 + 4 + 4 + 2 + 32)) return false;
        uint16_t depth, pre_def;
        if (!er.U16(depth))   return false;
        if (!er.U16(pre_def)) return false;
        st->cur_track->video.width  = w;
        st->cur_track->video.height = h;
        st->cur_track->video.depth  = depth;
    } else if (st->cur_track->kind == TRACK_AUDIO) {
        // audio sample entry (version 0)
        uint16_t v0; uint16_t v1; uint32_t pre_defined0;
        if (!er.U16(v0))   return false;       // version (0 or 1)
        if (!er.U16(v1))   return false;       // revision_level
        if (!er.U32(pre_defined0)) return false;
        uint16_t channels, sample_size, pre_defined1, reserved2;
        uint32_t sample_rate_fixed;            // 16.16 fixed
        if (!er.U16(channels))    return false;
        if (!er.U16(sample_size)) return false;
        if (!er.U16(pre_defined1)) return false;
        if (!er.U16(reserved2))   return false;
        if (!er.U32(sample_rate_fixed)) return false;
        st->cur_track->audio.channels    = channels;
        st->cur_track->audio.sample_size = sample_size;
        st->cur_track->audio.sample_rate = sample_rate_fixed >> 16;
        // version 1 extends with 16 more bytes; version 2 with even more.
        // they're optional info; codec setup comes from esds.
        if (v0 == 1) {
            if (!er.Skip(16)) return false;
        } else if (v0 == 2) {
            if (!er.Skip(36)) return false;
        }
    } else {
        // unknown handler  -  leave codec but don't try to interpret entry
    }
    // remainder of the entry should be "extra boxes"  -  codec-private data
    return CaptureCodecPrivate(st->cur_track, er);
}

bool ParseStts(ParserState* st, BoxHeader& bh) {
    // we don't materialise stts directly  -  defer to BuildSampleTable which
    // walks all four tables together for cache efficiency.  here we just
    // make sure the box is well-formed (entry_count fits).
    (void)st;
    Reader r{st->file_base, bh.box_end, bh.payload_start};
    uint8_t v; uint32_t f; uint32_t cnt;
    if (!ReadFullBoxHeader(&r, &v, &f)) return false;
    if (!r.U32(cnt)) return false;
    if ((uint64_t)cnt * 8u > bh.payload_size) return false;
    return true;
}

// dispatcher for boxes inside trak/mdia/minf/stbl
bool ParseTrakChild(ParserState* st, BoxHeader& bh);

// helper: walk every box in [start, end), dispatching by type.
bool ParseBoxList(ParserState* st, const uint8_t* start, const uint8_t* end) {
    if (++st->depth > kMaxBoxNestDepth) { --st->depth; return false; }
    Reader r{st->file_base, end, start};
    while (r.p < r.end) {
        BoxHeader bh;
        if (!ReadBoxHeader(&r, &bh)) { --st->depth; return false; }
        if (!ParseTrakChild(st, bh)) { --st->depth; return false; }
        r.p = bh.box_end;
    }
    --st->depth;
    return true;
}

bool ParseTrak(ParserState* st, BoxHeader& bh) {
    if (st->mv->track_count >= kMaxTracks) return true; // silently drop extras
    Track* tr = &st->mv->tracks[st->mv->track_count];
    *tr = Track{};
    tr->used = true;
    Track* prev = st->cur_track;
    st->cur_track = tr;
    bool ok = ParseBoxList(st, bh.payload_start, bh.box_end);
    st->cur_track = prev;
    if (ok) st->mv->track_count++;
    return ok;
}

bool ParseTrakChild(ParserState* st, BoxHeader& bh) {
    switch (bh.type) {
        case Box::mvhd: return ParseMvhd(st, bh);
        case Box::trak: return ParseTrak(st, bh);
        case Box::tkhd: return ParseTkhd(st, bh);
        case Box::mdia: return ParseBoxList(st, bh.payload_start, bh.box_end);
        case Box::mdhd: return ParseMdhd(st, bh);
        case Box::hdlr: return ParseHdlr(st, bh);
        case Box::minf: return ParseBoxList(st, bh.payload_start, bh.box_end);
        case Box::stbl: return ParseBoxList(st, bh.payload_start, bh.box_end);
        case Box::stsd: return ParseStsd(st, bh);
        case Box::stts: return ParseStts(st, bh);
        // leaf tables we'll re-read in BuildSampleTable  -  accept silently
        case Box::ctts: case Box::stsc: case Box::stsz: case Box::stz2:
        case Box::stco: case Box::co64: case Box::stss:
            return true;
        default:
            return true; // tolerate unknown boxes
    }
}

// =========================================================================
// sample table builder  -  combines stts + ctts + stsc + stsz + stco + stss
// =========================================================================

// re-find a child box of given type inside a container box list.
const uint8_t* FindBox(const uint8_t* start, const uint8_t* end,
                       uint32_t type, uint32_t* out_size) {
    Reader r{start, end, start};
    while (r.p < r.end) {
        BoxHeader bh;
        if (!ReadBoxHeader(&r, &bh)) return nullptr;
        if (bh.type == type) {
            *out_size = (uint32_t)bh.payload_size;
            return bh.payload_start;
        }
        r.p = bh.box_end;
    }
    return nullptr;
}

// recursive descent helper to walk path moov/trak/mdia/minf/stbl
const uint8_t* FindStbl(const uint8_t* file, uint32_t file_size,
                        uint32_t track_index, uint32_t* stbl_size) {
    uint32_t moov_size = 0;
    const uint8_t* moov = FindBox(file, file + file_size, Box::moov, &moov_size);
    if (!moov) return nullptr;
    // walk trak children, picking the Nth one
    Reader r{moov, moov + moov_size, moov};
    uint32_t i = 0;
    while (r.p < r.end) {
        BoxHeader bh;
        if (!ReadBoxHeader(&r, &bh)) return nullptr;
        if (bh.type == Box::trak) {
            if (i == track_index) {
                uint32_t mdia_size = 0;
                const uint8_t* mdia = FindBox(bh.payload_start, bh.box_end,
                                              Box::mdia, &mdia_size);
                if (!mdia) return nullptr;
                uint32_t minf_size = 0;
                const uint8_t* minf = FindBox(mdia, mdia + mdia_size,
                                              Box::minf, &minf_size);
                if (!minf) return nullptr;
                return FindBox(minf, minf + minf_size, Box::stbl, stbl_size);
            }
            i++;
        }
        r.p = bh.box_end;
    }
    return nullptr;
}

// build the per-sample table by interleaving the five small tables.
// this is the canonical iso/iec 14496-12 §8.7 algorithm.
bool BuildSampleTable(const uint8_t* file, uint32_t file_size,
                      uint32_t track_index, Track* tr) {
    uint32_t stbl_size = 0;
    const uint8_t* stbl = FindStbl(file, file_size, track_index, &stbl_size);
    if (!stbl) return false;
    const uint8_t* stbl_end = stbl + stbl_size;

    uint32_t stts_size = 0, ctts_size = 0, stsc_size = 0, stsz_size = 0;
    uint32_t stco_size = 0, co64_size = 0, stss_size = 0;
    const uint8_t* stts_p = FindBox(stbl, stbl_end, Box::stts, &stts_size);
    const uint8_t* ctts_p = FindBox(stbl, stbl_end, Box::ctts, &ctts_size);
    const uint8_t* stsc_p = FindBox(stbl, stbl_end, Box::stsc, &stsc_size);
    const uint8_t* stsz_p = FindBox(stbl, stbl_end, Box::stsz, &stsz_size);
    const uint8_t* stco_p = FindBox(stbl, stbl_end, Box::stco, &stco_size);
    const uint8_t* co64_p = FindBox(stbl, stbl_end, Box::co64, &co64_size);
    const uint8_t* stss_p = FindBox(stbl, stbl_end, Box::stss, &stss_size);

    if (!stts_p || !stsc_p || !stsz_p || (!stco_p && !co64_p)) return false;

    // ---- stsz: total sample count + per-sample sizes -------------------
    Reader rsz{file, stsz_p + stsz_size, stsz_p};
    uint8_t v; uint32_t f;
    uint32_t sample_size_default; uint32_t sample_count;
    if (!ReadFullBoxHeader(&rsz, &v, &f))   return false;
    if (!rsz.U32(sample_size_default))      return false;
    if (!rsz.U32(sample_count))             return false;
    if (sample_count == 0) return true;
    if (sample_count > 0x00ffffffu) return false; // sanity cap

    Sample* samples = (Sample*)KernelHeap::Alloc(sizeof(Sample) * sample_count);
    if (!samples) return false;
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = Sample{};
        samples[i].is_keyframe = false;
        if (sample_size_default != 0) {
            samples[i].size = sample_size_default;
        } else {
            uint32_t sz;
            if (!rsz.U32(sz)) { KernelHeap::Free(samples); return false; }
            samples[i].size = sz;
        }
    }

    // ---- stco / co64: chunk file offsets --------------------------------
    bool co64 = (co64_p != nullptr);
    const uint8_t* co_p = co64 ? co64_p : stco_p;
    uint32_t co_size    = co64 ? co64_size : stco_size;
    Reader rco{file, co_p + co_size, co_p};
    if (!ReadFullBoxHeader(&rco, &v, &f)) { KernelHeap::Free(samples); return false; }
    uint32_t chunk_count;
    if (!rco.U32(chunk_count)) { KernelHeap::Free(samples); return false; }
    if (chunk_count == 0) { KernelHeap::Free(samples); return true; }

    uint64_t* chunk_off = (uint64_t*)KernelHeap::Alloc(sizeof(uint64_t) * chunk_count);
    if (!chunk_off) { KernelHeap::Free(samples); return false; }
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (co64) {
            uint64_t o; if (!rco.U64(o)) { KernelHeap::Free(samples); KernelHeap::Free(chunk_off); return false; }
            chunk_off[i] = o;
        } else {
            uint32_t o; if (!rco.U32(o)) { KernelHeap::Free(samples); KernelHeap::Free(chunk_off); return false; }
            chunk_off[i] = o;
        }
    }

    // ---- stsc: sample-to-chunk run-length -------------------------------
    // each entry (first_chunk, samples_per_chunk, sample_description_index)
    // applies to all chunks from first_chunk until the next entry's first.
    Reader rsc{file, stsc_p + stsc_size, stsc_p};
    if (!ReadFullBoxHeader(&rsc, &v, &f)) { KernelHeap::Free(samples); KernelHeap::Free(chunk_off); return false; }
    uint32_t stsc_entries;
    if (!rsc.U32(stsc_entries) || stsc_entries == 0) {
        KernelHeap::Free(samples); KernelHeap::Free(chunk_off); return false;
    }

    // walk stsc to assign each sample its (chunk_index, intra_chunk_idx)
    // we hold one stsc record at a time and step through chunks.
    struct StscRec { uint32_t first_chunk, spc, sdi; };
    StscRec cur_run{1, 0, 0};
    StscRec next_run{0xffffffffu, 0, 0};
    if (!rsc.U32(cur_run.first_chunk)) goto fail;
    if (!rsc.U32(cur_run.spc))         goto fail;
    if (!rsc.U32(cur_run.sdi))         goto fail;
    if (stsc_entries > 1) {
        if (!rsc.U32(next_run.first_chunk)) goto fail;
        if (!rsc.U32(next_run.spc))         goto fail;
        if (!rsc.U32(next_run.sdi))         goto fail;
    }
    uint32_t stsc_idx; stsc_idx = 1; // index of *next* run already loaded
    {
        uint32_t sample_i = 0;
        for (uint32_t chunk_i = 0; chunk_i < chunk_count && sample_i < sample_count; chunk_i++) {
            // advance run if we've passed next_run.first_chunk
            uint32_t one_based = chunk_i + 1;
            while (stsc_idx + 1 < stsc_entries && one_based >= next_run.first_chunk) {
                cur_run = next_run;
                stsc_idx++;
                if (!rsc.U32(next_run.first_chunk)) goto fail;
                if (!rsc.U32(next_run.spc))         goto fail;
                if (!rsc.U32(next_run.sdi))         goto fail;
            }
            // also handle the "consume the last record" jump
            if (stsc_idx + 1 == stsc_entries && one_based >= next_run.first_chunk) {
                cur_run = next_run;
                stsc_idx++;
            }
            uint64_t off = chunk_off[chunk_i];
            uint32_t spc = cur_run.spc;
            for (uint32_t k = 0; k < spc && sample_i < sample_count; k++) {
                samples[sample_i].file_offset = off;
                off += samples[sample_i].size;
                sample_i++;
            }
        }
        // any tail samples without a chunk → drop sample_count to what we filled
        if (sample_i < sample_count) sample_count = sample_i;
    }

    // ---- stts: decoding timestamps --------------------------------------
    {
        Reader rt{file, stts_p + stts_size, stts_p};
        if (!ReadFullBoxHeader(&rt, &v, &f)) goto fail;
        uint32_t stts_entries;
        if (!rt.U32(stts_entries)) goto fail;
        uint64_t cur_dts = 0;
        uint32_t s_i = 0;
        for (uint32_t e = 0; e < stts_entries && s_i < sample_count; e++) {
            uint32_t cnt, delta;
            if (!rt.U32(cnt))   goto fail;
            if (!rt.U32(delta)) goto fail;
            for (uint32_t k = 0; k < cnt && s_i < sample_count; k++) {
                samples[s_i].dts = cur_dts;
                samples[s_i].pts = cur_dts; // overridden by ctts below
                cur_dts += delta;
                s_i++;
            }
        }
        // any uncovered tail keeps dts=0  -  best-effort
    }

    // ---- ctts: composition offsets (optional) ---------------------------
    if (ctts_p) {
        Reader rc{file, ctts_p + ctts_size, ctts_p};
        if (ReadFullBoxHeader(&rc, &v, &f)) {
            uint32_t ctts_entries;
            if (rc.U32(ctts_entries)) {
                uint32_t s_i = 0;
                for (uint32_t e = 0; e < ctts_entries && s_i < sample_count; e++) {
                    uint32_t cnt, off;
                    if (!rc.U32(cnt) || !rc.U32(off)) break;
                    for (uint32_t k = 0; k < cnt && s_i < sample_count; k++) {
                        // ctts offsets in v0 are unsigned; v1 uses signed.
                        // either way they're added to dts.
                        samples[s_i].pts = samples[s_i].dts + off;
                        s_i++;
                    }
                }
            }
        }
    }

    // ---- stss: sync samples (keyframes) ---------------------------------
    if (stss_p) {
        Reader rk{file, stss_p + stss_size, stss_p};
        if (ReadFullBoxHeader(&rk, &v, &f)) {
            uint32_t cnt;
            if (rk.U32(cnt)) {
                for (uint32_t i = 0; i < cnt; i++) {
                    uint32_t one_based;
                    if (!rk.U32(one_based)) break;
                    if (one_based >= 1 && one_based <= sample_count) {
                        samples[one_based - 1].is_keyframe = true;
                    }
                }
            }
        }
    } else {
        // no stss => every sample is a sync sample (typical for audio)
        for (uint32_t i = 0; i < sample_count; i++)
            samples[i].is_keyframe = true;
    }

    KernelHeap::Free(chunk_off);
    tr->samples      = samples;
    tr->sample_count = sample_count;
    return true;

fail:
    KernelHeap::Free(samples);
    KernelHeap::Free(chunk_off);
    return false;
}

} // anon namespace

// =========================================================================
// public api
// =========================================================================

bool Open(const uint8_t* data, uint32_t size, Movie& mv) {
    mv = Movie{};
    if (!data || size < 16) return false;

    // walk top-level boxes for ftyp + moov
    Reader r{data, data + size, data};
    bool saw_moov = false;
    const uint8_t* moov_payload = nullptr;
    uint32_t       moov_payload_len = 0;
    while (r.p < r.end) {
        BoxHeader bh;
        if (!ReadBoxHeader(&r, &bh)) break;
        if (bh.type == Box::ftyp) {
            Reader fr{data, bh.box_end, bh.payload_start};
            uint32_t major;
            if (fr.U32(major)) mv.major_brand = major;
        } else if (bh.type == Box::moov) {
            saw_moov         = true;
            moov_payload     = bh.payload_start;
            moov_payload_len = (uint32_t)bh.payload_size;
        }
        r.p = bh.box_end;
    }
    if (!saw_moov) return false;

    // first pass: parse moov tree → fill movie + tracks (no sample tables)
    ParserState st{};
    st.file_base = data;
    st.file_size = size;
    st.mv        = &mv;
    if (!ParseBoxList(&st, moov_payload, moov_payload + moov_payload_len)) {
        Close(mv);
        return false;
    }

    // second pass: build per-track sample tables from stbl
    for (int i = 0; i < mv.track_count; i++) {
        if (!BuildSampleTable(data, size, (uint32_t)i, &mv.tracks[i])) {
            // tolerate per-track failure  -  leave sample_count == 0
            mv.tracks[i].samples      = nullptr;
            mv.tracks[i].sample_count = 0;
        }
    }
    return true;
}

void Close(Movie& mv) {
    for (int i = 0; i < mv.track_count; i++) {
        if (mv.tracks[i].samples) {
            KernelHeap::Free(mv.tracks[i].samples);
            mv.tracks[i].samples      = nullptr;
            mv.tracks[i].sample_count = 0;
        }
    }
    mv.track_count = 0;
}

int FindFirstTrack(const Movie& mv, TrackKind kind) {
    for (int i = 0; i < mv.track_count; i++) {
        if (mv.tracks[i].kind == kind) return i;
    }
    return -1;
}

uint32_t DurationMs(const Movie& mv) {
    if (mv.timescale == 0) return 0;
    // (duration * 1000) / timescale, careful with overflow
    return (uint32_t)((mv.duration_units * 1000ull) / mv.timescale);
}

uint32_t TrackDurationMs(const Track& tr) {
    if (tr.timescale == 0) return 0;
    return (uint32_t)((tr.duration_units * 1000ull) / tr.timescale);
}

uint32_t SeekKeyframe(const Track& tr, uint64_t pts_units) {
    if (tr.sample_count == 0) return 0;
    uint32_t best = 0;
    for (uint32_t i = 0; i < tr.sample_count; i++) {
        if (tr.samples[i].pts > pts_units) break;
        if (tr.samples[i].is_keyframe) best = i;
    }
    return best;
}

uint64_t MsToUnits(const Track& tr, uint32_t ms) {
    return ((uint64_t)ms * tr.timescale) / 1000ull;
}

const char* CodecName(uint32_t fourcc) {
    switch (fourcc) {
        case Box::avc1: return "h264 (avc1)";
        case Box::hvc1: return "h265 (hvc1)";
        case Box::hev1: return "h265 (hev1)";
        case Box::mjpg: return "mjpeg";
        case Box::jpeg: return "jpeg";
        case Box::mp4a: return "aac (mp4a)";
        case Box::Opus: return "opus";
        case Box::alaw: return "a-law pcm";
        case Box::ulaw: return "u-law pcm";
        case Box::kvid: return "kurono kvid";
        default:        return "unknown";
    }
}

const char* HandlerName(uint32_t fourcc) {
    switch (fourcc) {
        case Handler::vide: return "video";
        case Handler::soun: return "audio";
        case Handler::hint: return "hint";
        case Handler::subt: return "subtitle";
        case Handler::text: return "text";
        default:            return "other";
    }
}

void DumpToSerial(const Movie& mv) {
    SerialLogger::Log("[MP4] dump: tracks=");
    SerialLogger::LogDec(mv.track_count);
    SerialLogger::Log(" timescale=");
    SerialLogger::LogDec((int)mv.timescale);
    SerialLogger::Log(" duration_ms=");
    SerialLogger::LogDec((int)DurationMs(mv));
    SerialLogger::Log("\r\n");
    for (int i = 0; i < mv.track_count; i++) {
        const Track& t = mv.tracks[i];
        SerialLogger::Log("[MP4]  track ");
        SerialLogger::LogDec(i);
        SerialLogger::Log(": id=");
        SerialLogger::LogDec((int)t.track_id);
        SerialLogger::Log(" kind=");
        SerialLogger::Log(HandlerName(t.handler));
        SerialLogger::Log(" codec=");
        SerialLogger::Log(CodecName(t.codec));
        SerialLogger::Log(" samples=");
        SerialLogger::LogDec((int)t.sample_count);
        if (t.kind == TRACK_VIDEO) {
            SerialLogger::Log(" ");
            SerialLogger::LogDec(t.video.width);
            SerialLogger::Log("x");
            SerialLogger::LogDec(t.video.height);
        } else if (t.kind == TRACK_AUDIO) {
            SerialLogger::Log(" ");
            SerialLogger::LogDec(t.audio.sample_rate);
            SerialLogger::Log("Hz x");
            SerialLogger::LogDec(t.audio.channels);
            SerialLogger::Log("ch ");
            SerialLogger::LogDec(t.audio.sample_size);
            SerialLogger::Log("-bit");
        }
        SerialLogger::Log("\r\n");
    }
}

} // namespace MP4
