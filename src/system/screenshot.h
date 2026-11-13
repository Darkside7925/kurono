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

    // png of a screen rectangle (clipped to the screen) at the given path. (satoru)
    static bool CaptureRegionPNG(const char* kvfs_path, int x, int y, int w, int h);

    // the print screen key: the whole screen, or only the focused window when
    // window_only, saved as /home/user/Pictures/Screenshot_<date>_<time>.png.
    // out_path (optional) receives the file name. (satoru)
    static bool CaptureToPictures(bool window_only, char* out_path, int out_cap);

    // the input loop records a key press here; the desktop frame takes it and
    // runs the capture (it owns the back buffer). take returns 0 none, 1 the
    // screen, 2 the focused window. (satoru)
    static void Request(bool window_only);
    static int  TakeRequest();
};

// end (satoru)
