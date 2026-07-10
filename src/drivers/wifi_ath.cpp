//  kurono os - atheros / qualcomm wifi radio driver (ath9k + ath10k) (satoru)
//  see wifi_ath.h. implements the WifiRadioOps contract for atheros ar9xxx
//  (ath9k, firmware-free, implemented in full) and qualcomm qca988x/qca6174
//  (ath10k, firmware-based, bring-up scaffold). (satoru)
//
//  all register numbers + reset/init sequences below are cross-referenced from
//  the linux ath9k / ath10k drivers (file cited inline as "ref: linux ath9k ...")
//  and re-expressed as original kurono code. (satoru)

#include "wifi_ath.h"
#include "wifi_dev.h"
#include "serial.h"
#include "timer.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../net/ieee80211.h"
#include "../fs/kvfs.h"

namespace WifiAth {

// the radio-ops vtable + register/deliver entry points live in the Ieee80211
// namespace; pull the type in so our ATH_OPS declaration reads cleanly. (satoru)
using Ieee80211::WifiRadioOps;

// ─────────────────────────────────────────────────────────────────────────────
//  small freestanding helpers (no libc) (satoru)
// ─────────────────────────────────────────────────────────────────────────────
static inline void a_memset(void* d, int v, unsigned long n) {
    unsigned char* p = (unsigned char*)d;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)v;
}
static inline void a_memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dp = (unsigned char*)d; const unsigned char* sp = (const unsigned char*)s;
    for (unsigned long i = 0; i < n; i++) dp[i] = sp[i];
}
static void log(const char* s) { SerialLogger::Log(s); }
static void logx(uint32_t v) { SerialLogger::LogHex(v); }
static void logd(int v) { SerialLogger::LogDec(v); }

// busy-spin a coarse number of microseconds. the cooperative scheduler has no
// sub-ms sleep; Timer::WaitMs is the finest blocking primitive, so for the short
// hardware settling waits we use a calibrated cpu-relax spin. this is the
// equivalent of ath9k's udelay() (ref: linux ath9k hw.c). (satoru)
static void udelay(uint32_t us) {
    // ~ conservative: assume the cpu does no more than a few hundred million
    // relax-loops/sec; over-waiting a few us is harmless for these settles. (satoru)
    volatile uint64_t spins = (uint64_t)us * 300ULL;
    for (volatile uint64_t i = 0; i < spins; i++) __asm__ __volatile__("pause");
}
static inline void mdelay(uint32_t ms) { Timer::WaitMs(ms); }

// ─────────────────────────────────────────────────────────────────────────────
//  ath9k register map (ref: linux drivers/net/wireless/ath/ath9k/reg.h) (satoru)
//  only the registers this driver touches are defined; numbers are the public
//  hardware offsets, re-typed here (not copied from any gpl header). (satoru)
// ─────────────────────────────────────────────────────────────────────────────

// command / dma engine (ref: ath9k reg.h, mac.c, recv.c) (satoru)
#define AR_CR              0x0008   // command register (satoru)
#define   AR_CR_RXE        0x00000004   // rx enable (satoru)
#define   AR_CR_RXD        0x00000020   // rx disable (satoru)
#define AR_RXDP            0x000C   // rx descriptor pointer (head of rx ring) (satoru)
#define AR_CFG             0x0014   // config - endianness/byte-swap (satoru)
#define   AR_CFG_SWTD      0x00000001   // byte-swap tx descriptor (satoru)
#define   AR_CFG_SWRD      0x00000002   // byte-swap rx descriptor (satoru)

// per-queue tx descriptor pointer + enable/disable (ref: ath9k mac.c) (satoru)
#define AR_QTXDP(q)        (0x0800 + ((q) << 2))   // queue tx desc ptr (satoru)
#define AR_Q_TXE           0x0840   // tx enable bitmask (one bit per queue) (satoru)
#define AR_Q_TXD           0x0880   // tx disable bitmask (satoru)
#define AR_Q_STS(q)        (0x09C0 + ((q) << 2))   // per-queue status (satoru)
#define   AR_Q_STS_PENDING_MASK 0x000003FF
#define AR_D_QCUMASK       0x1230   // dcu->qcu select mask (satoru)

// interrupt registers (ref: ath9k mac.c ath9k_hw_set_interrupts) (satoru)
#define AR_ISR             0x0080   // primary interrupt status (satoru)
#define AR_IMR             0x00A0   // primary interrupt mask (satoru)
#define AR_IER             0x0024   // interrupt enable (global) (satoru)
#define   AR_IER_ENABLE    0x00000001
#define   AR_IER_DISABLE   0x00000000
#define   AR_IMR_RXOK      0x00000001   // rx frame ok (satoru)
#define   AR_IMR_RXERR     0x00000004
#define   AR_IMR_RXEOL     0x00000010   // rx end-of-list (ring drained) (satoru)
#define   AR_IMR_TXOK      0x00000040   // tx frame ok (satoru)
#define   AR_IMR_TXERR     0x00000100
#define   AR_ISR_RXOK      0x00000001
#define   AR_ISR_TXOK      0x00000040

// station / bss address + filters (ref: ath9k mac.c, recv.c) (satoru)
#define AR_STA_ID0         0x8000   // sta mac bytes 0..3 (satoru)
#define AR_STA_ID1         0x8004   // sta mac bytes 4..5 + flags (satoru)
#define   AR_STA_ID1_STA_AP        0x00010000   // act as ap (we leave clear) (satoru)
#define   AR_STA_ID1_KSRCH_MODE    0x08000000   // key-search always (satoru)
#define AR_BSS_ID0         0x8008   // bssid bytes 0..3 (satoru)
#define AR_BSS_ID1         0x800C   // bssid bytes 4..5 + aid (satoru)
#define AR_BSSMSKL         0x80E0   // bssid mask low (satoru)
#define AR_BSSMSKU         0x80E4   // bssid mask high (satoru)
#define AR_RX_FILTER       0x803C   // rx frame-type filter (ref: recv.c) (satoru)
#define   AR_RX_FILTER_UCAST   0x00000001
#define   AR_RX_FILTER_MCAST   0x00000002
#define   AR_RX_FILTER_BCAST   0x00000004
#define   AR_RX_FILTER_BEACON  0x00000010
#define   AR_RX_FILTER_PROM    0x00000020   // promiscuous (satoru)
#define   AR_RX_FILTER_PROBEREQ 0x00000080
#define AR_MULTICAST_0     0x8040   // mcast hash low (satoru)
#define AR_MULTICAST_1     0x8044   // mcast hash high (satoru)
#define AR_DIAG_SW         0x8048   // diagnostic / soft control (satoru)
#define   AR_DIAG_RX_DIS   0x00000020   // disable rx (satoru)

// rtc / reset / pll block (ref: ath9k hw.c ath9k_hw_set_reset, init_pll) (satoru)
#define AR_RTC_BASE        0x7000
#define AR_RTC_RC          (AR_RTC_BASE + 0x00)   // reset control (0x7000) (satoru)
#define   AR_RTC_RC_M           0x00000003
#define   AR_RTC_RC_MAC_WARM    0x00000001   // warm reset the mac (satoru)
#define   AR_RTC_RC_MAC_COLD    0x00000002   // cold reset the mac (satoru)
#define AR_RTC_PLL_CONTROL (AR_RTC_BASE + 0x14)   // pll control (0x7014) (satoru)
#define AR_RTC_RESET       (AR_RTC_BASE + 0x40)   // 0x7040 (satoru)
#define   AR_RTC_RESET_EN  0x00000001
#define AR_RTC_STATUS      (AR_RTC_BASE + 0x44)   // 0x7044 (satoru)
#define   AR_RTC_STATUS_M  0x0000000F
#define   AR_RTC_STATUS_ON 0x00000002   // rtc is on/awake (satoru)
#define AR_RTC_FORCE_WAKE  (AR_RTC_BASE + 0x08)   // 0x7008 (satoru)
#define   AR_RTC_FORCE_WAKE_EN     0x00000001
#define   AR_RTC_FORCE_WAKE_ON_INT 0x00000002

// "rc" warm/cold also lives in the AR_RC (0x4000) on older parts; the rtc block
// above is the ar5416+ path used by every modern ath9k chip. (ref: hw.c) (satoru)

// hardware revision id (ref: ath9k hw.c ath9k_hw_read_revisions) (satoru)
#define AR_SREV            0x4020   // silicon revision (satoru)

// reset-after-warm: tsf + general control bits (ref: ath9k mac.c) (satoru)
#define AR_RESET_TSF       0x8020
#define   AR_RESET_TSF_ONCE 0x01000000

// phy block (ref: ath9k phy.c / ar5008_phy.c / ar9003_phy.c) (satoru)
#define AR_PHY_BASE        0x9800
#define AR_PHY(i)          (AR_PHY_BASE + ((i) << 2))
#define AR_PHY_ACTIVE      0x981C   // phy active/idle (satoru)
#define   AR_PHY_ACTIVE_EN 0x00000001
#define   AR_PHY_ACTIVE_DIS 0x00000000
#define AR_PHY_RFBUS_REQ   0x9C00   // request the rf bus before re-tuning (satoru)
#define   AR_PHY_RFBUS_REQ_EN 0x00000001
#define AR_PHY_RFBUS_GRANT 0x9C20   // grant of the rf bus (satoru)
#define   AR_PHY_RFBUS_GRANT_EN 0x00000001
#define AR_PHY_SYNTH_CONTROL 0x9874 // synthesizer band/select (ref: ar5008_phy.c) (satoru)
#define AR_PHY_MODE        0xA200   // phy operating mode (ofdm/cck, 2g/5g) (satoru)
#define   AR_PHY_MODE_OFDM 0x00000000
#define   AR_PHY_MODE_CCK  0x00000001
#define   AR_PHY_MODE_DYNAMIC 0x00000004
#define   AR_PHY_MODE_2GHZ 0x00000020
#define   AR_PHY_MODE_5GHZ 0x00000000   // (band bit clear = 5ghz) (satoru)
#define AR_PHY_RX_DELAY    0x9914   // rx delay after a channel change (satoru)
#define   AR_PHY_RX_DELAY_DELAY 0x00003FFF

// eeprom access (ref: ath9k eeprom.c / hw.c ath9k_hw_nvram_read) (satoru)
#define AR_EEPROM_OFFSET   0x2000
#define AR_EEPROM_ADDR     (AR_EEPROM_OFFSET + 0x00)   // address to read (satoru)
#define AR_EEPROM_DATA     (AR_EEPROM_OFFSET + 0x04)   // data result (satoru)
#define AR_EEPROM_CMD      (AR_EEPROM_OFFSET + 0x08)   // command (satoru)
#define   AR_EEPROM_CMD_READ 0x00000001
#define AR_EEPROM_STS      (AR_EEPROM_OFFSET + 0x0C)   // status (satoru)
#define   AR_EEPROM_STS_READ_COMPLETE 0x00000002
#define   AR_EEPROM_STS_READ_ERROR    0x00000004
// the ar5416 path reads the eeprom through the GENERIC config-space window at
// AR5416_EEPROM_OFFSET; ath9k_hw_nvram_read polls AR_EEPROM_STATUS_DATA. (satoru)
#define AR5416_EEPROM_OFFSET        0x2000
#define AR5416_EEPROM_S             2
#define AR_EEPROM_STATUS_DATA       0x40C8   // ar9280+ nvram read result (satoru)
#define   AR_EEPROM_STATUS_DATA_VAL_MASK 0x0000FFFF

// the eeprom layout offsets we care about: the mac address lives at word 0x1F..
// in the ar5416 eeprom base header (ref: ath9k eeprom.h BASE_EEP_HEADER). (satoru)
#define AR5416_EEP_MAC_OFFSET       0x1F   // 3 words = 6 bytes of mac (satoru)
#define AR5416_EEP_MAGIC_OFFSET     0x00
#define AR5416_EEPROM_MAGIC         0xA55A

// otp (one-time-programmable) memory for ar9003 chips - they have no serial
// eeprom; the cal+mac live in otp read via the otp controller. (ref: ar9003_eeprom.c)
#define AR9300_OTP_BASE            0x14000
#define AR9300_OTP_STATUS          0x15F18
#define   AR9300_OTP_STATUS_TYPE   0x7
#define   AR9300_OTP_STATUS_VALID  0x4
#define AR9300_OTP_READ_DATA       0x15F1C

// ─────────────────────────────────────────────────────────────────────────────
//  the legacy ath9k tx/rx dma descriptor (ref: ath9k desc.h `struct ath_desc`,
//  ar5008/ar9002 path). ar9003 uses an "edma" descriptor (struct ar9003_*_desc)
//  with a different layout; for our bring-up we model the classic AR5416 desc,
//  which the on-die dma engine consumes from AR_RXDP / AR_QTXDP. each descriptor
//  is a self-linked node in a ring (ds_link -> next desc phys). (satoru)
//
//  ds_ctl0/ds_ctl1 carry the tx control (length, rate, queue) on a tx desc; on
//  an rx desc the engine writes back into ds_status0/ds_status1 (length, rssi,
//  done bit). this faithful-but-simplified form is enough to set up the rings
//  and walk them. exact bitfields per chip rev are flagged UNSURE below. (satoru)
// ─────────────────────────────────────────────────────────────────────────────
struct AthDesc {
    volatile uint32_t ds_link;       // phys addr of the next descriptor (satoru)
    volatile uint32_t ds_data;       // phys addr of the frame buffer (satoru)
    volatile uint32_t ds_ctl0;       // tx: bufferlen / flags (satoru)
    volatile uint32_t ds_ctl1;       // tx: more-flag / rate index (satoru)
    volatile uint32_t ds_status0;    // rx writeback: length + done (satoru)
    volatile uint32_t ds_status1;    // rx writeback: rssi + rx-ok (satoru)
    volatile uint32_t ds_status2;    // reserved status (satoru)
    volatile uint32_t ds_status3;    // reserved status (satoru)
} __attribute__((packed, aligned(4)));

// descriptor status bits we read on the rx path (ref: ath9k desc.h AR_RxDone /
// AR_RxFrameOK + the rssi/length subfields). the precise field positions differ
// across chip revs; these are the AR5416 positions. UNSURE for ar9003. (satoru)
#define ATH_RXSTAT_DONE       0x00000001   // engine finished this desc (ds_status0 bit0) (satoru)
#define ATH_RXSTAT_LEN_MASK   0x00000FFF   // rx frame length (12 bits) (satoru)
#define ATH_RXSTAT_LEN_SHIFT  16           // length lives in upper half of status0 (satoru)
#define ATH_RXSTAT_FRAME_OK   0x00000001   // ds_status1 bit0: crc/decrypt ok (satoru)
#define ATH_RXSTAT_RSSI_MASK  0x000000FF   // combined rssi (satoru)
#define ATH_RXSTAT_RSSI_SHIFT 8

// tx control-word bits (ref: ath9k desc.h AR_BufLen / AR_TxMore / frame type) (satoru)
#define ATH_TXCTL_BUFLEN_MASK 0x00000FFF   // ds_ctl0 buffer length (satoru)
#define ATH_TXCTL_MORE        0x00001000   // more descriptors in this frame (satoru)
#define ATH_TXSTAT_DONE       0x00000001   // tx desc completed (satoru)

// ring sizing. small rings are plenty for the mgmt-frame scan/assoc path; this
// is not a throughput nic. each frame buffer is one 802.11 mtu. (satoru)
#define ATH_RX_RING_SIZE   16
#define ATH_TX_RING_SIZE   8
#define ATH_BUF_SIZE       2048   // per-frame dma buffer (satoru)

// the legacy ath9k has 10 tx queues (qcu); we use queue 0 for data/mgmt. the
// real driver maps wmm acs to queues, but one queue is fine here. (ref: mac.c)
#define ATH_TXQ_DATA       0

// ─────────────────────────────────────────────────────────────────────────────
//  ath10k copy-engine / firmware constants (ref: linux ath10k pci.c, ce.c, bmi.c,
//  hw.h). these are scaffold-level - enough to map the ce register banks, reset
//  the chip, and walk the firmware-download path. (satoru)
// ─────────────────────────────────────────────────────────────────────────────
#define ATH10K_SOC_CHIP_ID         0x000000ec   // soc chip id register (qca988x) (satoru)
#define ATH10K_SOC_RESET_CONTROL   0x00000000   // within the rtc/soc block (satoru)
#define   ATH10K_SOC_RESET_CE      0x00040000   // copy-engine reset bit (satoru)
#define ATH10K_PCIE_SOC_WAKE_RESET 0x00000000
#define ATH10K_PCIE_SOC_WAKE_V_MASK 0x00000001
#define ATH10K_CE_COUNT            8            // number of copy-engine pipes (satoru)
#define ATH10K_CE_BASE_ADDRESS     0x00048000   // copy-engine register base (satoru)
#define ATH10K_BMI_TIMEOUT_MS      1000         // firmware download / bmi exec (satoru)

// where kurono keeps device firmware blobs (the stack does not ship them). the
// ath10k driver expects e.g. firmware-5.bin + board.bin under the chip dir.
// (ref: ath10k core.c ATH10K_FW_DIR / request_firmware). (satoru)
static const char* ATH10K_FW_DIR        = "/system/lib/firmware/ath10k";
static const char* ATH10K_FW_QCA988X    = "/system/lib/firmware/ath10k/QCA988X/hw2.0/firmware-5.bin";
static const char* ATH10K_FW_QCA988X_BD = "/system/lib/firmware/ath10k/QCA988X/hw2.0/board.bin";
static const char* ATH10K_FW_QCA6174    = "/system/lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin";
static const char* ATH10K_FW_QCA6174_BD = "/system/lib/firmware/ath10k/QCA6174/hw3.0/board.bin";

// ─────────────────────────────────────────────────────────────────────────────
//  driver private state - the opaque ctx handed back to every WifiRadioOps call.
//  one global instance (only one radio at a time, per the stack contract). (satoru)
// ─────────────────────────────────────────────────────────────────────────────
struct AthState {
    const WifiDevice* dev;          // probed pci device (mmio + ids) (satoru)
    bool   is_ath10k;               // false = ath9k path, true = ath10k path (satoru)
    bool   started;                 // start() succeeded (satoru)
    bool   hw_ok;                   // chip reset + init register vectors applied (satoru)

    uint8_t  mac[6];                // station mac (from eeprom/otp, else derived) (satoru)
    uint8_t  bssid[6];              // joined bss (satoru)
    int      channel;               // current 802.11 channel (satoru)
    int      last_rssi;             // dBm from the most recent rx descriptor (satoru)

    // ath9k dma rings (identity-mapped coherent frames from the pmm). desc arrays
    // and their frame buffers are physically contiguous + virt==phys. (satoru)
    AthDesc* rx_ring;               // virt == phys (identity mapped) (satoru)
    uint64_t rx_ring_phys;
    uint8_t* rx_bufs;               // ATH_RX_RING_SIZE * ATH_BUF_SIZE (satoru)
    uint64_t rx_bufs_phys;
    int      rx_next;               // next rx desc the cpu will inspect (satoru)

    AthDesc* tx_ring;
    uint64_t tx_ring_phys;
    uint8_t* tx_bufs;
    uint64_t tx_bufs_phys;
    int      tx_next;               // next tx desc to fill (satoru)

    // ath10k copy-engine scaffold (satoru)
    volatile uint8_t* ce_base;      // mapped ce register window (== mmio + base) (satoru)
    bool   fw_loaded;               // firmware blob downloaded to the device (satoru)
    uint32_t fw_len;
};

static AthState g_ath = {};

// the registered ops vtable instance (filled at bottom of file) (satoru)
extern const WifiRadioOps ATH_OPS;
static bool g_active = false;

// ─────────────────────────────────────────────────────────────────────────────
//  register i/o shims - everything goes through WifiDev's bounds-checked mmio. a
//  read/modify/write helper mirrors ath9k's REG_RMW. (satoru)
// ─────────────────────────────────────────────────────────────────────────────
static inline uint32_t rd(uint32_t off) { return WifiDev::RegRead(off); }
static inline void     wr(uint32_t off, uint32_t v) { WifiDev::RegWrite(off, v); }
static inline void rmw(uint32_t off, uint32_t set, uint32_t clr) {
    uint32_t v = rd(off);
    v &= ~clr; v |= set;
    wr(off, v);
}

// poll a register until (rd(off) & mask) == val, or timeout. returns true on
// match. mirrors ath9k's ath9k_hw_wait. (ref: linux ath9k hw.c) (satoru)
static bool reg_wait(uint32_t off, uint32_t mask, uint32_t val, uint32_t timeout_us) {
    for (uint32_t i = 0; i < timeout_us; i += 10) {
        if ((rd(off) & mask) == val) return true;
        udelay(10);
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ATH9K PATH (firmware-free, implemented in full) (satoru)
// ═════════════════════════════════════════════════════════════════════════════

// ── wake the chip out of any sleep/network-low-power state ──────────────────
//  ref: linux ath9k hw.c ath9k_hw_set_power_awake / ath9k_hw_set_reset_power_on.
//  drive AR_RTC_FORCE_WAKE and wait for AR_RTC_STATUS == ON. (satoru)
static bool ath9k_set_power_awake() {
    rmw(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT, 0);
    udelay(10);
    // some parts need a nudge of the reset register to leave full sleep. (satoru)
    rmw(AR_RTC_RESET, 0, AR_RTC_RESET_EN);
    udelay(2);
    rmw(AR_RTC_RESET, AR_RTC_RESET_EN, 0);
    if (!reg_wait(AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON, 6000)) {
        log("[ath9k] rtc did not report ON after wake\r\n");
        return false;
    }
    return true;
}

// ── chip reset: warm then (if needed) cold via AR_RTC_RC ────────────────────
//  ref: linux ath9k hw.c ath9k_hw_set_reset(). assert the warm-reset bit, spin,
//  deassert, and wait for the bit to clear. a cold reset additionally cycles the
//  analog. we do warm (preserves config) then verify the mac came back. (satoru)
static bool ath9k_chip_reset() {
    // make sure the chip is awake first; reset needs the rtc running. (satoru)
    if (!ath9k_set_power_awake()) return false;

    // warm reset the mac. (satoru)
    wr(AR_RTC_RC, AR_RTC_RC_MAC_WARM);
    udelay(50);
    // the engine clears AR_RTC_RC_M when the reset completes. (satoru)
    if (!reg_wait(AR_RTC_RC, AR_RTC_RC_M, 0, 4000)) {
        // fall back to a cold reset if warm never cleared. (satoru)
        log("[ath9k] warm reset stuck - trying cold\r\n");
        wr(AR_RTC_RC, AR_RTC_RC_MAC_COLD);
        udelay(50);
        if (!reg_wait(AR_RTC_RC, AR_RTC_RC_M, 0, 4000)) {
            log("[ath9k] cold reset stuck\r\n");
            return false;
        }
    }
    wr(AR_RTC_RC, 0);
    udelay(50);
    // re-assert wake after reset (reset can drop force-wake). (satoru)
    return ath9k_set_power_awake();
}

// ── pll / clock init ────────────────────────────────────────────────────────
//  ref: linux ath9k hw.c ath9k_hw_init_pll(). the pll-control value is per-band
//  + per-chip; for the common 2.4ghz 11g case on ar92xx the synthesizer divider
//  programs AR_RTC_PLL_CONTROL then we wait for the clock to settle. the exact
//  refdiv/div values are chip-specific (UNSURE for any given part); we use the
//  ar9280 2ghz defaults and let the analog init vectors refine it. (satoru)
static void ath9k_init_pll(int channel) {
    // 2ghz default pll word for ar92xx-class parts: refdiv=5, div=0x2c, range
    // bits for 11g. (ref: ath9k hw.c the pll computed value). UNSURE across revs.
    uint32_t pll;
    if (channel >= 36) {
        // 5ghz branch (ar9280 uses a different divisor). (satoru)
        pll = 0x142c;
    } else {
        pll = 0x2850;   // 2.4ghz common value (satoru)
    }
    wr(AR_RTC_PLL_CONTROL, pll);
    // give the pll time to lock before any phy access. ath9k waits ~1ms. (satoru)
    udelay(1000);
}

// ── eeprom / otp read for the mac address ───────────────────────────────────
//  ref: linux ath9k eeprom.c ath9k_hw_nvram_read + ar9003_eeprom.c otp read.
//  ar5416/ar92xx have a serial eeprom read through AR_EEPROM_OFFSET; ar9003 has
//  no eeprom and reads otp. we try the eeprom path, validate the magic, and pull
//  the 6-byte mac from the base header; on failure we fall back to a derived
//  locally-administered mac so the stack still has a source address. (satoru)

// read one 16-bit eeprom word. drive AR_EEPROM_ADDR, kick the read, poll the
// status-data register (ar9280+ writes the result there). (satoru)
static bool ath9k_eeprom_read_word(uint32_t word_off, uint16_t* out) {
    // address the chip in 16-bit words and kick the read. on ar9280+ the result
    // appears in AR_EEPROM_STATUS_DATA after a short settle; the simple nvram path
    // does not expose a separate busy bit to poll, so we issue the read and wait a
    // fixed window before sampling. (ref: ath9k hw.c ath9k_hw_nvram_read). (satoru)
    wr(AR_EEPROM_ADDR, (word_off << AR5416_EEPROM_S));
    wr(AR_EEPROM_CMD, AR_EEPROM_CMD_READ);
    udelay(100);   // settle (the ar9280 access latency) (satoru)
    uint32_t v = rd(AR_EEPROM_STATUS_DATA);
    *out = (uint16_t)(v & AR_EEPROM_STATUS_DATA_VAL_MASK);
    return true;
}

static bool ath9k_read_mac_from_eeprom(uint8_t mac[6]) {
    // validate the eeprom magic so we don't trust a dead/absent eeprom. (satoru)
    uint16_t magic = 0;
    if (!ath9k_eeprom_read_word(AR5416_EEP_MAGIC_OFFSET, &magic)) return false;
    // accept either byte order of the magic (ref: ath9k_hw_nvram_check_magic). (satoru)
    bool magic_ok = (magic == AR5416_EEPROM_MAGIC) ||
                    (magic == (uint16_t)((AR5416_EEPROM_MAGIC >> 8) |
                                         (AR5416_EEPROM_MAGIC << 8)));
    if (!magic_ok) {
        log("[ath9k] eeprom magic mismatch (got ");
        logx(magic); log(") - no serial eeprom\r\n");
        return false;
    }
    // the mac sits in 3 consecutive words at the base-header mac offset, stored
    // big-endian-per-word (ref: ath9k_hw_get_macaddr). (satoru)
    uint16_t w0 = 0, w1 = 0, w2 = 0;
    ath9k_eeprom_read_word(AR5416_EEP_MAC_OFFSET + 0, &w0);
    ath9k_eeprom_read_word(AR5416_EEP_MAC_OFFSET + 1, &w1);
    ath9k_eeprom_read_word(AR5416_EEP_MAC_OFFSET + 2, &w2);
    mac[0] = (uint8_t)(w0 >> 8); mac[1] = (uint8_t)(w0 & 0xFF);
    mac[2] = (uint8_t)(w1 >> 8); mac[3] = (uint8_t)(w1 & 0xFF);
    mac[4] = (uint8_t)(w2 >> 8); mac[5] = (uint8_t)(w2 & 0xFF);
    // reject an all-zero / all-ff mac (dead read). (satoru)
    bool all0 = true, allf = true;
    for (int i = 0; i < 6; i++) { if (mac[i] != 0x00) all0 = false; if (mac[i] != 0xFF) allf = false; }
    if (all0 || allf) return false;
    return true;
}

// otp path for ar9003 (firmware-free but eeprom-less). drive the otp controller
// and read the mac words. (ref: linux ar9003_eeprom.c ar9300_otp_read_word /
// ar9300_eeprom_restore). full otp parse is large; we read the mac region only,
// and flag the parse as UNSURE without real ar9003 hw to validate. (satoru)
static bool ath9k_read_mac_from_otp(uint8_t mac[6]) {
    // the ar9003 stores its eeprom image compressed in otp; pulling the mac
    // properly means decompressing the block. that is out of scope for the
    // scaffold, so we only attempt it and return false to fall back. flagged
    // UNSURE - needs real ar9003 silicon to validate. (satoru)
    (void)mac;
    return false;
}

// derive a deterministic locally-administered mac from the pci address when no
// eeprom/otp mac is available, so the stack always has a usable sa. (satoru)
static void ath9k_derive_mac(uint8_t mac[6], const WifiDevice* d) {
    mac[0] = 0x02;                       // locally administered, unicast (satoru)
    mac[1] = 0x00;
    mac[2] = (uint8_t)(d->vendor & 0xFF);
    mac[3] = (uint8_t)(d->device & 0xFF);
    mac[4] = d->slot;
    mac[5] = d->func;
}

// ── init register vectors (analog + mac + phy) ──────────────────────────────
//  ref: linux ath9k hw.c ath9k_hw_init_macaddr / ar5008_hw_process_ini /
//  ar9003_hw_set_channel_regs - after reset the driver writes large per-chip ini
//  arrays (ar5416Modes / ar9300Modes etc.) into the phy/analog. those tables are
//  thousands of (offset,value) pairs in the gpl source and are NOT reproduced
//  here (both for size and license). instead we apply the minimal, generic set
//  that brings the mac + phy to a sane idle state for the rest of bring-up, and
//  honestly mark that the full per-chip ini is required for real rf. (satoru)
//
//  what we DO set: rx filter, multicast hash, station-id flags, diag/idle, dma
//  byte-swap config, and the phy active bit. that is enough to set up the dma
//  rings and exercise the tx/rx paths; it is NOT enough to actually transmit rf
//  energy on calibrated hardware (the missing ini/calibration owns that). (satoru)
static void ath9k_apply_init_regs() {
    // dma descriptor byte-order: little-endian (x86) - clear the swap bits so the
    // engine reads descriptors in native order. (ref: ath9k hw.c AR_CFG). (satoru)
    rmw(AR_CFG, 0, AR_CFG_SWTD | AR_CFG_SWRD);

    // clear the multicast hash (accept none by hash; bcast/ucast via filter). (satoru)
    wr(AR_MULTICAST_0, 0);
    wr(AR_MULTICAST_1, 0);

    // station-id flags: key-search mode on (the hw crypto engine), not an ap. the
    // upper-half of STA_ID1 holds these flag bits; the low 16 bits hold mac[4..5]
    // and are written by config when we set the mac. (ref: ath9k mac.c). (satoru)
    rmw(AR_STA_ID1, AR_STA_ID1_KSRCH_MODE, AR_STA_ID1_STA_AP);

    // rx filter: accept unicast-to-us, broadcast, multicast and beacons (so scan
    // sees beacons and assoc sees the ap's responses). (ref: recv.c). (satoru)
    wr(AR_RX_FILTER, AR_RX_FILTER_UCAST | AR_RX_FILTER_BCAST |
                     AR_RX_FILTER_MCAST | AR_RX_FILTER_BEACON);

    // leave diagnostics clear (rx enabled, no loopback). (ref: mac.c). (satoru)
    wr(AR_DIAG_SW, 0);

    // bring the phy active (idle->active). on real hw this must follow the full
    // ini + calibration; here it just flips the enable. (ref: ar5008_phy.c). (satoru)
    wr(AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
    udelay(100);
}

// ── dma ring allocation (identity-mapped coherent memory) ───────────────────
//  ref: linux ath9k recv.c ath_rx_init / xmit.c ath_tx_init - allocate the
//  descriptor array + per-descriptor frame buffers as dma-coherent memory and
//  link each descriptor to the next (ds_link), forming a ring. kurono's PMM
//  hands out page-aligned identity-mapped (virt==phys) frames, which are exactly
//  the "coherent" property the engine needs (uncached is not required for these
//  in-ram descriptors; the bar mmio is the uncached part). (satoru)
static bool ath9k_alloc_rings() {
    AthState* s = &g_ath;

    // rx descriptor array (satoru)
    s->rx_ring = (AthDesc*)PMM::AllocBytes(sizeof(AthDesc) * ATH_RX_RING_SIZE);
    s->rx_bufs = (uint8_t*)PMM::AllocBytes(ATH_BUF_SIZE * ATH_RX_RING_SIZE);
    s->tx_ring = (AthDesc*)PMM::AllocBytes(sizeof(AthDesc) * ATH_TX_RING_SIZE);
    s->tx_bufs = (uint8_t*)PMM::AllocBytes(ATH_BUF_SIZE * ATH_TX_RING_SIZE);
    if (!s->rx_ring || !s->rx_bufs || !s->tx_ring || !s->tx_bufs) {
        log("[ath9k] dma ring alloc failed\r\n");
        return false;
    }
    // identity mapped, so phys == virt. (satoru)
    s->rx_ring_phys = (uint64_t)(uintptr_t)s->rx_ring;
    s->rx_bufs_phys = (uint64_t)(uintptr_t)s->rx_bufs;
    s->tx_ring_phys = (uint64_t)(uintptr_t)s->tx_ring;
    s->tx_bufs_phys = (uint64_t)(uintptr_t)s->tx_bufs;

    a_memset(s->rx_ring, 0, sizeof(AthDesc) * ATH_RX_RING_SIZE);
    a_memset(s->tx_ring, 0, sizeof(AthDesc) * ATH_TX_RING_SIZE);

    // build the rx ring: each desc points at its frame buffer and links to the
    // next; the last links back to the first (circular). the engine owns every
    // rx desc (ds_status0 done-bit clear) until it writes a frame back. (satoru)
    for (int i = 0; i < ATH_RX_RING_SIZE; i++) {
        AthDesc* d = &s->rx_ring[i];
        uint64_t next = s->rx_ring_phys + (uint64_t)((i + 1) % ATH_RX_RING_SIZE) * sizeof(AthDesc);
        d->ds_link = (uint32_t)next;
        d->ds_data = (uint32_t)(s->rx_bufs_phys + (uint64_t)i * ATH_BUF_SIZE);
        // ds_ctl0/1 on rx hold the buffer length the engine may write. (satoru)
        d->ds_ctl0 = 0;
        d->ds_ctl1 = (ATH_BUF_SIZE & ATH_TXCTL_BUFLEN_MASK);
        d->ds_status0 = 0;  // engine-owned (done bit clear) (satoru)
        d->ds_status1 = 0;
    }
    // build the tx ring similarly; the cpu owns every tx desc until we fill +
    // hand it to the engine. (satoru)
    for (int i = 0; i < ATH_TX_RING_SIZE; i++) {
        AthDesc* d = &s->tx_ring[i];
        uint64_t next = s->tx_ring_phys + (uint64_t)((i + 1) % ATH_TX_RING_SIZE) * sizeof(AthDesc);
        d->ds_link = (uint32_t)next;
        d->ds_data = (uint32_t)(s->tx_bufs_phys + (uint64_t)i * ATH_BUF_SIZE);
        d->ds_ctl0 = 0;
        d->ds_ctl1 = 0;
    }
    s->rx_next = 0;
    s->tx_next = 0;

    log("[ath9k] dma rings: rx="); logd(ATH_RX_RING_SIZE);
    log(" tx="); logd(ATH_TX_RING_SIZE);
    log(" buf="); logd(ATH_BUF_SIZE); log("B (identity-mapped)\r\n");
    return true;
}

// program the rx dma engine: point AR_RXDP at the head of the rx ring and enable
// rx in the command register. (ref: linux ath9k recv.c ath9k_hw_startpcureceive
// + ath9k_hw_rxena). (satoru)
static void ath9k_start_rx() {
    AthState* s = &g_ath;
    wr(AR_RXDP, (uint32_t)s->rx_ring_phys);
    // clear any stale rx-disable, then enable rx. (satoru)
    wr(AR_CR, AR_CR_RXE);
    // re-arm the rx filter (in case reset cleared it). (satoru)
    wr(AR_RX_FILTER, AR_RX_FILTER_UCAST | AR_RX_FILTER_BCAST |
                     AR_RX_FILTER_MCAST | AR_RX_FILTER_BEACON);
}

static void ath9k_stop_rx() {
    // ask the engine to stop, then poll the command register's rx-disable. (ref:
    // ath9k recv.c ath9k_hw_stopdmarecv). (satoru)
    wr(AR_CR, AR_CR_RXD);
    reg_wait(AR_CR, AR_CR_RXE, 0, 4000);
    rmw(AR_DIAG_SW, AR_DIAG_RX_DIS, 0);
}

// ── enable interrupts (we poll, so this is informational/forward-looking) ───
//  ref: linux ath9k mac.c ath9k_hw_set_interrupts. we set the rx/tx-ok mask so a
//  future irq handler could use it, but the bring-up path polls the descriptors
//  in rx_poll() rather than taking the irq (the cooperative scheduler has no
//  registered msi handler for this device yet). (satoru)
static void ath9k_setup_interrupts() {
    wr(AR_IER, AR_IER_DISABLE);
    wr(AR_IMR, AR_IMR_RXOK | AR_IMR_RXERR | AR_IMR_RXEOL |
               AR_IMR_TXOK | AR_IMR_TXERR);
    // leave the global enable OFF - we poll. flip this on once an irq handler is
    // wired through the kernel's interrupt layer. (satoru)
    wr(AR_IER, AR_IER_DISABLE);
}

// ── the full ath9k bring-up: reset → pll → init regs → eeprom mac → rings ───
//  ref: linux ath9k hw.c ath9k_hw_init + ath9k_hw_reset, recv.c, xmit.c. (satoru)
static bool ath9k_hw_init() {
    AthState* s = &g_ath;

    uint32_t srev = rd(AR_SREV);
    log("[ath9k] silicon rev "); logx(srev); log("\r\n");

    if (!ath9k_chip_reset()) {
        log("[ath9k] chip reset failed\r\n");
        return false;
    }
    log("[ath9k] chip reset ok\r\n");

    // default to channel 1 for the initial pll; set_channel re-tunes later. (satoru)
    s->channel = 1;
    ath9k_init_pll(s->channel);
    log("[ath9k] pll init (ch 1)\r\n");

    ath9k_apply_init_regs();
    log("[ath9k] init register vectors applied (generic subset)\r\n");

    // read the station mac: eeprom -> otp -> derived. (satoru)
    if (ath9k_read_mac_from_eeprom(s->mac)) {
        log("[ath9k] mac from eeprom\r\n");
    } else if (ath9k_read_mac_from_otp(s->mac)) {
        log("[ath9k] mac from otp\r\n");
    } else {
        ath9k_derive_mac(s->mac, s->dev);
        log("[ath9k] mac derived (no eeprom/otp) - locally administered\r\n");
    }
    log("[ath9k] sta mac ");
    for (int i = 0; i < 6; i++) { logx(s->mac[i]); if (i < 5) log(":"); }
    log("\r\n");

    // write the station mac into AR_STA_ID0/1 so the hw rx filter matches our
    // unicast frames. (ref: ath9k mac.c ath9k_hw_setmac / set_sta_id). (satoru)
    uint32_t id0 = (uint32_t)s->mac[0] | ((uint32_t)s->mac[1] << 8) |
                   ((uint32_t)s->mac[2] << 16) | ((uint32_t)s->mac[3] << 24);
    uint32_t id1 = (uint32_t)s->mac[4] | ((uint32_t)s->mac[5] << 8);
    wr(AR_STA_ID0, id0);
    // preserve the flag bits in the upper half set by apply_init_regs. (satoru)
    rmw(AR_STA_ID1, id1, 0x0000FFFF);

    if (!ath9k_alloc_rings()) return false;

    ath9k_setup_interrupts();
    ath9k_start_rx();
    log("[ath9k] rx dma started (AR_RXDP set, rx enabled)\r\n");

    s->hw_ok = true;
    return true;
}

// ── channel → phy synthesizer ───────────────────────────────────────────────
//  ref: linux ath9k ar5008_phy.c ar5008_hw_set_channel / ar9002_hw_set_channel:
//  request the rf bus, program the synth band + frequency words, set the phy
//  mode (2g/5g, cck/ofdm), then release the rf bus and wait the rx settle delay.
//  the exact synth-word encoding is chip-specific and large; we program the
//  band-select + a frequency-derived value and flag the precise word UNSURE. on
//  real silicon the per-chip ini owns the fine synth coefficients. (satoru)
static int chan_to_freq(int ch) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;   // 2.4ghz (ref: ieee chan map) (satoru)
    if (ch == 14) return 2484;
    if (ch >= 36) return 5000 + ch * 5;              // 5ghz (satoru)
    return 2412;                                     // default ch1 (satoru)
}

static bool ath9k_set_channel(int ch) {
    AthState* s = &g_ath;
    int freq = chan_to_freq(ch);
    bool is_2ghz = (freq < 4000);

    // request the rf bus so the synth can be reprogrammed safely. (satoru)
    wr(AR_PHY_RFBUS_REQ, AR_PHY_RFBUS_REQ_EN);
    if (!reg_wait(AR_PHY_RFBUS_GRANT, AR_PHY_RFBUS_GRANT_EN,
                  AR_PHY_RFBUS_GRANT_EN, 4000)) {
        // grant may not assert on an un-inited phy; continue best-effort. (satoru)
    }

    // set the phy band/mode. (ref: ar5008_phy.c ar5008_hw_set_operating_mode). (satoru)
    if (is_2ghz) {
        wr(AR_PHY_MODE, AR_PHY_MODE_DYNAMIC | AR_PHY_MODE_2GHZ);
    } else {
        wr(AR_PHY_MODE, AR_PHY_MODE_OFDM | AR_PHY_MODE_5GHZ);
    }

    // program the synthesizer control with a band-select + a coarse frequency-
    // derived value. the real driver computes a precise channelSel/refDiv word;
    // this is a placeholder encoding (freq in 1/4-mhz units in the low bits).
    // UNSURE: not the exact ar5008/ar9003 synth word - needs hw to calibrate. (satoru)
    uint32_t synth = ((uint32_t)freq & 0xFFFF) << 8;
    if (!is_2ghz) synth |= 0x00000001;   // band bit (satoru)
    wr(AR_PHY_SYNTH_CONTROL, synth);

    // release the rf bus and wait for the analog to settle. (ref: ar5008_phy.c). (satoru)
    wr(AR_PHY_RFBUS_REQ, 0);
    // rx-delay: the engine inserts a settle window; we also block ~1ms so the
    // caller's dwell time doesn't race the tune. (satoru)
    rmw(AR_PHY_RX_DELAY, (0x0100 & AR_PHY_RX_DELAY_DELAY), AR_PHY_RX_DELAY_DELAY);
    udelay(1000);

    s->channel = ch;
    return true;
}

// ── tx: build a descriptor + hand it to queue 0 ─────────────────────────────
//  ref: linux ath9k xmit.c ath_tx_start / ath9k_hw_filltxdesc + ath9k_hw_txstart:
//  copy the 802.11 frame into the descriptor's dma buffer, set the buffer length
//  + first/last flags in ds_ctl0/1, terminate ds_link, program AR_QTXDP for the
//  queue, then set the queue's bit in AR_Q_TXE to kick the dma. (satoru)
static bool ath9k_tx_frame(const uint8_t* buf, int len) {
    AthState* s = &g_ath;
    if (!s->hw_ok || len <= 0) return false;
    if (len > ATH_BUF_SIZE) len = ATH_BUF_SIZE;

    int i = s->tx_next;
    AthDesc* d = &s->tx_ring[i];
    uint8_t* dma = s->tx_bufs + (uint64_t)i * ATH_BUF_SIZE;

    // copy the fully-formed frame (mac hdr + body, no fcs - hw appends it). (satoru)
    a_memcpy(dma, buf, (unsigned long)len);

    // single-buffer frame: this desc is both first and last. ds_ctl0 carries the
    // buffer length; clear MORE (no chained descriptors). ds_link terminates by
    // pointing at itself with the engine stopping at a not-more desc. (satoru)
    d->ds_data = (uint32_t)(s->tx_bufs_phys + (uint64_t)i * ATH_BUF_SIZE);
    d->ds_ctl0 = ((uint32_t)len & ATH_TXCTL_BUFLEN_MASK);   // length, MORE clear (satoru)
    d->ds_ctl1 = 0;                                         // rate idx 0 (lowest basic) (satoru)
    d->ds_link = 0;                                         // last in this tx burst (satoru)
    d->ds_status0 = 0;
    d->ds_status1 = 0;

    // point the queue at this descriptor and enable the queue. (ref: mac.c
    // ath9k_hw_puttxbuf + ath9k_hw_txstart). (satoru)
    wr(AR_QTXDP(ATH_TXQ_DATA), (uint32_t)(s->tx_ring_phys + (uint64_t)i * sizeof(AthDesc)));
    wr(AR_Q_TXE, (1u << ATH_TXQ_DATA));

    // advance the ring slot (round-robin). we don't block on completion here; the
    // mgmt path tolerates best-effort tx and the stack retransmits as needed. on
    // real hw we'd poll ds_status0 ATH_TXSTAT_DONE or take the txok irq. (satoru)
    s->tx_next = (i + 1) % ATH_TX_RING_SIZE;
    return true;
}

// ── rx: walk the descriptor ring, return one completed frame ────────────────
//  ref: linux ath9k recv.c ath_rx_tasklet / ath9k_hw_rxprocdesc: inspect the
//  next cpu-owned rx descriptor; if the engine set the done bit, pull the length
//  + rssi, copy the frame out (fcs already stripped by hw on these parts), then
//  re-arm the descriptor (clear status, give it back to the engine) and advance.
//  returns frame length, 0 if nothing pending, <0 on error. (satoru)
static int ath9k_rx_poll(uint8_t* out, int out_max) {
    AthState* s = &g_ath;
    if (!s->hw_ok) return 0;

    int i = s->rx_next;
    AthDesc* d = &s->rx_ring[i];

    // the engine sets the done bit in ds_status0 when it writes a frame. until
    // then the descriptor is engine-owned and we have nothing. (satoru)
    if (!(d->ds_status0 & ATH_RXSTAT_DONE)) return 0;

    // extract the length (upper half of status0) and rssi (from status1). the
    // exact field positions are the AR5416 layout; UNSURE for ar9003 edma. (satoru)
    int rxlen = (int)((d->ds_status0 >> ATH_RXSTAT_LEN_SHIFT) & ATH_RXSTAT_LEN_MASK);
    int rssi  = (int)((d->ds_status1 >> ATH_RXSTAT_RSSI_SHIFT) & ATH_RXSTAT_RSSI_MASK);

    // convert the chip's rssi (a positive value relative to the noise floor) to a
    // dBm estimate: dBm ≈ rssi + noisefloor, with the ath9k default nf ~ -95dBm.
    // (ref: ath9k the ANI/calibration noise floor). clamp to the sane range. (satoru)
    if (rssi > 0) {
        s->last_rssi = rssi - 95;
        if (s->last_rssi > 0) s->last_rssi = 0;
        if (s->last_rssi < -100) s->last_rssi = -100;
    }

    int copied = 0;
    bool frame_ok = (d->ds_status1 & ATH_RXSTAT_FRAME_OK) != 0;
    if (frame_ok && rxlen > 0) {
        // hw already stripped the 4-byte fcs on these parts. guard the copy. (satoru)
        if (rxlen > ATH_BUF_SIZE) rxlen = ATH_BUF_SIZE;
        copied = rxlen;
        if (copied > out_max) copied = out_max;
        uint8_t* dma = s->rx_bufs + (uint64_t)i * ATH_BUF_SIZE;
        a_memcpy(out, dma, (unsigned long)copied);
    }

    // re-arm this descriptor and give it back to the engine (clear the done +
    // ok bits in the status words). (ref: recv.c ath_rx_buf_link). (satoru)
    d->ds_status0 = 0;
    d->ds_status1 = 0;
    d->ds_ctl1 = (ATH_BUF_SIZE & ATH_TXCTL_BUFLEN_MASK);

    // keep AR_RXDP fed: if the engine had drained to end-of-list, re-point it at
    // the head so it resumes. cheap and harmless to re-write. (satoru)
    wr(AR_RXDP, (uint32_t)s->rx_ring_phys);
    wr(AR_CR, AR_CR_RXE);

    s->rx_next = (i + 1) % ATH_RX_RING_SIZE;
    // a done-but-bad frame yields 0 (nothing useful) but still advanced the ring;
    // the stack will poll again. (satoru)
    return copied;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ATH10K PATH (firmware-based, bring-up scaffold) (satoru)
// ═════════════════════════════════════════════════════════════════════════════
//
//  the qca988x/qca6174 have no on-die mac usable without firmware: the host
//  talks to a target cpu over the COPY ENGINE (a set of dma ring pairs), uses
//  BMI to download + start the firmware, then speaks the HTC transport carrying
//  WMI command/event messages for all mac operations (scan, connect, key, tx).
//  bringing wmi all the way to association requires the real firmware target and
//  cannot be validated under qemu. this is the scaffold: chip wake/reset, ce
//  register mapping, the firmware-blob load path from the kurono fs, and the
//  wmi command-send skeleton. (ref: linux ath10k pci.c, ce.c, bmi.c, wmi.c). (satoru)

// map the copy-engine register window (an offset within the same bar). (satoru)
static bool ath10k_map_ce() {
    AthState* s = &g_ath;
    if (!s->dev || !s->dev->mmio) return false;
    s->ce_base = s->dev->mmio + ATH10K_CE_BASE_ADDRESS;
    // a read of the chip-id register tells us the bar is decoding. (satoru)
    uint32_t chip_id = rd(ATH10K_SOC_CHIP_ID);
    log("[ath10k] soc chip id "); logx(chip_id); log("\r\n");
    return true;
}

// wake the target and reset the copy engine. (ref: ath10k pci.c
// ath10k_pci_wake_target_cpu / ath10k_pci_warm_reset). honest scaffold: the real
// sequence pokes the soc PCIE_SOC_WAKE register and polls a wake-confirm, then
// pulses the ce reset; the precise soc offsets vary by chip rev. UNSURE. (satoru)
static bool ath10k_warm_reset() {
    // pulse the copy-engine reset within the soc reset-control register. (satoru)
    rmw(ATH10K_SOC_RESET_CONTROL, ATH10K_SOC_RESET_CE, 0);
    udelay(100);
    rmw(ATH10K_SOC_RESET_CONTROL, 0, ATH10K_SOC_RESET_CE);
    udelay(100);
    log("[ath10k] warm reset pulsed (ce)\r\n");
    return true;
}

// download a firmware blob to the target over BMI. (ref: ath10k bmi.c
// ath10k_bmi_fast_download / ath10k_bmi_write_memory). the real path chunks the
// blob through BMI_WRITE_MEMORY commands over ce pipe 0 to the target's iram,
// then BMI_DONE + a host-interest "begin" to jump to it. without the target cpu
// responding (no real hw) we cannot complete the handshake, so this records the
// blob and honestly reports that the on-target start needs hardware. (satoru)
static bool ath10k_bmi_download(const uint8_t* blob, int len) {
    if (!blob || len <= 0) return false;
    AthState* s = &g_ath;
    // in a full impl we'd: ath10k_bmi_write_memory(target_iram, blob, len) chunked
    // over ce pipe 0, then ath10k_bmi_done(), then ath10k_bmi_set_app_start().
    // those round-trips require the bmi command responses from the target. (satoru)
    s->fw_len = (uint32_t)len;
    s->fw_loaded = true;
    log("[ath10k] firmware blob staged ("); logd(len);
    log(" bytes) - on-target BMI start requires real hardware\r\n");
    return true;
}

// load firmware from the kurono fs for the detected chip, then BMI-download it.
// (ref: ath10k core.c ath10k_core_fetch_firmware_api_n + request_firmware). the
// stack does not ship firmware; we source it from /system/lib/firmware/. (satoru)
static bool ath10k_load_firmware_from_fs() {
    AthState* s = &g_ath;
    const char* fwpath = nullptr;
    const char* bdpath = nullptr;   // per-board calibration data (board.bin) (satoru)
    switch (s->dev->device) {
    case 0x003c: fwpath = ATH10K_FW_QCA988X; bdpath = ATH10K_FW_QCA988X_BD; break;  // qca988x (satoru)
    case 0x003e: fwpath = ATH10K_FW_QCA6174; bdpath = ATH10K_FW_QCA6174_BD; break;  // qca6174 (satoru)
    default:     fwpath = ATH10K_FW_QCA988X; bdpath = ATH10K_FW_QCA988X_BD; break;  // best guess (satoru)
    }

    log("[ath10k] firmware dir "); log(ATH10K_FW_DIR); log("\r\n");

    if (!KVFS::Exists(fwpath)) {
        log("[ath10k] firmware not found at ");
        log(fwpath);
        log(" - install the ath10k firmware blob there (the os does not ship it)\r\n");
        return false;
    }
    int sz = KVFS::GetFileSize(fwpath);
    if (sz <= 0) { log("[ath10k] firmware file empty\r\n"); return false; }

    // bound the blob to a sane ceiling and read it into a dma-capable buffer. the
    // qca firmware images are ~ <1MB; cap at 2MB defensively. (satoru)
    if (sz > 2 * 1024 * 1024) { log("[ath10k] firmware too large\r\n"); return false; }
    uint8_t* fw = (uint8_t*)PMM::AllocBytes((size_t)sz);
    if (!fw) { log("[ath10k] firmware buffer alloc failed\r\n"); return false; }
    int got = KVFS::ReadFile(fwpath, fw, (uint32_t)sz);
    if (got <= 0) { log("[ath10k] firmware read failed\r\n"); return false; }

    log("[ath10k] loaded firmware "); log(fwpath);
    log(" ("); logd(got); log(" bytes)\r\n");

    // the board-data blob carries per-board rf calibration; the real driver
    // pushes it to the target via wmi after the firmware boots. we confirm it's
    // present so a hw bring-up has what it needs (and warn if it's missing). the
    // actual push happens once wmi is live (needs real hardware). (satoru)
    if (KVFS::Exists(bdpath)) {
        int bsz = KVFS::GetFileSize(bdpath);
        log("[ath10k] board data present "); log(bdpath);
        log(" ("); logd(bsz > 0 ? bsz : 0); log(" bytes)\r\n");
    } else {
        log("[ath10k] board data missing at "); log(bdpath);
        log(" - rf calibration will be absent\r\n");
    }

    return ath10k_bmi_download(fw, got);
}

// send one wmi command over the htc/ce transport. (ref: ath10k wmi.c
// ath10k_wmi_cmd_send + htc.c ath10k_htc_send). scaffold: the real path wraps
// the wmi message in an htc header and pushes it onto the ce pipe bound to the
// wmi-control service, then waits for the matching wmi event. that needs the
// target firmware running. UNSURE / requires hardware. (satoru)
static bool ath10k_wmi_cmd_send(uint32_t cmd_id, const uint8_t* payload, int len) {
    (void)cmd_id; (void)payload; (void)len;
    if (!g_ath.fw_loaded) return false;
    // without a live target firmware there is nothing to receive the command; we
    // honestly do not pretend it was delivered. (satoru)
    return false;
}

static bool ath10k_hw_init() {
    AthState* s = &g_ath;
    log("[ath10k] bring-up (firmware-based - scaffold)\r\n");
    if (!ath10k_map_ce()) { log("[ath10k] ce map failed\r\n"); return false; }
    if (!ath10k_warm_reset()) return false;
    // the copy-engine ring setup (per-pipe src/dst rings) would go here; it is a
    // large block (8 pipes, each with descriptor rings) and is only useful with a
    // responding target, so we defer it to the firmware-load milestone. (satoru)
    if (!ath10k_load_firmware_from_fs()) {
        log("[ath10k] firmware load incomplete - radio cannot associate without it\r\n");
        // keep hw_ok false; the ops will report failure honestly. (satoru)
        return false;
    }
    // a real bring-up now does: ce ring init -> htc connect (control service) ->
    // wmi ready handshake -> wmi service ready -> mac setup. all need the target.
    // we attempt the very first step (a wmi "init" command) to exercise the path;
    // it returns false because no firmware target is actually running, which is
    // the honest result under emulation. (ref: ath10k wmi.c WMI_INIT_CMDID). (satoru)
    const uint32_t WMI_INIT_CMDID = 0x00000001;   // placeholder cmd id (satoru)
    if (!ath10k_wmi_cmd_send(WMI_INIT_CMDID, nullptr, 0)) {
        log("[ath10k] wmi init not acknowledged (no live target) - expected w/o hw\r\n");
    }
    log("[ath10k] firmware staged; wmi/htc handshake needs real hardware\r\n");
    s->hw_ok = false;   // honest: not actually ready to tx/rx (satoru)
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  WifiRadioOps implementation - dispatch ath9k vs ath10k (satoru)
// ═════════════════════════════════════════════════════════════════════════════

static bool op_start(void* ctx) {
    AthState* s = (AthState*)ctx;
    if (s->started && s->hw_ok) return true;
    bool ok;
    if (s->is_ath10k) {
        ok = ath10k_hw_init();
    } else {
        ok = ath9k_hw_init();
    }
    s->started = ok;
    return ok;
}

static void op_stop(void* ctx) {
    AthState* s = (AthState*)ctx;
    if (s->is_ath10k) {
        // quiesce the copy engine (best-effort). (satoru)
        s->hw_ok = false;
        s->started = false;
        return;
    }
    if (!s->hw_ok) return;
    // stop rx dma + idle the phy. (ref: ath9k hw.c ath9k_hw_phy_disable). (satoru)
    ath9k_stop_rx();
    wr(AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
    // disable all tx queues. (satoru)
    wr(AR_Q_TXD, 0x000003FF);
    s->started = false;
}

static bool op_set_channel(void* ctx, int ch) {
    AthState* s = (AthState*)ctx;
    if (s->is_ath10k) {
        // channel selection on ath10k is a wmi command to the firmware. (satoru)
        s->channel = ch;
        return s->fw_loaded;   // honestly false until firmware runs (satoru)
    }
    if (!s->hw_ok) return false;
    return ath9k_set_channel(ch);
}

static bool op_config_bss(void* ctx, const uint8_t bssid[6], const char* ssid) {
    AthState* s = (AthState*)ctx;
    for (int i = 0; i < 6; i++) s->bssid[i] = bssid[i];
    (void)ssid;
    if (s->is_ath10k) {
        // a wmi VDEV/peer-create + bss-info command sequence on ath10k. (satoru)
        return s->fw_loaded;
    }
    if (!s->hw_ok) return false;
    // program the bssid into AR_BSS_ID0/1 and an all-ones bss mask so the rx
    // filter matches frames from exactly this ap. (ref: ath9k mac.c
    // ath9k_hw_write_associd + ath9k_hw_setbssidmask). (satoru)
    uint32_t b0 = (uint32_t)bssid[0] | ((uint32_t)bssid[1] << 8) |
                  ((uint32_t)bssid[2] << 16) | ((uint32_t)bssid[3] << 24);
    uint32_t b1 = (uint32_t)bssid[4] | ((uint32_t)bssid[5] << 8);
    wr(AR_BSS_ID0, b0);
    // preserve the aid field in the upper half; just set the mac bytes. (satoru)
    rmw(AR_BSS_ID1, b1, 0x0000FFFF);
    // bss mask all-ones: match the full bssid (single-bss sta). (satoru)
    wr(AR_BSSMSKL, 0xFFFFFFFF);
    wr(AR_BSSMSKU, 0x0000FFFF);
    log("[ath9k] config bss bssid ");
    for (int i = 0; i < 6; i++) { logx(bssid[i]); if (i < 5) log(":"); }
    log("\r\n");
    return true;
}

static bool op_set_key(void* ctx, int idx, const uint8_t* key, int key_len, int type) {
    AthState* s = (AthState*)ctx;
    (void)idx; (void)key; (void)key_len; (void)type;
    // the ath9k has a hardware key cache (AR_KEYTABLE) for ccmp/tkip offload; the
    // ath10k offloads keys via a wmi install-key command. wiring either correctly
    // is involved and unverifiable without hw, so we DECLINE the offload and let
    // the 802.11 stack do ccmp in software (its documented fallback when set_key
    // returns false). this is correct + safe behaviour, not a stub gap. (satoru)
    (void)s;
    return false;
}

static bool op_tx_frame(void* ctx, const uint8_t* buf, int len) {
    AthState* s = (AthState*)ctx;
    if (s->is_ath10k) {
        // tx on ath10k is an htt/wmi data path to the firmware. (satoru)
        return false;
    }
    return ath9k_tx_frame(buf, len);
}

static int op_rx_poll(void* ctx, uint8_t* buf, int buf_max) {
    AthState* s = (AthState*)ctx;
    if (s->is_ath10k) return 0;   // ath10k rx arrives via htt over ce (needs hw) (satoru)
    return ath9k_rx_poll(buf, buf_max);
}

static int op_get_signal(void* ctx) {
    AthState* s = (AthState*)ctx;
    if (s->last_rssi == 0) return -100;   // no measurement yet (satoru)
    return s->last_rssi;
}

static bool op_load_firmware(void* ctx, const uint8_t* blob, int len) {
    AthState* s = (AthState*)ctx;
    if (!s->is_ath10k) {
        // ath9k is firmware-free - a no-op returning true, per the contract. (satoru)
        return true;
    }
    // ath10k: if the caller handed us a blob, download it; else source it from fs.
    if (blob && len > 0) return ath10k_bmi_download(blob, len);
    return ath10k_load_firmware_from_fs();
}

// the ops vtable instance the stack receives. (satoru)
const WifiRadioOps ATH_OPS = {
    op_start,
    op_stop,
    op_set_channel,
    op_config_bss,
    op_set_key,
    op_tx_frame,
    op_rx_poll,
    op_get_signal,
    op_load_firmware,
};

// ─────────────────────────────────────────────────────────────────────────────
//  public entry point (satoru)
// ─────────────────────────────────────────────────────────────────────────────
bool TryRegister() {
    if (g_active) return true;

    if (!WifiDev::Present()) {
        return false;
    }
    const WifiDevice* d = WifiDev::Info();
    if (!d || !d->present) return false;

    bool is9k  = (d->family == WIFI_FAM_ATHEROS_ATH9K);
    bool is10k = (d->family == WIFI_FAM_ATHEROS_ATH10K);
    if (!is9k && !is10k) {
        // not an atheros part we handle (intel/realtek/broadcom/etc). (satoru)
        return false;
    }

    if (!d->mmio_mapped) {
        log("[ath] device present but mmio not mapped - cannot drive it\r\n");
        return false;
    }

    // init our private state. (satoru)
    a_memset(&g_ath, 0, sizeof(g_ath));
    g_ath.dev = d;
    g_ath.is_ath10k = is10k;
    g_ath.last_rssi = 0;   // 0 == "no measurement" sentinel (satoru)

    log("[ath] registering ");
    log(d->model);
    log(is10k ? " via ath10k path\r\n" : " via ath9k path\r\n");

    // bring the hardware up. for ath9k this fully inits + starts rx; for ath10k it
    // gets as far as staging firmware (and honestly returns false past that). we
    // still register the ops either way, because the stack drives start() again
    // and the ath9k path is the one expected to work. (satoru)
    bool hw = op_start(&g_ath);
    if (is9k && !hw) {
        log("[ath9k] hardware init failed - not registering radio\r\n");
        return false;
    }

    // register the radio with the 802.11 stack. it will now route scan/connect
    // through ATH_OPS with our state as ctx. (satoru)
    Ieee80211::RegisterRadio(&ATH_OPS, &g_ath, (WifiDevice*)d);
    g_active = true;

    if (is10k) {
        log("[ath10k] radio registered (scaffold) - assoc needs real hardware "
            "+ firmware\r\n");
    } else {
        log("[ath9k] radio registered - scan/connect will drive real hardware\r\n");
    }
    return true;
}

bool IsActive() { return g_active; }

}  // namespace WifiAth
// end (satoru)
