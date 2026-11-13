// intel gt (blitter engine) logic for xe-lpg. see intel_gt_calc.h. every
// layout cites the linux source it transcribes (v7.1-rc3, drivers/gpu/drm). (satoru)
#include "intel_gt_calc.h"
#include <stddef.h>

namespace IgpuGt {

namespace {

constexpr uint64_t PAGE_PRESENT = 1ull << 0, PAGE_RW = 1ull << 1;       // xe_gtt_defs.h (satoru)
constexpr uint64_t PPGTT_PTE_PAT0 = 1ull << 3, PPGTT_PTE_PAT1 = 1ull << 4;
constexpr uint64_t PPGTT_PTE_PAT2 = 1ull << 7, PPGTT_PTE_PAT3 = 1ull << 62;
constexpr uint64_t PPGTT_ADDR = 0x0000FFFFFFFFF000ull;

// i915 intel_lrc.c dg2_xcs_offsets (the table i915 uses for every non render
// engine from 12.55 on, the blitter of 12.70 included), written out: the
// offsets of the registers each MI_LOAD_REGISTER_IMM of the image loads. (satoru)
const uint16_t LRI_A[15] = { 0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140, 0x110,
                             0x1c0, 0x1c4, 0x1c8, 0x180, 0x2b4, 0x120, 0x124 };
const uint16_t LRI_B[9] = { 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270 };

inline uint32_t lri_header(uint32_t count) {
    // MI_LRI_NUM_REGS(x) = XE_INSTR_NUM_DW(2x + 1) = 2x - 1 (satoru)
    return MI_LOAD_REGISTER_IMM | MI_LRI_LRM_CS_MMIO | MI_LRI_FORCE_POSTED | (2u * count - 1u);
}

} // namespace

uint32_t MocsControl(int index) {
    // xe_mocs.c mtl_mocs_desc control values: IG_PAT bit 8, l4 policy bits 3:2
    // (0 wb, 1 wt, 3 uc). an unused index takes entry 1 (unused_entries_index). (satoru)
    constexpr uint32_t IG_PAT = 1u << 8, L4_WT = 1u << 2, L4_UC = 3u << 2;
    switch (index) {
        case 0: return 0;
        case 3: case 5: case 7: case 9: return IG_PAT | L4_UC;
        case 14: return IG_PAT | L4_WT;
        default: return IG_PAT;
    }
}

void LrcBuild(uint32_t* state, const LrcParams& p) {
    for (int i = 0; i < CTX_STATE_DWORDS; i++) state[i] = 0;
    int i = 0;
    state[i++] = MI_NOOP;                                   // NOP(1) (satoru)
    state[i++] = lri_header(15);
    for (int k = 0; k < 15; k++) { state[i] = p.mmio_base + LRI_A[k]; i += 2; }
    state[i++] = MI_NOOP;                                   // NOP(1) (satoru)
    state[i++] = lri_header(9);
    for (int k = 0; k < 9; k++) { state[i] = p.mmio_base + LRI_B[k]; i += 2; }
    state[i] = MI_BATCH_BUFFER_END | 1u;                    // set_offsets' terminator (satoru)

    // xe_lrc.c set_context_control: no synchronous context switch, and the
    // engine state of this (never saved) image must not be restored. (satoru)
    state[CTX_CONTEXT_CONTROL] = MaskedEnable(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH | CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
    // reset_stop_ring: the saved MI_MODE comes back with STOP_RING cleared (satoru)
    state[CTX_MI_MODE_SLOT + 1] = STOP_RING << 16;
    // xe_lrc_ctx_init (satoru)
    state[CTX_RING_START] = p.ring_ggtt;
    state[CTX_RING_HEAD] = 0;
    state[CTX_RING_TAIL] = 0;
    state[CTX_RING_CTL] = (p.ring_bytes - 4096u) | RING_VALID;   // RING_CTL_SIZE (satoru)
    state[CTX_TIMESTAMP] = 0;
    state[CTX_PDP0_UDW] = (uint32_t)(p.pml4_phys >> 32);          // xe_lrc_set_ppgtt (satoru)
    state[CTX_PDP0_LDW] = (uint32_t)p.pml4_phys;
}

uint64_t CtxDescriptor(uint32_t lrc_ggtt, uint32_t ctx_id) {
    // xe_lrc.c: LRC_VALID bit 0, LRC_ADDRESSING_MODE 4:3 = LRC_LEGACY_64B_CONTEXT
    // (3), LRC_PRIVILEGE bit 8 (set for a context with a vm: "it really just
    // means ppgtt mode"). xe_execlist.c: XEHP_SW_CTX_ID at bit 39, 16 wide. (satoru)
    uint64_t d = 1ull | (3ull << 3) | (1ull << 8);
    d |= (uint64_t)(ctx_id & 0xFFFFu) << 39;
    d |= (uint64_t)(lrc_ggtt & 0xFFFFF000u);
    return d;
}

uint64_t PpgttPte(uint64_t phys, int pat_index) {
    // xe_vm.c xelp_pte_encode_addr + pte_encode_pat_index, level 0 (satoru)
    uint64_t pte = (phys & PPGTT_ADDR) | PAGE_PRESENT | PAGE_RW;
    if (pat_index & 1) pte |= PPGTT_PTE_PAT0;
    if (pat_index & 2) pte |= PPGTT_PTE_PAT1;
    if (pat_index & 4) pte |= PPGTT_PTE_PAT2;
    if (pat_index & 8) pte |= PPGTT_PTE_PAT3;
    return pte;
}

uint64_t PpgttPde(uint64_t table_phys) {
    // xe_vm.c xelp_pde_encode_bo: present, rw, and pde_encode_pat_index of the
    // write-back index (3 on xe-lpg: the tables live in ordinary ram) (satoru)
    return (table_phys & PPGTT_ADDR) | PAGE_PRESENT | PAGE_RW | PPGTT_PTE_PAT0 | PPGTT_PTE_PAT1;
}

bool PpgttMap(Ppgtt& g, uint64_t va, uint64_t phys, uint64_t pages, int pat_index) {
    if (!g.pml4 || !g.alloc_table || !g.table) return false;
    if ((va & 0xFFF) || (phys & 0xFFF) || (va >> 48)) return false;
    for (uint64_t n = 0; n < pages; n++, va += 4096, phys += 4096) {
        uint64_t* t = g.table(g.ctx, g.pml4);
        if (!t) return false;
        for (int level = 3; level >= 1; level--) {
            uint64_t idx = (va >> (12 + 9 * level)) & 0x1FF;
            if (!(t[idx] & PAGE_PRESENT)) {
                uint64_t np = g.alloc_table(g.ctx);
                if (!np) return false;
                g.tables++;
                t[idx] = PpgttPde(np);
            }
            t = g.table(g.ctx, t[idx] & PPGTT_ADDR);
            if (!t) return false;
        }
        t[(va >> 12) & 0x1FF] = PpgttPte(phys, pat_index);
    }
    return true;
}

bool PpgttWalk(const Ppgtt& g, uint64_t va, uint64_t* phys, uint64_t* pte_out) {
    if (!g.pml4 || !g.table || (va >> 48)) return false;
    uint64_t* t = g.table(g.ctx, g.pml4);
    for (int level = 3; level >= 1; level--) {
        if (!t) return false;
        uint64_t e = t[(va >> (12 + 9 * level)) & 0x1FF];
        if (!(e & PAGE_PRESENT)) return false;
        t = g.table(g.ctx, e & PPGTT_ADDR);
    }
    if (!t) return false;
    uint64_t pte = t[(va >> 12) & 0x1FF];
    if (!(pte & PAGE_PRESENT)) return false;
    if (phys) *phys = (pte & PPGTT_ADDR) | (va & 0xFFF);
    if (pte_out) *pte_out = pte;
    return true;
}

int EmitJob(uint32_t* dw, uint32_t mmio_base, uint32_t start_seqno_ggtt, uint32_t seqno_ggtt,
            uint32_t seqno, uint64_t batch_va) {
    int i = 0;
    // xe_ring_ops.c __emit_job_gen12_simple with ring_ops_flush_tlb: the
    // preparser is held off around a flush that invalidates the tlbs and
    // stores the start seqno, so page tables written since the last job are
    // seen. (satoru)
    dw[i++] = PREPARSER_DISABLE | 1u;
    dw[i++] = MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | MI_INVALIDATE_TLB | (4u - 2u);
    dw[i++] = start_seqno_ggtt | MI_FLUSH_DW_USE_GTT;
    dw[i++] = 0;
    dw[i++] = seqno;
    dw[i++] = PREPARSER_DISABLE | 0u;
    // xe_hw_engine.c BLIT_CCTL_default_MOCS: XY_FAST_COPY_BLT has no mocs
    // field and takes the index from this register. linux loads it once into
    // the default context image; this context is never restored from a saved
    // engine state, so every job loads it. (satoru)
    dw[i++] = MI_LOAD_REGISTER_IMM | 1u;
    dw[i++] = mmio_base + BLIT_CCTL;
    dw[i++] = BLIT_CCTL_VALUE;
    dw[i++] = MI_BATCH_BUFFER_START | MI_BB_PPGTT | 1u;
    dw[i++] = (uint32_t)batch_va;
    dw[i++] = (uint32_t)(batch_va >> 32);
    dw[i++] = MI_ARB_ON_OFF;                                  // off: the seqno write is not preempted (satoru)
    dw[i++] = MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | (4u - 2u);
    dw[i++] = seqno_ggtt | MI_FLUSH_DW_USE_GTT;
    dw[i++] = 0;
    dw[i++] = seqno;
    dw[i++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    dw[i++] = MI_ARB_CHECK;
    dw[i++] = MI_NOOP;                                        // the tail is qword aligned (satoru)
    return i;
}

bool BlitRectOk(uint32_t pitch, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if (x + w > 0x7FFF || y + h > 0x7FFF) return false;       // signed 16 bit corners (satoru)
    if (pitch == 0 || (pitch & 3) || pitch > 0xFFFF) return false;
    if ((uint64_t)(x + w) * 4u > pitch) return false;
    return true;
}

int EmitFill(uint32_t* b, int i, uint64_t dst_va, uint32_t dst_pitch, int x, int y, int w, int h, uint32_t color) {
    // i915 intel_migrate.c emit_clear / xe_migrate.c emit_clear_main_copy, the
    // 16 dword form of graphics >= 12.50, with a real rectangle. (satoru)
    b[i++] = XY_FAST_COLOR_BLT | XY_FAST_COLOR_BLT_DEPTH_32 | (uint32_t)(XY_FAST_COLOR_BLT_DW - 2);
    b[i++] = ((uint32_t)MOCS_UC_INDEX << 22) | (dst_pitch - 1u);
    b[i++] = ((uint32_t)y << 16) | (uint32_t)x;
    b[i++] = ((uint32_t)(y + h) << 16) | (uint32_t)(x + w);
    b[i++] = (uint32_t)dst_va;
    b[i++] = (uint32_t)(dst_va >> 32);
    b[i++] = 1u << 31;                                        // system memory, not local memory (satoru)
    b[i++] = color;
    for (int k = 0; k < 8; k++) b[i++] = 0;
    return i;
}

int EmitCopy(uint32_t* b, int i, uint64_t dst_va, uint32_t dst_pitch, int dx, int dy,
             uint64_t src_va, uint32_t src_pitch, int sx, int sy, int w, int h) {
    // i915 intel_migrate.c emit_copy, the XY_FAST_COPY_BLT branch (the one a
    // 12.70 part without flat ccs takes), with real rectangles. linear on
    // both sides, so no tiling bits. (satoru)
    b[i++] = XY_FAST_COPY_BLT | (uint32_t)(XY_FAST_COPY_BLT_DW - 2);
    b[i++] = XY_FAST_COPY_BLT_DEPTH_32 | dst_pitch;
    b[i++] = ((uint32_t)dy << 16) | (uint32_t)dx;
    b[i++] = ((uint32_t)(dy + h) << 16) | (uint32_t)(dx + w);
    b[i++] = (uint32_t)dst_va;
    b[i++] = (uint32_t)(dst_va >> 32);
    b[i++] = ((uint32_t)sy << 16) | (uint32_t)sx;
    b[i++] = src_pitch;
    b[i++] = (uint32_t)src_va;
    b[i++] = (uint32_t)(src_va >> 32);
    return i;
}

int EmitFlushDw(uint32_t* b, int i, uint32_t scratch_ggtt) {
    b[i++] = MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | (4u - 2u);
    b[i++] = scratch_ggtt | MI_FLUSH_DW_USE_GTT;
    b[i++] = 0;
    b[i++] = 0;
    return i;
}

int EmitBatchEnd(uint32_t* b, int i) {
    b[i++] = MI_BATCH_BUFFER_END;
    if (i & 1) b[i++] = MI_NOOP;
    return i;
}

// ---- software model (satoru) ----
namespace {

struct Sim {
    const SimEnv* env;
    SimResult* r;
    uint32_t base;
    Ppgtt g;

    bool fail(const char* why, uint32_t arg) {
        r->ok = false;
        r->err = why;
        r->err_arg = arg;
        return false;
    }
    static uint64_t* table_cb(void* ctx, uint64_t phys) {
        const SimEnv* e = (const SimEnv*)ctx;
        return (uint64_t*)e->phys_page(e->ctx, phys & ~0xFFFull);
    }
    uint32_t* ggtt_dw(uint32_t addr) {
        uint8_t* p = env->ggtt_page(env->ctx, addr & ~0xFFFu);
        return p ? (uint32_t*)(p + (addr & 0xFFCu)) : nullptr;
    }
    uint8_t* va_ptr(uint64_t va) {
        uint64_t phys = 0;
        if (!PpgttWalk(g, va, &phys, nullptr)) return nullptr;
        uint8_t* p = env->phys_page(env->ctx, phys & ~0xFFFull);
        return p ? p + (phys & 0xFFF) : nullptr;
    }
    bool fill(const uint32_t* c) {
        uint32_t pitch = (c[1] & 0x3FFFFu) + 1u;
        int x1 = (int)(c[2] & 0xFFFF), y1 = (int)(c[2] >> 16), x2 = (int)(c[3] & 0xFFFF), y2 = (int)(c[3] >> 16);
        uint64_t dst = ((uint64_t)c[5] << 32) | c[4];
        if (((c[0] >> 19) & 7) != 2) return fail("fill: not 32 bpp", c[0]);
        if (!(c[6] >> 31)) return fail("fill: local memory type on an integrated part", c[6]);
        if (x2 <= x1 || y2 <= y1 || (uint32_t)x2 * 4u > pitch) return fail("fill: bad rectangle", c[3]);
        for (int y = y1; y < y2; y++) {
            uint64_t va = dst + (uint64_t)y * pitch + (uint64_t)x1 * 4u;
            for (int x = x1; x < x2; x++, va += 4) {
                uint8_t* p = va_ptr(va);
                if (!p) return fail("fill: destination not mapped in the ppgtt", (uint32_t)va);
                *(uint32_t*)p = c[7];
            }
        }
        r->fills++;
        r->blit_bytes += (uint64_t)(x2 - x1) * 4u * (uint64_t)(y2 - y1);
        return true;
    }
    bool copy(const uint32_t* c) {
        uint32_t dpitch = c[1] & 0xFFFFu, spitch = c[7] & 0xFFFFu;
        int dx1 = (int)(c[2] & 0xFFFF), dy1 = (int)(c[2] >> 16), dx2 = (int)(c[3] & 0xFFFF), dy2 = (int)(c[3] >> 16);
        int sx1 = (int)(c[6] & 0xFFFF), sy1 = (int)(c[6] >> 16);
        uint64_t dst = ((uint64_t)c[5] << 32) | c[4], src = ((uint64_t)c[9] << 32) | c[8];
        if (((c[1] >> 24) & 3) != 3) return fail("copy: not 32 bpp", c[1]);
        if (c[0] & ((3u << 20) | (3u << 13))) return fail("copy: tiled surface bits set", c[0]);
        if (dx2 <= dx1 || dy2 <= dy1 || (uint32_t)dx2 * 4u > dpitch) return fail("copy: bad rectangle", c[3]);
        if ((uint32_t)(sx1 + (dx2 - dx1)) * 4u > spitch) return fail("copy: source rectangle leaves the pitch", c[6]);
        uint64_t row = (uint64_t)(dx2 - dx1) * 4u;
        for (int y = 0; y < dy2 - dy1; y++) {
            uint64_t d = dst + (uint64_t)(dy1 + y) * dpitch + (uint64_t)dx1 * 4u;
            uint64_t s = src + (uint64_t)(sy1 + y) * spitch + (uint64_t)sx1 * 4u;
            uint64_t left = row;
            while (left) {
                uint64_t n = left;
                if (n > 4096 - (d & 0xFFF)) n = 4096 - (d & 0xFFF);
                if (n > 4096 - (s & 0xFFF)) n = 4096 - (s & 0xFFF);
                uint8_t* dp = va_ptr(d);
                uint8_t* sp = va_ptr(s);
                if (!dp) return fail("copy: destination not mapped in the ppgtt", (uint32_t)d);
                if (!sp) return fail("copy: source not mapped in the ppgtt", (uint32_t)s);
                __builtin_memmove(dp, sp, (size_t)n);
                d += n; s += n; left -= n;
            }
        }
        r->copies++;
        r->blit_bytes += row * (uint64_t)(dy2 - dy1);
        return true;
    }
    bool batch(uint64_t va) {
        uint32_t c[XY_FAST_COLOR_BLT_DW];
        for (int guard = 0; guard < 65536; guard++) {
            uint8_t* p = va_ptr(va);
            if (!p) return fail("batch not mapped in the ppgtt", (uint32_t)va);
            uint32_t h = *(uint32_t*)p;
            r->cmds++;
            if ((h >> 29) == 0) {
                uint32_t op = h & MI_OPCODE_MASK;
                if (op == MI_BATCH_BUFFER_END) return true;
                if (h == MI_NOOP) { va += 4; continue; }
                // a flush between dependent blits: the model copies in order, nothing to drain (satoru)
                if (op == MI_FLUSH_DW && (h & 0x3F) == 2) { va += 16; continue; }
                return fail("batch: unexpected mi command", h);
            }
            int len = (int)(h & 0xFF) + 2;
            bool is_fill = (h & BLT_OPCODE_MASK) == XY_FAST_COLOR_BLT;
            bool is_copy = (h & BLT_OPCODE_MASK) == XY_FAST_COPY_BLT;
            if (!is_fill && !is_copy) return fail("batch: unknown command", h);
            if (len != (is_fill ? XY_FAST_COLOR_BLT_DW : XY_FAST_COPY_BLT_DW)) return fail("batch: wrong blit length", h);
            for (int k = 0; k < len; k++) {
                uint8_t* q = va_ptr(va + (uint64_t)k * 4u);
                if (!q) return fail("batch not mapped in the ppgtt", (uint32_t)va);
                c[k] = *(uint32_t*)q;
            }
            if (is_fill ? !fill(c) : !copy(c)) return false;
            va += (uint64_t)len * 4u;
        }
        return fail("batch: no end within 64k dwords", (uint32_t)va);
    }
};

} // namespace

bool SimSubmit(const SimEnv& env, uint32_t mmio_base, uint64_t descriptor, SimResult* r) {
    *r = SimResult{};
    r->ok = true;
    Sim s;
    s.env = &env; s.r = r; s.base = mmio_base;
    s.g = Ppgtt{};
    if (!(descriptor & 1)) return s.fail("descriptor: not valid", (uint32_t)descriptor);
    if (((descriptor >> 3) & 3) != 3) return s.fail("descriptor: not the 64 bit legacy addressing mode", (uint32_t)descriptor);
    if (!(descriptor & (1ull << 8))) return s.fail("descriptor: ppgtt bit clear", (uint32_t)descriptor);
    uint32_t lrc = (uint32_t)descriptor & 0xFFFFF000u;
    uint32_t* st = s.ggtt_dw(lrc + 4096u);
    if (!st || !s.ggtt_dw(lrc)) return s.fail("context image not mapped in the ggtt", lrc);
    if (st[1] != (MI_LOAD_REGISTER_IMM | MI_LRI_LRM_CS_MMIO | MI_LRI_FORCE_POSTED | 29u))
        return s.fail("image: first load-register header", st[1]);
    if (st[CTX_CONTEXT_CONTROL - 1] != mmio_base + RING_CONTEXT_CONTROL || st[CTX_RING_HEAD - 1] != mmio_base + RING_HEAD ||
        st[CTX_RING_TAIL - 1] != mmio_base + RING_TAIL || st[CTX_RING_START - 1] != mmio_base + RING_START ||
        st[CTX_RING_CTL - 1] != mmio_base + RING_CTL || st[CTX_PDP0_UDW - 1] != mmio_base + 0x274 ||
        st[CTX_PDP0_LDW - 1] != mmio_base + 0x270)
        return s.fail("image: a register offset is not where the layout puts it", st[CTX_RING_START - 1]);
    uint32_t ctl = st[CTX_RING_CTL];
    if (!(ctl & RING_VALID)) return s.fail("image: ring not valid", ctl);
    uint32_t size = (ctl & 0x1FF000u) + 4096u;
    uint32_t start = st[CTX_RING_START];
    uint32_t head = st[CTX_RING_HEAD] & 0x1FFFFCu, tail = st[CTX_RING_TAIL] & 0x1FFFF8u;
    if ((start & 0xFFF) || head >= size || tail >= size) return s.fail("image: ring start, head or tail", tail);
    s.g.pml4 = ((uint64_t)st[CTX_PDP0_UDW] << 32) | st[CTX_PDP0_LDW];
    s.g.ctx = (void*)&env;
    s.g.table = Sim::table_cb;
    if (!s.g.pml4 || !env.phys_page(env.ctx, s.g.pml4)) return s.fail("image: no ppgtt root", st[CTX_PDP0_LDW]);

    auto next = [&](uint32_t* out) -> bool {
        uint32_t* p = s.ggtt_dw(start + head);
        if (!p) return false;
        *out = *p;
        head = (head + 4u) % size;
        return true;
    };
    for (int guard = 0; head != tail && guard < 1 << 20; guard++) {
        uint32_t h = 0, a = 0, b = 0, c = 0;
        if (!next(&h)) return s.fail("ring not mapped in the ggtt", start + head);
        r->cmds++;
        if ((h >> 29) != 0) return s.fail("ring: not an mi command", h);
        uint32_t op = h & MI_OPCODE_MASK;
        if (op == MI_NOOP || op == MI_ARB_CHECK || op == MI_ARB_ON_OFF || op == (0x02u << 23)) continue;
        if (op == MI_FLUSH_DW) {
            if ((h & 0x3F) != 2) return s.fail("ring: flush length", h);
            if (!next(&a) || !next(&b) || !next(&c)) return s.fail("ring not mapped in the ggtt", start + head);
            if (h & MI_INVALIDATE_TLB) r->tlb_invalidated = true;
            if (h & MI_FLUSH_DW_OP_STOREDW) {
                if (!(a & MI_FLUSH_DW_USE_GTT)) return s.fail("ring: flush store not through the ggtt", a);
                uint32_t* d = s.ggtt_dw(a & ~7u);
                if (!d) return s.fail("ring: flush store address not mapped in the ggtt", a);
                *d = c;
                r->stores++;
            }
            continue;
        }
        if (op == MI_STORE_DATA_IMM) {
            if (!(h & (1u << 22)) || (h & 0x3FF) != 2) return s.fail("ring: store-data form", h);
            if (!next(&a) || !next(&b) || !next(&c)) return s.fail("ring not mapped in the ggtt", start + head);
            uint32_t* d = s.ggtt_dw(a & ~3u);
            if (!d) return s.fail("ring: store address not mapped in the ggtt", a);
            *d = c;
            r->stores++;
            continue;
        }
        if (op == MI_LOAD_REGISTER_IMM) {
            if ((h & 0xFF) != 1) return s.fail("ring: load-register length", h);
            if (!next(&a) || !next(&b)) return s.fail("ring not mapped in the ggtt", start + head);
            if (a != mmio_base + BLIT_CCTL) return s.fail("ring: load of an unexpected register", a);
            r->blit_cctl = b;
            continue;
        }
        if (op == MI_BATCH_BUFFER_START) {
            if ((h & 0xFF) != 1 || !(h & MI_BB_PPGTT)) return s.fail("ring: batch start form", h);
            if (!next(&a) || !next(&b)) return s.fail("ring not mapped in the ggtt", start + head);
            if (!s.batch(((uint64_t)b << 32) | a)) return false;
            continue;
        }
        return s.fail("ring: unknown mi command", h);
    }
    if (head != tail) return s.fail("ring: did not reach the tail", head);
    st[CTX_RING_HEAD] = head;       // what a context save writes back (satoru)
    return true;
}

} // namespace IgpuGt
// end (satoru)
