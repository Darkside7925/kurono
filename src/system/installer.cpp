#include "installer.h"
#include "../drivers/nvme.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"   // page-aligned dma buffers for nvme reads/writes (satoru)
#include "../fs/kvfs.h"
#include "../fs/fat32.h"
#include "../linux/ext4.h"
#include "../shell/shell.h"

extern "C" {
    extern uint8_t _binary_kurono_installer_kernel_elf_start[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_kernel_elf_end[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_efi_efi_start[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_efi_efi_end[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_emergency_efi_start[] __attribute__((weak));
    extern uint8_t _binary_kurono_installer_emergency_efi_end[] __attribute__((weak));
}

#pragma pack(push, 1)
struct MBRPartitionEntry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t first_lba;
    uint32_t sector_count;
};

struct GPTHeader {
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entries_crc32;
};

struct GPTPartitionEntry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
};
#pragma pack(pop)

static const uint8_t GPT_EFI_SYSTEM_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t GPT_LINUX_FS_GUID[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

InstallerDiskInfo Installer::disks[INSTALLER_MAX_DISKS];
InstallerPartitionInfo Installer::parts[INSTALLER_MAX_PARTITIONS];
int Installer::disk_count = 0;
int Installer::part_count = 0;
bool Installer::initialized = false;

namespace {
static int s_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void s_cpy(char* d, const char* s, int mx) {
    int i = 0;
    if (!d || mx < 1) return;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void s_cat(char* d, const char* s, int mx) {
    int n = s_len(d);
    int i = 0;
    if (!d || mx < 1) return;
    while (s && s[i] && n < mx - 1) d[n++] = s[i++];
    d[n] = 0;
}

static bool s_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static int s_to_int(const char* s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int out_a(char* out, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) out[p++] = *s++;
    out[p] = 0;
    return p;
}

static int out_c(char* out, int p, int mx, char c) {
    if (p < mx - 1) out[p++] = c;
    out[p] = 0;
    return p;
}

static int out_u64(char* out, int p, int mx, uint64_t v) {
    if (v == 0) return out_c(out, p, mx, '0');
    char tmp[32];
    int n = 0;
    while (v && n < 31) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) p = out_c(out, p, mx, tmp[--n]);
    return p;
}

static bool mem_eq(const void* a, const void* b, int n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (int i = 0; i < n; i++) if (pa[i] != pb[i]) return false;
    return true;
}

static bool guid_is_zero(const uint8_t* g) {
    for (int i = 0; i < 16; i++) if (g[i] != 0) return false;
    return true;
}

static void utf16le_to_ascii(char* out, int mx, const uint16_t* in, int in_chars) {
    int p = 0;
    if (!out || mx < 1) return;
    for (int i = 0; i < in_chars && p < mx - 1; i++) {
        uint16_t ch = in[i];
        if (ch == 0) break;
        out[p++] = (ch < 128) ? (char)ch : '?';
    }
    out[p] = 0;
}

static InstallerFsType detect_fs_type(uint64_t start_lba, uint32_t sector_size, char* label, int label_mx, bool* esp_hint) {
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(sector_size * 4);
    if (!buf) return INST_FS_UNKNOWN;
    if (label && label_mx > 0) label[0] = 0;
    if (esp_hint) *esp_hint = false;

    InstallerFsType type = INST_FS_UNKNOWN;
    if (NVMe::Read(start_lba, 4, buf)) {
        bool sig = (buf[510] == 0x55 && buf[511] == 0xAA);
        bool fat32 = false;
        if (sig) {
            if ((buf[82] == 'F' && buf[83] == 'A' && buf[84] == 'T' && buf[85] == '3' && buf[86] == '2') ||
                (buf[54] == 'F' && buf[55] == 'A' && buf[56] == 'T' && buf[57] == '3' && buf[58] == '2')) {
                fat32 = true;
            }
        }
        if (fat32) {
            type = INST_FS_FAT32;
            if (label && label_mx > 0) {
                char vol[12];
                for (int i = 0; i < 11; i++) vol[i] = (char)buf[71 + i];
                vol[11] = 0;
                s_cpy(label, vol, label_mx);
            }
            if (esp_hint) *esp_hint = true;
        } else {
            uint16_t magic = *(uint16_t*)(buf + 1024 + 56);
            if (magic == 0xEF53) {
                type = INST_FS_EXT4;
                if (label && label_mx > 0) {
                    char vol[17];
                    for (int i = 0; i < 16; i++) vol[i] = (char)buf[1024 + 120 + i];
                    vol[16] = 0;
                    s_cpy(label, vol, label_mx);
                }
            }
        }
    }

    KernelHeap::Free(buf);
    return type;
}

static bool nvme_read_bytes(uint64_t byte_offset, uint32_t len, void* buf, void*) {
    if (!buf || len == 0) return false;
    uint32_t sector_size = NVMe::GetLBASize();
    if (sector_size == 0) sector_size = 512;
    uint64_t first_lba = byte_offset / sector_size;
    uint64_t last_lba = (byte_offset + len - 1) / sector_size;
    uint32_t sectors = (uint32_t)(last_lba - first_lba + 1);
    uint8_t* tmp = (uint8_t*)PMM::AllocBytes(sectors * sector_size);
    if (!tmp) return false;
    bool ok = NVMe::Read(first_lba, sectors, tmp);
    if (ok) memcpy(buf, tmp + (byte_offset % sector_size), len);
    PMM::FreeBytes(tmp, sectors * sector_size);
    return ok;
}

static bool nvme_write_bytes(uint64_t byte_offset, uint32_t len, const void* buf, void*) {
    if (!buf || len == 0) return false;
    uint32_t sector_size = NVMe::GetLBASize();
    if (sector_size == 0) sector_size = 512;
    uint64_t first_lba = byte_offset / sector_size;
    uint64_t last_lba = (byte_offset + len - 1) / sector_size;
    uint32_t sectors = (uint32_t)(last_lba - first_lba + 1);
    uint8_t* tmp = (uint8_t*)PMM::AllocBytes(sectors * sector_size);
    if (!tmp) return false;
    bool ok = NVMe::Read(first_lba, sectors, tmp);
    if (ok) {
        memcpy(tmp + (byte_offset % sector_size), buf, len);
        ok = NVMe::Write(first_lba, sectors, tmp);
        if (ok) NVMe::Flush();
    }
    PMM::FreeBytes(tmp, sectors * sector_size);
    return ok;
}

static int ext4_read_cb(uint64_t byte_offset, uint32_t len, void* buf, void* ctx) {
    (void)ctx;
    return nvme_read_bytes(byte_offset, len, buf, nullptr) ? 0 : -1;
}

static int ext4_write_cb(uint64_t byte_offset, uint32_t len, const void* buf, void* ctx) {
    (void)ctx;
    return nvme_write_bytes(byte_offset, len, buf, nullptr) ? 0 : -1;
}

static int fat32_read_cb(uint64_t byte_offset, uint32_t len, void* buf, void* ctx) {
    (void)ctx;
    return nvme_read_bytes(byte_offset, len, buf, nullptr) ? 0 : -1;
}

static int fat32_write_cb(uint64_t byte_offset, uint32_t len, const void* buf, void* ctx) {
    (void)ctx;
    return nvme_write_bytes(byte_offset, len, buf, nullptr) ? 0 : -1;
}

static int find_parent_split(const char* path) {
    int last = -1;
    for (int i = 0; path && path[i]; i++) if (path[i] == '/') last = i;
    return last;
}

static void ext4_mkdirs(const char* path) {
    if (!path || !*path) return;
    char cur[256];
    int p = 0;
    cur[p++] = '/';
    cur[p] = 0;
    for (int i = 1; path[i]; i++) {
        if (p < 255) cur[p++] = path[i];
        cur[p] = 0;
        if (path[i] == '/') {
            if (!Ext4::Exists(cur)) Ext4::Mkdir(cur, 0755);
        }
    }
    if (!Ext4::Exists(cur)) Ext4::Mkdir(cur, 0755);
}

static void ensure_parent_dirs(const char* path) {
    char parent[256];
    s_cpy(parent, path, sizeof(parent));
    int split = find_parent_split(parent);
    if (split <= 0) return;
    parent[split] = 0;
    ext4_mkdirs(parent);
}

static void copy_kvfs_to_ext4(const char* src, const char* dst) {
    if (!KVFS::Exists(src)) return;
    int len = KVFS::GetFileSize(src);
    if (len < 0) return;
    char* buf = (char*)KernelHeap::Alloc((uint32_t)len + 1);
    if (!buf) return;
    int rd = KVFS::ReadFile(src, buf, (uint32_t)len + 1);
    if (rd >= 0) {
        ensure_parent_dirs(dst);
        Ext4::WriteFile(dst, buf, (uint32_t)rd);
    }
    KernelHeap::Free(buf);
}

static void write_pkg_manifest(const char* path, const char* title, const char* body) {
    char text[2048];
    text[0] = 0;
    s_cat(text, "Package: ", sizeof(text));
    s_cat(text, title, sizeof(text));
    s_cat(text, "\n", sizeof(text));
    s_cat(text, body, sizeof(text));
    Ext4::WriteString(path, text);
}

static InstallerPartitionInfo* find_partition_by_name(const char* name) {
    for (int i = 0; i < Installer::GetPartitionCount(); i++) {
        InstallerPartitionInfo* p = Installer::GetPartition(i);
        if (p && p->present && s_eq(p->name, name)) return p;
    }
    return nullptr;
}

static uint32_t payload_size(uint8_t* start, uint8_t* end) {
    if (!start || !end || end < start) return 0;
    return (uint32_t)(end - start);
}
}

// boot-time persistence mount - see installer.h. brings up the nvme controller
// and mounts a whole-disk raw ext4 (superblock at offset 0) so the kvfs.img
// save/restore path works on a normal boot, not just after an install. the ext4
// block callbacks live in the anonymous namespace above; they're visible here in
// the same TU. (satoru)
bool Installer::MountDataDisk() {
    if (Ext4::IsMounted()) return true;            // already mounted (installed system)
    if (!NVMe::IsDetected()) NVMe::Init();
    if (!NVMe::IsDetected()) return false;          // no nvme data disk attached
    // Ext4::Mount validates the superblock magic, so a non-ext4 / partitioned
    // disk fails cleanly here rather than mis-mounting. (satoru)
    if (Ext4::Mount(ext4_read_cb, ext4_write_cb, nullptr, 0) == 0) {
        SerialLogger::Log("[persist] data disk mounted (raw ext4 @ 0)\r\n");
        return true;
    }
    SerialLogger::Log("[persist] nvme present but no ext4 superblock @ 0\r\n");
    return false;
}

void Installer::Init() {
    if (initialized) return;
    memset(disks, 0, sizeof(disks));
    memset(parts, 0, sizeof(parts));
    disk_count = 0;
    part_count = 0;
    initialized = true;
}

void Installer::Rescan() {
    Init();
    memset(disks, 0, sizeof(disks));
    memset(parts, 0, sizeof(parts));
    disk_count = 0;
    part_count = 0;

    if (!NVMe::IsDetected()) NVMe::Init();
    if (!NVMe::IsDetected()) {
        SerialLogger::Log("[Installer] No supported disks detected\r\n");
        return;
    }

    InstallerDiskInfo* d = &disks[disk_count++];
    d->present = true;
    d->type = INST_DISK_NVME;
    s_cpy(d->name, "nvme0n1", sizeof(d->name));
    s_cpy(d->driver, "nvme", sizeof(d->driver));
    s_cpy(d->model, NVMe::GetInfo().model, sizeof(d->model));
    d->total_lba = NVMe::GetCapacityLBA();
    d->sector_size = NVMe::GetLBASize() ? NVMe::GetLBASize() : 512;
    d->scheme = INST_SCHEME_UNKNOWN;
    d->has_esp = false;

    uint8_t* lba0 = (uint8_t*)KernelHeap::Alloc(d->sector_size * 4);
    if (!lba0) return;
    if (!NVMe::Read(0, 4, lba0)) {
        KernelHeap::Free(lba0);
        return;
    }

    bool mbr_sig = (lba0[510] == 0x55 && lba0[511] == 0xAA);
    MBRPartitionEntry* mbr = (MBRPartitionEntry*)(lba0 + 446);
    bool protective = false;
    if (mbr_sig) {
        for (int i = 0; i < 4; i++) if (mbr[i].type == 0xEE) protective = true;
    }

    if (protective) {
        GPTHeader* gh = (GPTHeader*)(lba0 + d->sector_size);
        if (mem_eq(gh->signature, "EFI PART", 8)) {
            d->scheme = INST_SCHEME_GPT;
            uint32_t max_entries = gh->num_partition_entries;
            if (max_entries > 128) max_entries = 128;
            uint32_t entry_size = gh->sizeof_partition_entry;
            if (entry_size < sizeof(GPTPartitionEntry)) entry_size = sizeof(GPTPartitionEntry);
            uint32_t entries_per_lba = d->sector_size / entry_size;
            if (entries_per_lba == 0) entries_per_lba = 1;

            uint8_t* entry_buf = (uint8_t*)KernelHeap::Alloc(d->sector_size);
            if (entry_buf) {
                for (uint32_t ei = 0; ei < max_entries && part_count < INSTALLER_MAX_PARTITIONS; ei++) {
                    uint64_t entry_lba = gh->partition_entries_lba + (ei / entries_per_lba);
                    if (!NVMe::Read(entry_lba, 1, entry_buf)) break;
                    GPTPartitionEntry* pe = (GPTPartitionEntry*)(entry_buf + (ei % entries_per_lba) * entry_size);
                    if (guid_is_zero(pe->type_guid) || pe->first_lba == 0 || pe->last_lba < pe->first_lba) continue;

                    InstallerPartitionInfo* p = &parts[part_count];
                    memset(p, 0, sizeof(*p));
                    p->present = true;
                    p->disk_index = 0;
                    p->part_index = part_count;
                    p->scheme = INST_SCHEME_GPT;
                    p->start_lba = pe->first_lba;
                    p->last_lba = pe->last_lba;
                    p->sector_count = pe->last_lba - pe->first_lba + 1;
                    p->size_bytes = p->sector_count * d->sector_size;
                    memcpy(p->type_guid, pe->type_guid, 16);
                    utf16le_to_ascii(p->label, sizeof(p->label), pe->name, 36);
                    s_cpy(p->name, "nvme0n1p", sizeof(p->name));
                    char idx[8]; idx[0] = 0; int ip = 0; uint32_t n = ei + 1; char rev[8]; int rn = 0; while (n && rn < 7) { rev[rn++] = (char)('0' + (n % 10)); n /= 10; } if (rn == 0) rev[rn++] = '0'; while (rn > 0 && ip < 7) idx[ip++] = rev[--rn]; idx[ip] = 0; s_cat(p->name, idx, sizeof(p->name));
                    p->esp = mem_eq(pe->type_guid, GPT_EFI_SYSTEM_GUID, 16);
                    p->bootable = p->esp;
                    if (mem_eq(pe->type_guid, GPT_LINUX_FS_GUID, 16)) p->bootable = true;
                    bool fs_esp_hint = false;
                    char fs_label[64]; fs_label[0] = 0;
                    p->fs_type = detect_fs_type(p->start_lba, d->sector_size, fs_label, sizeof(fs_label), &fs_esp_hint);
                    if (p->label[0] == 0 && fs_label[0]) s_cpy(p->label, fs_label, sizeof(p->label));
                    if (fs_esp_hint) p->esp = true;
                    if (p->esp) d->has_esp = true;
                    part_count++;
                }
                KernelHeap::Free(entry_buf);
            }
        }
    }

    if (d->scheme == INST_SCHEME_UNKNOWN && mbr_sig) {
        d->scheme = INST_SCHEME_MBR;
        for (int i = 0; i < 4 && part_count < INSTALLER_MAX_PARTITIONS; i++) {
            if (mbr[i].type == 0 || mbr[i].sector_count == 0) continue;
            InstallerPartitionInfo* p = &parts[part_count];
            memset(p, 0, sizeof(*p));
            p->present = true;
            p->disk_index = 0;
            p->part_index = part_count;
            p->scheme = INST_SCHEME_MBR;
            p->mbr_type = mbr[i].type;
            p->start_lba = mbr[i].first_lba;
            p->sector_count = mbr[i].sector_count;
            p->last_lba = p->start_lba + p->sector_count - 1;
            p->size_bytes = p->sector_count * d->sector_size;
            s_cpy(p->name, "nvme0n1p", sizeof(p->name));
            char one[2]; one[0] = (char)('1' + i); one[1] = 0; s_cat(p->name, one, sizeof(p->name));
            p->bootable = (mbr[i].status == 0x80);
            p->esp = (mbr[i].type == 0xEF);
            bool fs_esp_hint = false;
            char fs_label[64]; fs_label[0] = 0;
            p->fs_type = detect_fs_type(p->start_lba, d->sector_size, fs_label, sizeof(fs_label), &fs_esp_hint);
            if (fs_label[0]) s_cpy(p->label, fs_label, sizeof(p->label));
            if (mbr[i].type == 0x0B || mbr[i].type == 0x0C || fs_esp_hint) p->esp = true;
            if (p->esp) d->has_esp = true;
            part_count++;
        }
    }

    KernelHeap::Free(lba0);
    SerialLogger::Log("[Installer] Disk scan complete\r\n");
}

int Installer::GetDiskCount() { return disk_count; }
int Installer::GetPartitionCount() { return part_count; }
InstallerDiskInfo* Installer::GetDisk(int idx) { return (idx >= 0 && idx < disk_count) ? &disks[idx] : nullptr; }
InstallerPartitionInfo* Installer::GetPartition(int idx) { return (idx >= 0 && idx < part_count) ? &parts[idx] : nullptr; }

int Installer::FindESPPartition() {
    for (int i = 0; i < part_count; i++) if (parts[i].present && parts[i].esp) return i;
    return -1;
}

int Installer::FindFirstExt4Partition() {
    for (int i = 0; i < part_count; i++) if (parts[i].present && parts[i].fs_type == INST_FS_EXT4) return i;
    return -1;
}

void Installer::DumpDisks(char* out, int max_out) {
    int p = 0;
    p = out_a(out, p, max_out, "NAME      DRIVER  SCHEME  SECTOR  TOTAL-LBA  MODEL\n");
    for (int i = 0; i < disk_count; i++) {
        InstallerDiskInfo* d = &disks[i];
        p = out_a(out, p, max_out, d->name);
        for (int s = s_len(d->name); s < 10; s++) p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, d->driver);
        for (int s = s_len(d->driver); s < 8; s++) p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, d->scheme == INST_SCHEME_GPT ? "GPT" : (d->scheme == INST_SCHEME_MBR ? "MBR" : "UNK"));
        for (int s = (d->scheme == INST_SCHEME_UNKNOWN ? 3 : 3); s < 8; s++) p = out_c(out, p, max_out, ' ');
        p = out_u64(out, p, max_out, d->sector_size);
        for (int s = 0; s < 4; s++) p = out_c(out, p, max_out, ' ');
        p = out_u64(out, p, max_out, d->total_lba);
        p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, d->model[0] ? d->model : "(unknown)");
        p = out_c(out, p, max_out, '\n');
    }
}

void Installer::DumpPartitions(char* out, int max_out) {
    int p = 0;
    p = out_a(out, p, max_out, "IDX NAME        SCHEME FS     ESP START-LBA    SECTORS      LABEL\n");
    for (int i = 0; i < part_count; i++) {
        InstallerPartitionInfo* pt = &parts[i];
        p = out_u64(out, p, max_out, (uint64_t)i);
        p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, pt->name);
        for (int s = s_len(pt->name); s < 12; s++) p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, pt->scheme == INST_SCHEME_GPT ? "GPT" : (pt->scheme == INST_SCHEME_MBR ? "MBR" : "UNK"));
        for (int s = 3; s < 7; s++) p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, pt->fs_type == INST_FS_EXT4 ? "ext4" : (pt->fs_type == INST_FS_FAT32 ? "fat32" : "unknown"));
        for (int s = (pt->fs_type == INST_FS_EXT4 ? 4 : (pt->fs_type == INST_FS_FAT32 ? 5 : 7)); s < 7; s++) p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, pt->esp ? "yes " : "no  ");
        p = out_u64(out, p, max_out, pt->start_lba);
        for (int s = 0; s < 4; s++) p = out_c(out, p, max_out, ' ');
        p = out_u64(out, p, max_out, pt->sector_count);
        p = out_c(out, p, max_out, ' ');
        p = out_a(out, p, max_out, pt->label[0] ? pt->label : "(no label)");
        p = out_c(out, p, max_out, '\n');
    }
}

void Installer::DescribeInstallPlan(int partition_index, char* out, int max_out) {
    int p = 0;
    InstallerPartitionInfo* target = GetPartition(partition_index);
    if (!target || !target->present) {
        out_a(out, 0, max_out, "Invalid target partition index\n");
        return;
    }

    p = out_a(out, p, max_out, "Kurono Installer Plan\n");
    p = out_a(out, p, max_out, "  Target: ");
    p = out_a(out, p, max_out, target->name);
    p = out_a(out, p, max_out, "  fs=");
    p = out_a(out, p, max_out, target->fs_type == INST_FS_EXT4 ? "ext4" : (target->fs_type == INST_FS_FAT32 ? "fat32" : "unknown"));
    p = out_a(out, p, max_out, "\n");

    int esp = FindESPPartition();
    p = out_a(out, p, max_out, "  ESP: ");
    if (esp >= 0) p = out_a(out, p, max_out, parts[esp].name);
    else p = out_a(out, p, max_out, "not detected");
    p = out_a(out, p, max_out, "\n\n");

    p = out_a(out, p, max_out, "Layout to create:\n");
    p = out_a(out, p, max_out, "  /system\n  /system/logs\n  /system/kurono\n  /system/packages\n  /system/modules\n  /system/assets\n");
    p = out_a(out, p, max_out, "  /etc\n  /boot\n  /boot/grub\n  /apps\n  /apps/*/{logs,data}\n\n");
    p = out_a(out, p, max_out, "Package manifests to generate:\n");
    p = out_a(out, p, max_out, "  kernel.pkginfo\n  apps.pkginfo\n  modules.pkginfo\n  assets.pkginfo\n\n");
    p = out_a(out, p, max_out, "Installer notes:\n");
    p = out_a(out, p, max_out, "  - ext4 target write path is active\n");
    p = out_a(out, p, max_out, "  - FAT32 ESP detection and write path are active\n");
    p = out_a(out, p, max_out, "  - embedded EFI + kernel payloads will be deployed during install\n");
}

int Installer::InstallToPartition(int partition_index, char* out, int max_out) {
    InstallerPartitionInfo* target = GetPartition(partition_index);
    if (!target || !target->present) return out_a(out, 0, max_out, "Invalid target partition index\n");
    if (target->fs_type != INST_FS_EXT4) return out_a(out, 0, max_out, "Target partition is not ext4\n");

    InstallerDiskInfo* disk = GetDisk(target->disk_index);
    if (!disk) return out_a(out, 0, max_out, "Target disk missing\n");

    if (Ext4::Mount(ext4_read_cb, ext4_write_cb, nullptr, target->start_lba * disk->sector_size) != 0) {
        return out_a(out, 0, max_out, "Failed to mount ext4 target\n");
    }

    ext4_mkdirs("/system");                  // KLS linux-subsystem rootfs (satoru)
    ext4_mkdirs("/kurono");                   // kurono-native namespace (satoru)
    ext4_mkdirs("/kurono/var");
    ext4_mkdirs("/kurono/var/log");
    ext4_mkdirs("/kurono/var/lib");
    ext4_mkdirs("/kurono/etc");
    ext4_mkdirs("/system/packages");
    ext4_mkdirs("/system/modules");
    ext4_mkdirs("/system/assets");
    ext4_mkdirs("/etc");
    ext4_mkdirs("/boot");
    ext4_mkdirs("/boot/grub");
    ext4_mkdirs("/apps");

    const char* apps[] = {"terminal","files","calculator","editor","settings","tasks","browser","media",nullptr};
    for (int i = 0; apps[i]; i++) {
        char path[128];
        path[0] = 0;
        s_cat(path, "/apps/", sizeof(path));
        s_cat(path, apps[i], sizeof(path));
        ext4_mkdirs(path);
        s_cat(path, "/logs", sizeof(path));
        ext4_mkdirs(path);
        path[0] = 0;
        s_cat(path, "/apps/", sizeof(path));
        s_cat(path, apps[i], sizeof(path));
        s_cat(path, "/data", sizeof(path));
        ext4_mkdirs(path);
    }

    copy_kvfs_to_ext4("/kurono/var/log/serial.log", "/kurono/var/log/serial.log");
    copy_kvfs_to_ext4("/kurono/var/log/system.log", "/kurono/var/log/system.log");
    copy_kvfs_to_ext4("/kurono/var/log/boot.log",   "/kurono/var/log/boot.log");
    copy_kvfs_to_ext4("/boot/kernel.info", "/boot/kernel.info");
    copy_kvfs_to_ext4("/boot/apps.info", "/boot/apps.info");
    copy_kvfs_to_ext4("/etc/hostname", "/etc/hostname");
    copy_kvfs_to_ext4("/etc/os-release", "/etc/os-release");

    Ext4::WriteString("/etc/hostname", "kurono\n");
    Ext4::WriteString("/etc/os-release",
        "NAME=Kurono OS\n"
        "ID=kurono\n"
        "VERSION=1.0\n"
        "VARIANT=installed\n");

    char manifest[4096];
    manifest[0] = 0;
    s_cat(manifest,
        "Kurono installation manifest\n"
        "layout=/system,/etc,/boot,/apps\n"
        "profile=split-package\n"
        "packages=kernel,apps,modules,assets\n"
        "drivers=nvme,e1000,usb,hda,ac97,nvidia,intel,amd,virtio_gpu\n"
        "apps=terminal,file_manager,text_editor,settings,task_manager,browser,media_player\n",
        sizeof(manifest));
    Ext4::WriteString("/system/kurono/install.manifest", manifest);

    write_pkg_manifest("/system/packages/kernel.pkginfo", "kernel",
        "files=/boot/kurono.elf,/boot/kernel.info,/boot/grub/grub.cfg\n"
        "mode=split\n"
        "boot=efi+grub\n");
    write_pkg_manifest("/system/packages/apps.pkginfo", "apps",
        "files=/apps/terminal,/apps/files,/apps/calculator,/apps/editor,/apps/settings,/apps/tasks,/apps/browser,/apps/media\n");
    write_pkg_manifest("/system/packages/modules.pkginfo", "modules",
        "drivers=display,input,storage,network,audio,gpu,linux-bridge,virt\n"
        "storage=nvme,ext4\n");
    write_pkg_manifest("/system/packages/assets.pkginfo", "assets",
        "assets=logo,wallpaper,embedded-media,default-config\n");

    Ext4::WriteString("/system/modules/storage.list",
        "nvme\next4\npartition-gpt\npartition-mbr\nfat32-esp-detect\n");
    Ext4::WriteString("/system/assets/assets.list",
        "logo\nwallpaper\ndenji.mp4\n");

    uint32_t kernel_payload_sz = payload_size(_binary_kurono_installer_kernel_elf_start,
                                              _binary_kurono_installer_kernel_elf_end);
    uint32_t efi_payload_sz = payload_size(_binary_kurono_installer_efi_efi_start,
                                           _binary_kurono_installer_efi_efi_end);
    uint32_t emergency_payload_sz = payload_size(_binary_kurono_installer_emergency_efi_start,
                                                 _binary_kurono_installer_emergency_efi_end);

    if (kernel_payload_sz > 0) {
        Ext4::WriteFile("/boot/kurono.elf",
            _binary_kurono_installer_kernel_elf_start,
            kernel_payload_sz);
    }

    Ext4::WriteString("/boot/grub/grub.cfg",
        "menuentry 'Kurono OS' {\n"
        "  chainloader /EFI/KURONO/KURONO.EFI\n"
        "}\n"
        "menuentry 'Kurono OS (Emergency)' {\n"
        "  chainloader /EFI/KURONO/KEMERG.EFI\n"
        "}\n");

    int esp = FindESPPartition();
    char esp_note[512];
    esp_note[0] = 0;
    s_cat(esp_note, "Detected ESP: ", sizeof(esp_note));
    if (esp >= 0) s_cat(esp_note, parts[esp].name, sizeof(esp_note));
    else s_cat(esp_note, "none", sizeof(esp_note));
    s_cat(esp_note,
        "\nExpected ESP layout:\n"
        "  /EFI/KURONO/KURONO.EFI\n"
        "  /EFI/KURONO/KEMERG.EFI\n"
        "  /EFI/BOOT/BOOTX64.EFI\n",
        sizeof(esp_note));
    Ext4::WriteString("/boot/esp.layout", esp_note);

    Ext4::WriteString("/boot/INSTALL.README",
        "Kurono installer created the runtime filesystem layout on this ext4 target.\n"
        "Embedded EFI payloads and kernel image were staged for deployment.\n"
        "The ext4 target now contains /system, /etc, /boot, and /apps.\n");

    bool esp_written = false;
    if (esp >= 0) {
        InstallerPartitionInfo* epart = GetPartition(esp);
        InstallerDiskInfo* edisk = epart ? GetDisk(epart->disk_index) : nullptr;
        if (epart && edisk && FAT32::Mount(fat32_read_cb, fat32_write_cb, nullptr,
                                           epart->start_lba * edisk->sector_size) == 0) {
            static const char esp_readme[] =
                "Kurono EFI payloads installed.\r\n"
                "Primary loader: KURONO.EFI\r\n"
                "Emergency loader: KEMERG.EFI\r\n";
            static const char startup_nsh[] =
                "fs0:\\EFI\\KURONO\\KURONO.EFI\r\n";
            FAT32::Mkdirs("/EFI");
            FAT32::Mkdirs("/EFI/KURONO");
            FAT32::Mkdirs("/EFI/BOOT");
            if (efi_payload_sz > 0) {
                FAT32::WriteFile("/EFI/KURONO/KURONO.EFI",
                    _binary_kurono_installer_efi_efi_start,
                    efi_payload_sz);
                FAT32::WriteFile("/EFI/BOOT/BOOTX64.EFI",
                    _binary_kurono_installer_efi_efi_start,
                    efi_payload_sz);
            }
            if (emergency_payload_sz > 0) {
                FAT32::WriteFile("/EFI/KURONO/KEMERG.EFI",
                    _binary_kurono_installer_emergency_efi_start,
                    emergency_payload_sz);
            }
            FAT32::WriteFile("/EFI/KURONO/README.TXT",
                esp_readme,
                (uint32_t)(sizeof(esp_readme) - 1));
            FAT32::WriteFile("/STARTUP.NSH",
                startup_nsh,
                (uint32_t)(sizeof(startup_nsh) - 1));
            FAT32::Unmount();
            esp_written = efi_payload_sz > 0;
        }
    }

    Ext4::Unmount();

    int p = 0;
    p = out_a(out, p, max_out, "Install complete to ");
    p = out_a(out, p, max_out, target->name);
    p = out_a(out, p, max_out, "\nCreated layout: /system /etc /boot /apps\n");
    p = out_a(out, p, max_out, "Generated package manifests and boot layout notes\n");
    p = out_a(out, p, max_out,
        (kernel_payload_sz > 0 || efi_payload_sz > 0) ?
        "Embedded kernel + EFI payloads deployed to target\n" :
        "Installer layout deployed; embedded boot payloads unavailable in this build\n");
    if (esp >= 0) {
        p = out_a(out, p, max_out, "Detected ESP: ");
        p = out_a(out, p, max_out, parts[esp].name);
        p = out_a(out, p, max_out, esp_written ? " (written)\n" : " (mount/write failed)\n");
    }
    return p;
}

void Installer::RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (!sh) return;
    sh->RegisterCommand("installer", cmd_installer, "Disk installer / partition scanner");
}

int Installer::cmd_installer(void*, int argc, const char** argv, char* out, int mx) {
    if (argc < 2) {
        int p = 0;
        p = out_a(out, p, mx, "Kurono Installer\n");
        p = out_a(out, p, mx, "  installer scan              - rescan disks\n");
        p = out_a(out, p, mx, "  installer disks             - list disks\n");
        p = out_a(out, p, mx, "  installer parts             - list partitions\n");
        p = out_a(out, p, mx, "  installer esp               - show detected FAT32 ESP\n");
        p = out_a(out, p, mx, "  installer plan <idx>        - show install plan for partition\n");
        p = out_a(out, p, mx, "  installer install <idx>     - install layout onto ext4 partition\n");
        p = out_a(out, p, mx, "  installer auto              - install to first detected ext4 partition\n");
        return p;
    }

    if (s_eq(argv[1], "scan")) {
        Rescan();
        int p = out_a(out, 0, mx, "Disk scan complete\n");
        p = out_a(out, p, mx, "Disks: ");
        p = out_u64(out, p, mx, (uint64_t)GetDiskCount());
        p = out_a(out, p, mx, "  Partitions: ");
        p = out_u64(out, p, mx, (uint64_t)GetPartitionCount());
        p = out_c(out, p, mx, '\n');
        return p;
    }
    if (s_eq(argv[1], "disks")) {
        Rescan();
        DumpDisks(out, mx);
        return s_len(out);
    }
    if (s_eq(argv[1], "parts")) {
        Rescan();
        DumpPartitions(out, mx);
        return s_len(out);
    }
    if (s_eq(argv[1], "esp")) {
        Rescan();
        int esp = FindESPPartition();
        if (esp < 0) return out_a(out, 0, mx, "No FAT32 EFI System Partition detected\n");
        int p = 0;
        p = out_a(out, p, mx, "ESP detected: ");
        p = out_a(out, p, mx, parts[esp].name);
        p = out_a(out, p, mx, " label=");
        p = out_a(out, p, mx, parts[esp].label[0] ? parts[esp].label : "(no label)");
        p = out_c(out, p, mx, '\n');
        return p;
    }
    if (s_eq(argv[1], "plan") && argc >= 3) {
        Rescan();
        int idx = s_to_int(argv[2]);
        if (idx < 0 && argc >= 3) {
            InstallerPartitionInfo* pinfo = find_partition_by_name(argv[2]);
            if (pinfo) idx = pinfo->part_index;
        }
        DescribeInstallPlan(idx, out, mx);
        return s_len(out);
    }
    if (s_eq(argv[1], "install") && argc >= 3) {
        Rescan();
        int idx = s_to_int(argv[2]);
        if (idx < 0) {
            InstallerPartitionInfo* pinfo = find_partition_by_name(argv[2]);
            if (pinfo) idx = pinfo->part_index;
        }
        return InstallToPartition(idx, out, mx);
    }
    if (s_eq(argv[1], "auto")) {
        Rescan();
        int idx = FindFirstExt4Partition();
        if (idx < 0) return out_a(out, 0, mx, "No ext4 partition detected for install\n");
        return InstallToPartition(idx, out, mx);
    }

    return out_a(out, 0, mx, "Unknown installer subcommand\n");
}
