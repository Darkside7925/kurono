#pragma once
//  kurono os  -  realtek rtw pcie wifi radio driver (satoru)
//
//  implements the Ieee80211::WifiRadioOps contract for realtek pcie wlan parts
//  in the rtl88xx/rtl87xx family: rtl8821ce, rtl8723be, rtl8822ce, rtl8812ae.
//  this is the *radio* layer  -  the 802.11 mac (scan/auth/assoc) and the wpa2
//  supplicant live above us in net/ieee80211; we only touch the chip: the mac
//  power-on sequence, llt/fifo init, firmware download, the pci tx/rx dma rings,
//  the phy channel set, and the bssid/rx-filter regs. (satoru)
//
//  layering (see net/ieee80211.h):
//    net/ieee80211   protocol + supplicant
//        │  WifiRadioOps vtable  <-- this file fills it
//    drivers/wifi_rtw  (this) realtek hardware bring-up
//        │  drivers/wifi_dev RegRead/RegWrite + the mapped mmio window
//    hardware (pci mmio)
//
//  hardware reference: linux drivers/net/wireless/realtek/rtw88 (mac.c, pci.c,
//  fw.c, phy.c, reg.h) for the rtl8821c/8822c parts, and rtlwifi for the older
//  rtl8723be/rtl8812ae. all register defs + sequences below are re-derived from
//  the public datasheet register map and the linux drivers; the code is original
//  kurono (no gpl source paste). individual sequences cite their ref. (satoru)
//
//  HONESTY: qemu emulates no realtek wifi nic, and the firmware blob is NOT in
//  the kurono tree (realtek firmware is redistributable but large/binary). this
//  is a correct, complete-as-possible bring-up + ops implementation that a
//  maintainer finishes against real silicon. start() fails cleanly when the
//  firmware file is absent or the mmio window is dead, so nothing pretends to
//  associate without hardware. sequences i could not fully verify against a
//  datasheet are marked "UNSURE". (satoru)

#include "../kernel/types.h"

struct WifiDevice;   // fwd (drivers/wifi_dev.h) (satoru)

namespace WifiRtw {

// firmware search directory on the kurono fs. realtek ships per-chip blobs
// (e.g. rtw8821c_fw.bin / rtl8821cefw.bin); the maintainer drops the right one
// here. we try a small list of conventional names per chip. (satoru)
#define WIFI_RTW_FW_DIR  "/system/lib/firmware/"

// probe for a realtek rtw nic and, if present, bring it up and register the
// radio with Ieee80211. returns false (does nothing) unless WifiDev::Present()
// and the family is WIFI_FAM_REALTEK_RTW. safe to call once at boot/net-init;
// idempotent (a second call after a successful register is a no-op). (satoru)
bool TryRegister();

// true once TryRegister() has successfully registered the radio. (satoru)
bool Registered();

} // namespace WifiRtw
// end (satoru)
