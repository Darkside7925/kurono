//  intel igc (i225/i226) + igb (82576) ethernet driver for kurono os.
//  ported from linux drivers/net/ethernet/intel/igc/{igc_base,igc_i225,igc_mac,
//  igc_phy,igc_main}.c and the igb equivalents. register offsets and bit values
//  are the ones in igc_regs.h / igc_defines.h / igb e1000_regs.h.
//
//  scope: one tx queue + one rx queue, advanced descriptors, polled. no
//  interrupts, no rss, no xdp, no ptp, no tsn, no ethtool, no eee tuning.
//  hard rule: this file only ever READS nvm-backed state (RAL0/RAH0, EECD
//  auto-read bit). nothing here issues an nvm/flash/eeprom write or update. (satoru)

#include "igc.h"
#include "serial.h"
#include "../kernel/kdf.h"
#include "../proc/scheduler.h"
#include "../shell/shell.h"

// ---- pci ids (igc_hw.h / igb e1000_hw.h) (satoru)
#define IGC_VENDOR_INTEL   0x8086
#define IGC_ID_82576       0x10C9   // qemu -device igb (satoru)

// ---- registers shared by both families (igc_regs.h) (satoru)
#define R_CTRL      0x00000
#define R_STATUS    0x00008
#define R_EECD      0x00010
#define R_MDIC      0x00020
#define R_FCAL      0x00028
#define R_FCAH      0x0002C
#define R_FCT       0x00030
#define R_VET       0x00038
#define R_RCTL      0x00100
#define R_TCTL      0x00400
#define R_PHPM      0x00E14   // i225 phy power management (satoru)
#define R_ICR       0x01500
#define R_IMC       0x0150C
#define R_MPC       0x04010
#define R_MTA       0x05200
#define R_RAL0      0x05400
#define R_RAH0      0x05404
#define R_SWSM      0x05B50
#define R_SW_FW_SYNC 0x05B5C
#define R_MANC      0x05820
#define R_UTA       0x0A000

// ---- bits (igc_defines.h) (satoru)
#define CTRL_GIO_MASTER_DISABLE 0x00000004
#define CTRL_SLU        0x00000040
#define CTRL_FRCSPD     0x00000800
#define CTRL_FRCDPX     0x00001000
#define CTRL_RST        0x04000000
#define CTRL_PHY_RST    0x80000000u
#define STATUS_FD       0x00000001
#define STATUS_LU       0x00000002
#define STATUS_SPEED_100  0x00000040
#define STATUS_SPEED_1000 0x00000080
#define STATUS_SPEED_2500 0x00400000
#define STATUS_GIO_MASTER_ENABLE 0x00080000
#define EECD_AUTO_RD    0x00000200
#define SWSM_SMBI       0x00000001
#define SWSM_SWESMBI    0x00000002
#define SWFW_PHY0_SM    0x2
#define MANC_BLK_PHY_RST_ON_IDE 0x00040000
#define PHPM_PHY_RST_COMP 0x0100
#define RAH_AV          0x80000000u
#define MDIC_DATA_MASK  0x0000FFFF
#define MDIC_REG_SHIFT  16
#define MDIC_PHY_SHIFT  21
#define MDIC_OP_WRITE   0x04000000
#define MDIC_OP_READ    0x08000000
#define MDIC_READY      0x10000000
#define MDIC_ERROR      0x40000000

#define RCTL_EN         0x00000002
#define RCTL_BAM        0x00008000
#define RCTL_SECRC      0x04000000
#define RCTL_MO_SHIFT   12
#define RCTL_LBM_MASK   0x000000C0
#define RCTL_SBP        0x00000004
#define RCTL_SZ_256     0x00030000

#define TCTL_EN         0x00000002
#define TCTL_PSP        0x00000008
#define TCTL_CT_MASK    0x00000ff0
#define TCTL_COLD_MASK  0x003ff000
#define TCTL_RTLC       0x01000000
#define COLLISION_THRESHOLD 15
#define CT_SHIFT        4
#define COLLISION_DISTANCE 63
#define COLD_SHIFT      12

// ---- mii / phy (igc_defines.h) (satoru)
#define PHY_CONTROL     0x00
#define PHY_STATUS      0x01
#define PHY_ID1         0x02
#define PHY_ID2         0x03
#define PHY_AUTONEG_ADV 0x04
#define PHY_1000T_CTRL  0x09
#define MII_CR_RESTART_AUTO_NEG 0x0200
#define MII_CR_POWER_DOWN       0x0800
#define MII_CR_AUTO_NEG_EN      0x1000
#define MII_SR_LINK_STATUS      0x0004
#define NWAY_AR_10T_HD_CAPS     0x0020
#define NWAY_AR_10T_FD_CAPS     0x0040
#define NWAY_AR_100TX_HD_CAPS   0x0080
#define NWAY_AR_100TX_FD_CAPS   0x0100
#define NWAY_AR_PAUSE           0x0400
#define NWAY_AR_ASM_DIR         0x0800
#define CR_1000T_HD_CAPS        0x0100
#define CR_1000T_FD_CAPS        0x0200
#define CR_2500T_FD_CAPS        0x0080
#define MMDAC_REG       13
#define MMDAAD_REG      14
#define MMDAC_FUNC_DATA 0x4000
#define MMD_AN_DEV      7
#define ANEG_MULTIGBT_AN_CTRL 0x0020

// ---- queue register blocks. igc: 0xC000/0xE000 + n*0x40; igb (queue 0 < 4):
// 0x2800/0x3800 + n*0x100 (igb e1000_regs.h). (satoru)
struct QRegs {
    uint32_t rdbal, rdbah, rdlen, rdh, rdt, srrctl, rxdctl;
    uint32_t tdbal, tdbah, tdlen, tdh, tdt, txdctl;
};
static const QRegs QR_IGC = { 0xC000, 0xC004, 0xC008, 0xC010, 0xC018, 0xC00C, 0xC028,
                              0xE000, 0xE004, 0xE008, 0xE010, 0xE018, 0xE028 };
static const QRegs QR_IGB = { 0x2800, 0x2804, 0x2808, 0x2810, 0x2818, 0x280C, 0x2828,
                              0x3800, 0x3804, 0x3808, 0x3810, 0x3818, 0x3828 };

#define QUEUE_ENABLE    0x02000000u
#define SRRCTL_DESCTYPE_ADV_ONEBUF (1u << 25)
#define RX_HDR_LEN      256

// ---- advanced descriptors (igc_base.h / igc_defines.h) (satoru)
#define ADVTXD_DTYP_DATA 0x00300000u
#define ADVTXD_DCMD_EOP  0x01000000u
#define ADVTXD_DCMD_IFCS 0x02000000u
#define ADVTXD_DCMD_RS   0x08000000u
#define ADVTXD_DCMD_DEXT 0x20000000u
#define ADVTXD_PAYLEN_SHIFT 14
#define TXD_STAT_DD      0x1u
#define RXD_STAT_DD      0x1u
#define RXD_STAT_EOP     0x2u

#define NUM_RX_DESC   256
#define NUM_TX_DESC   64
#define BUF_SIZE      2048
#define MAX_TX_FRAME  1518

// one 16-byte advanced descriptor, used for both directions. rx read format:
// q0=pkt addr, d2/d3 = hdr addr. rx write-back: d2=status_error, d3=length(15:0).
// tx data: q0=buffer, d2=cmd_type_len, d3=olinfo_status (write-back sets DD in d3). (satoru)
struct AdvDesc {
    uint64_t q0;
    uint32_t d2;
    uint32_t d3;
} __attribute__((packed));

// ---- driver state (all statics, nothing on the heap) (satoru)
static volatile uint8_t* g_bar = nullptr;
static bool     g_is_igc = true;          // false = igb (82576) (satoru)
static const QRegs* g_q = &QR_IGC;
static uint8_t  g_bus, g_dev, g_fn;
static uint16_t g_devid;
static uint64_t g_bar_phys;
static uint8_t  g_mac[6];
static bool     g_found = false;          // pci device located (satoru)
static bool     g_detected = false;       // bring-up succeeded, rings live (satoru)
static volatile bool g_running = false;   // gates Send/Poll during restart (satoru)
static int      g_kdf_id = -1;
static int      g_phy_addr = 0;
static bool     g_phy_ok = false;
static bool     g_link_prev = false;
static bool     g_warned_nolink = false;
static uint64_t g_init_ms = 0;

static AdvDesc* g_rx = nullptr;
static AdvDesc* g_tx = nullptr;
static uint8_t* g_rxbuf = nullptr;
static uint8_t* g_txbuf = nullptr;
static uint64_t g_rx_phys = 0, g_tx_phys = 0, g_rxbuf_phys = 0, g_txbuf_phys = 0;
static uint16_t g_rx_cur = 0;
static uint16_t g_tx_use = 0, g_tx_clean = 0;

static Igc_PacketHandler g_handler = nullptr;
static uint32_t g_tx_count, g_rx_count, g_tx_bytes, g_rx_bytes, g_rx_missed;
static uint32_t g_tx_drop = 0;
static volatile uint32_t g_tx_lock = 0, g_rx_lock = 0;

// ---- low level helpers (satoru)
static inline void outl(uint16_t port, uint32_t v) { __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t r; __asm__ __volatile__("inl %1, %0" : "=a"(r) : "Nd"(port)); return r; }

static uint32_t PciRd(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    outl(0xCF8, (1u << 31) | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void PciWr(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    outl(0xCF8, (1u << 31) | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static inline uint32_t Rd(uint32_t off) { return *(volatile uint32_t*)(g_bar + off); }
static inline void Wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_bar + off) = v; }
static inline void Flush() { (void)Rd(R_STATUS); }   // wrfl() (satoru)

// clock-independent microsecond delay: an isa port 0x80 write is ~1us on real
// hardware and does not depend on the scheduler tick (which may not run yet). (satoru)
static void DelayUs(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) __asm__ __volatile__("outb %%al, $0x80" : : "a"(0));
}
static void DelayMs(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) DelayUs(1000); }

static bool Expired(uint64_t t0, uint32_t ms) { return (Scheduler::NowMs() - t0) > ms; }

static void LogHexPad(uint32_t v, int digits) {
    char b[9]; const char* h = "0123456789ABCDEF";
    for (int i = 0; i < digits; i++) b[i] = h[(v >> ((digits - 1 - i) * 4)) & 0xF];
    b[digits] = 0;
    SerialLogger::Log(b);
}
static void LogMac(const uint8_t* m) {
    for (int i = 0; i < 6; i++) { LogHexPad(m[i], 2); if (i < 5) SerialLogger::Log(":"); }
}

static inline void LockAcq(volatile uint32_t* l) {
    while (__atomic_exchange_n(l, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("pause");
    }
}
static inline bool LockTry(volatile uint32_t* l) { return __atomic_exchange_n(l, 1u, __ATOMIC_ACQUIRE) == 0; }
static inline void LockRel(volatile uint32_t* l) { __atomic_store_n(l, 0u, __ATOMIC_RELEASE); }

// ---- sw/fw semaphore (igc_acquire_swfw_sync_i225 / igc_get_hw_semaphore_i225).
// linux waits up to ~1s (200 x mdelay(5)); the caps here are lower so a wedged
// firmware cannot stall boot, and every loop also has an iteration cap. (satoru)
static bool g_clear_sem_once = true;

static void PutHwSemaphore() {   // igc_put_hw_semaphore (satoru)
    uint32_t s = Rd(R_SWSM);
    s &= ~(SWSM_SMBI | SWSM_SWESMBI);
    Wr(R_SWSM, s);
}

static bool GetHwSemaphore() {
    const int timeout = 200;
    int i;
    uint64_t t0 = Scheduler::NowMs();
    for (i = 0; i < timeout; i++) {
        if (!(Rd(R_SWSM) & SWSM_SMBI)) break;
        DelayUs(100);
        if (Expired(t0, 100)) { i = timeout; break; }
    }
    if (i == timeout && g_clear_sem_once) {
        // rare: semaphore already held unintentionally; clear once and retry (satoru)
        g_clear_sem_once = false;
        PutHwSemaphore();
        for (i = 0; i < timeout; i++) {
            if (!(Rd(R_SWSM) & SWSM_SMBI)) break;
            DelayUs(100);
        }
    }
    if (i == timeout) return false;
    for (i = 0; i < timeout; i++) {
        uint32_t s = Rd(R_SWSM);
        Wr(R_SWSM, s | SWSM_SWESMBI);
        if (Rd(R_SWSM) & SWSM_SWESMBI) break;
        DelayUs(100);
    }
    if (i == timeout) { PutHwSemaphore(); return false; }
    return true;
}

static bool AcquireSwFw(uint32_t mask) {
    uint32_t fwmask = mask << 16, swmask = mask;
    uint64_t t0 = Scheduler::NowMs();
    int i;
    for (i = 0; i < 40; i++) {
        if (!GetHwSemaphore()) return false;
        uint32_t sync = Rd(R_SW_FW_SYNC);
        if (!(sync & (fwmask | swmask))) {
            Wr(R_SW_FW_SYNC, sync | swmask);
            PutHwSemaphore();
            return true;
        }
        PutHwSemaphore();
        DelayMs(5);
        if (Expired(t0, 400)) break;
    }
    return false;
}

static void ReleaseSwFw(uint32_t mask) {
    if (!GetHwSemaphore()) return;
    Wr(R_SW_FW_SYNC, Rd(R_SW_FW_SYNC) & ~mask);
    PutHwSemaphore();
}

// ---- mdic phy access (igc_read_phy_reg_mdic / igc_write_phy_reg_mdic, igc_phy.c).
// poll count 1920 x 50us like IGC_GEN_POLL_TIMEOUT. caller holds the swfw lock. (satoru)
static bool MdicOp(bool write, int addr, uint32_t reg, uint16_t* val) {
    uint32_t mdic = (reg << MDIC_REG_SHIFT) | ((uint32_t)addr << MDIC_PHY_SHIFT) |
                    (write ? (MDIC_OP_WRITE | *val) : MDIC_OP_READ);
    Wr(R_MDIC, mdic);
    uint64_t t0 = Scheduler::NowMs();
    for (int i = 0; i < 1920; i++) {
        DelayUs(50);
        mdic = Rd(R_MDIC);
        if (mdic & MDIC_READY) break;
        if (Expired(t0, 150)) return false;
    }
    if (!(mdic & MDIC_READY)) return false;
    if (mdic & MDIC_ERROR) return false;
    if (!write) *val = (uint16_t)(mdic & MDIC_DATA_MASK);
    return true;
}

// igc_read_phy_reg_gpy: upper 16 bits of offset = mmd device (0 = clause 22),
// otherwise go through the MMDAC/MMDAAD indirection (__igc_access_xmdio_reg). (satoru)
static bool PhyAccess(bool write, uint32_t offset, uint16_t* val) {
    if (!g_phy_ok) return false;
    uint32_t dev = (offset >> 16) & 0xFFFF;
    uint32_t reg = offset & 0xFFFF;
    if (!AcquireSwFw(SWFW_PHY0_SM)) return false;
    bool ok;
    if (!dev) {
        ok = MdicOp(write, g_phy_addr, reg, val);
    } else {
        uint16_t t;
        ok = (t = (uint16_t)dev, MdicOp(true, g_phy_addr, MMDAC_REG, &t));
        if (ok) { t = (uint16_t)reg; ok = MdicOp(true, g_phy_addr, MMDAAD_REG, &t); }
        if (ok) { t = (uint16_t)(MMDAC_FUNC_DATA | dev); ok = MdicOp(true, g_phy_addr, MMDAC_REG, &t); }
        if (ok) ok = MdicOp(write, g_phy_addr, MMDAAD_REG, val);
        if (ok) { t = 0; ok = MdicOp(true, g_phy_addr, MMDAC_REG, &t); }
    }
    ReleaseSwFw(SWFW_PHY0_SM);
    return ok;
}
static bool PhyRd(uint32_t off, uint16_t* v) { return PhyAccess(false, off, v); }
static bool PhyWr(uint32_t off, uint16_t v) { return PhyAccess(true, off, &v); }

// ---- reset (igc_reset_hw_base / igb_reset_hw_82575) (satoru)
static bool ResetHw() {
    // igc_disable_pcie_master: stop bus-master requests so the reset cannot
    // leave a half finished tlp on the link. (satoru)
    Wr(R_CTRL, Rd(R_CTRL) | CTRL_GIO_MASTER_DISABLE);
    bool master_ok = false;
    uint64_t t0 = Scheduler::NowMs();
    for (int i = 0; i < 800; i++) {
        if (!(Rd(R_STATUS) & STATUS_GIO_MASTER_ENABLE)) { master_ok = true; break; }
        DelayUs(100);
        if (Expired(t0, 100)) break;
    }
    if (!master_ok) SerialLogger::Log("[igc] reset: pci-e master disable timed out (continuing)\r\n");

    Wr(R_IMC, 0xFFFFFFFFu);
    Wr(R_RCTL, 0);
    Wr(R_TCTL, TCTL_PSP);
    Flush();
    DelayMs(10);

    Wr(R_CTRL, Rd(R_CTRL) | CTRL_RST);   // global reset (satoru)
    Flush();
    DelayMs(2);

    // igc_get_auto_rd_done: wait for the hardware to finish its own nvm
    // autoload (a READ of nvm contents into mac regs). timeout is not fatal. (satoru)
    bool auto_ok = false;
    t0 = Scheduler::NowMs();
    for (int i = 0; i < 200; i++) {
        if (Rd(R_EECD) & EECD_AUTO_RD) { auto_ok = true; break; }
        DelayUs(500);
        if (Expired(t0, 100)) break;
    }
    Wr(R_IMC, 0xFFFFFFFFu);
    (void)Rd(R_ICR);
    SerialLogger::Log(auto_ok ? "[igc] reset: global reset done, nvm autoload done\r\n"
                              : "[igc] reset: global reset done, nvm autoload did NOT report done (continuing)\r\n");
    return true;
}

// ---- phy: reset + autoneg (igc_phy_hw_reset, igc_phy_setup_autoneg,
// igc_copper_link_autoneg, igc_power_up_phy_copper) (satoru)
static bool PhyProbeAddr() {
    // igc keeps phy->addr 0; igb probes from 1 (igb_get_phy_id_82575). try the
    // family default first, then the rest, accepting the first sane id. (satoru)
    static const int order_igc[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const int order_igb[] = { 1, 0, 2, 3, 4, 5, 6, 7 };
    const int* order = g_is_igc ? order_igc : order_igb;
    g_phy_ok = true;   // allow PhyAccess during probing (satoru)
    if (!AcquireSwFw(SWFW_PHY0_SM)) { g_phy_ok = false; return false; }
    for (int k = 0; k < 8; k++) {
        uint16_t id1 = 0;
        if (!MdicOp(false, order[k], PHY_ID1, &id1)) continue;
        if (id1 == 0xFFFF || id1 == 0) continue;
        g_phy_addr = order[k];
        ReleaseSwFw(SWFW_PHY0_SM);
        return true;
    }
    ReleaseSwFw(SWFW_PHY0_SM);
    g_phy_ok = false;
    return false;
}

static void PhyHwReset() {
    // MANC.BLK_PHY_RST_ON_IDE = firmware forbids phy resets (igc_check_reset_block) (satoru)
    if (Rd(R_MANC) & MANC_BLK_PHY_RST_ON_IDE) {
        SerialLogger::Log("[igc] phy: reset blocked by firmware (MANC), skipping\r\n");
        return;
    }
    if (!AcquireSwFw(SWFW_PHY0_SM)) {
        SerialLogger::Log("[igc] phy: could not take PHY0 sw/fw semaphore, skipping phy reset\r\n");
        return;
    }
    uint32_t ctrl = Rd(R_CTRL);
    Wr(R_CTRL, ctrl | CTRL_PHY_RST);
    Flush();
    DelayUs(100);                       // phy->reset_delay_us = 100 (satoru)
    Wr(R_CTRL, ctrl);
    Flush();
    DelayUs(150);
    if (g_is_igc) {
        // wait for PHPM.PHY_RST_COMP, bounded (satoru)
        bool done = false;
        uint64_t t0 = Scheduler::NowMs();
        for (int i = 0; i < 10000; i++) {
            if (Rd(R_PHPM) & PHPM_PHY_RST_COMP) { done = true; break; }
            DelayUs(1);
            if (Expired(t0, 100)) break;
        }
        if (!done) SerialLogger::Log("[igc] phy: reset-complete bit never set (continuing)\r\n");
    } else {
        DelayMs(10);
    }
    DelayUs(150);
    ReleaseSwFw(SWFW_PHY0_SM);
}

static bool PhySetupAutoneg() {
    // advertise everything the part supports: 10/100 hd+fd, 1000 fd and (igc
    // only) 2500 fd. flow control is advertised off (see notes in the report). (satoru)
    uint16_t adv = 0, gt = 0, mg = 0;
    if (!PhyRd(PHY_AUTONEG_ADV, &adv)) return false;
    if (!PhyRd(PHY_1000T_CTRL, &gt)) return false;
    adv &= ~(NWAY_AR_100TX_FD_CAPS | NWAY_AR_100TX_HD_CAPS | NWAY_AR_10T_FD_CAPS | NWAY_AR_10T_HD_CAPS);
    gt &= ~(CR_1000T_HD_CAPS | CR_1000T_FD_CAPS);
    adv |= NWAY_AR_10T_HD_CAPS | NWAY_AR_10T_FD_CAPS | NWAY_AR_100TX_HD_CAPS | NWAY_AR_100TX_FD_CAPS;
    gt |= CR_1000T_FD_CAPS;
    adv &= ~(NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);   // igc_fc_none (satoru)
    if (!PhyWr(PHY_AUTONEG_ADV, adv)) return false;
    if (!PhyWr(PHY_1000T_CTRL, gt)) return false;
    if (g_is_igc) {
        uint32_t mgreg = ((uint32_t)MMD_AN_DEV << 16) | ANEG_MULTIGBT_AN_CTRL;
        if (!PhyRd(mgreg, &mg)) return false;
        mg |= CR_2500T_FD_CAPS;
        if (!PhyWr(mgreg, mg)) return false;
    }
    // restart autonegotiation; do not wait for it (autoneg_wait_to_complete=false) (satoru)
    uint16_t pc = 0;
    if (!PhyRd(PHY_CONTROL, &pc)) return false;
    pc &= ~MII_CR_POWER_DOWN;                       // igc_power_up_phy_copper (satoru)
    pc |= MII_CR_AUTO_NEG_EN | MII_CR_RESTART_AUTO_NEG;
    return PhyWr(PHY_CONTROL, pc);
}

// ---- link status (STATUS register, igc_check_for_copper_link's mac view) (satoru)
static const char* SpeedStr(uint32_t st) {
    if (st & STATUS_SPEED_2500) return "2500 Mbps";
    uint32_t sp = (st >> 6) & 3;
    if (sp == 0) return "10 Mbps";
    if (sp == 1) return "100 Mbps";
    return "1000 Mbps";
}

static void ConfigCollisionDist() {   // igc_config_collision_dist (satoru)
    uint32_t t = Rd(R_TCTL);
    t &= ~TCTL_COLD_MASK;
    t |= (uint32_t)COLLISION_DISTANCE << COLD_SHIFT;
    Wr(R_TCTL, t);
    Flush();
}

// ---- receive address (igc_rar_set / igc_init_rx_addrs) (satoru)
static void RarSet(const uint8_t* a, uint32_t idx) {
    uint32_t lo = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
    uint32_t hi = (uint32_t)a[4] | ((uint32_t)a[5] << 8);
    if (lo || hi) hi |= RAH_AV;
    Wr(R_RAL0 + idx * 8, lo);
    Flush();
    Wr(R_RAH0 + idx * 8, hi);
    Flush();
}

static void ReadMacFromRar() {
    // linux reads the permanent address back from RAL0/RAH0 after reset (the
    // hardware autoload put the nvm mac there). read only. (satoru)
    uint32_t lo = Rd(R_RAL0), hi = Rd(R_RAH0);
    g_mac[0] = (uint8_t)lo; g_mac[1] = (uint8_t)(lo >> 8);
    g_mac[2] = (uint8_t)(lo >> 16); g_mac[3] = (uint8_t)(lo >> 24);
    g_mac[4] = (uint8_t)hi; g_mac[5] = (uint8_t)(hi >> 8);
    bool zero = !(lo | (hi & 0xFFFF));
    bool bad = zero || (g_mac[0] & 1) || (lo == 0xFFFFFFFFu);
    if (bad) {
        // no usable mac in the nic: make a locally administered one so the
        // stack still has an address. nothing is written to nvm. (satoru)
        uint32_t a, d;
        __asm__ __volatile__("rdtsc" : "=a"(a), "=d"(d));
        g_mac[0] = 0x02; g_mac[1] = 0x4B; g_mac[2] = (uint8_t)a; g_mac[3] = (uint8_t)(a >> 8);
        g_mac[4] = (uint8_t)(a >> 16); g_mac[5] = (uint8_t)(a ^ d);
        SerialLogger::Log("[igc] WARNING: RAL0/RAH0 held no valid MAC; using a generated locally-administered one\r\n");
    }
}

// ---- rings (igc_configure_tx_ring / igc_configure_rx_ring) (satoru)
static void ProgramRings() {
    // zero both rings, fill rx with buffers (satoru)
    for (int i = 0; i < NUM_RX_DESC; i++) {
        g_rx[i].q0 = g_rxbuf_phys + (uint64_t)i * BUF_SIZE;
        g_rx[i].d2 = 0;
        g_rx[i].d3 = 0;
    }
    for (int i = 0; i < NUM_TX_DESC; i++) {
        g_tx[i].q0 = g_txbuf_phys + (uint64_t)i * BUF_SIZE;
        g_tx[i].d2 = 0;
        g_tx[i].d3 = 0;
    }
    g_rx_cur = 0; g_tx_use = 0; g_tx_clean = 0;
    __asm__ __volatile__("mfence" ::: "memory");

    // tx: igc_setup_tctl then igc_configure_tx_ring. full 64-bit base. (satoru)
    Wr(g_q->txdctl, 0);
    Flush();
    Wr(g_q->tdlen, NUM_TX_DESC * sizeof(AdvDesc));
    Wr(g_q->tdbal, (uint32_t)(g_tx_phys & 0xFFFFFFFFu));
    Wr(g_q->tdbah, (uint32_t)(g_tx_phys >> 32));
    Wr(g_q->tdh, 0);
    Wr(g_q->tdt, 0);
    // linux uses WTHRESH=16 (batched write-back); one descriptor per packet here
    // and we poll for DD, so write back every descriptor (WTHRESH=1). (satoru)
    Wr(g_q->txdctl, 8u | (1u << 8) | (1u << 16) | QUEUE_ENABLE);

    uint32_t tctl = Rd(R_TCTL);
    tctl &= ~TCTL_CT_MASK;
    tctl |= TCTL_PSP | TCTL_RTLC | ((uint32_t)COLLISION_THRESHOLD << CT_SHIFT);
    tctl |= TCTL_EN;   // enable transmits (satoru)
    Wr(R_TCTL, tctl);

    // rx: igc_configure_rx_ring (queue disabled while programming) (satoru)
    Wr(g_q->rxdctl, 0);
    Wr(g_q->rdbal, (uint32_t)(g_rx_phys & 0xFFFFFFFFu));
    Wr(g_q->rdbah, (uint32_t)(g_rx_phys >> 32));
    Wr(g_q->rdlen, NUM_RX_DESC * sizeof(AdvDesc));
    Wr(g_q->rdh, 0);
    Wr(g_q->rdt, 0);
    uint32_t srr = Rd(g_q->srrctl);
    srr &= ~(0x7Fu | (0x3Fu << 8) | (7u << 25));
    srr |= ((uint32_t)(RX_HDR_LEN / 64) << 8) | (BUF_SIZE / 1024) | SRRCTL_DESCTYPE_ADV_ONEBUF;
    Wr(g_q->srrctl, srr);
    Wr(g_q->rxdctl, 8u | (8u << 8) | (4u << 16) | QUEUE_ENABLE);

    // wait for the queues to latch their enable bits (bounded) (satoru)
    uint64_t t0 = Scheduler::NowMs();
    for (int i = 0; i < 2000; i++) {
        if ((Rd(g_q->rxdctl) & QUEUE_ENABLE) && (Rd(g_q->txdctl) & QUEUE_ENABLE)) break;
        DelayUs(10);
        if (Expired(t0, 20)) break;
    }

    // igc_setup_rctl: enable, broadcast accept, strip crc. no lpe (frames are
    // limited to the 2 kb buffer), no promiscuous - filtering is via RAR0. (satoru)
    uint32_t rctl = Rd(R_RCTL);
    rctl &= ~((3u << RCTL_MO_SHIFT) | RCTL_LBM_MASK | RCTL_SBP | RCTL_SZ_256);
    rctl |= RCTL_EN | RCTL_BAM | RCTL_SECRC;
    Wr(R_RCTL, rctl);
    // hand every descriptor except one to the hardware (tail = last owned) (satoru)
    Wr(g_q->rdt, NUM_RX_DESC - 1);
}

// ---- the whole bring-up, run under kdf (igc_probe + igc_reset + igc_open) (satoru)
static bool BringUp() {
    g_running = false;
    g_detected = false;
    g_phy_ok = false;
    g_clear_sem_once = true;
    g_init_ms = Scheduler::NowMs();

    // pci: power state D0, memory space + bus master (pci_enable_device + pci_set_master) (satoru)
    uint32_t cmd = PciRd(g_bus, g_dev, g_fn, 0x04);
    PciWr(g_bus, g_dev, g_fn, 0x04, (cmd & 0xFFFF) | 0x6);
    if (PciRd(g_bus, g_dev, g_fn, 0x04) & (1u << 20)) {   // capability list present (satoru)
        uint8_t cp = (uint8_t)(PciRd(g_bus, g_dev, g_fn, 0x34) & 0xFC);
        for (int guard = 0; cp >= 0x40 && guard < 48; guard++) {
            uint32_t hd = PciRd(g_bus, g_dev, g_fn, cp);
            if ((hd & 0xFF) == 0x01) {
                uint32_t pm = PciRd(g_bus, g_dev, g_fn, cp + 4);
                if (pm & 3) {
                    SerialLogger::Log("[igc] device not in D0, forcing D0\r\n");
                    PciWr(g_bus, g_dev, g_fn, cp + 4, pm & ~3u);
                    DelayMs(10);
                }
                break;
            }
            cp = (uint8_t)((hd >> 8) & 0xFC);
        }
    }
    SerialLogger::Log("[igc] step: pci memory space + bus master enabled\r\n");

    // step: reset
    ResetHw();
    uint32_t st0 = Rd(R_STATUS);
    if (st0 == 0xFFFFFFFFu) {
        SerialLogger::Log("[igc] FAIL: STATUS reads all-ones after reset (device gone or bar wrong)\r\n");
        return false;
    }

    ReadMacFromRar();
    SerialLogger::Log("[igc] MAC "); LogMac(g_mac); SerialLogger::Log("\r\n");

    // igc_init_hw_base: rar0 = mac, rest cleared, mta + uta zeroed (satoru)
    RarSet(g_mac, 0);
    for (uint32_t i = 1; i < 16; i++) { Wr(R_RAL0 + i * 8, 0); Wr(R_RAH0 + i * 8, 0); }
    for (int i = 0; i < 128; i++) { Wr(R_MTA + i * 4, 0); Wr(R_UTA + i * 4, 0); }
    SerialLogger::Log("[igc] step: receive address + mta/uta programmed\r\n");

    // igc_setup_link -> igc_setup_copper_link_base: set link up, no forced
    // speed/duplex, then phy autoneg. (satoru)
    uint32_t ctrl = Rd(R_CTRL);
    ctrl |= CTRL_SLU;
    ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPX);
    Wr(R_CTRL, ctrl);
    Wr(R_FCT, 0x8808);          // FLOW_CONTROL_TYPE (satoru)
    Wr(R_FCAH, 0x00000100);     // FLOW_CONTROL_ADDRESS_HIGH (satoru)
    Wr(R_FCAL, 0x00C28001);     // FLOW_CONTROL_ADDRESS_LOW (satoru)

    if (PhyProbeAddr()) {
        uint16_t i1 = 0, i2 = 0;
        PhyRd(PHY_ID1, &i1); PhyRd(PHY_ID2, &i2);
        SerialLogger::Log("[igc] phy addr "); SerialLogger::LogDec(g_phy_addr);
        SerialLogger::Log(" id "); LogHexPad(i1, 4); LogHexPad(i2, 4); SerialLogger::Log("\r\n");
        PhyHwReset();
        // the reset may have changed what the id registers show; re-probe the address (satoru)
        PhyProbeAddr();
        if (g_phy_ok && PhySetupAutoneg())
            SerialLogger::Log("[igc] step: phy reset + autoneg restarted (adv 10/100/1000"
                              "/2500 as supported)\r\n");
        else
            SerialLogger::Log("[igc] step: phy autoneg setup FAILED (relying on nvm defaults)\r\n");
        // igc_phy_has_link(COPPER_LINK_UP_LIMIT=10, 10us): short check, link usually comes later (satoru)
        uint16_t ps = 0;
        for (int i = 0; g_phy_ok && i < 10; i++) {
            PhyRd(PHY_STATUS, &ps); PhyRd(PHY_STATUS, &ps);
            if (ps & MII_SR_LINK_STATUS) break;
            DelayUs(10);
        }
    } else {
        SerialLogger::Log("[igc] step: no phy answered on mdic (continuing, mac-only link)\r\n");
    }

    Wr(R_VET, 0x8100);   // igc_reset: ETH_P_8021Q (satoru)
    ProgramRings();
    SerialLogger::Log("[igc] step: tx/rx rings programmed (rx ring ");
    SerialLogger::LogHex64(g_rx_phys);
    SerialLogger::Log(" tx ring "); SerialLogger::LogHex64(g_tx_phys);
    SerialLogger::Log(")\r\n");

    // interrupts stay masked, we poll (igc_reset_hw_base already did IMC) (satoru)
    Wr(R_IMC, 0xFFFFFFFFu);
    (void)Rd(R_ICR);

    uint32_t st = Rd(R_STATUS);
    g_link_prev = (st & STATUS_LU) != 0;
    if (g_link_prev) ConfigCollisionDist();
    SerialLogger::Log("[igc] link ");
    SerialLogger::Log(g_link_prev ? "UP " : "DOWN (autoneg in progress, will be re-checked) ");
    if (g_link_prev) { SerialLogger::Log(SpeedStr(st)); SerialLogger::Log((st & STATUS_FD) ? " full duplex" : " half duplex"); }
    SerialLogger::Log("\r\n");

    g_warned_nolink = false;
    g_detected = true;
    g_running = true;
    return true;
}

// ---- kdf glue (satoru)
bool Igc::KdfInit() {
    if (!g_found) return false;
    if (!g_bar) {
        void* win = KDF::MapMMIO(g_bar_phys, 0x10000ULL);
        if (!win) { SerialLogger::Log("[igc] FAIL: KDF::MapMMIO(bar) failed\r\n"); return false; }
        g_bar = (volatile uint8_t*)win;
    }
    if (!g_rx) {
        uint8_t* rings = (uint8_t*)KDF::AllocDMA(8192);
        uint8_t* rxb = (uint8_t*)KDF::AllocDMA((uint64_t)NUM_RX_DESC * BUF_SIZE);
        uint8_t* txb = (uint8_t*)KDF::AllocDMA((uint64_t)NUM_TX_DESC * BUF_SIZE);
        if (!rings || !rxb || !txb) { SerialLogger::Log("[igc] FAIL: KDF::AllocDMA failed\r\n"); return false; }
        g_rx = (AdvDesc*)rings;
        g_tx = (AdvDesc*)(rings + 4096);
        g_rxbuf = rxb; g_txbuf = txb;
        g_rx_phys = KDF::PhysOf(rings);
        g_tx_phys = g_rx_phys + 4096;
        g_rxbuf_phys = KDF::PhysOf(rxb);
        g_txbuf_phys = KDF::PhysOf(txb);
    }
    return BringUp();
}

static bool IsIgcId(uint16_t d) {
    switch (d) {
    case 0x125C: case 0x125B: case 0x125D: case 0x125E: case 0x3102: case 0x5503:   // i226 (satoru)
    case 0x15F3: case 0x15F2: case 0x15F8: case 0x15F7: case 0x3100: case 0x3101:   // i225/i220 (satoru)
    case 0x5502: case 0x0D9F:
        return true;
    default: return false;
    }
}

static bool FindDevice() {
    // all 256 buses (the real laptop's nic sits on bus 0x81), functions 0-7 when
    // the header says multifunction. (satoru)
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = PciRd((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t ht = PciRd((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            uint32_t fn_limit = ((ht >> 16) & 0x80) ? 8 : 1;   // multifunction? (satoru)
            for (uint32_t fn = 0; fn < fn_limit; fn++) {
                uint32_t id = PciRd((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint16_t ven = id & 0xFFFF, devid = (uint16_t)(id >> 16);
                bool is_igc = (ven == IGC_VENDOR_INTEL) && IsIgcId(devid);
                bool is_igb = (ven == IGC_VENDOR_INTEL) && devid == IGC_ID_82576;
                if (is_igc || is_igb) {
                    uint32_t b0 = PciRd((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x10);
                    if (b0 & 1) continue;   // io bar, not what we want (satoru)
                    uint64_t base = (uint64_t)(b0 & ~0xFu);
                    if (((b0 >> 1) & 3) == 2)   // 64-bit bar: high half in bar1 (satoru)
                        base |= (uint64_t)PciRd((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x14) << 32;
                    if (!base) continue;
                    g_bus = (uint8_t)bus; g_dev = (uint8_t)dev; g_fn = (uint8_t)fn;
                    g_devid = devid; g_bar_phys = base;
                    g_is_igc = is_igc;
                    g_q = is_igc ? &QR_IGC : &QR_IGB;
                    SerialLogger::Log("[igc] found ");
                    SerialLogger::Log(is_igc ? "igc (i225/i226)" : "igb (82576)");
                    SerialLogger::Log(" 8086:"); LogHexPad(devid, 4);
                    SerialLogger::Log(" at pci ");
                    LogHexPad(bus, 2); SerialLogger::Log(":"); LogHexPad(dev, 2);
                    SerialLogger::Log("."); SerialLogger::LogDec((int)fn);
                    SerialLogger::Log(" BAR0 "); SerialLogger::LogHex64(base);
                    SerialLogger::Log("\r\n");
                    return true;
                }
            }
        }
    }
    return false;
}

bool Igc::Init() {
    g_found = false; g_detected = false; g_running = false;
    g_tx_count = g_rx_count = g_tx_bytes = g_rx_bytes = g_rx_missed = 0;
    g_tx_drop = 0;
    SerialLogger::Log("[igc] scanning pci (all 256 buses) for i225/i226/82576\r\n");
    if (!FindDevice()) {
        SerialLogger::Log("[igc] no igc/igb nic found\r\n");
        return false;
    }
    g_found = true;
    if (g_kdf_id < 0) g_kdf_id = KDF::RegisterDriver("igc", &Igc::KdfInit);
    if (g_kdf_id < 0) { SerialLogger::Log("[igc] FAIL: KDF::RegisterDriver failed\r\n"); return false; }
    bool ok = KDF::Start(g_kdf_id);
    if (!ok || !g_detected) {
        SerialLogger::Log("[igc] FAIL: bring-up did not complete\r\n");
        return false;
    }
    SerialLogger::Log("[igc] driver initialized successfully\r\n");
    return true;
}

bool Igc::Restart() {
    if (!g_found || g_kdf_id < 0) return false;
    SerialLogger::Log("[igc] restart: quiescing rings\r\n");
    g_running = false;
    // wait out any sender/poller already inside the data path (bounded) (satoru)
    for (int i = 0; i < 2000000; i++) {
        if (LockTry(&g_tx_lock)) {
            if (LockTry(&g_rx_lock)) {
                // rings off + mac reset happen inside BringUp via KDF::Start (satoru)
                bool ok = KDF::Start(g_kdf_id);
                LockRel(&g_rx_lock);
                LockRel(&g_tx_lock);
                SerialLogger::Log(ok && g_detected ? "[igc] restart: done\r\n" : "[igc] restart: FAILED\r\n");
                return ok && g_detected;
            }
            LockRel(&g_tx_lock);
        }
        __asm__ __volatile__("pause");
    }
    SerialLogger::Log("[igc] restart: data path stayed busy, aborted\r\n");
    g_running = g_detected;
    return false;
}

bool Igc::IsDetected() { return g_found && g_detected; }

bool Igc::IsLinkUp() {
    if (!g_detected || !g_running) return false;
    return (Rd(R_STATUS) & STATUS_LU) != 0;
}

void Igc::GetMAC(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = g_mac[i]; }

// reclaim finished tx descriptors: dd is written back into olinfo_status (d3) (satoru)
static void ReclaimTx() {
    while (g_tx_clean != g_tx_use) {
        volatile AdvDesc* d = &g_tx[g_tx_clean];
        if (!(d->d3 & TXD_STAT_DD)) break;
        g_tx_clean = (uint16_t)((g_tx_clean + 1) % NUM_TX_DESC);
    }
}

bool Igc::Send(const uint8_t* data, uint16_t length) {
    if (!g_detected || !g_running || !data || length == 0 || length > MAX_TX_FRAME) return false;
    LockAcq(&g_tx_lock);
    if (!g_running) { LockRel(&g_tx_lock); return false; }
    ReclaimTx();
    uint16_t next = (uint16_t)((g_tx_use + 1) % NUM_TX_DESC);
    if (next == g_tx_clean) {
        // full: give the nic a bounded chance to drain (satoru)
        uint64_t t0 = Scheduler::NowMs();
        for (int i = 0; i < 100000 && next == g_tx_clean; i++) {
            __asm__ __volatile__("pause");
            ReclaimTx();
            if ((i & 1023) == 1023 && Expired(t0, 20)) break;
        }
        if (next == g_tx_clean) {
            g_tx_drop++;
            if (g_tx_drop <= 3 || (g_tx_drop & 0xFF) == 0) {
                SerialLogger::Log("[igc] tx ring full, descriptor never drained: tx=");
                SerialLogger::LogDec((int)g_tx_count); SerialLogger::Log(" rx=");
                SerialLogger::LogDec((int)g_rx_count); SerialLogger::Log(" status=0x");
                SerialLogger::LogHex(Rd(R_STATUS)); SerialLogger::Log("\r\n");
            }
            LockRel(&g_tx_lock);
            return false;
        }
    }
    uint8_t* buf = g_txbuf + (uint32_t)g_tx_use * BUF_SIZE;
    for (uint16_t i = 0; i < length; i++) buf[i] = data[i];
    volatile AdvDesc* d = &g_tx[g_tx_use];
    d->q0 = g_txbuf_phys + (uint64_t)g_tx_use * BUF_SIZE;
    d->d3 = (uint32_t)length << ADVTXD_PAYLEN_SHIFT;     // olinfo_status, clears dd (satoru)
    d->d2 = ADVTXD_DTYP_DATA | ADVTXD_DCMD_DEXT | ADVTXD_DCMD_IFCS | ADVTXD_DCMD_EOP |
            ADVTXD_DCMD_RS | length;
    g_tx_use = next;
    g_tx_count++;
    g_tx_bytes += length;
    __asm__ __volatile__("mfence" ::: "memory");    // descriptor before doorbell (satoru)
    Wr(g_q->tdt, g_tx_use);
    LockRel(&g_tx_lock);
    return true;
}

void Igc::Poll() {
    if (!g_detected || !g_running) return;

    // link transitions: logged once each, collision distance re-applied on up (satoru)
    uint32_t st = Rd(R_STATUS);
    bool up = (st & STATUS_LU) != 0;
    if (up != g_link_prev) {
        g_link_prev = up;
        if (up) {
            ConfigCollisionDist();
            SerialLogger::Log("[igc] link UP ");
            SerialLogger::Log(SpeedStr(st));
            SerialLogger::Log((st & STATUS_FD) ? " full duplex\r\n" : " half duplex\r\n");
        } else {
            SerialLogger::Log("[igc] link DOWN\r\n");
        }
    }
    if (!up && !g_warned_nolink && Expired(g_init_ms, 10000)) {
        g_warned_nolink = true;
        uint16_t ps = 0;
        if (g_phy_ok) { PhyRd(PHY_STATUS, &ps); PhyRd(PHY_STATUS, &ps); }
        SerialLogger::Log("[igc] no link 10s after init: STATUS=0x"); SerialLogger::LogHex(st);
        SerialLogger::Log(" phy_status=0x"); SerialLogger::LogHex(ps);
        SerialLogger::Log(" tx="); SerialLogger::LogDec((int)g_tx_count);
        SerialLogger::Log(" rx="); SerialLogger::LogDec((int)g_rx_count);
        SerialLogger::Log(" (cable/partner? autoneg keeps retrying in hardware)\r\n");
    }

    if (!LockTry(&g_rx_lock)) return;   // another core is draining (satoru)
    if (!g_running) { LockRel(&g_rx_lock); return; }

    g_rx_missed += Rd(R_MPC);

    // tx reclaim is under the tx lock in Send; just try here (satoru)
    if (LockTry(&g_tx_lock)) { ReclaimTx(); LockRel(&g_tx_lock); }

    int consumed = 0;
    for (int n = 0; n < NUM_RX_DESC; n++) {
        volatile AdvDesc* d = &g_rx[g_rx_cur];
        uint32_t stat = d->d2;
        if (!(stat & RXD_STAT_DD)) break;
        __asm__ __volatile__("lfence" ::: "memory");     // read length after dd (satoru)
        uint16_t len = (uint16_t)(d->d3 & 0xFFFF);
        const uint8_t* buf = g_rxbuf + (uint32_t)g_rx_cur * BUF_SIZE;
        if ((stat & RXD_STAT_EOP) && len > 0 && len <= BUF_SIZE) {
            g_rx_count++;
            g_rx_bytes += len;
            if (g_handler) g_handler(buf, len);
        }
        // give the descriptor back: restore the packet address (write-back
        // overwrote it) and zero the header address / status. (satoru)
        d->q0 = g_rxbuf_phys + (uint64_t)g_rx_cur * BUF_SIZE;
        d->d3 = 0;
        d->d2 = 0;
        g_rx_cur = (uint16_t)((g_rx_cur + 1) % NUM_RX_DESC);
        consumed++;
    }
    if (consumed) {
        __asm__ __volatile__("mfence" ::: "memory");
        Wr(g_q->rdt, (uint32_t)((g_rx_cur + NUM_RX_DESC - 1) % NUM_RX_DESC));
    }
    LockRel(&g_rx_lock);
}

void Igc::SetPacketHandler(Igc_PacketHandler h) { g_handler = h; }
uint32_t Igc::GetTxCount() { return g_tx_count; }
uint32_t Igc::GetRxCount() { return g_rx_count; }
uint32_t Igc::GetTxBytes() { return g_tx_bytes; }
uint32_t Igc::GetRxBytes() { return g_rx_bytes; }
uint32_t Igc::GetRxMissed() { return g_rx_missed; }

// ---- shell: `nic`, `nic status`, `nic restart` (satoru)
static int OutCat(char* out, int p, int mx, const char* s) {
    while (*s && p < mx - 1) out[p++] = *s++;
    out[p] = 0;
    return p;
}
static int OutDec(char* out, int p, int mx, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < mx - 1) out[p++] = t[--n];
    out[p] = 0;
    return p;
}

int Igc::Status(char* out, int mx) {
    int p = 0;
    if (mx <= 0) return 0;
    out[0] = 0;
    if (!g_found) return OutCat(out, 0, mx, "igc: no i225/i226/82576 nic bound\n");
    p = OutCat(out, p, mx, g_is_igc ? "igc " : "igb ");
    p = OutCat(out, p, mx, g_detected ? "up" : "not initialised");
    p = OutCat(out, p, mx, ", link ");
    p = OutCat(out, p, mx, IsLinkUp() ? SpeedStr(Rd(R_STATUS)) : "down");
    p = OutCat(out, p, mx, ", mac ");
    const char* h = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char c[3] = { h[g_mac[i] >> 4], h[g_mac[i] & 15], 0 };
        p = OutCat(out, p, mx, c);
        if (i < 5) p = OutCat(out, p, mx, ":");
    }
    p = OutCat(out, p, mx, "\n  tx ");  p = OutDec(out, p, mx, g_tx_count);
    p = OutCat(out, p, mx, " pkts  rx "); p = OutDec(out, p, mx, g_rx_count);
    p = OutCat(out, p, mx, " pkts  rx-missed "); p = OutDec(out, p, mx, g_rx_missed);
    p = OutCat(out, p, mx, "  tx-drop "); p = OutDec(out, p, mx, g_tx_drop);
    p = OutCat(out, p, mx, "\n");
    return p;
}

static int cmd_nic(KuronoShell* /*sh*/, int argc, const char** argv, char* out, int mx) {
    bool restart = false;
    if (argc >= 2 && argv[1] && argv[1][0] == 'r') restart = true;
    if (restart) {
        bool ok = Igc::Restart();
        int p = OutCat(out, 0, mx, ok ? "nic restart: ok\n" : "nic restart: FAILED\n");
        return p + Igc::Status(out + p, mx - p);
    }
    return Igc::Status(out, mx);
}

void Igc::RegisterShellCommands() {
    KuronoShell::RegisterCommand("nic", "nic [status|restart] - igc/igb nic info, restart driver",
                                 ENV_AUTO, "system", cmd_nic);
}
// end (satoru)
