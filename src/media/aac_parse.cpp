// kurono os - aac metadata parser
// see aac_parse.h.  integer-only, freestanding-clean.
#include "aac_parse.h"

namespace AAC {

static const uint32_t kSrTable[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0
};

static const uint8_t kChanCfgTable[8] = {
    0,  // 0 = defined elsewhere
    1,  // mono
    2,  // stereo
    3,  // 3.0
    4,  // 4.0
    5,  // 5.0
    6,  // 5.1
    8,  // 7.1
};

// tiny bit reader (for ASC; adts uses byte-aligned tricks)
struct BR {
    const uint8_t* d; uint32_t n; uint32_t pos;
    bool Empty() const { return pos >= n * 8; }
    uint32_t U1() {
        if (pos >= n * 8) return 0;
        uint32_t v = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
        pos++;
        return v;
    }
    uint32_t U(uint32_t k) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < k; i++) v = (v << 1) | U1();
        return v;
    }
};

static uint32_t ReadAOT(BR& br) {
    uint32_t aot = br.U(5);
    if (aot == 31) aot = 32 + br.U(6);
    return aot;
}

static uint32_t ReadFreqAndRate(BR& br, uint32_t* sr_index) {
    uint32_t idx = br.U(4);
    *sr_index = idx;
    if (idx == 0xf) return br.U(24);
    return (idx < 13) ? kSrTable[idx] : 0;
}

bool ParseConfig(const uint8_t* data, uint32_t size, Config& out) {
    if (!data || size < 2) return false;
    out = Config{};
    BR br{data, size, 0};
    uint32_t aot = ReadAOT(br);
    uint32_t sr_idx = 0;
    uint32_t sr = ReadFreqAndRate(br, &sr_idx);
    uint32_t chan_cfg = br.U(4);
    if (aot == 5 || aot == 29) {                    // SBR / PS extension
        out.sbr_present = true;
        if (aot == 29) out.ps_present = true;
        uint32_t ext_sr = ReadFreqAndRate(br, &sr_idx);
        if (ext_sr) sr = ext_sr;
        aot = ReadAOT(br);
    }
    out.audio_object_type = (uint8_t)aot;
    out.sample_rate       = sr;
    out.channels = (chan_cfg < 8) ? kChanCfgTable[chan_cfg] : 0;
    return out.sample_rate != 0 && out.audio_object_type != 0;
}

uint32_t ParseAdtsFrame(const uint8_t* d, uint32_t size, AdtsFrame& out) {
    if (size < 7) return 0;
    // sync word: 0xfff (12 bits)
    if (d[0] != 0xff || (d[1] & 0xf0) != 0xf0) return 0;
    out = AdtsFrame{};
    out.crc_present       = ((d[1] & 0x01) == 0);
    out.profile           = (uint8_t)(((d[2] >> 6) & 0x03) + 1); // aot
    out.freq_index        = (uint8_t)((d[2] >> 2) & 0x0f);
    out.sample_rate       = (out.freq_index < 13) ? kSrTable[out.freq_index] : 0;
    out.channel_config    = (uint8_t)(((d[2] & 0x01) << 2) | ((d[3] >> 6) & 0x03));
    uint32_t fl = ((uint32_t)(d[3] & 0x03) << 11) |
                  ((uint32_t)d[4]          <<  3) |
                  ((uint32_t)(d[5] >> 5)   &  0x07);
    out.frame_length      = (uint16_t)fl;
    out.buffer_fullness   = (uint16_t)(((d[5] & 0x1f) << 6) | (d[6] >> 2));
    out.num_raw_data_blocks = (uint8_t)((d[6] & 0x03) + 1);
    if (fl < 7) return 0;
    if (fl > size) return 0;
    return fl;
}

uint32_t WalkAdts(const uint8_t* data, uint32_t size,
                  bool (*cb)(const AdtsFrame&, const uint8_t*, uint32_t, void*),
                  void* user) {
    uint32_t i = 0, n = 0;
    while (i + 7 <= size) {
        AdtsFrame f;
        uint32_t consumed = ParseAdtsFrame(data + i, size - i, f);
        if (!consumed) { i++; continue; }
        uint32_t hdr = f.crc_present ? 9u : 7u;
        if (cb && f.frame_length > hdr) {
            uint32_t payload_len = f.frame_length - hdr;
            if (i + hdr + payload_len <= size) {
                if (!cb(f, data + i + hdr, payload_len, user)) return n + 1;
            }
        }
        i += f.frame_length;
        n++;
    }
    return n;
}

const char* ProfileName(uint8_t aot) {
    switch (aot) {
        case 1:  return "AAC Main";
        case 2:  return "AAC LC";
        case 3:  return "AAC SSR";
        case 4:  return "AAC LTP";
        case 5:  return "AAC SBR (HE-AAC)";
        case 29: return "AAC PS (HE-AACv2)";
        default: return "AAC";
    }
}

} // namespace AAC
