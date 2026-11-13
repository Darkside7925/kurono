// host unit test for the gt data layouts and the software model of the
// blitter command streamer. not part of the kernel build. run from src/drivers:
//   g++ -std=c++17 -O1 -Wall -o /tmp/igt_test intel_gt_hosttest.cpp intel_gt_calc.cpp && /tmp/igt_test
// expected values are worked from the linux definitions in the comments or
// taken from a context image i915 dumped on the owner's laptop
// (debugfs i915_engine_info, 2026-11-09), not copied from the code under
// test. (satoru)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "intel_gt_calc.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace IgpuGt;

// ---- a small machine: ram at a fake physical base, a ggtt of 64 entries (satoru) ----
struct Machine {
    static constexpr uint64_t RAM_BASE = 0x40000000ull;      // 1 gb, like the laptop's pmm pages (satoru)
    static constexpr uint64_t RAM_BYTES = 24ull << 20;
    static constexpr uint32_t GGTT_BASE = 0x00800000u;
    std::vector<uint8_t> ram;
    uint64_t ggtt[64];
    uint64_t next;
    Machine() : ram(RAM_BYTES, 0), next(RAM_BASE) { memset(ggtt, 0, sizeof(ggtt)); }
    uint8_t* at(uint64_t phys) {
        if (phys < RAM_BASE || phys + 4096 > RAM_BASE + RAM_BYTES) return nullptr;
        return ram.data() + (phys - RAM_BASE);
    }
    uint64_t alloc(uint64_t pages) {
        uint64_t p = next;
        next += pages * 4096;
        if (next > RAM_BASE + RAM_BYTES) { printf("machine out of ram\n"); exit(2); }
        memset(at(p), 0, pages * 4096);
        return p;
    }
    uint32_t ggtt_map(uint64_t phys, uint32_t pages, uint32_t first) {
        for (uint32_t i = 0; i < pages; i++) ggtt[first + i] = phys + (uint64_t)i * 4096;
        return GGTT_BASE + first * 4096u;
    }
};
static uint8_t* m_ggtt_page(void* ctx, uint32_t a) {
    Machine* m = (Machine*)ctx;
    if (a < Machine::GGTT_BASE) return nullptr;
    uint32_t i = (a - Machine::GGTT_BASE) >> 12;
    if (i >= 64 || !m->ggtt[i]) return nullptr;
    return m->at(m->ggtt[i]);
}
static uint8_t* m_phys_page(void* ctx, uint64_t phys) { return ((Machine*)ctx)->at(phys); }
static uint64_t m_alloc_table(void* ctx) { return ((Machine*)ctx)->alloc(1); }
static uint64_t* m_table(void* ctx, uint64_t phys) { return (uint64_t*)((Machine*)ctx)->at(phys); }

static void test_constants() {
    // xe_mi_commands.h: __MI_INSTR(op) = op << 23; XE_INSTR_NUM_DW(n) = n - 2 (satoru)
    CHECK(MI_BATCH_BUFFER_END == 0x05000000u, "bb end");
    CHECK(MI_LOAD_REGISTER_IMM == 0x11000000u, "lri");
    CHECK(MI_FLUSH_DW == 0x13000000u, "flush dw");
    CHECK(MI_BATCH_BUFFER_START == 0x18800000u, "bb start");
    CHECK(MI_STORE_DATA_IMM == 0x10000000u, "sdi");
    // i915's own ring on the laptop shows "18800101 fb3c0000 0000fffe": a 3 dword
    // batch start with the ppgtt bit (satoru)
    CHECK((MI_BATCH_BUFFER_START | MI_BB_PPGTT | 1u) == 0x18800101u, "bb start ppgtt form 0x%08x", MI_BATCH_BUFFER_START | MI_BB_PPGTT | 1u);
    // xe_gpu_commands.h: (2 << 29 | 0x44 << 22) and (2 << 29 | 0x42 << 22) (satoru)
    CHECK(XY_FAST_COLOR_BLT == 0x51000000u, "fast color blt 0x%08x", XY_FAST_COLOR_BLT);
    CHECK(XY_FAST_COPY_BLT == 0x50800000u, "fast copy blt 0x%08x", XY_FAST_COPY_BLT);
    // masked registers: value in the low half, write mask in the high half (satoru)
    CHECK(MaskedEnable(GFX_DISABLE_LEGACY_MODE) == 0x00080008u, "masked enable");
    CHECK(MaskedDisable(STOP_RING) == 0x01000000u, "masked disable");
    // xe_pat.c xelpg_pat_table: [2] uc = l4 policy 3 << 2, [3] wb | coh 2, [4] wb | coh 3.
    // i915 on the laptop has 0x4800.. = 0, 4, c, 2, 3. (satoru)
    CHECK(PAT_VALUES[0] == 0 && PAT_VALUES[1] == 4 && PAT_VALUES[2] == 0xC && PAT_VALUES[3] == 2 && PAT_VALUES[4] == 3, "pat table");
    // xe_mocs.c mtl_mocs_desc; i915 on the laptop has 0x4000.. = 0, 100, 100, 10c,
    // 100, 10c, 100, 10c, 100, 10c, 100 (10), .., 104 (14), 100 (15) (satoru)
    const uint32_t want[16] = { 0, 0x100, 0x100, 0x10c, 0x100, 0x10c, 0x100, 0x10c, 0x100, 0x10c,
                                0x100, 0x100, 0x100, 0x100, 0x104, 0x100 };
    for (int i = 0; i < 16; i++) CHECK(MocsControl(i) == want[i], "mocs[%d] 0x%x != 0x%x", i, MocsControl(i), want[i]);
    // BLIT_CCTL: dst mocs 14:9, src mocs 6:1, both the uncached index 9 (satoru)
    CHECK(BLIT_CCTL_VALUE == ((9u << 9) | (9u << 1)), "blit cctl");
}

static void test_lrc() {
    uint32_t st[CTX_STATE_DWORDS];
    memset(st, 0xAA, sizeof(st));
    LrcParams p = { BLT_RING_BASE, 0x00803000u, 16384, 0x0000000875115000ull };
    LrcBuild(st, p);
    // i915's rcs0 image on the laptop starts "00000000 1108101d 00002244 ... 00002034 ..
    // 00002030 .. 00002038 .. 0000203c 00003001": same header and order with the
    // render base. the blitter base is 0x22000. (satoru)
    CHECK(st[0] == 0, "nop");
    CHECK(st[1] == 0x1108101du, "first lri header 0x%08x", st[1]);
    CHECK(st[2] == 0x22244 && st[4] == 0x22034 && st[6] == 0x22030 && st[8] == 0x22038 && st[10] == 0x2203c, "ring registers");
    CHECK(st[0x12] == 0x221c0 && st[0x14] == 0x221c4 && st[0x16] == 0x221c8, "bb per ctx / indirect ctx registers");
    CHECK(st[0x1c] == 0x22120 && st[0x1e] == 0x22124, "dg2 extra registers");
    // the same dump: dword 0x20 is a nop, 0x21 = 11081011, 0x22 = base + 0x3a8,
    // 0x30 = base + 0x274 (pdp0 udw), 0x32 = base + 0x270 (pdp0 ldw) (satoru)
    CHECK(st[0x20] == 0 && st[0x21] == 0x11081011u, "second lri header 0x%08x", st[0x21]);
    CHECK(st[0x22] == 0x223a8 && st[0x30] == 0x22274 && st[0x32] == 0x22270, "timestamp and pdp0 registers");
    CHECK(st[0x34] == (MI_BATCH_BUFFER_END | 1u), "terminator");
    CHECK(st[CTX_CONTEXT_CONTROL] == 0x00090009u, "context control 0x%08x", st[CTX_CONTEXT_CONTROL]);
    CHECK(st[CTX_RING_START] == 0x00803000u && st[CTX_RING_HEAD] == 0 && st[CTX_RING_TAIL] == 0, "ring start/head/tail");
    CHECK(st[CTX_RING_CTL] == 0x00003001u, "ring ctl 0x%08x (a 16k ring is 0x3001 in the dump too)", st[CTX_RING_CTL]);
    CHECK(st[CTX_PDP0_UDW] == 0x8 && st[CTX_PDP0_LDW] == 0x75115000u, "pdp0");
    CHECK(st[CTX_BB_PER_CTX_PTR] == 0 && st[CTX_CS_INDIRECT_CTX] == 0, "no wa batches");
    CHECK(st[0x71] == 0x01000000u, "mi_mode slot");
    for (int i = 0x35; i < 0x70; i++) CHECK(st[i] == 0, "zero tail of the image at %d", i);
    // descriptor: valid | 64 bit legacy << 3 | ppgtt bit 8 | id << 39 | lrca (satoru)
    CHECK(CtxDescriptor(0x00801000u, 1) == (0x119ull | (1ull << 39) | 0x00801000ull), "descriptor");
    CHECK((CtxDescriptor(0xfec8e000u, 0) & 0xFFF) == 0x119, "descriptor low bits");
}

static void test_ppgtt() {
    Machine m;
    Ppgtt g = {};
    g.pml4 = m.alloc(1);
    g.ctx = &m; g.alloc_table = m_alloc_table; g.table = m_table;
    // xe_gtt_defs.h: present 1, rw 2, pat0 bit 3, pat1 bit 4, pat2 bit 7 (satoru)
    CHECK(PpgttPte(0x12345000, PAT_UC) == (0x12345000ull | 3 | 0x10), "uc pte");
    CHECK(PpgttPte(0x12345000, PAT_WB_1WAY) == (0x12345000ull | 3 | 0x18), "wb 1 way pte");
    CHECK(PpgttPte(0x12345000, PAT_WB_2WAY) == (0x12345000ull | 3 | 0x80), "wb 2 way pte");
    CHECK(PpgttPde(0x00077000) == (0x77000ull | 3 | 0x18), "pde");
    uint64_t a = m.alloc(600), phys = 0, pte = 0;
    // identity map 600 pages that straddle a 2 mb boundary: 2 page tables (satoru)
    CHECK(PpgttMap(g, a, a, 600, PAT_UC), "map");
    CHECK(PpgttWalk(g, a, &phys, &pte) && phys == a && (pte & 0x10), "first page");
    CHECK(PpgttWalk(g, a + 599 * 4096 + 0x123, &phys, nullptr) && phys == a + 599 * 4096 + 0x123, "last page with offset");
    CHECK(!PpgttWalk(g, a + 600 * 4096, &phys, nullptr), "one past the end is not mapped");
    CHECK(!PpgttWalk(g, a - 4096, &phys, nullptr), "one before is not mapped");
    uint64_t span = ((a + 600 * 4096 - 1) >> 21) - (a >> 21) + 1;
    CHECK(g.tables == 2 + span, "tables %u for %llu pd entries", g.tables, (unsigned long long)span);
    CHECK(!PpgttMap(g, a + 1, a, 1, 0) && !PpgttMap(g, 1ull << 48, a, 1, 0), "unaligned or non canonical refused");
    // a far address needs its own pdp/pd/pt chain (satoru)
    uint32_t before = g.tables;
    CHECK(PpgttMap(g, 0x0000008000000000ull, a, 1, PAT_WB_1WAY) && g.tables == before + 3, "far mapping adds three tables");
    CHECK(PpgttWalk(g, 0x0000008000000000ull, &phys, nullptr) && phys == a, "far mapping resolves");
    // an address with a different index above 255 at every level, followed by
    // hand through the tables: pml4[300] -> pdp[301] -> pd[302] -> pt[303] (satoru)
    uint64_t hv = (300ull << 39) | (301ull << 30) | (302ull << 21) | (303ull << 12);
    CHECK(PpgttMap(g, hv, a + 4096, 1, PAT_UC), "high index mapping");
    uint64_t* t = (uint64_t*)m.at(g.pml4);
    const int ix[3] = { 300, 301, 302 };
    bool chain = true;
    for (int k = 0; k < 3 && chain; k++) {
        uint64_t e = t[ix[k]];
        chain = (e & 3) == 3 && (e & 0x18) == 0x18;
        t = chain ? (uint64_t*)m.at(e & 0x0000FFFFFFFFF000ull) : nullptr;
        chain = chain && t;
    }
    CHECK(chain && t[303] == ((a + 4096) | 3 | 0x10), "tables hold the entries at 300/301/302/303");
    CHECK(PpgttWalk(g, hv + 0x10, &phys, nullptr) && phys == a + 4096 + 0x10, "high index mapping resolves");
}

struct Ctx {
    Machine m;
    Ppgtt g;
    uint64_t blk, batch, sa, sb;
    uint32_t ggtt;
    uint32_t tail, seq;
    SimEnv env;
    uint32_t* state() { return (uint32_t*)m.at(blk + 2 * 4096); }
    uint32_t* ring() { return (uint32_t*)m.at(blk + 3 * 4096); }
    uint32_t* seqno() { return (uint32_t*)m.at(blk + 7 * 4096); }
    uint32_t* bb() { return (uint32_t*)m.at(batch); }
    void init(uint32_t start_tail) {
        g = Ppgtt{};
        g.pml4 = m.alloc(1);
        g.ctx = &m; g.alloc_table = m_alloc_table; g.table = m_table;
        blk = m.alloc(8);
        batch = m.alloc(2);
        ggtt = m.ggtt_map(blk, 8, 4);
        PpgttMap(g, batch, batch, 2, PAT_WB_1WAY);
        LrcParams p = { BLT_RING_BASE, ggtt + 3 * 4096, 16384, g.pml4 };
        LrcBuild(state(), p);
        tail = start_tail;
        state()[CTX_RING_HEAD] = start_tail;
        state()[CTX_RING_TAIL] = start_tail;
        seq = 0;
        env = SimEnv{ &m, m_ggtt_page, m_phys_page };
    }
    bool submit(int blen, SimResult* r) {
        blen = EmitBatchEnd(bb(), blen);
        uint32_t dw[JOB_DWORDS];
        int n = EmitJob(dw, BLT_RING_BASE, ggtt + 7 * 4096, ggtt + 7 * 4096 + 8, ++seq, batch);
        for (int i = 0; i < n; i++) { ring()[tail / 4] = dw[i]; tail = (tail + 4) % 16384; }
        state()[CTX_RING_TAIL] = tail;
        return SimSubmit(env, BLT_RING_BASE, CtxDescriptor(ggtt + 4096, 1), r);
    }
};

static uint32_t px(int x, int y, uint32_t salt) { return 0xFF000000u | (uint32_t)(x * 31 + y * 977 + (int)salt * 7919); }

static void test_job_words() {
    uint32_t dw[JOB_DWORDS];
    int n = EmitJob(dw, BLT_RING_BASE, 0x00807000, 0x00807008, 0x1234, 0x0000000140002000ull);
    CHECK(n == JOB_DWORDS && (n & 1) == 0, "job length %d", n);
    // xe_ring_ops.c: preparser_disable(true) = MI_ARB_CHECK | BIT(8) | 1 (satoru)
    CHECK(dw[0] == 0x02800101u && dw[5] == 0x02800100u, "preparser 0x%08x 0x%08x (i915's ring on the laptop has the same two words)", dw[0], dw[5]);
    // MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_IMM_DW | MI_INVALIDATE_TLB (satoru)
    CHECK(dw[1] == (0x13000000u | (1u << 14) | (1u << 18) | 2u), "tlb flush header 0x%08x", dw[1]);
    CHECK(dw[2] == (0x00807000u | 4u) && dw[3] == 0 && dw[4] == 0x1234, "start seqno store");
    CHECK(dw[6] == (0x11000000u | 1u) && dw[7] == 0x22204 && dw[8] == 0x1212, "blit cctl load");
    CHECK(dw[9] == 0x18800101u && dw[10] == 0x40002000u && dw[11] == 0x1, "batch start");
    CHECK(dw[12] == 0x04000000u && dw[17] == 0x04000001u && dw[18] == 0x02800000u, "arbitration words (the laptop ring has 04000001 / 02800000 too)");
    CHECK(dw[13] == (0x13000000u | (1u << 14) | 2u) && dw[14] == (0x00807008u | 4u) && dw[16] == 0x1234, "end seqno store");
}

static void test_blit_words() {
    uint32_t b[32];
    // i915 intel_migrate.c emit_copy, ver >= 9 branch: cmd | (10 - 2); BLT_DEPTH_32 |
    // pitch; x1y1; y2 << 16 | x2; dst lo; dst hi; src x1y1; src pitch; src lo; src hi (satoru)
    int n = EmitCopy(b, 0, 0x0000000142000000ull, 10240, 100, 200, 0x0000000050000000ull, 20480, 2660, 200, 300, 50);
    CHECK(n == 10, "copy length");
    CHECK(b[0] == (0x50800000u | 8u), "copy header 0x%08x", b[0]);
    CHECK(b[1] == ((3u << 24) | 10240u), "copy depth and pitch");
    CHECK(b[2] == ((200u << 16) | 100u) && b[3] == ((250u << 16) | 400u), "copy destination rectangle");
    CHECK(b[4] == 0x42000000u && b[5] == 1 && b[8] == 0x50000000u && b[9] == 0, "copy addresses");
    CHECK(b[6] == ((200u << 16) | 2660u) && b[7] == 20480u, "copy source corner and pitch");
    // i915 emit_clear, ver >= 12.50: cmd | depth | (16 - 2); mocs | (pitch - 1); x1y1;
    // x2y2; lo; hi; !lmem << 31; value; then zeros (satoru)
    n = EmitFill(b, 0, 0x0000000142000000ull, 10240, 8, 9, 10, 11, 0xFF112233u);
    CHECK(n == 16, "fill length");
    CHECK(b[0] == (0x51000000u | (2u << 19) | 14u), "fill header 0x%08x", b[0]);
    CHECK(b[1] == ((9u << 22) | 10239u), "fill mocs and pitch 0x%08x", b[1]);
    CHECK(b[2] == ((9u << 16) | 8u) && b[3] == ((20u << 16) | 18u), "fill rectangle");
    CHECK(b[6] == 0x80000000u && b[7] == 0xFF112233u, "fill memory type and colour");
    for (int i = 8; i < 16; i++) CHECK(b[i] == 0, "fill tail zero");
    CHECK(BlitRectOk(20480, 0, 0, 5120, 1600), "the compositor frame is a valid source");
    CHECK(!BlitRectOk(20480, 0, 0, 5121, 1), "a rectangle wider than the pitch");
    CHECK(!BlitRectOk(131072, 0, 0, 10, 10), "a pitch over 16 bits");
    CHECK(!BlitRectOk(20480, -1, 0, 10, 10) && !BlitRectOk(20480, 0, 0, 0, 10), "negative or empty");
    n = EmitBatchEnd(b, 16);
    CHECK(n == 18 && b[16] == MI_BATCH_BUFFER_END && b[17] == MI_NOOP, "batch end pads to a qword");
}

static void test_model_end_to_end() {
    Ctx c;
    c.init(8);
    const int W = 640, H = 96;
    const uint32_t SP = 5120 * 4;            // a slice of a wider "compositor" surface (satoru)
    const uint32_t DP = W * 4;
    uint64_t src_pages = ((uint64_t)SP * H + 4095) / 4096, dst_pages = ((uint64_t)DP * H + 4095) / 4096;
    c.sa = c.m.alloc(src_pages);
    c.sb = c.m.alloc(dst_pages);
    CHECK(PpgttMap(c.g, c.sa, c.sa, src_pages, PAT_WB_1WAY) && PpgttMap(c.g, c.sb, c.sb, dst_pages, PAT_UC), "surfaces mapped");
    uint32_t* s = (uint32_t*)c.m.at(c.sa);
    uint32_t* d = (uint32_t*)c.m.at(c.sb);
    for (int y = 0; y < H; y++) for (int x = 0; x < 5120; x++) s[y * 5120 + x] = px(x, y, 1);
    for (int i = 0; i < W * H; i++) d[i] = 0xDEADBEEF;
    // reference built with plain loops (satoru)
    std::vector<uint32_t> ref(W * H, 0xDEADBEEF);
    int i = 0;
    // 1: copy the slice at x offset 2560, three rectangles, like two damage lists
    struct R { int x, y, w, h; } rects[3] = { { 0, 0, 640, 10 }, { 17, 33, 200, 40 }, { 600, 90, 40, 6 } };
    for (auto& r : rects) {
        i = EmitCopy(c.bb(), i, c.sb, DP, r.x, r.y, c.sa, SP, 2560 + r.x, r.y, r.w, r.h);
        for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) ref[(r.y + y) * W + r.x + x] = px(2560 + r.x + x, r.y + y, 1);
    }
    // 2: a fill over part of that
    i = EmitFill(c.bb(), i, c.sb, DP, 100, 40, 50, 20, 0xFF010203u);
    for (int y = 40; y < 60; y++) for (int x = 100; x < 150; x++) ref[y * W + x] = 0xFF010203u;
    SimResult r;
    bool ok = c.submit(i, &r);
    CHECK(ok, "model accepted the job: %s (0x%x)", r.err ? r.err : "", r.err_arg);
    CHECK(r.copies == 3 && r.fills == 1 && r.stores == 2, "copies %u fills %u stores %u", r.copies, r.fills, r.stores);
    CHECK(r.tlb_invalidated && r.blit_cctl == BLIT_CCTL_VALUE, "tlb invalidate and mocs load seen");
    CHECK(memcmp(d, ref.data(), W * H * 4) == 0, "destination equals the reference");
    CHECK(c.seqno()[0] == 1 && c.seqno()[2] == 1, "start and end seqno written (%u, %u)", c.seqno()[0], c.seqno()[2]);
    CHECK(c.state()[CTX_RING_HEAD] == c.tail, "the saved head reached the tail");
    CHECK(r.blit_bytes == (640ull * 10 + 200 * 40 + 40 * 6 + 50 * 20) * 4, "blit bytes %llu", (unsigned long long)r.blit_bytes);
}

static void test_model_wrap_and_resubmit() {
    Ctx c;
    c.init(16384 - 40);                      // the first job straddles the end of the ring (satoru)
    c.sa = c.m.alloc(4);
    PpgttMap(c.g, c.sa, c.sa, 4, PAT_UC);
    uint32_t* d = (uint32_t*)c.m.at(c.sa);
    for (uint32_t n = 1; n <= 300; n++) {
        int i = EmitFill(c.bb(), 0, c.sa, 64 * 4, (int)(n % 56), (int)(n % 60), 8, 4, n);
        SimResult r;
        bool ok = c.submit(i, &r);
        if (!ok || c.seqno()[2] != n || d[(n % 60) * 64 + (n % 56)] != n) {
            CHECK(false, "job %u: %s seqno %u", n, r.err ? r.err : "wrong result", c.seqno()[2]);
            return;
        }
    }
    CHECK(true, "300 jobs through a 16k ring");
    CHECK(c.tail < 16384 && (c.tail & 7) == 0, "tail stays in the ring and qword aligned");
}

static void test_model_rejects() {
    SimResult r;
    {   // destination not mapped (satoru)
        Ctx c; c.init(0);
        int i = EmitFill(c.bb(), 0, 0x0000000060000000ull, 256, 0, 0, 8, 8, 1);
        CHECK(!c.submit(i, &r) && r.err && strstr(r.err, "destination not mapped"), "unmapped destination: %s", r.err ? r.err : "(accepted)");
    }
    {   // source not mapped (satoru)
        Ctx c; c.init(0);
        c.sa = c.m.alloc(1); PpgttMap(c.g, c.sa, c.sa, 1, PAT_UC);
        int i = EmitCopy(c.bb(), 0, c.sa, 256, 0, 0, 0x0000000060000000ull, 256, 0, 0, 8, 8);
        CHECK(!c.submit(i, &r) && r.err && strstr(r.err, "source not mapped"), "unmapped source: %s", r.err ? r.err : "(accepted)");
    }
    {   // unknown command in the batch (satoru)
        Ctx c; c.init(0);
        c.bb()[0] = 0x7A000004u;                 // a pipe control, not a blitter command (satoru)
        CHECK(!c.submit(1, &r) && r.err && strstr(r.err, "unknown command"), "unknown command: %s", r.err ? r.err : "(accepted)");
    }
    {   // batch without an end (satoru)
        Ctx c; c.init(0);
        memset(c.bb(), 0, 8192);
        uint32_t dw[JOB_DWORDS];
        int n = EmitJob(dw, BLT_RING_BASE, c.ggtt + 7 * 4096, c.ggtt + 7 * 4096 + 8, 1, c.batch);
        for (int k = 0; k < n; k++) c.ring()[k] = dw[k];
        c.state()[CTX_RING_TAIL] = (uint32_t)n * 4;
        CHECK(!SimSubmit(c.env, BLT_RING_BASE, CtxDescriptor(c.ggtt + 4096, 1), &r) && r.err, "runaway batch: %s", r.err ? r.err : "(accepted)");
    }
    {   // descriptor without the ppgtt bit, and a context that is not in the ggtt (satoru)
        Ctx c; c.init(0);
        CHECK(!SimSubmit(c.env, BLT_RING_BASE, CtxDescriptor(c.ggtt + 4096, 1) & ~(1ull << 8), &r), "descriptor without ppgtt");
        CHECK(!SimSubmit(c.env, BLT_RING_BASE, CtxDescriptor(0x00F00000u, 1), &r), "context outside the ggtt");
    }
    {   // an image whose ring is not valid (satoru)
        Ctx c; c.init(0);
        c.state()[CTX_RING_CTL] &= ~1u;
        CHECK(!SimSubmit(c.env, BLT_RING_BASE, CtxDescriptor(c.ggtt + 4096, 1), &r) && strstr(r.err, "ring not valid"), "invalid ring");
    }
}

int main() {
    test_constants();
    test_lrc();
    test_ppgtt();
    test_job_words();
    test_blit_words();
    test_model_end_to_end();
    test_model_wrap_and_resubmit();
    test_model_rejects();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
// end (satoru)
