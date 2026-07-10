#pragma once
//  kurono os - intel iwlwifi wifi radio driver (the WifiRadioOps backend) (satoru)
//
//  this is the chip-specific radio layer for intel wireless nics: ax200/ax201/
//  ax210, wireless-ac 9560/9260/8265/8260/7265/7260, and 3165/3168. it implements
//  the WifiRadioOps vtable that net/ieee80211 (the software 802.11 mac) calls, and
//  registers itself via Ieee80211::RegisterRadio. it does the hardware: apm power-
//  up, the csr reset/init handshake, the ucode (firmware) load over the pcie
//  transport, and the tfd/rbd dma rings. the 802.11 mac/scan/auth/assoc/wpa2 live
//  ABOVE this in ieee80211; this file owns only register/dma i/o. (satoru)
//
//  layering (this is the bottom-most software layer):
//    net/ieee80211   protocol + supplicant
//        │  WifiRadioOps (this driver fills it)
//    drivers/wifi_iwl  ← THIS - intel radio bring-up + tx/rx queues
//        │  WifiDev::RegRead/RegWrite (mapped pci mmio bar)
//    hardware (intel wifi nic)
//
//  ref: linux drivers/net/wireless/intel/iwlwifi - pcie/gen1_2/trans.c (apm + nic
//  init), iwl-csr.h (csr register defs), iwl-fh.h (flow-handler dma regs), iwl-
//  prph.h (apmg power-management prph regs), fw/file.h (the .ucode tlv format),
//  iwl-io.c (activate_nic / grab-nic-access). all code here is original kurono;
//  the linux source is cross-referenced for the HARDWARE behaviour only. (satoru)
//
//  HONESTY: qemu emulates no intel wifi nic and the iwlwifi-*.ucode blob is not
//  shipped in-tree, so this is a correct, complete-as-possible bring-up that the
//  maintainer finishes on real hardware. it does NOT claim to associate. any
//  sequence that needs real silicon to confirm is marked "// UNVERIFIED" below.
//  (satoru)

#include "../kernel/types.h"

struct WifiDevice;   // fwd (drivers/wifi_dev.h) (satoru)

class WifiIwl {
public:
    // probe + bring up the intel radio and register it with the 802.11 stack.
    // returns false unless WifiDev::Present() && family == WIFI_FAM_INTEL_IWLWIFI;
    // on a match it reads device id/rev, wires this driver's WifiRadioOps, and
    // calls Ieee80211::RegisterRadio. the heavy lifting (apm init, ucode load)
    // happens lazily inside the start() op the first time a scan/connect runs, so
    // registration is cheap + side-effect-light. (satoru)
    static bool TryRegister();

    // is this driver the registered radio? (satoru)
    static bool Active();

private:
    // ── the WifiRadioOps the stack calls (all take our ctx) ──────────────── (satoru)
    static bool op_start(void* ctx);
    static void op_stop(void* ctx);
    static bool op_set_channel(void* ctx, int ch);
    static bool op_config_bss(void* ctx, const uint8_t bssid[6], const char* ssid);
    static bool op_set_key(void* ctx, int idx, const uint8_t* key, int key_len, int type);
    static bool op_tx_frame(void* ctx, const uint8_t* buf, int len);
    static int  op_rx_poll(void* ctx, uint8_t* buf, int buf_max);
    static int  op_get_signal(void* ctx);
    static bool op_load_firmware(void* ctx, const uint8_t* blob, int len);
};
// end (satoru)
