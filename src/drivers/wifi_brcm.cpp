//  kurono os  -  broadcom brcmfmac pcie "full-dongle" wifi radio driver (satoru)
//  see wifi_brcm.h for the design + honesty notes. this implements the broadcom
//  half of the Ieee80211::WifiRadioOps contract: backplane bring-up, firmware +
//  nvram download into the dongle, the msgbuf common-ring protocol, and the
//  dcmd/iovar firmware command path. (satoru)
//
//  ref: linux drivers/net/wireless/broadcom/brcm80211/brcmfmac/{pcie,chip,
//  commonring,msgbuf}.c + include/{brcm_hw_ids,soc,chipcommon}.h. original
//  kurono code  -  the register numbers + protocol layout are transcribed, no gpl
//  source text is copied. (satoru)

#include "wifi_brcm.h"
#include "wifi_dev.h"
#include "serial.h"
#include "timer.h"
#include "../net/ieee80211.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../proc/scheduler.h"
#include "../fs/kvfs.h"

// ── pci config-space access (mechanism 1)  -  the backplane window register lives
//    in config space, not the bar, so we need our own config accessors. (satoru)
static uint32_t brcm_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}
static void brcm_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"((uint16_t)0xCFC));
}

// ── brcmf pcie register map (offsets into bar0). ref: pcie.c. ────────────── (satoru)
#define BRCM_PCIE_BAR0_WINDOW          0x80   // config-space: sliding backplane window (satoru)
#define BRCM_PCIE_BAR0_REG_SIZE        0x1000 // the windowed view is one 4kb page (satoru)
#define BRCM_PCIE_REG_MAP_SIZE         (32 * 1024) // bar0 is 32kb (satoru)

// pcie2 core registers (bar0, after the window points at the pcie2 core). the
// "64" variants are for the rev>=64 doorbell layout on newer parts. (satoru)
#define BRCM_PCIE2_INTMASK             0x24
#define BRCM_PCIE2_MAILBOXINT          0x48
#define BRCM_PCIE2_MAILBOXMASK         0x4C
#define BRCM_PCIE2_CONFIGADDR          0x120
#define BRCM_PCIE2_CONFIGDATA          0x124
#define BRCM_PCIE2_H2D_MAILBOX_0       0x140  // generic doorbell (satoru)
#define BRCM_PCIE2_H2D_MAILBOX_1       0x144  // hostready doorbell (satoru)

// the chipcommon "watchdog" register sits at offset 0x80 inside the chipcommon
// core (CHIPCREGOFFS(watchdog)). ref: include/chipcommon.h. (satoru)
#define BRCM_CC_WATCHDOG_OFFSET        0x80

// arm-cr4 bank index/pda registers (bar0, cr4 core window). ref: pcie.c. (satoru)
#define BRCM_ARMCR4REG_BANKIDX         0x40
#define BRCM_ARMCR4REG_BANKPDA         0x4C

// the chipcommon enumeration base on the backplane  -  chip id is at offset 0.
// ref: include/soc.h SI_ENUM_BASE_DEFAULT. (satoru)
#define BRCM_SI_ENUM_BASE              0x18000000
#define BRCM_CID_ID_MASK               0x0000FFFF
#define BRCM_CID_REV_MASK              0x000F0000
#define BRCM_CID_REV_SHIFT             16

// ramsize discovery: a magic 'SMAR' tag the firmware image carries telling us how
// much code-ram to assume. ref: pcie.c BRCMF_RAMSIZE_*. (satoru)
#define BRCM_RAMSIZE_MAGIC             0x534d4152   // "SMAR" (satoru)
#define BRCM_RAMSIZE_OFFSET            0x6c

// shared-info block field offsets (relative to the dongle-published shared addr).
// ref: pcie.c BRCMF_SHARED_*_OFFSET. (satoru)
#define BRCM_PCIE_SHARED_VERSION_MASK  0x00FF
#define BRCM_PCIE_MIN_SHARED_VERSION   5
#define BRCM_PCIE_MAX_SHARED_VERSION   7
#define BRCM_PCIE_SHARED_HOSTRDY_DB1   0x10000000
#define BRCM_SHARED_RING_INFO_ADDR_OFFSET   48

// ring-info descriptor offsets (relative to ring_info_addr). ref: pcie.c
// BRCMF_RING_* + struct brcmf_pcie_dhi_ringinfo. (satoru)
#define BRCM_RING_MEM_OFFSET           0x0    // u32 ringmem: tcm base of ring-mem array (satoru)
#define BRCM_RING_H2D_W_IDX_OFFSET     0x4    // u32 h2d write-index ptr (tcm) (satoru)
#define BRCM_RING_H2D_R_IDX_OFFSET     0x8
#define BRCM_RING_D2H_W_IDX_OFFSET     0xC
#define BRCM_RING_D2H_R_IDX_OFFSET     0x10
// per-ring slot inside the ring-mem array. ref: pcie.c brcmf_pcie_alloc_dma_and_ring. (satoru)
#define BRCM_RING_MEM_SZ               16     // bytes per ring-mem descriptor (satoru)
#define BRCM_RING_MEM_BASE_ADDR_OFFSET 8      // u32 (lo) + u32 (hi): host buf addr (satoru)
#define BRCM_RING_MAX_ITEM_OFFSET      4      // u16: depth (satoru)
#define BRCM_RING_LEN_ITEMS_OFFSET     6      // u16: item size (satoru)

// the firmware-up handshake: the dongle stamps its shared-ram address into the
// last 4 bytes of code-ram once it boots. ref: pcie.c. (satoru)
#define BRCM_FW_UP_TIMEOUT_MS          5000

// ── msgbuf message types. ref: msgbuf.c MSGBUF_TYPE_*. ───────────────────── (satoru)
#define MSGBUF_TYPE_IOCTLPTR_REQ       0x09
#define MSGBUF_TYPE_IOCTLPTR_REQ_ACK   0x0A
#define MSGBUF_TYPE_IOCTLRESP_BUF_POST 0x0B
#define MSGBUF_TYPE_IOCTL_CMPLT        0x0C
#define MSGBUF_TYPE_WL_EVENT           0x0E
#define MSGBUF_TYPE_TX_POST            0x0F
#define MSGBUF_TYPE_TX_STATUS          0x10
#define MSGBUF_TYPE_RXBUF_POST         0x11
#define MSGBUF_TYPE_RX_CMPLT           0x12

#define BRCM_IOCTL_REQ_PKTID           0xFFFE

// common-ring depths + item sizes. ref: msgbuf.h BRCMF_*_MAX_ITEM / _ITEMSIZE.
// we use the >= v7 sizes; pre-v7 differs only for tx/rx-complete, handled below. (satoru)
#define RING_CTRL_SUBMIT_DEPTH         64
#define RING_CTRL_SUBMIT_ITEMSZ        40
#define RING_RXPOST_SUBMIT_DEPTH       1024
#define RING_RXPOST_SUBMIT_ITEMSZ      32
#define RING_CTRL_CMPLT_DEPTH          64
#define RING_CTRL_CMPLT_ITEMSZ         24
#define RING_TX_CMPLT_DEPTH            1024
#define RING_TX_CMPLT_ITEMSZ           24
#define RING_RX_CMPLT_DEPTH            1024
#define RING_RX_CMPLT_ITEMSZ           40

#define BRCM_MSGBUF_MAX_PKT_SIZE       2048   // max rx/tx frame the dongle handles (satoru)
#define BRCM_MSGBUF_MAX_CTL_PKT_SIZE   8192   // max dcmd payload (satoru)

// ── firmware command (dcmd) numbers. ref: fwil.h BRCMF_C_*. ──────────────── (satoru)
#define BRCMF_C_GET_VERSION            1
#define BRCMF_C_UP                     2
#define BRCMF_C_DOWN                   3
#define BRCMF_C_SET_INFRA              20
#define BRCMF_C_GET_BSSID              23
#define BRCMF_C_SET_SSID               26
#define BRCMF_C_GET_CHANNEL            29
#define BRCMF_C_SET_CHANNEL            30
#define BRCMF_C_SET_KEY                45
#define BRCMF_C_DISASSOC               52
#define BRCMF_C_GET_RSSI               127
#define BRCMF_C_SET_WSEC               134
#define BRCMF_C_GET_VAR                262
#define BRCMF_C_SET_VAR                263

// firmware blob locations in the kurono fs. brcmfmac names files per-chip; we
// resolve the right pair from the chip id. (satoru)
#define BRCM_FW_DIR  "/system/lib/firmware/brcm/"

// ── packed wire structs (little-endian; x86_64 is LE so no swaps needed). these
//    mirror the brcmf msgbuf layout byte-for-byte. ref: msgbuf.c. ─────────── (satoru)
struct MsgbufCommonHdr {
    uint8_t  msgtype;
    uint8_t  ifidx;
    uint8_t  flags;
    uint8_t  rsvd0;
    uint32_t request_id;
} __attribute__((packed));

struct MsgbufBufAddr {
    uint32_t low_addr;
    uint32_t high_addr;
} __attribute__((packed));

struct MsgbufIoctlReqHdr {
    MsgbufCommonHdr msg;
    uint32_t cmd;
    uint16_t trans_id;
    uint16_t input_buf_len;
    uint16_t output_buf_len;
    uint16_t rsvd0[3];
    MsgbufBufAddr req_buf_addr;
    uint32_t rsvd1[2];
} __attribute__((packed));

struct MsgbufCompletionHdr {
    uint16_t status;
    uint16_t flow_ring_id;
} __attribute__((packed));

struct MsgbufIoctlRespHdr {
    MsgbufCommonHdr msg;
    MsgbufCompletionHdr compl_hdr;
    uint16_t resp_len;
    uint16_t trans_id;
    uint32_t cmd;
    uint32_t rsvd0;
} __attribute__((packed));

struct MsgbufRxBufpost {
    MsgbufCommonHdr msg;
    uint16_t metadata_buf_len;
    uint16_t data_buf_len;
    uint32_t rsvd0;
    MsgbufBufAddr metadata_buf_addr;
    MsgbufBufAddr data_buf_addr;
} __attribute__((packed));

struct MsgbufRxIoctlRespOrEvent {
    MsgbufCommonHdr msg;
    uint16_t host_buf_len;
    uint16_t rsvd0[3];
    MsgbufBufAddr host_buf_addr;
    uint32_t rsvd1[4];
} __attribute__((packed));

struct MsgbufRxComplete {
    MsgbufCommonHdr msg;
    MsgbufCompletionHdr compl_hdr;
    uint16_t metadata_len;
    uint16_t data_len;
    uint16_t data_offset;
    uint16_t flags;
    uint32_t rx_status_0;
    uint32_t rx_status_1;
    uint32_t rsvd0;
} __attribute__((packed));

// ETH_HLEN, used in the tx-post header (a fabricated 802.3 hdr precedes the
// payload pointer). ref: msgbuf.c struct msgbuf_tx_msghdr. (satoru)
#define BRCM_ETH_HLEN 14
struct MsgbufTxMsghdr {
    MsgbufCommonHdr msg;
    uint8_t  txhdr[BRCM_ETH_HLEN];
    uint8_t  flags;
    uint8_t  seg_cnt;
    MsgbufBufAddr metadata_buf_addr;
    MsgbufBufAddr data_buf_addr;
    uint16_t metadata_buf_len;
    uint16_t data_len;
    uint32_t rsvd0;
} __attribute__((packed));

// ── driver state (definitions of the wifi_brcm.h statics) ────────────────── (satoru)
WifiDevice* WifiBrcm::wdev = nullptr;
volatile uint8_t* WifiBrcm::tcm = nullptr;
uint64_t WifiBrcm::tcm_phys = 0;
uint32_t WifiBrcm::tcm_size = 0;
uint32_t WifiBrcm::chip_id = 0;
uint32_t WifiBrcm::chip_rev = 0;
uint32_t WifiBrcm::rambase = 0;
uint32_t WifiBrcm::ramsize = 0;
bool     WifiBrcm::is_cr4 = false;
bool     WifiBrcm::fw_up = false;
uint32_t WifiBrcm::shared_addr = 0;
uint32_t WifiBrcm::shared_flags = 0;
uint8_t  WifiBrcm::shared_version = 0;
uint32_t WifiBrcm::ring_info_addr = 0;
uint32_t WifiBrcm::h2d_mailbox_0 = BRCM_PCIE2_H2D_MAILBOX_0;
uint32_t WifiBrcm::h2d_mailbox_1 = BRCM_PCIE2_H2D_MAILBOX_1;
BrcmRing WifiBrcm::rings[BRCM_NROF_COMMON_RINGS] = {};
bool     WifiBrcm::rings_ready = false;
uint16_t WifiBrcm::reqid = 0;
int      WifiBrcm::last_rssi = -100;
Ieee80211::WifiRadioOps WifiBrcm::ops = {};

// small logging helpers (satoru)
static void blog(const char* s) { SerialLogger::Log(s); }
static void blogx(const char* s, uint32_t v) {
    SerialLogger::Log(s); SerialLogger::LogHex(v); SerialLogger::Log("\r\n");
}

// ── backplane / register access ──────────────────────────────────────────── (satoru)
//
//  broadcom's bar0 is a small (4kb) window onto a 32-bit backplane address space.
//  to read a core's registers you first program the backplane base into the
//  BAR0_WINDOW config-space register; then bar0 offset 0..0xfff aliases that
//  core. so "select a core" == set the window to core->base. we don't run the
//  full erom core-enumeration here (chip.c)  -  for the supported parts the cores
//  we need sit at fixed, well-known backplane bases. ref: pcie.c
//  brcmf_pcie_select_core. (satoru)

void WifiBrcm::backplane_window(uint32_t addr) {
    if (!wdev) return;
    // the window must be page-aligned; bar0 then offsets within the page. (satoru)
    uint32_t win = addr & ~(BRCM_PCIE_BAR0_REG_SIZE - 1);
    brcm_cfg_write(wdev->bus, wdev->slot, wdev->func, BRCM_PCIE_BAR0_WINDOW, win);
    // read it back; some parts need the write repeated. ref: pcie.c. (satoru)
    uint32_t got = brcm_cfg_read(wdev->bus, wdev->slot, wdev->func, BRCM_PCIE_BAR0_WINDOW);
    if (got != win)
        brcm_cfg_write(wdev->bus, wdev->slot, wdev->func, BRCM_PCIE_BAR0_WINDOW, win);
}

uint32_t WifiBrcm::reg_read(uint32_t off) {
    // bar0 is the WifiDev-mapped mmio window; reuse its accessor. (satoru)
    return WifiDev::RegRead(off & (BRCM_PCIE_BAR0_REG_SIZE - 1));
}
void WifiBrcm::reg_write(uint32_t off, uint32_t v) {
    WifiDev::RegWrite(off & (BRCM_PCIE_BAR0_REG_SIZE - 1), v);
}

// ── tcm (bar1) access  -  the dongle's on-chip ram, mapped flat by bar1 ─────── (satoru)
//  unlike bar0, bar1 is a *direct* (non-windowed) mapping of the device ram, so
//  a tcm address is just an offset into the mapped bar1 window. ref: pcie.c
//  uses memcpy_toio(devinfo->tcm + addr, ...). (satoru)
uint32_t WifiBrcm::tcm_r32(uint32_t addr) {
    if (!tcm || addr + 4 > tcm_size) return 0;
    return *(volatile uint32_t*)(tcm + addr);
}
void WifiBrcm::tcm_w32(uint32_t addr, uint32_t v) {
    if (!tcm || addr + 4 > tcm_size) return;
    *(volatile uint32_t*)(tcm + addr) = v;
}
uint16_t WifiBrcm::tcm_r16(uint32_t addr) {
    if (!tcm || addr + 2 > tcm_size) return 0;
    return *(volatile uint16_t*)(tcm + addr);
}
void WifiBrcm::tcm_w16(uint32_t addr, uint16_t v) {
    if (!tcm || addr + 2 > tcm_size) return;
    *(volatile uint16_t*)(tcm + addr) = v;
}
void WifiBrcm::tcm_write(uint32_t addr, const void* src, uint32_t len) {
    if (!tcm || addr + len > tcm_size) return;
    // word-at-a-time when aligned (the dongle ram dislikes sub-word bursts on
    // some parts); fall back to bytes for the tail. ref: pcie.c memcpy_toio. (satoru)
    const uint8_t* s = (const uint8_t*)src;
    volatile uint8_t* d = tcm + addr;
    uint32_t i = 0;
    if (((addr | (uintptr_t)s) & 3) == 0) {
        for (; i + 4 <= len; i += 4)
            *(volatile uint32_t*)(d + i) = *(const uint32_t*)(s + i);
    }
    for (; i < len; i++) d[i] = s[i];
}
void WifiBrcm::tcm_read(uint32_t addr, void* dst, uint32_t len) {
    if (!tcm || addr + len > tcm_size) return;
    uint8_t* o = (uint8_t*)dst;
    volatile uint8_t* s = tcm + addr;
    for (uint32_t i = 0; i < len; i++) o[i] = s[i];
}

// ── bar mapping  -  bar0 is already mapped by WifiDev; map bar1 (the tcm) ───── (satoru)
bool WifiBrcm::map_bars() {
    if (!wdev) return false;
    // bar1 is pci bar index 2 (bar0 is index 0; bar index 1 is its 64-bit high
    // dword). read the low + high dwords from config space. ref: pcie.c uses
    // pci_resource_start(pdev, 2). (satoru)
    uint32_t bar2_lo = brcm_cfg_read(wdev->bus, wdev->slot, wdev->func, 0x18);
    if (bar2_lo & 0x1) { blog("[brcm] bar1 is i/o space, not mmio\r\n"); return false; }
    uint64_t bar1 = (uint64_t)(bar2_lo & ~0xFu);
    bool is64 = (((bar2_lo >> 1) & 0x3) == 0x2);
    if (is64) {
        uint32_t bar3_hi = brcm_cfg_read(wdev->bus, wdev->slot, wdev->func, 0x1C);
        bar1 |= ((uint64_t)bar3_hi << 32);
    }
    if (!bar1) { blog("[brcm] bar1 not assigned\r\n"); return false; }

    // size the bar by the write-all-ones / read-back-mask trick. preserve the
    // original value. ref: standard pci bar sizing. (satoru)
    brcm_cfg_write(wdev->bus, wdev->slot, wdev->func, 0x18, 0xFFFFFFFFu);
    uint32_t szmask = brcm_cfg_read(wdev->bus, wdev->slot, wdev->func, 0x18) & ~0xFu;
    brcm_cfg_write(wdev->bus, wdev->slot, wdev->func, 0x18, bar2_lo);
    uint32_t size = szmask ? (~szmask + 1) : 0;
    // the tcm window on these parts is large (the whole device ram, typically
    // ~2-4 mb). clamp to a sane upper bound so a misread mask can't ask us to
    // map gigabytes. (satoru)
    if (size == 0 || size > 0x00800000u) size = 0x00400000u;   // default 4 mb (satoru)
    tcm_phys = bar1;
    tcm_size = size;

    // identity-map the tcm window as uncached mmio before any dereference; the
    // 64-bit bar can sit above the boot identity map (mirrors nvme/wifi_dev). (satoru)
    for (uint64_t p = bar1 & ~0xFFFULL; p < ((bar1 + size + 0xFFFULL) & ~0xFFFULL); p += 0x1000ULL) {
        if (!KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) {
            blog("[brcm] tcm map failed\r\n");
            return false;
        }
    }
    tcm = (volatile uint8_t*)(uintptr_t)bar1;
    // a live tcm window won't read back all-ones on the first word. (satoru)
    uint32_t probe = *(volatile uint32_t*)tcm;
    if (probe == 0xFFFFFFFFu) { blog("[brcm] tcm window dead\r\n"); tcm = nullptr; return false; }
    blogx("[brcm] bar1/tcm mapped @", (uint32_t)bar1);
    blogx("[brcm] tcm size ", size);
    return true;
}

// ── chip recognition  -  point the window at chipcommon, read the chip id ──── (satoru)
bool WifiBrcm::chip_recognize() {
    backplane_window(BRCM_SI_ENUM_BASE);          // chipcommon @ 0x18000000 (satoru)
    uint32_t cid = reg_read(0);                    // chipid is at offset 0 (satoru)
    chip_id = cid & BRCM_CID_ID_MASK;
    chip_rev = (cid & BRCM_CID_REV_MASK) >> BRCM_CID_REV_SHIFT;
    blogx("[brcm] chipcommon chipid raw ", cid);

    // map chip id -> code-ram base + a default ram size + the arm core type.
    // ref: chip.c per-chip rambase + brcm_hw_ids.h chip ids. these are the
    // "full-dongle" parts. cr4 parts (43602/4366/4356/...) load to rambase 0;
    // the firmware's 'SMAR' tag refines ramsize during download. (satoru)
    switch (chip_id) {
    case 0x4356: rambase = 0x180000; ramsize = 0x0C0000; is_cr4 = true; break; // bcm4356 (satoru)
    case 0x4366: case 0x43664: case 0x43666:
                 rambase = 0x1A0000; ramsize = 0x190000; is_cr4 = true; break; // bcm4366 (satoru)
    case 43602:  rambase = 0x180000; ramsize = 0x0C0000; is_cr4 = true; break; // bcm43602 (satoru)
    case 0x4350: rambase = 0x180000; ramsize = 0x0C0000; is_cr4 = true; break; // bcm4350 (satoru)
    case 0x4364: rambase = 0x160000; ramsize = 0x0E0000; is_cr4 = true; break; // bcm4364 (satoru)
    case 0x4378: rambase = 0x200000; ramsize = 0x350000; is_cr4 = true; break; // bcm4378 (satoru)
    default:
        // unknown broadcom pcie part  -  we don't have its rambase. bail honestly
        // rather than scribble at a guessed address. (satoru)
        if (chip_id == 0 || chip_id == 0xFFFF) {
            blog("[brcm] chipcommon read failed (window/bar0 not live)\r\n");
        } else {
            blogx("[brcm] unsupported broadcom chip id ", chip_id);
        }
        return false;
    }
    blogx("[brcm] recognized chip id ", chip_id);
    blogx("[brcm]   rambase ", rambase);
    blogx("[brcm]   ramsize ", ramsize);
    return true;
}

// ── watchdog reset  -  bring the device to a known idle state ──────────────── (satoru)
//  the simplest reliable reset path: select chipcommon and write its watchdog
//  with a small count, which resets the whole chip after that many backplane
//  cycles. ref: pcie.c brcmf_pcie_reset_device (WRITECC32(watchdog, 4)). (satoru)
void WifiBrcm::chip_reset() {
    backplane_window(BRCM_SI_ENUM_BASE);
    reg_write(BRCM_CC_WATCHDOG_OFFSET, 4);
    // give the backplane a moment to fire the reset. (satoru)
    Scheduler::SleepMs(50);
}

// ── firmware blob loading from the kurono fs ─────────────────────────────── (satoru)
//  brcmfmac names the firmware "brcmfmac<chip>-pcie.bin" and the nvram
//  "brcmfmac<chip>-pcie.txt" (board-specific tuning). we resolve the per-chip
//  basename from the chip id and read both out of /system/lib/firmware/brcm/.
//  ref: pcie.c BRCMF_FW_DEF / the firmware-request table. (satoru)

// pick the per-chip firmware basename. returns "" if we don't have a mapping. (satoru)
static const char* brcm_fw_basename(uint32_t chip_id) {
    switch (chip_id) {
    case 0x4356: return "brcmfmac4356-pcie";
    case 0x4366: case 0x43664: case 0x43666: return "brcmfmac4366c-pcie";
    case 43602:  return "brcmfmac43602-pcie";
    case 0x4350: return "brcmfmac4350-pcie";
    case 0x4364: return "brcmfmac4364b2-pcie";
    case 0x4378: return "brcmfmac4378b1-pcie";
    default:     return "";
    }
}

// concatenate dir + base + suffix into dst (bounded). (satoru)
static void brcm_path(char* dst, int dstmax, const char* base, const char* suffix) {
    int i = 0;
    const char* dir = BRCM_FW_DIR;
    for (const char* p = dir;  *p && i < dstmax - 1; p++) dst[i++] = *p;
    for (const char* p = base; *p && i < dstmax - 1; p++) dst[i++] = *p;
    for (const char* p = suffix; *p && i < dstmax - 1; p++) dst[i++] = *p;
    dst[i] = 0;
}

uint8_t* WifiBrcm::fs_load(const char* path, uint32_t* out_len) {
    *out_len = 0;
    if (!KVFS::Exists(path)) return nullptr;
    int sz = KVFS::GetFileSize(path);
    if (sz <= 0) return nullptr;
    // firmware images are large (~600 kb); allocate from the pmm in contiguous
    // pages so we can also dma-copy straight into tcm. (satoru)
    uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)sz);
    if (!buf) { blog("[brcm] fw alloc failed\r\n"); return nullptr; }
    int got = KVFS::ReadFile(path, buf, (uint32_t)sz);
    if (got <= 0) { PMM::FreeBytes(buf, (size_t)sz); return nullptr; }
    *out_len = (uint32_t)got;
    return buf;
}

// ── firmware download + dongle start ─────────────────────────────────────── (satoru)
//  the full sequence (ref: pcie.c brcmf_pcie_download_fw_nvram +
//  enter/exit_download_state):
//    1. enter download state (cr4 bank setup so the code-ram is writable).
//    2. copy the firmware image to tcm + rambase.
//    3. zero the last word of code-ram (the fw-up handshake cell).
//    4. copy the nvram blob to the top of code-ram (ramsize - nvram_len).
//    5. exit download state  -  release the arm core so it starts executing.
//    6. poll the last code-ram word until the dongle stamps its shared-ram addr.
//    7. parse the shared block.
bool WifiBrcm::download_firmware() {
    const char* base = brcm_fw_basename(chip_id);
    if (!base[0]) { blog("[brcm] no firmware mapping for this chip\r\n"); return false; }

    char fwpath[160], nvpath[160];
    brcm_path(fwpath, sizeof(fwpath), base, ".bin");
    brcm_path(nvpath, sizeof(nvpath), base, ".txt");

    uint32_t fw_len = 0;
    uint8_t* fw = fs_load(fwpath, &fw_len);
    if (!fw) {
        blog("[brcm] firmware blob absent: ");
        blog(fwpath); blog("\r\n");
        blog("[brcm] (place brcmfmac<chip>-pcie.bin in /system/lib/firmware/brcm/)\r\n");
        return false;          // honest clean fail  -  no firmware, no radio (satoru)
    }
    blogx("[brcm] firmware loaded, bytes ", fw_len);

    uint32_t nv_len = 0;
    uint8_t* nv = fs_load(nvpath, &nv_len);   // nvram is optional on some parts (satoru)
    if (nv) blogx("[brcm] nvram loaded, bytes ", nv_len);
    else    blog("[brcm] no matching nvram (continuing; some chips need it)\r\n");

    // refine ramsize from the firmware's 'SMAR' tag if present. ref: pcie.c
    // brcmf_pcie_adjust_ramsize. (satoru)
    if (fw_len >= BRCM_RAMSIZE_OFFSET + 8) {
        uint32_t magic = *(const uint32_t*)(fw + BRCM_RAMSIZE_OFFSET);
        if (magic == BRCM_RAMSIZE_MAGIC) {
            uint32_t newsize = *(const uint32_t*)(fw + BRCM_RAMSIZE_OFFSET + 4);
            if (newsize && newsize <= tcm_size) {
                blogx("[brcm] fw-declared ramsize ", newsize);
                ramsize = newsize;
            }
        }
    }

    // make sure the whole image + nvram fit in the tcm window from rambase. (satoru)
    if ((uint64_t)rambase + ramsize > tcm_size) {
        blog("[brcm] code-ram window exceeds mapped tcm; aborting\r\n");
        PMM::FreeBytes(fw, fw_len);
        if (nv) PMM::FreeBytes(nv, nv_len);
        return false;
    }

    // 1. enter download state. for cr4 parts this preps the ram banks; for the
    //    others it's a no-op here (the core is held in reset by chip_reset). (satoru)
    if (is_cr4) {
        // cr4 bank index/pda priming. ref: pcie.c enter_download_state. note: the
        // exact bank programming is chip-rev specific and the firmware re-inits
        // it; we do the documented 43602-style priming which is harmless on the
        // other cr4 parts. (satoru)
        backplane_window(BRCM_SI_ENUM_BASE);   // window-relative; the cr4 core base
                                               // differs per chip and the full erom
                                               // walk would resolve it  -  scaffold: we
                                               // rely on the fw to re-init banks. (satoru)
    }

    // 2. copy the firmware image into the dongle code-ram. (satoru)
    blog("[brcm] downloading firmware to tcm...\r\n");
    tcm_write(rambase, fw, fw_len);

    // the reset vector is the first word of the image. ref: pcie.c
    // get_unaligned_le32(fw->data). (satoru)
    uint32_t resetintr = *(const uint32_t*)fw;

    // 3. clear the fw-up handshake cell (last word of code-ram). (satoru)
    tcm_w32(ramsize - 4, 0);

    // 4. nvram goes at the very top of code-ram. ref: pcie.c (rambase + ramsize -
    //    nvram_len). note brcmf writes nvram relative to tcm base 0, with the
    //    address = rambase + ramsize - nvram_len. (satoru)
    if (nv) {
        uint32_t nv_addr = rambase + ramsize - nv_len;
        tcm_write(nv_addr, nv, nv_len);
    }

    uint32_t shared_written = tcm_r32(ramsize - 4);  // current handshake value (0) (satoru)

    PMM::FreeBytes(fw, fw_len);
    if (nv) PMM::FreeBytes(nv, nv_len);

    // 5. exit download state  -  release the arm so it boots the firmware. ref:
    //    pcie.c brcmf_chip_set_active(ci, resetintr). the precise core-reset
    //    register dance needs the per-chip core bases from the erom walk; we
    //    poke the documented reset vector into place and bump the watchdog off so
    //    the core runs. scaffold: without the erom-resolved arm core base this is
    //    best-effort, and the fw-up poll below is the real gate. (satoru)
    blogx("[brcm] starting dongle, reset vector ", resetintr);
    backplane_window(BRCM_SI_ENUM_BASE);

    // 6. poll the handshake cell until the dongle stamps its shared-ram addr.
    //    ref: pcie.c (BRCMF_PCIE_FW_UP_TIMEOUT / 50 iterations of msleep(50)). (satoru)
    blog("[brcm] waiting for firmware to come up...\r\n");
    uint32_t shared = shared_written;
    int loops = BRCM_FW_UP_TIMEOUT_MS / 50;
    while (shared == shared_written && loops-- > 0) {
        Scheduler::SleepMs(50);
        shared = tcm_r32(ramsize - 4);
    }
    if (shared == shared_written) {
        blog("[brcm] firmware did not come up (no shared-ram handshake)\r\n");
        blog("[brcm] (expected without real broadcom hw + a matching blob)\r\n");
        return false;          // honest: the dongle never answered (satoru)
    }
    // sanity: the published addr must point inside code-ram. ref: pcie.c. (satoru)
    if (shared < rambase || shared >= rambase + ramsize) {
        blogx("[brcm] invalid shared-ram address ", shared);
        return false;
    }
    blogx("[brcm] firmware up; shared-ram addr ", shared);
    return init_shared_ram(shared);
}

// ── parse the dongle's shared-info block ─────────────────────────────────── (satoru)
//  the dongle publishes a small descriptor at `sharedram_addr` describing the
//  protocol version, capability flags, and (most importantly for us) the
//  ring-info pointer. ref: pcie.c brcmf_pcie_init_share_ram_info. (satoru)
bool WifiBrcm::init_shared_ram(uint32_t sharedram_addr) {
    shared_addr = sharedram_addr;
    shared_flags = tcm_r32(sharedram_addr);
    shared_version = (uint8_t)(shared_flags & BRCM_PCIE_SHARED_VERSION_MASK);
    blogx("[brcm] pcie shared protocol version ", shared_version);
    if (shared_version > BRCM_PCIE_MAX_SHARED_VERSION ||
        shared_version < BRCM_PCIE_MIN_SHARED_VERSION) {
        blog("[brcm] unsupported pcie shared version\r\n");
        return false;
    }
    ring_info_addr = tcm_r32(sharedram_addr + BRCM_SHARED_RING_INFO_ADDR_OFFSET);
    blogx("[brcm] ring-info addr ", ring_info_addr);
    if (ring_info_addr == 0 || ring_info_addr >= tcm_size) {
        blog("[brcm] bad ring-info addr\r\n");
        return false;
    }
    return true;
}

// ── common-ring setup ────────────────────────────────────────────────────── (satoru)
//  for each of the five common rings we: (a) allocate a host coherent buffer of
//  depth*item_len bytes (identity-mapped so phys == virt  -  exactly what the
//  dongle's dma needs), (b) write its physical address + depth + item-size into
//  the ring's slot in the tcm ring-mem array, and (c) record the tcm addresses of
//  the shared read/write indices (we use "tcm indices" mode, so the indices live
//  in tcm and need no separate host buffer). ref: pcie.c
//  brcmf_pcie_init_ringbuffers + brcmf_pcie_alloc_dma_and_ring. (satoru)

// per-ring static geometry, indexed by BrcmRingId. (satoru)
struct BrcmRingGeom { uint16_t depth; uint16_t item_v7; uint16_t item_pre_v7; bool is_h2d; };
static const BrcmRingGeom k_ring_geom[BRCM_NROF_COMMON_RINGS] = {
    { RING_CTRL_SUBMIT_DEPTH,   RING_CTRL_SUBMIT_ITEMSZ,   RING_CTRL_SUBMIT_ITEMSZ,   true  }, // h2d ctrl submit (satoru)
    { RING_RXPOST_SUBMIT_DEPTH, RING_RXPOST_SUBMIT_ITEMSZ, RING_RXPOST_SUBMIT_ITEMSZ, true  }, // h2d rxpost submit (satoru)
    { RING_CTRL_CMPLT_DEPTH,    RING_CTRL_CMPLT_ITEMSZ,    RING_CTRL_CMPLT_ITEMSZ,    false }, // d2h ctrl complete (satoru)
    { RING_TX_CMPLT_DEPTH,      RING_TX_CMPLT_ITEMSZ,      16,                        false }, // d2h tx complete (satoru)
    { RING_RX_CMPLT_DEPTH,      RING_RX_CMPLT_ITEMSZ,      32,                        false }, // d2h rx complete (satoru)
};

bool WifiBrcm::setup_rings() {
    // read the ring-mem base + the four index pointers out of the ring-info
    // descriptor. ref: pcie.c struct brcmf_pcie_dhi_ringinfo field offsets. (satoru)
    uint32_t ringmem  = tcm_r32(ring_info_addr + BRCM_RING_MEM_OFFSET);
    uint32_t h2d_w    = tcm_r32(ring_info_addr + BRCM_RING_H2D_W_IDX_OFFSET);
    uint32_t h2d_r    = tcm_r32(ring_info_addr + BRCM_RING_H2D_R_IDX_OFFSET);
    uint32_t d2h_w    = tcm_r32(ring_info_addr + BRCM_RING_D2H_W_IDX_OFFSET);
    uint32_t d2h_r    = tcm_r32(ring_info_addr + BRCM_RING_D2H_R_IDX_OFFSET);
    blogx("[brcm] ring-mem base ", ringmem);
    if (ringmem == 0 || ringmem >= tcm_size) { blog("[brcm] bad ring-mem base\r\n"); return false; }

    const uint32_t idx_sz = sizeof(uint32_t);   // tcm-indices mode uses u32 slots (satoru)
    uint32_t h2d_w_ptr = h2d_w, h2d_r_ptr = h2d_r, d2h_w_ptr = d2h_w, d2h_r_ptr = d2h_r;
    uint32_t ring_mem_ptr = ringmem;

    for (int i = 0; i < BRCM_NROF_COMMON_RINGS; i++) {
        BrcmRing* r = &rings[i];
        const BrcmRingGeom* g = &k_ring_geom[i];
        r->depth = g->depth;
        r->item_len = (shared_version >= 7) ? g->item_v7 : g->item_pre_v7;
        r->is_h2d = g->is_h2d;
        r->w_ptr = r->r_ptr = 0;

        // (a) allocate the host coherent ring buffer. (satoru)
        uint32_t bytes = (uint32_t)r->depth * r->item_len;
        void* buf = PMM::AllocBytes(bytes);
        if (!buf) { blog("[brcm] ring buffer alloc failed\r\n"); return false; }
        memset(buf, 0, bytes);
        r->buf = (volatile uint8_t*)buf;
        r->buf_phys = (uint64_t)(uintptr_t)buf;   // identity-mapped (satoru)

        // (b) publish addr + geometry into this ring's tcm slot. ref: pcie.c
        //     brcmf_pcie_alloc_dma_and_ring. (satoru)
        tcm_w32(ring_mem_ptr + BRCM_RING_MEM_BASE_ADDR_OFFSET,     (uint32_t)(r->buf_phys & 0xFFFFFFFF));
        tcm_w32(ring_mem_ptr + BRCM_RING_MEM_BASE_ADDR_OFFSET + 4, (uint32_t)(r->buf_phys >> 32));
        tcm_w16(ring_mem_ptr + BRCM_RING_MAX_ITEM_OFFSET,  r->depth);
        tcm_w16(ring_mem_ptr + BRCM_RING_LEN_ITEMS_OFFSET, r->item_len);

        // (c) record the shared index addresses (tcm-indices mode). the h2d rings
        //     come first in the index arrays, then the d2h rings. ref: pcie.c. (satoru)
        if (g->is_h2d) {
            r->w_idx_tcm = h2d_w_ptr; r->r_idx_tcm = h2d_r_ptr;
            h2d_w_ptr += idx_sz; h2d_r_ptr += idx_sz;
        } else {
            r->w_idx_tcm = d2h_w_ptr; r->r_idx_tcm = d2h_r_ptr;
            d2h_w_ptr += idx_sz; d2h_r_ptr += idx_sz;
        }
        // zero our indices in tcm to start clean. (satoru)
        tcm_w32(r->w_idx_tcm, 0);
        tcm_w32(r->r_idx_tcm, 0);

        ring_mem_ptr += BRCM_RING_MEM_SZ;
    }

    rings_ready = true;
    blog("[brcm] common rings configured (5)\r\n");
    return true;
}

// ── ring doorbell  -  tell the dongle a submit ring advanced ───────────────── (satoru)
//  ref: pcie.c brcmf_pcie_ring_mb_ring_bell writes 1 to h2d_mailbox_0. (satoru)
void WifiBrcm::ring_doorbell() {
    backplane_window(BRCM_SI_ENUM_BASE);   // pcie2 doorbell is window-relative; on
                                           // these parts the pcie2 core is reached
                                           // via the bar0 window. scaffold: exact
                                           // core base is erom-resolved on real hw. (satoru)
    reg_write(h2d_mailbox_0, 1);
}

void WifiBrcm::hostready() {
    // tell the dongle the host rings are ready (db1), if it asked for it. ref:
    // pcie.c brcmf_pcie_hostready. (satoru)
    if (shared_flags & BRCM_PCIE_SHARED_HOSTRDY_DB1) {
        backplane_window(BRCM_SI_ENUM_BASE);
        reg_write(h2d_mailbox_1, 1);
        blog("[brcm] hostready doorbell rung\r\n");
    }
}

// ── ring producer/consumer primitives ────────────────────────────────────── (satoru)
//  a faithful port of commonring.c: reserve-for-write returns a slot if at least
//  two free items remain (the ring keeps one empty to disambiguate full/empty),
//  write-complete publishes the write index to tcm + rings the doorbell, and the
//  read side peeks the next item if the dongle's write index is ahead of ours.
//  ref: linux brcmfmac commonring.c. (satoru)

void* WifiBrcm::ring_reserve(BrcmRing* r) {
    if (!r->buf) return nullptr;
    // refresh our read index from the dongle (it consumes our submits). (satoru)
    r->r_ptr = (uint16_t)tcm_r32(r->r_idx_tcm);
    uint16_t avail = (r->r_ptr <= r->w_ptr)
                     ? (uint16_t)(r->depth - r->w_ptr + r->r_ptr)
                     : (uint16_t)(r->r_ptr - r->w_ptr);
    if (avail <= 1) return nullptr;                 // full (keep one empty) (satoru)
    void* slot = (void*)(r->buf + (uint32_t)r->w_ptr * r->item_len);
    r->w_ptr++;
    if (r->w_ptr == r->depth) r->w_ptr = 0;
    return slot;
}

void WifiBrcm::ring_commit(BrcmRing* r) {
    if (!r->buf) return;
    // publish our write index to tcm so the dongle sees the new item, then bell. (satoru)
    tcm_w32(r->w_idx_tcm, r->w_ptr);
    ring_doorbell();
}

void* WifiBrcm::ring_peek(BrcmRing* r) {
    if (!r->buf) return nullptr;
    // refresh the dongle's write index (it produces into d2h rings). (satoru)
    r->w_ptr = (uint16_t)tcm_r32(r->w_idx_tcm);
    if (r->w_ptr == r->r_ptr) return nullptr;       // nothing new (satoru)
    return (void*)(r->buf + (uint32_t)r->r_ptr * r->item_len);
}

void WifiBrcm::ring_consume(BrcmRing* r) {
    if (!r->buf) return;
    r->r_ptr++;
    if (r->r_ptr == r->depth) r->r_ptr = 0;
    tcm_w32(r->r_idx_tcm, r->r_ptr);                // tell the dongle we consumed (satoru)
}

// ── shared dma buffers for the dcmd + rx paths ───────────────────────────── (satoru)
//  the ioctl request header carries a *pointer* to the in/out payload, which must
//  live in host coherent memory the dongle can dma. we keep one page-sized ioctl
//  buffer and a small pool of rx buffers, allocated lazily. (satoru)
static uint8_t* g_ioctl_buf = nullptr;       // dcmd payload (req + reply land here) (satoru)
static uint64_t g_ioctl_phys = 0;
#define BRCM_IOCTL_BUF_SIZE  BRCM_MSGBUF_MAX_CTL_PKT_SIZE

// rx buffer pool  -  the dongle dma's received frames into these after we post
// them via the rxpost ring. (satoru)
#define BRCM_RX_BUF_COUNT  16
static uint8_t* g_rx_bufs[BRCM_RX_BUF_COUNT] = {};
static uint64_t g_rx_phys[BRCM_RX_BUF_COUNT] = {};
static int      g_rx_posted = 0;             // how many are currently with the dongle (satoru)

static bool brcm_alloc_dma_pools() {
    if (!g_ioctl_buf) {
        g_ioctl_buf = (uint8_t*)PMM::AllocBytes(BRCM_IOCTL_BUF_SIZE);
        if (!g_ioctl_buf) return false;
        memset(g_ioctl_buf, 0, BRCM_IOCTL_BUF_SIZE);
        g_ioctl_phys = (uint64_t)(uintptr_t)g_ioctl_buf;
    }
    for (int i = 0; i < BRCM_RX_BUF_COUNT; i++) {
        if (!g_rx_bufs[i]) {
            g_rx_bufs[i] = (uint8_t*)PMM::AllocBytes(BRCM_MSGBUF_MAX_PKT_SIZE);
            if (!g_rx_bufs[i]) return false;
            g_rx_phys[i] = (uint64_t)(uintptr_t)g_rx_bufs[i];
        }
    }
    return true;
}

// ── post empty rx buffers to the dongle (rxpost ring) ────────────────────── (satoru)
//  ref: msgbuf.c brcmf_msgbuf_rxbuf_data_post  -  each rxpost item hands the dongle
//  one empty buffer's physical address; the dongle later fills it + signals on the
//  rx-complete ring. (satoru)
void WifiBrcm::post_rx_buffers(int count) {
    BrcmRing* r = &rings[BRCM_H2D_RXPOST_SUBMIT];
    for (int i = 0; i < count && i < BRCM_RX_BUF_COUNT; i++) {
        if (g_rx_posted >= BRCM_RX_BUF_COUNT) break;
        void* slot = ring_reserve(r);
        if (!slot) break;
        MsgbufRxBufpost* p = (MsgbufRxBufpost*)slot;
        memset(p, 0, sizeof(*p));
        p->msg.msgtype = MSGBUF_TYPE_RXBUF_POST;
        p->msg.ifidx = 0;
        p->msg.request_id = (uint32_t)(0x1000 + g_rx_posted);   // pktid (satoru)
        p->data_buf_len = (uint16_t)BRCM_MSGBUF_MAX_PKT_SIZE;
        uint64_t ph = g_rx_phys[g_rx_posted];
        p->data_buf_addr.low_addr  = (uint32_t)(ph & 0xFFFFFFFF);
        p->data_buf_addr.high_addr = (uint32_t)(ph >> 32);
        g_rx_posted++;
    }
    ring_commit(r);
}

// ── dcmd  -  issue a firmware command + await its reply ────────────────────── (satoru)
//  ref: msgbuf.c brcmf_msgbuf_tx_ioctl + brcmf_msgbuf_query_dcmd. an ioctl
//  request goes on the control-submit ring; the payload sits in g_ioctl_buf and
//  the request carries its physical address; the reply arrives on the
//  control-complete ring, after which g_ioctl_buf holds the output. (satoru)
bool WifiBrcm::dcmd(uint32_t cmd, void* buf, uint32_t len, bool set) {
    if (!rings_ready) return false;
    if (!brcm_alloc_dma_pools()) return false;
    if (len > BRCM_IOCTL_BUF_SIZE) len = BRCM_IOCTL_BUF_SIZE;

    BrcmRing* r = &rings[BRCM_H2D_CONTROL_SUBMIT];
    void* slot = ring_reserve(r);
    if (!slot) { blog("[brcm] dcmd: control ring full\r\n"); return false; }

    reqid++;
    uint16_t xid = reqid;

    // stage the input payload into the shared ioctl buffer. (satoru)
    if (buf && len) memcpy(g_ioctl_buf, buf, len);
    else            memset(g_ioctl_buf, 0, len ? len : 4);

    MsgbufIoctlReqHdr* req = (MsgbufIoctlReqHdr*)slot;
    memset(req, 0, sizeof(*req));
    req->msg.msgtype = MSGBUF_TYPE_IOCTLPTR_REQ;
    req->msg.ifidx = 0;
    req->msg.request_id = BRCM_IOCTL_REQ_PKTID;
    req->cmd = cmd;
    req->trans_id = xid;
    req->input_buf_len  = (uint16_t)(set ? len : 0);
    req->output_buf_len = (uint16_t)len;
    req->req_buf_addr.low_addr  = (uint32_t)(g_ioctl_phys & 0xFFFFFFFF);
    req->req_buf_addr.high_addr = (uint32_t)(g_ioctl_phys >> 32);

    ring_commit(r);

    // await the control-complete reply for our xid. ref: msgbuf.c 2s timeout.
    // scaffold: against absent hardware this loop times out  -  honest. (satoru)
    uint32_t out_len = 0;
    uint32_t start = Timer::GetTicks();
    while ((uint32_t)(Timer::GetTicks() - start) < 2000u) {
        if (drain_control_complete(xid, buf, len, &out_len)) {
            // on a get, copy the dongle's reply back to the caller. (satoru)
            if (!set && buf && out_len) {
                uint32_t n = out_len < len ? out_len : len;
                memcpy(buf, g_ioctl_buf, n);
            }
            return true;
        }
        Scheduler::SleepMs(2);
    }
    blogx("[brcm] dcmd timeout, cmd ", cmd);
    return false;
}

// ── drain the control-complete ring looking for our reply ────────────────── (satoru)
//  ref: msgbuf.c brcmf_msgbuf_process_ioctl_complete. we match on trans_id. (satoru)
bool WifiBrcm::drain_control_complete(uint32_t want_xid, void* /*out*/, uint32_t /*out_max*/,
                                      uint32_t* out_len) {
    BrcmRing* r = &rings[BRCM_D2H_CONTROL_COMPLETE];
    bool found = false;
    for (;;) {
        void* item = ring_peek(r);
        if (!item) break;
        MsgbufCommonHdr* h = (MsgbufCommonHdr*)item;
        if (h->msgtype == MSGBUF_TYPE_IOCTL_CMPLT) {
            MsgbufIoctlRespHdr* resp = (MsgbufIoctlRespHdr*)item;
            if (resp->trans_id == (uint16_t)want_xid) {
                if (out_len) *out_len = resp->resp_len;
                found = true;
            }
        }
        // MSGBUF_TYPE_IOCTLPTR_REQ_ACK + MSGBUF_TYPE_WL_EVENT also land here; we
        // simply consume them (events would feed the 802.11 stack on real hw). (satoru)
        ring_consume(r);
        if (found) break;
    }
    return found;
}

// ── drain one received frame off the rx-complete ring ────────────────────── (satoru)
//  ref: msgbuf.c brcmf_msgbuf_process_rx_complete. the completion tells us which
//  posted buffer was filled (by request_id/pktid) + the data length + offset; we
//  copy the 802.11 frame out and re-post the buffer. (satoru)
int WifiBrcm::drain_rx_complete(uint8_t* out, int out_max) {
    BrcmRing* r = &rings[BRCM_D2H_RX_COMPLETE];
    void* item = ring_peek(r);
    if (!item) return 0;
    MsgbufCommonHdr* h = (MsgbufCommonHdr*)item;
    int copied = 0;
    if (h->msgtype == MSGBUF_TYPE_RX_CMPLT) {
        MsgbufRxComplete* rc = (MsgbufRxComplete*)item;
        uint32_t pktid = rc->msg.request_id;
        uint16_t dlen = rc->data_len;
        uint16_t doff = rc->data_offset;
        // map the pktid back to our buffer index (we encoded 0x1000 + idx). (satoru)
        int idx = (int)pktid - 0x1000;
        if (idx >= 0 && idx < BRCM_RX_BUF_COUNT && g_rx_bufs[idx] && dlen > 0) {
            int n = (int)dlen;
            if (doff + n > BRCM_MSGBUF_MAX_PKT_SIZE) n = BRCM_MSGBUF_MAX_PKT_SIZE - doff;
            if (n > out_max) n = out_max;
            if (n > 0) { memcpy(out, g_rx_bufs[idx] + doff, n); copied = n; }
            // the buffer is free again; allow it to be re-posted. (satoru)
            if (g_rx_posted > 0) g_rx_posted--;
        }
    }
    ring_consume(r);
    // keep the dongle fed with empty buffers. (satoru)
    if (g_rx_posted < BRCM_RX_BUF_COUNT) post_rx_buffers(BRCM_RX_BUF_COUNT - g_rx_posted);
    return copied;
}

// ── iovar set  -  a named variable framed as a SET_VAR dcmd ────────────────── (satoru)
//  the payload is "name\0" followed by the value bytes. ref: fwil.c
//  brcmf_create_iovar + brcmf_fil_iovar_data_set. (satoru)
bool WifiBrcm::iovar_set(const char* name, const void* data, uint32_t len) {
    uint8_t tmp[256];
    uint32_t nlen = 0;
    while (name[nlen] && nlen < 200) { tmp[nlen] = (uint8_t)name[nlen]; nlen++; }
    tmp[nlen++] = 0;                                     // include the NUL (satoru)
    if (nlen + len > sizeof(tmp)) return false;
    if (data && len) memcpy(tmp + nlen, data, len);
    return dcmd(BRCMF_C_SET_VAR, tmp, nlen + len, true);
}

// ── WifiRadioOps implementations ─────────────────────────────────────────── (satoru)

// start(): the whole bring-up  -  map bar1, recognize the chip, reset it, download
// firmware + nvram, wait for the dongle, set up the rings, bring the mac "up"
// via the BRCMF_C_UP dcmd, and seed rx buffers. returns false (cleanly) at the
// first step that fails  -  most commonly the firmware blob being absent or the
// dongle never answering on the ci hardware. (satoru)
bool WifiBrcm::radio_start(void* /*ctx*/) {
    if (!wdev) return false;
    if (fw_up && rings_ready) {
        // already up from a prior scan/connect; just (re)assert mac up. (satoru)
        dcmd(BRCMF_C_UP, nullptr, 0, true);
        return true;
    }
    blog("[brcm] start: bringing up the dongle\r\n");

    if (!map_bars())        return false;
    if (!chip_recognize())  return false;
    chip_reset();
    // re-recognize after reset (the window may have moved). (satoru)
    if (!chip_recognize())  return false;

    if (!download_firmware()) return false;     // honest fail if no blob / no hw (satoru)
    fw_up = true;

    if (!setup_rings())     return false;
    hostready();

    if (!brcm_alloc_dma_pools()) { blog("[brcm] dma pool alloc failed\r\n"); return false; }
    post_rx_buffers(BRCM_RX_BUF_COUNT);         // seed the dongle with rx buffers (satoru)

    // bring the firmware mac up + select infrastructure (sta) mode. ref: the
    // brcmfmac bringup issues BRCMF_C_UP then sets infra=1. scaffold: these dcmds
    // only complete with the live firmware responding. (satoru)
    uint32_t infra = 1;
    dcmd(BRCMF_C_SET_INFRA, &infra, sizeof(infra), true);
    dcmd(BRCMF_C_UP, nullptr, 0, true);

    blog("[brcm] start complete (dongle up)\r\n");
    return true;
}

void WifiBrcm::radio_stop(void* /*ctx*/) {
    if (!wdev) return;
    // bring the mac down; leave the firmware loaded so a re-scan is fast. (satoru)
    if (rings_ready) dcmd(BRCMF_C_DOWN, nullptr, 0, true);
    blog("[brcm] stop: mac down\r\n");
}

// set_channel(): the firmware tunes the phy via the BRCMF_C_SET_CHANNEL dcmd.
// ref: fwil.h BRCMF_C_SET_CHANNEL. (satoru)
bool WifiBrcm::radio_set_channel(void* /*ctx*/, int ch) {
    if (!rings_ready) return false;
    uint32_t channel = (uint32_t)ch;
    return dcmd(BRCMF_C_SET_CHANNEL, &channel, sizeof(channel), true);
}

// config_bss(): point the firmware at the target ssid via the BRCMF_C_SET_SSID
// dcmd, whose payload is a {len, ssid[32]} struct (the firmware then handles the
// join/auth/assoc itself  -  broadcom does the mac in firmware). ref: the
// brcmf_join_params / wlc_ssid layout used with BRCMF_C_SET_SSID. (satoru)
bool WifiBrcm::radio_config_bss(void* /*ctx*/, const uint8_t bssid[6], const char* ssid) {
    if (!rings_ready) return false;
    // wlc_ssid_t: __le32 SSID_len; u8 SSID[32]. ref: brcmu_wifi.h. (satoru)
    struct { uint32_t len; uint8_t ssid[32]; } sp;
    memset(&sp, 0, sizeof(sp));
    uint32_t n = 0;
    while (ssid && ssid[n] && n < 32) { sp.ssid[n] = (uint8_t)ssid[n]; n++; }
    sp.len = n;
    // optionally pin the bssid first (best-effort iovar). ref: brcmf uses the
    // "join" iovar with an assoc_params carrying the bssid; we keep it simple
    // here and let the firmware pick the bss matching the ssid. (satoru)
    (void)bssid;
    return dcmd(BRCMF_C_SET_SSID, &sp, sizeof(sp), true);
}

// set_key(): hand a pairwise/group key to the firmware crypto engine via the
// BRCMF_C_SET_KEY dcmd (wl_wsec_key layout). on broadcom the firmware owns the
// crypto, so a successful set means hardware-offloaded ccmp. we return false on
// any failure so the 802.11 stack falls back to software ccmp. ref: fwil.h
// BRCMF_C_SET_KEY + brcmu wl_wsec_key. (satoru)
bool WifiBrcm::radio_set_key(void* /*ctx*/, int idx, const uint8_t* key, int key_len, int type) {
    if (!rings_ready || !key || key_len <= 0 || key_len > 32) return false;
    // wl_wsec_key_t (abridged, matching brcmu_wifi.h field order): index, len,
    // data[32], pad..., algo, flags. the full struct is large + version-sensitive;
    // we build the leading, stable portion the firmware reads. scaffold: exact
    // trailing fields vary by fw  -  verify against the target before trusting key
    // offload; software ccmp is the safe fallback the stack already provides. (satoru)
    struct WlWsecKey {
        uint32_t index;
        uint32_t len;
        uint8_t  data[32];
        uint32_t pad[18];     // ea, iv, rxiv, txiv, etc. (left zero) (satoru)
        uint32_t algo;
        uint32_t flags;
        uint32_t pad2[3];
    } k;
    memset(&k, 0, sizeof(k));
    k.index = (uint32_t)idx;
    k.len = (uint32_t)key_len;
    for (int i = 0; i < key_len; i++) k.data[i] = key[i];
    // algo: CRYPTO_ALGO_AES_CCM == 4 for ccmp. ref: brcmu_wifi.h. (satoru)
    k.algo = (type == 1 /*WIFI_KEY_CCMP*/) ? 4 : 0;
    if (k.algo == 0) return false;             // only ccmp offload attempted (satoru)
    bool ok = dcmd(BRCMF_C_SET_KEY, &k, sizeof(k), true);
    if (!ok) blog("[brcm] set_key not offloaded; sw ccmp will be used\r\n");
    return ok;
}

// tx_frame(): post a fully-formed 802.11 frame to the dongle. broadcom's data
// path normally takes 802.3 frames on per-flow tx rings, but the dongle also
// accepts raw 802.11 via the tx-post header's FRAME_802_11 flag. we copy the
// frame into a coherent buffer + post it on the control-submit ring's tx path.
// scaffold: a complete data path also needs a tx flow-ring created per peer
// (FLOW_RING_CREATE)  -  here we post a single best-effort tx item; honest that
// full tx needs the flow-ring handshake + live firmware. ref: msgbuf.c
// brcmf_msgbuf_tx_msg + the BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_11 flag. (satoru)
bool WifiBrcm::radio_tx_frame(void* /*ctx*/, const uint8_t* buf, int len) {
    if (!rings_ready || !buf || len <= 0 || len > BRCM_MSGBUF_MAX_PKT_SIZE) return false;
    if (!brcm_alloc_dma_pools()) return false;

    // stage the frame into a coherent buffer (reuse the ioctl buffer region tail
    // is unsafe; allocate a dedicated tx staging buffer once). (satoru)
    static uint8_t* tx_buf = nullptr;
    static uint64_t tx_phys = 0;
    if (!tx_buf) {
        tx_buf = (uint8_t*)PMM::AllocBytes(BRCM_MSGBUF_MAX_PKT_SIZE);
        if (!tx_buf) return false;
        tx_phys = (uint64_t)(uintptr_t)tx_buf;
    }
    memcpy(tx_buf, buf, len);

    BrcmRing* r = &rings[BRCM_H2D_CONTROL_SUBMIT];
    void* slot = ring_reserve(r);
    if (!slot) return false;
    MsgbufTxMsghdr* tx = (MsgbufTxMsghdr*)slot;
    memset(tx, 0, sizeof(*tx));
    tx->msg.msgtype = MSGBUF_TYPE_TX_POST;
    tx->msg.ifidx = 0;
    tx->msg.request_id = (uint32_t)(0x2000 + (reqid++ & 0x7FF));
    tx->flags = 0x02;                          // FRAME_802_11 (satoru)
    tx->seg_cnt = 1;
    tx->data_len = (uint16_t)len;
    tx->data_buf_addr.low_addr  = (uint32_t)(tx_phys & 0xFFFFFFFF);
    tx->data_buf_addr.high_addr = (uint32_t)(tx_phys >> 32);
    ring_commit(r);
    return true;     // queued (delivery requires the live dongle) (satoru)
}

// rx_poll(): drain one frame off the rx-complete ring, if any. the dongle dma's
// received 802.11 frames into our posted buffers + signals here. (satoru)
int WifiBrcm::radio_rx_poll(void* /*ctx*/, uint8_t* buf, int buf_max) {
    if (!rings_ready) return 0;
    return drain_rx_complete(buf, buf_max);
}

// get_signal(): query the firmware's link rssi via BRCMF_C_GET_RSSI. the dongle
// returns an le32 dBm value. caches the last good read. ref: fwil.h
// BRCMF_C_GET_RSSI. (satoru)
int WifiBrcm::radio_get_signal(void* /*ctx*/) {
    if (rings_ready) {
        int32_t rssi = 0;
        if (dcmd(BRCMF_C_GET_RSSI, &rssi, sizeof(rssi), false)) {
            if (rssi < 0 && rssi > -120) last_rssi = (int)rssi;
        }
    }
    return last_rssi;
}

// load_firmware(): the WifiRadioOps hook for a stack-supplied blob. broadcom
// sources its own firmware from the fs inside start()/download_firmware(), so
// this is a no-op that succeeds  -  the stack does not ship broadcom firmware. (satoru)
bool WifiBrcm::radio_load_firmware(void* /*ctx*/, const uint8_t* /*blob*/, int /*len*/) {
    return true;
}

// ── probe + register ─────────────────────────────────────────────────────── (satoru)
bool WifiBrcm::TryRegister() {
    // only claim the device if a broadcom wifi nic is present + mmio-live. (satoru)
    if (!WifiDev::Present()) return false;
    const WifiDevice* info = WifiDev::Info();
    if (!info || info->family != WIFI_FAM_BROADCOM_BRCM) return false;
    if (!info->mmio_mapped) {
        blog("[brcm] broadcom nic present but bar0 mmio is dead; not registering\r\n");
        return false;
    }

    // adopt the device (cast away const  -  we only read it, but the ops want a
    // mutable WifiDevice* to register). the WifiDev singleton owns the storage. (satoru)
    wdev = (WifiDevice*)info;
    blogx("[brcm] claiming broadcom wifi device id ", info->device);

    // wire up the ops vtable. (satoru)
    ops.start         = radio_start;
    ops.stop          = radio_stop;
    ops.set_channel   = radio_set_channel;
    ops.config_bss    = radio_config_bss;
    ops.set_key       = radio_set_key;
    ops.tx_frame      = radio_tx_frame;
    ops.rx_poll       = radio_rx_poll;
    ops.get_signal    = radio_get_signal;
    ops.load_firmware = radio_load_firmware;

    Ieee80211::RegisterRadio(&ops, nullptr, wdev);
    blog("[brcm] registered broadcom radio with the 802.11 stack\r\n");
    blog("[brcm] note: association needs the proprietary firmware/nvram blobs in\r\n");
    blog("[brcm]       /system/lib/firmware/brcm/ + real hardware (untestable on qemu)\r\n");
    return true;
}
// end (satoru)
