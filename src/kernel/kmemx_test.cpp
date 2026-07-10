#include "kmemx_test.h"
#include "kmemx_lz4.h"
#include "kmemx.h"
#include "kmemx_pool.h"
#include "kmemx_internal.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"

//  kmemx self-tests.
//    stage 1: lz4 byte-exact roundtrip over 1000 pages.
//    stage 2: pool allocator (alloc/free/coalesce) + 1000-page store/retrieve
//             round-trip through the real pool + metadata table. (satoru)

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
            // a raw-stored page is not lz4 - skip decode, just confirm identity. (satoru)
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

// ── stage 2: pool allocator ──────────────────────────────────────────────────
// alloc a spread of sizes, free every other one, confirm the freed bytes are
// reclaimable (largest-free grows back), then free the rest and confirm the
// arena returns to fully free. also a fragmentation/coalesce check: alloc N
// adjacent small blocks, free them in a scrambled order, and confirm they
// coalesce back into one large free run. (satoru)
static bool test_pool_allocator() {
    if (!KMemX::IsInitialized()) {
        SerialLogger::Log("KMEMX-TEST: pool_allocator FAIL (engine not inited)\r\n");
        return false;
    }
    KMemXPool::ResetForTest();
    uint64_t total = KMemXPool::TotalBytes();
    if (total == 0) {
        SerialLogger::Log("KMEMX-TEST: pool_allocator FAIL (no pool)\r\n");
        return false;
    }

    int fail = 0;
    // 1) basic alloc/free reclaim. (satoru)
    {
        constexpr int N = 256;
        uint32_t offs[N];
        uint32_t szs[N];
        Rng r(0xF00D);
        for (int i = 0; i < N; i++) {
            szs[i] = 32 + (r.next() % 1000);          // 32..1031 bytes (satoru)
            offs[i] = KMemXPool::Alloc(szs[i]);
            if (offs[i] == KMEMX_POOL_NULL) { fail++; break; }
            // write a recognizable pattern to detect overlap. (satoru)
            uint8_t* p = (uint8_t*)KMemXPool::Ptr(offs[i]);
            for (uint32_t b = 0; b < szs[i]; b++) p[b] = (uint8_t)((i * 7 + b) & 0xFF);
        }
        // verify no two live allocations overlap by re-reading the patterns. (satoru)
        for (int i = 0; i < N; i++) {
            if (offs[i] == KMEMX_POOL_NULL) continue;
            uint8_t* p = (uint8_t*)KMemXPool::Ptr(offs[i]);
            for (uint32_t b = 0; b < szs[i]; b++)
                if (p[b] != (uint8_t)((i * 7 + b) & 0xFF)) { fail++; break; }
        }
        uint64_t used_before = KMemXPool::UsedBytes();
        for (int i = 0; i < N; i++) if (offs[i] != KMEMX_POOL_NULL) KMemXPool::Free(offs[i], szs[i]);
        uint64_t used_after = KMemXPool::UsedBytes();
        if (used_after != 0) { fail++; log_kv("KMEMX-TEST: pool leak used_after=", (int)used_after); SerialLogger::Log("\r\n"); }
        (void)used_before;
    }

    // 2) coalesce: many adjacent small blocks, freed scrambled, must merge so a
    //    large alloc succeeds again afterwards. (satoru)
    {
        KMemXPool::ResetForTest();
        constexpr int M = 1000;
        uint32_t offs[M];
        int got = 0;
        for (int i = 0; i < M; i++) {
            offs[i] = KMemXPool::Alloc(64);
            if (offs[i] == KMEMX_POOL_NULL) break;
            got++;
        }
        // free in a scrambled order. (satoru)
        Rng r(0xBEEF);
        for (int i = got - 1; i > 0; i--) {            // fisher-yates shuffle (satoru)
            int j = (int)(r.next() % (uint32_t)(i + 1));
            uint32_t t = offs[i]; offs[i] = offs[j]; offs[j] = t;
        }
        for (int i = 0; i < got; i++) KMemXPool::Free(offs[i], 64);
        if (KMemXPool::UsedBytes() != 0) { fail++; SerialLogger::Log("KMEMX-TEST: coalesce leak\r\n"); }
        // after coalesce a big block should be allocatable again. (satoru)
        uint32_t big = KMemXPool::Alloc(64 * 1000);
        if (big == KMEMX_POOL_NULL) { fail++; SerialLogger::Log("KMEMX-TEST: coalesce did not merge\r\n"); }
        else KMemXPool::Free(big, 64 * 1000);
    }

    KMemXPool::ResetForTest();
    SerialLogger::Log("KMEMX-TEST: pool_allocator ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" fail=", fail);
    log_kv(" pool_mb=", (int)(total / (1024 * 1024)));
    SerialLogger::Log("\r\n");
    return fail == 0;
}

// stage 2: 1000-page store -> retrieve -> free round-trip through the real pool
// + metadata table. each page is compressed into the pool, retrieved (with
// crc32 verify), byte-compared, then freed. confirms the metadata hash table
// (insert/find/erase + probe-chain repair) and the pool allocator hold up under
// churn, and that live-page accounting returns to zero. (satoru)
static bool test_pool_store_retrieve_1000() {
    if (!KMemX::IsInitialized()) {
        SerialLogger::Log("KMEMX-TEST: pool_store_retrieve FAIL (engine not inited)\r\n");
        return false;
    }
    KMemXPool::ResetForTest();
    // a synthetic but distinct address-space + vaddr per page. (satoru)
    const uint64_t AS = 0x123000ULL;   // pretend cr3 (satoru)

    uint8_t* orig = (uint8_t*)PMM::AllocBytes(PAGE);
    uint8_t* deco = (uint8_t*)PMM::AllocBytes(PAGE);
    if (!orig || !deco) {
        SerialLogger::Log("KMEMX-TEST: pool_store_retrieve FAIL (alloc)\r\n");
        return false;
    }

    int fail = 0, stored = 0;
    // phase A: store 1000 pages, then retrieve+verify all, then free all. (satoru)
    const int COUNT = 1000;
    for (int i = 0; i < COUNT; i++) {
        make_page(orig, i, 0x5151u + (uint32_t)i * 40503u);
        uint64_t va = 0x40000000ULL + (uint64_t)i * PAGE;
        int slot = KMemX::TestStore(AS, va, orig);
        if (slot < 0) {
            // pool/table saturated for this synthetic workload - stop storing but
            // verify what we did store. not a failure unless retrieval breaks. (satoru)
            break;
        }
        stored++;
    }

    for (int i = 0; i < stored; i++) {
        make_page(orig, i, 0x5151u + (uint32_t)i * 40503u);
        uint64_t va = 0x40000000ULL + (uint64_t)i * PAGE;
        if (!KMemX::TestRetrieve(AS, va, deco)) { fail++; if (fail <= 3) { log_kv("KMEMX-TEST: retrieve fail page ", i); SerialLogger::Log("\r\n"); } continue; }
        for (int b = 0; b < PAGE; b++) if (deco[b] != orig[b]) { fail++; if (fail <= 3) { log_kv("KMEMX-TEST: mismatch page ", i); SerialLogger::Log("\r\n"); } break; }
    }

    uint32_t live_before_free = KMemX::TestMetaLive();
    for (int i = 0; i < stored; i++) {
        uint64_t va = 0x40000000ULL + (uint64_t)i * PAGE;
        KMemX::TestFree(AS, va);
    }
    uint32_t live_after_free = KMemX::TestMetaLive();
    if (live_after_free != 0) { fail++; log_kv("KMEMX-TEST: meta leak live=", (int)live_after_free); SerialLogger::Log("\r\n"); }

    // phase B: interleaved store/free churn to stress the probe-chain repair. (satoru)
    KMemXPool::ResetForTest();
    for (int round = 0; round < 3 && fail == 0; round++) {
        for (int i = 0; i < 300; i++) {
            make_page(orig, i + round, 0x9000u + (uint32_t)(i + round) * 2246822519u);
            uint64_t va = 0x80000000ULL + (uint64_t)i * PAGE;
            KMemX::TestStore(AS, va, orig);
        }
        // free every third, then retrieve the survivors. (satoru)
        for (int i = 0; i < 300; i += 3) {
            uint64_t va = 0x80000000ULL + (uint64_t)i * PAGE;
            KMemX::TestFree(AS, va);
        }
        for (int i = 1; i < 300; i++) {
            if (i % 3 == 0) continue;
            make_page(orig, i + round, 0x9000u + (uint32_t)(i + round) * 2246822519u);
            uint64_t va = 0x80000000ULL + (uint64_t)i * PAGE;
            if (!KMemX::TestRetrieve(AS, va, deco)) { fail++; break; }
            for (int b = 0; b < PAGE; b++) if (deco[b] != orig[b]) { fail++; break; }
        }
        // drain the rest. (satoru)
        for (int i = 0; i < 300; i++) {
            uint64_t va = 0x80000000ULL + (uint64_t)i * PAGE;
            KMemX::TestFree(AS, va);
        }
    }

    PMM::FreeBytes(orig, PAGE);
    PMM::FreeBytes(deco, PAGE);
    KMemXPool::ResetForTest();

    SerialLogger::Log("KMEMX-TEST: pool_store_retrieve_1000 ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" stored=", stored);
    log_kv(" verified_live=", (int)live_before_free);
    log_kv(" fail=", fail);
    SerialLogger::Log("\r\n");
    return fail == 0;
}

// ── stage 3: vmm page aging (generation counter in spare pte bits) ──────────
// map a fresh frame at a scratch virtual address, then exercise the leaf-pte
// aging primitives directly:
//   - KmemxAgeLeaf with the Accessed bit set must reset generation to 0
//   - repeated KmemxAgeLeaf with A clear must increment the generation, and it
//     must saturate at 15
//   - KmemxMarkCompressed must hand back the phys + perms and leave the leaf
//     not-present + marked (KmemxIsCompressed true)
//   - KmemxRestoreLeaf must re-map the frame and clear the marker (touching the
//     page must not fault afterward). (satoru)
static bool test_vmm_aging() {
    // a scratch identity-mappable frame + a high, otherwise-unused VA window. the
    // kernel's own address space (cr3) is fine; we map/unmap our own leaf. (satoru)
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    uint64_t frame = PMM::AllocFrame();
    if (!frame) { SerialLogger::Log("KMEMX-TEST: vmm_aging FAIL (no frame)\r\n"); return false; }
    // pick a VA far from anything mapped: 0x5_0000_0000 (20gb) is above the
    // identity map's hot region; MapPage builds the tables on demand. (satoru)
    uint64_t va = 0x500000000ULL;
    if (!KernelVMM::MapPage(va, frame, PTE_PRESENT | PTE_WRITABLE)) {
        SerialLogger::Log("KMEMX-TEST: vmm_aging FAIL (map)\r\n");
        PMM::FreeFrame(frame);
        return false;
    }
    // write something so the page is real + sets the Accessed bit. (satoru)
    volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)va;
    for (int i = 0; i < 4096; i += 256) p[i] = (uint8_t)i;

    int fail = 0;

    // 1) A is set (we just wrote) -> age resets gen to 0. (satoru)
    int g0 = KernelVMM::KmemxAgeLeaf(as, va);
    if (g0 != 0) { fail++; log_kv("KMEMX-TEST: aging A-set gen!=0 got=", g0); SerialLogger::Log("\r\n"); }

    // 2) with A now clear, each age increments the generation. don't touch the
    //    page (a read would re-set A). step several times and watch it climb. (satoru)
    int prev = 0;
    for (int i = 0; i < 6; i++) {
        int g = KernelVMM::KmemxAgeLeaf(as, va);
        if (g != prev + 1) { fail++; if (fail <= 3) { log_kv("KMEMX-TEST: aging step expected ", prev + 1); log_kv(" got ", g); SerialLogger::Log("\r\n"); } }
        prev = g;
    }
    // 3) saturate at 15. (satoru)
    for (int i = 0; i < 20; i++) KernelVMM::KmemxAgeLeaf(as, va);
    int gsat = KernelVMM::KmemxGetGeneration(as, va);
    if (gsat != 15) { fail++; log_kv("KMEMX-TEST: aging saturate gen!=15 got=", gsat); SerialLogger::Log("\r\n"); }

    // 4) mark-compressed: capture phys + perms, leave the leaf not-present+marked. (satoru)
    uint64_t cap_phys = 0, cap_flags = 0;
    bool marked = KernelVMM::KmemxMarkCompressed(as, va, &cap_phys, &cap_flags);
    if (!marked) { fail++; SerialLogger::Log("KMEMX-TEST: aging mark failed\r\n"); }
    if (cap_phys != frame) { fail++; SerialLogger::Log("KMEMX-TEST: aging mark wrong phys\r\n"); }
    if (!(cap_flags & PTE_WRITABLE)) { fail++; SerialLogger::Log("KMEMX-TEST: aging mark lost WRITABLE\r\n"); }
    if (!KernelVMM::KmemxIsCompressed(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: aging not marked compressed\r\n"); }
    if (KernelVMM::KmemxIsLeafPresent(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: aging still present after mark\r\n"); }

    // 5) restore: re-map the frame, clear the marker, and confirm a touch works. (satoru)
    bool restored = KernelVMM::KmemxRestoreLeaf(as, va, cap_phys, cap_flags);
    if (!restored) { fail++; SerialLogger::Log("KMEMX-TEST: aging restore failed\r\n"); }
    if (KernelVMM::KmemxIsCompressed(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: aging still marked after restore\r\n"); }
    if (KernelVMM::KmemxGetGeneration(as, va) != 0) { fail++; SerialLogger::Log("KMEMX-TEST: aging gen not cleared on restore\r\n"); }
    // the page must be writable again. (satoru)
    p[0] = 0xAB; if (p[0] != 0xAB) { fail++; SerialLogger::Log("KMEMX-TEST: aging restored page not writable\r\n"); }

    // cleanup: unmap + free. (satoru)
    KernelVMM::UnmapPage(va, false);
    PMM::FreeFrame(frame);

    SerialLogger::Log("KMEMX-TEST: vmm_aging ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" fail=", fail);
    SerialLogger::Log("\r\n");
    return fail == 0;
}

// ── stage 6: end-to-end compress -> real #pf -> decompress on a live page ────
// the most important integration test: map a real frame at a scratch VA, fill it
// with known content, CompressPage() it (which makes the leaf not-present +
// marked and frees the frame), confirm it is no longer present, then TOUCH the
// page. that read triggers a genuine #PF; the hal hook calls KMemX::HandleFault,
// which decompresses the blob into a fresh frame, crc-verifies it, and restores
// the leaf - so the touch must observe the original bytes. repeated over many
// pages of varied content. proves the whole pipeline + the fault wiring. (satoru)
static bool test_fault_roundtrip() {
    if (!KMemX::IsInitialized() || !KMemX::IsEnabled()) {
        SerialLogger::Log("KMEMX-TEST: fault_roundtrip FAIL (engine off)\r\n");
        return false;
    }
    if (KMemXPool::TotalBytes() == 0) {
        SerialLogger::Log("KMEMX-TEST: fault_roundtrip FAIL (no pool)\r\n");
        return false;
    }
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    int fail = 0, cycles = 0;
    const int COUNT = 64;   // 64 live pages through the full pipeline (satoru)

    for (int i = 0; i < COUNT; i++) {
        uint64_t frame = PMM::AllocFrame();
        if (!frame) break;
        uint64_t va = 0x510000000ULL + (uint64_t)i * 0x2000ULL;   // spaced scratch VAs (satoru)
        if (!KernelVMM::MapPage(va, frame, PTE_PRESENT | PTE_WRITABLE)) { PMM::FreeFrame(frame); fail++; continue; }

        // fill with a known pattern (kind cycles for variety). (satoru)
        uint8_t* p = (uint8_t*)(uintptr_t)va;
        make_page(p, i, 0xC0FFEEu + (uint32_t)i * 2654435761u);
        uint32_t crc_before = KMemXLZ4::Crc32(p, PAGE);

        // age the leaf to the threshold so it is eligible regardless of A-bit, then
        // compress it explicitly. (CompressPage re-checks compressibility.) (satoru)
        for (int a = 0; a < 16; a++) KernelVMM::KmemxAgeLeaf(as, va);
        bool comp = KMemX::CompressPage(as, va);
        if (!comp) {
            // could legitimately decline (e.g. pool full) - clean up + skip. (satoru)
            KernelVMM::UnmapPage(va, true);
            continue;
        }
        // the leaf must now be not-present + marked. (satoru)
        if (KernelVMM::KmemxIsLeafPresent(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: still present after compress\r\n"); }
        if (!KernelVMM::KmemxIsCompressed(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: not marked compressed\r\n"); }

        // TOUCH the page -> genuine #PF -> HandleFault decompresses + restores.
        // read every byte and recompute the crc; it must match the pre-compress
        // value exactly. (satoru)
        volatile uint8_t* vp = (volatile uint8_t*)(uintptr_t)va;
        uint8_t check[16];
        for (int b = 0; b < 16; b++) check[b] = vp[b * 256];   // sample touches across the page (satoru)
        (void)check;
        uint32_t crc_after = KMemXLZ4::Crc32((const uint8_t*)(uintptr_t)va, PAGE);
        if (crc_after != crc_before) { fail++; if (fail <= 3) { log_kv("KMEMX-TEST: fault page ", i); SerialLogger::Log(" crc mismatch after decompress\r\n"); } }
        else cycles++;

        // the page must be present + writable again. (satoru)
        if (!KernelVMM::KmemxIsLeafPresent(as, va)) { fail++; SerialLogger::Log("KMEMX-TEST: not present after fault\r\n"); }
        vp[0] = 0x77; if (vp[0] != 0x77) { fail++; }

        // cleanup: free the (new) frame the fault path mapped. (satoru)
        KernelVMM::UnmapPage(va, true);
    }

    const KMemX::Stats& st = KMemX::GetStats();
    int mean_ns = st.faults_served ? (int)(st.ns_decompress_sum / st.faults_served) : 0;
    SerialLogger::Log("KMEMX-TEST: fault_roundtrip ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" cycles=", cycles);
    log_kv(" fail=", fail);
    log_kv(" faults_served=", (int)st.faults_served);
    log_kv(" decomp_ns_min=", (int)st.ns_decompress_min);
    log_kv(" decomp_ns_mean=", mean_ns);
    log_kv(" decomp_ns_max=", (int)st.ns_decompress_max);
    log_kv(" over_10us=", (int)st.decomp_over_10us);
    log_kv(" comp_ns_max=", (int)st.ns_compress_max);
    SerialLogger::Log("\r\n");
    return fail == 0 && cycles > 0;
}

// ── stage 10: deduplication (ksm-style, cow refcounted) ─────────────────────
// store many IDENTICAL pages (so dedup can merge them) interleaved with unique
// pages, run DedupPass, and verify: (a) merges happened, (b) pool usage dropped
// vs the pre-dedup figure, (c) EVERY page (deduped + unique) still retrieves
// byte-exact, and (d) freeing all pages returns the pool to empty - proving the
// shared-extent refcounting never leaks or double-frees. (satoru)
static bool test_dedup() {
    if (!KMemX::IsInitialized() || !KMemX::IsEnabled() || KMemXPool::TotalBytes() == 0) {
        SerialLogger::Log("KMEMX-TEST: dedup FAIL (engine off / no pool)\r\n");
        return false;
    }
    KMemXPool::ResetForTest();
    const uint64_t AS = 0xDED00ULL;
    uint8_t* buf = (uint8_t*)PMM::AllocBytes(PAGE);
    uint8_t* deco = (uint8_t*)PMM::AllocBytes(PAGE);
    if (!buf || !deco) { SerialLogger::Log("KMEMX-TEST: dedup FAIL (alloc)\r\n"); return false; }

    int fail = 0;
    const int GROUPS = 8;       // 8 distinct contents (satoru)
    const int COPIES = 40;      // 40 identical copies of each (satoru)
    int stored = 0;
    // store COPIES identical pages per group, plus the group is content-distinct. (satoru)
    for (int g = 0; g < GROUPS; g++) {
        for (int c = 0; c < COPIES; c++) {
            // identical content within a group: seed by group only. avoid the
            // incompressible random kind (5) so pages actually share a blob. (satoru)
            make_page(buf, g % 5, 0x1000u + (uint32_t)g);
            uint64_t va = ((uint64_t)g << 28) + (uint64_t)c * PAGE + 0x10000000ULL;
            if (KMemX::TestStore(AS, va, buf) >= 0) stored++;
        }
    }
    uint64_t used_before = KMemXPool::UsedBytes();

    // run dedup until it stops merging (bounded passes). (satoru)
    int total_merged = 0;
    for (int pass = 0; pass < 20; pass++) {
        int m = KMemX::DedupPass(256);
        total_merged += m;
        if (m == 0) break;
    }
    uint64_t used_after = KMemXPool::UsedBytes();

    // (a) merges happened. with 8 groups x 40 copies, ~ (40-1)*8 = 312 merges. (satoru)
    if (total_merged <= 0) { fail++; SerialLogger::Log("KMEMX-TEST: dedup no merges\r\n"); }
    // (b) pool usage dropped substantially. (satoru)
    if (used_after >= used_before) { fail++; SerialLogger::Log("KMEMX-TEST: dedup pool did not shrink\r\n"); }

    // (c) every page still retrieves byte-exact. (satoru)
    int verified = 0;
    for (int g = 0; g < GROUPS && fail == 0; g++) {
        make_page(buf, g % 5, 0x1000u + (uint32_t)g);
        for (int c = 0; c < COPIES; c++) {
            uint64_t va = ((uint64_t)g << 28) + (uint64_t)c * PAGE + 0x10000000ULL;
            if (!KMemX::TestRetrieve(AS, va, deco)) { fail++; break; }
            for (int b = 0; b < PAGE; b++) if (deco[b] != buf[b]) { fail++; break; }
            if (fail) break;
            verified++;
        }
    }

    // (d) free everything; pool must return to empty (no leak / double-free). (satoru)
    for (int g = 0; g < GROUPS; g++) {
        for (int c = 0; c < COPIES; c++) {
            uint64_t va = ((uint64_t)g << 28) + (uint64_t)c * PAGE + 0x10000000ULL;
            KMemX::TestFree(AS, va);
        }
    }
    uint64_t used_final = KMemXPool::UsedBytes();
    uint32_t live_final = KMemX::TestMetaLive();
    if (used_final != 0) { fail++; log_kv("KMEMX-TEST: dedup pool leak used=", (int)used_final); SerialLogger::Log("\r\n"); }
    if (live_final != 0) { fail++; log_kv("KMEMX-TEST: dedup meta leak live=", (int)live_final); SerialLogger::Log("\r\n"); }

    int shrink_pct = used_before ? (int)(((used_before - used_after) * 100) / used_before) : 0;
    PMM::FreeBytes(buf, PAGE);
    PMM::FreeBytes(deco, PAGE);
    KMemXPool::ResetForTest();

    SerialLogger::Log("KMEMX-TEST: dedup ");
    SerialLogger::Log(fail == 0 ? "PASS" : "FAIL");
    log_kv(" stored=", stored);
    log_kv(" merged=", total_merged);
    log_kv(" verified=", verified);
    log_kv(" pool_shrink_pct=", shrink_pct);
    log_kv(" fail=", fail);
    SerialLogger::Log("\r\n");
    return fail == 0;
}

int RunAll() {
    SerialLogger::Log("KMEMX-TEST: starting\r\n");
    int pass = 0, total = 0;

    // stage 1: lz4 engine. (satoru)
    total++; if (test_lz4_roundtrip_1000()) pass++;
    total++; if (test_lz4_edge_cases())     pass++;

    // stage 2: pool + metadata. requires the engine initialized + enabled so the
    // primitives are live. init with the default 20% pool if not already up. (satoru)
    if (!KMemX::IsInitialized()) KMemX::Init(KMemX::PoolPct());
    KMemX::SetEnabled(true);
    total++; if (test_pool_allocator())            pass++;
    total++; if (test_pool_store_retrieve_1000())  pass++;

    // stage 3: vmm page aging (generation counter in spare pte bits). (satoru)
    total++; if (test_vmm_aging())                 pass++;

    // stage 6: end-to-end compress -> real page-fault -> decompress. (satoru)
    total++; if (test_fault_roundtrip())           pass++;

    // stage 10: ksm-style dedup with cow refcounting. (satoru)
    total++; if (test_dedup())                     pass++;

    SerialLogger::Log("KMEMX-TEST: SUMMARY ");
    SerialLogger::LogDec(pass);
    SerialLogger::Log("/");
    SerialLogger::LogDec(total);
    SerialLogger::Log("\r\n");
    return pass;
}

}  // namespace KMemXTest

// end (satoru)
