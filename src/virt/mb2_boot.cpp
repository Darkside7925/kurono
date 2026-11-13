//  kurono os - multiboot2 guest loader (see mb2_boot.h) (satoru)
#include "mb2_boot.h"
#include "guest_mem.h"
#include "../drivers/serial.h"

namespace {
    const char* g_err = "";
    uint64_t g_fb = 0;

    uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

    // zero a guest range in bounded pieces (satoru)
    bool zero_guest(uint64_t gpa, uint64_t len) {
        while (len) {
            uint32_t n = len > (1u << 20) ? (1u << 20) : (uint32_t)len;
            if (!GuestMemoryManager::ZeroGuestPhys(gpa, n)) return false;
            gpa += n; len -= n;
        }
        return true;
    }
    bool copy_guest(uint64_t gpa, const uint8_t* src, uint64_t len) {
        while (len) {
            uint32_t n = len > (1u << 20) ? (1u << 20) : (uint32_t)len;
            if (!GuestMemoryManager::WriteGuestPhys(gpa, src, n)) return false;
            gpa += n; src += n; len -= n;
        }
        return true;
    }

    // the info is built in a host buffer, then copied in one piece (satoru)
    uint8_t g_info[4096];
    uint32_t g_ilen = 0;
    uint8_t* tag_begin(uint32_t type) {
        g_ilen = (g_ilen + 7u) & ~7u;
        uint8_t* t = g_info + g_ilen;
        for (int i = 0; i < 8; i++) t[i] = 0;
        t[0] = (uint8_t)type; t[1] = (uint8_t)(type >> 8);
        g_ilen += 8;
        return t;
    }
    void tag_end(uint8_t* t) {
        uint32_t sz = (uint32_t)((g_info + g_ilen) - t);
        t[4] = (uint8_t)sz; t[5] = (uint8_t)(sz >> 8); t[6] = (uint8_t)(sz >> 16); t[7] = (uint8_t)(sz >> 24);
    }
    void put32(uint32_t v) { for (int i = 0; i < 4; i++) g_info[g_ilen++] = (uint8_t)(v >> (8 * i)); }
    void put64(uint64_t v) { put32((uint32_t)v); put32((uint32_t)(v >> 32)); }
    void put8(uint8_t v) { g_info[g_ilen++] = v; }
}

const char* MB2Boot::Error() { return g_err; }
uint64_t MB2Boot::FramebufferGPA() { return g_fb; }

bool MB2Boot::Load(const uint8_t* elf, uint64_t size, const char* cmdline,
                   uint32_t* entry, uint32_t* mbi) {
    g_fb = 0;
    if (!elf || size < 64 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 2) {
        g_err = "not an ELF64 file"; return false;
    }
    if (rd16(elf + 18) != 62) { g_err = "not an x86_64 ELF"; return false; }
    // a multiboot2 header in the first 32 KiB of the file, 8-byte aligned (satoru)
    bool mb2 = false;
    for (uint64_t o = 0; o + 16 <= size && o < 32768; o += 8) {
        if (rd32(elf + o) != 0xE85250D6u) continue;
        uint32_t arch = rd32(elf + o + 4), hl = rd32(elf + o + 8), ck = rd32(elf + o + 12);
        if (arch == 0 && (uint32_t)(0xE85250D6u + arch + hl + ck) == 0) { mb2 = true; break; }
    }
    if (!mb2) { g_err = "no multiboot2 header in the first 32 KiB"; return false; }

    uint64_t phoff = rd64(elf + 32);
    uint16_t phent = rd16(elf + 54), phnum = rd16(elf + 56);
    if (phent < 56 || phoff + (uint64_t)phnum * phent > size) { g_err = "bad program headers"; return false; }
    uint64_t ram_top = (uint64_t)GUEST_HIGH_RAM_START + GuestMemoryManager::GetHighRAMSize();
    uint64_t img_end = 0;
    int loads = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = elf + phoff + (uint64_t)i * phent;
        if (rd32(ph) != 1) continue;   // PT_LOAD (satoru)
        uint64_t off = rd64(ph + 8), pa = rd64(ph + 24), fsz = rd64(ph + 32), msz = rd64(ph + 40);
        if (msz == 0) continue;
        if (off + fsz > size || fsz > msz) { g_err = "segment outside the file"; return false; }
        if (pa < GUEST_HIGH_RAM_START || pa + msz > ram_top) { g_err = "segment outside guest RAM (give the guest more RAM)"; return false; }
        if (!copy_guest(pa, elf + off, fsz) || !zero_guest(pa + fsz, msz - fsz)) { g_err = "guest RAM write failed"; return false; }
        if (pa + msz > img_end) img_end = pa + msz;
        loads++;
    }
    if (!loads) { g_err = "no PT_LOAD segments"; return false; }

    // framebuffer: the top 4 MB of guest RAM, kept out of the memory map (satoru)
    uint64_t fb_len = (uint64_t)FB_PITCH * FB_H;
    uint64_t fb = (ram_top - (4ull << 20)) & ~((2ull << 20) - 1);
    if (fb <= img_end + (1ull << 20)) { g_err = "not enough guest RAM for the image + framebuffer"; return false; }
    zero_guest(fb, fb_len);
    g_fb = fb;

    // info goes one page past the image; the guest copies what it keeps (satoru)
    uint64_t info_gpa = (img_end + 0x1FFF) & ~0xFFFull;
    g_ilen = 8;   // total_size + reserved, filled in last
    for (int i = 0; i < 8; i++) g_info[i] = 0;
    {   // 1: command line
        uint8_t* t = tag_begin(1);
        for (int i = 0; cmdline && cmdline[i] && g_ilen < 1024; i++) put8((uint8_t)cmdline[i]);
        put8(0);
        tag_end(t);
    }
    {   // 4: basic memory info (KiB) (satoru)
        uint8_t* t = tag_begin(4);
        put32(640);
        put32((uint32_t)((fb - GUEST_HIGH_RAM_START) >> 10));
        tag_end(t);
    }
    {   // 6: memory map from the hypervisor's e820, framebuffer cut out (satoru)
        uint8_t* t = tag_begin(6);
        put32(24); put32(0);
        int n = GuestMemoryManager::GetE820Count();
        const E820Entry* e = GuestMemoryManager::GetE820Table();
        for (int i = 0; i < n && g_ilen + 48 < sizeof(g_info) - 256; i++) {
            uint64_t b = e[i].base, l = e[i].length, end = b + l;
            uint32_t ty = e[i].type;
            if (ty == 1 && b < fb + (4ull << 20) && end > fb) {
                if (fb > b) { put64(b); put64(fb - b); put32(1); put32(0); }
                put64(fb); put64(4ull << 20); put32(2); put32(0);
                uint64_t after = fb + (4ull << 20);
                if (end > after) { put64(after); put64(end - after); put32(1); put32(0); }
                continue;
            }
            put64(b); put64(l); put32(ty); put32(0);
        }
        tag_end(t);
    }
    {   // 8: framebuffer, direct rgb, 32 bpp, x8r8g8b8 (satoru)
        uint8_t* t = tag_begin(8);
        put64(fb); put32(FB_PITCH); put32(FB_W); put32(FB_H);
        put8((uint8_t)FB_BPP); put8(1); put8(0); put8(0);   // bpp, type rgb, reserved(2)
        put8(16); put8(8); put8(8); put8(8); put8(0); put8(8);   // red pos/size, green, blue
        put8(0); put8(0);
        tag_end(t);
    }
    {   // 0: end
        uint8_t* t = tag_begin(0);
        tag_end(t);
    }
    uint32_t total = g_ilen;
    g_info[0] = (uint8_t)total; g_info[1] = (uint8_t)(total >> 8); g_info[2] = (uint8_t)(total >> 16); g_info[3] = (uint8_t)(total >> 24);
    if (info_gpa + total >= fb || !GuestMemoryManager::WriteGuestPhys(info_gpa, g_info, total)) {
        g_err = "could not place the multiboot2 info"; return false;
    }
    *entry = (uint32_t)rd64(elf + 24);
    *mbi = (uint32_t)info_gpa;
    SerialLogger::Log("[mb2] guest image loaded: entry "); SerialLogger::LogHex(*entry);
    SerialLogger::Log(" end "); SerialLogger::LogHex64(img_end);
    SerialLogger::Log(" info "); SerialLogger::LogHex((uint32_t)info_gpa);
    SerialLogger::Log(" fb "); SerialLogger::LogHex64(fb);
    SerialLogger::Log("\r\n");
    g_err = "";
    return true;
}

// end (satoru)
