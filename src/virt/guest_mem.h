// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Guest Memory Manager
//  Manages physical memory layout for a virtual machine guest.
//  Allocates RAM, sets up EPT/NPT mappings, handles MMIO regions,
//  provides the E820 memory map for the guest BIOS/kernel.
//
//  Standard PC Memory Layout (for guest):
//    0x00000000 - 0x0009FFFF  640 KB  Conventional RAM
//    0x000A0000 - 0x000BFFFF  128 KB  VGA framebuffer (MMIO)
//    0x000C0000 - 0x000FFFFF  256 KB  ROM area (BIOS, video ROM)
//    0x00100000 - 0x0xFFFFFF  ~15 MB  Extended RAM (up to guest size)
//    0xFEC00000 - 0xFEC003FF  1 KB    I/O APIC (MMIO)
//    0xFED00000 - 0xFED003FF  1 KB    HPET (MMIO)
//    0xFEE00000 - 0xFEE00FFF  4 KB    Local APIC (MMIO)
//
//  Reference: E820 specification, Intel SDM Vol 3C Chapter 28
// ═══════════════════════════════════════════════════════════════════════════
#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── E820 Memory Map Types ───────────────────────────────────────────────
constexpr uint32_t E820_TYPE_RAM       = 1;  // Usable RAM
constexpr uint32_t E820_TYPE_RESERVED  = 2;  // Reserved, unusable
constexpr uint32_t E820_TYPE_ACPI_RECL = 3;  // ACPI reclaimable
constexpr uint32_t E820_TYPE_ACPI_NVS  = 4;  // ACPI non-volatile storage
constexpr uint32_t E820_TYPE_BAD       = 5;  // Bad memory

// ─── E820 Table Entry ────────────────────────────────────────────────────
struct E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_extended; // ACPI 3.0 extended attributes
} __attribute__((packed));

constexpr int MAX_E820_ENTRIES = 32;

// ─── MMIO Region Constants ───────────────────────────────────────────────
constexpr uint64_t GUEST_IOAPIC_BASE  = 0xFEC00000ULL;
constexpr uint64_t GUEST_HPET_BASE    = 0xFED00000ULL;
constexpr uint64_t GUEST_LAPIC_BASE   = 0xFEE00000ULL;
constexpr uint64_t GUEST_VGA_BASE     = 0x000A0000ULL;
constexpr uint64_t GUEST_VGA_SIZE     = 0x00020000ULL; // 128 KB
constexpr uint64_t GUEST_ROM_BASE     = 0x000C0000ULL;
constexpr uint64_t GUEST_ROM_SIZE     = 0x00040000ULL; // 256 KB
constexpr uint64_t GUEST_LOW_RAM_END  = 0x000A0000ULL; // 640 KB
constexpr uint64_t GUEST_HIGH_RAM_START = 0x00100000ULL; // 1 MB

// ─── Guest configuration ─────────────────────────────────────────────────
constexpr uint32_t GUEST_DEFAULT_RAM_MB = 64;
constexpr uint32_t GUEST_MAX_RAM_MB     = 128; // Stay within our 64MB heap budget
constexpr uint32_t GUEST_MIN_RAM_MB     = 4;

// ═══════════════════════════════════════════════════════════════════════════
//  GuestPhysMap — a region of guest physical memory backed by host memory
// ═══════════════════════════════════════════════════════════════════════════
struct GuestPhysMap {
    uint64_t  guest_phys;   // Guest physical address
    uint8_t*  host_virt;    // Host virtual (KernelHeap-allocated)
    uint32_t  size;         // Size in bytes
    uint32_t  type;         // MEM_RAM, MEM_ROM, MEM_MMIO, etc.
    bool      allocated;    // true if we allocated host_virt
};

constexpr int MAX_GUEST_PHYS_MAPS = 16;

// ═══════════════════════════════════════════════════════════════════════════
//  BDA — BIOS Data Area setup helpers
// ═══════════════════════════════════════════════════════════════════════════
constexpr uint32_t BDA_BASE = 0x0400;
constexpr uint32_t BDA_COM1 = 0x0400; // COM1 base port
constexpr uint32_t BDA_COM2 = 0x0402; // COM2 base port
constexpr uint32_t BDA_EQUIP_FLAGS = 0x0410;
constexpr uint32_t BDA_MEM_SIZE_KB = 0x0413; // Conventional memory size in KB
constexpr uint32_t BDA_KBD_FLAGS   = 0x0417;

// ═══════════════════════════════════════════════════════════════════════════
//  GuestMemoryManager — manages the complete guest physical address space
// ═══════════════════════════════════════════════════════════════════════════
class GuestMemoryManager {
public:
    // ── Initialization ───────────────────────────────────────────────────
    static void Init(uint32_t ram_mb);

    // ── Memory allocation ────────────────────────────────────────────────
    // Allocates host-backed RAM for the guest and sets up the memory map.
    static bool AllocateGuestRAM(uint32_t ram_mb);
    static void FreeGuestRAM();

    // ── Guest physical memory access ─────────────────────────────────────
    // Translate guest physical address → host virtual pointer.
    // Returns nullptr if address is not mapped.
    static uint8_t* GuestPhysToHost(uint64_t guest_phys);
    static bool     WriteGuestPhys(uint64_t guest_phys, const void* data,
                                    uint32_t size);
    static bool     ReadGuestPhys(uint64_t guest_phys, void* buf,
                                   uint32_t size);
    static bool     ZeroGuestPhys(uint64_t guest_phys, uint32_t size);

    // ── E820 Memory Map ──────────────────────────────────────────────────
    // Build and return the E820 table for the guest.
    static int       GetE820Count();
    static const E820Entry* GetE820Table();

    // Write E820 table into guest memory at the specified address.
    // Linux boot protocol places it at a specific location.
    static bool      WriteE820ToGuest(uint64_t guest_addr);

    // ── BDA / IVT Setup ──────────────────────────────────────────────────
    // Set up minimal BIOS Data Area and Interrupt Vector Table
    // for the guest's real-mode environment.
    static void      SetupBDA();
    static void      SetupIVT();

    // ── Region Access ────────────────────────────────────────────────────
    static uint8_t*  GetLowRAM()       { return low_ram; }
    static uint8_t*  GetHighRAM()      { return high_ram; }
    static uint32_t  GetLowRAMSize()   { return low_ram_size; }
    static uint32_t  GetHighRAMSize()  { return high_ram_size; }
    static uint32_t  GetTotalRAM()     { return total_ram_bytes; }

    // ── MMIO ─────────────────────────────────────────────────────────────
    static uint8_t*  GetROMArea()      { return rom_area; }
    static uint8_t*  GetVGABuffer()    { return vga_buffer; }

    // ── Debug ────────────────────────────────────────────────────────────
    static void DumpMemoryMap();
    static void DumpE820();

private:
    // ── Guest RAM regions ────────────────────────────────────────────────
    static uint8_t*  low_ram;        // 0x0 - 0x9FFFF (640 KB)
    static uint32_t  low_ram_size;
    static uint8_t*  high_ram;       // 0x100000+ (Extended RAM)
    static uint32_t  high_ram_size;
    static uint32_t  total_ram_bytes;

    // ── Special regions ──────────────────────────────────────────────────
    static uint8_t*  vga_buffer;     // 128 KB for VGA framebuffer
    static uint8_t*  rom_area;       // 256 KB for ROM/BIOS area

    // ── Physical mapping table ───────────────────────────────────────────
    static GuestPhysMap phys_maps[MAX_GUEST_PHYS_MAPS];
    static int          phys_map_count;

    // ── E820 table ───────────────────────────────────────────────────────
    static E820Entry e820_table[MAX_E820_ENTRIES];
    static int       e820_count;

    static bool initialized;

    // ── Internal helpers ─────────────────────────────────────────────────
    static void AddPhysMap(uint64_t guest, uint8_t* host, uint32_t size,
                           uint32_t type);
    static void BuildE820Table();
};
