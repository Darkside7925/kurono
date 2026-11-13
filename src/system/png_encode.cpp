#include "png_encode.h"

namespace {

// crc32 (png chunk crc), table built on first use. (satoru)
uint32_t g_crc_table[256];
bool     g_crc_ready = false;

void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[n] = c;
    }
    g_crc_ready = true;
}

uint32_t crc_update(uint32_t crc, const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// lsb-first bit writer, as deflate wants. (satoru)
struct Bits {
    uint8_t* out;
    uint32_t pos, cap;
    uint32_t acc;
    int      n;
    bool     overflow;
    void put(uint32_t v, int bits) {
        acc |= v << n;
        n += bits;
        while (n >= 8) {
            if (pos < cap) out[pos++] = (uint8_t)acc; else overflow = true;
            acc >>= 8;
            n -= 8;
        }
    }
    // huffman codes go out msb-first. (satoru)
    void huff(uint32_t code, int bits) {
        uint32_t r = 0;
        for (int i = 0; i < bits; i++) { r = (r << 1) | (code & 1); code >>= 1; }
        put(r, bits);
    }
    void flush() { if (n > 0) put(0, 8 - n); }
};

// fixed huffman literal/length symbol. (satoru)
void put_sym(Bits& b, int sym) {
    if (sym < 144)      b.huff(0x30u + (uint32_t)sym, 8);
    else if (sym < 256) b.huff(0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) b.huff((uint32_t)(sym - 256), 7);
    else                b.huff(0xC0u + (uint32_t)(sym - 280), 8);
}

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                               67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void put_match(Bits& b, int len, int dist) {
    int li = 28;
    while (li > 0 && kLenBase[li] > len) li--;
    put_sym(b, 257 + li);
    if (kLenExtra[li]) b.put((uint32_t)(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (di > 0 && kDistBase[di] > dist) di--;
    b.huff((uint32_t)di, 5);
    if (kDistExtra[di]) b.put((uint32_t)(dist - kDistBase[di]), kDistExtra[di]);
}

int match_len(const uint8_t* d, uint32_t i, uint32_t n, uint32_t dist) {
    if (dist == 0 || dist > i || dist > 32768u) return 0;
    uint32_t max = n - i;
    if (max > 258) max = 258;
    uint32_t k = 0;
    while (k < max && d[i + k] == d[i + k - dist]) k++;
    return (int)k;
}

}  // namespace

namespace PngEncode {

uint32_t RawSize(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    return (uint32_t)h * (1u + 3u * (uint32_t)w);
}

uint32_t MaxSize(int w, int h) {
    uint32_t raw = RawSize(w, h);
    // 9 bits per literal at worst, plus signature, chunks and zlib framing. (satoru)
    return raw + raw / 8u + 256u;
}

uint32_t Encode(const uint8_t* src, int w, int h, uint32_t pitch, int src_bpp,
                uint8_t* raw, uint8_t* out, uint32_t out_cap) {
    if (!src || !raw || !out || w <= 0 || h <= 0) return 0;
    if (src_bpp != 32 && src_bpp != 24 && src_bpp != 16) return 0;
    if (out_cap < MaxSize(w, h)) return 0;
    if (!g_crc_ready) crc_init();

    // scanlines, filter type 0, rgb. (satoru)
    const uint32_t stride = 1u + 3u * (uint32_t)w;
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (uint64_t)y * pitch;
        uint8_t* d = raw + (uint64_t)y * stride;
        *d++ = 0;
        for (int x = 0; x < w; x++) {
            if (src_bpp == 32) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; s += 4;
            } else if (src_bpp == 24) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; s += 3;
            } else {
                uint16_t v = (uint16_t)(s[0] | (s[1] << 8)); s += 2;
                d[0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                d[1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                d[2] = (uint8_t)((v & 0x1F) << 3);
            }
            d += 3;
        }
    }
    const uint32_t n = stride * (uint32_t)h;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint32_t p = 0;
    for (int i = 0; i < 8; i++) out[p++] = sig[i];

    // IHDR (satoru)
    put_be32(out + p, 13); p += 4;
    uint32_t ihdr = p;
    out[p++] = 'I'; out[p++] = 'H'; out[p++] = 'D'; out[p++] = 'R';
    put_be32(out + p, (uint32_t)w); p += 4;
    put_be32(out + p, (uint32_t)h); p += 4;
    out[p++] = 8; out[p++] = 2; out[p++] = 0; out[p++] = 0; out[p++] = 0;
    put_be32(out + p, crc_update(0xFFFFFFFFu, out + ihdr, 17) ^ 0xFFFFFFFFu); p += 4;

    // IDAT: zlib header, one fixed-huffman block, adler32. (satoru)
    uint32_t idat_len_at = p; p += 4;
    uint32_t idat = p;
    out[p++] = 'I'; out[p++] = 'D'; out[p++] = 'A'; out[p++] = 'T';
    out[p++] = 0x78; out[p++] = 0x01;
    Bits b;
    b.out = out; b.pos = p; b.cap = out_cap - 24u; b.acc = 0; b.n = 0; b.overflow = false;
    b.put(1, 1);   // bfinal (satoru)
    b.put(1, 2);   // btype 01 = fixed huffman (satoru)
    uint32_t i = 0;
    while (i < n) {
        int best = 0, bd = 0;
        int l = match_len(raw, i, n, 3);
        if (l > best) { best = l; bd = 3; }
        l = match_len(raw, i, n, stride);
        if (l > best) { best = l; bd = (int)stride; }
        if (best >= 3) {
            put_match(b, best, bd);
            i += (uint32_t)best;
        } else {
            put_sym(b, raw[i]);
            i++;
        }
    }
    put_sym(b, 256);
    b.flush();
    if (b.overflow) return 0;
    p = b.pos;
    uint32_t s1 = 1, s2 = 0;
    for (uint32_t k = 0; k < n; ) {
        uint32_t chunk = n - k;
        if (chunk > 5552u) chunk = 5552u;
        for (uint32_t j = 0; j < chunk; j++) { s1 += raw[k + j]; s2 += s1; }
        s1 %= 65521u; s2 %= 65521u;
        k += chunk;
    }
    put_be32(out + p, (s2 << 16) | s1); p += 4;
    put_be32(out + idat_len_at, p - idat - 4u);
    put_be32(out + p, crc_update(0xFFFFFFFFu, out + idat, p - idat) ^ 0xFFFFFFFFu); p += 4;

    // IEND (satoru)
    put_be32(out + p, 0); p += 4;
    uint32_t iend = p;
    out[p++] = 'I'; out[p++] = 'E'; out[p++] = 'N'; out[p++] = 'D';
    put_be32(out + p, crc_update(0xFFFFFFFFu, out + iend, 4) ^ 0xFFFFFFFFu); p += 4;
    return p;
}

}  // namespace PngEncode

// end (satoru)
