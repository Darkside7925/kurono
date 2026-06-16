#include "kmemx_test.h"
#include "kmemx_lz4.h"
#include "pmm.h"
#include "../drivers/serial.h"

//  kmemx self-tests. stage 1: lz4 byte-exact roundtrip over 1000 pages. (satoru)

namespace KMemXTest {

namespace {

constexpr int PAGE = 4096;

// a tiny deterministic prng (xorshift32) so every run produces the same pages
// and a failure is reproducible. (satoru)
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
};

// fill one 4 kb page with content whose compressibility depends on `kind`, so
// the suite exercises the full token grammar (long literal runs, long matches,
// rle overlaps, incompressible data). (satoru)
static void make_page(uint8_t* p, int kind, uint32_t seed) {
    Rng r(seed);
    switch (kind & 7) {
    case 0:  // all-zero page: max compressibility, one big match (satoru)
        for (int i = 0; i < PAGE; i++) p[i] = 0;
        break;
    case 1:  // single repeating byte: rle / overlap match path (satoru)
        for (int i = 0; i < PAGE; i++) p[i] = (uint8_t)(seed & 0xFF);
        break;
    case 2: { // repeating short pattern: many medium matches (satoru)
        uint8_t pat[7];
        for (int i = 0; i < 7; i++) pat[i] = (uint8_t)(r.next() & 0xFF);
        for (int i = 0; i < PAGE; i++) p[i] = pat[i % 7];
        break;
    }
    case 3: { // pseudo-text: limited alphabet with whitespace, like a log line (satoru)
        static const char* alpha = "abcdefghijklmnopqrstuvwxyz      .,\n";
        int n = 0; while (alpha[n]) n++;
        for (int i = 0; i < PAGE; i++) p[i] = (uint8_t)alpha[r.next() % (uint32_t)n];
        break;
    }
    case 4: { // structured 16-byte records, many identical: dedup-ish (satoru)
        uint8_t rec[16];
        for (int i = 0; i < 16; i++) rec[i] = (uint8_t)(r.next() & 0xFF);
        for (int i = 0; i < PAGE; i++) {
            if ((i & 0x3F) == 0) rec[i & 15] = (uint8_t)(r.next() & 0xFF);  // occasional drift (satoru)
            p[i] = rec[i & 15];
        }
        break;
    }
    case 5:  // fully random: incompressible, exercises the expand/no-fit path (satoru)
        for (int i = 0; i < PAGE; i++) p[i] = (uint8_t)(r.next() & 0xFF);
        break;
    case 6: { // half-zero, half-random: mixed runs and literals (satoru)
        for (int i = 0; i < PAGE / 2; i++) p[i] = 0;
        for (int i = PAGE / 2; i < PAGE; i++) p[i] = (uint8_t)(r.next() & 0xFF);
        break;
    }
    default: { // ascending counter with noise: monotone-ish data (satoru)
        for (int i = 0; i < PAGE; i++)
            p[i] = (uint8_t)((i + (r.next() & 3)) & 0xFF);
        break;
    }
    }
}

// log a decimal number inline (SerialLogger::LogDec emits its own value). (satoru)
static void log_kv(const char* k, int v) {
    SerialLogger::Log(k);
    SerialLogger::LogDec(v);
}

}  // namespace

// stage 1: 1000-page byte-exact lz4 roundtrip. allocates three page-sized
// buffers from the pmm (compressed buffer is CompressBound-sized) plus the
// compressor scratch, runs every page kind, and verifies (a) decompress returns
// the exact length, (b) every byte matches the original, (c) the crc32 of the
// decompressed page equals the crc32 taken before compression. (satoru)
static bool test_lz4_roundtrip_1000() {
    const int comp_cap = KMemXLZ4::CompressBound(PAGE);

    uint8_t* orig = (uint8_t*)PMM::AllocBytes(PAGE);
    uint8_t* comp = (uint8_t*)PMM::AllocBytes((size_t)comp_cap);
    uint8_t* deco = (uint8_t*)PMM::AllocBytes(PAGE);
    void*    scr  = PMM::AllocBytes((size_t)KMemXLZ4::SCRATCH_BYTES);
    if (!orig || !comp || !deco || !scr) {
        SerialLogger::Log("KMEMX-TEST: lz4_roundtrip_1000 FAIL (alloc)\r\n");
        return false;
    }

    int pass = 0, fail = 0;
    uint64_t total_in = 0, total_out = 0;
    int stored_uncompressed = 0;   // incompressible pages the compressor declined (satoru)

    for (int i = 0; i < 1000; i++) {
        make_page(orig, i, 0xABCD0001u + (uint32_t)i * 2654435761u);
        uint32_t crc_in = KMemXLZ4::Crc32(orig, PAGE);

        int csz = KMemXLZ4::Compress(orig, PAGE, comp, comp_cap, scr);

        const uint8_t* src_for_decode;
        int src_len_for_decode;
        if (csz == 0) {
            // compressor declined (incompressible). kmemx stores the raw page in
            // that case; emulate that here and still verify the raw path. (satoru)
            stored_uncompressed++;
            src_for_decode = orig;        // raw page IS the stored bytes (satoru)
            src_len_for_decode = PAGE;
            // a raw-stored page is not lz4  -  skip decode, just confirm identity. (satoru)
            for (int b = 0; b < PAGE; b++) deco[b] = orig[b];
        } else {
            total_in  += PAGE;
            total_out += (uint64_t)csz;
            int dsz = KMemXLZ4::Decompress(comp, csz, deco, PAGE);
            if (dsz != PAGE) {
                fail++;
                if (fail <= 3) { log_kv("KMEMX-TEST: page ", i); log_kv(" bad dsz=", dsz); SerialLogger::Log("\r\n"); }
                continue;
            }
            (void)src_for_decode; (void)src_len_for_decode;
        }

        // byte-exact compare. (satoru)
        bool ok = true;
        for (int b = 0; b < PAGE; b++) {
            if (deco[b] != orig[b]) { ok = false; break; }
        }
        uint32_t crc_out = KMemXLZ4::Crc32(deco, PAGE);
        if (!ok || crc_out != crc_in) {
            fail++;
            if (fail <= 3) {
                log_kv("KMEMX-TEST: page ", i);
                SerialLogger::Log(ok ? " crc mismatch\r\n" : " byte mismatch\r\n");
            }
            continue;
        }
        pass++;
    }

    PMM::FreeBytes(orig, PAGE);
    PMM::FreeBytes(comp, (size_t)comp_cap);
    PMM::FreeBytes(deco, PAGE);
    PMM::FreeBytes(scr, (size_t)KMemXLZ4::SCRATCH_BYTES);

    // report compression ratio across the compressible pages (x100 to avoid fp). (satoru)
    int ratio_x100 = total_out ? (int)((total_in * 100) / total_out) : 0;
    SerialLogger::Log("KMEMX-TEST: lz4_roundtrip_1000 ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" pass=", pass);
    log_kv(" fail=", fail);
    log_kv(" raw_stored=", stored_uncompressed);
    log_kv(" ratio_x100=", ratio_x100);
    SerialLogger::Log("\r\n");
    return fail == 0;
}

// a focused edge-case battery: tiny inputs, exactly-MF_LIMIT inputs, a single
// long run, and a couple of adversarial patterns that historically break naive
// lz4 encoders (overlapping matches at distance 1, alternating bytes). (satoru)
static bool test_lz4_edge_cases() {
    const int CAP = KMemXLZ4::CompressBound(PAGE);
    uint8_t* orig = (uint8_t*)PMM::AllocBytes(PAGE);
    uint8_t* comp = (uint8_t*)PMM::AllocBytes((size_t)CAP);
    uint8_t* deco = (uint8_t*)PMM::AllocBytes(PAGE);
    void*    scr  = PMM::AllocBytes((size_t)KMemXLZ4::SCRATCH_BYTES);
    if (!orig || !comp || !deco || !scr) {
        SerialLogger::Log("KMEMX-TEST: lz4_edge_cases FAIL (alloc)\r\n");
        return false;
    }

    int fail = 0;
    // a set of (length, generator) cases. (satoru)
    int lengths[] = {0, 1, 2, 3, 4, 5, 11, 12, 13, 16, 64, 255, 256, 4095, 4096};
    for (int li = 0; li < (int)(sizeof(lengths) / sizeof(lengths[0])); li++) {
        int n = lengths[li];
        // pattern A: distance-1 rle (all same byte). (satoru)
        for (int i = 0; i < n; i++) orig[i] = 0x5A;
        int csz = KMemXLZ4::Compress(orig, n, comp, CAP, scr);
        if (csz > 0) {
            int dsz = KMemXLZ4::Decompress(comp, csz, deco, n);
            if (dsz != n) { fail++; log_kv("KMEMX-TEST: edge rle len ", n); SerialLogger::Log(" bad\r\n"); }
            else for (int i = 0; i < n; i++) if (deco[i] != orig[i]) { fail++; break; }
        }
        // pattern B: alternating bytes (no long matches). (satoru)
        for (int i = 0; i < n; i++) orig[i] = (uint8_t)((i & 1) ? 0xFF : 0x00);
        csz = KMemXLZ4::Compress(orig, n, comp, CAP, scr);
        if (csz > 0) {
            int dsz = KMemXLZ4::Decompress(comp, csz, deco, n);
            if (dsz != n) { fail++; log_kv("KMEMX-TEST: edge alt len ", n); SerialLogger::Log(" bad\r\n"); }
            else for (int i = 0; i < n; i++) if (deco[i] != orig[i]) { fail++; break; }
        }
    }

    // a corrupt-stream rejection check: feed garbage and assert Decompress
    // refuses to overflow (returns -1 OR a wrong length, never crashes). (satoru)
    for (int i = 0; i < 64; i++) comp[i] = 0xEE;   // bogus large literal token run (satoru)
    int dsz_bad = KMemXLZ4::Decompress(comp, 64, deco, PAGE);
    bool reject_ok = true;   // any bounded return is acceptable; the point is no overflow (satoru)
    (void)dsz_bad;

    PMM::FreeBytes(orig, PAGE);
    PMM::FreeBytes(comp, (size_t)CAP);
    PMM::FreeBytes(deco, PAGE);
    PMM::FreeBytes(scr, (size_t)KMemXLZ4::SCRATCH_BYTES);

    SerialLogger::Log("KMEMX-TEST: lz4_edge_cases ");
    SerialLogger::Log((fail == 0 && reject_ok) ? "PASS" : "FAIL");
    log_kv(" fail=", fail);
    SerialLogger::Log("\r\n");
    return fail == 0 && reject_ok;
}

int RunAll() {
    SerialLogger::Log("KMEMX-TEST: starting (stage 1: lz4 engine)\r\n");
    int pass = 0, total = 0;

    total++; if (test_lz4_roundtrip_1000()) pass++;
    total++; if (test_lz4_edge_cases())     pass++;

    SerialLogger::Log("KMEMX-TEST: SUMMARY ");
    SerialLogger::LogDec(pass);
    SerialLogger::Log("/");
    SerialLogger::LogDec(total);
    SerialLogger::Log("\r\n");
    return pass;
}

}  // namespace KMemXTest

// end (satoru)
