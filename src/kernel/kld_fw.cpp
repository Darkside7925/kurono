// KLD firmware store implementation. see kld_fw.h for the layout and rules.
//
// (satoru)

#include "kld_fw.h"
#include "kld_broker.h"
#include "heap.h"
#include "../drivers/blockdev.h"
#include "../system/installer.h"
#include "../drivers/serial.h"
#include "../fs/fat32.h"
#include "../fs/kvfs.h"
#include "../proc/spinlock.h"

namespace KldFw {

namespace {

constexpr const char* FW_PREFIX = "/system/lib/firmware/";
constexpr uint32_t MAX_STORE_FILE = 8u << 20;
constexpr uint32_t MAX_MANIFEST = 32u << 10;
constexpr int MAX_FAMILY = 8;

Spinlock g_lock;
bool     g_we_mounted;
bool     g_mount_tried;
uint32_t g_served;
uint32_t g_misses;
uint32_t g_refused;

unsigned slen(const char* s) {
    unsigned n = 0;
    while (s && s[n]) n++;
    return n;
}

bool starts(const char* s, const char* p) {
    while (*p) {
        if (*s != *p) return false;
        s++; p++;
    }
    return true;
}

bool streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void copy(char* d, const char* s, unsigned max) {
    unsigned i = 0;
    while (s && s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void cat(char* d, const char* s, unsigned max) {
    unsigned n = slen(d);
    unsigned i = 0;
    while (s && s[i] && n + 1 < max) d[n++] = s[i++];
    d[n] = 0;
}

void log_s(const char* s) { SerialLogger::Log(s ? s : "?"); }
void log_dec(uint32_t v) { SerialLogger::LogDec((int)v); }

// decimal or 0x-prefixed hex. (satoru)
uint32_t parse_size(const char* s, uint32_t* out) {
    uint32_t v = 0;
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            uint32_t d;
            if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
            else return 0;
            v = v * 16 + d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
        if (*s) return 0;
    }
    *out = v;
    return 1;
}

// a single plain component for the requested firmware name. (satoru)
bool valid_component(const char* name, unsigned n) {
    if (n == 0) return false;
    if (n == 1 && name[0] == '.') return false;
    if (n == 2 && name[0] == '.' && name[1] == '.') return false;
    for (unsigned i = 0; i < n; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
            return false;
    }
    if (name[0] == '-' || name[0] == '.') return false;
    return true;
}

// a plain file name, or one vendor directory and a plain file name
// ("i915/mtl_dmc.bin", as linux firmware names are); never "..", never a
// second level. (satoru)
bool valid_name(const char* name) {
    unsigned n = slen(name);
    if (n == 0 || n > 120) return false;
    unsigned slash = n;
    for (unsigned i = 0; i < n; i++)
        if (name[i] == '/') {
            if (slash != n) return false;
            slash = i;
        }
    if (slash == n) return valid_component(name, n);
    return valid_component(name, slash) &&
           valid_component(name + slash + 1, n - slash - 1);
}

bool valid_family(const char* f) {
    unsigned n = slen(f);
    if (n == 0 || n > MAX_FAMILY) return false;
    for (unsigned i = 0; i < n; i++) {
        char c = f[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

// the family the kernel started this container as: the broker's record first
// (kernel runners), then the loader-recorded exe_path (execve/bench and the
// test families). never anything the container asks for. (satoru)
void family_for(uint64_t as, const char* exe_path, char out[16]) {
    out[0] = 0;
    const char* f = KldBroker::FamilyOfAddressSpace(as);
    if (f && f[0]) {
        copy(out, f, 16);
        return;
    }
    if (!exe_path || !exe_path[0]) return;
    const char* base = exe_path;
    for (const char* p = exe_path; *p; p++)
        if (*p == '/') base = p + 1;
    if (base[0] != 'k' || base[1] != 'l' || base[2] != 'd' || base[3] != '-')
        return;
    copy(out, base + 4, 16);
}

// byte-granular read behind the fat32 driver, through the block registry so
// nvme and ahci both work. (satoru)
int g_fw_bdev = -1;

int fw_bd_read(uint64_t off, uint32_t len, void* buf, void*) {
    if (g_fw_bdev < 0 || !buf || len == 0) return -1;
    return BlockDev::ReadBytes(g_fw_bdev, off, len, buf) ? 0 : -1;
}

bool store_path(const char* family, const char* file, char* out, unsigned max) {
    copy(out, "/firmware/", max);
    if (family && family[0]) {
        cat(out, family, max);
        cat(out, "/", max);
    }
    cat(out, file, max);
    return out[0] != 0;
}

// mount the KURONO fat32 partition once. if someone else already mounted a
// fat32 volume, we cannot prove it is this partition and do not use it.
// (satoru)
bool store_mount() {
    if (FAT32::IsMounted())
        return g_we_mounted;
    if (g_mount_tried && !g_we_mounted)
        return false;
    g_mount_tried = true;

    // the partition scanner the shell and the installer use. the store is the
    // single GPT partition named KURONO (what the installer creates).
    // (satoru)
    Installer::Rescan();
    uint64_t lo = 0;
    uint32_t ss = 0;
    int hits = 0;
    int bd = -1;
    for (int i = 0; i < Installer::GetPartitionCount(); i++) {
        InstallerPartitionInfo* p = Installer::GetPartition(i);
        if (!p || !p->present) continue;
        if (!streq(p->label, "KURONO")) continue;
        if (p->fs_type != INST_FS_FAT32) continue;
        InstallerDiskInfo* d = Installer::GetDisk(p->disk_index);
        if (!d) continue;
        for (int b = 0; b < BlockDev::Count(); b++) {
            BlockDevice* dev = BlockDev::Get(b);
            if (dev && streq(dev->name, d->name)) {
                lo = p->start_lba;
                ss = d->sector_size;
                bd = b;
                hits++;
                break;
            }
        }
    }
    if (hits != 1 || bd < 0) {
        log_s("[kld-fw] no single FAT32 KURONO partition for the firmware store\r\n");
        return false;
    }
    g_fw_bdev = bd;
    int rc = FAT32::Mount(fw_bd_read, nullptr, nullptr, lo * (uint64_t)ss);
    if (rc != 0) {
        log_s("[kld-fw] KURONO fat32 mount failed, store unavailable\r\n");
        return false;
    }
    g_we_mounted = true;
    log_s("[kld-fw] firmware store mounted from the KURONO partition\r\n");
    return true;
}

// read /firmware/<family>/<store83> into a heap buffer. (satoru)
uint8_t* read_store_file(const char* family, const char* store83,
                         uint32_t want, uint32_t* got) {
    *got = 0;
    if (!valid_name(store83)) return nullptr;
    char path[160];
    if (!store_path(family, store83, path, sizeof(path))) return nullptr;
    int sz = FAT32::GetFileSize(path);
    if (sz <= 0 || (uint32_t)sz > MAX_STORE_FILE) return nullptr;
    if (want && (uint32_t)sz != want) {
        log_s("[kld-fw] store size mismatch for ");
        log_s(path);
        log_s("\r\n");
        return nullptr;
    }
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc((size_t)sz);
    if (!buf) return nullptr;
    int n = FAT32::ReadFile(path, buf, (uint32_t)sz);
    if (n != sz) {
        KernelHeap::Free(buf);
        return nullptr;
    }
    *got = (uint32_t)sz;
    return buf;
}

// find the exact driver name in the family manifest. (satoru)
bool manifest_lookup(const char* family, const char* name,
                     char* store83, uint32_t* size) {
    store83[0] = 0;
    *size = 0;
    char path[160];
    if (!store_path(family, "MANIFEST.TXT", path, sizeof(path))) return false;
    int sz = FAT32::GetFileSize(path);
    if (sz <= 0 || (uint32_t)sz > MAX_MANIFEST) {
        log_s("[kld-fw] manifest not readable\r\n");
        return false;
    }
    char* buf = (char*)KernelHeap::Alloc((size_t)sz + 1);
    if (!buf) return false;
    int n = FAT32::ReadFile(path, buf, (uint32_t)sz);
    if (n != sz) {
        log_s("[kld-fw] manifest short read\r\n");
        KernelHeap::Free(buf);
        return false;
    }
    buf[sz] = 0;

    bool found = false;
    char* p = buf;
    while (*p && !found) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = 0;
        int ll = (int)slen(line);
        while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == ' ' || line[ll-1] == '\t'))
            line[--ll] = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        char* tok[4] = { nullptr, nullptr, nullptr, nullptr };
        int nt = 0;
        char* q = line;
        while (*q && nt < 4) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            tok[nt++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            if (*q) *q++ = 0;
        }
        if (nt < 3) continue;
        if (!streq(tok[0], name)) continue;
        uint32_t szv = 0;
        if (!valid_name(tok[1]) || !parse_size(tok[2], &szv) || szv == 0)
            continue;
        copy(store83, tok[1], 16);
        *size = szv;
        found = true;
    }
    KernelHeap::Free(buf);
    return found;
}

// one request. true = KVFS now has the file. (satoru)
bool load_one(const char* family, const char* name, const char* kvfs_path) {
    char store83[16];
    uint32_t size = 0;
    bool shared = false;

    // the two cfg80211 files are shared by every family and keep their short
    // store names. (satoru)
    if (streq(name, "regulatory.db")) {
        copy(store83, "REGULATO.DB", sizeof(store83));
        shared = true;
    } else if (streq(name, "regulatory.db.p7s")) {
        copy(store83, "REGULATO.P7S", sizeof(store83));
        shared = true;
    }
    if (!shared && !manifest_lookup(family, name, store83, &size))
        return false;

    uint32_t got = 0;
    uint8_t* data = read_store_file(shared ? "" : family, store83, size, &got);
    if (!data) return false;

    KVFS::Mkdirs("/system/lib/firmware");
    // a vendor directory name ("i915/...") needs its directory. (satoru)
    {
        char dir[160];
        copy(dir, kvfs_path, sizeof(dir));
        unsigned n = slen(dir);
        while (n && dir[n - 1] != '/') n--;
        if (n > slen(FW_PREFIX)) {
            dir[n - 1] = 0;
            KVFS::Mkdirs(dir);
        }
    }
    int rc = KVFS::WriteFile(kvfs_path, data, got);
    KernelHeap::Free(data);
    if (rc < 0)
        return false;

    log_s("[kld-fw] ");
    log_s(family[0] ? family : "-");
    log_s("/");
    log_s(name);
    log_s(": served from the KURONO store (");
    log_dec(got);
    log_s(" bytes)\r\n");
    g_served++;
    return true;
}

}  // namespace

bool EnsureFile(const char* kurono_path, uint64_t address_space,
                const char* exe_path)
{
    if (!kurono_path || !starts(kurono_path, FW_PREFIX))
        return true;   // not ours
    // the name is checked BEFORE kvfs: kvfs may normalize ".." and find a
    // real file, which is exactly what a path escape wants. (satoru)
    const char* name = kurono_path + slen(FW_PREFIX);
    if (!valid_name(name)) {
        g_refused++;
        log_s("[kld-fw] reject ");
        log_s(name);
        log_s(": not a plain firmware file name\r\n");
        return false;
    }
    if (KVFS::Resolve(kurono_path))
        return true;   // embedded set (or an earlier request) already has it

    char family[16];
    family_for(address_space, exe_path, family);
    if (!valid_family(family)) {
        g_misses++;
        log_s("[kld-fw] ");
        log_s(name);
        log_s(": no container family, store skipped\r\n");
        return true;
    }

    g_lock.Lock();
    if (KVFS::Resolve(kurono_path)) {
        g_lock.Unlock();
        return true;
    }
    bool mounted = store_mount();
    bool ok = mounted && load_one(family, name, kurono_path);
    g_lock.Unlock();

    if (!ok) {
        g_misses++;
        log_s("[kld-fw] ");
        log_s(family);
        log_s("/");
        log_s(name);
        log_s(": not in store\r\n");
    }
    return true;
}

uint32_t Served() { return g_served; }
uint32_t Misses() { return g_misses; }
uint32_t Refused() { return g_refused; }

}  // namespace KldFw

// end (satoru)
