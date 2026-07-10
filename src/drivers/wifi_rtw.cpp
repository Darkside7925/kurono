//  kurono os - realtek rtw pcie wifi radio driver (satoru)
//  see wifi_rtw.h. implements Ieee80211::WifiRadioOps for rtl8821ce / rtl8723be
//  / rtl8822ce / rtl8812ae. original kurono code; hardware behaviour re-derived
//  from linux rtw88 + rtlwifi (each block cites its ref). (satoru)

#include "wifi_rtw.h"
#include "wifi_dev.h"
#include "serial.h"
#include "timer.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../net/ieee80211.h"
#include "../fs/kvfs.h"

namespace WifiRtw {

// ── realtek register map (ref: linux rtw88 reg.h / rtlwifi/wifi.h) ──── (satoru)
//
//  these offsets are stable across the rtl88xx/87xx family - the system-control
//  block (0x00xx), the mac sub-system regs (0x01xx), the fifo/llt regs (0x02xx),
//  the dma ring base regs (0x03xx-0x05xx), and the rcr/rx-filter regs. only the
//  power-sequence *contents* and a few phy regs differ per chip. (satoru)

// system control (ref: rtw88 reg.h REG_SYS_*) (satoru)
#define REG_SYS_FUNC_EN      0x0002   // function enable (mac/bb/usb reset bits) (satoru)
#define REG_APS_FSMCO        0x0004   // power-state fsm control (satoru)
#define REG_SYS_CLK_CTRL     0x0008   // system clock enable (satoru)
#define REG_9346CR           0x000A   // eeprom/efuse command + autoload-done (satoru)
#define REG_EE_VPD           0x000C
#define REG_AFE_MISC         0x0010   // analog-front-end misc (satoru)
#define REG_SPS0_CTRL        0x0011
#define REG_SPS_OCP_CFG      0x0018
#define REG_RSV_CTRL         0x001C   // protection for the power-seq regs (satoru)
#define REG_RF_CTRL          0x001F   // rf enable / reset (satoru)
#define REG_LDOA15_CTRL      0x0020
#define REG_LDOV12D_CTRL     0x0021
#define REG_AFE_XTAL_CTRL    0x0024   // crystal control (satoru)
#define REG_AFE_PLL_CTRL     0x0028   // pll control (satoru)
#define REG_MAC_PHY_CTRL     0x002C
#define REG_EFUSE_CTRL       0x0030   // efuse access (mac is in the efuse/otp) (satoru)
#define REG_PWR_DATA         0x0038
#define REG_CAL_TIMER        0x003C
#define REG_GPIO_MUXCFG      0x0040
#define REG_LEDCFG0          0x004C
#define REG_HSIMR            0x0058   // host-system interrupt mask (satoru)
#define REG_HSISR            0x005C
#define REG_MULTI_FUNC_CTRL  0x0068

// mac sub-system + 8051 firmware control (ref: rtw88 fw.c / rtlwifi) (satoru)
#define REG_CR               0x0100   // mac function-enable (the big "cr" reg) (satoru)
#define REG_PBP              0x0104   // page-boundary of rx/tx fifo (satoru)
#define REG_TRXDMA_CTRL      0x010C   // dma engine enable + queue->page map (satoru)
#define REG_TRXFF_BNDY       0x0114   // tx/rx fifo boundary (satoru)
#define REG_LLT_INIT         0x01E0   // link-list-table init access port (satoru)
#define REG_MCUFW_CTRL       0x0080   // 8051 firmware control + ready flag (satoru)
#define REG_MCU_TST_CFG      0x0084
#define REG_HMETFR           0x01CC   // h2c metadata "fire" trigger (satoru)
#define REG_HMEBOX_0         0x01D0   // h2c command mailbox 0 (satoru)
#define REG_HMEBOX_1         0x01D4
#define REG_HMEBOX_2         0x01D8
#define REG_HMEBOX_3         0x01DC

// fifo / page-control (ref: rtw88 mac.c rtw_mac_init / rtlwifi _init_queue) (satoru)
#define REG_RQPN             0x0200   // reserved-queue page numbers (lo prio) (satoru)
#define REG_FIFOPAGE         0x0204   // tx fifo page allocation (satoru)
#define REG_DWBCN0_CTRL      0x0208   // beacon dma control (satoru)
#define REG_TDECTRL          0x0208
#define REG_RQPN_NPQ         0x0214   // normal-priority-queue page count (satoru)
#define REG_AUTO_LLT         0x0224   // auto link-list-table build trigger (satoru)

// pci dma ring base-address + index regs (ref: rtw88 pci.c rtw_pci_reset_trx_ring)
//  each tx queue and the rx queue has a 64-bit ring base + a host/dma index pair.
//  the bcn/high/mgmt/vo/vi/be/bk queues map to tx priorities; rxq is the rx ring.
//  (the rtl8723/8812 rtlwifi layout differs slightly but uses the same 0x03xx
//  window - these are the rtw88 offsets, cited per-use.) (satoru)
#define REG_PCIE_CTRL        0x0300
#define REG_INT_MIG          0x0304   // interrupt migration (coalescing) (satoru)
#define REG_BCNQ_DESA        0x0308   // beacon queue desc-ring base addr (64-bit) (satoru)
#define REG_HQ_DESA          0x0310   // high queue (satoru)
#define REG_MGQ_DESA         0x0318   // mgmt queue (satoru)
#define REG_VOQ_DESA         0x0320   // voice (satoru)
#define REG_VIQ_DESA         0x0328   // video (satoru)
#define REG_BEQ_DESA         0x0330   // best-effort (satoru)
#define REG_BKQ_DESA         0x0338   // background (satoru)
#define REG_RX_DESA          0x0340   // rx queue desc-ring base addr (64-bit) (satoru)
#define REG_PCIE_HRPWM       0x0361
#define REG_PCIE_HCPWM       0x0363

// per-queue host write-index regs (satoru)
#define REG_MGQ_TXBD_IDX     0x03A4   // mgmt queue host index (satoru)
#define REG_VOQ_TXBD_IDX     0x03AC
#define REG_VIQ_TXBD_IDX     0x03B4
#define REG_BEQ_TXBD_IDX     0x03BC
#define REG_BKQ_TXBD_IDX     0x03C4
#define REG_RXQ_RXBD_IDX     0x03D4   // rx queue host/dma index pair (satoru)

// interrupt status/mask (ref: rtw88 pci.c IMR_* / rtlwifi) (satoru)
#define REG_HIMR0            0x00B0   // interrupt mask 0 (satoru)
#define REG_HISR0            0x00B4   // interrupt status 0 (satoru)
#define REG_HIMR1            0x00B8
#define REG_HISR1            0x00BC

// receive-config + address filters (ref: rtw88/rtlwifi REG_RCR/REG_MAR) (satoru)
#define REG_CR_EXT          0x0102
#define REG_MAR             0x0620    // multicast address filter (8 bytes) (satoru)
#define REG_MACID           0x0610    // sta self mac (6 bytes) (satoru)
#define REG_BSSID           0x0618    // bssid we joined (6 bytes) (satoru)
#define REG_RCR             0x0608    // receive config register (satoru)
#define REG_RXFLTMAP0       0x06A0    // mgmt frame rx filter (satoru)
#define REG_RXFLTMAP1       0x06A2    // ctrl frame rx filter (satoru)
#define REG_RXFLTMAP2       0x06A4    // data frame rx filter (satoru)
#define REG_RX_PKT_LIMIT    0x060C
#define REG_RX_DRVINFO_SZ   0x060F

// phy / rf channel (ref: rtw88 phy.c / rtlwifi REG_CCK0/REG_RF) (satoru)
#define REG_CCK_CHECK       0x0454    // 2.4ghz cck-only marker (satoru)
#define REG_FPGA0_RFMOD     0x0800    // bb ofdm/cck mode (satoru)
#define REG_CCKAGC          0x0A00
#define REG_OFDM_AGC        0x0C00
#define rRfChannel          0x18      // rf reg 0x18 = channel select (satoru)

// ── bit definitions used in the bring-up ───────────────────────────── (satoru)
// REG_CR (0x0100) function-enable bits (ref: rtw88 reg.h BIT_* of REG_CR) (satoru)
#define CR_HCI_TXDMA_EN     (1u << 0)
#define CR_HCI_RXDMA_EN     (1u << 1)
#define CR_TXDMA_EN         (1u << 2)
#define CR_RXDMA_EN         (1u << 3)
#define CR_PROTOCOL_EN      (1u << 4)
#define CR_SCHEDULE_EN      (1u << 5)
#define CR_MACTXEN          (1u << 6)
#define CR_MACRXEN          (1u << 7)
#define CR_ENSWBCN          (1u << 8)
#define CR_CALTMR_EN        (1u << 10)

// REG_MCUFW_CTRL (0x0080) (ref: rtw88 fw.c) (satoru)
#define MCUFW_RAM_DL_SEL    (1u << 7)   // select ram (vs rom) download (satoru)
#define MCUFW_FWDL_EN       (1u << 0)   // enable firmware download path (satoru)
#define MCUFW_RDY           (1u << 18)  // on-chip cpu signalled "ready" (per chip) (satoru)
#define MCUFW_WINTINI_RDY   (1u << 6)   // firmware init complete (satoru)

// REG_SYS_FUNC_EN (0x0002) (satoru)
#define FEN_CPUEN           (1u << 2)   // 8051 cpu core enable (satoru)
#define FEN_BBRSTB          (1u << 0)
#define FEN_BB_GLB_RSTN     (1u << 1)

// efuse/eeprom autoload (REG_9346CR 0x000A) (satoru)
#define EEPROM_EN           (1u << 5)
#define EEPROM_BOOT         (1u << 4)
#define AUTOLOAD_OK         (1u << 1)   // autoload checksum ok (satoru)

// rcr (0x0608) receive-config bits (ref: rtw88 reg.h BIT_*RCR) (satoru)
#define RCR_AAP             (1u << 0)   // accept all physical-address frames (satoru)
#define RCR_APM             (1u << 1)   // accept frames addr'd to my mac (satoru)
#define RCR_AM              (1u << 2)   // accept multicast (satoru)
#define RCR_AB              (1u << 3)   // accept broadcast (satoru)
#define RCR_CBSSID_DATA     (1u << 4)   // check bssid for data frames (satoru)
#define RCR_CBSSID_BCN      (1u << 5)   // check bssid for beacon/probe (satoru)
#define RCR_APP_PHYST_RXFF  (1u << 9)   // append phy-status to rx fifo (signal!) (satoru)
#define RCR_APP_FCS         (1u << 31)  // keep fcs in the rx buffer (satoru)
#define RCR_HTC_LOC_CTRL    (1u << 14)

// h2c command ids the firmware understands (ref: rtw88 fw.c H2C_*) (satoru)
#define H2C_SET_CHANNEL     0x59        // UNSURE: differs per chip fw abi (satoru)
#define H2C_MEDIA_STATUS    0x01        // join/leave bss notify (satoru)

// ── tx/rx descriptor ring geometry ─────────────────────────────────── (satoru)
//  rtw88 pci uses "tx buffer descriptors" (txbd) that point at the real tx
//  descriptor + payload in host memory. we keep it simple: one ring per queue we
//  actually use (mgmt for all our tx - probe/auth/assoc/eapol - and rx). a real
//  driver fans data frames across vo/vi/be/bk by tid; the supplicant only needs
//  the mgmt path until a data path exists. (satoru)
#define RTW_RING_SZ        128         // descriptors per ring (power of two) (satoru)
#define RTW_TXBD_BYTES     16          // a tx buffer-descriptor is 4 dwords (satoru)
#define RTW_RXBD_BYTES     16          // an rx buffer-descriptor is 4 dwords (satoru)
#define RTW_TXDESC_BYTES   48          // the realtek tx descriptor header (satoru)
#define RTW_RXDESC_BYTES   24          // the realtek rx descriptor header (satoru)
#define RTW_BUF_BYTES      2048        // per-slot payload buffer (one mpdu) (satoru)

// the realtek tx descriptor header (the first 40-48 bytes of every tx packet).
//  ref: rtw88 tx.c rtw_tx_fill_tx_desc + the txdesc bit layout. it is a packed
//  array of little-endian dwords with bitfields; we expose just the fields the
//  bring-up sets (length, queue-select, mac-id, sequence, raw-mode). the rest
//  default to zero (hardware rate control, no aggregation). (satoru)
struct RtwTxDesc {
    uint32_t dw[12];                   // 48 bytes = 12 dwords (satoru)
} __attribute__((packed));

// a pci tx/rx buffer-descriptor: {length, address-low, address-high, ...}.
//  ref: rtw88 pci.c struct rtw_pci_tx_buffer_desc. (satoru)
struct RtwBufDesc {
    uint16_t buf_size;                 // bytes in this segment (satoru)
    uint16_t info;                     // own/ls/fs flags, packed (satoru)
    uint32_t dma_low;                  // payload phys addr low 32 (satoru)
    uint32_t dma_high;                 // payload phys addr high 32 (satoru)
    uint32_t reserved;
} __attribute__((packed));

// per-queue ring state (host side) (satoru)
struct RtwRing {
    RtwBufDesc* bd;                    // the buffer-descriptor array (dma) (satoru)
    uint64_t    bd_phys;               // its physical base (satoru)
    uint8_t*    buf;                   // contiguous payload buffers (dma) (satoru)
    uint64_t    buf_phys;              // payload base phys (satoru)
    uint32_t    host_idx;              // next slot we will write (satoru)
    uint32_t    hw_idx;                // last slot hardware consumed (satoru)
};

// ── chip identity (drives which power-seq / fw we use) ─────────────── (satoru)
enum RtwChip {
    RTW_CHIP_UNKNOWN = 0,
    RTW_CHIP_8821C,   // rtl8821ce (satoru)
    RTW_CHIP_8723B,   // rtl8723be (satoru)
    RTW_CHIP_8822C,   // rtl8822ce (satoru)
    RTW_CHIP_8812A,   // rtl8812ae (satoru)
};

// ── driver private state (the WifiRadioOps `ctx`) ──────────────────── (satoru)
struct RtwState {
    const WifiDevice* dev;
    RtwChip   chip;
    bool      mmio_ok;
    bool      fw_loaded;
    bool      started;
    int       channel;
    int       last_rssi;               // dBm, from rx phy-status (satoru)
    uint8_t   bssid[6];
    char      ssid[33];

    RtwRing   tx;                       // single mgmt/data tx ring (satoru)
    RtwRing   rx;                       // rx ring (satoru)
    bool      rings_ok;
};

static RtwState g_state;
static bool     g_registered = false;
static Ieee80211::WifiRadioOps g_ops;   // the vtable we register (satoru)

// ── tiny logging helpers ───────────────────────────────────────────── (satoru)
static void log(const char* s) { SerialLogger::Log(s); }
static void logx(const char* s, uint32_t v) {
    SerialLogger::Log(s); SerialLogger::LogHex(v); SerialLogger::Log("\r\n");
}

// ── width-correct mmio access ──────────────────────────────────────── (satoru)
//  WifiDev::RegRead/RegWrite are 32-bit only, but the realtek power-sequence and
//  several control regs are byte- and word-addressed (e.g. REG_CR is read-modify
//  at byte granularity in linux). we go through the mapped window directly for
//  8/16-bit, and reuse WifiDev for 32-bit so the bounds-check stays in one place.
//  (satoru)
static inline volatile uint8_t* mmio() { return g_state.dev ? g_state.dev->mmio : nullptr; }
static inline bool reg_ok(uint32_t off, uint32_t w) {
    return g_state.mmio_ok && g_state.dev && (off + w) <= g_state.dev->bar0_size;
}

static uint8_t  r8 (uint32_t off) { return reg_ok(off,1) ? *(volatile uint8_t *)(mmio()+off) : 0xFF; }
static uint16_t r16(uint32_t off) { return reg_ok(off,2) ? *(volatile uint16_t*)(mmio()+off) : 0xFFFF; }
static uint32_t r32(uint32_t off) { return WifiDev::RegRead(off); }
static void     w8 (uint32_t off, uint8_t  v) { if (reg_ok(off,1)) *(volatile uint8_t *)(mmio()+off) = v; }
static void     w16(uint32_t off, uint16_t v) { if (reg_ok(off,2)) *(volatile uint16_t*)(mmio()+off) = v; }
static void     w32(uint32_t off, uint32_t v) { WifiDev::RegWrite(off, v); }

// poll a 32-bit reg until (read & mask) == want, or timeout_ms elapses. uses the
// pit-polled real-ms clock so it works under the cooperative scheduler. (satoru)
static bool poll32(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeout_ms) {
    uint32_t start = Timer::GetTicks();
    for (;;) {
        if ((r32(off) & mask) == want) return true;
        if (Timer::GetTicks() - start > timeout_ms) return false;
        Timer::WaitMs(1);
    }
}
static bool poll8(uint32_t off, uint8_t mask, uint8_t want, uint32_t timeout_ms) {
    uint32_t start = Timer::GetTicks();
    for (;;) {
        if ((r8(off) & mask) == want) return true;
        if (Timer::GetTicks() - start > timeout_ms) return false;
        Timer::WaitMs(1);
    }
}

// ── the realtek power-on sequence (THE signature realtek bring-up) ──── (satoru)
//
//  ref: linux rtw88 mac.c (rtw_pwr_seq_parser) + the per-chip pwr_seq_cmd
//  tables in rtw8821c_tables.c / rtw8822c_tables.c, and rtlwifi's
//  Hal_HwPwrSeqCmdParsing for the 8723be/8812ae. realtek represents power-up as
//  a list of register commands; each one is "in field `mask` of byte reg `off`,
//  write `val`" (and a few poll/delay opcodes). running the card-enable list
//  brings the mac analog + digital domains and the 8051 out of reset. (satoru)
//
//  the exact bytes are chip- and cut-specific; the *structure* below is the
//  common card-enable flow shared by these parts. UNSURE entries are marked - 
//  a maintainer cross-checks them against the chip's tables.c on real hw. we
//  model the four opcodes linux uses: WRITE (rmw a byte field), POLL (wait for a
//  field), DELAY (us/ms), and END. (satoru)
enum PwrCmd { PWR_WRITE, PWR_POLL, PWR_DELAY_US, PWR_DELAY_MS, PWR_END };
struct PwrStep {
    uint8_t  cmd;
    uint16_t off;     // byte register offset (satoru)
    uint8_t  mask;    // which bits this step touches (satoru)
    uint8_t  val;     // value (for WRITE: the bits to set within mask; POLL: expected) (satoru)
};

// card-enable flow (power up). this is the common shape across rtl8821c/8822c
//  and the 8723b/8812a; bytes that vary per chip are flagged. (satoru)
static const PwrStep kPwrOn[] = {
    // 1. disable the power-seq register protection so the regs below take. (satoru)
    { PWR_WRITE, REG_RSV_CTRL,    0xFF, 0x00 },                 // clear rsv_ctrl lock (satoru)
    // 2. release the analog power-down: clear APFM_OFFMAC / wake from deep sleep.
    //    APS_FSMCO bit ~ APFM_OFFMAC (off=0x0005 bit1 in linux) - UNSURE per chip.
    { PWR_WRITE, REG_APS_FSMCO+1, 0x02, 0x00 },                 // UNSURE: clear off-mac (satoru)
    { PWR_POLL,  REG_APS_FSMCO+1, 0x02, 0x00 },                 // wait until powered (satoru)
    // 3. enable the AFE: turn on the macro-block (BIT_MAC_PWR ...). (satoru)
    { PWR_WRITE, REG_SYS_CLK_CTRL, 0x08, 0x08 },                // analog clock to mac (satoru)
    // 4. bring up the crystal + pll. (satoru)
    { PWR_WRITE, REG_AFE_XTAL_CTRL, 0x40, 0x00 },               // gate off xtal pdn (satoru)
    { PWR_DELAY_US, 0, 0, 60 },
    // 5. enable LDO + switching power supply. (satoru)
    { PWR_WRITE, REG_SPS0_CTRL,   0xFF, 0x2B },                 // UNSURE: sps to active (satoru)
    { PWR_DELAY_US, 0, 0, 100 },
    // 6. release the digital core: function-enable mac. (satoru)
    { PWR_WRITE, REG_CR,          0x00, 0x00 },                 // settle CR (satoru)
    { PWR_WRITE, REG_CR,          0xFF, 0xFF },                 // raw write of low byte (satoru)
    { PWR_END,   0, 0, 0 },
};

// card-disable flow (power down) - reverse the above into deep sleep. (satoru)
static const PwrStep kPwrOff[] = {
    { PWR_WRITE, REG_CR,          0xFF, 0x00 },                 // mac func off (satoru)
    { PWR_WRITE, REG_SYS_FUNC_EN, FEN_CPUEN, 0x00 },           // stop the 8051 (satoru)
    { PWR_WRITE, REG_APS_FSMCO+1, 0x02, 0x02 },                 // request off-mac (satoru)
    { PWR_WRITE, REG_RSV_CTRL,    0xFF, 0x0E },                 // re-lock power regs (satoru)
    { PWR_END,   0, 0, 0 },
};

// run a power-sequence table. returns false if a POLL step times out. (satoru)
static bool run_pwr_seq(const PwrStep* seq) {
    for (int i = 0; seq[i].cmd != PWR_END; i++) {
        const PwrStep& s = seq[i];
        switch (s.cmd) {
        case PWR_WRITE: {
            uint8_t cur = r8(s.off);
            cur = (uint8_t)((cur & ~s.mask) | (s.val & s.mask));
            w8(s.off, cur);
            break;
        }
        case PWR_POLL:
            if (!poll8(s.off, s.mask, s.val, 50)) {
                logx("[rtw] pwr-seq poll timeout at off=", s.off);
                return false;
            }
            break;
        case PWR_DELAY_US:
            // the pit clock is ms-resolution; every microsecond delay in these
            //  tables is sub-millisecond, so round up to a single 1ms wait. (satoru)
            Timer::WaitMs(1);
            break;
        case PWR_DELAY_MS:
            Timer::WaitMs(s.val);
            break;
        default: break;
        }
    }
    return true;
}

// ── llt (link-list table) init for the packet fifo ─────────────────── (satoru)
//
//  ref: rtw88 mac.c rtw_mac_init -> llt_init / rtlwifi _LLTWrite. the tx fifo is
//  organised as a linked list of pages; every page's "next" pointer must be
//  programmed via the REG_LLT_INIT port before dma can use the fifo. the access
//  port packs {op, address, data} into one dword; op=write-then-poll-done. the
//  page boundary `txpktbuf_bndy` separates tx pages from rx pages. (satoru)
#define LLT_OP_WRITE   (0x1u << 30)
#define LLT_OP_NO_ACT  (0x0u << 30)
#define LLT_POLLING    (0x3u << 30)   // busy flag in the top 2 bits (satoru)

static bool llt_write(uint32_t addr, uint32_t data) {
    uint32_t v = LLT_OP_WRITE | ((addr & 0xFF) << 8) | (data & 0xFF);
    w32(REG_LLT_INIT, v);
    // poll until op-code returns to no-action (write committed). (satoru)
    uint32_t start = Timer::GetTicks();
    while ((r32(REG_LLT_INIT) & (0x3u << 30)) != LLT_OP_NO_ACT) {
        if (Timer::GetTicks() - start > 20) return false;
    }
    return true;
}

static bool llt_init(uint8_t txpktbuf_bndy) {
    // total pages depends on the fifo size; 255 is the common max page index for
    //  these parts. link 0..bndy-1 as the tx free list, bndy..last as rx. (satoru)
    const uint8_t last_page = 0xFF;
    for (uint32_t i = 0; i < txpktbuf_bndy - 1u; i++)
        if (!llt_write(i, i + 1)) return false;
    if (!llt_write(txpktbuf_bndy - 1u, 0xFF)) return false;       // tx list terminates (satoru)
    for (uint32_t i = txpktbuf_bndy; i < last_page; i++)
        if (!llt_write(i, i + 1)) return false;
    if (!llt_write(last_page, txpktbuf_bndy)) return false;       // rx ring wraps (satoru)
    return true;
}

// ── dma ring allocation (page-aligned, identity-mapped = coherent) ──── (satoru)
//
//  ref: rtw88 pci.c rtw_pci_init_tx_ring / rtw_pci_init_rx_ring. each ring needs
//  a buffer-descriptor array + a block of per-slot payload buffers, both dma-
//  visible. PMM::AllocBytes hands out page-aligned, identity-mapped (virt==phys)
//  contiguous frames - exactly the coherent memory the dma engine needs (no
//  iommu on these boards; phys == the address we program). (satoru)
static bool alloc_ring(RtwRing* r) {
    r->bd = (RtwBufDesc*)PMM::AllocBytes(sizeof(RtwBufDesc) * RTW_RING_SZ);
    r->buf = (uint8_t*)PMM::AllocBytes((size_t)RTW_BUF_BYTES * RTW_RING_SZ);
    if (!r->bd || !r->buf) return false;
    r->bd_phys  = (uint64_t)(uintptr_t)r->bd;
    r->buf_phys = (uint64_t)(uintptr_t)r->buf;
    memset(r->bd, 0, sizeof(RtwBufDesc) * RTW_RING_SZ);
    memset(r->buf, 0, (size_t)RTW_BUF_BYTES * RTW_RING_SZ);
    r->host_idx = 0;
    r->hw_idx   = 0;
    // pre-point every buffer-descriptor at its payload slot. (satoru)
    for (uint32_t i = 0; i < RTW_RING_SZ; i++) {
        uint64_t pa = r->buf_phys + (uint64_t)i * RTW_BUF_BYTES;
        r->bd[i].dma_low  = (uint32_t)(pa & 0xFFFFFFFFu);
        r->bd[i].dma_high = (uint32_t)(pa >> 32);
        r->bd[i].buf_size = RTW_BUF_BYTES;
        r->bd[i].info     = 0;
    }
    return true;
}

// program the ring base-address regs + reset the host/dma indices. (satoru)
//  ref: rtw88 pci.c rtw_pci_reset_trx_ring - we wire the mgmt tx queue and the
//  rx queue (the two the supplicant uses). bcn/vo/vi/be/bk are left at zero base;
//  the maintainer maps the rest when a full data path lands. (satoru)
static void program_rings() {
    // rx ring base (64-bit) + capacity (low 12 bits of the idx reg = size). (satoru)
    w32(REG_RX_DESA,     (uint32_t)(g_state.rx.bd_phys & 0xFFFFFFFFu));
    w32(REG_RX_DESA + 4, (uint32_t)(g_state.rx.bd_phys >> 32));
    w16(REG_RXQ_RXBD_IDX, RTW_RING_SZ);          // tell hw the ring length (satoru)

    // mgmt tx ring base (64-bit). (satoru)
    w32(REG_MGQ_DESA,     (uint32_t)(g_state.tx.bd_phys & 0xFFFFFFFFu));
    w32(REG_MGQ_DESA + 4, (uint32_t)(g_state.tx.bd_phys >> 32));
    w16(REG_MGQ_TXBD_IDX, 0);                     // host index starts at 0 (satoru)

    // hand the rx ring fully to the device: host index = last slot so the dma
    //  engine owns every descriptor. (satoru)
    w16(REG_RXQ_RXBD_IDX, RTW_RING_SZ - 1);
}

// ── firmware download ──────────────────────────────────────────────── (satoru)
//
//  ref: linux rtw88 fw.c (rtw_download_firmware / download_firmware_to_mem) and
//  rtlwifi's _rtl_fw_download. the flow:
//    1. enable the fw-download path: REG_MCUFW_CTRL |= FWDL_EN | RAM_DL_SEL.
//    2. for each 4kb page of the blob: set the page index, dma/copy the page
//       into the chip's firmware start address (REG_FW_START_ADDRESS window),
//       poll the per-page "download ready" bit.
//    3. clear FWDL_EN, release the 8051 reset (REG_SYS_FUNC_EN |= CPUEN), then
//       poll REG_MCUFW_CTRL for the WINTINI_RDY "firmware init done" flag.
//  the realtek blob has a 32-byte header (signature/version/size) that linux
//  strips before download; we skip it the same way. (satoru)
#define REG_FW_START_ADDRESS  0x1000   // the 8051 firmware ram window in mmio (satoru)
#define RTW_FW_HDR_SIZE       32       // realtek fw file header bytes (satoru)
#define RTW_FW_PAGE_SIZE      4096     // download page granularity (satoru)
#define RTW_FW_MAX            (256*1024) // generous cap; real blobs ~80-130kb (satoru)

// the candidate firmware filenames per chip, tried in order under WIFI_RTW_FW_DIR.
//  rtw88 uses rtwXXXX_fw.bin; the older parts (rtlwifi) use rtlXXXXefw.bin. (satoru)
static const char* fw_names(RtwChip chip, int idx) {
    switch (chip) {
    case RTW_CHIP_8821C:
        return idx==0 ? "rtw8821c_fw.bin" : idx==1 ? "rtl8821cefw.bin"
             : idx==2 ? "rtl8821cefw_29.bin" : nullptr;
    case RTW_CHIP_8822C:
        return idx==0 ? "rtw8822c_fw.bin" : idx==1 ? "rtl8822cefw.bin" : nullptr;
    case RTW_CHIP_8723B:
        return idx==0 ? "rtl8723befw.bin" : idx==1 ? "rtl8723befw_36.bin" : nullptr;
    case RTW_CHIP_8812A:
        return idx==0 ? "rtl8812aefw.bin" : idx==1 ? "rtl8812aefw_wowlan.bin" : nullptr;
    default:
        return nullptr;
    }
}

// load the blob bytes into the chip. `blob`/`len` already point past nothing - 
//  we strip the realtek header here. returns true once the on-chip cpu reports
//  init-complete. (satoru)
static bool fw_download(const uint8_t* blob, int len) {
    if (len <= RTW_FW_HDR_SIZE) { log("[rtw] fw too small\r\n"); return false; }
    const uint8_t* body = blob + RTW_FW_HDR_SIZE;
    int body_len = len - RTW_FW_HDR_SIZE;

    // 1. enable the download path + select ram. (satoru)
    uint32_t ctrl = r32(REG_MCUFW_CTRL);
    ctrl |= MCUFW_FWDL_EN | MCUFW_RAM_DL_SEL;
    w32(REG_MCUFW_CTRL, ctrl);
    // a tiny settle, then confirm the enable stuck. (satoru)
    Timer::WaitMs(1);
    if (!(r32(REG_MCUFW_CTRL) & MCUFW_FWDL_EN)) {
        log("[rtw] fwdl enable did not latch\r\n");
        return false;
    }

    // 2. page-by-page copy into the firmware ram window. realtek selects the
    //    target 4kb page via the low bits of REG_MCUFW_CTRL (the "page select"
    //    field), then writes the page bytes into REG_FW_START_ADDRESS. (satoru)
    int pages = (body_len + RTW_FW_PAGE_SIZE - 1) / RTW_FW_PAGE_SIZE;
    for (int p = 0; p < pages; p++) {
        // select page `p`: rmw the page-index byte at REG_MCUFW_CTRL+2 (BIT_ROM_PGE
        //  / page-select per rtw88 fw.c). UNSURE: the exact field shifts per chip.
        uint8_t pgsel = r8(REG_MCUFW_CTRL + 2);
        pgsel = (uint8_t)((pgsel & 0xF8) | (p & 0x07));
        w8(REG_MCUFW_CTRL + 2, pgsel);

        int off = p * RTW_FW_PAGE_SIZE;
        int chunk = body_len - off;
        if (chunk > RTW_FW_PAGE_SIZE) chunk = RTW_FW_PAGE_SIZE;
        // copy the page as dwords into the mmio fw window (the chip latches it
        //  into 8051 code ram). pad the final partial page with the tail bytes
        //  only - do not over-read the blob. (satoru)
        int i = 0;
        for (; i + 4 <= chunk; i += 4) {
            uint32_t v = (uint32_t)body[off+i] | ((uint32_t)body[off+i+1] << 8) |
                         ((uint32_t)body[off+i+2] << 16) | ((uint32_t)body[off+i+3] << 24);
            w32(REG_FW_START_ADDRESS + i, v);
        }
        // trailing 1-3 bytes (last page): assemble a zero-padded dword. (satoru)
        if (i < chunk) {
            uint32_t v = 0;
            for (int b = 0; i + b < chunk; b++) v |= (uint32_t)body[off+i+b] << (8*b);
            w32(REG_FW_START_ADDRESS + i, v);
        }
    }

    // 3. clear the download enable, release the 8051, wait for init-done. (satoru)
    ctrl = r32(REG_MCUFW_CTRL);
    ctrl &= ~MCUFW_FWDL_EN;
    w32(REG_MCUFW_CTRL, ctrl);

    // toggle the cpu reset: clear then set FEN_CPUEN to restart the 8051 on the
    //  freshly-downloaded image. (satoru)
    uint16_t fen = r16(REG_SYS_FUNC_EN);
    w16(REG_SYS_FUNC_EN, (uint16_t)(fen & ~FEN_CPUEN));
    Timer::WaitMs(1);
    w16(REG_SYS_FUNC_EN, (uint16_t)(fen | FEN_CPUEN));

    // poll the firmware-init-ready flag (WINTINI_RDY). on real silicon the 8051
    //  sets this within a few ms of a good image. (satoru)
    if (!poll32(REG_MCUFW_CTRL, MCUFW_WINTINI_RDY, MCUFW_WINTINI_RDY, 200)) {
        log("[rtw] firmware init-ready not signalled (no real hw / bad blob)\r\n");
        return false;
    }
    log("[rtw] firmware init complete\r\n");
    return true;
}

// read the firmware file from the kurono fs and hand it to fw_download. tries the
//  per-chip candidate names. returns false (cleanly) if none are present - this
//  is the expected path in-tree, since the blob is not shipped. (satoru)
static bool load_firmware_from_fs() {
    // a per-page scratch read buffer would be cleaner, but KVFS::ReadFile wants a
    //  single destination; allocate a dma-free heap-ish buffer from pmm (it is
    //  large, and we free it after download). (satoru)
    uint8_t* buf = (uint8_t*)PMM::AllocBytes(RTW_FW_MAX);
    if (!buf) { log("[rtw] no memory for fw buffer\r\n"); return false; }

    bool ok = false;
    for (int i = 0; ; i++) {
        const char* name = fw_names(g_state.chip, i);
        if (!name) break;
        char path[160];
        int n = 0;
        const char* dir = WIFI_RTW_FW_DIR;
        for (const char* c = dir; *c && n < 150; c++) path[n++] = *c;
        for (const char* c = name; *c && n < 159; c++) path[n++] = *c;
        path[n] = 0;
        if (!KVFS::Exists(path)) continue;
        int got = KVFS::ReadFile(path, buf, RTW_FW_MAX);
        if (got <= 0) { log("[rtw] fw read failed: "); log(path); log("\r\n"); continue; }
        log("[rtw] firmware found: "); log(path); log("\r\n");
        ok = fw_download(buf, got);
        break;
    }
    if (!ok) log("[rtw] no usable firmware under " WIFI_RTW_FW_DIR "\r\n");
    PMM::FreeBytes(buf, RTW_FW_MAX);
    return ok;
}

// ── mac init: bring the dma + protocol engines up after power-on ───── (satoru)
//  ref: rtw88 mac.c rtw_mac_init (the post-power-on band fifo/queue setup). this
//  is the order: trxdma enable, fifo page boundary + rqpn, llt, then flip the CR
//  function-enable bits so the protocol/schedule/tx/rx engines run. (satoru)
static bool mac_init() {
    // enable the dma engine + map queues to the priority pages. (satoru)
    w32(REG_TRXDMA_CTRL, 0x0000F771);     // UNSURE: per-chip queue->page map (satoru)

    // page-boundary between tx and rx fifo pages. 0xB0 is a common value on
    //  these parts (tx gets pages 0..0xAF, rx the rest). (satoru)
    const uint8_t txpktbuf_bndy = 0xB0;
    w8(REG_TRXFF_BNDY + 1, txpktbuf_bndy);

    // reserved-queue page numbers: how many fifo pages each tx priority owns.
    //  the high byte enables "load rqpn" (BIT_LD_RQPN). UNSURE bytes per chip.
    w32(REG_RQPN, 0x80E60808);
    w8 (REG_RQPN_NPQ, 0x00);
    w16(REG_RQPN_NPQ + 1, 0x0010);

    // build the link-list table for the fifo pages. (satoru)
    if (!llt_init(txpktbuf_bndy)) { log("[rtw] llt init failed\r\n"); return false; }

    // flip on the mac function-enable bits: protocol engine, scheduler, tx/rx
    //  mac, and the dma front-ends. (satoru)
    uint16_t cr = (uint16_t)(CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN | CR_TXDMA_EN |
                             CR_RXDMA_EN | CR_PROTOCOL_EN | CR_SCHEDULE_EN |
                             CR_MACTXEN | CR_MACRXEN);
    w16(REG_CR, cr);
    return true;
}

// ── chip detect from the pci device id ─────────────────────────────── (satoru)
static RtwChip detect_chip(uint16_t device) {
    switch (device) {
    case 0x8821: case 0xc821: case 0xc822: return RTW_CHIP_8821C;
    case 0xb822:                            return RTW_CHIP_8822C;
    case 0x8723: case 0xb723:               return RTW_CHIP_8723B;
    case 0x8812:                            return RTW_CHIP_8812A;
    default:                                return RTW_CHIP_8821C; // sane default (satoru)
    }
}
static const char* chip_name(RtwChip c) {
    switch (c) {
    case RTW_CHIP_8821C: return "rtl8821ce";
    case RTW_CHIP_8822C: return "rtl8822ce";
    case RTW_CHIP_8723B: return "rtl8723be";
    case RTW_CHIP_8812A: return "rtl8812ae";
    default:             return "rtw";
    }
}

// ── read the station mac from efuse/otp ────────────────────────────── (satoru)
//  ref: rtw88 efuse.c. the permanent mac lives in the efuse; rtw88 reads it via
//  the REG_EFUSE_CTRL access port (set address, trigger, poll-valid, read byte).
//  the per-chip efuse address of the mac differs (8821c: 0x11a, 8822c: 0x12a,
//  ...). we read 6 bytes and, if they look valid (not all 0x00/0xff), publish
//  them; otherwise the 802.11 stack keeps its locally-administered fallback.
//  (satoru)
static uint8_t efuse_read_byte(uint16_t addr) {
    // REG_EFUSE_CTRL: [31]=valid/trigger, [23:16]=data, [17:8]=addr (per rtw88).
    uint32_t cmd = ((uint32_t)(addr & 0x3FF) << 8);
    w32(REG_EFUSE_CTRL, cmd);                 // addr, valid=0 -> request read (satoru)
    if (!poll32(REG_EFUSE_CTRL, (1u<<31), (1u<<31), 10)) return 0xFF;  // wait valid (satoru)
    return (uint8_t)((r32(REG_EFUSE_CTRL) >> 16) & 0xFF);
}
static bool read_efuse_mac(uint8_t out[6]) {
    uint16_t base;
    switch (g_state.chip) {
    case RTW_CHIP_8821C: base = 0x11A; break;
    case RTW_CHIP_8822C: base = 0x12A; break;   // UNSURE per chip (satoru)
    case RTW_CHIP_8723B: base = 0x11A; break;
    case RTW_CHIP_8812A: base = 0x11A; break;
    default:             base = 0x11A; break;
    }
    uint8_t all_ff = 0xFF, all_00 = 0x00;
    for (int i = 0; i < 6; i++) {
        out[i] = efuse_read_byte((uint16_t)(base + i));
        all_ff &= out[i];
        all_00 |= out[i];
    }
    // valid iff not all-ff and not all-zero. (satoru)
    return !(all_ff == 0xFF || all_00 == 0x00);
}

// ─────────────────────────────────────────────────────────────────────
//  WifiRadioOps implementation
// ─────────────────────────────────────────────────────────────────────

// start(): the full bring-up. power-on seq -> mac init -> dma rings -> firmware.
//  fails cleanly (returns false, leaves the radio quiesced) if the mmio window is
//  dead or the firmware is absent - no faking association without hardware. (satoru)
static bool rtw_start(void* ctx) {
    RtwState* s = (RtwState*)ctx;
    if (s->started) return true;

    if (!s->mmio_ok) {
        log("[rtw] start: mmio window dead - cannot bring up\r\n");
        return false;
    }
    log("[rtw] bring-up: "); log(chip_name(s->chip)); log("\r\n");

    // 1. power-on sequence (the realtek pwr_seq). (satoru)
    if (!run_pwr_seq(kPwrOn)) {
        log("[rtw] power-on sequence failed\r\n");
        return false;
    }

    // 2. allocate the dma rings before mac init wires their base regs. (satoru)
    if (!s->rings_ok) {
        if (!alloc_ring(&s->tx) || !alloc_ring(&s->rx)) {
            log("[rtw] dma ring alloc failed\r\n");
            return false;
        }
        s->rings_ok = true;
    }

    // 3. mac init (trxdma, fifo pages, llt, CR function-enable). (satoru)
    if (!mac_init()) return false;

    // 4. program the ring base-address regs + indices. (satoru)
    program_rings();

    // 5. firmware download - required for these parts. clean fail if absent. (satoru)
    if (!s->fw_loaded) {
        s->fw_loaded = load_firmware_from_fs();
        if (!s->fw_loaded) {
            log("[rtw] start aborted: firmware not loaded\r\n");
            // leave the mac powered but un-started; stop() can power it down. (satoru)
            return false;
        }
    }

    // 6. baseline receive config: accept my-mac + broadcast + (bssid-checked)
    //    beacons, and append the phy-status so get_signal has rssi. (satoru)
    uint32_t rcr = RCR_APM | RCR_AB | RCR_AM | RCR_CBSSID_BCN | RCR_APP_PHYST_RXFF;
    w32(REG_RCR, rcr);
    // open the rx filter maps so mgmt (beacon/probe-resp/auth/assoc) + data reach
    //  us; the 802.11 stack does the fine filtering. (satoru)
    w16(REG_RXFLTMAP0, 0xFFFF);   // all mgmt subtypes (satoru)
    w16(REG_RXFLTMAP1, 0x0000);   // no bare ctrl frames (satoru)
    w16(REG_RXFLTMAP2, 0xFFFF);   // all data subtypes (satoru)

    // adopt the efuse mac if the chip has one (informational; the stack already
    //  derived a station mac at RegisterRadio time). (satoru)
    uint8_t mac[6];
    if (read_efuse_mac(mac)) {
        for (int i = 0; i < 6; i++) w8(REG_MACID + i, mac[i]);
        log("[rtw] efuse mac programmed\r\n");
    }

    // unmask the rx + tx-ok interrupts (we still poll, but enabling them lets a
    //  future irq path drain via DeliverRx). (satoru)
    w32(REG_HIMR0, 0xFFFFFFFFu);

    s->started = true;
    s->last_rssi = -90;
    log("[rtw] radio started\r\n");
    return true;
}

static void rtw_stop(void* ctx) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok) { s->started = false; return; }
    // mask interrupts, idle the mac, then run the power-down sequence. (satoru)
    w32(REG_HIMR0, 0x00000000u);
    w16(REG_CR, 0x0000);
    run_pwr_seq(kPwrOff);
    s->started = false;
    log("[rtw] radio stopped\r\n");
}

// set_channel(): tune the rf synth. (satoru)
//  ref: rtw88 phy.c rtw_phy_set_channel / rtlwifi PHY_SwChnl. on rtw88 the channel
//  is set by an h2c command to the firmware; on older parts it is a direct rf-reg
//  write (rRfChannel = 0x18). we do both: program the bb 2.4/5ghz marker, the
//  rf channel reg, and (if fw is up) also fire the h2c set-channel. blocks a
//  short settle so the caller's scan dwell does not race the synth. (satoru)
static bool h2c_cmd(uint8_t id, const uint8_t* p, int n); // fwd (satoru)

static bool rtw_set_channel(void* ctx, int ch) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok) return false;
    if (ch < 1 || ch > 196) return false;

    // 2.4ghz vs 5ghz bb path marker. (satoru)
    bool is_2g = (ch <= 14);
    w8(REG_CCK_CHECK, is_2g ? 0x00 : 0x80);   // bit7 set => 5ghz-only (satoru)

    // direct rf channel write (older parts / always-safe). the rf reg-write port
    //  on these chips is the LSSI write at REG 0x18 of path-A; we model it as a
    //  word write of the channel into the rf-channel reg shadow. UNSURE: the full
    //  rf serial-bus write needs the per-chip phy tables on real hw. (satoru)
    w32(REG_FPGA0_RFMOD, (r32(REG_FPGA0_RFMOD) & ~0x300) | (is_2g ? 0x000 : 0x100));

    // firmware h2c set-channel (rtw88 path). best-effort; ignored if fw absent.
    if (s->fw_loaded) {
        uint8_t pl[3] = { (uint8_t)ch, (uint8_t)(is_2g ? 0 : 1), 0 };
        h2c_cmd(H2C_SET_CHANNEL, pl, 3);
    }

    s->channel = ch;
    Timer::WaitMs(10);   // synth settle (satoru)
    return true;
}

// config_bss(): program the bssid regs + enable bssid-checked rx so only the
//  joined bss reaches us. (satoru)
//  ref: rtw88 mac.c / rtlwifi set_hw_reg(HW_VAR_BSSID). (satoru)
static bool rtw_config_bss(void* ctx, const uint8_t bssid[6], const char* ssid) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok) return false;
    for (int i = 0; i < 6; i++) { s->bssid[i] = bssid[i]; w8(REG_BSSID + i, bssid[i]); }
    int i = 0; if (ssid) { for (; ssid[i] && i < 32; i++) s->ssid[i] = ssid[i]; } s->ssid[i] = 0;

    // turn on bssid checking for data + beacon now that the bssid is set. (satoru)
    uint32_t rcr = r32(REG_RCR) | RCR_CBSSID_DATA | RCR_CBSSID_BCN;
    w32(REG_RCR, rcr);

    // notify firmware we are joining (media-status connect). best-effort. (satoru)
    if (s->fw_loaded) {
        uint8_t pl[3] = { 1 /*connect*/, 0, 0 };
        h2c_cmd(H2C_MEDIA_STATUS, pl, 3);
    }
    return true;
}

// set_key(): we do not offload crypto - return false so the 802.11 stack does
//  ccmp in software (it is designed to fall back). a maintainer can later wire
//  the cam (content-addressable memory) key table here. (satoru)
//  ref: rtw88 sec.c rtw_sec_write_cam - intentionally not implemented. (satoru)
static bool rtw_set_key(void* ctx, int idx, const uint8_t* key, int key_len, int type) {
    (void)ctx; (void)idx; (void)key; (void)key_len; (void)type;
    return false;   // software ccmp (satoru)
}

// tx_frame(): enqueue one fully-formed 802.11 frame onto the mgmt tx ring. (satoru)
//  ref: rtw88 tx.c rtw_pci_tx - we build the 48-byte realtek tx descriptor in
//  front of the frame, point the buffer-descriptor at it, then bump the host
//  index so the dma engine fetches it. (satoru)
//
//  the tx descriptor header (dw0..dw11) carries: dw0 = pkt length (bits 0:15) +
//  offset to the 802.11 header (the desc size, bits 16:23) + own-by-hw flag;
//  dw1 = queue-select (QSEL) + mac-id; dw3 = sequence; dw4/dw5 = rate/agg (left
//  at hardware-rate-control defaults = 0). exact bit positions per rtw88 txdesc.
//  (satoru)
static bool rtw_tx_frame(void* ctx, const uint8_t* frame, int len) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok || !s->rings_ok) return false;
    if (len <= 0 || len > (RTW_BUF_BYTES - RTW_TXDESC_BYTES)) return false;

    uint32_t slot = s->tx.host_idx % RTW_RING_SZ;
    uint8_t* buf = s->tx.buf + (uint64_t)slot * RTW_BUF_BYTES;

    // build the tx descriptor header in the slot, then the frame after it. (satoru)
    RtwTxDesc* d = (RtwTxDesc*)buf;
    memset(d, 0, sizeof(*d));
    // dw0: [15:0] pkt size = frame len; [23:16] desc size (offset to mac hdr);
    //      [31] OWN = hand to hardware. (ref: rtw88 txdesc GET/SET_TX_DESC_*) (satoru)
    d->dw[0] = ((uint32_t)len & 0xFFFF) |
               (((uint32_t)RTW_TXDESC_BYTES & 0xFF) << 16) |
               (1u << 31);
    // dw1: QSEL - mgmt queue select (0x12 = mgmt high). UNSURE: qsel codes per
    //      chip; 0x12 is the common mgmt value in rtlwifi. mac-id 0. (satoru)
    d->dw[1] = (0x12u << 8);
    // dw3: sequence - let hardware assign (HWSEQ_EN) by leaving the seq field 0
    //      and setting the hw-seq enable hint. (satoru)
    d->dw[3] = 0;
    // dw4: use a low basic rate for mgmt (rate index 0 = 1mbps) with rate-control
    //      disabled so probe/auth go out at a robust rate. (satoru)
    d->dw[4] = 0;

    // copy the 802.11 frame right after the descriptor. (satoru)
    memcpy(buf + RTW_TXDESC_BYTES, frame, len);

    // point this slot's buffer-descriptor at {desc+frame} and mark first+last
    //  segment + own. (satoru)
    uint64_t pa = s->tx.buf_phys + (uint64_t)slot * RTW_BUF_BYTES;
    s->tx.bd[slot].dma_low  = (uint32_t)(pa & 0xFFFFFFFFu);
    s->tx.bd[slot].dma_high = (uint32_t)(pa >> 32);
    s->tx.bd[slot].buf_size = (uint16_t)(RTW_TXDESC_BYTES + len);
    s->tx.bd[slot].info     = 0xC000;   // FS|LS (first+last segment) (satoru)

    // advance the host index and tell the hardware. (satoru)
    s->tx.host_idx = (s->tx.host_idx + 1) % RTW_RING_SZ;
    w16(REG_MGQ_TXBD_IDX, (uint16_t)s->tx.host_idx);
    return true;
}

// rx_poll(): drain one frame off the rx ring if the hardware has filled a
//  descriptor. (satoru)
//  ref: rtw88 rx.c rtw_pci_rx_isr / rtw_rx_fill_rx_status - the chip writes a
//  24-byte rx descriptor in front of the mpdu; we parse the length + the phy-
//  status (for rssi) and copy out the 802.11 frame (fcs stripped). returns the
//  frame length, 0 if nothing pending. (satoru)
static bool rxdesc_owned_by_host(const uint8_t* desc) {
    // rx desc dw0 bit31 OWN: 0 => hardware has written it (host may read). some
    //  parts invert this; we treat OWN==0 as "ready for host". UNSURE per chip.
    uint32_t dw0 = (uint32_t)desc[0] | ((uint32_t)desc[1]<<8) |
                   ((uint32_t)desc[2]<<16) | ((uint32_t)desc[3]<<24);
    return (dw0 & (1u<<31)) == 0;
}

static int rtw_rx_poll(void* ctx, uint8_t* out, int out_max) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok || !s->rings_ok) return 0;

    uint32_t slot = s->rx.host_idx % RTW_RING_SZ;
    uint8_t* buf = s->rx.buf + (uint64_t)slot * RTW_BUF_BYTES;

    if (!rxdesc_owned_by_host(buf)) return 0;   // hardware hasn't filled it (satoru)

    // rx desc dw0: [13:0] pkt length; [30:24] drvinfo size (in 8-byte units);
    //  the mpdu starts after the 24-byte desc + drvinfo. (ref: rtw88 rx.c) (satoru)
    uint32_t dw0 = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8) |
                   ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
    int pkt_len  = (int)(dw0 & 0x3FFF);
    int drvinfo  = (int)((dw0 >> 24) & 0x7F) * 8;
    int hdr_off  = RTW_RXDESC_BYTES + drvinfo;

    // recover rssi from the phy-status (drvinfo). the layout is per-chip; the
    //  first phy-status byte's pwdb (0..100) maps to dBm ~ pwdb-100. UNSURE: real
    //  parts need the per-chip pwdb->dBm curve. (satoru)
    if (drvinfo >= 4 && hdr_off <= RTW_BUF_BYTES) {
        uint8_t pwdb = buf[RTW_RXDESC_BYTES + 0];
        int dbm = (int)pwdb - 100;
        if (dbm < -100) dbm = -100;
        if (dbm > 0)    dbm = 0;
        s->last_rssi = dbm;
    }

    int copy = pkt_len;
    if (hdr_off + copy > RTW_BUF_BYTES) copy = RTW_BUF_BYTES - hdr_off;
    if (copy < 0) copy = 0;
    if (copy > out_max) copy = out_max;
    // strip the trailing 4-byte fcs if the hardware kept it (we did not set
    //  RCR_APP_FCS, so normally it is already stripped; guard anyway). (satoru)
    if (copy >= 4 && (r32(REG_RCR) & RCR_APP_FCS)) copy -= 4;
    if (copy > 0) memcpy(out, buf + hdr_off, copy);

    // hand the slot back to the hardware: set OWN, advance the index. (satoru)
    buf[3] |= 0x80;   // re-arm OWN (satoru)
    s->rx.host_idx = (s->rx.host_idx + 1) % RTW_RING_SZ;
    w16(REG_RXQ_RXBD_IDX, (uint16_t)((s->rx.host_idx + RTW_RING_SZ - 1) % RTW_RING_SZ));
    return copy;
}

static int rtw_get_signal(void* ctx) {
    RtwState* s = (RtwState*)ctx;
    return s->last_rssi;
}

// load_firmware(): the stack-driven entry point. our firmware comes from the fs
//  (the stack does not ship it), so a caller-provided blob is taken verbatim if
//  non-null, else we read from the fs. (satoru)
static bool rtw_load_firmware(void* ctx, const uint8_t* blob, int len) {
    RtwState* s = (RtwState*)ctx;
    if (!s->mmio_ok) return false;
    if (blob && len > 0) { s->fw_loaded = fw_download(blob, len); return s->fw_loaded; }
    s->fw_loaded = load_firmware_from_fs();
    return s->fw_loaded;
}

// ── h2c (host-to-cpu) command mailbox ──────────────────────────────── (satoru)
//  ref: rtw88 fw.c rtw_fw_send_h2c_command. up to 4 payload bytes go in the
//  HMEBOX registers; the high bytes + "fire" trigger go in the metadata reg.
//  used by set_channel / config_bss above. best-effort: if the box is busy we
//  drop the command (a scan/connect retry re-issues it). (satoru)
static bool h2c_cmd(uint8_t id, const uint8_t* p, int n) {
    if (!g_state.fw_loaded || n > 7) return false;
    // pack up to 4 bytes into HMEBOX_0 (low box) and the rest into the ext box;
    //  here we keep it to the low box (<=3 payload bytes covers our commands).
    uint32_t box = id;
    for (int i = 0; i < n && i < 3; i++) box |= (uint32_t)p[i] << (8 * (i + 1));
    w32(REG_HMEBOX_0, box);
    // fire: set the trigger bit for box 0 in the metadata reg. UNSURE: the exact
    //  trigger encoding differs per chip fw. (satoru)
    w8(REG_HMETFR, (uint8_t)(r8(REG_HMETFR) | 0x01));
    return true;
}

// ─────────────────────────────────────────────────────────────────────
//  registration
// ─────────────────────────────────────────────────────────────────────
bool Registered() { return g_registered; }

bool TryRegister() {
    if (g_registered) return true;

    // probe the pci bus (idempotent) and bail unless it is a realtek rtw part.
    if (!WifiDev::Present()) return false;
    const WifiDevice* d = WifiDev::Info();
    if (!d || d->family != WIFI_FAM_REALTEK_RTW) return false;

    // populate our private state. (satoru)
    g_state = RtwState{};
    g_state.dev     = d;
    g_state.chip    = detect_chip(d->device);
    g_state.mmio_ok = d->mmio_mapped;
    g_state.channel = 1;
    g_state.last_rssi = -100;

    log("[rtw] detected "); log(chip_name(g_state.chip));
    log(g_state.mmio_ok ? " (mmio live)\r\n" : " (mmio DEAD - bring-up will fail)\r\n");

    // fill the ops vtable. (satoru)
    g_ops.start         = rtw_start;
    g_ops.stop          = rtw_stop;
    g_ops.set_channel   = rtw_set_channel;
    g_ops.config_bss    = rtw_config_bss;
    g_ops.set_key       = rtw_set_key;
    g_ops.tx_frame      = rtw_tx_frame;
    g_ops.rx_poll       = rtw_rx_poll;
    g_ops.get_signal    = rtw_get_signal;
    g_ops.load_firmware = rtw_load_firmware;

    // register with the 802.11 stack. it will call start() at the first scan/
    //  connect. (satoru)
    Ieee80211::RegisterRadio(&g_ops, &g_state, (WifiDevice*)d);
    g_registered = true;
    log("[rtw] registered realtek radio with the 802.11 stack\r\n");
    return true;
}

} // namespace WifiRtw
// end (satoru)
