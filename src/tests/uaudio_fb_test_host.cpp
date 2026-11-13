// host unit test for the usb audio feedback math (src/drivers/usb_audio_fb.h).
// build + run:
//   g++ -std=c++17 -O2 -I src tools/uaudio_fb_test.cpp -o /mnt/windows/Users/ibrah/kw/scratch/uaudio/uaudio_fb_test
//   /mnt/windows/Users/ibrah/kw/scratch/uaudio/uaudio_fb_test
// covers the uac 10.14 (3 byte) and 16.16 (4 byte) encodings, the linux style
// format auto-detect, plausibility rejection, smoothing and the long-run packet
// frame count at the nominal rate and with a clock offset. (satoru)
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "drivers/usb_audio_fb.h"

static int g_fail = 0;
static void check(bool c, const char* what) {
    printf("[fbtest] %-52s %s\n", what, c ? "ok" : "FAIL");
    if (!c) g_fail++;
}

static void put24(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16);
}
static void put32(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

int main() {
    const uint32_t fs_nom = UsbAudioFb::Nominal(48000, false);   // per 1 ms frame (satoru)
    const uint32_t hs_nom = UsbAudioFb::Nominal(48000, true);    // per 125 us uframe (satoru)
    check(fs_nom == 0x300000u, "nominal 48 kHz full speed is 48.0 q16.16");
    check(hs_nom == 0x60000u, "nominal 48 kHz high speed is 6.0 q16.16 per uframe");

    uint8_t b[4];
    int32_t sh = UsbAudioFb::kShiftAuto;
    put24(b, 48u << 14);                                          // 48.0 in 10.14 (satoru)
    uint32_t f = UsbAudioFb::Parse(b, 3, fs_nom, &sh);
    check(f == fs_nom && sh == 2, "10.14 value 48.0 auto-detects to 48.0");

    sh = UsbAudioFb::kShiftAuto;
    put32(b, 6u << 16);                                           // 6.0 per uframe (satoru)
    f = UsbAudioFb::Parse(b, 4, hs_nom, &sh);
    check(f == hs_nom && sh == 0, "16.16 value 6.0 auto-detects to nominal");

    // 44100 full speed: 44.1 in 10.14, nominal is 44100/1000 (satoru)
    uint32_t nom44 = UsbAudioFb::Nominal(44100, false);
    sh = UsbAudioFb::kShiftAuto;
    uint32_t raw44 = (uint32_t)(((uint64_t)44100 * 16384u) / 1000u);
    put24(b, raw44);
    f = UsbAudioFb::Parse(b, 3, nom44, &sh);
    check(f > nom44 - 4 && f < nom44 + 4, "10.14 value 44.1 kHz lands on nominal");

    // an implausible value must be rejected once the format is known (satoru)
    sh = UsbAudioFb::kShiftAuto;
    put32(b, 6u << 16);
    UsbAudioFb::Parse(b, 4, hs_nom, &sh);                         // lock the format (satoru)
    put32(b, 18u << 16);                                          // 3x nominal (satoru)
    f = UsbAudioFb::Parse(b, 4, hs_nom, &sh);
    check(f == 0 && sh == UsbAudioFb::kShiftAuto, "implausible feedback rejected and re-detected");

    // smoothing converges to a 100 ppm fast clock (satoru)
    UsbAudioFb::Smooth sm;
    uint32_t fast = (uint32_t)((uint64_t)fs_nom + (fs_nom * 100u) / 1000000u);
    for (int i = 0; i < 256; i++) sm.Update(fast);
    check(sm.Valid() && sm.cur >= fs_nom && sm.cur <= fast, "smoothing converges inside the band");

    // 60 s at exactly 48 kHz is 2 880 000 frames, packet by packet (satoru)
    UsbAudioFb::Pacer pace;
    uint64_t total = 0;
    uint32_t minf = 0xFFFFFFFFu, maxf = 0;
    for (int i = 0; i < 60000; i++) {
        uint32_t fr = pace.Peek(fs_nom, 0, 0xFFFF);
        if (fr < minf) minf = fr;
        if (fr > maxf) maxf = fr;
        pace.Commit(fs_nom, 0, 0xFFFF);
        total += fr;
    }
    check(total == 2880000ull, "pacer 48 kHz for 60 s is exact");
    check(minf == 48 && maxf == 48, "pacer emits a flat 48 frames per 1 ms packet");

    // high speed: 8 microframes per 1 ms service interval (satoru)
    UsbAudioFb::Pacer hp;
    total = 0;
    for (int i = 0; i < 60000; i++) {
        uint32_t fr = hp.Peek(hs_nom, 3, 0xFFFF);                 // 8 * 48 = 384 (satoru)
        hp.Commit(hs_nom, 3, 0xFFFF);
        total += fr;
    }
    check(total == 2880000ull, "pacer high speed 8 uframes per 1 ms interval is exact");

    // a +100 ppm device clock: 10 minutes must track it within one frame (satoru)
    UsbAudioFb::Pacer cp;
    uint32_t fastq = (uint32_t)((uint64_t)fs_nom + (fs_nom * 100u) / 1000000u);
    total = 0;
    uint32_t last = 0, flips = 0;
    for (int i = 0; i < 600000; i++) {                            // 600 s at 1 ms (satoru)
        uint32_t fr = cp.Peek(fastq, 0, 0xFFFF);
        cp.Commit(fastq, 0, 0xFFFF);
        total += fr;
        if (i && fr != last) flips++;
        last = fr;
    }
    uint64_t want = ((uint64_t)fastq * 600000ull) >> 16;          // the exact q16 rate (satoru)
    long diff = (long)((int64_t)total - (int64_t)want);
    if (diff < 0) diff = -diff;
    check(diff <= 1, "pacer tracks a +100 ppm clock within one frame");
    long ideal = (long)((int64_t)total - (int64_t)(28800000ull + 2880ull));
    if (ideal < 0) ideal = -ideal;
    check(ideal <= 10, "pacer is within 10 frames of the ideal 100 ppm clock");
    check(flips > 8, "pacer spreads the extra frames over time");

    // simulated 10.14 feedback stream: 48.0048 samples per ms with jitter
    // around it. parse and smooth each packet like the driver does, then pace
    // 60 s of packets: every packet is 48 or 49 frames and the total tracks
    // the device clock. (satoru)
    {
        int32_t sh2 = UsbAudioFb::kShiftAuto;
        UsbAudioFb::Smooth sm2;
        UsbAudioFb::Pacer p2;
        uint64_t tot2 = 0;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        int bad = 0;
        for (int i = 0; i < 60000; i++) {
            uint32_t want = fastq;                                    // q16.16 target (satoru)
            uint32_t jit = (uint32_t)((i * 7919) % 2048);             // +/- 1/32 sample (satoru)
            uint32_t v = (i & 1) ? want + jit : want - jit;
            put24(b, v >> 2);                                         // 10.14 = q16.16 >> 2 (satoru)
            uint32_t f = UsbAudioFb::Parse(b, 3, fs_nom, &sh2);
            if (!f) { bad++; continue; }
            uint32_t sm = sm2.Update(f);
            uint32_t fr = p2.Peek(sm, 0, 0xFFFF);
            p2.Commit(sm, 0, 0xFFFF);
            tot2 += fr;
            if (fr < lo) lo = fr;
            if (fr > hi) hi = fr;
        }
        check(bad == 0, "simulated 10.14 stream parses every packet");
        check(lo >= 47 && hi <= 49, "simulated packets stay 48/49 frames with jitter");
        uint64_t want2 = ((uint64_t)fastq * 60000ull) >> 16;
        long d2 = (long)((int64_t)tot2 - (int64_t)want2);
        if (d2 < 0) d2 = -d2;
        check(d2 <= 30, "simulated stream total tracks the device clock");
    }

    // simulated high speed 16.16: 6.0 per 125 us uframe, 8 uframes per 1 ms
    // service interval, with the same jitter. packet sizes stay 48/49. (satoru)
    {
        int32_t sh3 = UsbAudioFb::kShiftAuto;
        UsbAudioFb::Smooth sm3;
        UsbAudioFb::Pacer p3;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (int i = 0; i < 10000; i++) {
            uint32_t jit = (uint32_t)((i * 4099) % 256);
            uint32_t v = (i & 1) ? hs_nom + jit : hs_nom - jit;
            put32(b, v);
            uint32_t f = UsbAudioFb::Parse(b, 4, hs_nom, &sh3);
            if (!f) continue;
            uint32_t sm = sm3.Update(f);
            uint32_t fr = p3.Peek(sm, 3, 0xFFFF);
            p3.Commit(sm, 3, 0xFFFF);
            if (fr < lo) lo = fr;
            if (fr > hi) hi = fr;
        }
        check(lo >= 47 && hi <= 49, "simulated 16.16 uframe stream stays 48/49 frames");
    }

    // max-frame clamp must not bank the excess forever (satoru)
    UsbAudioFb::Pacer mp;
    uint32_t cl = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t fr = mp.Peek(0x1000000u, 0, 64);                 // 256 frames, cap 64 (satoru)
        mp.Commit(0x1000000u, 0, 64);
        cl += fr;
    }
    check(cl == 64000u, "pacer clamps to max frames without banking");

    printf("[fbtest] %s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}

// end (satoru)
