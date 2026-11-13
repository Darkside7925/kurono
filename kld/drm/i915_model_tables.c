// i915 register model data (d4.1): values the model device presents before
// the driver writes a register. every entry names its source line in
// scratch/kld/gfx/survey_regs.md; values marked "model" are chosen so probe
// proceeds where the real cold value is unknown (the d4.4 read-only run
// records the real ones). (satoru)

#include <linux/types.h>
#include "drm/i915_model_tables.h"

const struct i915m_init i915m_init_table[] = {
    /* group 1: identity, straps, fuses (survey section 1) */
    { 0x000d8c, 0x03118000 },   /* GMD_ID_GRAPHICS 12.70 (model) */
    { 0x380d8c, 0x03400000 },   /* GMD_ID_MEDIA 13.00 (model) */
    { 0x0510a0, 0x03800000 },   /* GMD_ID_DISPLAY 14.0 */
    { 0x10100c, 0x00000200 },   /* GU_CNTL_PROTECTED DEPRESENT */
    { 0x051000, 0x00000000 },   /* SKL_DFSM nothing fused off */
    { 0x051004, 0x40000000 },   /* SKL_DSSM cdclk ref 38.4 MHz */
    { 0x00a26c, 0x00000000 },   /* CTC_MODE */
    { 0x000d00, 0x00000016 },   /* RPM_CONFIG0 crystal 38.4, ctc shift 3 */
    { 0x009134, 0x000000ff },   /* XEHP_EU_ENABLE */
    { 0x00913c, 0x0000ffff },   /* GEOMETRY_DSS_ENABLE */
    { 0x009144, 0x0000ffff },   /* COMPUTE_DSS_ENABLE */
    { 0x009114, 0x00000070 },   /* XEHP_FUSE4 */
    { 0x138010, 0x00000038 },   /* MTL_GT_ACTIVITY_FACTOR */
    { 0x389140, 0x00010005 },   /* media VEBOX_VDBOX enable: vcs0 vcs2 vecs0 */
    { 0x00911c, 0x0f000000 },   /* HSW_PAVP_FUSE1 sfc enable */
    { 0x120010, 0x00000000 },   /* HSW_EDRAM_CAP none */
    { 0x108040, 0x0000f3c0 },   /* GGC ggms 3, gms 0xf3 = 16 MiB dsm (model) */
    { 0x138914, 0x00000000 },   /* MTL_PCODE_STOLEN_ACCESS: bar path */
    { 0x042000, 0x8fff0000 },   /* SKL_FUSE_STATUS download done + pg0..pg11 */
    { 0x041000, 0x80000000 },   /* CPU_VGACNTRL vga already disabled (model) */
    { 0x045700, 0x00000221 },   /* MTL_MEM_SS_INFO_GLOBAL ddr5 2ch 2 qgv (model) */
    { 0x045710, 0x20200120 },   /* QGV point 0 low (model) */
    { 0x045714, 0x00004020 },   /* QGV point 0 high (model) */
    { 0x045718, 0x20200120 },   /* QGV point 1 low (model) */
    { 0x04571c, 0x00004020 },   /* QGV point 1 high (model) */
    { 0x045780, 0x000f0003 },   /* MTL_LATENCY_LP0_LP1 (model) */
    { 0x045784, 0x002d001e },   /* LP2_LP3 */
    { 0x045788, 0x0064005a },   /* LP4_LP5 */
    { 0x04578c, 0x00000014 },   /* MTL_LATENCY_SAGV */
    /* ddi buffers idle at reset (survey section 5) */
    { 0x064000, 0x00000080 },
    { 0x064100, 0x00000080 },
    { 0x064300, 0x00000080 },
    { 0x064400, 0x00000080 },
    { 0x064500, 0x00000080 },
    { 0x064600, 0x00000080 },
};
const unsigned int i915m_init_count =
    sizeof(i915m_init_table) / sizeof(i915m_init_table[0]);

// end (satoru)
