//  kurono os  -  guest memory manager
//  manages physical memory layout for a virtual machine guest.
//  allocates ram, sets up ept/npt mappings, handles mmio regions,
//  provides the e820 memory map for the guest bios/kernel.
//
//  standard pc memory layout (for guest):
//    0x00000000 - 0x0009ffff  640 kb  conventional ram
//    0x000a0000 - 0x000bffff  128 kb  vga framebuffer (mmio)
//    0x000c0000 - 0x000fffff  256 kb  rom area (bios, video rom)
//    0x00100000 - 0x0xffffff  ~15 mb  extended ram (up to guest size)
//    0xfec00000 - 0xfec003ff  1 kb    i/o apic (mmio)
//    0xfed00000 - 0xfed003ff  1 kb    hpet (mmio)
//    0xfee00000 - 0xfee00fff  4 kb    local apic (mmio)
//
//  reference: e820 specification, intel sdm vol 3c chapter 28
#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr uint32_t E820_TYPE_RAM       = 1;  // usable ram
constexpr uint32_t E820_TYPE_RESERVED  = 2;  // reserved, unusable
constexpr uint32_t E820_TYPE_ACPI_RECL = 3;  // acpi reclaimable
constexpr uint32_t E820_TYPE_ACPI_NVS  = 4;  // acpi non-volatile storage
constexpr uint32_t E820_TYPE_BAD       = 5;  // bad memory

struct E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_extended; // acpi 3.0 extended attributes
} __attribute__((packed));

constexpr int MAX_E820_ENTRIES = 32;

constexpr uint64_t GUEST_IOAPIC_BASE  = 0xFEC00000ULL;
constexpr uint64_t GUEST_HPET_BASE    = 0xFED00000ULL;
constexpr uint64_t GUEST_LAPIC_BASE   = 0xFEE00000ULL;
constexpr uint64_t GUEST_VGA_BASE     = 0x000A0000ULL;
constexpr uint64_t GUEST_VGA_SIZE     = 0x00020000ULL; // 128 kb
constexpr uint64_t GUEST_ROM_BASE     = 0x000C0000ULL;
constexpr uint64_t GUEST_ROM_SIZE     = 0x00040000ULL; // 256 kb
constexpr uint64_t GUEST_LOW_RAM_END  = 0x000A0000ULL; // 640 kb
constexpr uint64_t GUEST_HIGH_RAM_START = 0x00100000ULL; // 1 mb

constexpr uint32_t GUEST_DEFAULT_RAM_MB = 64;
constexpr uint32_t GUEST_MAX_RAM_MB     = 128; // stay within our 64mb heap budget
constexpr uint32_t GUEST_MIN_RAM_MB     = 4;

//  guestphysmap  -  a region of guest physical memory backed by host memory
struct GuestPhysMap {
    uint64_t  guest_phys;   // guest physical address
    uint8_t*  host_virt;    // host virtual (kernelheap-allocated)
    uint32_t  size;         // size in bytes
    uint32_t  type;         // mem_ram, mem_rom, mem_mmio, etc.
    bool      allocated;    // true if we allocated host_virt
};

constexpr int MAX_GUEST_PHYS_MAPS = 16;

//  bda  -  bios data area setup helpers
constexpr uint32_t BDA_BASE = 0x0400;
constexpr uint32_t BDA_COM1 = 0x0400; // com1 base port
constexpr uint32_t BDA_COM2 = 0x0402; // com2 base port
constexpr uint32_t BDA_EQUIP_FLAGS = 0x0410;
constexpr uint32_t BDA_MEM_SIZE_KB = 0x0413; // conventional memory size in kb
constexpr uint32_t BDA_KBD_FLAGS   = 0x0417;

//  guestmemorymanager  -  manages the complete guest physical address space
class GuestMemoryManager {
public:
    static void Init(uint32_t ram_mb);

    // allocates host-backed ram for the guest and sets up the memory map.
    static bool AllocateGuestRAM(uint32_t ram_mb);
    static void FreeGuestRAM();

    // translate guest physical address → host virtual pointer.
    // returns nullptr if address is not mapped.
    static uint8_t* GuestPhysToHost(uint64_t guest_phys);
    static bool     WriteGuestPhys(uint64_t guest_phys, const void* data,
                                    uint32_t size);
    static bool     ReadGuestPhys(uint64_t guest_phys, void* buf,
                                   uint32_t size);
    static bool     ZeroGuestPhys(uint64_t guest_phys, uint32_t size);

    // build and return the e820 table for the guest.
    static int       GetE820Count();
    static const E820Entry* GetE820Table();

    // write e820 table into guest memory at the specified address.
    // linux boot protocol places it at a specific location.
    static bool      WriteE820ToGuest(uint64_t guest_addr);

    // set up minimal bios data area and interrupt vector table
    // for the guest's real-mode environment.
    static void      SetupBDA();
    static void      SetupIVT();

    static uint8_t*  GetLowRAM()       { return low_ram; }
    static uint8_t*  GetHighRAM()      { return high_ram; }
    static uint32_t  GetLowRAMSize()   { return low_ram_size; }
    static uint32_t  GetHighRAMSize()  { return high_ram_size; }
    static uint32_t  GetTotalRAM()     { return total_ram_bytes; }

    static uint8_t*  GetROMArea()      { return rom_area; }
    static uint8_t*  GetVGABuffer()    { return vga_buffer; }

    static void DumpMemoryMap();
    static void DumpE820();

private:
    static uint8_t*  low_ram;        // 0x0 - 0x9ffff (640 kb)
    static uint32_t  low_ram_size;
    static uint8_t*  high_ram;       // 0x100000+ (extended ram)
    static uint32_t  high_ram_size;
    static uint32_t  total_ram_bytes;

    static uint8_t*  vga_buffer;     // 128 kb for vga framebuffer
    static uint8_t*  rom_area;       // 256 kb for rom/bios area

    static GuestPhysMap phys_maps[MAX_GUEST_PHYS_MAPS];
    static int          phys_map_count;

    static E820Entry e820_table[MAX_E820_ENTRIES];
    static int       e820_count;

    static bool initialized;

    static void AddPhysMap(uint64_t guest, uint8_t* host, uint32_t size,
                           uint32_t type);
    static void BuildE820Table();
};
