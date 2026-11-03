//  KLD broker implementation - see kld_broker.h for the design and the honest
//  limits of the isolation it provides.
//
//  end (satoru)

#include "kld_broker.h"
#include "pci.h"
#include "../drivers/serial.h"

namespace KldBroker {

namespace {

// ── grant table ─────────────────────────────────────────────────────────────
// one entry per (container pid, device) pair. small and fixed: a handful of
// driver containers is the realistic ceiling, and a fixed table means no
// allocation on a path an untrusted process can drive. (satoru)
constexpr int MAX_GRANTS = 16;

struct Grant {
    bool     used;
    int      pid;
    uint32_t bdf;
    uint32_t cfg_reads;
    uint32_t cfg_writes;
};

Grant g_grants[MAX_GRANTS];
uint32_t g_find_calls = 0;

bool bdf_valid(uint32_t bdf) {
    // bus/dev/func must be in range and nothing may be set above bit 23. (satoru)
    if (bdf & 0xFF000000u) return false;
    uint8_t dev  = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t func = (uint8_t)(bdf & 0xFF);
    return dev < 32 && func < 8;
}

bool is_granted(int pid, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].pid == pid && g_grants[i].bdf == bdf)
            return true;
    return false;
}

Grant* find_grant(int pid, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].pid == pid && g_grants[i].bdf == bdf)
            return &g_grants[i];
    return nullptr;
}

bool add_grant(int pid, uint32_t bdf) {
    if (is_granted(pid, bdf)) return true;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used) {
            g_grants[i].used = true;
            g_grants[i].pid = pid;
            g_grants[i].bdf = bdf;
            g_grants[i].cfg_reads = 0;
            g_grants[i].cfg_writes = 0;
            return true;
        }
    }
    return false;
}

void log_bdf(const char* pfx, uint32_t bdf) {
    SerialLogger::Log(pfx);
    SerialLogger::LogDec((int)((bdf >> 16) & 0xFF));
    SerialLogger::Log(":");
    SerialLogger::LogDec((int)((bdf >> 8) & 0xFF));
    SerialLogger::Log(".");
    SerialLogger::LogDec((int)(bdf & 0xFF));
}

// ── ops ─────────────────────────────────────────────────────────────────────

// ── the device cache ────────────────────────────────────────────────────────
// A bus scan MUST be cached. The first version scanned 256x32x8 on every
// PCI_FIND, and a driver with a 115-entry id_table (e1000e) therefore drove
// ~7.5 MILLION config reads, each two port-I/O cycles - it never finished.
// Scan once into this table, then FIND is a linear search over a handful of
// entries. (satoru)
constexpr int MAX_DEVS = 64;

struct DevEntry {
    uint32_t bdf;
    uint16_t vendor;
    uint16_t device;
};

DevEntry g_devs[MAX_DEVS];
int  g_dev_count = 0;
bool g_scanned = false;

// buses 0..7 covers qemu and the primary buses of real machines. NOT a full
// recursive bridge walk - a device behind a deeper bridge is invisible, which is
// a known limit rather than a silent one. (satoru)
constexpr uint32_t SCAN_BUS_LIMIT = 8;

void scan_bus_once() {
    if (g_scanned) return;
    g_scanned = true;
    for (uint32_t bus = 0; bus < SCAN_BUS_LIMIT; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t v = PCI::GetVendor((uint8_t)bus, dev, func);
                if (v == 0xFFFF || v == 0) continue;
                if (g_dev_count >= MAX_DEVS) goto done;
                g_devs[g_dev_count].bdf    = PackBdf((uint8_t)bus, dev, func);
                g_devs[g_dev_count].vendor = v;
                g_devs[g_dev_count].device = PCI::GetDevice((uint8_t)bus, dev, func);
                g_dev_count++;
                // a non-multifunction device (header type bit 7 clear) has no
                // func 1..7; skipping them cuts the scan 8x. (satoru)
                if (func == 0) {
                    uint8_t hdr = PCI::Read8((uint8_t)bus, dev, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
done:
    SerialLogger::Log("[kldb] pci scan cached: ");
    SerialLogger::LogDec(g_dev_count);
    SerialLogger::Log(" devices on buses 0-");
    SerialLogger::LogDec((int)SCAN_BUS_LIMIT - 1);
    SerialLogger::Log("\r\n");
}

// match a vendor/device against the cache, skipping the first `skip` hits so a
// container can enumerate multiple identical NICs. a match GRANTS it. (satoru)
int64_t op_pci_find(int pid, uint32_t vendor, uint32_t device, uint32_t skip) {
    g_find_calls++;
    if (vendor == 0 || vendor == 0xFFFF) return KLD_EINVAL;
    scan_bus_once();

    uint32_t seen = 0;
    for (int i = 0; i < g_dev_count; i++) {
        if (g_devs[i].vendor != (uint16_t)vendor) continue;
        if (g_devs[i].device != (uint16_t)device) continue;
        if (seen++ < skip) continue;
        if (!add_grant(pid, g_devs[i].bdf)) return KLD_ENOSPC;
        log_bdf("[kldb] granted ", g_devs[i].bdf);
        SerialLogger::Log(" (vendor/device matched)\r\n");
        return (int64_t)g_devs[i].bdf;
    }
    return KLD_ENODEV;
}

int64_t op_cfg_read(int pid, uint32_t bdf, uint32_t off, uint32_t width) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (off > 0xFFF) return KLD_EINVAL;
    Grant* g = find_grant(pid, bdf);
    if (!g) return KLD_EPERM;
    uint8_t bus = (uint8_t)((bdf >> 16) & 0xFF);
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t fn  = (uint8_t)(bdf & 0xFF);
    g->cfg_reads++;
    switch (width) {
        case 1: return (int64_t)PCI::Read8(bus, dev, fn, (uint8_t)off);
        case 2: return (int64_t)PCI::Read16(bus, dev, fn, (uint8_t)off);
        case 4: return (int64_t)PCI::Read32(bus, dev, fn, (uint8_t)off);
        default: return KLD_EINVAL;
    }
}

int64_t op_cfg_write(int pid, uint32_t bdf, uint32_t off, uint32_t width, uint32_t val) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (off > 0xFFF) return KLD_EINVAL;
    Grant* g = find_grant(pid, bdf);
    if (!g) return KLD_EPERM;
    uint8_t bus = (uint8_t)((bdf >> 16) & 0xFF);
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t fn  = (uint8_t)(bdf & 0xFF);
    g->cfg_writes++;

    // config writes are byte/word granular in the driver but the hardware
    // accessor is dword-wide, so read-modify-write the containing dword rather
    // than clobbering neighbouring registers. getting this wrong corrupts
    // unrelated config state in a way that is very hard to attribute. (satoru)
    if (width == 4) {
        PCI::Write32(bus, dev, fn, (uint8_t)(off & 0xFC), val);
        return KLD_OK;
    }
    if (width != 1 && width != 2) return KLD_EINVAL;
    uint32_t aligned = off & 0xFC;
    uint32_t cur = PCI::Read32(bus, dev, fn, (uint8_t)aligned);
    uint32_t shift = (off & 3u) * 8u;
    uint32_t mask = (width == 1) ? 0xFFu : 0xFFFFu;
    cur &= ~(mask << shift);
    cur |= (val & mask) << shift;
    PCI::Write32(bus, dev, fn, (uint8_t)aligned, cur);
    return KLD_OK;
}

int64_t op_pci_bar(int pid, uint32_t bdf, uint32_t idx) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (idx > 5) return KLD_EINVAL;
    if (!is_granted(pid, bdf)) return KLD_EPERM;
    uint8_t bus = (uint8_t)((bdf >> 16) & 0xFF);
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t fn  = (uint8_t)(bdf & 0xFF);
    return (int64_t)(uint32_t)PCI::GetBAR(bus, dev, fn, (int)idx);
}

int64_t op_pci_enable(int pid, uint32_t bdf) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!is_granted(pid, bdf)) return KLD_EPERM;
    uint8_t bus = (uint8_t)((bdf >> 16) & 0xFF);
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t fn  = (uint8_t)(bdf & 0xFF);
    // memory space + bus master. EnableBusMaster already ORs both bits. (satoru)
    PCI::EnableBusMaster(bus, dev, fn);
    log_bdf("[kldb] enabled (mem+busmaster) ", bdf);
    SerialLogger::Log("\r\n");
    return KLD_OK;
}

}  // namespace

void Init() {
    for (int i = 0; i < MAX_GRANTS; i++) g_grants[i].used = false;
    SerialLogger::Log("[kldb] KLD broker initialized (SYS_KLD_CALL pci proxy)\r\n");
}

int64_t Call(int pid, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (op) {
        case KLD_OP_PCI_FIND:
            return op_pci_find(pid, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2);
        case KLD_OP_PCI_CFG_READ:
            return op_cfg_read(pid, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2);
        case KLD_OP_PCI_CFG_WRITE:
            return op_cfg_write(pid, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
        case KLD_OP_PCI_BAR:
            return op_pci_bar(pid, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_PCI_ENABLE:
            return op_pci_enable(pid, (uint32_t)a0);
        case KLD_OP_STATUS: {
            int n = 0;
            for (int i = 0; i < MAX_GRANTS; i++)
                if (g_grants[i].used && g_grants[i].pid == pid) n++;
            return (int64_t)n;
        }
        default:
            return KLD_EINVAL;
    }
}

void ReleasePid(int pid) {
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (g_grants[i].used && g_grants[i].pid == pid) {
            log_bdf("[kldb] released ", g_grants[i].bdf);
            SerialLogger::Log(" (container exited)\r\n");
            g_grants[i].used = false;
        }
    }
}

int Status(char* out, int mx) {
    int p = 0;
    auto cat = [&](const char* s) { while (*s && p < mx - 1) out[p++] = *s++; out[p] = 0; };
    auto cati = [&](int v) {
        char r[12]; int ri = 0;
        if (v == 0) r[ri++] = '0';
        else { char t[12]; int ti = 0; int x = v; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; } while (ti) r[ri++] = t[--ti]; }
        r[ri] = 0; cat(r);
    };
    cat("KLD broker (ring-3 linux driver containers):\n");
    cat("  pci_find calls: "); cati((int)g_find_calls); cat("\n");
    int n = 0;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used) continue;
        n++;
        cat("  grant pid="); cati(g_grants[i].pid);
        cat(" bdf=");        cati((int)((g_grants[i].bdf >> 16) & 0xFF));
        cat(":");            cati((int)((g_grants[i].bdf >> 8) & 0xFF));
        cat(".");            cati((int)(g_grants[i].bdf & 0xFF));
        cat(" cfgr=");       cati((int)g_grants[i].cfg_reads);
        cat(" cfgw=");       cati((int)g_grants[i].cfg_writes);
        cat("\n");
    }
    if (!n) cat("  (no devices granted)\n");
    return p;
}

}  // namespace KldBroker

// end (satoru)
