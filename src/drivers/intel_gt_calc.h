#pragma once
#include <stdint.h>

// intel gt (blitter engine) logic for xe-lpg (meteor lake / arrow lake,
// graphics 12.70): everything that is pure data. the logical ring context
// image, the context descriptor, the 4 level ppgtt, the ring and batch
// commands, and a software model of the command streamer that executes what
// the builders produce. no mmio, no kernel calls and no libc, so the same file
// builds on the host for the unit test and runs in qemu as the stand-in for
// the hardware (kurono.igpu.gttest=1). every layout is a transcription of the
// linux source named next to it (v7.1-rc3, drivers/gpu/drm). (satoru)
namespace IgpuGt {

// ---- engine (xe/regs/xe_engine_regs.h) (satoru) ----
constexpr uint32_t BLT_RING_BASE = 0x22000;
constexpr uint32_t RING_TAIL = 0x30, RING_HEAD = 0x34, RING_START = 0x38, RING_CTL = 0x3c;
constexpr uint32_t RING_IPEHR = 0x68, RING_INSTDONE = 0x6c, RING_ACTHD = 0x74;
constexpr uint32_t RING_HWS_PGA = 0x80, ENGINE_ID = 0x8c, RING_HWSTAM = 0x98, RING_MI_MODE = 0x9c;
constexpr uint32_t RING_EIR = 0xb0, RING_EMR = 0xb4, RING_ESR = 0xb8;
constexpr uint32_t RING_RESET_CTL = 0xd0;          // i915 RING_RESET_CTL (satoru)
constexpr uint32_t BLIT_CCTL = 0x204;
constexpr uint32_t RING_EXECLIST_STATUS_LO = 0x234, RING_EXECLIST_STATUS_HI = 0x238;
constexpr uint32_t RING_CONTEXT_CONTROL = 0x244, RING_MODE = 0x29c;
constexpr uint32_t RING_EXECLIST_SQ_LO = 0x510, RING_EXECLIST_SQ_HI = 0x514, RING_EXECLIST_CONTROL = 0x550;
constexpr uint32_t GFX_DISABLE_LEGACY_MODE = 1u << 3;
constexpr uint32_t STOP_RING = 1u << 8;
constexpr uint32_t EL_CTRL_LOAD = 1u << 0;
constexpr uint32_t CTX_CTRL_INHIBIT_SYN_CTX_SWITCH = 1u << 3;
constexpr uint32_t CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT = 1u << 0;
constexpr uint32_t RING_VALID = 1u;
inline constexpr uint32_t MaskedEnable(uint32_t b) { return (b << 16) | b; }
inline constexpr uint32_t MaskedDisable(uint32_t b) { return b << 16; }

// ---- context image dword indices (xe/regs/xe_lrc_layout.h) (satoru) ----
constexpr int CTX_CONTEXT_CONTROL = 0x02 + 1;
constexpr int CTX_RING_HEAD = 0x04 + 1;
constexpr int CTX_RING_TAIL = 0x06 + 1;
constexpr int CTX_RING_START = 0x08 + 1;
constexpr int CTX_RING_CTL = 0x0a + 1;
constexpr int CTX_BB_PER_CTX_PTR = 0x12 + 1;
constexpr int CTX_CS_INDIRECT_CTX = 0x14 + 1;
constexpr int CTX_TIMESTAMP = 0x22 + 1;
constexpr int CTX_PDP0_UDW = 0x30 + 1;
constexpr int CTX_PDP0_LDW = 0x32 + 1;
constexpr int CTX_STATE_DWORDS = 1024;             // one 4k page after the pphwsp (satoru)
constexpr int CTX_MI_MODE_SLOT = 0x70;             // lrc_ring_mi_mode for >= 12.50 (satoru)

// ---- mi commands (xe/instructions/xe_mi_commands.h) (satoru) ----
constexpr uint32_t MI_NOOP = 0;
constexpr uint32_t MI_ARB_CHECK = 0x05u << 23;
constexpr uint32_t MI_ARB_ON_OFF = 0x08u << 23;
constexpr uint32_t MI_ARB_ENABLE = 1u;
constexpr uint32_t MI_BATCH_BUFFER_END = 0x0Au << 23;
constexpr uint32_t MI_STORE_DATA_IMM = 0x20u << 23;
constexpr uint32_t MI_LOAD_REGISTER_IMM = 0x22u << 23;
constexpr uint32_t MI_LRI_LRM_CS_MMIO = 1u << 19;
constexpr uint32_t MI_LRI_FORCE_POSTED = 1u << 12;
constexpr uint32_t MI_FLUSH_DW = 0x26u << 23;
constexpr uint32_t MI_INVALIDATE_TLB = 1u << 18;
constexpr uint32_t MI_FLUSH_DW_OP_STOREDW = 1u << 14;
constexpr uint32_t MI_FLUSH_DW_USE_GTT = 1u << 2;
constexpr uint32_t MI_BATCH_BUFFER_START = 0x31u << 23;
constexpr uint32_t MI_BB_PPGTT = 1u << 8;
constexpr uint32_t MI_OPCODE_MASK = 0x3Fu << 23;
constexpr uint32_t PREPARSER_DISABLE = MI_ARB_CHECK | (1u << 8);   // | state (xe_ring_ops.c) (satoru)

// ---- blitter commands (xe/instructions/xe_gpu_commands.h, i915 intel_migrate.c) (satoru) ----
constexpr uint32_t XY_FAST_COLOR_BLT = (2u << 29) | (0x44u << 22);
constexpr uint32_t XY_FAST_COLOR_BLT_DEPTH_32 = 2u << 19;
constexpr int XY_FAST_COLOR_BLT_DW = 16;
constexpr uint32_t XY_FAST_COPY_BLT = (2u << 29) | (0x42u << 22);
constexpr uint32_t XY_FAST_COPY_BLT_DEPTH_32 = 3u << 24;
constexpr int XY_FAST_COPY_BLT_DW = 10;
constexpr uint32_t BLT_OPCODE_MASK = (7u << 29) | (0x7Fu << 22);

// ---- pat and mocs for xe-lpg (xe_pat.c xelpg_pat_table, xe_mocs.c mtl_mocs_desc) (satoru) ----
// pat index 2 is uncached (what a scanned buffer needs), 3 is write-back with
// one way coherency: the gpu's accesses snoop the cpu caches, so it reads what
// the cpu wrote without a clflush. 4 is two way. (satoru)
constexpr int PAT_WB = 0, PAT_WT = 1, PAT_UC = 2, PAT_WB_1WAY = 3, PAT_WB_2WAY = 4;
constexpr int PAT_ENTRIES = 5;
constexpr uint32_t PAT_VALUES[PAT_ENTRIES] = { 0x0, 0x4, 0xC, 0x2, 0x3 };   // l4 policy 3:2, coherency 1:0 (satoru)
constexpr int MOCS_ENTRIES = 16;
constexpr int MOCS_UC_INDEX = 9;                   // "uncached, l3 no lookup, go to memory" (satoru)
uint32_t MocsControl(int index);                   // the GLOB_MOCS value linux programs for index (satoru)
constexpr uint32_t BLIT_CCTL_VALUE = ((uint32_t)MOCS_UC_INDEX << 9) | ((uint32_t)MOCS_UC_INDEX << 1);

// ---- context image (xe_lrc.c empty_lrc_data + xe_lrc_ctx_init, i915 dg2_xcs_offsets) (satoru) ----
struct LrcParams {
    uint32_t mmio_base;
    uint32_t ring_ggtt;      // ggtt address of the ring (satoru)
    uint32_t ring_bytes;     // power of two pages, 4k to 2m (satoru)
    uint64_t pml4_phys;      // root of the context's ppgtt (satoru)
};
// fills the 1024 dword register state page (the page after the pphwsp). (satoru)
void LrcBuild(uint32_t* state, const LrcParams& p);
// what the hardware is handed in the execlist submit queue: valid, 64 bit
// legacy addressing, ppgtt, the sw context id at bit 39 (xe_execlist.c). (satoru)
uint64_t CtxDescriptor(uint32_t lrc_ggtt, uint32_t ctx_id);

// ---- ppgtt, 4 levels of 4k pages (xe_vm.c xelp_pte_encode_*, xe_gtt_defs.h) (satoru) ----
uint64_t PpgttPte(uint64_t phys, int pat_index);    // leaf (satoru)
uint64_t PpgttPde(uint64_t table_phys);             // non leaf, tables are pat 3 like xe (satoru)
struct Ppgtt {
    uint64_t pml4;                                   // physical address of the root table (satoru)
    void* ctx;
    uint64_t (*alloc_table)(void* ctx);              // a zeroed 4k page, 0 when out of memory (satoru)
    uint64_t* (*table)(void* ctx, uint64_t phys);    // cpu view of a table page (satoru)
    uint32_t tables;                                 // pages handed out so far (satoru)
};
// map pages 4k pages at va (identity callers pass va == phys). an already
// present leaf is overwritten. false when a table page cannot be had. (satoru)
bool PpgttMap(Ppgtt& g, uint64_t va, uint64_t phys, uint64_t pages, int pat_index);
// software walk, for the model and the tests. (satoru)
bool PpgttWalk(const Ppgtt& g, uint64_t va, uint64_t* phys, uint64_t* pte);

// ---- ring and batch emission (xe_ring_ops.c __emit_job_gen12_simple) (satoru) ----
constexpr int JOB_DWORDS = 20;
// one job: tlb invalidate + start seqno, BLIT_CCTL, the batch, flush + seqno.
// dw has room for JOB_DWORDS; returns the count written (always even). (satoru)
int EmitJob(uint32_t* dw, uint32_t mmio_base, uint32_t start_seqno_ggtt, uint32_t seqno_ggtt,
            uint32_t seqno, uint64_t batch_va);
// rect limits of the two blits: 16 bit coordinates, 16 bit copy pitch. (satoru)
bool BlitRectOk(uint32_t pitch, int x, int y, int w, int h);
int EmitFill(uint32_t* b, int i, uint64_t dst_va, uint32_t dst_pitch, int x, int y, int w, int h, uint32_t color);
int EmitCopy(uint32_t* b, int i, uint64_t dst_va, uint32_t dst_pitch, int dx, int dy,
             uint64_t src_va, uint32_t src_pitch, int sx, int sy, int w, int h);
int EmitBatchEnd(uint32_t* b, int i);
// MI_FLUSH_DW with a store to scratch_ggtt: drains the blitter write cache and is a command barrier,
// what i915 emits between dependent blits (gen12_emit_flush_xcs). 4 dwords. (satoru)
int EmitFlushDw(uint32_t* b, int i, uint32_t scratch_ggtt);

// ---- software model of the blitter command streamer (satoru) ----
// it does what a submit does on hardware, from the same bytes: reads the
// context image through the ggtt, runs the ring from head to tail, follows the
// batch through the ppgtt and executes the blits on the mapped memory. it is
// strict: an unknown opcode, a wrong length, an address without a mapping or a
// malformed image stops it with a reason. it proves the builders agree with
// this file's reading of the formats, not that the hardware does. (satoru)
struct SimEnv {
    void* ctx;
    uint8_t* (*ggtt_page)(void* ctx, uint32_t ggtt_addr);   // 4k page behind a ggtt address, or null (satoru)
    uint8_t* (*phys_page)(void* ctx, uint64_t phys);        // 4k page of ram, or null (satoru)
};
struct SimResult {
    bool ok;
    const char* err;
    uint32_t err_arg;
    uint32_t cmds, fills, copies, stores;
    uint64_t blit_bytes;
    uint32_t blit_cctl;
    bool tlb_invalidated;
};
bool SimSubmit(const SimEnv& env, uint32_t mmio_base, uint64_t descriptor, SimResult* r);

} // namespace IgpuGt
// end (satoru)
