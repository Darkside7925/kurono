#pragma once
//  kurono os  -  broadcom brcmfmac pcie "full-dongle" wifi radio driver (satoru)
//
//  this implements the Ieee80211::WifiRadioOps contract for broadcom pcie wifi
//  chips (bcm4356 / bcm4366 / bcm43602 / bcm4350 / bcm4364 / bcm4378  -  the
//  "full-dongle" parts). broadcom is unlike intel/atheros/realtek: there is no
//  on-host mac. the chip runs a proprietary arm firmware ("the dongle") and the
//  host talks to it ONLY over message rings in host-coherent memory plus a small
//  set of doorbell/mailbox registers. so this driver is, in order: (satoru)
//
//    1. backplane bring-up  -  reach the chipcommon + arm cores through the
//       windowed bar0 (the brcmf BAR0_WINDOW config register), read the chip id,
//       and watchdog-reset the device out of any prior state. (satoru)
//    2. firmware download  -  copy the firmware blob + an nvram blob into the
//       device's on-chip ram, which is directly mapped by bar1 ("tcm"), then
//       release the arm core and wait for the dongle to publish its shared-ram
//       address (the "fw is up" handshake). blobs come from the kurono fs at
//       /system/lib/firmware/brcm/. start() fails cleanly if they are absent. (satoru)
//    3. msgbuf rings  -  parse the dongle's ring-info out of tcm, allocate the five
//       common rings (h2d control-submit + rxpost-submit, d2h control-complete +
//       tx-complete + rx-complete) in host coherent memory, hand their addresses
//       to the dongle, and ring the hostready doorbell. (satoru)
//    4. dcmd / iovars  -  every control op (channel, join, key, rssi) is a firmware
//       command (dcmd) marshalled through the control-submit ring with the reply
//       drained from the control-complete ring. (satoru)
//
//  HONESTY: a broadcom part cannot associate without its proprietary firmware +
//  nvram, and qemu emulates no such device, so this code is UNTESTABLE on the
//  ci hardware. the backplane addressing, the download sequence, the ring layout
//  and the dcmd framing are implemented faithfully to the linux driver so that on
//  real hardware with the blobs present they would drive the dongle; where a step
//  fundamentally needs the live firmware to respond (ring create-complete, dcmd
//  reply, rx-complete) the code is honest scaffold that posts the request and
//  drains replies but will time out against absent hardware. each such spot is
//  marked "scaffold:". (satoru)
//
//  ref: linux drivers/net/wireless/broadcom/brcm80211/brcmfmac  -  pcie.c (the
//  full-dongle bus + backplane + fw download), chip.c (the soc backplane / chip
//  id), commonring.c + msgbuf.c (the ring protocol + dcmd), include/brcm_hw_ids.h
//  (the device table). code below is original kurono; no gpl text copied. (satoru)

#include "../kernel/types.h"

struct WifiDevice;            // fwd (drivers/wifi_dev.h) (satoru)

namespace Ieee80211 { struct WifiRadioOps; }

// the five brcmf common message rings, in the dongle's fixed index order. the
// first two are host->device (submit), the last three device->host (complete).
// ref: linux brcmfmac bus.h (BRCMF_{H2D,D2H}_MSGRING_*). (satoru)
enum BrcmRingId {
    BRCM_H2D_CONTROL_SUBMIT = 0,   // dcmd/ioctl requests to the firmware (satoru)
    BRCM_H2D_RXPOST_SUBMIT  = 1,   // host hands the dongle empty rx buffers (satoru)
    BRCM_D2H_CONTROL_COMPLETE = 2, // dcmd/ioctl replies + wl events (satoru)
    BRCM_D2H_TX_COMPLETE    = 3,   // tx-done notifications (satoru)
    BRCM_D2H_RX_COMPLETE    = 4,   // received frames land here (satoru)
    BRCM_NROF_COMMON_RINGS  = 5
};

// one host-side common ring: a producer/consumer ring of fixed-size items living
// in host coherent (dma-able, identity-mapped) memory. the read/write indices it
// shares with the dongle live in the tcm (we use the "tcm indices" mode, which
// needs no host index buffer). ref: linux brcmfmac commonring.c. (satoru)
struct BrcmRing {
    volatile uint8_t* buf;     // host coherent ring buffer (item_len*depth) (satoru)
    uint64_t buf_phys;         // its physical (== virtual, identity-mapped) addr (satoru)
    uint16_t depth;            // number of items (satoru)
    uint16_t item_len;         // bytes per item (satoru)
    uint16_t w_ptr;            // our cached write index (producer side) (satoru)
    uint16_t r_ptr;            // our cached read index (consumer side) (satoru)
    uint32_t w_idx_tcm;        // tcm address holding the shared write index (satoru)
    uint32_t r_idx_tcm;        // tcm address holding the shared read index (satoru)
    bool     is_h2d;           // host->device (we produce) vs d2h (we consume) (satoru)
};

class WifiBrcm {
public:
    // probe + register. returns false unless a broadcom wifi nic is present and
    // mmio-live; otherwise initialises this driver's state and registers its
    // WifiRadioOps with the 802.11 stack, returning true. (satoru)
    static bool TryRegister();

private:
    // ── the WifiRadioOps vtable thunks (static so they match the c fn-ptr abi) ──
    static bool radio_start(void* ctx);
    static void radio_stop(void* ctx);
    static bool radio_set_channel(void* ctx, int ch);
    static bool radio_config_bss(void* ctx, const uint8_t bssid[6], const char* ssid);
    static bool radio_set_key(void* ctx, int idx, const uint8_t* key, int key_len, int type);
    static bool radio_tx_frame(void* ctx, const uint8_t* buf, int len);
    static int  radio_rx_poll(void* ctx, uint8_t* buf, int buf_max);
    static int  radio_get_signal(void* ctx);
    static bool radio_load_firmware(void* ctx, const uint8_t* blob, int len);

    // ── backplane access (bar0 windowed register window) ─────────────────────
    // set the bar0 sliding window to a backplane address, so bar0 offset 0..0xfff
    // maps the targeted core's registers. ref: pcie.c brcmf_pcie_select_core. (satoru)
    static void backplane_window(uint32_t addr);
    static uint32_t reg_read(uint32_t off);             // bar0 register read (satoru)
    static void     reg_write(uint32_t off, uint32_t v);// bar0 register write (satoru)

    // ── tcm access (bar1 directly maps the dongle's on-chip ram) ─────────────
    static uint32_t tcm_r32(uint32_t addr);
    static void     tcm_w32(uint32_t addr, uint32_t v);
    static uint16_t tcm_r16(uint32_t addr);
    static void     tcm_w16(uint32_t addr, uint16_t v);
    static void     tcm_write(uint32_t addr, const void* src, uint32_t len);
    static void     tcm_read(uint32_t addr, void* dst, uint32_t len);

    // ── bring-up phases ──────────────────────────────────────────────────────
    static bool map_bars();                  // map bar0 (regs) + bar1 (tcm) (satoru)
    static bool chip_recognize();            // read the chipcommon chip id (satoru)
    static void chip_reset();                // watchdog-reset the device (satoru)
    static bool download_firmware();         // fw + nvram -> tcm, start the arm (satoru)
    static bool init_shared_ram(uint32_t sharedram_addr); // parse the shared block (satoru)
    static bool setup_rings();               // allocate + register the 5 rings (satoru)
    static void hostready();                 // ring the hostready doorbell (satoru)

    // ── ring primitives (commonring.c-equivalent) ────────────────────────────
    static void* ring_reserve(BrcmRing* r);  // reserve one item to write, or null (satoru)
    static void  ring_commit(BrcmRing* r);   // publish writes + ring the doorbell (satoru)
    static void* ring_peek(BrcmRing* r);     // next item to read, or null (satoru)
    static void  ring_consume(BrcmRing* r);  // advance the read index (satoru)
    static void  ring_doorbell();            // poke the h2d mailbox (satoru)

    // ── firmware command interface (dcmd + iovar) ────────────────────────────
    // issue a firmware command. cmd is a BRCMF_C_* number; buf/len is the in/out
    // payload. blocks (cooperatively) for the control-complete reply up to a
    // deadline. returns true if the dongle acked. (satoru)
    static bool dcmd(uint32_t cmd, void* buf, uint32_t len, bool set);
    // an iovar is a named variable set/get framed as a dcmd over BRCMF_C_*_VAR.
    static bool iovar_set(const char* name, const void* data, uint32_t len);

    // drain the d2h control-complete ring once, looking for a dcmd reply matching
    // the in-flight transaction id; copy the reply payload back. (satoru)
    static bool drain_control_complete(uint32_t want_xid, void* out, uint32_t out_max,
                                       uint32_t* out_len);
    // drain the d2h rx-complete ring once; if a frame is ready copy it out. (satoru)
    static int  drain_rx_complete(uint8_t* out, int out_max);
    // hand the dongle a batch of empty rx buffers via the rxpost ring. (satoru)
    static void post_rx_buffers(int count);

    // load a firmware blob file from the kurono fs into a freshly allocated
    // buffer. returns the buffer (caller frees) + length, or null. (satoru)
    static uint8_t* fs_load(const char* path, uint32_t* out_len);

    // ── driver state (single radio) ──────────────────────────────────────────
    static WifiDevice* wdev;          // the probed pci device (satoru)
    static volatile uint8_t* tcm;     // bar1: dongle on-chip ram window (satoru)
    static uint64_t tcm_phys;
    static uint32_t tcm_size;
    static uint32_t chip_id;          // e.g. 0x4356 (satoru)
    static uint32_t chip_rev;
    static uint32_t rambase;          // start of the dongle code ram in tcm (satoru)
    static uint32_t ramsize;
    static bool     is_cr4;           // arm-cr4 core vs cm3/ca7 (affects reset) (satoru)
    static bool     fw_up;            // the dongle published its shared-ram addr (satoru)
    static uint32_t shared_addr;      // tcm address of the dongle's shared block (satoru)
    static uint32_t shared_flags;
    static uint8_t  shared_version;
    static uint32_t ring_info_addr;   // tcm address of the ring-info descriptor (satoru)
    static uint32_t h2d_mailbox_0;    // doorbell register (windowed pcie2 core) (satoru)
    static uint32_t h2d_mailbox_1;    // hostready doorbell (satoru)
    static BrcmRing rings[BRCM_NROF_COMMON_RINGS];
    static bool     rings_ready;
    static uint16_t reqid;            // rolling dcmd transaction id (satoru)
    static int      last_rssi;        // cached link rssi from BRCMF_C_GET_RSSI (satoru)

    // the registered ops table + this driver's singleton context. (satoru)
    static Ieee80211::WifiRadioOps ops;
};
// end (satoru)
