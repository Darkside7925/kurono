#pragma once
#include "../kernel/types.h"

// ahci / sata block driver. (satoru)
//
// this is the gap that made kurono unusable as a rescue environment: every disk
// path in the tree went through NVMe::, so a machine with a sata ssd or a
// spinning disk enumerated zero disks. the gpt/mbr parser in system/installer.cpp
// was always fine - it just had nothing but nvme to look at.
//
// registers each attached port with BlockDev, so the existing partition scanner,
// the ext4/fat32 mounts (which already take a part_offset), and anything else
// built on the registry light up on sata with no change to them. (satoru)

constexpr int AHCI_MAX_PORTS = 32;

struct AhciPortInfo {
    bool     present;
    int      port;              // hba port index (satoru)
    uint64_t sectors;
    uint32_t sector_size;
    char     model[48];         // from ata identify, byte-swapped (satoru)
    char     serial[24];
};

class Ahci {
public:
    // probe pci for an ahci controller (class 01:06:01), map its abar, reset it,
    // and bring up every port with a device attached. registers each with
    // BlockDev. safe to call twice. returns true if at least one disk came up.
    // (satoru)
    static bool Init();

    // the real init body. must run inside KDF::Start's guarded context, because
    // KDF::MapMMIO attributes the mapping to the *active* driver and returns
    // null when there isn't one - calling it straight from Init() fails with
    // "MapMMIO(abar) failed" even though the pci probe found the controller.
    // public only so KDF::RegisterDriver can take its address. (satoru)
    static bool KdfInit();

    static bool IsDetected();
    static int  GetPortCount();
    static AhciPortInfo* GetPort(int idx);

    // lba read/write. `count` is in sectors. these block until the command
    // completes or the port reports an error; there is no irq path yet, which is
    // fine for a rescue tool but would need revisiting for throughput. (satoru)
    static bool Read(int port, uint64_t lba, uint32_t count, void* buf);
    static bool Write(int port, uint64_t lba, uint32_t count, const void* buf);
    static bool Flush(int port);

private:
    static bool ProbePci(uint64_t* abar_out, uint8_t* bus, uint8_t* dev, uint8_t* fn);
    static bool PortReset(int port);
    static bool PortIdentify(int port, AhciPortInfo* info);
    static int  FindFreeSlot(int port);
    static bool RunCommand(int port, int slot);

    static volatile uint8_t* s_abar;
    static AhciPortInfo      s_ports[AHCI_MAX_PORTS];
    static int               s_port_count;
    static bool              s_detected;
};

// end (satoru)
