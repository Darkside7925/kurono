// host unit test for the frame pacer state machine. not part of the kernel
// build. run from src/ui:
//   g++ -std=c++17 -O1 -Wall -o /tmp/fp_test frame_pacer_hosttest.cpp && /tmp/fp_test
// the sequences are synthetic frame outcomes; budgets are recomputed here
// from the rate, not taken from the code under test. (satoru)
#include <cstdio>
#include <cstdint>
#include "frame_pacer_policy.h"

using namespace FramePacerPolicy;

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t period_ns(uint32_t hz) { return 1000000000ull / hz; }

// one continuous real frame that had work and missed its budget badly.
static void feed_load(Pacer& p, int frames, uint64_t cost_ns) {
    for (int i = 0; i < frames; i++) {
        uint32_t now = 1000u + (uint32_t)i * 4u;
        p.OnFrame(now, cost_ns, period_ns(p.CurrentHz()) / 2 + 1, true, true, true);
    }
}

// one continuous real idle frame (nothing to draw).
static void feed_idle(Pacer& p, int frames) {
    static uint32_t now = 50000u;
    for (int i = 0; i < frames; i++)
        p.OnFrame(now++, 200000ull, 0, true, true, false);
}

static void test_cap_clamp() {
    Pacer p;
    p.Rebuild(240, 144);
    CHECK(p.CapHz() == 144, "cap clamps to the output, got %u", p.CapHz());
    CHECK(p.CurrentHz() == 144, "rate starts at the clamped cap");
    p.Rebuild(165, 240);
    CHECK(p.CapHz() == 165, "a chosen rate below the output is kept, got %u", p.CapHz());
    p.Rebuild(240, 0);
    CHECK(p.CapHz() == 240, "no reported output keeps the chosen cap");
    p.Rebuild(30, 0);
    CHECK(p.CapHz() == 60, "floor is 60, got %u", p.CapHz());
}

static void test_boot_burst_does_not_step_down() {
    Pacer p;
    p.Rebuild(165, 240);
    // boot animation / hand-off: expensive but not real desktop frames.
    for (int i = 0; i < 400; i++)
        p.OnFrame(1000u + (uint32_t)i, 20000000ull, 15000000ull, true, false, true);
    CHECK(p.CurrentHz() == 165, "boot film must not step down, got %u", p.CurrentHz());
    // a broken continuity also resets the window: alternating real frames
    // cannot complete one.
    for (int i = 0; i < 400; i++)
        p.OnFrame(2000u + (uint32_t)i * 4u, 20000000ull, 15000000ull, (i & 1) == 0, true, true);
    CHECK(p.CurrentHz() == 165, "non-continuous frames must not step down, got %u", p.CurrentHz());
}

static void test_idle_recovers() {
    Pacer p;
    p.Rebuild(165, 240);
    // the ladder starts at the cap: idle must not push it down.
    feed_idle(p, 2000);
    CHECK(p.CurrentHz() == 165, "idle must not step down, got %u", p.CurrentHz());

    // a fresh pacer capped low, idle-only windows step back up to the cap.
    Pacer q;
    q.Rebuild(240, 240);
    feed_load(q, 400, 30000000ull);            // heavy: steps down from 240
    CHECK(q.CurrentHz() < 240, "heavy frames must step down, got %u", q.CurrentHz());
    uint32_t low = q.CurrentHz();
    feed_idle(q, 4000);                        // plenty of all-idle windows
    CHECK(q.CurrentHz() == 240, "idle should recover to the cap, got %u (from %u)", q.CurrentHz(), low);
    CHECK(q.LastAction() == ACT_UP, "last decision was a step up");

    // the real idle desktop only renders the 250 ms fallback frame, so each
    // idle sample is NOT continuous with the previous one. those gapped idle
    // frames must still recover. (satoru)
    Pacer r;
    r.Rebuild(240, 240);
    feed_load(r, 400, 30000000ull);
    uint32_t low2 = r.CurrentHz();
    for (int i = 0; i < 200; i++)
        r.OnFrame(60000u + (uint32_t)i * 300u, 800000ull, 0, false, true, false);
    CHECK(r.CurrentHz() == 240, "gapped idle fallback frames recover too, got %u (from %u)", r.CurrentHz(), low2);
}

static void test_load_then_recovery() {
    Pacer p;
    p.Rebuild(240, 240);
    feed_load(p, 120, 10000000ull);            // 10 ms frames in a 4.17 ms budget
    CHECK(p.CurrentHz() == 165, "240 -> 165 after a heavy window, got %u", p.CurrentHz());
    // keep missing: walks down the ladder, one step per window.
    for (int step = 0; step < 5 && p.CurrentHz() > 60; step++) feed_load(p, 400, 10000000ull);
    CHECK(p.CurrentHz() == 60, "sustained load reaches the 60 floor, got %u", p.CurrentHz());

    // the load drops: cheap frames with work still present. four clean
    // windows at 60 hz (~2 s) must step back up.
    int guard = 0;
    while (p.CurrentHz() < 240 && guard++ < 200) {
        for (int i = 0; i < 40; i++)
            p.OnFrame(90000u + (uint32_t)guard * 100u + (uint32_t)i, 500000ull, 0, true, true, true);
    }
    CHECK(p.CurrentHz() == 240, "cheap work frames recover to the cap, got %u", p.CurrentHz());

    // and a single isolated miss in an otherwise idle window is not load.
    Pacer r;
    r.Rebuild(240, 240);
    for (int w = 0; w < 8; w++) {
        for (int i = 0; i < 119; i++) r.OnFrame(70000u + (uint32_t)w * 200u + (uint32_t)i, 100000ull, 0, true, true, false);
        r.OnFrame(70000u + (uint32_t)w * 200u + 119u, 20000000ull, 15000000ull, true, true, true);
    }
    CHECK(r.CurrentHz() == 240, "one slow frame per idle window is not sustained load, got %u", r.CurrentHz());
}

int main() {
    test_cap_clamp();
    test_boot_burst_does_not_step_down();
    test_idle_recovers();
    test_load_then_recovery();
    printf("frame pacer: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// end (satoru)
