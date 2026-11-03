#include "ahci.h"
#include "blockdev.h"
#include "serial.h"
#include "nvme.h"   // shared bare-metal write guard switch (satoru)
#include "../kernel/pci.h"
#include "../kernel/pmm.h"
#include "../kernel/kdf.h"
#include "../hal/hal.h"

// ahci / sata block driver - see ahci.h for why. (satoru)

namespace {

// ── hba global registers (abar + offset) ────────────────────────────────────
constexpr uint32_t HBA_CAP     = 0x00;
constexpr uint32_t HBA_GHC     = 0x04;
constexpr uint32_t HBA_IS      = 0x08;
constexpr uint32_t HBA_PI      = 0x0C;   // ports implemented bitmap (satoru)
constexpr uint32_t HBA_VS      = 0x10;

constexpr uint32_t GHC_HR      = (1u << 0);    // hba reset (satoru)
constexpr uint32_t GHC_AE      = (1u << 31);   // ahci enable (satoru)

// ── per-port registers (abar + 0x100 + port*0x80 + offset) ──────────────────
constexpr uint32_t PORT_BASE   = 0x100;
constexpr uint32_t PORT_STRIDE = 0x80;
constexpr uint32_t P_CLB       = 0x00;
constexpr uint32_t P_CLBU      = 0x04;
constexpr uint32_t P_FB        = 0x08;
constexpr uint32_t P_FBU       = 0x0C;
constexpr uint32_t P_IS        = 0x10;
constexpr uint32_t P_IE        = 0x14;
constexpr uint32_t P_CMD       = 0x18;
constexpr uint32_t P_TFD       = 0x20;
constexpr uint32_t P_SIG       = 0x24;
constexpr uint32_t P_SSTS      = 0x28;
constexpr uint32_t P_SCTL      = 0x2C;
constexpr uint32_t P_SERR      = 0x30;
constexpr uint32_t P_CI        = 0x38;

constexpr uint32_t CMD_ST      = (1u << 0);    // start (satoru)
constexpr uint32_t CMD_FRE     = (1u << 4);    // fis receive enable (satoru)
constexpr uint32_t CMD_FR      = (1u << 14);   // fis receive running (satoru)
constexpr uint32_t CMD_CR      = (1u << 15);   // command list running (satoru)

constexpr uint32_t TFD_BSY     = 0x80;
constexpr uint32_t TFD_DRQ     = 0x08;
constexpr uint32_t TFD_ERR     = 0x01;

// port signatures (satoru)
constexpr uint32_t SIG_ATA     = 0x00000101;

// ata commands (satoru)
constexpr uint8_t ATA_IDENTIFY       = 0xEC;
constexpr uint8_t ATA_READ_DMA_EXT   = 0x25;
constexpr uint8_t ATA_WRITE_DMA_EXT  = 0x35;
constexpr uint8_t ATA_FLUSH_EXT      = 0xEA;

// one command header, 32 bytes. (satoru)
struct CmdHeader {
    uint16_t flags;      // cfl:5 a:1 w:1 p:1 r:1 b:1 c:1 rsv:1 pmp:4 (satoru)
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed));

struct PrdtEntry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc;        // bits 21:0 = byte count - 1; bit 31 = interrupt (satoru)
} __attribute__((packed));

// command table: 64-byte cfis, 16-byte atapi, 48 reserved, then prdt. (satoru)
struct CmdTable {
    uint8_t   cfis[64];
    uint8_t   acmd[16];
    uint8_t   rsv[48];
    PrdtEntry prdt[8];
} __attribute__((packed));

// host-to-device register fis, 20 bytes. (satoru)
struct FisRegH2D {
    uint8_t fis_type;
    uint8_t pmport_c;    // bit 7 = c (command, not control) (satoru)
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth, icc, control;
    uint8_t rsv1[4];
} __attribute__((packed));

// per-port dma structures. the hba dmas into these by physical address, so they
// come from PMM (identity mapped in the low 4gb) rather than the heap. (satoru)
struct PortMem {
    CmdHeader* clist;    // 32 headers, 1kb aligned (satoru)
    uint8_t*   fis;      // 256 bytes, 256 aligned (satoru)
    CmdTable*  ctbl;     // one table per slot, 128 aligned (satoru)
    uint8_t*   bounce;   // 128kb dma staging buffer (satoru)
};
PortMem g_pmem[AHCI_MAX_PORTS] = {};

constexpr uint32_t BOUNCE_BYTES = 128 * 1024;

volatile uint8_t* abar_ptr = nullptr;

inline uint32_t rd(uint32_t off) {
    if (!abar_ptr) return 0;
    return *(volatile uint32_t*)(abar_ptr + off);
}
inline void wr(uint32_t off, uint32_t v) {
    if (!abar_ptr) return;
    *(volatile uint32_t*)(abar_ptr + off) = v;
}
inline uint32_t prd(int port, uint32_t off) {
    return rd(PORT_BASE + (uint32_t)port * PORT_STRIDE + off);
}
inline void pwr(int port, uint32_t off, uint32_t v) {
    wr(PORT_BASE + (uint32_t)port * PORT_STRIDE + off, v);
}

// crude spin delay. the ahci spec's waits are in milliseconds and this runs
// before the scheduler is necessarily up, so busy-wait rather than sleep.
// (satoru)
void spin(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops * 1000u; i++) __asm__ __volatile__("pause");
}

// ata identify returns model/serial as byte-swapped 16-bit words. (satoru)
void ata_string(char* dst, const uint16_t* src, int words, int cap) {
    int n = 0;
    for (int i = 0; i < words && n < cap - 2; i++) {
        dst[n++] = (char)(src[i] >> 8);
        dst[n++] = (char)(src[i] & 0xFF);
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) n--;
    dst[n] = 0;
}

// blockdev trampolines. ctx carries the port index. (satoru)
bool bd_read(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    return Ahci::Read((int)(uintptr_t)ctx, lba, count, buf);
}
bool bd_write(void* ctx, uint64_t lba, uint32_t count, const void* buf) {
    return Ahci::Write((int)(uintptr_t)ctx, lba, count, buf);
}
bool bd_flush(void* ctx) {
    return Ahci::Flush((int)(uintptr_t)ctx);
}

}  // namespace

volatile uint8_t* Ahci::s_abar        = nullptr;
AhciPortInfo      Ahci::s_ports[AHCI_MAX_PORTS] = {};
int               Ahci::s_port_count  = 0;
bool              Ahci::s_detected    = false;

bool Ahci::IsDetected()          { return s_detected; }
int  Ahci::GetPortCount()        { return s_port_count; }
AhciPortInfo* Ahci::GetPort(int i) {
    if (i < 0 || i >= s_port_count) return nullptr;
    return &s_ports[i];
}

bool Ahci::ProbePci(uint64_t* abar_out, uint8_t* bus_out, uint8_t* dev_out, uint8_t* fn_out) {
    // class 0x01 (mass storage), subclass 0x06 (sata), prog-if 0x01 (ahci).
    // buses 0-7 matches what the kld broker scans; ahci controllers live low.
    // (satoru)
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint16_t vend = PCI::GetVendor((uint8_t)bus, dev, fn);
                if (vend == 0xFFFF) continue;
                uint32_t cls = PCI::GetClassCode((uint8_t)bus, dev, fn);
                if (((cls >> 16) & 0xFF) != 0x01) continue;   // mass storage
                if (((cls >> 8)  & 0xFF) != 0x06) continue;   // sata
                if ((cls & 0xFF) != 0x01) continue;           // ahci progif

                // abar is bar5, always a 32-bit memory bar per the spec. (satoru)
                uint32_t bar5 = PCI::GetBAR((uint8_t)bus, dev, fn, 5);
                uint64_t base = (uint64_t)(bar5 & ~0xFu);
                if (!base) continue;

                PCI::EnableBusMaster((uint8_t)bus, dev, fn);
                *abar_out = base;
                *bus_out = (uint8_t)bus; *dev_out = dev; *fn_out = fn;
                return true;
            }
        }
    }
    return false;
}

static int g_ahci_kdf_id = -1;

bool Ahci::Init() {
    if (s_detected) return true;

    if (g_ahci_kdf_id < 0)
        g_ahci_kdf_id = KDF::RegisterDriver("ahci", &Ahci::KdfInit);
    if (g_ahci_kdf_id < 0) {
        // kdf unavailable: run unsandboxed so storage still comes up. MapMMIO
        // will fail without an active driver, so this path is best-effort and
        // says so rather than pretending. (satoru)
        SerialLogger::Log("[ahci] KDF unavailable; running unsandboxed init\r\n");
        return KdfInit();
    }
    // KDF::Start runs KdfInit inside RunGuarded, which is what sets the active
    // driver that MapMMIO needs. (satoru)
    bool ok = KDF::Start(g_ahci_kdf_id);
    return ok && s_detected;
}

bool Ahci::KdfInit() {
    if (s_detected) return true;

    uint64_t abar = 0;
    uint8_t bus = 0, dev = 0, fn = 0;
    if (!ProbePci(&abar, &bus, &dev, &fn)) {
        SerialLogger::Log("[ahci] no controller found\r\n");
        return false;
    }

    // map the hba register window. 0x2000 covers globals plus all 32 ports
    // (0x100 + 32*0x80 = 0x1100), through kdf so an access past it hits a guard
    // page instead of unrelated mmio. (satoru)
    void* win = KDF::MapMMIO(abar, 0x2000ULL);
    if (!win) {
        SerialLogger::Log("[ahci] FATAL: MapMMIO(abar) failed\r\n");
        return false;
    }
    abar_ptr = (volatile uint8_t*)win;
    s_abar   = abar_ptr;

    SerialLogger::Log("[ahci] controller at ");
    SerialLogger::LogDec(bus); SerialLogger::Log(":");
    SerialLogger::LogDec(dev); SerialLogger::Log(".");
    SerialLogger::LogDec(fn);
    SerialLogger::Log(" abar=");
    SerialLogger::LogHex64(abar);
    SerialLogger::Log(" vs=");
    SerialLogger::LogHex(rd(HBA_VS));
    SerialLogger::Log("\r\n");

    // take ownership: set AE, reset, set AE again (the reset clears it). (satoru)
    wr(HBA_GHC, rd(HBA_GHC) | GHC_AE);
    wr(HBA_GHC, rd(HBA_GHC) | GHC_HR);
    for (int i = 0; i < 1000 && (rd(HBA_GHC) & GHC_HR); i++) spin(1);
    if (rd(HBA_GHC) & GHC_HR) {
        SerialLogger::Log("[ahci] FATAL: hba reset did not clear\r\n");
        return false;
    }
    wr(HBA_GHC, rd(HBA_GHC) | GHC_AE);

    uint32_t pi = rd(HBA_PI);
    s_port_count = 0;

    // dump what the hba actually reports before filtering on it. two rounds of
    // guessing why "0 disks" came back cost more than this line would have.
    // (satoru)
    SerialLogger::Log("[ahci] cap=");
    SerialLogger::LogHex(rd(HBA_CAP));
    SerialLogger::Log(" pi=");
    SerialLogger::LogHex(pi);
    SerialLogger::Log(" ghc=");
    SerialLogger::LogHex(rd(HBA_GHC));
    SerialLogger::Log("\r\n");
    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;
        SerialLogger::Log("[ahci]  port ");
        SerialLogger::LogDec(p);
        SerialLogger::Log(" ssts=");
        SerialLogger::LogHex(prd(p, P_SSTS));
        SerialLogger::Log(" sig=");
        SerialLogger::LogHex(prd(p, P_SIG));
        SerialLogger::Log(" tfd=");
        SerialLogger::LogHex(prd(p, P_TFD));
        SerialLogger::Log(" cmd=");
        SerialLogger::LogHex(prd(p, P_CMD));
        SerialLogger::Log("\r\n");
    }

    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        if (!(pi & (1u << port))) continue;

        // spin the port up and let the phy settle before believing SSTS. after a
        // GHC.HR the link is still negotiating, so reading DET once immediately
        // reports 0 ("no device") on a port that does have a disk - which is
        // exactly how this reported "0 disk(s) online" with a drive attached.
        // set SUD (harmless when the hba does not do staggered spin-up), clear
        // stale errors, then poll DET rather than sampling it once. (satoru)
        pwr(port, P_CMD, prd(port, P_CMD) | (1u << 1));   // SUD (satoru)
        pwr(port, P_SERR, 0xFFFFFFFF);

        uint32_t ssts = 0, det = 0, ipm = 0;
        for (int tries = 0; tries < 200; tries++) {
            ssts = prd(port, P_SSTS);
            det  = ssts & 0x0F;
            ipm  = (ssts >> 8) & 0x0F;
            if (det == 3 && ipm == 1) break;
            spin(2);
        }
        if (det != 3 || ipm != 1) continue;          // no device / not active (satoru)

        // bring the port up BEFORE reading PxSIG. the hba latches the signature
        // from the device's initial D2H FIS, which only arrives once FIS-receive
        // is enabled - reading it first returns 0xFFFFFFFF on a perfectly good
        // disk, which is what made this report "0 disk(s) online" while SSTS
        // plainly said DET=3 IPM=1. PortReset is also what waits out the
        // BSY|DRQ in TFD. (satoru)
        if (!PortReset(port)) continue;

        uint32_t sig = prd(port, P_SIG);
        if (sig != SIG_ATA) {
            SerialLogger::Log("[ahci] port ");
            SerialLogger::LogDec(port);
            SerialLogger::Log(" not plain ata (sig=");
            SerialLogger::LogHex(sig);
            SerialLogger::Log("), skipping\r\n");
            continue;                                 // atapi / port multiplier (satoru)
        }

        AhciPortInfo info = {};
        if (!PortIdentify(port, &info)) {
            SerialLogger::Log("[ahci] port ");
            SerialLogger::LogDec(port);
            SerialLogger::Log(" identify failed\r\n");
            continue;
        }
        info.present = true;
        info.port    = port;
        s_ports[s_port_count] = info;

        // register with the block layer. name them sda, sdb, ... in discovery
        // order, matching what a user typing `lsblk` expects to see. (satoru)
        BlockDevice bd = {};
        bd.name[0] = 's'; bd.name[1] = 'd';
        bd.name[2] = (char)('a' + s_port_count); bd.name[3] = 0;
        bd.driver[0] = 'a'; bd.driver[1] = 'h'; bd.driver[2] = 'c';
        bd.driver[3] = 'i'; bd.driver[4] = 0;
        for (int i = 0; i < 47 && info.model[i]; i++) bd.model[i] = info.model[i];
        bd.capacity_lba = info.sectors;
        bd.sector_size  = info.sector_size;
        bd.removable    = false;
        bd.ctx   = (void*)(uintptr_t)port;
        bd.read  = bd_read;
        bd.write = bd_write;
        bd.flush = bd_flush;
        BlockDev::Register(bd);

        s_port_count++;
    }

    s_detected = (s_port_count > 0);
    SerialLogger::Log("[ahci] ");
    SerialLogger::LogDec(s_port_count);
    SerialLogger::Log(" disk(s) online\r\n");
    return s_detected;
}

bool Ahci::PortReset(int port) {
    // stop the port before touching CLB/FB - the spec requires ST and FRE clear
    // and CR/FR observed low before the pointers may be rewritten. (satoru)
    pwr(port, P_CMD, prd(port, P_CMD) & ~CMD_ST);
    for (int i = 0; i < 500 && (prd(port, P_CMD) & CMD_CR); i++) spin(1);
    pwr(port, P_CMD, prd(port, P_CMD) & ~CMD_FRE);
    for (int i = 0; i < 500 && (prd(port, P_CMD) & CMD_FR); i++) spin(1);

    PortMem& pm = g_pmem[port];
    if (!pm.clist) {
        // one page each for the command list and fis area (both need far less
        // but a page is the allocator's unit and keeps alignment trivial), one
        // for the command tables, and a 128kb dma staging buffer. (satoru)
        pm.clist  = (CmdHeader*)PMM::AllocBytes(4096);
        pm.fis    = (uint8_t*)  PMM::AllocBytes(4096);
        pm.ctbl   = (CmdTable*) PMM::AllocBytes(sizeof(CmdTable) * 32);
        pm.bounce = (uint8_t*)  PMM::AllocBytes(BOUNCE_BYTES);
        if (!pm.clist || !pm.fis || !pm.ctbl || !pm.bounce) {
            SerialLogger::Log("[ahci] FATAL: dma alloc failed\r\n");
            return false;
        }
    }

    uint8_t* cl = (uint8_t*)pm.clist;
    for (uint32_t i = 0; i < 4096; i++) cl[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) pm.fis[i] = 0;

    // pmm hands back identity-mapped low memory, so the virtual address is the
    // physical address the hba needs. (satoru)
    uint64_t clb = (uint64_t)(uintptr_t)pm.clist;
    uint64_t fb  = (uint64_t)(uintptr_t)pm.fis;
    pwr(port, P_CLB,  (uint32_t)(clb & 0xFFFFFFFF));
    pwr(port, P_CLBU, (uint32_t)(clb >> 32));
    pwr(port, P_FB,   (uint32_t)(fb & 0xFFFFFFFF));
    pwr(port, P_FBU,  (uint32_t)(fb >> 32));

    // point each command header at its own table. (satoru)
    for (int slot = 0; slot < 32; slot++) {
        uint64_t ct = (uint64_t)(uintptr_t)&pm.ctbl[slot];
        pm.clist[slot].prdtl = 1;
        pm.clist[slot].ctba  = (uint32_t)(ct & 0xFFFFFFFF);
        pm.clist[slot].ctbau = (uint32_t)(ct >> 32);
    }

    pwr(port, P_SERR, 0xFFFFFFFF);   // write-1-to-clear (satoru)
    pwr(port, P_IS,   0xFFFFFFFF);
    pwr(port, P_IE,   0);            // polled, no interrupts yet (satoru)

    pwr(port, P_CMD, prd(port, P_CMD) | CMD_FRE);
    // wait for the device to go idle before starting the command engine. (satoru)
    for (int i = 0; i < 1000 && (prd(port, P_TFD) & (TFD_BSY | TFD_DRQ)); i++) spin(1);
    if (prd(port, P_TFD) & (TFD_BSY | TFD_DRQ)) return false;
    pwr(port, P_CMD, prd(port, P_CMD) | CMD_ST);
    return true;
}

int Ahci::FindFreeSlot(int port) {
    uint32_t used = prd(port, P_CI);
    for (int i = 0; i < 32; i++) if (!(used & (1u << i))) return i;
    return -1;
}

bool Ahci::RunCommand(int port, int slot) {
    // clear stale status, issue, then poll CI until the bit clears. (satoru)
    pwr(port, P_IS, 0xFFFFFFFF);
    pwr(port, P_CI, 1u << slot);

    for (int i = 0; i < 100000; i++) {
        if (!(prd(port, P_CI) & (1u << slot))) break;
        if (prd(port, P_TFD) & TFD_ERR) {
            SerialLogger::Log("[ahci] port ");
            SerialLogger::LogDec(port);
            SerialLogger::Log(" task-file error tfd=");
            SerialLogger::LogHex(prd(port, P_TFD));
            SerialLogger::Log(" serr=");
            SerialLogger::LogHex(prd(port, P_SERR));
            SerialLogger::Log("\r\n");
            pwr(port, P_SERR, 0xFFFFFFFF);
            return false;
        }
        __asm__ __volatile__("pause");
    }
    if (prd(port, P_CI) & (1u << slot)) {
        SerialLogger::Log("[ahci] port ");
        SerialLogger::LogDec(port);
        SerialLogger::Log(" command timeout\r\n");
        return false;
    }
    return !(prd(port, P_TFD) & TFD_ERR);
}

bool Ahci::PortIdentify(int port, AhciPortInfo* info) {
    PortMem& pm = g_pmem[port];
    int slot = FindFreeSlot(port);
    if (slot < 0) return false;

    CmdHeader* hdr = &pm.clist[slot];
    hdr->flags = (uint16_t)(sizeof(FisRegH2D) / 4);   // cfl, w=0 (device->host)
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    CmdTable* tbl = &pm.ctbl[slot];
    for (uint32_t i = 0; i < sizeof(CmdTable); i++) ((uint8_t*)tbl)[i] = 0;

    uint64_t bph = (uint64_t)(uintptr_t)pm.bounce;
    tbl->prdt[0].dba  = (uint32_t)(bph & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(bph >> 32);
    tbl->prdt[0].dbc  = 512 - 1;                      // identify returns 512 bytes

    FisRegH2D* fis = (FisRegH2D*)tbl->cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;
    fis->command  = ATA_IDENTIFY;
    fis->device   = 0;

    if (!RunCommand(port, slot)) return false;

    const uint16_t* id = (const uint16_t*)pm.bounce;
    ata_string(info->model,  id + 27, 20, (int)sizeof(info->model));
    ata_string(info->serial, id + 10, 10, (int)sizeof(info->serial));

    // word 83 bit 10 = lba48 supported. words 100..103 hold the 48-bit count;
    // words 60..61 the 28-bit one. (satoru)
    uint64_t sectors = 0;
    if (id[83] & (1u << 10)) {
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    }
    if (!sectors) sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (!sectors) return false;

    // word 106: bit 14 set + bit 12 set means a logical sector larger than 512,
    // with the size in words 117..118. otherwise assume 512. (satoru)
    uint32_t ssz = 512;
    if ((id[106] & (1u << 14)) && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256 && words <= 4096) ssz = words * 2;
    }

    info->sectors     = sectors;
    info->sector_size = ssz;

    SerialLogger::Log("[ahci] port ");
    SerialLogger::LogDec(port);
    SerialLogger::Log(" ");
    SerialLogger::Log(info->model);
    SerialLogger::Log(" sectors=");
    SerialLogger::LogDec((int)sectors);
    SerialLogger::Log(" ssz=");
    SerialLogger::LogDec((int)ssz);
    SerialLogger::Log("\r\n");
    return true;
}

namespace {

// shared body for read and write - identical except the ata opcode, the header's
// write bit, and which way the bounce copy runs. (satoru)
bool ahci_xfer(int port, uint64_t lba, uint32_t count, void* buf, bool write) {
    if (!Ahci::IsDetected() || !buf || count == 0) return false;

    AhciPortInfo* pi = nullptr;
    for (int i = 0; i < Ahci::GetPortCount(); i++) {
        AhciPortInfo* p = Ahci::GetPort(i);
        if (p && p->port == port) { pi = p; break; }
    }
    if (!pi) return false;

    PortMem& pm = g_pmem[port];
    const uint32_t ssz = pi->sector_size;
    const uint32_t max_sec = BOUNCE_BYTES / ssz;
    uint8_t* user = (uint8_t*)buf;

    // chunk to the staging buffer so a caller can ask for more than 128kb.
    // (satoru)
    while (count > 0) {
        uint32_t chunk = count > max_sec ? max_sec : count;
        uint32_t bytes = chunk * ssz;

        if (write) {
            for (uint32_t i = 0; i < bytes; i++) pm.bounce[i] = user[i];
        }

        // one outstanding command per port: this is a rescue-path driver and the
        // polling loop below cannot track several in flight anyway. NCQ would
        // need the irq path first. (satoru)
        const int slot = 0;

        CmdHeader* hdr = &pm.clist[slot];
        hdr->flags = (uint16_t)((sizeof(FisRegH2D) / 4) | (write ? (1u << 6) : 0));
        hdr->prdtl = 1;
        hdr->prdbc = 0;

        CmdTable* tbl = &pm.ctbl[slot];
        for (uint32_t i = 0; i < 64 + 16 + 48; i++) ((uint8_t*)tbl)[i] = 0;

        uint64_t bph = (uint64_t)(uintptr_t)pm.bounce;
        tbl->prdt[0].dba  = (uint32_t)(bph & 0xFFFFFFFF);
        tbl->prdt[0].dbau = (uint32_t)(bph >> 32);
        tbl->prdt[0].dbc  = bytes - 1;

        FisRegH2D* fis = (FisRegH2D*)tbl->cfis;
        fis->fis_type = 0x27;
        fis->pmport_c = 0x80;
        fis->command  = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
        fis->lba0 = (uint8_t)(lba & 0xFF);
        fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
        fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
        fis->device = 0x40;                        // lba mode (satoru)
        fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
        fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
        fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
        fis->countl = (uint8_t)(chunk & 0xFF);
        fis->counth = (uint8_t)((chunk >> 8) & 0xFF);

        // RunCommand is private; go through the public wrappers' shared path by
        // issuing here with the same poll logic. (satoru)
        pwr(port, P_IS, 0xFFFFFFFF);
        pwr(port, P_CI, 1u << slot);
        bool ok = true;
        int i = 0;
        for (; i < 100000; i++) {
            if (!(prd(port, P_CI) & (1u << slot))) break;
            if (prd(port, P_TFD) & TFD_ERR) { ok = false; break; }
            __asm__ __volatile__("pause");
        }
        if (i >= 100000 || (prd(port, P_CI) & (1u << slot))) ok = false;
        if (prd(port, P_TFD) & TFD_ERR) ok = false;

        if (!ok) {
            SerialLogger::Log("[ahci] xfer fail port=");
            SerialLogger::LogDec(port);
            SerialLogger::Log(" lba=");
            SerialLogger::LogDec((int)lba);
            SerialLogger::Log(" tfd=");
            SerialLogger::LogHex(prd(port, P_TFD));
            SerialLogger::Log("\r\n");
            pwr(port, P_SERR, 0xFFFFFFFF);
            return false;
        }

        if (!write) {
            for (uint32_t j = 0; j < bytes; j++) user[j] = pm.bounce[j];
        }

        user  += bytes;
        lba   += chunk;
        count -= chunk;
    }
    return true;
}

}  // namespace

bool Ahci::Read(int port, uint64_t lba, uint32_t count, void* buf) {
    return ahci_xfer(port, lba, count, buf, false);
}

bool Ahci::Write(int port, uint64_t lba, uint32_t count, const void* buf) {
    // the bare-metal write guard has no sata window: guarded = read-only. (satoru)
    if (NVMe::WriteGuardOn()) {
        // once per boot: a retrying caller used to flood serial and the disk log (satoru)
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; SerialLogger::Log("[diskguard] REFUSED ahci write (further refusals not logged)\r\n"); }
        return false;
    }
    return ahci_xfer(port, lba, count, (void*)buf, true);
}

bool Ahci::Flush(int port) {
    if (!s_detected) return false;
    PortMem& pm = g_pmem[port];
    if (!pm.clist) return false;

    int slot = 0;
    CmdHeader* hdr = &pm.clist[slot];
    hdr->flags = (uint16_t)(sizeof(FisRegH2D) / 4);
    hdr->prdtl = 0;
    hdr->prdbc = 0;

    CmdTable* tbl = &pm.ctbl[slot];
    for (uint32_t i = 0; i < 64; i++) ((uint8_t*)tbl)[i] = 0;

    FisRegH2D* fis = (FisRegH2D*)tbl->cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;
    fis->command  = ATA_FLUSH_EXT;
    fis->device   = 0x40;

    return RunCommand(port, slot);
}

// end (satoru)
