#include "kasset.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../kernel/pmm.h"
#include "../fs/kvfs.h"
#include "../drivers/disklog.h"

// see kasset.h. a small fixed table of named assets; each is looked up on the
// KURONO partition at most once and loaded at most once. state transitions use
// atomics so two kernel processes asking at the same time can't both load it:
// the loser just sees "not ready" and gets nullptr for that call. (satoru)

namespace {
    enum : int { ST_UNKNOWN = 0, ST_MISSING = 1, ST_PRESENT = 2, ST_LOADING = 3, ST_READY = 4 };

    struct Entry {
        char     name[11];
        volatile int state;
        uint64_t size;
        uint8_t* data;
    };

    const int MAX_ASSETS = 32;
    Entry g_assets[MAX_ASSETS];
    volatile int g_count = 0;

    bool same83(const char* a, const char* b) {
        for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
        return true;
    }

    bool embedded(const uint8_t* s, const uint8_t* e) {
        return s != nullptr && e != nullptr && e > s;
    }

    Entry* find_or_add(const char* name83) {
        int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; i++) if (same83(g_assets[i].name, name83)) return &g_assets[i];
        // claim a new slot (a duplicate slot for the same name is harmless: it
        // only costs a second directory lookup) (satoru)
        int slot = __atomic_fetch_add(&g_count, 1, __ATOMIC_ACQ_REL);
        if (slot >= MAX_ASSETS) { __atomic_store_n(&g_count, MAX_ASSETS, __ATOMIC_RELEASE); return nullptr; }
        Entry* en = &g_assets[slot];
        for (int i = 0; i < 11; i++) en->name[i] = name83[i];
        en->size = 0; en->data = nullptr;
        __atomic_store_n(&en->state, ST_UNKNOWN, __ATOMIC_RELEASE);
        return en;
    }

    void log_name(const char* tag, const Entry* en) {
        char nm[13]; int k = 0;
        for (int i = 0; i < 8 && en->name[i] != ' '; i++) nm[k++] = en->name[i];
        nm[k++] = '.';
        for (int i = 8; i < 11 && en->name[i] != ' '; i++) nm[k++] = en->name[i];
        nm[k] = 0;
        SerialLogger::Log("[kasset] "); SerialLogger::Log(tag); SerialLogger::Log(" "); SerialLogger::Log(nm);
    }

    // directory lookup only (satoru)
    void probe(Entry* en) {
        if (__atomic_load_n(&en->state, __ATOMIC_ACQUIRE) != ST_UNKNOWN) return;
        uint64_t sz = 0;
        int rc = DiskLog::StatRootFile(en->name, &sz);
        if (rc == 0 && sz > 0 && sz < (1ull << 31)) {
            en->size = sz;
            int exp = ST_UNKNOWN;
            __atomic_compare_exchange_n(&en->state, &exp, ST_PRESENT, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        } else {
            // not cached as missing for good: the partition may simply not be
            // mounted yet this early in boot, so leave it unknown (satoru)
        }
    }
}

namespace KAsset {

bool Has(const char* name83, const uint8_t* s, const uint8_t* e) {
    if (embedded(s, e)) return true;
    Entry* en = find_or_add(name83);
    if (!en) return false;
    probe(en);
    int st = __atomic_load_n(&en->state, __ATOMIC_ACQUIRE);
    return st == ST_PRESENT || st == ST_LOADING || st == ST_READY;
}

const uint8_t* Data(const char* name83, const uint8_t* s, const uint8_t* e) {
    if (embedded(s, e)) return s;
    Entry* en = find_or_add(name83);
    if (!en) return nullptr;
    probe(en);
    int exp = ST_PRESENT;
    if (__atomic_compare_exchange_n(&en->state, &exp, ST_LOADING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        uint64_t alloc = (en->size + 4095) & ~4095ull;
        uint8_t* buf = (uint8_t*)PMM::AllocBytes((size_t)alloc);
        int rc = buf ? DiskLog::ReadRootFile(en->name, buf, alloc, nullptr) : -1;
        if (rc == 0) {
            en->data = buf;
            __atomic_store_n(&en->state, ST_READY, __ATOMIC_RELEASE);
            log_name("loaded", en); SerialLogger::Log(" kb "); SerialLogger::LogDec((int)(en->size >> 10)); SerialLogger::Log("\r\n");
        } else {
            if (buf) PMM::FreeBytes(buf, (size_t)alloc);
            __atomic_store_n(&en->state, ST_PRESENT, __ATOMIC_RELEASE);   // allow a retry later (satoru)
            log_name("load failed", en); SerialLogger::Log(" rc "); SerialLogger::LogDec(rc); SerialLogger::Log("\r\n");
            return nullptr;
        }
    }
    return __atomic_load_n(&en->state, __ATOMIC_ACQUIRE) == ST_READY ? en->data : nullptr;
}

uint32_t Size(const char* name83, const uint8_t* s, const uint8_t* e) {
    if (embedded(s, e)) return (uint32_t)(e - s);
    Entry* en = find_or_add(name83);
    if (!en) return 0;
    probe(en);
    int st = __atomic_load_n(&en->state, __ATOMIC_ACQUIRE);
    return (st == ST_PRESENT || st == ST_LOADING || st == ST_READY) ? (uint32_t)en->size : 0;
}

void Release(const char* name83) {
    int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n && i < MAX_ASSETS; i++) {
        Entry* en = &g_assets[i];
        if (!same83(en->name, name83)) continue;
        int exp = ST_READY;
        if (__atomic_compare_exchange_n(&en->state, &exp, ST_LOADING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            uint64_t alloc = (en->size + 4095) & ~4095ull;
            if (en->data) PMM::FreeBytes(en->data, (size_t)alloc);
            en->data = nullptr;
            __atomic_store_n(&en->state, ST_PRESENT, __ATOMIC_RELEASE);
            log_name("released", en); SerialLogger::Log("\r\n");
        }
    }
}

bool EnsureVfsFile(const char* path, const char* name83, const uint8_t* s, const uint8_t* e) {
    if (KVFS::Exists(path)) return true;
    const uint8_t* data = Data(name83, s, e);
    uint32_t sz = Size(name83, s, e);
    if (!data || !sz) return false;
    char dir[128]; int last = -1;
    for (int i = 0; path[i] && i < 127; i++) { dir[i] = path[i]; if (path[i] == '/') last = i; }
    if (last > 0) { dir[last] = 0; KVFS::Mkdirs(dir); }
    KVFS::WriteFileStatic(path, data, sz);
    return true;
}

bool Deliver(const char* name83, const uint8_t* s, const uint8_t* e) {
    if (!Linked(s, e)) return false;
    uint64_t have = 0;
    if (DiskLog::StatRootFile(name83, &have) == 0) return true;
    uint32_t len = (uint32_t)(e - s);
    int rc = DiskLog::WriteRootFile(name83, s, len);
    SerialLogger::Log("[kasset] deliver ");
    for (int i = 0; i < 11 && name83[i] != ' '; i++) { char c[2] = { name83[i], 0 }; SerialLogger::Log(c); }
    SerialLogger::Log(" rc="); SerialLogger::LogDec(rc); SerialLogger::Log("\r\n");
    return rc == 0;
}

}  // namespace KAsset

// end (satoru)
