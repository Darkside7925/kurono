//  KLD line interrupts: MADT discovery and I/O APIC redirection entries.
//
//  The MADT is read through the bootloader's RSDP with the same XSDT/RSDT walk
//  SMP::find_madt uses. At boot this file only reads the ACPI table: the I/O
//  APIC registers are not touched until the first IRQ_LINE_ATTACH (their
//  version and redirection count are read lazily then). The registers are
//  reached through the boot identity map exactly as smp.cpp reaches the local
//  APIC; before the first access the base is checked against the identity-map
//  limit (PMM::IDENTITY_CEIL_BYTES) and the firmware memory map (usable ram is
//  refused). The access counter proves the default boot never touches them.
//
//  Every IOREGSEL/IOWIN pair runs under one irqsave lock (the isr masks a
//  delivery on the bsp while a syscall programs another cpu), and the whole
//  read-modify-write of a redirection entry is one critical section. Logs run
//  after the lock is released.
//
// end (satoru)

#include "kld_ioapic.h"
#include "../proc/smp.h"
#include "../proc/spinlock.h"
#include "pmm.h"
#include "kld_broker.h"
#include "../drivers/serial.h"
#include "io.h"

namespace KldIoApic {

namespace {

struct AcpiSdt {
    uint8_t  signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

constexpr int MAX_IOAPICS = 4;
constexpr int MAX_OVERRIDES = 32;
constexpr uint32_t IOWIN = 0x10;
constexpr uint8_t REG_VERSION = 0x01;

struct IoApic {
    uint64_t base;
    uint32_t gsi_base;
    uint32_t id;
    uint32_t version;
    uint32_t max_redir;
    bool     owned;       // base checked + version read
};

struct Override {
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
};

IoApic g_io[MAX_IOAPICS];
int g_nio;
Override g_ov[MAX_OVERRIDES];
int g_nov;
bool g_madt_done;
volatile int g_hw_state;      // 0 cold, 1 mapping/reading, 2 ready (satoru)
Spinlock g_io_lock;           // one select+data pair at a time (satoru)
unsigned long g_accesses;
unsigned long g_reg_reads;
unsigned long g_reg_writes;

bool sdt_ok(const AcpiSdt* t)
{
    uint64_t p = (uint64_t)(uintptr_t)t;
    if (!t || p >= PMM::IDENTITY_CEIL_BYTES)
        return false;
    if (t->length < sizeof(AcpiSdt) || t->length > (1u << 20))
        return false;
    if (p + t->length > PMM::IDENTITY_CEIL_BYTES)
        return false;
    // a firmware table that claims to live in usable ram is not a table we
    // may dereference (the broker's map snapshot answers this). (satoru)
    if (KldBroker::PhysIsUsableRam(p, t->length))
        return false;
    return true;
}

const AcpiSdt* root_table(uint64_t rsdp_phys)
{
    const uint8_t* rsdp = (const uint8_t*)(uintptr_t)rsdp_phys;
    // the boot rsdp lives in a KERNEL static (high-half under main's kernel);
    // the table pointers inside it are firmware physical addresses and are
    // walked through the low identity map, which main keeps. only the table
    // pointers, not this kernel pointer, are bounded below. (satoru)
    if (!rsdp)
        return nullptr;
    if (rsdp[15] >= 2) {
        uint64_t xsdt = *(const uint64_t*)(rsdp + 24);
        const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)xsdt;
        if (sdt_ok(t) && t->signature[0] == 'X' && t->signature[1] == 'S' &&
            t->signature[2] == 'D' && t->signature[3] == 'T')
            return t;
    }
    uint32_t rsdt = *(const uint32_t*)(rsdp + 16);
    const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)rsdt;
    if (sdt_ok(t) && t->signature[0] == 'R' && t->signature[1] == 'S' &&
        t->signature[2] == 'D' && t->signature[3] == 'T')
        return t;
    return nullptr;
}

const AcpiSdt* find_madt()
{
    uint64_t rsdp = BootAcpi::Rsdp();
    const AcpiSdt* root = root_table(rsdp);
    if (!root)
        return nullptr;
    bool wide = root->signature[0] == 'X';
    uint32_t esz = wide ? 8 : 4;
    uint32_t n = (root->length - sizeof(AcpiSdt)) / esz;
    const uint8_t* ptrs = (const uint8_t*)root + sizeof(AcpiSdt);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t tp = wide ? *(const uint64_t*)(ptrs + i * 8)
                           : *(const uint32_t*)(ptrs + i * 4);
        const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)tp;
        if (!sdt_ok(t))
            continue;
        if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
            t->signature[2] == 'I' && t->signature[3] == 'C')
            return t;
    }
    return nullptr;
}

void parse_madt(const AcpiSdt* madt)
{
    const uint8_t* p = (const uint8_t*)madt + sizeof(AcpiSdt) + 8;
    const uint8_t* end = (const uint8_t*)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;
        if (type == 1 && len >= 12 && g_nio < MAX_IOAPICS) {
            IoApic* io = &g_io[g_nio++];
            io->id = p[2];
            io->base = *(const uint32_t*)(p + 4);
            io->gsi_base = *(const uint32_t*)(p + 8);
        } else if (type == 2 && len >= 10 && g_nov < MAX_OVERRIDES) {
            Override* ov = &g_ov[g_nov++];
            ov->source = p[3];
            ov->gsi = *(const uint32_t*)(p + 4);
            ov->flags = *(const uint16_t*)(p + 8);
        }
        p += len;
    }
}

// raw access, caller holds g_io_lock. (satoru)
uint32_t raw_read(IoApic* io, uint8_t reg)
{
    volatile uint32_t* sel = (volatile uint32_t*)(uintptr_t)io->base;
    volatile uint32_t* win = (volatile uint32_t*)(uintptr_t)(io->base + IOWIN);
    g_accesses++;
    g_reg_writes++;
    *sel = reg;
    g_accesses++;
    g_reg_reads++;
    return *win;
}

void raw_write(IoApic* io, uint8_t reg, uint32_t val)
{
    volatile uint32_t* sel = (volatile uint32_t*)(uintptr_t)io->base;
    volatile uint32_t* win = (volatile uint32_t*)(uintptr_t)(io->base + IOWIN);
    g_accesses++;
    g_reg_writes++;
    *sel = reg;
    g_accesses++;
    g_reg_writes++;
    *win = val;
}

void map_and_read()
{
    for (int i = 0; i < g_nio; i++) {
        IoApic* io = &g_io[i];
        uint64_t page = io->base & ~0xFFFULL;
        // identity-map check, the way smp.cpp reaches the local APIC: the boot
        // pages only cover up to PMM::IDENTITY_CEIL_BYTES, and a base that the
        // firmware calls usable ram is the kernel's memory, not mmio. the
        // mtrrs already make this hole uncached. (satoru)
        if (!page || io->base + 0x1000 > PMM::IDENTITY_CEIL_BYTES ||
            KldBroker::PhysIsUsableRam(page, 0x1000)) {
            SerialLogger::Log("[kld-ioapic] ioapic ");
            SerialLogger::LogDec(i);
            SerialLogger::Log(" base ");
            SerialLogger::LogHex64(io->base);
            SerialLogger::Log(" is outside the identity map or inside usable ram, "
                              "attach refused\r\n");
            continue;
        }
        uint32_t v = 0;
        {
            SpinLockGuard g(g_io_lock);
            v = raw_read(io, REG_VERSION);
        }
        io->version = v;
        io->max_redir = (v >> 16) & 0xFF;
        if (v == 0xFFFFFFFFu || io->max_redir > 240) {
            // no ioapic there (or an implausible register): never program it.
            // (satoru)
            SerialLogger::Log("[kld-ioapic] ioapic ");
            SerialLogger::LogDec(i);
            SerialLogger::Log(" version ");
            SerialLogger::LogHex(v);
            SerialLogger::Log(" is invalid, attach refused\r\n");
            io->owned = false;
            continue;
        }
        io->owned = true;
        SerialLogger::Log("[kld-ioapic] ioapic ");
        SerialLogger::LogDec(i);
        SerialLogger::Log(" base ");
        SerialLogger::LogHex64(io->base);
        SerialLogger::Log(" version ");
        SerialLogger::LogDec((int)(v & 0xFF));
        SerialLogger::Log(" pins ");
        SerialLogger::LogDec((int)(io->max_redir + 1));
        SerialLogger::Log("\r\n");
    }
}

void ensure_hw()
{
    int s = __atomic_load_n(&g_hw_state, __ATOMIC_ACQUIRE);
    if (s == 2)
        return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_hw_state, &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        map_and_read();
        __atomic_store_n(&g_hw_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&g_hw_state, __ATOMIC_ACQUIRE) == 1)
        __asm__ __volatile__("pause");
}

// the ioapic whose redirection range contains @gsi, never a fallback. (satoru)
IoApic* io_for_gsi(uint32_t gsi)
{
    for (int i = 0; i < g_nio; i++) {
        IoApic* io = &g_io[i];
        if (!io->owned)
            continue;
        if (gsi >= io->gsi_base && gsi - io->gsi_base <= io->max_redir)
            return io;
    }
    return nullptr;
}

bool gsi_entry(uint32_t gsi, IoApic** io_out, uint32_t* index_out)
{
    ensure_hw();
    IoApic* io = io_for_gsi(gsi);
    if (!io)
        return false;
    uint32_t idx = gsi - io->gsi_base;
    // the register index is one byte: 0x10 + 2*idx + 1 must stay <= 0xff.
    // real ioapics top out at 120 pins; anything more refuses the attach. (satoru)
    if (idx > 119) {
        SerialLogger::Log("[kld-ioapic] gsi ");
        SerialLogger::LogDec((int)gsi);
        SerialLogger::Log(" index ");
        SerialLogger::LogDec((int)idx);
        SerialLogger::Log(" does not fit the register window\r\n");
        return false;
    }
    *io_out = io;
    *index_out = idx;
    return true;
}

}  // namespace

bool Ensure()
{
    if (g_madt_done)
        return g_nio > 0;
    g_madt_done = true;
    const AcpiSdt* madt = find_madt();
    if (!madt) {
        SerialLogger::Log("[kld-ioapic] no MADT\r\n");
        return false;
    }
    parse_madt(madt);
    SerialLogger::Log("[kld-ioapic] MADT: ");
    SerialLogger::LogDec(g_nio);
    SerialLogger::Log(" ioapic(s)");
    for (int i = 0; i < g_nio; i++) {
        SerialLogger::Log(", base ");
        SerialLogger::LogHex64(g_io[i].base);
        SerialLogger::Log(" gsi_base ");
        SerialLogger::LogDec((int)g_io[i].gsi_base);
    }
    SerialLogger::Log(", ");
    SerialLogger::LogDec(g_nov);
    SerialLogger::Log(" override(s)");
    for (int i = 0; i < g_nov; i++) {
        SerialLogger::Log(" ");
        SerialLogger::LogDec((int)g_ov[i].source);
        SerialLogger::Log("->");
        SerialLogger::LogDec((int)g_ov[i].gsi);
        SerialLogger::Log(" flags ");
        SerialLogger::LogHex(g_ov[i].flags);
    }
    SerialLogger::Log("\r\n");
    return g_nio > 0;
}

uint32_t IoApicCount()
{
    return (uint32_t)g_nio;
}

uint64_t Base()
{
    return g_nio ? g_io[0].base : 0;
}

bool Resolve(uint32_t gsi, bool isa, uint32_t* out_gsi, uint32_t* out_flags)
{
    *out_gsi = gsi;
    *out_flags = 0;
    if (isa && gsi < 16) {
        for (int i = 0; i < g_nov; i++)
            if (g_ov[i].source == gsi) {
                *out_gsi = g_ov[i].gsi;
                *out_flags = g_ov[i].flags;
                break;
            }
    }
    ensure_hw();
    return io_for_gsi(*out_gsi) != nullptr;
}

bool IsKernelOwned(uint32_t gsi)
{
    // the 8259 cascade is never a device line. (satoru)
    return gsi == 2;
}

bool LegacyBusy(uint32_t gsi, uint32_t resolved_gsi)
{
    // read the two 8259 masks the kernel keeps in PIC1_DATA/PIC2_DATA: a clear
    // bit means the line is enabled and has a native user. (satoru)
    uint16_t mask = (uint16_t)(inb(0x21) | ((uint16_t)inb(0xA1) << 8));
    bool busy = false;
    if (gsi < 16 && !(mask & (1u << gsi)))
        busy = true;
    if (resolved_gsi < 16 && !(mask & (1u << resolved_gsi)))
        busy = true;
    for (int i = 0; i < g_nov; i++)
        if (g_ov[i].gsi == resolved_gsi && g_ov[i].source < 16 &&
            !(mask & (1u << g_ov[i].source)))
            busy = true;
    SerialLogger::Log("[kld-ioapic] pic mask ");
    SerialLogger::LogHex(mask);
    SerialLogger::Log(" gsi ");
    SerialLogger::LogDec((int)gsi);
    SerialLogger::Log(" resolved ");
    SerialLogger::LogDec((int)resolved_gsi);
    SerialLogger::Log(busy ? " busy" : " free");
    SerialLogger::Log("\r\n");
    return busy;
}

bool Program(uint32_t gsi, uint32_t vector, uint32_t apic_id, uint32_t flags,
             bool masked)
{
    IoApic* io;
    uint32_t idx;
    if (!gsi_entry(gsi, &io, &idx))
        return false;
    uint32_t low = (vector & 0xFFu) |
                   (0u << 8) |                          // fixed delivery
                   (0u << 11) |                         // physical destination
                   ((flags & LINE_ACTIVE_LOW) ? (1u << 13) : 0u) |
                   ((flags & LINE_LEVEL) ? (1u << 15) : 0u) |
                   (masked ? (1u << 16) : 0u);
    uint32_t high = (apic_id & 0xFFu) << 24;
    {
        SpinLockGuard g(g_io_lock);
        raw_write(io, (uint8_t)(0x10 + 2 * idx), low);
        raw_write(io, (uint8_t)(0x10 + 2 * idx + 1), high);
    }
    SerialLogger::Log("[kld-ioapic] program gsi ");
    SerialLogger::LogDec((int)gsi);
    SerialLogger::Log(" vector ");
    SerialLogger::LogHex(vector);
    SerialLogger::Log(" apic ");
    SerialLogger::LogDec((int)apic_id);
    SerialLogger::Log((flags & LINE_LEVEL) ? " level" : " edge");
    SerialLogger::Log((flags & LINE_ACTIVE_LOW) ? " active-low" : " active-high");
    SerialLogger::Log(masked ? " masked" : " unmasked");
    SerialLogger::Log("\r\n");
    return true;
}

bool Mask(uint32_t gsi)
{
    IoApic* io;
    uint32_t idx;
    if (!gsi_entry(gsi, &io, &idx))
        return false;
    SpinLockGuard g(g_io_lock);
    uint8_t reg = (uint8_t)(0x10 + 2 * idx);
    uint32_t low = raw_read(io, reg);
    raw_write(io, reg, low | (1u << 16));
    return true;
}

bool Unmask(uint32_t gsi)
{
    IoApic* io;
    uint32_t idx;
    if (!gsi_entry(gsi, &io, &idx))
        return false;
    SpinLockGuard g(g_io_lock);
    uint8_t reg = (uint8_t)(0x10 + 2 * idx);
    uint32_t low = raw_read(io, reg);
    raw_write(io, reg, low & ~(1u << 16));
    return true;
}

int EoiMode(uint32_t gsi, bool lapic_suppress)
{
    ensure_hw();
    IoApic* io = io_for_gsi(gsi);
    if (!io)
        return -1;
    if (!lapic_suppress)
        return 0;   // the local EOI broadcasts; no ioapic access needed
    return (io->version & 0xFF) >= 0x20 ? 1 : -1;
}

uint32_t VersionForGsi(uint32_t gsi)
{
    ensure_hw();
    IoApic* io = io_for_gsi(gsi);
    return io ? io->version : 0;
}

bool Eoi(uint32_t gsi, uint32_t vector)
{
    ensure_hw();
    IoApic* io = io_for_gsi(gsi);
    if (!io || (io->version & 0xFF) < 0x20)
        return false;
    SpinLockGuard g(g_io_lock);
    // EOIR is a DIRECT memory-mapped register at base+0x40, not an indexed
    // one: IOREGSEL 0x40 would name the low dword of redirection entry 24 on a
    // 120-pin PCH ioapic. (satoru)
    volatile uint32_t* eoir = (volatile uint32_t*)(uintptr_t)(io->base + 0x40);
    g_accesses++;
    g_reg_writes++;
    *eoir = vector & 0xFFu;
    return true;
}

// snapshot of the redirection table, low dwords with the read-only status bits
// (delivery status bit 12, remote irr bit 14) masked out. (satoru)
constexpr int SNAP_MAX = 240;
uint32_t g_snap_lo[SNAP_MAX];
uint32_t g_snap_hi[SNAP_MAX];
int g_snap_n;
IoApic* g_snap_io;

bool g_audit;   // kurono.kld.ioapic.audit=1: debug only, ~1000 register reads per call (satoru)

void SetAudit(bool on) { g_audit = on; }
bool AuditEnabled() { return g_audit; }

void SnapshotEntries()
{
    if (!g_audit)
        return;
    ensure_hw();
    if (!g_nio)
        return;
    IoApic* io = &g_io[0];
    if (!io->owned)
        return;
    g_snap_io = io;
    g_snap_n = (int)io->max_redir + 1;
    if (g_snap_n > SNAP_MAX)
        g_snap_n = SNAP_MAX;
    SpinLockGuard g(g_io_lock);
    for (int i = 0; i < g_snap_n; i++) {
        g_snap_lo[i] = raw_read(io, (uint8_t)(0x10 + 2 * i)) & ~((1u << 12) | (1u << 14));
        g_snap_hi[i] = raw_read(io, (uint8_t)(0x10 + 2 * i + 1));
    }
}

bool CheckEntriesExcept(uint32_t gsi)
{
    if (!g_audit)
        return true;
    if (!g_snap_io || !g_snap_io->owned)
        return false;
    int idx = (int)(gsi - g_snap_io->gsi_base);
    bool ok = true;
    int bad = -1;
    {
        SpinLockGuard g(g_io_lock);
        for (int i = 0; i < g_snap_n; i++) {
            if (i == idx)
                continue;
            uint32_t lo = raw_read(g_snap_io, (uint8_t)(0x10 + 2 * i)) &
                          ~((1u << 12) | (1u << 14));
            uint32_t hi = raw_read(g_snap_io, (uint8_t)(0x10 + 2 * i + 1));
            if (lo != g_snap_lo[i] || hi != g_snap_hi[i]) {
                ok = false;
                if (bad < 0)
                    bad = i;
            }
        }
    }
    SerialLogger::Log("[kld-ioapic] entry dump: ");
    SerialLogger::LogDec(g_snap_n);
    SerialLogger::Log(" entries, others unchanged: ");
    SerialLogger::Log(ok ? "PASS" : "FAIL");
    if (!ok) {
        SerialLogger::Log(" (first changed index ");
        SerialLogger::LogDec(bad);
        SerialLogger::Log(")");
    }
    SerialLogger::Log("\r\n");
    return ok;
}

void MaskLegacyPic(uint32_t gsi)
{
    if (gsi >= 16)
        return;
    uint16_t port = 0x21;
    uint8_t bit = (uint8_t)gsi;
    if (gsi >= 8) {
        port = 0xA1;
        bit = (uint8_t)(gsi - 8);
    }
    uint8_t mask = inb(port);
    if (!(mask & (1u << bit))) {
        outb(port, (uint8_t)(mask | (1u << bit)));
        SerialLogger::Log("[kld-ioapic] 8259 line ");
        SerialLogger::LogDec((int)gsi);
        SerialLogger::Log(" masked\r\n");
    }
}

bool OwnsRange(uint64_t base, uint64_t len)
{
    if (!len)
        return false;
    for (int i = 0; i < g_nio; i++) {
        uint64_t lo = g_io[i].base & ~0xFFFULL;
        uint64_t hi = lo + 0x1000;
        if (base < hi && base + len > lo)
            return true;
    }
    return false;
}

unsigned long Accesses()
{
    return g_accesses;
}

unsigned long RegReads()
{
    return g_reg_reads;
}

unsigned long RegWrites()
{
    return g_reg_writes;
}

}  // namespace KldIoApic

// end (satoru)
