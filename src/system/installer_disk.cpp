// installer_disk.cpp: disk scan, gpt/mbr writing and a small fat32 writer for
// the graphical installer. this file is kernel only; the ring-3 app talks to
// installer_service.cpp over a unix socket. (satoru)
#include "installer_disk.h"
#include "../drivers/blockdev.h"
#include "../drivers/nvme.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"
#include "../kernel/kcrash.h"    // KBOOT.CFG image for a new install (satoru)

extern "C" {
    extern uint8_t _binary_kurono_installer_efi_efi_start[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_efi_efi_end[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_emergency_efi_start[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_emergency_efi_end[] __attribute__((weak));
}

namespace {

const uint64_t MIB = 1024ull * 1024ull;
const uint32_t MAX_GPT_ENTRIES = 128;
const uint64_t MIN_PART_BYTES = 300ull * MIB;
const uint64_t MAX_PART_BYTES = 4096ull * MIB;
const uint64_t LOG_BYTES = 16ull * MIB;
const uint64_t KFS_MAX = 3072ull * MIB;
// the partition must hold the EFI loader twice (KURONO.EFI and BOOTX64.EFI),
// the emergency loader, the log and the kfs. the exact payload sizes are
// known, so the reserve is computed below rather than guessed. (satoru)
const uint64_t FAT_OVERHEAD = 32ull * MIB;
const uint32_t FAT_EOC = 0x0FFFFFFFu;
const uint32_t FAT_FREE = 0;

#pragma pack(push, 1)
struct GptHdr {
    char     sig[8];
    uint32_t rev;
    uint32_t hsize;
    uint32_t hcrc;
    uint32_t reserved;
    uint64_t cur_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t nentries;
    uint32_t esize;
    uint32_t entries_crc;
};

struct GptEnt {
    uint8_t  type[16];
    uint8_t  unique[16];
    uint64_t first;
    uint64_t last;
    uint64_t attrs;
    uint16_t name[36];
};

struct MbrEnt {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t first_lba;
    uint32_t sectors;
};
#pragma pack(pop)

const uint8_t GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
const uint8_t GUID_LINUX_FS[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
const uint8_t GUID_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

// type guids of other operating systems (mixed-endian on-disk byte order), used
// only to recognise what is already on a disk so it is never touched by
// accident. (satoru)
const uint8_t GUID_MS_RESERVED[16] = {
    0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
    0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE
};
const uint8_t GUID_WIN_RE[16] = {
    0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D,
    0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC
};
const uint8_t GUID_LINUX_SWAP[16] = {
    0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
    0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F
};
const uint8_t GUID_LINUX_LVM[16] = {
    0x79, 0xD3, 0xD6, 0xE6, 0x07, 0xF5, 0xC2, 0x44,
    0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28
};
const uint8_t GUID_LINUX_ROOT[16] = {
    0xE3, 0xBC, 0x68, 0x4F, 0xCD, 0xE8, 0xB1, 0x4D,
    0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09
};
const uint8_t GUID_LINUX_HOME[16] = {
    0xE1, 0xC7, 0x3A, 0x93, 0xB4, 0x2E, 0x13, 0x4F,
    0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15
};
const uint8_t GUID_LINUX_LUKS[16] = {
    0xCB, 0x7C, 0x7D, 0xCA, 0xED, 0x63, 0x53, 0x4C,
    0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC
};
const uint8_t GUID_APPLE_HFS[16] = {
    0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,
    0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xC3
};
const uint8_t GUID_APPLE_APFS[16] = {
    0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
    0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xC3
};

struct PartX {
    KinPart p;
    uint8_t type_guid[16];
};

struct DiskX {
    int      dev;
    char     name[16];
    char     driver[16];
    char     model[64];
    uint64_t total_lba;
    uint32_t ssz;
    uint64_t bytes;
    bool     removable;
    uint32_t scheme;
    bool     gpt_ok;
    bool     has_gpt_sig;
    bool     protective;
    GptHdr   gpt;
    uint8_t  disk_guid[16];
    int      nparts;
    PartX    parts[KIN_MAX_PARTS];
    int      kurono_count;
    bool     has_other;
    uint32_t os_mask;      // KinOsMask bits found by scan_disk (satoru)
};

int s_len(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }

void s_cpy(char* d, const char* s, int mx) {
    if (!d || mx < 1) return;
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void s_cat(char* d, const char* s, int mx) {
    if (!d || mx < 1) return;
    int n = s_len(d);
    int i = 0;
    if (s) while (s[i] && n < mx - 1) d[n++] = s[i++];
    d[n] = 0;
}

bool s_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; i++; }
    return a[i] == 0 && b[i] == 0;
}

bool mem_eq(const void* a, const void* b, int n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (int i = 0; i < n; i++) if (pa[i] != pb[i]) return false;
    return true;
}

bool guid_zero(const uint8_t* g) {
    for (int i = 0; i < 16; i++) if (g[i]) return false;
    return true;
}

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

uint32_t crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

uint64_t g_guid_ctr = 0;
void make_guid(uint8_t g[16]) {
    uint64_t a = Timer::GetRealMs64() ^ (0x9E3779B97F4A7C15ull * (++g_guid_ctr));
    uint64_t b = a ^ (a >> 29) ^ 0xD1B54A32D192ED03ull;
    for (int i = 0; i < 2; i++) {
        a ^= a << 13; a ^= a >> 7; a ^= a << 17;
    }
    b ^= b << 13; b ^= b >> 7; b ^= b << 17;
    wr64(g, a);
    wr64(g + 8, b);
    g[6] = (uint8_t)((g[6] & 0x0F) | 0x40);   // rfc 4122 version 4 (satoru)
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

uint64_t align_up(uint64_t v, uint64_t a) { return a ? ((v + a - 1) / a) * a : v; }

void part_sep(char* name, int cap) {
    int n = s_len(name);
    if (n > 0 && n < cap - 1 && name[n - 1] >= '0' && name[n - 1] <= '9') {
        name[n++] = 'p';
        name[n] = 0;
    }
}

void base_name(const DiskX* d, int idx, char* out, int mx) {
    s_cpy(out, d->name, mx);
    part_sep(out, mx);
    char num[8];
    int n = 0;
    int v = idx;
    if (v == 0) num[n++] = '0';
    char rev[8];
    int rn = 0;
    while (v) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    while (rn) num[n++] = rev[--rn];
    num[n] = 0;
    s_cat(out, num, mx);
}

void utf16_name(const uint16_t* in, int n, char* out, int mx) {
    int p = 0;
    if (!out || mx < 1) return;
    for (int i = 0; i < n && p < mx - 1; i++) {
        uint16_t c = in[i];
        if (c == 0) break;
        out[p++] = (c < 128) ? (char)c : '?';
    }
    out[p] = 0;
}

void utf16_set(uint16_t* out, int n, const char* s) {
    int i = 0;
    for (; i < n && s && s[i]; i++) out[i] = (uint16_t)(uint8_t)s[i];
    for (; i < n; i++) out[i] = 0;
}

// ---- block helpers -------------------------------------------------------

// the single choke point for every raw write the installer makes. nothing may
// write until an install armed a list of allowed byte extents for exactly one
// disk; a write that is not fully inside one of them is refused and logged.
// the extents are the gpt areas and the new KURONO partition, never an existing
// partition. (satoru)
struct WrExt { uint64_t lo, hi; };   // byte offsets, hi exclusive (satoru)
WrExt g_ext[8];
int   g_next = 0;
int   g_ext_dev = -1;
uint32_t g_ext_refused = 0;

void ext_disarm() { g_next = 0; g_ext_dev = -1; }

void ext_arm(int dev) { g_next = 0; g_ext_dev = dev; }

struct ExtGuard {
    ~ExtGuard() { ext_disarm(); }
};

bool ext_add(uint32_t ssz, uint64_t first_lba, uint64_t last_lba) {
    if (g_next >= 8 || last_lba < first_lba) return false;
    g_ext[g_next].lo = first_lba * (uint64_t)ssz;
    g_ext[g_next].hi = (last_lba + 1) * (uint64_t)ssz;
    g_next++;
    return true;
}

// an allowed extent must not touch any partition that already exists, except
// the mbr sector which is not a partition. (satoru)
bool ext_clear_of_parts(const DiskX* d) {
    for (int e = 0; e < g_next; e++) {
        for (int i = 0; i < d->nparts; i++) {
            uint64_t plo = d->parts[i].p.start_lba * (uint64_t)d->ssz;
            uint64_t phi = (d->parts[i].p.last_lba + 1) * (uint64_t)d->ssz;
            if (g_ext[e].lo < phi && plo < g_ext[e].hi) return false;
        }
        if (g_ext[e].hi > d->bytes) return false;
    }
    return true;
}

bool disk_write(int dev, uint64_t off, uint32_t len, const void* buf) {
    uint64_t end = off + len;
    bool ok = (dev == g_ext_dev) && end > off;
    if (ok) {
        ok = false;
        for (int i = 0; i < g_next; i++) {
            if (off >= g_ext[i].lo && end <= g_ext[i].hi) { ok = true; break; }
        }
    }
    if (!ok) {
        if (g_ext_refused++ < 8) {
            SerialLogger::Log("[installer] REFUSED raw write outside the target extent off=");
            SerialLogger::LogHex64(off);
            SerialLogger::Log(" len=");
            SerialLogger::LogDec((int)len);
            SerialLogger::Log("\r\n");
        }
        return false;
    }
    return BlockDev::WriteBytes(dev, off, len, buf);
}

const char* fs_name_for(const uint8_t* boot) {
    if (boot[3] == 'N' && boot[4] == 'T' && boot[5] == 'F' && boot[6] == 'S') return "ntfs";
    if ((boot[82] == 'F' && boot[83] == 'A' && boot[84] == 'T' && boot[85] == '3' && boot[86] == '2') ||
        (boot[54] == 'F' && boot[55] == 'A' && boot[56] == 'T' && boot[57] == '3' && boot[58] == '2'))
        return "fat32";
    if (boot[1024 + 56] == 0x53 && boot[1024 + 57] == 0xEF) return "ext4";
    return "unknown";
}

void fs_label(const uint8_t* boot, const char* fs, char* out, int mx) {
    out[0] = 0;
    if (!fs) return;
    if (s_eq(fs, "fat32")) {
        char v[12];
        for (int i = 0; i < 11; i++) v[i] = (char)boot[71 + i];
        v[11] = 0;
        int n = 11;
        while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == 0)) { v[n - 1] = 0; n--; }
        s_cpy(out, v, mx);
    } else if (s_eq(fs, "ext4")) {
        char v[17];
        for (int i = 0; i < 16; i++) v[i] = (char)boot[1024 + 120 + i];
        v[16] = 0;
        s_cpy(out, v, mx);
    }
}

const char* type_name(const uint8_t* guid) {
    if (mem_eq(guid, GUID_ESP, 16)) return "EFI System";
    if (mem_eq(guid, GUID_LINUX_FS, 16)) return "Linux filesystem";
    if (mem_eq(guid, GUID_BASIC_DATA, 16)) return "Basic data";
    return "GPT partition";
}

// fill one partition row from its boot sector. only the first 4k are read. (satoru)
void detect_part_fs(int dev, uint32_t ssz, uint64_t start_lba, uint64_t sectors,
                    uint8_t* boot, KinPart* p) {
    s_cpy(p->fstype, "unknown", sizeof(p->fstype));
    p->label[0] = 0;
    if (sectors == 0) return;
    uint32_t want = ssz * 2;
    if (want < 2048) want = 2048;
    if (want > 8192) want = 8192;
    uint32_t cnt = want / ssz;
    if (cnt == 0) cnt = 1;
    if (cnt > sectors) cnt = (uint32_t)sectors;
    memset(boot, 0, 8192);
    if (!BlockDev::Read(dev, start_lba, cnt, boot)) return;
    const char* fs = fs_name_for(boot);
    s_cpy(p->fstype, fs, sizeof(p->fstype));
    fs_label(boot, fs, p->label, sizeof(p->label));
}

bool name_is_kurono(const char* n) { return n && s_eq(n, "KURONO"); }

// ---- other operating system detection -------------------------------------
// read only. looks for what is already on a disk so the installer can say so
// and refuse to rewrite it without a second confirmation. (satoru)

// true when the fat32 volume at start_lba has \EFI\Microsoft in its root. only
// the first cluster of each directory is read, which is where it always is on
// a windows esp. (satoru)
bool esp_find_dir(int dev, uint32_t ssz, uint64_t data_lba, uint32_t spc, uint32_t cluster,
                  const char* prefix, uint32_t* next) {
    uint32_t bytes = spc * ssz;
    if (spc == 0 || bytes == 0 || bytes > 65536 || cluster < 2) return false;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(bytes);
    if (!buf) return false;
    bool found = false;
    if (BlockDev::Read(dev, data_lba + (uint64_t)(cluster - 2) * spc, spc, buf)) {
        int pl = s_len(prefix);
        for (uint32_t o = 0; o + 32 <= bytes; o += 32) {
            const uint8_t* e = buf + o;
            if (e[0] == 0) break;
            if (e[0] == 0xE5 || e[11] == 0x0F || !(e[11] & 0x10)) continue;
            bool m = true;
            for (int i = 0; i < pl; i++) if (e[i] != (uint8_t)prefix[i]) { m = false; break; }
            if (!m) continue;
            if (next) *next = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (uint32_t)(e[26] | (e[27] << 8));
            found = true;
            break;
        }
    }
    KernelHeap::Free(buf);
    return found;
}

bool esp_has_microsoft(int dev, uint32_t ssz, uint64_t start_lba, const uint8_t* boot) {
    if (!(boot[82] == 'F' && boot[83] == 'A' && boot[84] == 'T' && boot[85] == '3' && boot[86] == '2'))
        return false;
    uint32_t bps = (uint32_t)boot[11] | ((uint32_t)boot[12] << 8);
    if (bps != ssz) return false;
    uint32_t spc = boot[13];
    uint32_t rsv = (uint32_t)boot[14] | ((uint32_t)boot[15] << 8);
    uint32_t nf = boot[16];
    uint32_t fsz = rd32(boot + 36);
    uint32_t root = rd32(boot + 44);
    uint64_t data_lba = start_lba + rsv + (uint64_t)nf * fsz;
    uint32_t efi = 0;
    if (!esp_find_dir(dev, ssz, data_lba, spc, root, "EFI", &efi)) return false;
    return esp_find_dir(dev, ssz, data_lba, spc, efi, "MICROS", nullptr);
}

bool sig_at(const uint8_t* b, int off, const char* s8) {
    for (int i = 0; s8[i]; i++) if (b[off + i] != (uint8_t)s8[i]) return false;
    return true;
}

// classify one existing (not KURONO) partition. boot holds its first sectors,
// already read by detect_part_fs. (satoru)
uint32_t classify_part(int dev, uint32_t ssz, const PartX* px, bool is_gpt,
                       uint8_t mbr_type, const uint8_t* boot) {
    uint32_t m = 0;
    const char* fs = px->p.fstype;
    bool ntfs = sig_at(boot, 3, "NTFS    ");
    bool fve = sig_at(boot, 3, "-FVE-FS-");
    bool exfat = sig_at(boot, 3, "EXFAT   ");
    bool nxsb = sig_at(boot, 32, "NXSB");
    bool esp = (px->p.flags & KIN_PART_ESP) != 0;
    if (ntfs || fve) m |= KIN_OS_WINDOWS;
    if (s_eq(fs, "ext4")) m |= KIN_OS_LINUX;
    if (nxsb) m |= KIN_OS_MAC;
    if (is_gpt) {
        const uint8_t* g = px->type_guid;
        if (mem_eq(g, GUID_MS_RESERVED, 16) || mem_eq(g, GUID_WIN_RE, 16)) m |= KIN_OS_WINDOWS;
        // microsoft basic data holds windows volumes, but a plain fat32 or
        // exfat one is just a data stick. (satoru)
        if (mem_eq(g, GUID_BASIC_DATA, 16) && !s_eq(fs, "fat32") && !exfat) m |= KIN_OS_WINDOWS;
        if (mem_eq(g, GUID_LINUX_FS, 16) || mem_eq(g, GUID_LINUX_SWAP, 16) ||
            mem_eq(g, GUID_LINUX_LVM, 16) || mem_eq(g, GUID_LINUX_ROOT, 16) ||
            mem_eq(g, GUID_LINUX_HOME, 16) || mem_eq(g, GUID_LINUX_LUKS, 16)) m |= KIN_OS_LINUX;
        if (mem_eq(g, GUID_APPLE_HFS, 16) || mem_eq(g, GUID_APPLE_APFS, 16)) m |= KIN_OS_MAC;
    } else {
        if (mbr_type == 0x07 && !exfat) m |= KIN_OS_WINDOWS;
        if (mbr_type == 0x27 || mbr_type == 0x42) m |= KIN_OS_WINDOWS;
        if (mbr_type == 0x82 || mbr_type == 0x83 || mbr_type == 0x8E || mbr_type == 0xFD) m |= KIN_OS_LINUX;
        if (mbr_type == 0xAF || mbr_type == 0xA8 || mbr_type == 0xAB) m |= KIN_OS_MAC;
    }
    if (esp && s_eq(fs, "fat32") && esp_has_microsoft(dev, ssz, px->p.start_lba, boot))
        m |= KIN_OS_WINDOWS;
    return m;
}

// a disk with no partition table at all may still hold a whole-disk
// filesystem; writing a gpt over it would destroy it. sec is lba 0. (satoru)
uint32_t classify_whole(int dev, const uint8_t* sec) {
    uint32_t m = 0;
    if (sig_at(sec, 3, "NTFS    ") || sig_at(sec, 3, "-FVE-FS-")) m |= KIN_OS_WINDOWS | KIN_OS_WHOLE;
    if (sig_at(sec, 32, "NXSB")) m |= KIN_OS_MAC | KIN_OS_WHOLE;
    if (sig_at(sec, 3, "EXFAT   ")) m |= KIN_OS_WHOLE;
    if (sec[510] == 0x55 && sec[511] == 0xAA &&
        (sig_at(sec, 82, "FAT32") || sig_at(sec, 54, "FAT16") || sig_at(sec, 54, "FAT12")))
        m |= KIN_OS_WHOLE;
    uint8_t sb[64];
    if (BlockDev::ReadBytes(dev, 1024, 64, sb) && sb[56] == 0x53 && sb[57] == 0xEF)
        m |= KIN_OS_LINUX | KIN_OS_WHOLE;
    return m;
}

// "This disk has Windows and Linux on it". empty when nothing was found. (satoru)
void os_line_for(uint32_t mask, char* out, int cap) {
    out[0] = 0;
    uint32_t os = mask & (KIN_OS_WINDOWS | KIN_OS_MAC | KIN_OS_LINUX);
    if (!os && !(mask & KIN_OS_WHOLE)) return;
    if (!os) { s_cpy(out, "This disk has files on it", cap); return; }
    const char* names[3] = { "Windows", "Mac", "Linux" };
    uint32_t bits[3] = { KIN_OS_WINDOWS, KIN_OS_MAC, KIN_OS_LINUX };
    int n = 0, seen = 0;
    for (int i = 0; i < 3; i++) if (os & bits[i]) n++;
    s_cpy(out, "This disk has ", cap);
    for (int i = 0; i < 3; i++) {
        if (!(os & bits[i])) continue;
        if (seen > 0) s_cat(out, seen == n - 1 ? " and " : ", ", cap);
        s_cat(out, names[i], cap);
        seen++;
    }
    s_cat(out, " on it", cap);
}

bool scan_disk(int dev, DiskX* d) {
    BlockDevice* bd = BlockDev::Get(dev);
    if (!bd) return false;
    memset(d, 0, sizeof(*d));
    d->dev = dev;
    d->ssz = bd->sector_size ? bd->sector_size : 512;
    if (d->ssz < 512 || d->ssz > 4096) return false;
    d->total_lba = bd->capacity_lba;
    if (d->total_lba < 64) return false;
    d->bytes = d->total_lba * (uint64_t)d->ssz;
    d->removable = bd->removable;
    s_cpy(d->name, bd->name, sizeof(d->name));
    s_cpy(d->driver, bd->driver, sizeof(d->driver));
    s_cpy(d->model, bd->model, sizeof(d->model));
    d->scheme = KIN_SCHEME_NONE;

    uint8_t* sec = (uint8_t*)KernelHeap::Alloc(d->ssz);
    if (!sec) return false;
    if (!BlockDev::Read(dev, 0, 1, sec)) { KernelHeap::Free(sec); return false; }
    bool mbr_sig = (sec[510] == 0x55 && sec[511] == 0xAA);
    MbrEnt* mbr = (MbrEnt*)(sec + 446);
    bool protective = false;
    bool mbr_used = false;
    for (int i = 0; i < 4; i++) {
        if (mbr[i].type == 0xEE) protective = true;
        if (mbr[i].type != 0 && mbr[i].sectors != 0) mbr_used = true;
    }
    d->protective = protective;

    uint8_t* hdr = (uint8_t*)KernelHeap::Alloc(d->ssz);
    uint8_t* boot = (uint8_t*)KernelHeap::Alloc(8192);
    if (!hdr || !boot) {
        if (hdr) KernelHeap::Free(hdr);
        if (boot) KernelHeap::Free(boot);
        KernelHeap::Free(sec);
        return false;
    }

    bool gpt_sig = false;
    if (mbr_sig && d->total_lba > 1 && BlockDev::Read(dev, 1, 1, hdr)) {
        gpt_sig = mem_eq(hdr, "EFI PART", 8);
    }
    d->has_gpt_sig = gpt_sig;
    if (protective && gpt_sig) {
        GptHdr* g = (GptHdr*)hdr;
        bool ok = (g->rev == 0x00010000u) &&
                  (g->hsize >= 92 && g->hsize <= d->ssz) &&
                  (g->cur_lba == 1) &&
                  (g->backup_lba > 1 && g->backup_lba < d->total_lba) &&
                  (g->first_usable > 0 && g->last_usable >= g->first_usable &&
                   g->last_usable < d->total_lba) &&
                  (g->nentries > 0 && g->nentries <= MAX_GPT_ENTRIES) &&
                  (g->esize == 128) &&
                  (g->entries_lba > 1 && g->entries_lba + (g->nentries * g->esize + d->ssz - 1) / d->ssz <= d->total_lba);
        if (!ok) {
            d->scheme = KIN_SCHEME_BAD;
        } else {
            memcpy(&d->gpt, g, sizeof(GptHdr));
            memcpy(d->disk_guid, g->disk_guid, 16);
            d->gpt_ok = true;
            d->scheme = KIN_SCHEME_GPT;
            uint64_t entries_bytes = (uint64_t)g->nentries * g->esize;
            uint8_t* ent = (uint8_t*)KernelHeap::Alloc(128);
            if (ent) {
                for (uint32_t i = 0; i < g->nentries && d->nparts < KIN_MAX_PARTS; i++) {
                    uint64_t off = g->entries_lba * (uint64_t)d->ssz + (uint64_t)i * g->esize;
                    if (!BlockDev::ReadBytes(dev, off, 128, ent)) break;
                    GptEnt* e = (GptEnt*)ent;
                    if (guid_zero(e->type) || e->first == 0 || e->last < e->first) continue;
                    if (e->last >= d->total_lba) continue;
                    PartX* px = &d->parts[d->nparts];
                    memset(px, 0, sizeof(*px));
                    memcpy(px->type_guid, e->type, 16);
                    base_name(d, i + 1, px->p.name, sizeof(px->p.name));
                    utf16_name(e->name, 36, px->p.label, sizeof(px->p.label));
                    px->p.start_lba = e->first;
                    px->p.last_lba = e->last;
                    px->p.bytes = (e->last - e->first + 1) * (uint64_t)d->ssz;
                    s_cpy(px->p.typestr, type_name(e->type), sizeof(px->p.typestr));
                    detect_part_fs(dev, d->ssz, e->first, e->last - e->first + 1, boot, &px->p);
                    if (mem_eq(e->type, GUID_ESP, 16)) px->p.flags |= KIN_PART_ESP;
                    char gname[40];
                    utf16_name(e->name, 36, gname, sizeof(gname));
                    if (name_is_kurono(gname) || name_is_kurono(px->p.label)) {
                        px->p.flags |= KIN_PART_KURONO;
                        d->kurono_count++;
                    } else {
                        d->has_other = true;
                        d->os_mask |= classify_part(dev, d->ssz, px, true, 0, boot);
                    }
                    if (s_eq(px->p.fstype, "ntfs") || s_eq(px->p.fstype, "ext4"))
                        px->p.flags |= KIN_PART_OTHER_OS;
                    d->nparts++;
                }
                KernelHeap::Free(ent);
            }
            (void)entries_bytes;
        }
    } else if (mbr_sig && mbr_used) {
        d->scheme = KIN_SCHEME_MBR;
        for (int i = 0; i < 4 && d->nparts < KIN_MAX_PARTS; i++) {
            if (mbr[i].type == 0 || mbr[i].sectors == 0) continue;
            PartX* px = &d->parts[d->nparts];
            memset(px, 0, sizeof(*px));
            px->p.start_lba = mbr[i].first_lba;
            px->p.last_lba = (uint64_t)mbr[i].first_lba + mbr[i].sectors - 1;
            if (px->p.last_lba >= d->total_lba) continue;
            px->p.bytes = (uint64_t)mbr[i].sectors * (uint64_t)d->ssz;
            base_name(d, i + 1, px->p.name, sizeof(px->p.name));
            if (mbr[i].status == 0x80) px->p.flags |= KIN_PART_BOOTABLE;
            if (mbr[i].type == 0xEF) px->p.flags |= KIN_PART_ESP;
            detect_part_fs(dev, d->ssz, px->p.start_lba, mbr[i].sectors, boot, &px->p);
            char tn[24];
            s_cpy(tn, "MBR type 0x", sizeof(tn));
            const char* hex = "0123456789abcdef";
            char hx[3] = { hex[(mbr[i].type >> 4) & 0xF], hex[mbr[i].type & 0xF], 0 };
            s_cat(tn, hx, sizeof(tn));
            s_cpy(px->p.typestr, tn, sizeof(px->p.typestr));
            if (name_is_kurono(px->p.label)) {
                px->p.flags |= KIN_PART_KURONO;
                d->kurono_count++;
            } else {
                d->has_other = true;
                d->os_mask |= classify_part(dev, d->ssz, px, false, mbr[i].type, boot);
            }
            if (s_eq(px->p.fstype, "ntfs") || s_eq(px->p.fstype, "ext4"))
                px->p.flags |= KIN_PART_OTHER_OS;
            d->nparts++;
        }
    } else {
        d->scheme = KIN_SCHEME_NONE;
        d->os_mask |= classify_whole(dev, sec);
    }

    KernelHeap::Free(boot);
    KernelHeap::Free(hdr);
    KernelHeap::Free(sec);
    return true;
}

// the usb stick (or any removable disk) that carries a KURONO partition is the
// medium kurono booted from; it is never an install target. (satoru)
bool is_running_disk(const DiskX* d) {
    return d->removable && d->kurono_count > 0;
}

void fill_disk(const DiskX* d, KinDisk* out) {
    memset(out, 0, sizeof(*out));
    out->index = d->dev;
    out->present = 1;
    s_cpy(out->name, d->name, sizeof(out->name));
    s_cpy(out->driver, d->driver, sizeof(out->driver));
    s_cpy(out->model, d->model, sizeof(out->model));
    out->bytes = d->bytes;
    out->sector_size = d->ssz;
    out->scheme = d->scheme;
    out->part_count = (uint32_t)d->nparts;
    if (d->kurono_count > 0) out->flags |= KIN_DISK_KURONO;
    if (d->has_other) out->flags |= KIN_DISK_OTHER_OS;
    if (d->removable) out->flags |= KIN_DISK_REMOVABLE;
    if (d->scheme == KIN_SCHEME_BAD) out->flags |= KIN_DISK_BADGPT;
    if (d->os_mask & KIN_OS_WINDOWS) out->flags |= KIN_DISK_WINDOWS;
    if (d->os_mask & KIN_OS_MAC) out->flags |= KIN_DISK_MAC;
    if (d->os_mask & KIN_OS_LINUX) out->flags |= KIN_DISK_LINUX;
    if (is_running_disk(d)) out->flags |= KIN_DISK_RUNNING;
    if (NVMe::WriteGuardOn()) out->flags |= KIN_DISK_READONLY;
}

void copy_parts(const DiskX* d, KinPlan* p) {
    p->parts_n = (uint32_t)d->nparts;
    for (int i = 0; i < d->nparts; i++) p->parts[i] = d->parts[i].p;
}

void plan_refuse(KinPlan* p, const char* why) {
    p->mode = KIN_PLAN_REFUSE;
    p->ok = 0;
    s_cpy(p->why, why, sizeof(p->why));
}

bool choose_gap(const DiskX* d, uint64_t first_usable, uint64_t last_usable,
                uint64_t* out_start, uint64_t* out_last) {
    uint64_t starts[KIN_MAX_PARTS];
    uint64_t lasts[KIN_MAX_PARTS];
    int n = 0;
    for (int i = 0; i < d->nparts; i++) {
        starts[n] = d->parts[i].p.start_lba;
        lasts[n] = d->parts[i].p.last_lba;
        n++;
    }
    for (int i = 1; i < n; i++) {
        uint64_t s = starts[i], l = lasts[i];
        int j = i - 1;
        while (j >= 0 && starts[j] > s) { starts[j + 1] = starts[j]; lasts[j + 1] = lasts[j]; j--; }
        starts[j + 1] = s; lasts[j + 1] = l;
    }
    uint64_t align = (MIB + d->ssz - 1) / d->ssz;
    uint64_t cursor = first_usable;
    uint64_t best_s = 0, best_l = 0, best_len = 0;
    for (int i = 0; i <= n; i++) {
        uint64_t gap_last = (i < n) ? (starts[i] > 0 ? starts[i] - 1 : 0) : last_usable;
        uint64_t gap_first = cursor;
        if (gap_last >= gap_first) {
            uint64_t s = align_up(gap_first, align);
            if (s < gap_first) s = gap_first;
            if (s <= gap_last) {
                uint64_t len = gap_last - s + 1;
                if (len > best_len) { best_len = len; best_s = s; best_l = gap_last; }
            }
        }
        if (i < n && lasts[i] + 1 > cursor) cursor = lasts[i] + 1;
    }
    uint64_t min_sectors = (MIN_PART_BYTES + d->ssz - 1) / d->ssz;
    if (best_len < min_sectors) return false;
    uint64_t max_sectors = MAX_PART_BYTES / d->ssz;
    if (best_len > max_sectors) best_l = best_s + max_sectors - 1;
    // end on the same 1 mib boundary sgdisk uses, so tools do not warn about
    // an unaligned partition end. (satoru)
    uint64_t aligned_last = ((best_l + 1) / align) * align - 1;
    if (aligned_last >= best_s && aligned_last < best_l) best_l = aligned_last;
    *out_start = best_s;
    *out_last = best_l;
    return true;
}

// ---- gpt writing ---------------------------------------------------------

struct GptPlan {
    uint32_t scheme_mode;
    uint64_t entries_lba;
    uint64_t backup_lba;
    uint64_t backup_entries_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t nentries;
    uint8_t  disk_guid[16];
    uint8_t* entries;      // nentries * 128 bytes (satoru)
    uint32_t entries_bytes;
    uint32_t entries_crc;
};

bool build_entries_new(const DiskX* d, const KinPlan* p, GptPlan* gp) {
    (void)d;
    memset(gp, 0, sizeof(*gp));
    gp->nentries = MAX_GPT_ENTRIES;
    gp->entries_bytes = gp->nentries * 128;
    gp->entries = (uint8_t*)KernelHeap::Alloc(gp->entries_bytes);
    if (!gp->entries) return false;
    memset(gp->entries, 0, gp->entries_bytes);
    GptEnt* e = (GptEnt*)gp->entries;
    memcpy(e->type, GUID_BASIC_DATA, 16);
    make_guid(e->unique);
    e->first = p->part_start_lba;
    e->last = p->part_last_lba;
    e->attrs = 0;
    utf16_set(e->name, 36, "KURONO");
    gp->entries_crc = crc32(gp->entries, gp->entries_bytes);
    return true;
}

bool build_entries_convert(const DiskX* d, const KinPlan* p, GptPlan* gp) {
    memset(gp, 0, sizeof(*gp));
    gp->nentries = MAX_GPT_ENTRIES;
    gp->entries_bytes = gp->nentries * 128;
    gp->entries = (uint8_t*)KernelHeap::Alloc(gp->entries_bytes);
    if (!gp->entries) return false;
    memset(gp->entries, 0, gp->entries_bytes);
    GptEnt* arr = (GptEnt*)gp->entries;
    for (int i = 0; i < d->nparts; i++) {
        const KinPart* src = &d->parts[i].p;
        GptEnt* e = &arr[i];
        memcpy(e->type, d->parts[i].type_guid, 16);
        if (guid_zero(e->type)) {
            if (src->flags & KIN_PART_ESP) memcpy(e->type, GUID_ESP, 16);
            else if (s_eq(src->fstype, "ext4")) memcpy(e->type, GUID_LINUX_FS, 16);
            else memcpy(e->type, GUID_BASIC_DATA, 16);
        }
        make_guid(e->unique);
        e->first = src->start_lba;
        e->last = src->last_lba;
        e->attrs = (src->flags & KIN_PART_BOOTABLE) ? 4ull : 0ull;
        if (src->flags & KIN_PART_KURONO) utf16_set(e->name, 36, "KURONO");
        else if (src->label[0]) utf16_set(e->name, 36, src->label);
        else {
            char nm[24];
            s_cpy(nm, "Partition ", sizeof(nm));
            char num[4]; int nn = 0; int v = i + 1;
            char rev[4]; int rn = 0;
            while (v) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
            while (rn) num[nn++] = rev[--rn];
            num[nn] = 0;
            s_cat(nm, num, sizeof(nm));
            utf16_set(e->name, 36, nm);
        }
    }
    int slot = d->nparts;
    if (slot >= (int)gp->nentries) return false;
    GptEnt* e = &arr[slot];
    memcpy(e->type, GUID_BASIC_DATA, 16);
    make_guid(e->unique);
    e->first = p->part_start_lba;
    e->last = p->part_last_lba;
    utf16_set(e->name, 36, "KURONO");
    gp->entries_crc = crc32(gp->entries, gp->entries_bytes);
    return true;
}

bool build_entries_append(const DiskX* d, const KinPlan* p, GptPlan* gp) {
    memset(gp, 0, sizeof(*gp));
    const GptHdr* h = &d->gpt;
    gp->nentries = h->nentries;
    gp->entries_bytes = gp->nentries * h->esize;
    gp->entries = (uint8_t*)KernelHeap::Alloc(gp->entries_bytes);
    if (!gp->entries) return false;
    if (!BlockDev::ReadBytes(d->dev, h->entries_lba * (uint64_t)d->ssz,
                             gp->entries_bytes, gp->entries)) {
        KernelHeap::Free(gp->entries);
        gp->entries = nullptr;
        return false;
    }
    int slot = -1;
    for (uint32_t i = 0; i < gp->nentries; i++) {
        GptEnt* e = (GptEnt*)(gp->entries + i * h->esize);
        if (guid_zero(e->type)) { slot = (int)i; break; }
    }
    if (slot < 0) {
        KernelHeap::Free(gp->entries);
        gp->entries = nullptr;
        return false;
    }
    GptEnt* e = (GptEnt*)(gp->entries + slot * h->esize);
    memcpy(e->type, GUID_BASIC_DATA, 16);
    make_guid(e->unique);
    e->first = p->part_start_lba;
    e->last = p->part_last_lba;
    e->attrs = 0;
    utf16_set(e->name, 36, "KURONO");
    gp->entries_crc = crc32(gp->entries, gp->entries_bytes);
    return true;
}

void fill_header(GptHdr* h, uint64_t cur, uint64_t backup, uint64_t first_usable,
                 uint64_t last_usable, const uint8_t guid[16], uint64_t entries_lba,
                 uint32_t nentries, uint32_t esize, uint32_t ecrc) {
    memset(h, 0, sizeof(*h));
    memcpy(h->sig, "EFI PART", 8);
    h->rev = 0x00010000u;
    h->hsize = 92;
    h->cur_lba = cur;
    h->backup_lba = backup;
    h->first_usable = first_usable;
    h->last_usable = last_usable;
    memcpy(h->disk_guid, guid, 16);
    h->entries_lba = entries_lba;
    h->nentries = nentries;
    h->esize = esize;
    h->entries_crc = ecrc;
    h->hcrc = 0;
    h->hcrc = crc32((const uint8_t*)h, sizeof(GptHdr));
}

bool write_gpt(const DiskX* d, const KinPlan* p, char* why, int cap) {
    GptPlan gp;
    memset(&gp, 0, sizeof(gp));
    bool ok = false;
    if (p->mode == KIN_PLAN_CREATE) ok = build_entries_new(d, p, &gp);
    else if (p->mode == KIN_PLAN_CONVERT) ok = build_entries_convert(d, p, &gp);
    else if (p->mode == KIN_PLAN_APPEND) ok = build_entries_append(d, p, &gp);
    if (!ok) {
        s_cpy(why, "could not build the gpt entry table", cap);
        return false;
    }

    uint32_t entry_sectors = (gp.entries_bytes + d->ssz - 1) / d->ssz;
    uint64_t entries_lba, backup_lba, backup_entries_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    memcpy(disk_guid, d->disk_guid, 16);
    if (p->mode == KIN_PLAN_APPEND) {
        entries_lba = d->gpt.entries_lba;
        backup_lba = d->gpt.backup_lba;
        backup_entries_lba = (backup_lba > entry_sectors + 1) ? backup_lba - entry_sectors : 0;
        first_usable = d->gpt.first_usable;
        last_usable = d->gpt.last_usable;
        if (!backup_entries_lba) {
            KernelHeap::Free(gp.entries);
            s_cpy(why, "backup gpt has no room", cap);
            return false;
        }
    } else {
        entries_lba = 2;
        backup_lba = d->total_lba - 1;
        backup_entries_lba = backup_lba - entry_sectors;
        first_usable = 2 + entry_sectors;
        if (first_usable < 34) first_usable = 34;
        last_usable = backup_entries_lba - 1;
        if (!guid_zero(disk_guid)) {
            // keep an existing gpt disk guid on conversion if one was found (satoru)
        } else {
            make_guid(disk_guid);
        }
    }
    if (last_usable <= first_usable || last_usable >= d->total_lba) {
        KernelHeap::Free(gp.entries);
        s_cpy(why, "disk has no room for a gpt", cap);
        return false;
    }

    // only the table areas of this disk may be written now: the entry arrays,
    // both headers and (for a new table) the protective mbr. (satoru)
    ext_arm(d->dev);
    bool armed = ext_add(d->ssz, entries_lba, entries_lba + entry_sectors - 1) &&
                 ext_add(d->ssz, backup_entries_lba, backup_entries_lba + entry_sectors - 1) &&
                 ext_add(d->ssz, backup_lba, backup_lba) &&
                 ext_add(d->ssz, 1, 1) &&
                 (p->mode == KIN_PLAN_APPEND || ext_add(d->ssz, 0, 0)) &&
                 ext_clear_of_parts(d);
    if (!armed) {
        KernelHeap::Free(gp.entries);
        s_cpy(why, "the partition table area overlaps a partition; refusing to write", cap);
        return false;
    }

    // write the entry arrays first, then the backup header, then the primary
    // header, and for a new table the protective mbr last: a crash at any point
    // leaves the old table readable. (satoru)
    bool w = disk_write(d->dev, entries_lba * (uint64_t)d->ssz, gp.entries_bytes, gp.entries);
    if (w) w = disk_write(d->dev, backup_entries_lba * (uint64_t)d->ssz, gp.entries_bytes, gp.entries);
    if (!w) {
        KernelHeap::Free(gp.entries);
        s_cpy(why, "writing the gpt entries failed (disk write guard?)", cap);
        return false;
    }

    GptHdr primary, backup;
    fill_header(&primary, 1, backup_lba, first_usable, last_usable, disk_guid,
                entries_lba, gp.nentries, 128, gp.entries_crc);
    fill_header(&backup, backup_lba, 1, first_usable, last_usable, disk_guid,
                backup_entries_lba, gp.nentries, 128, gp.entries_crc);
    w = disk_write(d->dev, backup_lba * (uint64_t)d->ssz, sizeof(backup), &backup);
    if (w) w = disk_write(d->dev, (uint64_t)d->ssz, sizeof(primary), &primary);
    if (!w) {
        KernelHeap::Free(gp.entries);
        s_cpy(why, "writing the gpt header failed (disk write guard?)", cap);
        return false;
    }

    if (p->mode != KIN_PLAN_APPEND) {
        uint8_t* mbr = (uint8_t*)KernelHeap::Alloc(d->ssz);
        if (!mbr) {
            KernelHeap::Free(gp.entries);
            s_cpy(why, "out of memory writing the protective mbr", cap);
            return false;
        }
        memset(mbr, 0, d->ssz);
        MbrEnt* e = (MbrEnt*)(mbr + 446);
        e->type = 0xEE;
        e->first_lba = 1;
        uint64_t mbr_sectors = d->total_lba - 1;
        if (d->ssz > 512) mbr_sectors = (d->bytes / 512ull) - 1;
        if (mbr_sectors > 0xFFFFFFFFull) mbr_sectors = 0xFFFFFFFFull;
        e->sectors = (uint32_t)mbr_sectors;
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        w = disk_write(d->dev, 0, d->ssz, mbr);
        KernelHeap::Free(mbr);
        if (!w) {
            KernelHeap::Free(gp.entries);
            s_cpy(why, "writing the protective mbr failed (disk write guard?)", cap);
            return false;
        }
    }

    BlockDev::Flush(d->dev);
    KernelHeap::Free(gp.entries);

    // read the primary header back and check the crc. (satoru)
    uint8_t* chk = (uint8_t*)KernelHeap::Alloc(d->ssz);
    if (chk) {
        bool rok = BlockDev::Read(d->dev, 1, 1, chk);
        GptHdr* h = (GptHdr*)chk;
        uint32_t want = h->hcrc;
        h->hcrc = 0;
        bool crc_ok = rok && mem_eq(h->sig, "EFI PART", 8) && crc32(chk, sizeof(GptHdr)) == want;
        KernelHeap::Free(chk);
        if (!crc_ok) {
            s_cpy(why, "gpt verification failed after writing", cap);
            return false;
        }
    }
    return true;
}

// ---- fat32 writer --------------------------------------------------------

struct FatDir {
    uint32_t cluster;
    uint8_t* buf;
    int      used;
};

struct FatVol {
    int      dev;
    uint32_t ssz;
    uint64_t part_lba;
    uint32_t total_sectors;
    uint32_t reserved;
    uint32_t spc;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t cluster_bytes;
    uint32_t clusters;
    uint32_t next;
    uint32_t free_clusters;
    uint64_t data_off;
    uint64_t fat_off;
    uint8_t* fat;
    uint32_t fat_bytes;
    uint8_t* zero;
    uint32_t zero_bytes;
    FatDir   dirs[8];
    int      ndirs;
};

uint64_t fat_cluster_off(const FatVol* f, uint32_t c) {
    return f->data_off + (uint64_t)(c - 2) * f->cluster_bytes;
}

bool fat_write(FatVol* f, uint64_t off, uint32_t len, const void* buf) {
    return disk_write(f->dev, f->part_lba * (uint64_t)f->ssz + off, len, buf);
}

bool fat_zero(FatVol* f, uint64_t off, uint64_t len) {
    if (!f->zero) return false;
    while (len > 0) {
        uint32_t n = (len > f->zero_bytes) ? f->zero_bytes : (uint32_t)len;
        if (!fat_write(f, off, n, f->zero)) return false;
        off += n;
        len -= n;
    }
    return true;
}

void fat_set(FatVol* f, uint32_t c, uint32_t v) {
    if (!f->fat || c >= f->clusters + 2) return;
    wr32(f->fat + (uint64_t)c * 4, v & 0x0FFFFFFFu);
}

uint32_t fat_get(FatVol* f, uint32_t c) {
    if (!f->fat || c >= f->clusters + 2) return FAT_EOC;
    return rd32(f->fat + (uint64_t)c * 4) & 0x0FFFFFFFu;
}

uint32_t fat_alloc_run(FatVol* f, uint32_t n) {
    if (n == 0) return 0;
    if ((uint64_t)f->next + n > (uint64_t)f->clusters + 2) return 0;
    uint32_t start = f->next;
    for (uint32_t i = 0; i < n; i++)
        fat_set(f, start + i, (i + 1 == n) ? FAT_EOC : (start + i + 1));
    f->next += n;
    if (f->free_clusters >= n) f->free_clusters -= n;
    return start;
}

void short_name(const char* src, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!src) return;
    int last_dot = -1;
    for (int i = 0; src[i]; i++) if (src[i] == '.') last_dot = i;
    int di = 0, ei = 8;
    bool ext = false;
    for (int i = 0; src[i]; i++) {
        if (i == last_dot) { ext = true; continue; }
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~') {
            if (!ext && di < 8) out[di++] = (uint8_t)c;
            else if (ext && ei < 11) out[ei++] = (uint8_t)c;
        }
    }
}

FatDir* fat_dir_new(FatVol* f, uint32_t cluster) {
    if (f->ndirs >= (int)(sizeof(f->dirs) / sizeof(f->dirs[0]))) return nullptr;
    FatDir* d = &f->dirs[f->ndirs];
    d->cluster = cluster;
    d->used = 0;
    d->buf = (uint8_t*)KernelHeap::Alloc(f->cluster_bytes);
    if (!d->buf) return nullptr;
    memset(d->buf, 0, f->cluster_bytes);
    f->ndirs++;
    return d;
}

FatDir* fat_dir_find(FatVol* f, uint32_t cluster) {
    for (int i = 0; i < f->ndirs; i++) if (f->dirs[i].cluster == cluster) return &f->dirs[i];
    return nullptr;
}

void fat_entry(uint8_t* e, const uint8_t name[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attr;
    wr16(e + 20, (uint16_t)(cluster >> 16));
    wr16(e + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(e + 28, size);
    // a fixed 2026-01-01 timestamp keeps the image deterministic. (satoru)
    wr16(e + 14, 0);
    wr16(e + 16, (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1));
    wr16(e + 18, (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1));
    wr16(e + 22, 0);
    wr16(e + 24, (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1));
}

bool fat_dir_add(FatVol* f, FatDir* d, const char* name, uint8_t attr,
                 uint32_t cluster, uint32_t size) {
    if (!d || d->used < 0 || d->used + 32 > (int)f->cluster_bytes) return false;
    uint8_t nm[11];
    short_name(name, nm);
    fat_entry(d->buf + d->used, nm, attr, cluster, size);
    d->used += 32;
    return true;
}

bool fat_dir_dots(FatVol* f, FatDir* d, uint32_t parent_cluster) {
    uint8_t dot[11];
    for (int i = 0; i < 11; i++) dot[i] = ' ';
    dot[0] = '.';
    fat_entry(d->buf, dot, 0x10, d->cluster, 0); d->used += 32;
    dot[1] = '.';
    uint32_t dotdot = (parent_cluster == 2) ? 0 : parent_cluster;
    fat_entry(d->buf + d->used, dot, 0x10, dotdot, 0); d->used += 32;
    (void)f;
    return true;
}

bool fat_mkdir(FatVol* f, FatDir* parent, const char* name, uint32_t parent_cluster, FatDir** out) {
    uint32_t c = fat_alloc_run(f, 1);
    if (!c) return false;
    FatDir* d = fat_dir_new(f, c);
    if (!d) return false;
    if (!fat_dir_dots(f, d, parent_cluster ? parent_cluster : 0)) return false;
    if (!fat_dir_add(f, parent, name, 0x10, c, 0)) return false;
    if (out) *out = d;
    return true;
}

bool fat_file_zero(FatVol* f, FatDir* dir, const char* name, uint64_t bytes,
                   uint32_t pct, InstallerDisk::Progress* prog, char* why, int cap) {
    if (bytes == 0) return fat_dir_add(f, dir, name, 0x20, 0, 0);
    uint32_t need = (uint32_t)((bytes + f->cluster_bytes - 1) / f->cluster_bytes);
    uint32_t first = fat_alloc_run(f, need);
    if (!first) {
        s_cpy(why, "the new partition is too small for the requested file", cap);
        return false;
    }
    for (uint32_t i = 0; i < need; i++) {
        if (fat_get(f, first + i) != ((i + 1 == need) ? FAT_EOC : first + i + 1)) {
            s_cpy(why, "internal fat allocation is not contiguous", cap);
            return false;
        }
    }
    uint64_t off = fat_cluster_off(f, first);
    uint64_t left = bytes;
    uint64_t done = 0;
    while (left > 0) {
        uint32_t n = (left > f->zero_bytes) ? f->zero_bytes : (uint32_t)left;
        if (!fat_write(f, off, n, f->zero)) {
            s_cpy(why, "writing the zero filled file failed", cap);
            return false;
        }
        off += n;
        left -= n;
        done += n;
        if (prog && prog->fn && (done % (32ull * MIB)) < f->zero_bytes) {
            uint32_t p = pct;
            if (bytes > 0) p = pct + (uint32_t)((done * 20ull) / bytes);
            prog->fn(p, "writing KURONO data files", prog->ctx);
        }
    }
    return fat_dir_add(f, dir, name, 0x20, first, (uint32_t)(bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : bytes));
}

bool fat_file_data(FatVol* f, FatDir* dir, const char* name, const uint8_t* data,
                   uint32_t len, char* why, int cap) {
    if (len == 0) return fat_dir_add(f, dir, name, 0x20, 0, 0);
    uint32_t need = (len + f->cluster_bytes - 1) / f->cluster_bytes;
    uint32_t first = fat_alloc_run(f, need);
    if (!first) {
        s_cpy(why, "the new partition is too small for the efi loader", cap);
        return false;
    }
    uint64_t off = fat_cluster_off(f, first);
    uint32_t left = len;
    const uint8_t* p = data;
    while (left > 0) {
        uint32_t n = (left > 1u * MIB) ? (uint32_t)(1u * MIB) : left;
        if (!fat_write(f, off, n, p)) {
            s_cpy(why, "writing the efi loader failed", cap);
            return false;
        }
        off += n;
        p += n;
        left -= n;
    }
    uint32_t tail = len % f->cluster_bytes;
    if (tail) {
        uint32_t pad = f->cluster_bytes - tail;
        if (!fat_zero(f, off, pad)) {
            s_cpy(why, "zeroing the efi loader tail failed", cap);
            return false;
        }
    }
    return fat_dir_add(f, dir, name, 0x20, first, len);
}

bool fat_finish(FatVol* f, char* why, int cap) {
    for (int i = 0; i < f->ndirs; i++) {
        FatDir* d = &f->dirs[i];
        if (!fat_write(f, fat_cluster_off(f, d->cluster), f->cluster_bytes, d->buf)) {
            s_cpy(why, "writing a fat directory failed", cap);
            return false;
        }
    }
    for (uint32_t i = 0; i < f->num_fats; i++) {
        if (!fat_write(f, f->fat_off + (uint64_t)i * f->fat_bytes, f->fat_bytes, f->fat)) {
            s_cpy(why, "writing the fat table failed", cap);
            return false;
        }
    }
    // fsinfo sectors report the free count and the next free cluster. (satoru)
    uint8_t* si = (uint8_t*)KernelHeap::Alloc(f->ssz);
    if (!si) {
        s_cpy(why, "out of memory writing fat metadata", cap);
        return false;
    }
    memset(si, 0, f->ssz);
    wr32(si + 0, 0x41615252u);
    wr32(si + 484, 0x61417272u);
    wr32(si + 488, f->free_clusters);
    wr32(si + 492, f->next);
    wr32(si + 508, 0xAA550000u);
    bool w = fat_write(f, (uint64_t)1 * f->ssz, f->ssz, si);
    if (w) w = fat_write(f, (uint64_t)7 * f->ssz, f->ssz, si);
    KernelHeap::Free(si);
    if (!w) {
        s_cpy(why, "writing the fat fsinfo failed", cap);
        return false;
    }
    return true;
}

void fat_boot_sector(FatVol* f, uint8_t* bs) {
    memset(bs, 0, f->ssz);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "MSWIN4.1", 8);
    wr16(bs + 11, (uint16_t)f->ssz);
    bs[13] = (uint8_t)f->spc;
    wr16(bs + 14, (uint16_t)f->reserved);
    bs[16] = (uint8_t)f->num_fats;
    wr16(bs + 17, 0);
    wr16(bs + 19, 0);
    bs[21] = 0xF8;
    wr16(bs + 22, 0);
    wr16(bs + 24, 63);
    wr16(bs + 26, 255);
    wr32(bs + 28, (uint32_t)(f->part_lba > 0xFFFFFFFFull ? 0 : f->part_lba));
    wr32(bs + 32, f->total_sectors);
    wr32(bs + 36, f->fat_sectors);
    wr16(bs + 40, 0);
    wr16(bs + 42, 0);
    wr32(bs + 44, 2);
    wr16(bs + 48, 1);
    wr16(bs + 50, 6);
    bs[64] = 0x80;
    bs[66] = 0x29;
    uint8_t guid[16];
    make_guid(guid);
    uint32_t volid = (uint32_t)(rd64(guid) ^ (rd64(guid + 8) << 1));
    wr32(bs + 67, volid);
    memcpy(bs + 71, "KURONO     ", 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;
}

bool fat_boot_write(FatVol* f, char* why, int cap) {
    uint8_t* bs = (uint8_t*)KernelHeap::Alloc(f->ssz);
    if (!bs) {
        s_cpy(why, "out of memory writing the fat boot sector", cap);
        return false;
    }
    fat_boot_sector(f, bs);
    bool ok = fat_write(f, 0, f->ssz, bs);
    if (ok) ok = fat_write(f, (uint64_t)6 * f->ssz, f->ssz, bs);
    KernelHeap::Free(bs);
    if (!ok) s_cpy(why, "writing the fat boot sector failed", cap);
    return ok;
}

bool fat_format(FatVol* f, int dev, uint32_t ssz, uint64_t part_lba,
                uint32_t total_sectors, char* why, int cap) {
    memset(f, 0, sizeof(*f));
    if (ssz != 512 && ssz != 4096) {
        s_cpy(why, "unsupported sector size for fat32", cap);
        return false;
    }
    if (total_sectors < 32768u) {
        s_cpy(why, "the new partition is too small for fat32", cap);
        return false;
    }
    f->dev = dev;
    f->ssz = ssz;
    f->part_lba = part_lba;
    f->total_sectors = total_sectors;
    f->reserved = (ssz == 4096) ? 4u : 32u;
    f->num_fats = 2;
    uint32_t spc;
    if (ssz == 512) {
        spc = 8;   // 4k clusters (satoru)
        while (spc > 1) {
            uint32_t fat = 1;
            uint32_t clusters = 0;
            for (int it = 0; it < 8; it++) {
                uint32_t data = total_sectors - f->reserved - 2u * fat;
                clusters = data / spc;
                uint32_t need = (uint32_t)(((uint64_t)(clusters + 2) * 4 + ssz - 1) / ssz);
                if (need <= fat) break;
                fat = need;
            }
            if (clusters >= 65525u) break;
            spc /= 2;
        }
    } else {
        spc = 1;
    }
    f->spc = spc;
    f->cluster_bytes = ssz * spc;
    if (f->cluster_bytes > 65536u) {
        s_cpy(why, "unsupported fat cluster size", cap);
        return false;
    }
    uint32_t fat = 1;
    uint32_t clusters = 0;
    for (int it = 0; it < 12; it++) {
        uint32_t data = total_sectors - f->reserved - 2u * fat;
        clusters = data / spc;
        uint32_t need = (uint32_t)(((uint64_t)(clusters + 2) * 4 + ssz - 1) / ssz);
        if (need <= fat) break;
        fat = need;
    }
    f->fat_sectors = fat;
    f->fat_bytes = fat * ssz;
    f->clusters = clusters;
    f->fat_off = (uint64_t)f->reserved * ssz;
    f->data_off = (uint64_t)(f->reserved + f->num_fats * fat) * ssz;
    if (f->clusters < 4096u) {
        s_cpy(why, "the new partition is too small for fat32", cap);
        return false;
    }
    f->fat = (uint8_t*)KernelHeap::Alloc(f->fat_bytes);
    if (!f->fat) {
        s_cpy(why, "out of memory for the fat table", cap);
        return false;
    }
    memset(f->fat, 0, f->fat_bytes);
    f->zero_bytes = 1u * MIB;
    f->zero = (uint8_t*)KernelHeap::Alloc(f->zero_bytes);
    if (!f->zero) {
        KernelHeap::Free(f->fat);
        f->fat = nullptr;
        s_cpy(why, "out of memory for the fat writer", cap);
        return false;
    }
    memset(f->zero, 0, f->zero_bytes);
    f->next = 2;
    f->free_clusters = f->clusters;
    fat_set(f, 0, 0x0FFFFFF8u);
    fat_set(f, 1, 0x0FFFFFFFu);

    // clear the reserved area, then lay down the boot sector and its backup. (satoru)
    if (!fat_zero(f, 0, (uint64_t)f->reserved * ssz) ||
        !fat_boot_write(f, why, cap)) {
        KernelHeap::Free(f->zero);
        KernelHeap::Free(f->fat);
        f->zero = nullptr; f->fat = nullptr;
        return false;
    }

    // root directory is cluster 2. (satoru)
    uint32_t root = fat_alloc_run(f, 1);
    if (root != 2) {
        s_cpy(why, "internal fat root cluster mismatch", cap);
        return false;
    }
    if (!fat_zero(f, fat_cluster_off(f, 2), f->cluster_bytes)) {
        s_cpy(why, "clearing the fat root directory failed", cap);
        return false;
    }
    FatDir* d = fat_dir_new(f, 2);
    if (!d) {
        s_cpy(why, "out of memory for the fat root directory", cap);
        return false;
    }
    return true;
}

void fat_free(FatVol* f) {
    for (int i = 0; i < f->ndirs; i++) if (f->dirs[i].buf) KernelHeap::Free(f->dirs[i].buf);
    if (f->fat) KernelHeap::Free(f->fat);
    if (f->zero) KernelHeap::Free(f->zero);
    memset(f, 0, sizeof(*f));
}

// ---- install phases ------------------------------------------------------

void progress(InstallerDisk::Progress* p, uint32_t pct, const char* stage) {
    if (p && p->fn) p->fn(pct, stage, p->ctx);
}

bool payload_size(const uint8_t* a, const uint8_t* b, uint32_t* out) {
    if (!a || !b || b <= a) return false;
    uint64_t n = (uint64_t)(b - a);
    if (n == 0 || n > 0xFFFFFFFFull) return false;
    *out = (uint32_t)n;
    return true;
}

uint32_t payload_bytes(const uint8_t* a, const uint8_t* b) {
    uint32_t n = 0;
    payload_size(a, b, &n);
    return n;
}

bool install_files(const DiskX* d, const KinPlan* plan, InstallerDisk::Progress* prog,
                   char* why, int cap) {
    int dev = d->dev;
    uint32_t ssz = d->ssz;
    uint64_t part_lba = plan->part_start_lba;
    uint64_t part_sectors64 = plan->part_last_lba - plan->part_start_lba + 1;
    if (part_sectors64 == 0 || part_sectors64 > 0xFFFFFFFFull) {
        s_cpy(why, "the planned partition has an invalid size", cap);
        return false;
    }
    uint32_t part_sectors = (uint32_t)part_sectors64;

    progress(prog, 18, "formatting the KURONO partition");
    FatVol fv;
    if (!fat_format(&fv, dev, ssz, part_lba, part_sectors, why, cap)) return false;

    FatDir* root = fat_dir_find(&fv, 2);
    if (!root) { fat_free(&fv); s_cpy(why, "internal fat root missing", cap); return false; }

    // the volume label lives in the boot sector and as a directory entry. (satoru)
    uint8_t label[11];
    memcpy(label, "KURONO     ", 11);
    fat_entry((uint8_t*)root->buf + root->used, label, 0x08, 0, 0);
    root->used += 32;

    uint32_t efi_len = payload_bytes(_binary_kurono_installer_efi_efi_start,
                                     _binary_kurono_installer_efi_efi_end);
    uint32_t emg_len = payload_bytes(_binary_kurono_installer_emergency_efi_start,
                                     _binary_kurono_installer_emergency_efi_end);
    if (efi_len == 0) {
        fat_free(&fv);
        s_cpy(why, "this kernel has no embedded EFI loader (boot the full kernel)", cap);
        return false;
    }

    progress(prog, 22, "creating the EFI directories");
    FatDir* efi = nullptr;
    FatDir* kern = nullptr;
    FatDir* boot = nullptr;
    if (!fat_mkdir(&fv, root, "EFI", 0, &efi) ||
        !fat_mkdir(&fv, efi, "KURONO", efi->cluster, &kern) ||
        !fat_mkdir(&fv, efi, "BOOT", efi->cluster, &boot)) {
        fat_free(&fv);
        s_cpy(why, "could not create the EFI directories", cap);
        return false;
    }

    // EFI/KURONO/ and EFI/BOOT/ both get the loader. BOOTX64.EFI is the
    // removable-media fallback every UEFI firmware boots with no nvram entry.
    // (satoru)
    progress(prog, 28, "copying the EFI loader");
    if (!fat_file_data(&fv, kern, "KURONO.EFI",
                       _binary_kurono_installer_efi_efi_start, efi_len, why, cap)) {
        fat_free(&fv);
        return false;
    }
    if (!fat_file_data(&fv, boot, "BOOTX64.EFI",
                       _binary_kurono_installer_efi_efi_start, efi_len, why, cap)) {
        fat_free(&fv);
        return false;
    }
    progress(prog, 42, "copying the emergency loader");
    if (emg_len > 0) {
        if (!fat_file_data(&fv, kern, "KEMERG.EFI",
                           _binary_kurono_installer_emergency_efi_start, emg_len, why, cap)) {
            fat_free(&fv);
            return false;
        }
    }

    static const char readme[] =
        "Kurono OS installed by the Kurono installer.\r\n"
        "Primary loader: \\EFI\\KURONO\\KURONO.EFI\r\n"
        "Fallback loader: \\EFI\\BOOT\\BOOTX64.EFI\r\n"
        "Boot log: \\KURONO.LOG\r\n"
        "Persistent store: \\KURONO.KFS\r\n";
    if (!fat_file_data(&fv, efi, "README.TXT", (const uint8_t*)readme,
                       (uint32_t)(sizeof(readme) - 1), why, cap)) {
        fat_free(&fv);
        return false;
    }
    static const char install_info[] =
        "name=Kurono OS\r\n"
        "partition=KURONO\r\n"
        "filesystem=fat32\r\n"
        "loader=EFI/BOOT/BOOTX64.EFI\r\n"
        "log=KURONO.LOG\r\n"
        "store=KURONO.KFS\r\n";
    if (!fat_file_data(&fv, root, "KURONO.INST", (const uint8_t*)install_info,
                       (uint32_t)(sizeof(install_info) - 1), why, cap)) {
        fat_free(&fv);
        return false;
    }
    static const char nsh[] =
        "echo Kurono OS\r\n"
        "fs0:\\EFI\\BOOT\\BOOTX64.EFI\r\n";
    if (!fat_file_data(&fv, root, "STARTUP.NSH", (const uint8_t*)nsh,
                       (uint32_t)(sizeof(nsh) - 1), why, cap)) {
        fat_free(&fv);
        return false;
    }

    progress(prog, 50, "creating KURONO.LOG");
    if (!fat_file_zero(&fv, root, "KURONO.LOG", LOG_BYTES, 50, prog, why, cap)) {
        fat_free(&fv);
        return false;
    }
    {   // KBOOT.CFG: the boot watch, A/B kernel trials and Restore need it (satoru)
        uint8_t kb[512];
        KCrash::BootFileImage(kb);
        if (!fat_file_data(&fv, root, "KBOOT.CFG", kb, (uint32_t)sizeof(kb), why, cap)) {
            fat_free(&fv);
            return false;
        }
    }
    progress(prog, 58, "creating KURONO.KFS");
    if (!fat_file_zero(&fv, root, "KURONO.KFS", plan->kfs_bytes, 58, prog, why, cap)) {
        fat_free(&fv);
        return false;
    }

    progress(prog, 94, "finishing the filesystem");
    if (!fat_finish(&fv, why, cap)) { fat_free(&fv); return false; }
    if (!fat_boot_write(&fv, why, cap)) { fat_free(&fv); return false; }
    BlockDev::Flush(dev);
    fat_free(&fv);
    return true;
}

bool verify_fat(int dev, uint32_t ssz, const KinPlan* plan, char* why, int cap) {
    uint8_t* bs = (uint8_t*)KernelHeap::Alloc(4096);
    if (!bs) { s_cpy(why, "out of memory verifying the new volume", cap); return false; }
    bool ok = BlockDev::Read(dev, plan->part_start_lba, 4096 / ssz, bs);
    bool fat = ok && bs[510] == 0x55 && bs[511] == 0xAA &&
               ((bs[82] == 'F' && bs[83] == 'A' && bs[84] == 'T' && bs[85] == '3' && bs[86] == '2') ||
                (bs[54] == 'F' && bs[55] == 'A' && bs[56] == 'T' && bs[57] == '3' && bs[58] == '2'));
    bool label = fat && mem_eq(bs + 71, "KURONO     ", 11);
    KernelHeap::Free(bs);
    if (!ok) { s_cpy(why, "could not read the new partition back", cap); return false; }
    if (!fat) { s_cpy(why, "the new partition is not fat32", cap); return false; }
    if (!label) { s_cpy(why, "the new partition label is not KURONO", cap); return false; }
    // read the gpt entry back and confirm the KURONO name is where we put it. (satoru)
    uint8_t* ent = (uint8_t*)KernelHeap::Alloc(128);
    uint8_t* hdr = (uint8_t*)KernelHeap::Alloc(ssz);
    if (ent && hdr && BlockDev::Read(dev, 1, 1, hdr)) {
        GptHdr* h = (GptHdr*)hdr;
        if (mem_eq(h->sig, "EFI PART", 8) && h->esize == 128) {
            bool found = false;
            for (uint32_t i = 0; i < h->nentries; i++) {
                uint64_t off = h->entries_lba * (uint64_t)ssz + (uint64_t)i * h->esize;
                if (!BlockDev::ReadBytes(dev, off, 128, ent)) break;
                GptEnt* e = (GptEnt*)ent;
                if (guid_zero(e->type)) continue;
                char nm[40];
                utf16_name(e->name, 36, nm, sizeof(nm));
                if (s_eq(nm, "KURONO") && e->first == plan->part_start_lba &&
                    e->last == plan->part_last_lba) { found = true; break; }
            }
            if (!found) {
                if (ent) KernelHeap::Free(ent);
                if (hdr) KernelHeap::Free(hdr);
                s_cpy(why, "the KURONO gpt entry was not found after writing", cap);
                return false;
            }
        }
    }
    if (ent) KernelHeap::Free(ent);
    if (hdr) KernelHeap::Free(hdr);
    return true;
}

}  // namespace

namespace InstallerDisk {

bool PayloadAvailable() {
    return payload_bytes(_binary_kurono_installer_efi_efi_start,
                         _binary_kurono_installer_efi_efi_end) > 0;
}

int Scan(KinList* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->magic = KIN_LIST_MAGIC;
    int n = BlockDev::Count();
    for (int i = 0; i < n && (int)out->count < KIN_MAX_DISKS; i++) {
        DiskX d;
        if (!scan_disk(i, &d)) continue;
        fill_disk(&d, &out->disks[out->count]);
        out->count++;
    }
    return (int)out->count;
}

bool Safety(int dev, DiskSafety* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    DiskX d;
    if (!scan_disk(dev, &d)) return false;
    out->os_mask = d.os_mask;
    out->running = is_running_disk(&d);
    out->readonly = NVMe::WriteGuardOn();
    os_line_for(d.os_mask, out->line, sizeof(out->line));
    return true;
}

int Plan(int disk, uint32_t kfs_mb, KinPlan* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->magic = KIN_PLAN_MAGIC;
    out->disk_index = disk;
    out->payloads = PayloadAvailable() ? 1 : 0;
    DiskX d;
    if (!scan_disk(disk, &d)) {
        plan_refuse(out, "could not read this disk");
        return -1;
    }
    s_cpy(out->disk_name, d.name, sizeof(out->disk_name));
    s_cpy(out->model, d.model, sizeof(out->model));
    out->has_other_os = d.has_other ? 1 : 0;
    out->os_mask = d.os_mask;
    os_line_for(d.os_mask, out->os_line, sizeof(out->os_line));
    copy_parts(&d, out);
    if (is_running_disk(&d)) {
        out->running_disk = 1;
        plan_refuse(out, "Kurono is running from this drive");
        return -1;
    }
    if (!out->payloads) {
        plan_refuse(out, "this kernel has no embedded EFI loader; boot the full kernel");
        return -1;
    }
    if (d.scheme == KIN_SCHEME_BAD) {
        plan_refuse(out, "the partition table on this disk is damaged; refusing to touch it");
        return -1;
    }
    if (d.kurono_count > 1) {
        plan_refuse(out, "this disk has more than one KURONO partition; refusing to guess");
        return -1;
    }
    if (d.kurono_count == 1) {
        out->mode = KIN_PLAN_EXISTING;
        out->has_existing = 1;
        for (int i = 0; i < d.nparts; i++) {
            if (d.parts[i].p.flags & KIN_PART_KURONO) {
                out->part_start_lba = d.parts[i].p.start_lba;
                out->part_last_lba = d.parts[i].p.last_lba;
                out->part_bytes = d.parts[i].p.bytes;
                if (!s_eq(d.parts[i].p.fstype, "fat32")) {
                    plan_refuse(out, "the existing KURONO partition is not fat32");
                    return -1;
                }
                break;
            }
        }
        out->kfs_bytes = 0;
        out->log_bytes = 0;
        out->ok = 1;
        s_cpy(out->why, "Kurono is already installed on this disk", sizeof(out->why));
        return 0;
    }

    uint64_t first_usable = 0, last_usable = 0;
    if (d.scheme == KIN_SCHEME_GPT) {
        out->mode = KIN_PLAN_APPEND;
        first_usable = d.gpt.first_usable;
        last_usable = d.gpt.last_usable;
    } else if (d.scheme == KIN_SCHEME_MBR) {
        out->mode = KIN_PLAN_CONVERT;
        out->need_convert = 1;
        uint32_t entry_sectors = (MAX_GPT_ENTRIES * 128u + d.ssz - 1) / d.ssz;
        first_usable = 2 + entry_sectors;
        if (first_usable < 34) first_usable = 34;
        last_usable = d.total_lba - 2 - entry_sectors;
    } else {
        out->mode = KIN_PLAN_CREATE;
        uint32_t entry_sectors = (MAX_GPT_ENTRIES * 128u + d.ssz - 1) / d.ssz;
        first_usable = 2 + entry_sectors;
        if (first_usable < 34) first_usable = 34;
        last_usable = d.total_lba - 2 - entry_sectors;
        if (last_usable <= first_usable) {
            plan_refuse(out, "the disk is too small for a gpt");
            return -1;
        }
    }
    if (d.scheme != KIN_SCHEME_MBR) {
        // existing gpt partitions may not intrude into the backup area. (satoru)
        for (int i = 0; i < d.nparts; i++) {
            if (d.parts[i].p.start_lba < first_usable || d.parts[i].p.last_lba > last_usable) {
                plan_refuse(out, "an existing partition leaves no room for a gpt backup table");
                return -1;
            }
        }
    } else {
        for (int i = 0; i < d.nparts; i++) {
            if (d.parts[i].p.start_lba < first_usable) {
                plan_refuse(out, "an existing partition starts inside the area gpt needs");
                return -1;
            }
            if (d.parts[i].p.last_lba > last_usable) {
                plan_refuse(out, "an existing partition reaches the end of the disk; no room for the backup gpt");
                return -1;
            }
        }
    }
    uint64_t ps, pl;
    if (!choose_gap(&d, first_usable, last_usable, &ps, &pl)) {
        plan_refuse(out, "not enough free space for a KURONO partition (need about 300 MB)");
        return -1;
    }
    out->part_start_lba = ps;
    out->part_last_lba = pl;
    out->part_bytes = (pl - ps + 1) * (uint64_t)d.ssz;
    out->log_bytes = LOG_BYTES;
    uint64_t efi_len = payload_bytes(_binary_kurono_installer_efi_efi_start,
                                     _binary_kurono_installer_efi_efi_end);
    uint64_t emg_len = payload_bytes(_binary_kurono_installer_emergency_efi_start,
                                     _binary_kurono_installer_emergency_efi_end);
    uint64_t reserve = FAT_OVERHEAD + LOG_BYTES + 2ull * efi_len + emg_len;
    uint64_t kfs = out->part_bytes > reserve ? out->part_bytes - reserve : 0;
    if (kfs > KFS_MAX) kfs = KFS_MAX;
    if (kfs_mb) {
        uint64_t want = (uint64_t)kfs_mb * MIB;
        if (want > kfs) want = kfs;
        if (want < 64ull * MIB) want = 64ull * MIB;
        kfs = want;
    }
    kfs = (kfs / MIB) * MIB;
    if (kfs < 64ull * MIB) {
        plan_refuse(out, "not enough free space for KURONO.KFS (need at least 64 MB)");
        return -1;
    }
    out->kfs_bytes = kfs;
    // rewriting a table that holds another os, or writing a table over a whole
    // disk filesystem, needs the second confirmation. a gpt disk with free
    // space for the new partition does not. (satoru)
    if (out->os_mask && (out->mode == KIN_PLAN_CONVERT ||
                         (out->mode == KIN_PLAN_CREATE && (out->os_mask & KIN_OS_WHOLE))))
        out->need_os_ack = 1;
    if (NVMe::WriteGuardOn()) {
        out->guard_blocked = 1;
        plan_refuse(out, "the disk write guard forbids changing this disk's partition table");
        return -1;
    }
    out->ok = 1;
    s_cpy(out->why, out->mode == KIN_PLAN_CONVERT
                        ? "an old MBR table will be converted to GPT; existing partitions are preserved"
                        : "a KURONO partition will be created in free space",
           sizeof(out->why));
    return 0;
}

int Install(int disk, uint32_t kfs_mb, uint32_t convert_ack, uint32_t keep_ack,
            uint32_t os_ack, Progress* prog, char* why, int cap) {
    if (!why || cap < 2) return -1;
    why[0] = 0;
    ExtGuard extguard;   // no raw write is allowed once this function returns (satoru)
    progress(prog, 1, "checking the disk");
    KinPlan plan;
    if (Plan(disk, kfs_mb, &plan) != 0 || !plan.ok) {
        s_cpy(why, plan.why[0] ? plan.why : "no safe install plan for this disk", cap);
        return -1;
    }
    if (plan.mode == KIN_PLAN_EXISTING) {
        s_cpy(why, "Kurono is already installed on this disk", cap);
        return 0;
    }
    if (plan.need_convert && !convert_ack) {
        s_cpy(why, "the MBR to GPT conversion was not confirmed", cap);
        return -1;
    }
    if (plan.has_other_os && !keep_ack) {
        s_cpy(why, "the existing partitions were not confirmed", cap);
        return -1;
    }
    if (plan.need_os_ack && !os_ack) {
        s_cpy(why, plan.os_line[0] ? plan.os_line : "the other system on this disk was not confirmed", cap);
        return -1;
    }
    progress(prog, 2, "the disk plan is valid");

    DiskX d;
    if (!scan_disk(disk, &d)) {
        s_cpy(why, "could not re-read this disk", cap);
        return -1;
    }
    if (d.kurono_count != 0) {
        s_cpy(why, "a KURONO partition appeared on this disk during the install", cap);
        return -1;
    }

    progress(prog, 4, plan.mode == KIN_PLAN_CONVERT ? "converting the partition table"
                                                    : "writing the partition table");
    if (!write_gpt(&d, &plan, why, cap)) return -1;

    progress(prog, 12, "preparing the KURONO partition");
    // from here on only the new partition may be written. (satoru)
    ext_arm(d.dev);
    if (plan.part_start_lba < 34 || plan.part_last_lba + 34 > d.total_lba ||
        !ext_add(d.ssz, plan.part_start_lba, plan.part_last_lba) || !ext_clear_of_parts(&d)) {
        s_cpy(why, "the new partition would overlap an existing one; refusing to write", cap);
        return -1;
    }
    if (!install_files(&d, &plan, prog, why, cap)) return -1;

    progress(prog, 97, "verifying the installation");
    if (!verify_fat(d.dev, d.ssz, &plan, why, cap)) return -1;

    progress(prog, 100, "installation complete");
    return 0;
}

}  // namespace InstallerDisk

// end (satoru)
