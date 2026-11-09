#pragma once
#include "../kernel/types.h"

// screenshot - snapshot the active framebuffer into a 32bpp uncompressed
// bottom-up BMP and persist it through the kvfs. heap-backed, freed after
// the write. (satoru)

class Screenshot {
public:
    // capture the current back buffer to a BMP at the given kvfs path.
    // returns true on success (buffer allocated, encoded, written). (satoru)
    static bool CaptureToBMP(const char* kvfs_path);

    // capture to a unique path "/home/user/screenshot_<ms>.bmp" derived
    // from the real-time clock for uniqueness. (satoru)
    static bool CaptureTimestamped();

    // downscaled capture (longest side <= max_dim) into a BMP at the given
    // path. used by the screen recorder so a frame sequence stays small. (satoru)
    static bool CaptureScaledToBMP(const char* kvfs_path, int max_dim);
};

// end (satoru)
