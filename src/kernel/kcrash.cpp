// kcrash: crash record ring + KCRASH.LOG / KDRIVERS.CFG / KBOOT.CFG. formats in
// kcrash.h. files are looked up (and created if missing) only in BootInit; a
// failure path only overwrites their preallocated clusters. (satoru)

#include "kcrash.h"
#include "pmm.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/rtc.h"
#include "../proc/scheduler.h"

namespace KCrash {
namespace {

constexpr uint32_t REC_MAGIC  = 0x3152434Bu;   // 'KCR1' (satoru)
constexpr uint32_t LOGH_MAGIC = 0x4852434Bu;   // 'KCRH' (satoru)
constexpr uint32_t DRV_MAGIC  = 0x5652444Bu;   // 'KDRV' (satoru)
constexpr uint32_t BOOT_MAGIC = 0x544F424Bu;   // 'KBOT' (satoru)
constexpr uint32_t LOG_BYTES  = 1u << 20;
constexpr uint32_t SLOT_BYTES = 4096;
constexpr int      SLOTS      = 16;
constexpr uint32_t DRV_BYTES  = 4096;
constexpr int      DRV_ENTRIES = 64;
constexpr uint32_t BOOT_BYTES = 512;

const char LOG83[11]  = { 'K','C','R','A','S','H',' ',' ','L','O','G' };
const char DRV83[11]  = { 'K','D','R','I','V','E','R','S','C','F','G' };
const char BOOT83[11] = { 'K','B','O','O','T',' ',' ',' ','C','F','G' };

struct __attribute__((packed)) DrvEntry {
    char     family[16];
    uint8_t  policy;
    uint8_t  pad[3];
    uint32_t fails;
    uint32_t last_fail_boot;
    uint32_t reserved;
};
static_assert(sizeof(DrvEntry) == 32, "kdrivers entry is 32 bytes");

// in-memory state. the page buffers are only memcpy sources for disklog, which
// copies them into its own dma bounce buffer. (satoru)
// g_ring_n records were reported, g_flushed of them are on disk (or were
// overwritten in the ring before the disk was free). (satoru)
Record   g_ring[SLOTS];
uint32_t g_ring_n = 0;
uint32_t g_flushed = 0;
bool     g_hdr_dirty = false;      // a slot landed but its header write did not (satoru)
bool     g_drv_dirty = false;      // KDRIVERS.CFG changed in memory (satoru)
uint32_t g_busy_logged = 0;        // last seq the "not yet on disk" line was printed for (satoru)
volatile uint32_t g_flushing = 0;  // one flusher at a time; it owns g_page (satoru)
uint32_t g_seq = 0;
uint32_t g_boot_id = 0;
volatile uint32_t g_lock = 0;

bool     g_disk = false;           // all three extents known (satoru)
uint64_t g_log_lba = 0, g_drv_lba = 0, g_boot_lba = 0;
uint64_t g_drv_span = 0, g_boot_span = 0;
uint32_t g_log_next = 0, g_log_count = 0;
alignas(4096) uint8_t g_page[4096];   // one 4 KiB write at a time (satoru)
alignas(4096) uint8_t g_drv[4096];    // KDRIVERS.CFG image (satoru)
bool     g_drv_ok = false;
Record   g_last_disk;               // newest record of an earlier boot (satoru)
bool     g_have_last_disk = false;

uint32_t fnv(const uint8_t* p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
void zero(void* d, uint32_t n) { uint8_t* p = (uint8_t*)d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
void copy(void* d, const void* s, uint32_t n)
{
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) a[i] = b[i];
}
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
// families are stored truncated to 15 characters: compare that form on both
// sides so a longer name still matches its own entry. (satoru)
bool fam_eq(const char* a, const char* b)
{
    for (int i = 0; i < 15; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}

void lock()   { while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) __asm__ __volatile__("pause"); }
void unlock() { __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE); }

// whole-lba write of len bytes (rounded up) from buf at byte offset off of an
// extent that spans `span` bytes. (satoru)
bool put(uint64_t lba, uint64_t span, uint32_t off, const uint8_t* buf, uint32_t len)
{
    uint32_t lb = DiskLog::LbaSize();
    if (!lb || off % lb) return false;
    uint32_t n = (len + lb - 1) / lb * lb;
    if ((uint64_t)off + n > span) return false;
    return DiskLog::WriteRootExtent(lba + off / lb, buf, n);
}

// boot-time writes may wait: a few short tries with a sleep between. (satoru)
bool put_boot(uint64_t lba, uint64_t span, uint32_t off, const uint8_t* buf, uint32_t len)
{
    for (int t = 0; t < 100; t++) {
        if (put(lba, span, off, buf, len)) return true;
        Scheduler::SleepMs(20);
    }
    return false;
}

void log_record(const char* tag, const Record& r)
{
    SerialLogger::Log(tag);
    SerialLogger::Log(" seq="); SerialLogger::LogDec((int)r.seq);
    SerialLogger::Log(" boot="); SerialLogger::LogDec((int)r.boot_id);
    SerialLogger::Log(" kind="); SerialLogger::Log(r.kind == KIND_KLD ? "KLD" : "?");
    char fam[17]; copy(fam, r.family, 16); fam[16] = 0;
    SerialLogger::Log(" family="); SerialLogger::Log(fam);
    SerialLogger::Log(" bdf="); SerialLogger::LogHex(r.bdf);
    SerialLogger::Log(" id="); SerialLogger::LogHex(((uint32_t)r.vendor << 16) | r.device);
    SerialLogger::Log(" pid="); SerialLogger::LogDec(r.pid);
    SerialLogger::Log(" rc="); SerialLogger::LogDec(r.rc);
    SerialLogger::Log(" sig="); SerialLogger::LogDec(r.signal);
    SerialLogger::Log(" attempt="); SerialLogger::LogDec(r.attempt);
    SerialLogger::Log(r.result == RES_GAVE_UP ? " result=gave-up" : " result=restarting");
    char act[33]; copy(act, r.action, 32); act[32] = 0;
    SerialLogger::Log(" action=\""); SerialLogger::Log(act); SerialLogger::Log("\"");
    SerialLogger::Log(" up_ms="); SerialLogger::LogDec((int)r.uptime_ms);
    SerialLogger::Log("\r\n");
}

uint64_t rtc_now()
{
    RTC::Date d; RTC::Time t;
    if (!RTC::ReadDateTime(d, t)) return 0;
    uint64_t v = d.year;
    v = v * 100 + d.mon; v = v * 100 + d.dom;
    v = v * 100 + t.h;   v = v * 100 + t.m; v = v * 100 + t.s;
    return v;
}

// find a root file, creating it (zero filled, then `init` over the start) when
// missing. boot time only: the create allocates fat clusters. (satoru)
bool open_file(const char* n83, uint32_t want, const uint8_t* init, uint32_t init_len,
               uint64_t* lba, uint64_t* span, const char* name)
{
    uint32_t size = 0;
    int rc = DiskLog::RootFileExtent(n83, lba, span, &size);
    if (rc == 2) {
        uint8_t* z = (uint8_t*)PMM::AllocBytes(want);
        if (!z) return false;
        zero(z, want);
        if (init) copy(z, init, init_len);
        int wr = DiskLog::WriteRootFile(n83, z, want);
        PMM::FreeBytes(z, want);
        SerialLogger::Log("[kcrash] created "); SerialLogger::Log(name);
        SerialLogger::Log(wr == 0 ? "\r\n" : " FAILED\r\n");
        if (wr != 0) return false;
        rc = DiskLog::RootFileExtent(n83, lba, span, &size);
    }
    if (rc != 0) {
        SerialLogger::Log("[kcrash] "); SerialLogger::Log(name);
        SerialLogger::Log(rc == 5 ? " is fragmented or misaligned, not used\r\n" : " not usable\r\n");
        return false;
    }
    if (size != want) {
        SerialLogger::Log("[kcrash] "); SerialLogger::Log(name);
        SerialLogger::Log(" has the wrong size, not used\r\n");
        return false;
    }
    return true;
}

void drv_seal()
{
    wr32(g_drv + 0, DRV_MAGIC);
    wr16(g_drv + 4, 1);
    wr16(g_drv + 6, DRV_ENTRIES);
    wr32(g_drv + 8, fnv(g_drv + 16, DRV_BYTES - 16));
}

DrvEntry* drv_find(const char* fam, bool create)
{
    DrvEntry* e = (DrvEntry*)(g_drv + 16);
    for (int i = 0; i < DRV_ENTRIES; i++)
        if (e[i].family[0] && fam_eq(e[i].family, fam)) return &e[i];
    if (!create) return nullptr;
    for (int i = 0; i < DRV_ENTRIES; i++) {
        if (e[i].family[0]) continue;
        zero(&e[i], sizeof(DrvEntry));
        for (int k = 0; k < 15 && fam[k]; k++) e[i].family[k] = fam[k];
        return &e[i];
    }
    return nullptr;
}

bool read_file(const char* n83, uint8_t* dst, uint32_t len)
{
    uint8_t* b = (uint8_t*)PMM::AllocBytes(len);
    if (!b) return false;
    bool ok = DiskLog::ReadRootFile(n83, b, len, nullptr) == 0;
    if (ok) copy(dst, b, len);
    PMM::FreeBytes(b, len);
    return ok;
}

void boot_disk()
{
    // KBOOT.CFG: read, bump the boot counter, write back. nothing acts on its
    // flags yet. (satoru)
    {
        uint8_t init[BOOT_BYTES];
        zero(init, BOOT_BYTES);
        wr32(init, BOOT_MAGIC); wr16(init + 4, 1);
        wr32(init + 508, fnv(init, 508));
        uint64_t span = 0;
        if (!open_file(BOOT83, BOOT_BYTES, init, BOOT_BYTES, &g_boot_lba, &span, "KBOOT.CFG")) return;
        g_boot_span = span;
        zero(g_page, sizeof(g_page));
        if (!read_file(BOOT83, g_page, BOOT_BYTES)) return;
        bool ok = rd32(g_page) == BOOT_MAGIC && rd32(g_page + 508) == fnv(g_page, 508);
        if (!ok) {
            SerialLogger::Log("[kcrash] KBOOT.CFG bad magic or checksum, reset\r\n");
            copy(g_page, init, BOOT_BYTES);
        }
        g_boot_id = rd32(g_page + 8) + 1;
        wr32(g_page + 8, g_boot_id);
        wr32(g_page + 508, fnv(g_page, 508));
        bool w = put_boot(g_boot_lba, g_boot_span, 0, g_page, BOOT_BYTES);
        SerialLogger::Log("[kcrash] KBOOT.CFG boot_count="); SerialLogger::LogDec((int)g_boot_id);
        SerialLogger::Log(" flags="); SerialLogger::LogHex(rd32(g_page + 12));
        SerialLogger::Log(w ? "\r\n" : " (write FAILED)\r\n");
    }
    // KDRIVERS.CFG. (satoru)
    {
        zero(g_drv, DRV_BYTES);
        drv_seal();
        if (!open_file(DRV83, DRV_BYTES, g_drv, DRV_BYTES, &g_drv_lba, &g_drv_span, "KDRIVERS.CFG")) return;
        if (!read_file(DRV83, g_drv, DRV_BYTES) ||
            rd32(g_drv) != DRV_MAGIC || rd32(g_drv + 8) != fnv(g_drv + 16, DRV_BYTES - 16)) {
            SerialLogger::Log("[kcrash] KDRIVERS.CFG bad magic or checksum, using defaults\r\n");
            zero(g_drv, DRV_BYTES);
            drv_seal();
        }
        g_drv_ok = true;
        DrvEntry* e = (DrvEntry*)(g_drv + 16);
        for (int i = 0; i < DRV_ENTRIES; i++) {
            if (!e[i].family[0]) continue;
            char fam[17]; copy(fam, e[i].family, 16); fam[16] = 0;
            SerialLogger::Log("[kcrash] KDRIVERS.CFG "); SerialLogger::Log(fam);
            SerialLogger::Log(e[i].policy == 1 ? " disabled" : e[i].policy == 2 ? " nostop" : " enabled");
            SerialLogger::Log(" fails="); SerialLogger::LogDec((int)e[i].fails);
            SerialLogger::Log("\r\n");
        }
    }
    // KCRASH.LOG: header, then print the newest record of an earlier boot. (satoru)
    {
        uint8_t init[32];
        zero(init, sizeof(init));
        wr32(init, LOGH_MAGIC); wr32(init + 4, 1);
        wr32(init + 16, fnv(init, 16));
        uint64_t span = 0;
        if (!open_file(LOG83, LOG_BYTES, init, sizeof(init), &g_log_lba, &span, "KCRASH.LOG")) return;
        uint8_t* b = (uint8_t*)PMM::AllocBytes(LOG_BYTES);
        if (!b) return;
        if (DiskLog::ReadRootFile(LOG83, b, LOG_BYTES, nullptr) != 0) { PMM::FreeBytes(b, LOG_BYTES); return; }
        if (rd32(b) == LOGH_MAGIC && rd32(b + 16) == fnv(b, 16)) {
            g_log_next = rd32(b + 8) % SLOTS;
            g_log_count = rd32(b + 12);
            if (g_log_count > SLOTS) g_log_count = SLOTS;
        } else {
            SerialLogger::Log("[kcrash] KCRASH.LOG header bad, starting empty\r\n");
            g_log_next = 0; g_log_count = 0;
        }
        if (g_log_count) {
            uint32_t last = (g_log_next + SLOTS - 1) % SLOTS;
            Record r; copy(&r, b + SLOT_BYTES + last * SLOT_BYTES, sizeof(Record));
            if (r.magic == REC_MAGIC && r.checksum == fnv((const uint8_t*)&r, sizeof(Record) - 4)) {
                log_record("[kcrash] last:", r);
                g_last_disk = r; g_have_last_disk = true;   // for recovery's fix last issue (satoru)
            }
            else
                SerialLogger::Log("[kcrash] last: record damaged\r\n");
            g_seq = r.seq;
        } else {
            SerialLogger::Log("[kcrash] last: none\r\n");
        }
        PMM::FreeBytes(b, LOG_BYTES);
        g_disk = true;
        SerialLogger::Log("[kcrash] KCRASH.LOG ready (");
        SerialLogger::LogDec((int)g_log_count); SerialLogger::Log(" records)\r\n");
    }
}

}  // namespace

// fix 4 (1112): the only caller used to be the kld supervisor's first family
// runner, so a boot without a supervised kld family never opened the store and
// kurono.kcrash=1 never created KBOOT.CFG. the boot watch process calls this too
// now: the first call does the work (a later disk call still upgrades a memory
// only one), every other returns at once, so KBOOT.CFG's boot counter is bumped
// once per boot. a caller that comes in while another runs waits for it. (satoru)
void BootInit(bool disk)
{
    static volatile uint32_t s_state = 0;   // 0 none, 1 running, 2 memory only, 3 disk tried (satoru)
    uint32_t want = disk ? 3u : 2u;
    for (;;) {
        uint32_t cur = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
        if (cur >= want) return;
        if (cur == 1) { Scheduler::SleepMs(5); continue; }   // yield: the holder may be asleep in put_boot on this cpu (satoru)
        if (__atomic_compare_exchange_n(&s_state, &cur, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    if (!disk) {
        SerialLogger::Log("[kcrash] crash records in memory only (kurono.kcrash=1 adds the disk)\r\n");
        __atomic_store_n(&s_state, 2u, __ATOMIC_RELEASE);
        return;
    }
    boot_disk();
    if (!g_disk)
        SerialLogger::Log("[kcrash] no usable KURONO partition files: crash records in memory only\r\n");
    __atomic_store_n(&s_state, 3u, __ATOMIC_RELEASE);
}

void BootFileImage(uint8_t* out512)
{
    zero(out512, BOOT_BYTES);
    wr32(out512, BOOT_MAGIC); wr16(out512 + 4, 1);
    wr32(out512 + 508, fnv(out512, 508));
}

bool FamilyDisabled(const char* family)
{
    if (!g_drv_ok || !family) return false;
    DrvEntry* e = drv_find(family, false);
    return e && e->policy == 1;
}

void Report(Record& r)
{
    lock();
    r.magic = REC_MAGIC;
    r.version = 1;
    r.boot_id = g_boot_id;
    r.seq = ++g_seq;
    r.uptime_ms = Timer::GetRealMs64();
    r.rtc = rtc_now();
    r.checksum = fnv((const uint8_t*)&r, sizeof(Record) - 4);
    g_ring[g_ring_n % SLOTS] = r;
    g_ring_n++;
    unlock();
    log_record("[kcrash]", r);
    FlushPending();
}

// write what the ring and KDRIVERS.CFG still owe the disk. never holds the
// kcrash lock across a disk write, and gives up at once when the disk is
// busy: the caller polls again later. (satoru)
void FlushPending()
{
    if (!g_disk && !g_drv_ok) return;
    if (__atomic_exchange_n(&g_flushing, 1u, __ATOMIC_ACQUIRE)) return;
    for (;;) {
        bool hdr = false;
        Record r;
        uint32_t slot = 0, next = 0, count = 0;
        lock();
        if (g_disk && g_ring_n - g_flushed > (uint32_t)SLOTS) g_flushed = g_ring_n - SLOTS;
        bool have = g_disk && g_flushed < g_ring_n;
        if (have) { r = g_ring[g_flushed % SLOTS]; slot = g_log_next; }
        else if (g_disk && g_hdr_dirty) hdr = true;
        unlock();
        if (have) {
            zero(g_page, sizeof(g_page));
            copy(g_page, &r, sizeof(Record));
            if (!put(g_log_lba, LOG_BYTES, SLOT_BYTES + slot * SLOT_BYTES, g_page, SLOT_BYTES)) {
                if (g_busy_logged != r.seq) {
                    g_busy_logged = r.seq;
                    SerialLogger::Log("[kcrash] disk busy, record seq=");
                    SerialLogger::LogDec((int)r.seq);
                    SerialLogger::Log(" kept in memory (not yet on disk)\r\n");
                }
                break;
            }
            lock();
            g_flushed++;
            g_log_next = (g_log_next + 1) % SLOTS;
            if (g_log_count < SLOTS) g_log_count++;
            g_hdr_dirty = true;
            unlock();
            SerialLogger::Log("[kcrash] record seq=");
            SerialLogger::LogDec((int)r.seq);
            SerialLogger::Log(" written to KCRASH.LOG\r\n");
            hdr = true;
        }
        if (hdr) {
            // slot first, header second: a torn write leaves the old header
            // pointing at complete records. (satoru)
            lock();
            next = g_log_next; count = g_log_count;
            g_hdr_dirty = false;
            unlock();
            zero(g_page, sizeof(g_page));
            wr32(g_page, LOGH_MAGIC); wr32(g_page + 4, 1);
            wr32(g_page + 8, next); wr32(g_page + 12, count);
            wr32(g_page + 16, fnv(g_page, 16));
            if (!put(g_log_lba, LOG_BYTES, 0, g_page, SLOT_BYTES)) {
                lock(); g_hdr_dirty = true; unlock();
                break;
            }
            continue;
        }
        break;
    }
    // KDRIVERS.CFG: copy under the lock, write without it. (satoru)
    bool drv = false;
    lock();
    if (g_drv_ok && g_drv_dirty) { copy(g_page, g_drv, DRV_BYTES); g_drv_dirty = false; drv = true; }
    unlock();
    if (drv) {
        if (put(g_drv_lba, g_drv_span, 0, g_page, DRV_BYTES)) {
            SerialLogger::Log("[kcrash] KDRIVERS.CFG written\r\n");
        } else {
            lock(); g_drv_dirty = true; unlock();
            SerialLogger::Log("[kcrash] disk busy, KDRIVERS.CFG kept in memory (not yet on disk)\r\n");
        }
    }
    __atomic_store_n(&g_flushing, 0u, __ATOMIC_RELEASE);
}

bool Pending()
{
    lock();
    bool p = (g_disk && (g_flushed < g_ring_n || g_hdr_dirty)) || (g_drv_ok && g_drv_dirty);
    unlock();
    return p;
}

void NoteGiveUp(const char* family)
{
    if (!g_drv_ok || !family) return;
    lock();
    DrvEntry* e = drv_find(family, true);
    if (e) {
        e->fails++;
        e->last_fail_boot = g_boot_id;
        drv_seal();
        g_drv_dirty = true;
    }
    unlock();
    SerialLogger::Log("[kcrash] KDRIVERS.CFG ");
    SerialLogger::Log(family);
    SerialLogger::Log(e ? " fail counter bumped\r\n" : " table full, fail counter not kept\r\n");
    FlushPending();
}

bool LastRecord(Record* out)
{
    lock();
    bool ok = false;
    if (g_ring_n) { *out = g_ring[(g_ring_n - 1) % SLOTS]; ok = true; }
    else if (g_have_last_disk) { *out = g_last_disk; ok = true; }
    unlock();
    return ok;
}

int DriverCount()
{
    if (!g_drv_ok) return 0;
    int n = 0;
    DrvEntry* e = (DrvEntry*)(g_drv + 16);
    for (int i = 0; i < DRV_ENTRIES; i++) if (e[i].family[0]) n++;
    return n;
}

bool DriverAt(int idx, char* family16, uint8_t* policy, uint32_t* fails)
{
    if (!g_drv_ok) return false;
    DrvEntry* e = (DrvEntry*)(g_drv + 16);
    int n = 0;
    for (int i = 0; i < DRV_ENTRIES; i++) {
        if (!e[i].family[0]) continue;
        if (n++ != idx) continue;
        copy(family16, e[i].family, 15); family16[15] = 0;
        if (policy) *policy = e[i].policy;
        if (fails) *fails = e[i].fails;
        return true;
    }
    return false;
}

bool SetDriverPolicy(const char* family, uint8_t policy)
{
    if (!g_drv_ok || !family) return false;
    lock();
    DrvEntry* e = drv_find(family, true);
    if (e) { e->policy = policy; drv_seal(); g_drv_dirty = true; }
    unlock();
    FlushPending();
    return e != nullptr;
}

}  // namespace KCrash

// end (satoru)
