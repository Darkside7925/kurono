// host unit test for the hardware cursor position maths (IgpuCalc::CursorPosVal,
// CursorLocal). not part of the kernel build. run from src/drivers:
//   g++ -std=c++17 -O1 -Wall -o /tmp/cursor_test intel_cursor_hosttest.cpp intel_modeset_calc.cpp && /tmp/cursor_test
// expected values come from the linux register layout (i9xx_cursor_update_arm:
// sign bit plus magnitude per axis) and from the compositor's rule that the plane
// works in panel pixels, worked here by hand. (satoru)
#include <cstdio>
#include <cstdint>
#include "intel_modeset_calc.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace IgpuCalc;

static uint32_t pos(int x, int y) {   // the linux encoding, written out independently (satoru)
    uint32_t v = 0;
    uint32_t ax = (uint32_t)(x < 0 ? -x : x), ay = (uint32_t)(y < 0 ? -y : y);
    v |= ax & 0x7FFF;
    if (x < 0) v |= 0x8000;
    v |= (ay & 0x7FFF) << 16;
    if (y < 0) v |= 0x80000000u;
    return v;
}

static void test_pos_val() {
    CHECK(CursorPosVal(0, 0) == 0, "origin");
    CHECK(CursorPosVal(100, 50) == 0x00320064u, "100,50 = 0x%08x", CursorPosVal(100, 50));
    CHECK(CursorPosVal(2559, 1599) == 0x063F09FFu, "far corner of 2560x1600 = 0x%08x", CursorPosVal(2559, 1599));
    CHECK(CursorPosVal(-5, 7) == 0x00078005u, "partly off the left edge keeps |x|: 0x%08x", CursorPosVal(-5, 7));
    CHECK(CursorPosVal(9, -12) == 0x800C0009u, "partly off the top keeps |y|: 0x%08x", CursorPosVal(9, -12));
    CHECK(CursorPosVal(-63, -63) == 0x803F803Fu, "both axes negative: 0x%08x", CursorPosVal(-63, -63));
    for (int x = -64; x < 2600; x += 37)
        for (int y = -64; y < 1650; y += 41)
            CHECK(CursorPosVal(x, y) == pos(x, y), "pos %d,%d", x, y);
}

static void test_local_single_output() {
    // the panel: 2560x1600 at the origin. ui scale 100: the plane follows the pointer 1:1. (satoru)
    int lx = -1, ly = -1;
    CHECK(CursorLocal(100, false, 0, 0, 2560, 1600, 2560, 1600, 0, 0, &lx, &ly) && lx == 0 && ly == 0, "100%% origin");
    CHECK(CursorLocal(100, false, 0, 0, 2560, 1600, 2560, 1600, 2559, 1599, &lx, &ly) && lx == 2559 && ly == 1599, "100%% last pixel");
    CHECK(CursorLocal(100, false, 0, 0, 2560, 1600, 2560, 1600, 1280, 800, &lx, &ly) && lx == 1280 && ly == 800, "100%% middle");
    CHECK(!CursorLocal(100, false, 0, 0, 2560, 1600, 2560, 1600, 2560, 10, &lx, &ly), "100%% one past the right edge");
    CHECK(!CursorLocal(100, false, 0, 0, 2560, 1600, 2560, 1600, 10, -1, &lx, &ly), "100%% above the top");
    // other scales: the compositor is smaller than the panel (2560*100/scale), the arrow is drawn
    // in software in logical space; the plane must stay off whatever the pointer position is. (satoru)
    const int scales[] = { 125, 150, 200 };
    for (int s : scales) {
        int lw = 2560 * 100 / s, lh = 1600 * 100 / s;
        CHECK(!CursorLocal(s, false, 0, 0, lw, lh, 2560, 1600, lw / 2, lh / 2, &lx, &ly), "%d%% single output: plane off (mid)", s);
        CHECK(!CursorLocal(s, false, 0, 0, lw, lh, 2560, 1600, 0, 0, &lx, &ly), "%d%% single output: plane off (origin)", s);
        // the same sizes even with the scale value lying 100 (a stale scale): the size check holds (satoru)
        CHECK(!CursorLocal(100, false, 0, 0, lw, lh, 2560, 1600, lw / 2, lh / 2, &lx, &ly), "%d%% sizes differ: plane off", s);
    }
}

static void test_local_multi() {
    // two heads side by side: the second panel 1920x1080 at x=2560. local = pointer - output origin. (satoru)
    int lx = -1, ly = -1;
    CHECK(CursorLocal(100, true, 2560, 0, 1920, 1080, 1920, 1080, 2560, 0, &lx, &ly) && lx == 0 && ly == 0, "second head origin");
    CHECK(CursorLocal(100, true, 2560, 0, 1920, 1080, 1920, 1080, 4479, 1079, &lx, &ly) && lx == 1919 && ly == 1079, "second head last pixel: %d,%d", lx, ly);
    CHECK(!CursorLocal(100, true, 2560, 0, 1920, 1080, 1920, 1080, 2559, 500, &lx, &ly), "pointer on the first head is not on the second");
    // multi output with a scaled output (compositor 1536x960 for a 2560x1600 panel at 166%): off (satoru)
    CHECK(!CursorLocal(125, true, 0, 0, 2048, 1280, 2560, 1600, 100, 100, &lx, &ly), "multi, scaled output: off");
    // multi output, scale value 125 but output not scaled: allowed, the size check decides (satoru)
    CHECK(CursorLocal(125, true, 0, 0, 2560, 1600, 2560, 1600, 100, 100, &lx, &ly) && lx == 100, "multi, unscaled output on");
}

int main() {
    test_pos_val();
    test_local_single_output();
    test_local_multi();
    printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "ALL PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// end (satoru)
