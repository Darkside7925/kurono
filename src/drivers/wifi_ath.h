#pragma once
//  kurono os - atheros / qualcomm wifi radio driver (ath9k + ath10k) (satoru)
//
//  this is the per-vendor radio driver for atheros (qualcomm) wireless nics. it
//  implements the WifiRadioOps vtable from net/ieee80211.h and registers it via
//  Ieee80211::RegisterRadio, so the software 802.11 mac (scan/auth/assoc + wpa2)
//  drives this hardware. it talks to the chip through drivers/wifi_dev
//  (RegRead/RegWrite over the mapped pci bar). (satoru)
//
//  two chip families are handled (see WifiDev::Info()->family):
//    WIFI_FAM_ATHEROS_ATH9K  - ar9xxx, on-die mac, FIRMWARE-FREE. this is the
//      tractable one and is implemented in full: chip reset (AR_RTC_RC), pll +
//      clock, the init register vectors, eeprom/otp mac read, descriptor-based
//      tx/rx dma rings in identity-mapped coherent memory, channel synth, the
//      bss filters, and rssi from the rx descriptor. (satoru)
//    WIFI_FAM_ATHEROS_ATH10K - qca988x / qca6174, FIRMWARE-BASED. these use the
//      copy engine (ce) + a firmware blob + the wmi/htc protocol. implemented as
//      a bring-up scaffold: pci/ce init, firmware load from the kurono fs, and
//      the wmi command-path skeleton. full wmi assoc needs real hardware. (satoru)
//
//  ref: linux drivers/net/wireless/ath/ath9k (hw.c, mac.c, recv.c, xmit.c,
//  reg.h, ar9003_*) and ath/ath10k (pci.c, ce.c, bmi.c, wmi.c). all code here is
//  original kurono - no gpl paste; the register numbers + sequences are cited to
//  their ath9k/ath10k source file. (satoru)

#include "../kernel/types.h"

namespace WifiAth {

// probe the device family, init the correct path (ath9k or ath10k), and register
// the radio with the 802.11 stack. returns false unless WifiDev::Present() and
// the family is ATH9K or ATH10K; otherwise brings the radio up and returns true.
// this is the single entry point the kernel/net layer calls to light up an
// atheros card. (satoru)
bool TryRegister();

// true once TryRegister() succeeded and the ops are live. (satoru)
bool IsActive();

}  // namespace WifiAth
// end (satoru)
