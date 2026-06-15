//  kurono os  -  intel iwlwifi wifi radio driver implementation (satoru)
//  see wifi_iwl.h. implements WifiRadioOps for intel wireless nics and registers
//  it with net/ieee80211. cross-referenced from linux iwlwifi for the hardware
//  behaviour; original kurono code. (satoru)

#include "wifi_iwl.h"
#include "wifi_dev.h"
#include "serial.h"
#include "timer.h"
#include "../net/ieee80211.h"
#include "../kernel/pmm.h"     // dma-coherent (identity-mapped, page-aligned) rings (satoru)
#include "../fs/kvfs.h"        // read the iwlwifi-*.ucode blob from the kurono fs (satoru)
#include <string.h>

// ════════════════════════════════════════════════════════════════════════════
//  register definitions  -  ref: linux iwlwifi/iwl-csr.h, iwl-fh.h, iwl-prph.h
//  these are the same offsets the real hardware decodes; named lowercase per the
//  kurono convention but kept 1:1 with the linux macro names in comments. (satoru)
// ════════════════════════════════════════════════════════════════════════════

// ── CSR (control/status registers)  -  directly pci-mapped, no nic-access needed.
//    ref: iwl-csr.h (satoru)
#define CSR_HW_IF_CONFIG_REG    0x000   // hardware interface config (satoru)
#define CSR_INT_COALESCING      0x004
#define CSR_INT                 0x008   // host interrupt status/ack (satoru)
#define CSR_INT_MASK            0x00c   // host interrupt enable (satoru)
#define CSR_FH_INT_STATUS       0x010   // busmaster (dma) int status/ack (satoru)
#define CSR_RESET               0x020   // busmaster enable, nmi, sw reset (satoru)
#define CSR_GP_CNTRL            0x024   // general-purpose control (mac clock/init) (satoru)
#define CSR_HW_REV              0x028   // hardware revision (satoru)
#define CSR_EEPROM_GP           0x030
#define CSR_GIO_REG             0x03c
#define CSR_GP_UCODE_REG        0x048
#define CSR_UCODE_DRV_GP1       0x054
#define CSR_UCODE_DRV_GP1_SET   0x058
#define CSR_UCODE_DRV_GP1_CLR   0x05c
#define CSR_MBOX_SET_REG        0x088
#define CSR_LED_REG             0x094
#define CSR_DRAM_INT_TBL_REG    0x0a0
#define CSR_HW_RF_ID            0x09c   // rf id revision (satoru)
#define CSR_GIO_CHICKEN_BITS    0x100   // pcie link power-management workarounds (satoru)
#define CSR_ANA_PLL_CFG         0x20c   // analog phase-lock-loop config (satoru)
#define CSR_DBG_HPET_MEM_REG    0x240
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250
#define CSR_HW_REV_WA_REG       0x22c

// HBUS (host bus)  -  indirect access to internal sram + periphery. these need the
// mac powered up (grab-nic-access). ref: iwl-csr.h §HBUS (satoru)
#define HBUS_TARG_MEM_RADDR     0x40c
#define HBUS_TARG_MEM_WADDR     0x410
#define HBUS_TARG_MEM_WDAT      0x418
#define HBUS_TARG_MEM_RDAT      0x41c
#define HBUS_TARG_PRPH_WADDR    0x444
#define HBUS_TARG_PRPH_RADDR    0x448
#define HBUS_TARG_PRPH_WDAT     0x44c
#define HBUS_TARG_PRPH_RDAT     0x450
#define HBUS_TARG_WRPTR         0x460   // per-queue tx/rx write pointer (satoru)

// CSR_HW_IF_CONFIG_REG bits (satoru)
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE       0x00080000
#define CSR_HW_IF_CONFIG_REG_PCI_OWN_SET        0x00400000
#define CSR_HW_IF_CONFIG_REG_WAKE_ME            0x08000000
#define CSR_HW_IF_CONFIG_REG_PREPARE            0x08000000   // alias: prepare-me (satoru)

// CSR_RESET bits (satoru)
#define CSR_RESET_REG_FLAG_SW_RESET             0x00000080
#define CSR_RESET_REG_FLAG_MASTER_DISABLED      0x00000100
#define CSR_RESET_REG_FLAG_STOP_MASTER          0x00000200
#define CSR_RESET_LINK_PWR_MGMT_DISABLED        0x80000000

// CSR_GP_CNTRL bits (pre-bz / gen1+gen2 families we target) (satoru)
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY   0x00000001
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE         0x00000004
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ    0x00000008
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP    0x00000010
#define CSR_GP_CNTRL_REG_FLAG_XTAL_ON           0x00000400
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW     0x08000000   // platform rf-kill switch (satoru)

// CSR_GIO bits (satoru)
#define CSR_GIO_REG_VAL_L0S_DISABLED            0x00000002

// CSR_GIO_CHICKEN_BITS (satoru)
#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX      0x00800000
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER 0x20000000

// CSR_DBG_HPET_MEM_REG fh-wait-threshold workaround value (satoru)
#define CSR_DBG_HPET_MEM_REG_VAL                0xFFFF0000

// CSR_ANA_PLL pll config value (5xxx-style; pll_cfg parts only) (satoru)
#define CSR50_ANA_PLL_CFG_VAL                   0x00880300

// CSR_MBOX_SET_REG os-alive bit (satoru)
#define CSR_MBOX_SET_REG_OS_ALIVE               (1u << 5)

// CSR_INT interrupt cause bits (set by ucode/dma, acked by writing 1). ref:
// iwl-csr.h §CSR_INT_BIT_* (satoru)
#define CSR_INT_BIT_FH_RX        (1u << 31)  // rx dma + cmd responses (satoru)
#define CSR_INT_BIT_HW_ERR       (1u << 29)
#define CSR_INT_BIT_RX_PERIODIC  (1u << 28)
#define CSR_INT_BIT_FH_TX        (1u << 27)  // tx dma (satoru)
#define CSR_INT_BIT_SW_ERR       (1u << 25)  // ucode error (satoru)
#define CSR_INT_BIT_RF_KILL      (1u << 7)
#define CSR_INT_BIT_SW_RX        (1u << 3)   // rx, command responses (satoru)
#define CSR_INT_BIT_WAKEUP       (1u << 1)
#define CSR_INT_BIT_ALIVE        (1u << 0)   // ucode raises once it inits (satoru)

// the interrupt mask we arm: rx + tx + the error/alive/wakeup events. ref:
// iwl-csr.h CSR_INI_SET_MASK (satoru)
#define CSR_INI_SET_MASK        (CSR_INT_BIT_FH_RX | CSR_INT_BIT_HW_ERR | \
                                 CSR_INT_BIT_FH_TX | CSR_INT_BIT_SW_ERR | \
                                 CSR_INT_BIT_RF_KILL | CSR_INT_BIT_SW_RX | \
                                 CSR_INT_BIT_WAKEUP | CSR_INT_BIT_ALIVE | \
                                 CSR_INT_BIT_RX_PERIODIC)

// FH (flow handler) interrupt bits in CSR_FH_INT_STATUS. ref: iwl-csr.h (satoru)
#define CSR_FH_INT_BIT_ERR       (1u << 31)
#define CSR_FH_INT_BIT_HI_PRIOR  (1u << 30)
#define CSR_FH_INT_BIT_RX_CHNL0  (1u << 16)
#define CSR_FH_INT_BIT_TX_CHNL0  (1u << 0)
#define CSR_FH_INT_RX_MASK      (CSR_FH_INT_BIT_HI_PRIOR | CSR_FH_INT_BIT_RX_CHNL0 | (1u<<17))
#define CSR_FH_INT_TX_MASK      (CSR_FH_INT_BIT_TX_CHNL0 | (1u<<1))

// CSR_DRAM_INT_TBL (ict  -  interrupt coalescing table; we leave it disabled and
// use the legacy non-ict isr path which is simpler for a bring-up). (satoru)

// ── FH (flow-handler / busmaster dma) registers. these are CSR-class (directly
//    mapped, no nic-access). ref: iwl-fh.h (satoru)
#define FH_MEM_LOWER_BOUND              0x1000
// keep-warm buffer base (dummy dram accesses to hold dram out of low-power). it
// must be a 4k-aligned phys addr >> 4. ref: iwl-fh.h FH_KW_MEM_ADDR_REG (satoru)
#define FH_KW_MEM_ADDR_REG             (FH_MEM_LOWER_BOUND + 0x97C)
// tfd circular-buffer base pointers, one dword per queue, phys >> 8. queue 0..15
// at +0x9D0. ref: iwl-fh.h FH_MEM_CBBC_0_15_LOWER_BOUND (satoru)
#define FH_MEM_CBBC_0_15_LOWER_BOUND   (FH_MEM_LOWER_BOUND + 0x9D0)
#define FH_MEM_CBBC_QUEUE(q)           (FH_MEM_CBBC_0_15_LOWER_BOUND + 4 * (q))

// rx ring (rscsr/rcsr/rssr)  -  legacy single-rx-queue path. ref: iwl-fh.h (satoru)
#define FH_MEM_RSCSR_LOWER_BOUND       (FH_MEM_LOWER_BOUND + 0xBC0)
#define FH_MEM_RSCSR_CHNL0             (FH_MEM_RSCSR_LOWER_BOUND)
#define FH_RSCSR_CHNL0_STTS_WPTR_REG   (FH_MEM_RSCSR_CHNL0)         // rx status buf base >>4 (satoru)
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG  (FH_MEM_RSCSR_CHNL0 + 0x004) // rbd cb base >>8 (satoru)
#define FH_RSCSR_CHNL0_RBDCB_WPTR_REG  (FH_MEM_RSCSR_CHNL0 + 0x008) // rx write index (satoru)
#define FH_RSCSR_CHNL0_RDPTR           (FH_MEM_RSCSR_CHNL0 + 0x00c) // rx read index (satoru)

#define FH_MEM_RCSR_LOWER_BOUND        (FH_MEM_LOWER_BOUND + 0xC00)
#define FH_MEM_RCSR_CHNL0_CONFIG_REG   (FH_MEM_RCSR_LOWER_BOUND)    // rx dma config (satoru)
#define FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL    0x80000000          // dma channel enable (satoru)
#define FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY       0x00000004
#define FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL  0x00001000
#define FH_RCSR_CHNL0_RX_CONFIG_SINGLE_FRAME_MSK 0x00008000
#define FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K    0x00000000          // 4k receive buffers (satoru)
#define FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS        20                  // log2(#rbds) field (satoru)
#define FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS      4                   // rb timeout field (satoru)
#define RX_RB_TIMEOUT                  0x11

#define FH_MEM_RSSR_LOWER_BOUND        (FH_MEM_LOWER_BOUND + 0xC40)
#define FH_MEM_RSSR_SHARED_CTRL_REG    (FH_MEM_RSSR_LOWER_BOUND)
#define FH_MEM_RSSR_RX_STATUS_REG      (FH_MEM_RSSR_LOWER_BOUND + 0x004)
#define FH_MEM_RSSR_RX_ENABLE_ERR_IRQ2DRV  (FH_MEM_RSSR_LOWER_BOUND + 0x008)
#define FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE   0x01000000              // rx channel idle (satoru)

// tx dma channel config (tcsr) + status (tssr). ref: iwl-fh.h (satoru)
#define FH_TCSR_LOWER_BOUND            (FH_MEM_LOWER_BOUND + 0xD00)
#define FH_TCSR_CHNL_TX_CONFIG_REG(c)  (FH_TCSR_LOWER_BOUND + 0x20 * (c))
#define FH_TCSR_TX_CONFIG_REG_VAL_MSG_MODE_TXF          0x00000000
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE     0x00000008
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_IFTFD       0x00200000
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_RTC_IFTFD        0x00800000
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE       0x80000000
#define FH_TSSR_LOWER_BOUND            (FH_MEM_LOWER_BOUND + 0xEA0)
#define FH_TSSR_TX_MSG_CONFIG_REG      (FH_TSSR_LOWER_BOUND + 0x008)
#define FH_TSSR_TX_STATUS_REG          (FH_TSSR_LOWER_BOUND + 0x010)
#define FH_TX_CHICKEN_BITS_REG         (FH_MEM_LOWER_BOUND + 0xE98)
#define FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN  0x00000002

// ── PRPH (periphery) registers  -  internal, NOT pci-mapped. accessed indirectly
//    via HBUS_TARG_PRPH_*. ref: iwl-prph.h (satoru)
#define APMG_BASE                      0x3000
#define APMG_CLK_CTRL_REG              (APMG_BASE + 0x0000)
#define APMG_CLK_EN_REG                (APMG_BASE + 0x0004)
#define APMG_CLK_DIS_REG               (APMG_BASE + 0x0008)
#define APMG_PS_CTRL_REG               (APMG_BASE + 0x000c)
#define APMG_PCIDEV_STT_REG            (APMG_BASE + 0x0010)
#define APMG_RTC_INT_STT_REG           (APMG_BASE + 0x001c)
#define APMG_CLK_VAL_DMA_CLK_RQT       0x00000200       // request dma clock (satoru)
#define APMG_PS_CTRL_MSK_PWR_SRC       0x03000000
#define APMG_PS_CTRL_VAL_PWR_SRC_VMAIN 0x00000000       // main power (not aux) (satoru)
#define APMG_PS_CTRL_VAL_PWR_SRC_VAUX  0x02000000
#define APMG_PCIDEV_STT_VAL_L1_ACT_DIS 0x00000800       // disable l1-active (satoru)
#define APMG_RTC_INT_STT_RFKILL        0x10000000

// scheduler (scd) prph base  -  used for tx-queue chain/credit setup on real hw.
// ref: iwl-prph.h / iwl-scd.h. left minimal here (UNVERIFIED full scd setup needs
// the mvm fw api + real silicon). (satoru)
#define SCD_BASE                       0xa02c00

// ════════════════════════════════════════════════════════════════════════════
//  dma ring layout  -  ref: iwl-fh.h (TFD / RBD formats) (satoru)
// ════════════════════════════════════════════════════════════════════════════

// rx: 256 receive-buffer descriptors, each a 4k buffer. the legacy rbd is just
// the 4k-aligned phys address >> 8 packed in a 32-bit word. ref: iwl-fh.h §rx
// (the gen1 single-rx-queue rbd-circular-buffer). (satoru)
#define IWL_RX_RING_COUNT     256
#define IWL_RX_LOG2_COUNT     8          // log2(256) for the rcsr rbdcb-size field (satoru)
#define IWL_RX_BUF_SIZE       4096       // 4k receive buffers (satoru)

// tx: 256 transmit frame descriptors per queue. one tfd points at up to 20 tbs
// (transmit buffers); we use a single tb per frame (the whole 802.11 frame in one
// contiguous dram block). ref: iwl-fh.h struct iwl_tfd / iwl_tfd_tb. (satoru)
#define IWL_TX_RING_COUNT     256
#define IWL_NUM_OF_TBS        20
#define IWL_TX_BUF_SIZE       4096

// the queue we use for management/data tx during bring-up. real iwlwifi uses a
// dedicated cmd queue + per-ac data queues negotiated with the fw scheduler; for
// a single-queue bring-up we drive queue 0. (satoru)
#define IWL_TX_QUEUE          0

// one transmit-buffer descriptor inside a tfd. lo = low 32 bits of the tb dma
// address; hi_n_len = high-4-bits-of-addr in [3:0] and length in [15:4]. ref:
// iwl-fh.h struct iwl_tfd_tb + enum iwl_tfd_tb_hi_n_len. (satoru)
struct IwlTfdTb {
    uint32_t lo;
    uint16_t hi_n_len;
} __attribute__((packed));

// the legacy 128-byte transmit frame descriptor. ref: iwl-fh.h struct iwl_tfd.
// (satoru)
struct IwlTfd {
    uint8_t  reserved1[3];
    uint8_t  num_tbs;                 // active tbs in [4:0] (satoru)
    IwlTfdTb tbs[IWL_NUM_OF_TBS];
    uint32_t pad;
} __attribute__((packed));

// the 8-byte rx status buffer the hardware writes the closed-write-pointer into.
// ref: iwl-fh.h FH_RSCSR_CHNL0_STTS_WPTR_REG. (satoru)
struct IwlRbStatus {
    uint16_t closed_rb_num;           // lower 12 bits valid (satoru)
    uint16_t closed_fr_num;
    uint16_t finished_rb_num;
    uint16_t finished_fr_num;
    uint32_t unused;
} __attribute__((packed));

// the per-rx-buffer packet header the device prepends to each received frame in
// the 4k rb. ref: iwl-fh.h struct iwl_rx_packet (len_n_flags + cmd header). the
// 802.11 frame (with the rx phy-info command payload) follows. exact offsets vary
// by fw api; the maintainer tunes the payload offset on real silicon. (satoru)
struct IwlRxPacketHdr {
    uint32_t len_n_flags;             // [13:0] = byte count of the rest (satoru)
    uint8_t  cmd;                     // rx command id (satoru)
    uint8_t  flags;
    uint8_t  seq[2];
} __attribute__((packed));
#define IWL_RX_LEN_MASK   0x00003FFF

// ════════════════════════════════════════════════════════════════════════════
//  driver state
// ════════════════════════════════════════════════════════════════════════════
struct IwlState {
    bool      registered;             // RegisterRadio called (satoru)
    bool      started;                // start() succeeded  -  rings live, ucode loaded (satoru)
    bool      fw_loaded;              // ucode dma'd in and alive seen (satoru)
    const WifiDevice* dev;

    uint32_t  hw_rev;                 // CSR_HW_REV (satoru)
    uint32_t  hw_rf_id;              // CSR_HW_RF_ID (satoru)

    // dma rings (all from PMM::AllocBytes -> identity-mapped, phys==virt). (satoru)
    IwlTfd*       tx_tfds;            // 256 tfds for queue 0 (satoru)
    uint8_t*      tx_bufs;            // 256 * 4k tx staging buffers (satoru)
    uint32_t      tx_head, tx_tail;   // ring indices (satoru)

    uint32_t*     rbd;               // 256 rx-buffer descriptors (phys>>8 words) (satoru)
    uint8_t*      rx_bufs;           // 256 * 4k rx buffers (satoru)
    IwlRbStatus*  rb_status;         // hw-written rx status (satoru)
    uint32_t      rx_read;          // our read index into the rbd ring (satoru)

    uint8_t*      kw;                // keep-warm 4k buffer (satoru)

    // current channel / bss / signal cached for the ops. (satoru)
    int           channel;
    uint8_t       bssid[6];
    int           last_rssi;        // dBm; -100 until a frame arrives (satoru)
};

static IwlState g_iwl = {};

// the WifiRadioOps vtable the stack calls. filled at TryRegister. (the struct is
// declared inside namespace Ieee80211, so it's qualified here.) (satoru)
static Ieee80211::WifiRadioOps g_iwl_ops = {};

// ════════════════════════════════════════════════════════════════════════════
//  low-level register access. CSR/FH are directly mapped via WifiDev::Reg*.
//  prph is indirect through the hbus window and needs the mac powered up. (satoru)
// ════════════════════════════════════════════════════════════════════════════

static inline uint32_t r32(uint32_t off)            { return WifiDev::RegRead(off); }
static inline void     w32(uint32_t off, uint32_t v){ WifiDev::RegWrite(off, v); }

// set/clear bits preserving the rest (the iwl_set_bit / iwl_clear_bit idiom  -  the
// hardware leaves default bits set after reset that we must not stomp). (satoru)
static inline void set_bit(uint32_t off, uint32_t mask)   { w32(off, r32(off) | mask); }
static inline void clear_bit(uint32_t off, uint32_t mask) { w32(off, r32(off) & ~mask); }

// busy-wait until (read(off) & mask) == mask, up to timeout_us. returns true on
// success. uses Timer::GetTicks() (ms) for the coarse bound plus a tight spin so
// short (microsecond) polls don't sleep a whole ms. ref: iwl-io.c iwl_poll_bit.
// (satoru)
static bool poll_bit(uint32_t off, uint32_t mask, uint32_t timeout_us) {
    uint32_t deadline_ms = Timer::GetTicks() + (timeout_us / 1000) + 2;
    for (;;) {
        if ((r32(off) & mask) == mask) return true;
        if (Timer::GetTicks() >= deadline_ms) {
            // last chance read (the deadline check can race a just-set bit) (satoru)
            return (r32(off) & mask) == mask;
        }
        for (volatile int j = 0; j < 200; j++) {}   // ~microsecond-scale spin (satoru)
    }
}

// short microsecond delay via a calibrated-ish busy loop. the pit timer is ms-
// granular, so sub-ms waits spin. not precise, but the iwl sequences only need
// "at least N us" ordering, which a generous spin satisfies. (satoru)
static void udelay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++)
        for (volatile int j = 0; j < 300; j++) {}
}

// grab nic access: assert MAC_ACCESS_REQ and wait for MAC_CLOCK_READY with no
// GOING_TO_SLEEP, so the mac (ucode processor) is awake and internal sram/prph is
// reachable. required before any hbus prph/mem access. ref: iwl-io.c
// iwl_grab_nic_access. (satoru)
static bool grab_nic_access() {
    set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    return poll_bit(CSR_GP_CNTRL,
                    CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                    15000);
}

static void release_nic_access() {
    clear_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

// indirect prph read/write through the hbus window. the address register encodes
// the prph offset; for a write the data reg is loaded first. caller must already
// hold nic access. ref: iwl-io.c iwl_read_prph_no_grab / iwl_write_prph_no_grab.
// (satoru)
static uint32_t read_prph(uint32_t ofs) {
    w32(HBUS_TARG_PRPH_RADDR, ((ofs & 0x000FFFFF) | (3u << 24)));
    return r32(HBUS_TARG_PRPH_RDAT);
}
static void write_prph(uint32_t ofs, uint32_t val) {
    w32(HBUS_TARG_PRPH_WADDR, ((ofs & 0x000FFFFF) | (3u << 24)));
    w32(HBUS_TARG_PRPH_WDAT, val);
}
static void set_bits_prph(uint32_t ofs, uint32_t mask) {
    write_prph(ofs, read_prph(ofs) | mask);
}
static void set_bits_mask_prph(uint32_t ofs, uint32_t bits, uint32_t mask) {
    uint32_t v = read_prph(ofs);
    write_prph(ofs, (v & ~mask) | (bits & mask));
}

// ════════════════════════════════════════════════════════════════════════════
//  apm (advanced power management) + nic reset/init.
//  ref: linux iwlwifi/pcie/gen1_2/trans.c  -  iwl_pcie_apm_init,
//  iwl_pcie_set_hw_ready, iwl_pcie_prepare_card_hw, iwl_trans_pcie_sw_reset,
//  iwl_pcie_gen1_2_activate_nic. (satoru)
// ════════════════════════════════════════════════════════════════════════════

// activate the nic: assert INIT_DONE to move D0U*->D0A* and wait for the mac
// clock to come ready, so prph/sram access works. ref: gen1_2 activate_nic (the
// pre-bz path: INIT_DONE + poll MAC_CLOCK_READY). (satoru)
static bool apm_activate_nic() {
    set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    bool ok = poll_bit(CSR_GP_CNTRL,
                       CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                       25000);
    if (!ok) SerialLogger::Log("[iwl] activate_nic: mac clock not ready\r\n");
    return ok;
}

// ask the hardware for pci ownership and confirm it was granted. ref: gen1_2
// iwl_pcie_set_hw_ready. (satoru)
static bool set_hw_ready() {
    set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PCI_OWN_SET);
    bool ok = poll_bit(CSR_HW_IF_CONFIG_REG,
                       CSR_HW_IF_CONFIG_REG_PCI_OWN_SET,
                       50000);
    if (ok) set_bit(CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
    return ok;
}

// take ownership of the card, retrying via the WAKE_ME poke if the management
// engine (csme) still holds it. ref: gen1_2 iwl_pcie_prepare_card_hw. (satoru)
static bool prepare_card_hw() {
    if (set_hw_ready()) return true;

    set_bit(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
    udelay(2000);

    for (int iter = 0; iter < 10; iter++) {
        set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_WAKE_ME);
        for (int t = 0; t < 15; t++) {
            if (set_hw_ready()) return true;
            udelay(1000);
        }
    }
    SerialLogger::Log("[iwl] prepare_card_hw: could not take ownership\r\n");
    return false;
}

// software-reset the whole controller (results in SHRD_HW_RST), then re-take
// ownership. ref: gen1_2 iwl_trans_pcie_sw_reset (pre-bz path: CSR_RESET sw-reset
// + ~5ms settle). (satoru)
static bool sw_reset(bool retake_ownership) {
    set_bit(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    udelay(6000);
    if (retake_ownership) return prepare_card_hw();
    return true;
}

// bring up the card's basic functionality after reset (no ucode yet). mirrors
// iwl_pcie_apm_init: disable l0s, set the chicken/hpet workarounds, enable HAP
// wake, activate the nic (init-done + clock-ready), then enable the dma clock and
// disable l1-active via apmg prph. ref: gen1_2 iwl_pcie_apm_init. (satoru)
static bool apm_init() {
    SerialLogger::Log("[iwl] apm_init: bringing up basic nic functions\r\n");

    // disable the l0s exit timer (platform nmi w/a  -  applies to <8000 family; we
    // set it unconditionally, it is harmless on newer parts). (satoru)
    set_bit(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);
    // disable l0s without affecting l1; don't wait for ich l0s (ich bug w/a). (satoru)
    set_bit(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
    // fh wait threshold to max (hw-error-under-stress w/a). (satoru)
    set_bit(CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);
    // enable HAP INTA so the device can wake its pcie link l1a->l0s. (satoru)
    set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE);
    // l0s disabled in gio (unstable on these devices). (satoru)
    set_bit(CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);

    if (!apm_activate_nic())
        return false;

    // enable the dma clock and let it stabilise, then disable l1-active and clear
    // any apmg rfkill interrupt. these are apmg-prph (periphery) accesses, valid
    // now that the mac clock is ready. parts without apmg (newer ax / gen2) skip
    // this  -  UNVERIFIED which exact device families we get; the apmg writes are
    // ignored by the prph mux on parts that lack the block. (satoru)
    write_prph(APMG_CLK_EN_REG, APMG_CLK_VAL_DMA_CLK_RQT);
    udelay(20);
    set_bits_prph(APMG_PCIDEV_STT_REG, APMG_PCIDEV_STT_VAL_L1_ACT_DIS);
    write_prph(APMG_RTC_INT_STT_REG, APMG_RTC_INT_STT_RFKILL);

    SerialLogger::Log("[iwl] apm_init: device enabled\r\n");
    return true;
}

// power source select: main (vmain) vs aux (vaux). on apmg parts this is an apmg
// prph write. ref: gen1_2 iwl_pcie_set_pwr. (satoru)
static void apm_set_pwr_vmain() {
    set_bits_mask_prph(APMG_PS_CTRL_REG,
                       APMG_PS_CTRL_VAL_PWR_SRC_VMAIN,
                       APMG_PS_CTRL_MSK_PWR_SRC);
}

// quiesce: stop the busmaster, clear init-done to drop to low power. ref: gen1_2
// iwl_pcie_apm_stop_master + iwl_pcie_apm_stop. (satoru)
static void apm_stop() {
    set_bit(CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);
    poll_bit(CSR_RESET, CSR_RESET_REG_FLAG_MASTER_DISABLED, 100000);
    clear_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

// is the hardware rf-kill switch engaged? if so the radio can't tx. ref: gen1_2
// iwl_is_rfkill_set (reads CSR_GP_CNTRL bit 27). (satoru)
static bool hw_rfkill_set() {
    return (r32(CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0;
    // NOTE on real hw the bit is "1 = switch NOT killing"; the sense is inverted
    // per platform wiring. UNVERIFIED polarity  -  confirm on silicon. (satoru)
}

// ════════════════════════════════════════════════════════════════════════════
//  dma ring allocation + setup. all rings come from PMM::AllocBytes, which hands
//  back page-aligned, zeroed, contiguous, identity-mapped (phys==virt) memory  - 
//  exactly the dma-coherent property the device needs. the returned pointer value
//  IS the physical address for the descriptor base registers. (satoru)
// ════════════════════════════════════════════════════════════════════════════

static inline uint64_t dma_phys(const void* p) { return (uint64_t)(uintptr_t)p; }

static bool alloc_rings() {
    // tx: 256 tfds (128 bytes each => 32k) + 256 4k staging buffers (1m). (satoru)
    g_iwl.tx_tfds = (IwlTfd*)PMM::AllocBytes(sizeof(IwlTfd) * IWL_TX_RING_COUNT);
    g_iwl.tx_bufs = (uint8_t*)PMM::AllocBytes((size_t)IWL_TX_BUF_SIZE * IWL_TX_RING_COUNT);
    // rx: 256-entry rbd ring (1k) + status buf + 256 4k rx buffers (1m). (satoru)
    g_iwl.rbd       = (uint32_t*)PMM::AllocBytes(sizeof(uint32_t) * IWL_RX_RING_COUNT);
    g_iwl.rb_status = (IwlRbStatus*)PMM::AllocBytes(sizeof(IwlRbStatus));
    g_iwl.rx_bufs   = (uint8_t*)PMM::AllocBytes((size_t)IWL_RX_BUF_SIZE * IWL_RX_RING_COUNT);
    // keep-warm 4k. (satoru)
    g_iwl.kw        = (uint8_t*)PMM::AllocBytes(IWL_RX_BUF_SIZE);

    if (!g_iwl.tx_tfds || !g_iwl.tx_bufs || !g_iwl.rbd || !g_iwl.rb_status ||
        !g_iwl.rx_bufs || !g_iwl.kw) {
        SerialLogger::Log("[iwl] alloc_rings: out of dma memory\r\n");
        return false;
    }
    g_iwl.tx_head = g_iwl.tx_tail = 0;
    g_iwl.rx_read = 0;
    return true;
}

// program + enable the rx dma ring (legacy single-queue rscsr/rcsr path). ref:
// iwl-fh.h §rx + linux iwl_pcie_rx_hw_init (gen1). steps: stop rx dma, point the
// rbd-cb base + status-buf base + write-pointer regs at our rings, then enable the
// channel with 4k buffers / 256 rbds / host-irq. (satoru)
static bool rx_hw_init() {
    // fill the rbd ring: each entry = (4k buffer phys >> 8). 256 buffers. (satoru)
    for (int i = 0; i < IWL_RX_RING_COUNT; i++) {
        uint64_t bp = dma_phys(g_iwl.rx_bufs + (uint64_t)i * IWL_RX_BUF_SIZE);
        g_iwl.rbd[i] = (uint32_t)(bp >> 8);
    }

    if (!grab_nic_access()) return false;

    // stop rx dma while we reconfigure. (satoru)
    write_prph(0x0, 0);   // no-op guard to ensure prph mux is warm (satoru)
    w32(FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);

    // reset the rx read/write pointers. (satoru)
    w32(FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);
    w32(FH_RSCSR_CHNL0_RDPTR, 0);

    // rbd circular-buffer base (phys >> 8). (satoru)
    w32(FH_RSCSR_CHNL0_RBDCB_BASE_REG, (uint32_t)(dma_phys(g_iwl.rbd) >> 8));
    // rx status buffer base (phys >> 4). (satoru)
    w32(FH_RSCSR_CHNL0_STTS_WPTR_REG, (uint32_t)(dma_phys(g_iwl.rb_status) >> 4));

    // enable the rx dma channel: normal-operate, 4k buffers, 256 rbds, host irq,
    // single-frame + rb timeout. ref: iwl-fh.h FH_MEM_RCSR_CHNL0_CONFIG bit fields.
    // (satoru)
    uint32_t cfg = FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL
                 | FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY
                 | FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL
                 | FH_RCSR_CHNL0_RX_CONFIG_SINGLE_FRAME_MSK
                 | FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K
                 | (RX_RB_TIMEOUT << FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS)
                 | (IWL_RX_LOG2_COUNT << FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS);
    w32(FH_MEM_RCSR_CHNL0_CONFIG_REG, cfg);

    // hand all 256 rbds to the hardware: write pointer = count-1, aligned to 8. the
    // device consumes rbds up to this index. ref: iwl_pcie_rxq_inc_wr_ptr. (satoru)
    w32(FH_RSCSR_CHNL0_RBDCB_WPTR_REG, (IWL_RX_RING_COUNT - 1) & ~7u);

    release_nic_access();
    SerialLogger::Log("[iwl] rx ring enabled (256x4k)\r\n");
    return true;
}

// program the tx dma ring for our queue + the keep-warm buffer. ref: iwl-fh.h
// §tx (FH_MEM_CBBC_QUEUE / FH_KW_MEM_ADDR_REG / FH_TCSR_CHNL_TX_CONFIG) + linux
// iwl_pcie_tx_init. (satoru)
static bool tx_hw_init() {
    if (!grab_nic_access()) return false;

    // keep-warm buffer base (4k-aligned phys >> 4). (satoru)
    w32(FH_KW_MEM_ADDR_REG, (uint32_t)(dma_phys(g_iwl.kw) >> 4));

    // tfd circular-buffer base for our queue (phys >> 8). (satoru)
    w32(FH_MEM_CBBC_QUEUE(IWL_TX_QUEUE), (uint32_t)(dma_phys(g_iwl.tx_tfds) >> 8));

    // global tx-dma message config + chicken bits (scd auto-retry). ref: iwl-fh.h
    // FH_TSSR_TX_MSG_CONFIG_REG. value mirrors the linux default credit/arb mode.
    // (satoru)
    w32(FH_TX_CHICKEN_BITS_REG,
        r32(FH_TX_CHICKEN_BITS_REG) | FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN);

    // enable the tx dma channel for our queue: dma enable, credit enable, irq on
    // end-of-tfd (host + rtc). ref: iwl-fh.h FH_TCSR_TX_CONFIG bit fields. (satoru)
    w32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_TX_QUEUE),
        FH_TCSR_TX_CONFIG_REG_VAL_MSG_MODE_TXF
      | FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE
      | FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_IFTFD
      | FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_RTC_IFTFD
      | FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE);

    release_nic_access();

    // NOTE: a full tx path also needs the firmware scheduler (scd) queue chain +
    // the per-queue byte-count table + a tx command (TX_CMD) wrapper carrying the
    // rate/antenna/flags  -  all part of the mvm fw api. that handshake needs the
    // alive notification + real silicon. UNVERIFIED beyond the fh-level ring here.
    // (satoru)
    SerialLogger::Log("[iwl] tx ring enabled (queue 0)\r\n");
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  firmware (ucode) load.
//  ref: linux iwlwifi/iwl-drv.c (the .ucode tlv parser) + fw/file.h (the file
//  format) + pcie/gen1_2/trans.c iwl_pcie_load_given_ucode (the section dma).
//
//  the iwlwifi-*.ucode file is a tlv container: an iwl_tlv_ucode_header (zero +
//  magic 'IWL\n' + human-readable string + ver/build) followed by tlv records
//  {type:u32, length:u32, data[length]} padded to 4 bytes. the runtime code/data
//  live in IWL_UCODE_TLV_SEC_RT (19) sections (or the older INST/DATA tlvs); each
//  SEC_RT section is {dst_addr:u32, bytes...}. the driver dma's each section into
//  device sram and then kicks the embedded processor, which raises CSR_INT_BIT_
//  ALIVE. the firmware path is /system/lib/firmware/<name> (the stack does not
//  ship it; the maintainer drops the blob there). (satoru)
// ════════════════════════════════════════════════════════════════════════════

// the firmware directory we read ucode from. /system resolves through the kvfs
// compat symlinks into the canonical /kurono tree. (satoru)
#define IWL_FW_DIR   "/system/lib/firmware/"

// little-endian fetch helpers (the file + descriptors are le; x86 is le so these
// are plain loads, but kept explicit for clarity + portability). (satoru)
static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define IWL_TLV_UCODE_MAGIC      0x0a4c5749u   // 'IWL\n' (ref: fw/file.h) (satoru)
#define IWL_UCODE_TLV_INST       1
#define IWL_UCODE_TLV_DATA       2
#define IWL_UCODE_TLV_SEC_RT     19

// dma one ucode section into device sram via the FH busmaster section loader. on
// real iwlwifi this uses FH_TFDIB / TFD-init-buffer registers (iwl_pcie_load_
// section) which dma `byte_count` from a host buffer to `dst_addr` in nic memory.
// for the bring-up we use the hbus sram window (HBUS_TARG_MEM_WADDR/WDAT) which is
// slower but correct + needs no extra fh setup. it copies dword-by-dword with
// auto-increment. ref: iwl-csr.h §HBUS_TARG_MEM. caller holds nic access. (satoru)
static void load_section_to_sram(uint32_t dst_addr, const uint8_t* data, uint32_t len) {
    w32(HBUS_TARG_MEM_WADDR, dst_addr);
    uint32_t words = (len + 3) / 4;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t off = i * 4;
        uint32_t v = 0;
        // assemble the (possibly short) trailing dword. (satoru)
        for (uint32_t b = 0; b < 4 && off + b < len; b++)
            v |= (uint32_t)data[off + b] << (b * 8);
        w32(HBUS_TARG_MEM_WDAT, v);   // auto-increments the write address (satoru)
    }
}

// parse the tlv container and dma every runtime section into sram. returns the
// number of sections loaded, or -1 on a malformed blob. (satoru)
static int parse_and_load_ucode(const uint8_t* blob, uint32_t total) {
    if (total < 8) { SerialLogger::Log("[iwl] ucode too small\r\n"); return -1; }

    // tlv header: zero(4) magic(4) human_readable(64) ver(4) build(4) ignore(8). a
    // non-tlv (legacy v1/v2) header has a non-zero first dword; we only support the
    // modern tlv format that every ax/ac part ships. ref: fw/file.h
    // iwl_tlv_ucode_header. (satoru)
    if (le32(blob) != 0 || le32(blob + 4) != IWL_TLV_UCODE_MAGIC) {
        SerialLogger::Log("[iwl] ucode: not a tlv container (bad magic)\r\n");
        return -1;
    }
    const uint32_t hdr_len = 4 + 4 + 64 + 4 + 4 + 8;   // 88 bytes (satoru)
    if (total < hdr_len) return -1;

    if (!grab_nic_access()) {
        SerialLogger::Log("[iwl] ucode: no nic access for sram load\r\n");
        return -1;
    }

    int sections = 0;
    uint32_t pos = hdr_len;
    while (pos + 8 <= total) {
        uint32_t type = le32(blob + pos);
        uint32_t len  = le32(blob + pos + 4);
        pos += 8;
        if (pos + len > total) {
            SerialLogger::Log("[iwl] ucode: tlv overruns file\r\n");
            break;
        }
        const uint8_t* tlv = blob + pos;

        if (type == IWL_UCODE_TLV_SEC_RT && len >= 4) {
            // SEC_RT: dst_addr(4) then the section bytes. ref: iwl-drv.c
            // iwl_store_ucode_sec / IWL_UCODE_TLV_SEC_RT. (satoru)
            uint32_t dst = le32(tlv);
            load_section_to_sram(dst, tlv + 4, len - 4);
            sections++;
        } else if (type == IWL_UCODE_TLV_INST) {
            // legacy instruction blob -> loads at sram inst base. UNVERIFIED base
            // per family; on tlv files this rarely appears. (satoru)
            load_section_to_sram(0x00000000, tlv, len);
            sections++;
        } else if (type == IWL_UCODE_TLV_DATA) {
            load_section_to_sram(0x00800000, tlv, len);   // data sram base (typ) (satoru)
            sections++;
        }
        // tlv records are 4-byte padded. ref: fw/file.h note. (satoru)
        pos += (len + 3) & ~3u;
    }

    release_nic_access();
    SerialLogger::Log("[iwl] ucode: loaded ");
    SerialLogger::LogDec(sections);
    SerialLogger::Log(" runtime section(s) into sram\r\n");
    return sections;
}

// per-model ucode filename hints. iwlwifi names the blob by hardware; the exact
// api-version suffix (e.g. -72.ucode) varies, so read_ucode_file tries the model-
// specific base name first, then a couple of generic fallbacks. the maintainer
// drops the right file under /system/lib/firmware/. ref: iwlwifi/cfg/*.c
// fw_name_pre tables. (satoru)
struct FwName { uint16_t dev; const char* name; };
static const FwName k_fw_names[] = {
    { 0x2723, "iwlwifi-cc-a0.ucode"      },   // ax200 (satoru)
    { 0x2725, "iwlwifi-ty-a0-gf-a0.ucode"},   // ax210 (satoru)
    { 0x7af0, "iwlwifi-so-a0-gf-a0.ucode"},   // ax211 (satoru)
    { 0x51f0, "iwlwifi-so-a0-gf-a0.ucode"},
    { 0xa0f0, "iwlwifi-QuZ-a0-hr-b0.ucode"},  // ax201 (satoru)
    { 0x4df0, "iwlwifi-QuZ-a0-hr-b0.ucode"},
    { 0x06f0, "iwlwifi-Qu-c0-hr-b0.ucode" },
    { 0x9df0, "iwlwifi-9000-pu-b0-jf-b0.ucode" }, // 9560 (satoru)
    { 0xa370, "iwlwifi-9000-pu-b0-jf-b0.ucode" },
    { 0x2526, "iwlwifi-9260-th-b0-jf-b0.ucode" }, // 9260 (satoru)
    { 0x24fd, "iwlwifi-8265-36.ucode"     },  // 8265 (satoru)
    { 0x24f3, "iwlwifi-8000C-36.ucode"    },  // 8260 (satoru)
    { 0x095a, "iwlwifi-7265D-29.ucode"    },  // 7265 (satoru)
    { 0x095b, "iwlwifi-7265D-29.ucode"    },
    { 0x08b1, "iwlwifi-7260-17.ucode"     },  // 7260 (satoru)
    { 0x3165, "iwlwifi-7265D-29.ucode"    },  // 3165 (satoru)
    { 0x24fb, "iwlwifi-3168-29.ucode"     },  // 3168 (satoru)
};

// a small staging buffer for the firmware blob. the kvfs per-file ceiling is 4mb
// and ucode blobs are ~1-2.5mb; we read into a dma-coherent buffer so the section
// loader can also (on real hw) point the fh dma at it directly. (satoru)
#define IWL_FW_MAX   (3u * 1024 * 1024)

// read the ucode file for this device from the kurono fs into a buffer, returning
// the byte count (0 on any failure / not found). fills *out_buf with a PMM buffer
// the caller frees. (satoru)
static uint32_t read_ucode_file(uint16_t device, uint8_t** out_buf) {
    *out_buf = nullptr;

    // build the candidate name list: the model-specific hint first, then a few
    // wildcards the maintainer might have used. (satoru)
    char path[160];
    const char* chosen = nullptr;
    for (unsigned i = 0; i < sizeof(k_fw_names) / sizeof(k_fw_names[0]); i++) {
        if (k_fw_names[i].dev == device) { chosen = k_fw_names[i].name; break; }
    }
    // also try a generic "iwlwifi.ucode" the maintainer can symlink. (satoru)
    const char* tries[3];
    int nt = 0;
    if (chosen) tries[nt++] = chosen;
    tries[nt++] = "iwlwifi.ucode";
    tries[nt++] = "iwlwifi-default.ucode";

    for (int t = 0; t < nt; t++) {
        // path = IWL_FW_DIR + tries[t] (separate src/dst indices  -  concatenate the
        // dir then the name, bounded by sizeof(path)). (satoru)
        int p = 0;
        const char* d = IWL_FW_DIR;
        for (int i = 0; d[i] && p < 120; i++) path[p++] = d[i];
        for (int q = 0; tries[t][q] && p < 159; q++) path[p++] = tries[t][q];
        path[p] = 0;

        int sz = KVFS::GetFileSize(path);
        if (sz <= 0) continue;
        if ((uint32_t)sz > IWL_FW_MAX) {
            SerialLogger::Log("[iwl] ucode file too large, skipping: ");
            SerialLogger::Log(path); SerialLogger::Log("\r\n");
            continue;
        }
        uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)sz);
        if (!buf) return 0;
        int got = KVFS::ReadFile(path, buf, (uint32_t)sz);
        if (got <= 0) { PMM::FreeBytes(buf, (size_t)sz); continue; }

        SerialLogger::Log("[iwl] ucode: read ");
        SerialLogger::LogDec(got);
        SerialLogger::Log(" bytes from ");
        SerialLogger::Log(path);
        SerialLogger::Log("\r\n");
        *out_buf = buf;
        return (uint32_t)got;
    }

    SerialLogger::Log("[iwl] ucode: no firmware found under " IWL_FW_DIR "\r\n");
    SerialLogger::Log("[iwl]   drop iwlwifi-*.ucode there (the OS does not ship it)\r\n");
    return 0;
}

// load + start the firmware: dma the runtime sections into sram, kick the
// embedded processor, and wait for the ALIVE interrupt. ref: iwl-drv.c
// iwl_load_ucode_wait_alive + gen1_2 iwl_pcie_load_given_ucode + the alive
// handshake. (satoru)
static bool fw_load_and_start(const uint8_t* blob, uint32_t len) {
    if (parse_and_load_ucode(blob, len) <= 0)
        return false;

    // clear stale interrupts, then kick the processor out of reset. on real
    // iwlwifi this is done by clearing CSR_RESET (the cpu-reset bit) after the
    // sections are in place; the ucode then posts CSR_INT_BIT_ALIVE. ref: gen1_2
    // iwl_pcie_load_given_ucode tail. (satoru)
    w32(CSR_INT, 0xFFFFFFFF);                       // ack all (satoru)
    w32(CSR_RESET, 0);                              // release cpu reset (satoru)

    // wait for the alive notification (the ucode raises CSR_INT_BIT_ALIVE once it
    // initialises). poll the int-status register since our isr path is minimal.
    // ref: iwl-csr.h CSR_INT_BIT_ALIVE. (satoru)
    bool alive = poll_bit(CSR_INT, CSR_INT_BIT_ALIVE, 1000000);   // up to ~1s (satoru)
    if (alive) {
        w32(CSR_INT, CSR_INT_BIT_ALIVE);            // ack (satoru)
        SerialLogger::Log("[iwl] ucode ALIVE\r\n");
        return true;
    }

    // honesty: without the device we cannot observe ALIVE. report it plainly; the
    // sections are in sram and the cpu was released  -  the maintainer confirms the
    // alive handshake (and the subsequent fw INIT/calibration phase) on silicon.
    // (satoru)
    SerialLogger::Log("[iwl] ucode: ALIVE not observed (expected without real hw)\r\n");
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
//  rx drain  -  pull received frames out of the rbd ring and push to the stack.
//  ref: iwl-fh.h §rx (the closed-rb write pointer in rb_status) + linux
//  iwl_pcie_rx_handle. (satoru)
// ════════════════════════════════════════════════════════════════════════════

// pull at most one frame; returns its length copied into buf, 0 if none. updates
// the rssi cache from the rx info if present. (satoru)
static int rx_drain_one(uint8_t* buf, int buf_max) {
    if (!g_iwl.started || !g_iwl.rb_status) return 0;

    // the device writes the index of the last closed (filled) rb here. (satoru)
    uint16_t hw_closed = g_iwl.rb_status->closed_rb_num & 0x0FFF;
    if ((g_iwl.rx_read & 0x0FFF) == hw_closed)
        return 0;   // nothing new (satoru)

    int idx = g_iwl.rx_read % IWL_RX_RING_COUNT;
    const uint8_t* rb = g_iwl.rx_bufs + (uint64_t)idx * IWL_RX_BUF_SIZE;

    // the rb begins with the rx-packet header giving the payload byte count. the
    // 802.11 frame sits after the rx command's phy-info; the exact payload offset
    // is fw-api-specific. we expose the header length and hand the body region to
    // the stack. UNVERIFIED payload offset  -  the maintainer pins it on silicon.
    // (satoru)
    const IwlRxPacketHdr* h = (const IwlRxPacketHdr*)rb;
    uint32_t plen = le32((const uint8_t*)&h->len_n_flags) & IWL_RX_LEN_MASK;

    g_iwl.rx_read = (g_iwl.rx_read + 1) & 0x0FFF;

    if (plen == 0 || plen > IWL_RX_BUF_SIZE) return 0;

    // skip the rx packet header to reach the command payload; a real mvm rx_mpdu
    // command then carries an rx_status struct + the 802.11 frame. for the bring-
    // up we surface the bytes after the header as-is. (satoru)
    int hdr = (int)sizeof(IwlRxPacketHdr);
    int body = (int)plen - hdr;
    if (body <= 0) return 0;
    if (body > buf_max) body = buf_max;
    memcpy(buf, rb + hdr, (size_t)body);
    return body;
}

// ════════════════════════════════════════════════════════════════════════════
//  WifiRadioOps implementations
// ════════════════════════════════════════════════════════════════════════════

bool WifiIwl::op_start(void* ctx) {
    (void)ctx;
    if (g_iwl.started) return true;
    if (!g_iwl.dev || !g_iwl.dev->mmio_mapped) {
        SerialLogger::Log("[iwl] start: mmio not mapped\r\n");
        return false;
    }

    SerialLogger::Log("[iwl] start: bringing up ");
    SerialLogger::Log(g_iwl.dev->model);
    SerialLogger::Log("\r\n");

    // mask interrupts during bring-up. (satoru)
    w32(CSR_INT_MASK, 0);
    w32(CSR_INT, 0xFFFFFFFF);
    w32(CSR_FH_INT_STATUS, 0xFFFFFFFF);

    // take ownership, reset, and bring up basic functions. ref: gen1_2 start_hw
    // -> prepare_card_hw -> sw_reset -> apm_init. (satoru)
    if (!prepare_card_hw()) return false;
    if (!sw_reset(true))    return false;
    if (!apm_init())        return false;
    apm_set_pwr_vmain();

    // read hw revision now that the mac is up (diagnostic + family checks). (satoru)
    g_iwl.hw_rev   = r32(CSR_HW_REV);
    g_iwl.hw_rf_id = r32(CSR_HW_RF_ID);
    SerialLogger::Log("[iwl] HW_REV=");
    SerialLogger::LogHex(g_iwl.hw_rev);
    SerialLogger::Log(" RF_ID=");
    SerialLogger::LogHex(g_iwl.hw_rf_id);
    SerialLogger::Log("\r\n");

    if (hw_rfkill_set())
        SerialLogger::Log("[iwl] warning: hw rf-kill appears engaged\r\n");

    // allocate + program the dma rings. (satoru)
    if (!alloc_rings()) return false;
    if (!rx_hw_init())  return false;
    if (!tx_hw_init())  return false;

    // arm interrupts (we still poll, but the device expects the mask set). (satoru)
    w32(CSR_INT_MASK, CSR_INI_SET_MASK);

    // load + start the firmware. if absent or alive isn't observed, we fail start
    // gracefully (the stack reports "no radio could start"). the rings + apm stay
    // configured so a maintainer with the blob + hardware proceeds from here.
    // (satoru)
    uint8_t* fw = nullptr;
    uint32_t fwlen = read_ucode_file(g_iwl.dev->device, &fw);
    if (fwlen == 0) {
        SerialLogger::Log("[iwl] start: failing gracefully  -  firmware required\r\n");
        return false;
    }
    bool ok = fw_load_and_start(fw, fwlen);
    PMM::FreeBytes(fw, (size_t)fwlen);
    g_iwl.fw_loaded = ok;

    if (!ok) {
        // hardware + rings are up but the ucode didn't come alive (no device).
        // honest failure  -  do NOT pretend the radio is operational. (satoru)
        return false;
    }

    g_iwl.started   = true;
    g_iwl.last_rssi = -100;
    SerialLogger::Log("[iwl] start: radio operational\r\n");
    return true;
}

void WifiIwl::op_stop(void* ctx) {
    (void)ctx;
    if (!g_iwl.dev || !g_iwl.dev->mmio_mapped) return;

    // stop rx dma, mask + ack interrupts, quiesce the apm. ref: gen1_2
    // iwl_pcie_apm_stop / _iwl_trans_pcie_stop_device. (satoru)
    if (grab_nic_access()) {
        w32(FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
        w32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_TX_QUEUE), 0);
        release_nic_access();
    }
    w32(CSR_INT_MASK, 0);
    w32(CSR_INT, 0xFFFFFFFF);
    apm_stop();

    g_iwl.started   = false;
    g_iwl.fw_loaded = false;
    SerialLogger::Log("[iwl] stop: radio quiesced\r\n");
}

bool WifiIwl::op_set_channel(void* ctx, int ch) {
    (void)ctx;
    g_iwl.channel = ch;
    // tuning the phy is a firmware command: on the mvm api this is a PHY_CONTEXT_
    // CMD (add/modify) carrying the band + channel + width, or on dvm parts an
    // RXON with the channel. that requires the live fw command queue (post-alive).
    // we cache the channel and report success only if the fw is up  -  otherwise the
    // synth never actually moves, and lying would race the scan dwell. since we
    // can't issue the cmd without silicon, return whether fw is loaded. ref: mvm
    // phy_ctxt_cmd. UNVERIFIED  -  needs real hw. (satoru)
    if (!g_iwl.fw_loaded) return false;
    // (real impl: build + enqueue PHY_CONTEXT_CMD on the cmd queue, wait for the
    // response, then return.) (satoru)
    return true;
}

bool WifiIwl::op_config_bss(void* ctx, const uint8_t bssid[6], const char* ssid) {
    (void)ctx; (void)ssid;
    if (bssid) memcpy(g_iwl.bssid, bssid, 6);
    // programming the bss (rx address filter / MAC_CTXT_CMD with the bssid) is a
    // firmware command on the mvm api. cache it; the real filter program needs the
    // live cmd queue. ref: mvm mac_ctxt_cmd / rxon. UNVERIFIED. (satoru)
    return g_iwl.fw_loaded;
}

bool WifiIwl::op_set_key(void* ctx, int idx, const uint8_t* key, int key_len, int type) {
    (void)ctx; (void)idx; (void)key; (void)key_len; (void)type;
    // we do NOT offload crypto. returning false tells the 802.11 stack to do ccmp
    // in software (wifi_crypto.cpp), which is correct + already implemented. a real
    // hw-offload path would send an ADD_STA / set-key fw command. (satoru)
    return false;
}

bool WifiIwl::op_tx_frame(void* ctx, const uint8_t* buf, int len) {
    (void)ctx;
    if (!g_iwl.started || !buf || len <= 0 || len > IWL_TX_BUF_SIZE) return false;

    // copy the frame into the next tx staging buffer and build a single-tb tfd.
    // ref: iwl-fh.h struct iwl_tfd / iwl_tfd_tb. NOTE: a real frame must be wrapped
    // in a TX_CMD (rate/antenna/flags + the 802.11 frame) and the queue advanced
    // through the fw scheduler; without the live fw scheduler the device won't
    // actually transmit. we build the fh-level tfd faithfully so the path is
    // exercised + ready, and bump the hw write pointer. UNVERIFIED tx completion
    // needs real hw + the mvm tx api. (satoru)
    int slot = g_iwl.tx_head % IWL_TX_RING_COUNT;
    uint8_t* sbuf = g_iwl.tx_bufs + (uint64_t)slot * IWL_TX_BUF_SIZE;
    memcpy(sbuf, buf, (size_t)len);

    IwlTfd* tfd = &g_iwl.tx_tfds[slot];
    memset(tfd, 0, sizeof(*tfd));
    tfd->num_tbs = 1;
    uint64_t bp = dma_phys(sbuf);
    tfd->tbs[0].lo = (uint32_t)(bp & 0xFFFFFFFF);
    // hi_n_len: bits[3:0] = addr[35:32], bits[15:4] = length. ref: iwl-fh.h enum
    // iwl_tfd_tb_hi_n_len. (satoru)
    tfd->tbs[0].hi_n_len = (uint16_t)(((bp >> 32) & 0xF) | ((uint32_t)len << 4));

    g_iwl.tx_head = (g_iwl.tx_head + 1) % IWL_TX_RING_COUNT;

    // advance the device's tx write pointer for our queue. ref: iwl-csr.h
    // HBUS_TARG_WRPTR (queue selector in [11:8], index in [7:0]). (satoru)
    if (grab_nic_access()) {
        w32(HBUS_TARG_WRPTR, (g_iwl.tx_head & 0xFF) | (IWL_TX_QUEUE << 8));
        release_nic_access();
    }
    return true;
}

int WifiIwl::op_rx_poll(void* ctx, uint8_t* buf, int buf_max) {
    (void)ctx;
    if (!buf || buf_max <= 0) return 0;
    return rx_drain_one(buf, buf_max);
}

int WifiIwl::op_get_signal(void* ctx) {
    (void)ctx;
    return g_iwl.last_rssi;   // -100 until a frame's phy-info updates it (satoru)
}

bool WifiIwl::op_load_firmware(void* ctx, const uint8_t* blob, int len) {
    (void)ctx;
    // the stack may hand us a blob directly; otherwise start() sources it from the
    // fs. either way the dma + alive handshake is the same. (satoru)
    if (!blob || len <= 0) return false;
    bool ok = fw_load_and_start(blob, (uint32_t)len);
    g_iwl.fw_loaded = ok;
    return ok;
}

// ════════════════════════════════════════════════════════════════════════════
//  registration
// ════════════════════════════════════════════════════════════════════════════

bool WifiIwl::Active() { return g_iwl.registered; }

bool WifiIwl::TryRegister() {
    if (g_iwl.registered) return true;

    if (!WifiDev::Present())
        return false;
    const WifiDevice* d = WifiDev::Info();
    if (!d || d->family != WIFI_FAM_INTEL_IWLWIFI)
        return false;

    // capture the device. we do NOT power up the radio here (that happens lazily in
    // start()); registration just wires the vtable so Scan/Connect can drive it.
    // (satoru)
    g_iwl = IwlState{};
    g_iwl.dev       = d;
    g_iwl.channel   = 0;
    g_iwl.last_rssi = -100;

    if (!d->mmio_mapped)
        SerialLogger::Log("[iwl] warning: mmio window not live  -  start() will fail\r\n");

    // fill the ops vtable. (satoru)
    g_iwl_ops.start         = &WifiIwl::op_start;
    g_iwl_ops.stop          = &WifiIwl::op_stop;
    g_iwl_ops.set_channel   = &WifiIwl::op_set_channel;
    g_iwl_ops.config_bss    = &WifiIwl::op_config_bss;
    g_iwl_ops.set_key       = &WifiIwl::op_set_key;
    g_iwl_ops.tx_frame      = &WifiIwl::op_tx_frame;
    g_iwl_ops.rx_poll       = &WifiIwl::op_rx_poll;
    g_iwl_ops.get_signal    = &WifiIwl::op_get_signal;
    g_iwl_ops.load_firmware = &WifiIwl::op_load_firmware;

    Ieee80211::RegisterRadio(&g_iwl_ops, &g_iwl, (WifiDevice*)d);
    g_iwl.registered = true;

    SerialLogger::Log("[iwl] registered intel radio: ");
    SerialLogger::Log(d->model);
    SerialLogger::Log(" (");
    SerialLogger::LogHex(d->vendor);
    SerialLogger::Log(":");
    SerialLogger::LogHex(d->device);
    SerialLogger::Log(")\r\n");
    return true;
}
// end (satoru)
