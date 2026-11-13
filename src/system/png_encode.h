#pragma once
#include "../kernel/types.h"

// png_encode - small freestanding png writer for screenshots. 8-bit rgb,
// filter none, one fixed-huffman deflate block whose lz77 only looks at the
// previous pixel and the pixel above (flat ui areas shrink a lot, photos stay
// near raw size). no allocation: the caller owns every buffer. (satoru)

namespace PngEncode {

// bytes the raw (filtered) scanline stream needs: h * (1 + 3 * w). (satoru)
uint32_t RawSize(int w, int h);

// worst-case encoded png size for a w x h image. (satoru)
uint32_t MaxSize(int w, int h);

// encode the w x h region whose top-left pixel is at src (rows pitch bytes
// apart, src_bpp 32, 24 or 16; 32 is 0xAARRGGBB little-endian). raw is a
// scratch buffer of RawSize(w, h) bytes, out holds MaxSize(w, h) bytes.
// returns the png length, 0 on bad arguments. (satoru)
uint32_t Encode(const uint8_t* src, int w, int h, uint32_t pitch, int src_bpp,
                uint8_t* raw, uint8_t* out, uint32_t out_cap);

}  // namespace PngEncode

// end (satoru)
