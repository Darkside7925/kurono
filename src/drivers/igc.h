#pragma once
//  intel igc (i225/i226 2.5g) and igb (82576) ethernet driver for kurono os.
//  ported from the linux igc/igb drivers (drivers/net/ethernet/intel/igc, igb).
//  one shared advanced-descriptor single-queue tx/rx data path; the reset and
//  phy branch differs per device family. polled (no interrupts), like e1000.
//  registered with kdf as "igc" so it can be stopped and re-initialised
//  without a reboot (`nic restart`). nvm/eeprom/flash is NEVER written. (satoru)

#include "../kernel/types.h"

// received packet callback (same shape as E1000_PacketHandler) (satoru)
typedef void (*Igc_PacketHandler)(const uint8_t* data, uint16_t length);

class Igc {
public:
    static bool Init();           // scan pci, bring up under kdf, program rings
    static bool IsDetected();     // bound to an i225/i226/82576?
    static bool IsLinkUp();       // STATUS.LU, read live every call
    static void GetMAC(uint8_t mac[6]);
    static bool Send(const uint8_t* data, uint16_t length);
    static void Poll();           // drain rx, reclaim tx, track link changes
    static void SetPacketHandler(Igc_PacketHandler handler);

    static uint32_t GetTxCount();
    static uint32_t GetRxCount();
    static uint32_t GetTxBytes();
    static uint32_t GetRxBytes();
    static uint32_t GetRxMissed();

    // stop the rings, reset the mac and bring everything back up again via kdf,
    // keeping the mmio/dma mappings and the packet handler. (satoru)
    static bool Restart();
    static int  Status(char* out, int mx);          // human readable summary (satoru)
    static void RegisterShellCommands();            // `nic` command (satoru)

    static bool KdfInit();        // kdf init entry (public so it can be a plain fn ptr) (satoru)
};
// end (satoru)
