//  kurono os  -  guest memory manager implementation
//  allocates and manages guest physical memory backed by host heap.
//
//  memory layout for guest:
//    0x00000000 - 0x000003ff  ivt (256 entries × 4 bytes)
//    0x00000400 - 0x000004ff  bios data area (bda)
//    0x00000500 - 0x00007bff  available for bootloader
//    0x00007c00 - 0x00007dff  boot sector (512 bytes)
//    0x00007e00 - 0x0000ffff  available
//    0x00010000 - 0x0008ffff  available ram (setup, cmdline, etc.)
//    0x00090000 - 0x0009fbff  extended bda / available
//    0x0009fc00 - 0x0009ffff  extended bios data area (ebda)
//    0x000a0000 - 0x000bffff  vga framebuffer
//    0x000c0000 - 0x000fffff  rom area
//    0x00100000 - 0x00ffffff  extended ram (up to guest size)
//    0xfec00000 -             i/o apic
//    0xfed00000 -             hpet
//    0xfee00000 -             local apic
#include "guest_mem.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

uint8_t*     GuestMemoryManager::low_ram          = nullptr;
uint32_t     GuestMemoryManager::low_ram_size     = 0;
uint8_t*     GuestMemoryManager::high_ram         = nullptr;
uint32_t     GuestMemoryManager::high_ram_size    = 0;
uint32_t     GuestMemoryManager::total_ram_bytes  = 0;
uint8_t*     GuestMemoryManager::vga_buffer       = nullptr;
uint8_t*     GuestMemoryManager::rom_area         = nullptr;
GuestPhysMap GuestMemoryManager::phys_maps[MAX_GUEST_PHYS_MAPS];
int          GuestMemoryManager::phys_map_count   = 0;
E820Entry    GuestMemoryManager::e820_table[MAX_E820_ENTRIES];
int          GuestMemoryManager::e820_count       = 0;
bool         GuestMemoryManager::initialized      = false;

//  init

void GuestMemoryManager::Init(uint32_t ram_mb) {
    if (initialized) return;

    SerialLogger::Log("GuestMem: Initializing with ");
    SerialLogger::LogDec(ram_mb);
    SerialLogger::Log(" MB\r\n");

    phys_map_count = 0;
    e820_count = 0;

    if (!AllocateGuestRAM(ram_mb)) {
        SerialLogger::Log("GuestMem: FAILED to allocate guest RAM!\r\n");
        return;
    }

    BuildE820Table();
    SetupBDA();
    SetupIVT();

    initialized = true;
    SerialLogger::Log("GuestMem: Initialization complete\r\n");
}

//  allocateguestram  -  allocate host memory for guest physical address space

bool GuestMemoryManager::AllocateGuestRAM(uint32_t ram_mb) {
    if (ram_mb < GUEST_MIN_RAM_MB) ram_mb = GUEST_MIN_RAM_MB;
    if (ram_mb > GUEST_MAX_RAM_MB) ram_mb = GUEST_MAX_RAM_MB;

    // low ram: 640 kb (conventional memory)
    low_ram_size = (uint32_t)GUEST_LOW_RAM_END; // 0xa0000 = 640 kb
    low_ram = (uint8_t*)KernelHeap::Alloc(low_ram_size);
    if (!low_ram) {
        SerialLogger::Log("GuestMem: Failed to alloc low RAM\r\n");
        return false;
    }
    memset(low_ram, 0, low_ram_size);

    // high ram: from 1 mb to (ram_mb * 1024 * 1024)
    uint32_t total_bytes = ram_mb * 1024 * 1024;
    high_ram_size = total_bytes - (uint32_t)GUEST_HIGH_RAM_START;
    high_ram = (uint8_t*)KernelHeap::Alloc(high_ram_size);
    if (!high_ram) {
        SerialLogger::Log("GuestMem: Failed to alloc high RAM (");
        SerialLogger::LogDec(high_ram_size / 1024);
        SerialLogger::Log(" KB), trying smaller...\r\n");

        // try with half
        ram_mb /= 2;
        if (ram_mb < GUEST_MIN_RAM_MB) ram_mb = GUEST_MIN_RAM_MB;
        total_bytes = ram_mb * 1024 * 1024;
        high_ram_size = total_bytes - (uint32_t)GUEST_HIGH_RAM_START;
        high_ram = (uint8_t*)KernelHeap::Alloc(high_ram_size);
        if (!high_ram) {
            KernelHeap::Free(low_ram);
            low_ram = nullptr;
            return false;
        }
    }
    memset(high_ram, 0, high_ram_size);

    total_ram_bytes = low_ram_size + high_ram_size;

    // vga buffer: 128 kb
    vga_buffer = (uint8_t*)KernelHeap::Alloc((uint32_t)GUEST_VGA_SIZE);
    if (vga_buffer) {
        memset(vga_buffer, 0, (uint32_t)GUEST_VGA_SIZE);
    }

    // rom area: 256 kb (for bios rom, video rom, etc.)
    rom_area = (uint8_t*)KernelHeap::Alloc((uint32_t)GUEST_ROM_SIZE);
    if (rom_area) {
        memset(rom_area, 0xFF, (uint32_t)GUEST_ROM_SIZE); // roms default to 0xff
    }

    // register physical mappings
    AddPhysMap(0x00000000ULL, low_ram, low_ram_size, E820_TYPE_RAM);
    if (vga_buffer)
        AddPhysMap(GUEST_VGA_BASE, vga_buffer, (uint32_t)GUEST_VGA_SIZE,
                   E820_TYPE_RESERVED);
    if (rom_area)
        AddPhysMap(GUEST_ROM_BASE, rom_area, (uint32_t)GUEST_ROM_SIZE,
                   E820_TYPE_RESERVED);
    AddPhysMap(GUEST_HIGH_RAM_START, high_ram, high_ram_size, E820_TYPE_RAM);

    SerialLogger::Log("GuestMem: Allocated ");
    SerialLogger::LogDec(total_ram_bytes / 1024);
    SerialLogger::Log(" KB (low=");
    SerialLogger::LogDec(low_ram_size / 1024);
    SerialLogger::Log("K high=");
    SerialLogger::LogDec(high_ram_size / 1024);
    SerialLogger::Log("K)\r\n");

    return true;
}

void GuestMemoryManager::FreeGuestRAM() {
    if (low_ram) { KernelHeap::Free(low_ram); low_ram = nullptr; }
    if (high_ram) { KernelHeap::Free(high_ram); high_ram = nullptr; }
    if (vga_buffer) { KernelHeap::Free(vga_buffer); vga_buffer = nullptr; }
    if (rom_area) { KernelHeap::Free(rom_area); rom_area = nullptr; }
    low_ram_size = high_ram_size = total_ram_bytes = 0;
    phys_map_count = 0;
    initialized = false;
}

//  addphysmap  -  register a guest physical → host virtual mapping

void GuestMemoryManager::AddPhysMap(uint64_t guest, uint8_t* host,
                                     uint32_t size, uint32_t type) {
    if (phys_map_count >= MAX_GUEST_PHYS_MAPS) return;
    GuestPhysMap& m = phys_maps[phys_map_count++];
    m.guest_phys = guest;
    m.host_virt  = host;
    m.size       = size;
    m.type       = type;
    m.allocated  = true;
}

//  guest physical address translation

uint8_t* GuestMemoryManager::GuestPhysToHost(uint64_t guest_phys) {
    for (int i = 0; i < phys_map_count; i++) {
        const GuestPhysMap& m = phys_maps[i];
        if (guest_phys >= m.guest_phys &&
            guest_phys < m.guest_phys + m.size) {
            uint64_t offset = guest_phys - m.guest_phys;
            return m.host_virt + offset;
        }
    }
    return nullptr; // not mapped
}

bool GuestMemoryManager::WriteGuestPhys(uint64_t guest_phys, const void* data,
                                          uint32_t size) {
    // handle writes that might span regions
    const uint8_t* src = (const uint8_t*)data;
    uint32_t written = 0;

    while (written < size) {
        uint8_t* host = GuestPhysToHost(guest_phys + written);
        if (!host) return false;

        // find how much contiguous we can write
        uint32_t chunk = size - written;
        // limit to remaining region
        for (int i = 0; i < phys_map_count; i++) {
            const GuestPhysMap& m = phys_maps[i];
            uint64_t addr = guest_phys + written;
            if (addr >= m.guest_phys && addr < m.guest_phys + m.size) {
                uint32_t remaining = (uint32_t)(m.guest_phys + m.size - addr);
                if (chunk > remaining) chunk = remaining;
                break;
            }
        }

        memcpy(host, src + written, chunk);
        written += chunk;
    }
    return true;
}

bool GuestMemoryManager::ReadGuestPhys(uint64_t guest_phys, void* buf,
                                         uint32_t size) {
    uint8_t* dst = (uint8_t*)buf;
    uint32_t read = 0;

    while (read < size) {
        uint8_t* host = GuestPhysToHost(guest_phys + read);
        if (!host) return false;

        uint32_t chunk = size - read;
        for (int i = 0; i < phys_map_count; i++) {
            const GuestPhysMap& m = phys_maps[i];
            uint64_t addr = guest_phys + read;
            if (addr >= m.guest_phys && addr < m.guest_phys + m.size) {
                uint32_t remaining = (uint32_t)(m.guest_phys + m.size - addr);
                if (chunk > remaining) chunk = remaining;
                break;
            }
        }

        memcpy(dst + read, host, chunk);
        read += chunk;
    }
    return true;
}

bool GuestMemoryManager::ZeroGuestPhys(uint64_t guest_phys, uint32_t size) {
    uint32_t zeroed = 0;
    while (zeroed < size) {
        uint8_t* host = GuestPhysToHost(guest_phys + zeroed);
        if (!host) return false;

        uint32_t chunk = size - zeroed;
        for (int i = 0; i < phys_map_count; i++) {
            const GuestPhysMap& m = phys_maps[i];
            uint64_t addr = guest_phys + zeroed;
            if (addr >= m.guest_phys && addr < m.guest_phys + m.size) {
                uint32_t remaining = (uint32_t)(m.guest_phys + m.size - addr);
                if (chunk > remaining) chunk = remaining;
                break;
            }
        }

        memset(host, 0, chunk);
        zeroed += chunk;
    }
    return true;
}

//  e820 memory map

void GuestMemoryManager::BuildE820Table() {
    e820_count = 0;

    // entry 0: low ram (0 to 640 kb)
    e820_table[e820_count].base   = 0x00000000ULL;
    e820_table[e820_count].length = GUEST_LOW_RAM_END;
    e820_table[e820_count].type   = E820_TYPE_RAM;
    e820_table[e820_count].acpi_extended = 1;
    e820_count++;

    // entry 1: vga + rom area (reserved, 0xa0000 - 0xfffff)
    e820_table[e820_count].base   = GUEST_VGA_BASE;
    e820_table[e820_count].length = (uint64_t)GUEST_HIGH_RAM_START - GUEST_VGA_BASE;
    e820_table[e820_count].type   = E820_TYPE_RESERVED;
    e820_table[e820_count].acpi_extended = 1;
    e820_count++;

    // entry 2: extended ram (1 mb to end of guest ram)
    e820_table[e820_count].base   = GUEST_HIGH_RAM_START;
    e820_table[e820_count].length = high_ram_size;
    e820_table[e820_count].type   = E820_TYPE_RAM;
    e820_table[e820_count].acpi_extended = 1;
    e820_count++;

    // entry 3: reserved  -  mmio hole for apic/hpet (0xfec00000 - 0xfef00000)
    e820_table[e820_count].base   = 0xFEC00000ULL;
    e820_table[e820_count].length = 0x00300000ULL; // 3 mb
    e820_table[e820_count].type   = E820_TYPE_RESERVED;
    e820_table[e820_count].acpi_extended = 1;
    e820_count++;

    SerialLogger::Log("GuestMem: E820 table built with ");
    SerialLogger::LogDec(e820_count);
    SerialLogger::Log(" entries\r\n");
}

int GuestMemoryManager::GetE820Count() {
    return e820_count;
}

const E820Entry* GuestMemoryManager::GetE820Table() {
    return e820_table;
}

bool GuestMemoryManager::WriteE820ToGuest(uint64_t guest_addr) {
    return WriteGuestPhys(guest_addr, e820_table,
                          e820_count * sizeof(E820Entry));
}

//  bda setup  -  minimal bios data area for real-mode guest
//  the bda lives at 0x0400 - 0x04ff in conventional memory.

void GuestMemoryManager::SetupBDA() {
    if (!low_ram) return;
    // bda starts at physical 0x0400, which is low_ram + 0x0400
    uint8_t* bda = low_ram + BDA_BASE;

    // zero the bda
    memset(bda, 0, 256);

    // com1 base address
    bda[0x00] = 0xF8; // com1 = 0x03f8
    bda[0x01] = 0x03;
    // com2 base address
    bda[0x02] = 0xF8; // com2 = 0x02f8
    bda[0x03] = 0x02;

    // equipment flags (word at 0x0410)
    // bit 1: math coprocessor
    // bit 4-5: initial video mode (10 = 80x25 color)
    // bit 9-11: number of com ports - 1
    uint16_t equip = (1 << 1) | (2 << 4) | (1 << 9);
    bda[0x10] = (uint8_t)(equip & 0xFF);
    bda[0x11] = (uint8_t)(equip >> 8);

    // conventional memory size in kb (at 0x0413)
    uint16_t conv_kb = (uint16_t)(low_ram_size / 1024);
    bda[0x13] = (uint8_t)(conv_kb & 0xFF);
    bda[0x14] = (uint8_t)(conv_kb >> 8);

    // keyboard buffer (circular buffer at 0x001e-0x003d)
    // head and tail pointers
    bda[0x1A] = 0x1E; // keyboard buffer head
    bda[0x1B] = 0x00;
    bda[0x1C] = 0x1E; // keyboard buffer tail
    bda[0x1D] = 0x00;

    // video state
    bda[0x49] = 0x03; // current video mode (80x25 text)
    bda[0x4A] = 80;   // columns per line
    bda[0x4B] = 0;
    bda[0x84] = 24;   // number of rows - 1

    // hard disk count (at 0x0475)
    bda[0x75] = 1; // 1 hard disk

    SerialLogger::Log("GuestMem: BDA configured at 0x0400\r\n");
}

//  ivt setup  -  minimal interrupt vector table for real-mode guest
//  the ivt occupies 0x0000-0x03ff (256 entries × 4 bytes each).
//  each entry is segment:offset in little-endian (offset low, offset high,
//  segment low, segment high).

void GuestMemoryManager::SetupIVT() {
    if (!low_ram) return;

    // set up minimal ivt entries
    // most entries point to a simple iret at a known location.
    // we'll put an iret instruction (0xcf) at 0x0500.
    low_ram[0x0500] = 0xCF; // iret instruction

    // set all 256 ivt entries to point to the iret at 0000:0500
    uint32_t* ivt = (uint32_t*)low_ram;
    for (int i = 0; i < 256; i++) {
        ivt[i] = 0x00000500; // offset = 0x0500, segment = 0x0000
    }

    // int 0x10 (video bios)  -  point to a minimal handler
    // we'll put a simple handler at 0x0510 that just does iret
    low_ram[0x0510] = 0xCF; // iret
    ivt[0x10] = 0x00000510;

    // int 0x12 (get memory size)  -  return conventional memory size
    // handler at 0x0520: mov ax, [0x0413] ; iret
    low_ram[0x0520] = 0xA1; // mov ax, [addr16]
    low_ram[0x0521] = 0x13; // addr low = 0x13
    low_ram[0x0522] = 0x04; // addr high = 0x04 → [0x0413]
    low_ram[0x0523] = 0xCF; // iret
    ivt[0x12] = 0x00000520;

    // int 0x13 (disk bios)  -  minimal: return error (cf set)
    // handler at 0x0530: stc ; iret
    low_ram[0x0530] = 0xF9; // stc (set carry flag)
    low_ram[0x0531] = 0xCF; // iret
    ivt[0x13] = 0x00000530;

    // int 0x15 (system services)  -  we handle e820 via vm exits
    // handler at 0x0540: iret
    low_ram[0x0540] = 0xCF; // iret
    ivt[0x15] = 0x00000540;

    // int 0x16 (keyboard)  -  return no key
    // handler at 0x0550: xor ax, ax ; iret
    low_ram[0x0550] = 0x31; // xor
    low_ram[0x0551] = 0xC0; // ax, ax
    low_ram[0x0552] = 0xCF; // iret
    ivt[0x16] = 0x00000550;

    SerialLogger::Log("GuestMem: IVT configured (256 entries)\r\n");
}

//  debug

void GuestMemoryManager::DumpMemoryMap() {
    SerialLogger::Log("=== Guest Memory Map ===\r\n");
    for (int i = 0; i < phys_map_count; i++) {
        const GuestPhysMap& m = phys_maps[i];
        SerialLogger::Log("  ");
        SerialLogger::LogHex((uint32_t)(m.guest_phys >> 32));
        SerialLogger::LogHex((uint32_t)m.guest_phys);
        SerialLogger::Log(" - ");
        SerialLogger::LogHex((uint32_t)((m.guest_phys + m.size) >> 32));
        SerialLogger::LogHex((uint32_t)(m.guest_phys + m.size));
        SerialLogger::Log(" (");
        SerialLogger::LogDec(m.size / 1024);
        SerialLogger::Log(" KB) type=");
        SerialLogger::LogDec(m.type);
        SerialLogger::Log(" host=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)m.host_virt);
        SerialLogger::Log("\r\n");
    }
}

void GuestMemoryManager::DumpE820() {
    SerialLogger::Log("=== E820 Memory Map ===\r\n");
    for (int i = 0; i < e820_count; i++) {
        const E820Entry& e = e820_table[i];
        SerialLogger::Log("  ");
        SerialLogger::LogHex((uint32_t)(e.base >> 32));
        SerialLogger::LogHex((uint32_t)e.base);
        SerialLogger::Log(" - ");
        SerialLogger::LogHex((uint32_t)((e.base + e.length) >> 32));
        SerialLogger::LogHex((uint32_t)(e.base + e.length));
        SerialLogger::Log(" type=");
        switch (e.type) {
            case E820_TYPE_RAM:       SerialLogger::Log("RAM");      break;
            case E820_TYPE_RESERVED:  SerialLogger::Log("Reserved"); break;
            case E820_TYPE_ACPI_RECL: SerialLogger::Log("ACPI");     break;
            case E820_TYPE_ACPI_NVS:  SerialLogger::Log("NVS");      break;
            default:                  SerialLogger::LogDec(e.type);   break;
        }
        SerialLogger::Log("\r\n");
    }
}
