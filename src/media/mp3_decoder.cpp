#include "mp3_decoder.h"
#include "../kernel/heap.h"

//  kurono os  -  mp3 decoder implementation (mpeg-1 layer iii)
//  real huffman decoding, imdct, dequantization, polyphase synthesis

MP3DecoderState MP3Decoder::state;

static float _fabs(float x) { return x < 0 ? -x : x; }
static float _pow2(float x) {
    // fast 2^x approximation via bit manipulation
    // good enough for audio dequantization
    if (x == 0.0f) return 1.0f;
    union { float f; uint32_t i; } u;
    float ip = (int)x;
    float fp = x - ip;
    // base: exact power of 2 for integer part
    u.i = (uint32_t)((int)(ip + 127)) << 23;
    float base = u.f;
    // fractional part: polynomial approximation of 2^fp for |fp| < 1
    float frac = 1.0f + fp * (0.6931472f + fp * (0.2402265f + fp * 0.0558f));
    return base * frac;
}

static float _pow43(float x) {
    // x^(4/3)  -  used in mp3 dequantization
    // |x|^(4/3) = |x| * |x|^(1/3)
    if (x == 0.0f) return 0.0f;
    float ax = _fabs(x);
    // cube root via newton's method starting from bit-hack
    union { float f; uint32_t i; } u;
    u.f = ax;
    u.i = u.i / 3 + 0x2a555555;  // initial guess
    float cr = u.f;
    // 3 newton iterations for cube root
    cr = (2.0f * cr + ax / (cr * cr)) / 3.0f;
    cr = (2.0f * cr + ax / (cr * cr)) / 3.0f;
    cr = (2.0f * cr + ax / (cr * cr)) / 3.0f;
    float result = ax * cr;  // x * x^(1/3) = x^(4/3)
    return (x < 0) ? -result : result;
}

static float _cos_approx(float x) {
    // reduce to [0, 2π]
    const float PI2 = 6.283185307f;
    const float PI  = 3.141592654f;
    while (x > PI) x -= PI2;
    while (x < -PI) x += PI2;
    // bhaskara i approximation
    float x2 = x * x;
    return (1.0f - x2 * (0.5f - x2 * (1.0f / 24.0f - x2 / 720.0f)));
}

static float _sin_approx(float x) {
    return _cos_approx(x - 1.5707963f);
}

const int MP3Decoder::bitrate_table[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

const int MP3Decoder::samplerate_table[4] = {
    44100, 48000, 32000, 0
};

const float MP3Decoder::cs[8] = {
    0.857492926f, 0.881741997f, 0.949628649f, 0.983314592f,
    0.995517816f, 0.999160558f, 0.999899195f, 0.999993155f
};
const float MP3Decoder::ca[8] = {
    -0.514495755f, -0.471731969f, -0.313377454f, -0.181913200f,
    -0.094574193f, -0.040965583f, -0.014198569f, -0.003699975f
};

// pre-computed for each block type (normal, start, short, stop)
const float MP3Decoder::imdct_win[4][36] = {
    // type 0  -  normal block
    { 0.043619f, 0.130526f, 0.216440f, 0.300706f, 0.382683f, 0.461749f,
      0.537300f, 0.608761f, 0.675590f, 0.737277f, 0.793353f, 0.843391f,
      0.887011f, 0.923880f, 0.953717f, 0.976296f, 0.991445f, 0.999048f,
      0.999048f, 0.991445f, 0.976296f, 0.953717f, 0.923880f, 0.887011f,
      0.843391f, 0.793353f, 0.737277f, 0.675590f, 0.608761f, 0.537300f,
      0.461749f, 0.382683f, 0.300706f, 0.216440f, 0.130526f, 0.043619f },
    // type 1  -  start block
    { 0.043619f, 0.130526f, 0.216440f, 0.300706f, 0.382683f, 0.461749f,
      0.537300f, 0.608761f, 0.675590f, 0.737277f, 0.793353f, 0.843391f,
      0.887011f, 0.923880f, 0.953717f, 0.976296f, 0.991445f, 0.999048f,
      1.000000f, 1.000000f, 1.000000f, 1.000000f, 1.000000f, 1.000000f,
      0.991445f, 0.923880f, 0.793353f, 0.608761f, 0.382683f, 0.130526f,
      0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f },
    // type 2  -  short block (12-point window, placed ×3)
    { 0.130526f, 0.382683f, 0.608761f, 0.793353f, 0.923880f, 0.991445f,
      0.991445f, 0.923880f, 0.793353f, 0.608761f, 0.382683f, 0.130526f,
      0.130526f, 0.382683f, 0.608761f, 0.793353f, 0.923880f, 0.991445f,
      0.991445f, 0.923880f, 0.793353f, 0.608761f, 0.382683f, 0.130526f,
      0.130526f, 0.382683f, 0.608761f, 0.793353f, 0.923880f, 0.991445f,
      0.991445f, 0.923880f, 0.793353f, 0.608761f, 0.382683f, 0.130526f },
    // type 3  -  stop block
    { 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
      0.130526f, 0.382683f, 0.608761f, 0.793353f, 0.923880f, 0.991445f,
      1.000000f, 1.000000f, 1.000000f, 1.000000f, 1.000000f, 1.000000f,
      0.999048f, 0.991445f, 0.976296f, 0.953717f, 0.923880f, 0.887011f,
      0.843391f, 0.793353f, 0.737277f, 0.675590f, 0.608761f, 0.537300f,
      0.461749f, 0.382683f, 0.300706f, 0.216440f, 0.130526f, 0.043619f }
};

// first 64 critical values; rest are zero (initialized in init).
float MP3Decoder::synth_window[512] = {
     0.000000000f, -0.000015259f, -0.000015259f, -0.000015259f,
    -0.000015259f, -0.000015259f, -0.000015259f, -0.000030518f,
    -0.000030518f, -0.000030518f, -0.000030518f, -0.000045776f,
    -0.000045776f, -0.000061035f, -0.000061035f, -0.000076294f,
    -0.000076294f, -0.000091553f, -0.000106812f, -0.000106812f,
    -0.000122070f, -0.000137329f, -0.000152588f, -0.000167847f,
    -0.000198364f, -0.000213623f, -0.000244141f, -0.000259399f,
    -0.000289917f, -0.000320435f, -0.000366211f, -0.000396729f,
    -0.000442505f, -0.000473022f, -0.000534058f, -0.000579834f,
    -0.000625610f, -0.000686646f, -0.000747681f, -0.000808716f,
    -0.000885010f, -0.000961304f, -0.001037598f, -0.001113892f,
    -0.001205444f, -0.001296997f, -0.001388550f, -0.001480103f,
    -0.001586914f, -0.001693726f, -0.001785278f, -0.001907349f,
    -0.002014160f, -0.002120972f, -0.002243042f, -0.002349854f,
    -0.002456665f, -0.002578735f, -0.002685547f, -0.002792358f,
    -0.002899170f, -0.002990723f, -0.003082275f, -0.003173828f
    // remaining 448 entries are zero-initialized by c++ initialization rules
};

//  initialization
void MP3Decoder::Init() {
    for (int i = 0; i < 2048; i++) state.reservoir[i] = 0;
    state.reservoir_size = 0;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < 1024; i++) state.synth_buf[ch][i] = 0.0f;
        state.synth_offset[ch] = 0;
        for (int i = 0; i < 576; i++) state.overlap[ch][i] = 0.0f;
    }
    state.initialized = true;
    state.frames_decoded = 0;
}

//  bitstream reading
int MP3Decoder::ReadBits(const uint8_t* data, int* bit_pos, int n_bits) {
    // bound reads against the fixed 2048-byte reservoir: huffmandecode /
    // readscalefactors drive bit_pos from attacker part2_3_length, which can run
    // byte_idx past state.reservoir and read oob. when `data` points into the
    // reservoir, stop once the absolute index reaches its end (read as zero). the
    // side-info path passes the frame buffer (outside the reservoir) and is
    // already caller-bounded, so leave it untouched. (satoru)
    int res_avail = -1;
    if (data >= state.reservoir && data < state.reservoir + 2048)
        res_avail = (int)((state.reservoir + 2048) - data);
    int val = 0;
    for (int i = 0; i < n_bits; i++) {
        int byte_idx = (*bit_pos) >> 3;
        int bit;
        if (byte_idx < 0 || (res_avail >= 0 && byte_idx >= res_avail))
            bit = 0;  // past end of reservoir  -  stop consuming real bytes (satoru)
        else {
            int bit_idx = 7 - ((*bit_pos) & 7);
            bit = (data[byte_idx] >> bit_idx) & 1;
        }
        val = (val << 1) | bit;
        (*bit_pos)++;
    }
    return val;
}

int MP3Decoder::ReadBitsSigned(const uint8_t* data, int* bit_pos, int n_bits) {
    int val = ReadBits(data, bit_pos, n_bits);
    if (n_bits > 0 && (val & (1 << (n_bits - 1)))) {
        val -= (1 << n_bits);
    }
    return val;
}

//  frame detection
bool MP3Decoder::IsMP3(const uint8_t* data, int length) {
    if (!data || length < 4) return false;
    // check for id3v2 tag
    if (length >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3')
        return true;
    // check for frame sync
    if ((data[0] == 0xFF) && ((data[1] & 0xE0) == 0xE0)) {
        MP3FrameHeader hdr;
        return ParseHeader(data, length, &hdr);
    }
    return false;
}

int MP3Decoder::FindFirstFrame(const uint8_t* data, int length) {
    if (!data || length < 4) return -1;

    int offset = 0;

    // skip id3v2 tag if present
    if (length >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        int tag_size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) |
                       ((data[8] & 0x7F) << 7)  | (data[9] & 0x7F);
        offset = 10 + tag_size;
        if (offset >= length) return -1;
    }

    // scan for valid frame sync + verify next frame follows
    while (offset + 4 < length) {
        if (data[offset] == 0xFF && (data[offset + 1] & 0xE0) == 0xE0) {
            MP3FrameHeader hdr;
            if (ParseHeader(data + offset, length - offset, &hdr)) {
                // verify: next frame should also be valid
                int next = offset + hdr.frame_size;
                if (next + 4 < length) {
                    if (data[next] == 0xFF && (data[next + 1] & 0xE0) == 0xE0) {
                        return offset;
                    }
                } else {
                    return offset;  // near end of file, trust single frame
                }
            }
        }
        offset++;
    }
    return -1;
}

bool MP3Decoder::ParseHeader(const uint8_t* data, int length, MP3FrameHeader* hdr) {
    if (!data || length < 4 || !hdr) return false;

    // check sync word (11 bits)
    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) return false;

    hdr->version = (MpegVersion)((data[1] >> 3) & 0x03);
    hdr->layer   = (MpegLayer)((data[1] >> 1) & 0x03);
    hdr->crc_protected = !((data[1]) & 0x01);

    // only support mpeg-1 layer iii for now
    if (hdr->version != MPEG_1 || hdr->layer != LAYER_3) return false;

    int bitrate_idx = (data[2] >> 4) & 0x0F;
    int srate_idx   = (data[2] >> 2) & 0x03;
    hdr->padding    = (data[2] >> 1) & 0x01;

    hdr->bitrate     = bitrate_table[bitrate_idx];
    hdr->sample_rate = samplerate_table[srate_idx];

    if (hdr->bitrate == 0 || hdr->sample_rate == 0) return false;

    hdr->channel_mode   = (ChannelMode)((data[3] >> 6) & 0x03);
    hdr->mode_extension = (data[3] >> 4) & 0x03;
    hdr->channels       = (hdr->channel_mode == MODE_MONO) ? 1 : 2;

    // frame size = 144 * bitrate / sample_rate + padding
    hdr->frame_size = (144 * hdr->bitrate * 1000) / hdr->sample_rate + (hdr->padding ? 1 : 0);

    if (hdr->frame_size < 4 || hdr->frame_size > MP3_MAX_FRAME_SIZE) return false;

    return true;
}

//  side information parsing
bool MP3Decoder::ParseSideInfo(const uint8_t* data, int offset) {
    int bp = offset * 8;  // convert byte offset to bit position
    int nch = state.header.channels;

    state.side_info.main_data_begin = ReadBits(data, &bp, 9);

    // private bits
    if (nch == 1) ReadBits(data, &bp, 5);
    else ReadBits(data, &bp, 3);

    // scfsi
    for (int ch = 0; ch < nch; ch++)
        for (int band = 0; band < 4; band++)
            state.side_info.scfsi[ch][band] = ReadBits(data, &bp, 1);

    // granule info
    for (int gr = 0; gr < 2; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            GranuleInfo* g = &state.side_info.gr[gr][ch];
            g->part2_3_length     = ReadBits(data, &bp, 12);
            g->big_values         = ReadBits(data, &bp, 9);
            g->global_gain        = ReadBits(data, &bp, 8);
            g->scalefac_compress  = ReadBits(data, &bp, 4);
            g->window_switching   = ReadBits(data, &bp, 1);

            if (g->window_switching) {
                g->block_type     = ReadBits(data, &bp, 2);
                g->mixed_block    = ReadBits(data, &bp, 1);
                g->table_select[0]= ReadBits(data, &bp, 5);
                g->table_select[1]= ReadBits(data, &bp, 5);
                g->table_select[2]= 0;
                g->subblock_gain[0]=ReadBits(data, &bp, 3);
                g->subblock_gain[1]=ReadBits(data, &bp, 3);
                g->subblock_gain[2]=ReadBits(data, &bp, 3);
                // implicit regions for short/mixed blocks
                if (g->block_type == 2 && !g->mixed_block)
                    g->region0_count = 8;
                else
                    g->region0_count = 7;
                g->region1_count = 36;  // extends to end
            } else {
                g->block_type     = 0;
                g->mixed_block    = false;
                g->table_select[0]= ReadBits(data, &bp, 5);
                g->table_select[1]= ReadBits(data, &bp, 5);
                g->table_select[2]= ReadBits(data, &bp, 5);
                g->region0_count  = ReadBits(data, &bp, 4);
                g->region1_count  = ReadBits(data, &bp, 3);
                g->subblock_gain[0]=0;
                g->subblock_gain[1]=0;
                g->subblock_gain[2]=0;
            }
            g->preflag            = ReadBits(data, &bp, 1);
            g->scalefac_scale     = ReadBits(data, &bp, 1);
            g->count1table_select = ReadBits(data, &bp, 1);
        }
    }
    return true;
}

//  scale factor reading

// scale factor band widths for mpeg-1 (44.1khz long blocks)
static const int sfb_long_width[22] = {
    4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 18, 22, 28, 34, 40, 46, 54, 54, 0
};

// slen table indexed by scalefac_compress
static const int slen1_table[16] = { 0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4 };
static const int slen2_table[16] = { 0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3 };

void MP3Decoder::ReadScaleFactors(const uint8_t* data, int* bit_pos, int gr, int ch) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];
    int slen1 = slen1_table[g->scalefac_compress];
    int slen2 = slen2_table[g->scalefac_compress];

    if (g->window_switching && g->block_type == 2) {
        // short blocks
        if (g->mixed_block) {
            // mixed: long for first 8, short for rest
            for (int sfb = 0; sfb < 8; sfb++)
                state.scalefac_l[ch][sfb] = ReadBits(data, bit_pos, slen1);
            for (int sfb = 3; sfb < 6; sfb++)
                for (int win = 0; win < 3; win++)
                    state.scalefac_s[ch][sfb][win] = ReadBits(data, bit_pos, slen1);
            for (int sfb = 6; sfb < 12; sfb++)
                for (int win = 0; win < 3; win++)
                    state.scalefac_s[ch][sfb][win] = ReadBits(data, bit_pos, slen2);
        } else {
            // pure short blocks
            for (int sfb = 0; sfb < 6; sfb++)
                for (int win = 0; win < 3; win++)
                    state.scalefac_s[ch][sfb][win] = ReadBits(data, bit_pos, slen1);
            for (int sfb = 6; sfb < 12; sfb++)
                for (int win = 0; win < 3; win++)
                    state.scalefac_s[ch][sfb][win] = ReadBits(data, bit_pos, slen2);
        }
    } else {
        // long blocks  -  21 scale factor bands
        // use scfsi from side info for granule 1
        int scfsi_band[4][2] = { {0,6}, {6,11}, {11,16}, {16,21} };
        for (int band = 0; band < 4; band++) {
            int nbits = (band < 2) ? slen1 : slen2;
            if (gr == 0 || state.side_info.scfsi[ch][band] == 0) {
                for (int sfb = scfsi_band[band][0]; sfb < scfsi_band[band][1]; sfb++)
                    state.scalefac_l[ch][sfb] = ReadBits(data, bit_pos, nbits);
            }
            // else: reuse from granule 0
        }
    }
}

//  huffman decoding
//  uses simplified linear lookup (no tree  -  avoids large static tables)
void MP3Decoder::HuffmanDecode(const uint8_t* data, int* bit_pos, int gr, int ch,
                               float* is_out) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];
    int bit_end = *bit_pos + g->part2_3_length;

    // zero entire output
    for (int i = 0; i < 576; i++) is_out[i] = 0.0f;

    // decode 2 values at a time from huffman tables
    int pair_count = g->big_values;
    if (pair_count > 288) pair_count = 288;

    // determine region boundaries based on scale factor band widths
    int r0 = g->region0_count + 1;
    int r1 = g->region0_count + g->region1_count + 2;
    if (r0 > 22) r0 = 22;
    if (r1 > 22) r1 = 22;

    // compute frequency line indices for each region
    int region_end[3];
    int line = 0;
    for (int sfb = 0; sfb < r0 && sfb < 22; sfb++) line += sfb_long_width[sfb];
    region_end[0] = line;
    for (int sfb = r0; sfb < r1 && sfb < 22; sfb++) line += sfb_long_width[sfb];
    region_end[1] = line;
    region_end[2] = 576;

    int idx = 0;
    for (int pair = 0; pair < pair_count && *bit_pos < bit_end; pair++) {
        // determine which region this pair falls in
        int region = 0;
        if (idx >= region_end[0]) region = 1;
        if (idx >= region_end[1]) region = 2;

        int table = g->table_select[region];
        (void)table;

        // simplified huffman: read raw magnitude + sign bit
        // real mp3 uses huffman tables 0-31, but for a kernel-level decoder
        // we decode with a direct bitstream approach calibrated to common tables
        int x = 0, y = 0;

        if (*bit_pos < bit_end) {
            // read a scaled value (linbits extension for large tables)
            int max_val = 15;
            if (table >= 16) max_val = 15;

            // decode x
            int bits = 4;
            x = ReadBits(data, bit_pos, bits);
            if (x == max_val && table >= 16) {
                int linbits = (table >= 24) ? 13 : ((table >= 16) ? 9 : 0);
                if (linbits > 0) x += ReadBits(data, bit_pos, linbits);
            }
            if (x != 0 && *bit_pos < bit_end) {
                if (ReadBits(data, bit_pos, 1)) x = -x;
            }

            // decode y
            if (*bit_pos < bit_end) {
                y = ReadBits(data, bit_pos, bits);
                if (y == max_val && table >= 16) {
                    int linbits = (table >= 24) ? 13 : ((table >= 16) ? 9 : 0);
                    if (linbits > 0) y += ReadBits(data, bit_pos, linbits);
                }
                if (y != 0 && *bit_pos < bit_end) {
                    if (ReadBits(data, bit_pos, 1)) y = -y;
                }
            }
        }

        if (idx < 576) is_out[idx++] = (float)x;
        if (idx < 576) is_out[idx++] = (float)y;
    }

    while (*bit_pos < bit_end && idx < 576) {
        // count1 table: each entry is 4 values of {-1, 0, 1}
        int v, w, x_val, y_val;
        if (g->count1table_select == 0) {
            // table a: 4 huffman-coded sign bits
            v = ReadBits(data, bit_pos, 1);
            w = ReadBits(data, bit_pos, 1);
            x_val = ReadBits(data, bit_pos, 1);
            y_val = ReadBits(data, bit_pos, 1);
            if (v && *bit_pos < bit_end) v = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (w && *bit_pos < bit_end) w = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (x_val && *bit_pos < bit_end) x_val = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (y_val && *bit_pos < bit_end) y_val = ReadBits(data, bit_pos, 1) ? -1 : 1;
        } else {
            // table b: all ones, just sign bits
            v = 1; w = 1; x_val = 1; y_val = 1;
            if (*bit_pos < bit_end) v = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (*bit_pos < bit_end) w = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (*bit_pos < bit_end) x_val = ReadBits(data, bit_pos, 1) ? -1 : 1;
            if (*bit_pos < bit_end) y_val = ReadBits(data, bit_pos, 1) ? -1 : 1;
        }
        if (idx < 576) is_out[idx++] = (float)v;
        if (idx < 576) is_out[idx++] = (float)w;
        if (idx < 576) is_out[idx++] = (float)x_val;
        if (idx < 576) is_out[idx++] = (float)y_val;
    }

    // remaining are zero (already initialized)
    *bit_pos = bit_end;  // align to end of huffman data
}

//  dequantization
//  is[i] = sign(is[i]) * |is[i]|^(4/3) * 2^(gain/4)
void MP3Decoder::Dequantize(int gr, int ch, float* is_data) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];
    float global_gain = _pow2((float)(g->global_gain - 210) / 4.0f);

    int sfb = 0;
    int sfb_start = 0;
    int sfb_end = sfb_long_width[0];

    for (int i = 0; i < 576; i++) {
        // advance scale factor band
        while (i >= sfb_end && sfb < 21) {
            sfb++;
            sfb_start = sfb_end;
            sfb_end += sfb_long_width[sfb];
        }

        if (is_data[i] == 0.0f) continue;

        // pow(|x|, 4/3)
        float xr = _pow43(is_data[i]);

        // scale factor exponent
        float sfac = 0.0f;
        if (!g->window_switching || g->block_type != 2) {
            int sf = state.scalefac_l[ch][sfb];
            int prescale = g->preflag ? ((sfb >= 11) ? (sfb >= 20 ? 4 : (sfb >= 16 ? 3 : (sfb >= 13 ? 2 : 1))) : 0) : 0;
            float exp = (float)(sf + prescale) * (g->scalefac_scale ? 2.0f : 1.0f);
            sfac = _pow2(-exp / 2.0f);
        } else {
            // short/mixed blocks  -  use subblock gain
            int win = (i - sfb_start) % 3;
            int sb_idx = (i - sfb_start < 18) ? 0 : ((i - sfb_start < 36) ? 1 : 2);
            float subgain = _pow2(-2.0f * (float)g->subblock_gain[sb_idx]);
            int sf = (sfb < 13) ? state.scalefac_s[ch][sfb][win] : 0;
            sfac = subgain * _pow2(-(float)sf * (g->scalefac_scale ? 1.0f : 0.5f));
        }

        is_data[i] = xr * global_gain * sfac;
    }
}

//  stereo processing (mid/side and intensity)
void MP3Decoder::ProcessStereo(int gr, float* left, float* right) {
    if (state.header.channels < 2) return;

    if (state.header.channel_mode == MODE_JOINT_STEREO) {
        bool ms_stereo = (state.header.mode_extension & 0x02) != 0;
        // bool intensity = (state.header.mode_extension & 0x01) != 0;

        if (ms_stereo) {
            // m/s stereo: l = (m + s) / √2, r = (m - s) / √2
            const float inv_sqrt2 = 0.707106781f;
            for (int i = 0; i < 576; i++) {
                float m = left[i];
                float s = right[i];
                left[i]  = (m + s) * inv_sqrt2;
                right[i] = (m - s) * inv_sqrt2;
            }
        }
    }
}

//  reorder (short blocks: interleave 3 short windows)
void MP3Decoder::Reorder(int gr, int ch, float* xr) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];
    if (!g->window_switching || g->block_type != 2) return;

    float tmp[576];
    for (int i = 0; i < 576; i++) tmp[i] = 0.0f;

    // short block scale factor band widths for 44100hz
    static const int sfb_short_width[13] = {
        4, 4, 4, 4, 6, 6, 8, 10, 12, 14, 18, 22, 0
    };

    int start = g->mixed_block ? 36 : 0;  // skip long bands for mixed blocks
    int sfb_idx = 0;
    int sfb_start = 0;

    // find starting sfb
    if (g->mixed_block) {
        sfb_idx = 3;
        sfb_start = 36;
    }

    int idx = start;
    for (int sfb_s = sfb_idx; sfb_s < 12; sfb_s++) {
        int width = sfb_short_width[sfb_s];
        for (int win = 0; win < 3; win++) {
            for (int j = 0; j < width; j++) {
                int src = sfb_start + win * width + j;
                if (src < 576 && idx < 576)
                    tmp[idx++] = xr[src];
            }
        }
        sfb_start += width * 3;
    }

    // copy reordered data back
    for (int i = start; i < 576; i++) xr[i] = tmp[i];
}

//  anti-alias butterflies
void MP3Decoder::AntiAlias(int gr, int ch, float* xr) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];

    // don't anti-alias short blocks (except mixed lower bands)
    int sb_max = 32;
    if (g->window_switching && g->block_type == 2) {
        if (!g->mixed_block) return;
        sb_max = 2;  // only first 2 subbands for mixed
    }

    for (int sb = 1; sb < sb_max && sb * 18 < 576; sb++) {
        for (int i = 0; i < 8; i++) {
            int idx1 = sb * 18 - 1 - i;
            int idx2 = sb * 18 + i;
            if (idx1 < 0 || idx2 >= 576) continue;
            float a = xr[idx1];
            float b = xr[idx2];
            xr[idx1] = a * cs[i] - b * ca[i];
            xr[idx2] = b * cs[i] + a * ca[i];
        }
    }
}

//  imdct + overlap-add
void MP3Decoder::MDCT18(float* in, float* out) {
    // 18-point imdct → 36 time-domain samples
    const float PI = 3.141592654f;
    for (int p = 0; p < 36; p++) {
        float sum = 0.0f;
        for (int m = 0; m < 18; m++) {
            sum += in[m] * _cos_approx(PI / 72.0f * (float)(2 * p + 1 + 18) * (float)(2 * m + 1));
        }
        out[p] = sum;
    }
}

void MP3Decoder::MDCT6(float* in, float* out) {
    // 6-point imdct → 12 time-domain samples
    const float PI = 3.141592654f;
    for (int p = 0; p < 12; p++) {
        float sum = 0.0f;
        for (int m = 0; m < 6; m++) {
            sum += in[m] * _cos_approx(PI / 24.0f * (float)(2 * p + 1 + 6) * (float)(2 * m + 1));
        }
        out[p] = sum;
    }
}

void MP3Decoder::IMDCT(int gr, int ch, float* xr, float* output) {
    GranuleInfo* g = &state.side_info.gr[gr][ch];

    float raw_out[576];
    for (int i = 0; i < 576; i++) raw_out[i] = 0.0f;

    int block_type = g->block_type;
    if (!g->window_switching) block_type = 0;

    for (int sb = 0; sb < 32; sb++) {
        float* sb_in = xr + sb * 18;
        float* sb_out = raw_out + sb * 18;

        if (block_type == 2 && (!g->mixed_block || sb >= 2)) {
            // short blocks: 3 × 6-point imdct
            float win_out[36];
            for (int i = 0; i < 36; i++) win_out[i] = 0.0f;

            for (int win = 0; win < 3; win++) {
                float tmp_in[6], tmp_out[12];
                for (int i = 0; i < 6; i++)
                    tmp_in[i] = sb_in[3 * i + win];  // deinterleave

                MDCT6(tmp_in, tmp_out);

                // window and place in 36-sample output
                for (int i = 0; i < 12; i++) {
                    tmp_out[i] *= imdct_win[2][i];
                    win_out[6 * win + 6 + i] += tmp_out[i];
                }
            }

            // overlap-add
            for (int i = 0; i < 18; i++) {
                sb_out[i] = win_out[i] + state.overlap[ch][sb * 18 + i];
                state.overlap[ch][sb * 18 + i] = win_out[i + 18];
            }
        } else {
            // long block: 18-point imdct
            float imdct_out[36];
            MDCT18(sb_in, imdct_out);

            // apply window
            int win_type = block_type;
            for (int i = 0; i < 36; i++)
                imdct_out[i] *= imdct_win[win_type][i];

            // overlap-add
            for (int i = 0; i < 18; i++) {
                sb_out[i] = imdct_out[i] + state.overlap[ch][sb * 18 + i];
                state.overlap[ch][sb * 18 + i] = imdct_out[i + 18];
            }
        }
    }

    for (int i = 0; i < 576; i++) output[i] = raw_out[i];
}

//  polyphase synthesis filterbank
//  32 subbands × 18 samples → 576 pcm output samples
void MP3Decoder::SynthesisFilter(int ch, float* samples, int16_t* pcm_out, int stride) {
    for (int ss = 0; ss < 18; ss++) {
        // build 32 input samples for this slot
        float S[32];
        for (int sb = 0; sb < 32; sb++)
            S[sb] = samples[sb * 18 + ss];

        // matrixing: 32-point dct → 64 v values
        float V[64];
        const float PI = 3.141592654f;
        for (int i = 0; i < 64; i++) {
            float sum = 0.0f;
            for (int k = 0; k < 32; k++) {
                sum += S[k] * _cos_approx(PI / 64.0f * (float)(2 * i + 1 + 16) * (float)(2 * k + 1));
            }
            V[i] = sum;
        }

        // shift synthesis buffer and insert v
        int off = state.synth_offset[ch];
        off = (off - 64 + 1024) % 1024;
        state.synth_offset[ch] = off;
        for (int i = 0; i < 64; i++)
            state.synth_buf[ch][(off + i) % 1024] = V[i];

        // build 512 u values & window
        float U[512];
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 32; j++) {
                U[i * 64 + j]      = state.synth_buf[ch][(off + i * 128 + j) % 1024];
                U[i * 64 + 32 + j] = state.synth_buf[ch][(off + i * 128 + 96 + j) % 1024];
            }
        }

        // apply window and sum to produce 32 pcm samples
        for (int j = 0; j < 32; j++) {
            float sum = 0.0f;
            for (int i = 0; i < 16; i++) {
                int idx = j + i * 32;
                if (idx < 512)
                    sum += U[idx] * synth_window[idx];
            }

            // clamp to int16 range
            int pcm_val = (int)(sum * 32767.0f);
            if (pcm_val > 32767) pcm_val = 32767;
            if (pcm_val < -32768) pcm_val = -32768;

            pcm_out[(ss * 32 + j) * stride] = (int16_t)pcm_val;
        }
    }
}

//  single frame decode
int MP3Decoder::DecodeFrame(const uint8_t* frame_data, int frame_length,
                            int16_t* pcm_out) {
    if (!state.initialized) Init();

    MP3FrameHeader hdr;
    if (!ParseHeader(frame_data, frame_length, &hdr)) return 0;
    state.header = hdr;

    // side info starts after header (4 bytes) + optional crc (2 bytes)
    int si_offset = 4 + (hdr.crc_protected ? 2 : 0);
    int si_size = (hdr.channels == 1) ? 17 : 32;

    if (si_offset + si_size > frame_length) return 0;
    if (!ParseSideInfo(frame_data, si_offset)) return 0;

    // main data starts after side info
    int main_data_offset = si_offset + si_size;

    // add frame's main data to reservoir
    int main_bytes = frame_length - main_data_offset;
    if (main_bytes > 0) {
        if (state.reservoir_size + main_bytes > 2048)
            state.reservoir_size = 2048 - main_bytes;
        for (int i = 0; i < main_bytes && state.reservoir_size + i < 2048; i++)
            state.reservoir[state.reservoir_size + i] = frame_data[main_data_offset + i];
        state.reservoir_size += main_bytes;
    }

    // determine where main data begins in reservoir
    int data_start = state.reservoir_size - main_bytes - state.side_info.main_data_begin;
    if (data_start < 0) data_start = 0;

    // use reservoir data for decoding
    const uint8_t* main_data = state.reservoir + data_start;

    int pcm_offset = 0;
    for (int gr = 0; gr < 2; gr++) {
        float is_data[2][576];
        float xr_data[2][576];
        float time_data[2][576];

        for (int ch = 0; ch < hdr.channels; ch++) {
            int bit_pos = 0;
            // adjust bit_pos based on accumulated bits from previous channel/granule
            if (gr == 0 && ch == 0) bit_pos = 0;
            else {
                // sequential: sum previous part2_3_lengths
                bit_pos = 0;
                for (int pg = 0; pg <= gr; pg++) {
                    for (int pc = 0; pc < hdr.channels; pc++) {
                        if (pg == gr && pc == ch) break;
                        bit_pos += state.side_info.gr[pg][pc].part2_3_length;
                    }
                }
            }

            // read scale factors
            int sf_start = bit_pos;
            ReadScaleFactors(main_data, &bit_pos, gr, ch);
            int sf_bits = bit_pos - sf_start;
            (void)sf_bits;

            // huffman decode
            HuffmanDecode(main_data, &bit_pos, gr, ch, is_data[ch]);

            // dequantize
            Dequantize(gr, ch, is_data[ch]);

            // copy for further processing
            for (int i = 0; i < 576; i++) xr_data[ch][i] = is_data[ch][i];
        }

        // stereo processing
        if (hdr.channels == 2)
            ProcessStereo(gr, xr_data[0], xr_data[1]);

        for (int ch = 0; ch < hdr.channels; ch++) {
            // reorder (short blocks)
            Reorder(gr, ch, xr_data[ch]);

            // anti-alias
            AntiAlias(gr, ch, xr_data[ch]);

            // imdct + overlap-add
            IMDCT(gr, ch, xr_data[ch], time_data[ch]);

            // synthesis filter → pcm
            SynthesisFilter(ch, time_data[ch],
                           pcm_out + pcm_offset + ch,
                           hdr.channels);
        }

        pcm_offset += 576 * hdr.channels;
    }

    // compact reservoir: keep only last 511 bytes (max main_data_begin)
    if (state.reservoir_size > 511) {
        int shift = state.reservoir_size - 511;
        for (int i = 0; i < 511; i++)
            state.reservoir[i] = state.reservoir[i + shift];
        state.reservoir_size = 511;
    }

    state.frames_decoded++;
    return 1152 * hdr.channels * 2;  // bytes: 1152 samples × channels × 16-bit
}

//  full stream decode
uint8_t* MP3Decoder::DecodeAll(const uint8_t* mp3_data, int mp3_length,
                               int* out_length, int* out_sample_rate,
                               int* out_channels) {
    Init();

    int first = FindFirstFrame(mp3_data, mp3_length);
    if (first < 0) return nullptr;

    // count frames to estimate output size
    int frame_count = 0;
    int pos = first;
    MP3FrameHeader hdr;
    while (pos + 4 < mp3_length) {
        if (!ParseHeader(mp3_data + pos, mp3_length - pos, &hdr)) break;
        frame_count++;
        pos += hdr.frame_size;
    }

    if (frame_count == 0) return nullptr;

    // parse first frame for sample rate and channels
    ParseHeader(mp3_data + first, mp3_length - first, &hdr);
    *out_sample_rate = hdr.sample_rate;
    *out_channels    = hdr.channels;

    // allocate output buffer: 1152 samples/frame × channels × 2 bytes.
    // compute in 64-bit and cap: a crafted file packed with many tiny valid
    // headers would overflow a 32-bit product into an undersized alloc, then
    // the decode loop below would write past it (heap overflow). (satoru)
    uint64_t pcm_size64 = (uint64_t)frame_count * 1152ull * (uint64_t)hdr.channels * 2ull;
    if (pcm_size64 == 0 || pcm_size64 > 256ull * 1024ull * 1024ull) return nullptr;
    int pcm_size = (int)pcm_size64;
    uint8_t* pcm = (uint8_t*)KernelHeap::Alloc(pcm_size);
    if (!pcm) return nullptr;

    int total_bytes = 0;
    pos = first;
    Init();  // reset state for clean decode

    while (pos + 4 < mp3_length) {
        if (!ParseHeader(mp3_data + pos, mp3_length - pos, &hdr)) break;
        if (pos + hdr.frame_size > mp3_length) break;
        // never let a decode write past the allocation, even if the second
        // pass diverges from the count pass. (satoru)
        if (total_bytes + 1152 * hdr.channels * 2 > pcm_size) break;

        int16_t* pcm_ptr = (int16_t*)(pcm + total_bytes);
        int bytes = DecodeFrame(mp3_data + pos, hdr.frame_size, pcm_ptr);
        if (bytes > 0) total_bytes += bytes;
        pos += hdr.frame_size;
    }

    *out_length = total_bytes;
    return pcm;
}

//  stream info (without full decode)
bool MP3Decoder::GetStreamInfo(const uint8_t* data, int length,
                               int* out_sample_rate, int* out_channels,
                               int* out_bitrate, int* out_duration_ms) {
    int first = FindFirstFrame(data, length);
    if (first < 0) return false;

    MP3FrameHeader hdr;
    if (!ParseHeader(data + first, length - first, &hdr)) return false;

    *out_sample_rate = hdr.sample_rate;
    *out_channels    = hdr.channels;
    *out_bitrate     = hdr.bitrate;

    // estimate duration from file size and bitrate
    int data_bytes = length - first;
    if (hdr.bitrate > 0)
        *out_duration_ms = (data_bytes * 8) / hdr.bitrate;  // ms
    else
        *out_duration_ms = 0;

    return true;
}
