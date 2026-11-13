// bootcfg: see bootcfg.h. (satoru)
#include "bootcfg.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../kernel/pmm.h"
#include "../proc/scheduler.h"
#include "../hal/hal.h"
#include "../kernel/kcrash.h"   // the crash store opens in the watch process (satoru)

namespace {

const char BOOT83[11] = { 'K','B','O','O','T',' ',' ',' ','C','F','G' };
const uint32_t MAGIC = 0x544F424Bu;   // 'KBOT' (satoru)
volatile uint32_t g_lock = 0;

uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
void rds(const uint8_t* p, char* out) { for (int i = 0; i < 31; i++) out[i] = (char)p[i]; out[31] = 0; }
void wrs(uint8_t* p, const char* s) { int i = 0; for (; i < 31 && s[i]; i++) p[i] = (uint8_t)s[i]; for (; i < 32; i++) p[i] = 0; }

// one 4 KiB bounce page: disklog reads into identity mapped ram. (satoru)
alignas(4096) uint8_t g_page[4096];

bool load(uint64_t* lba, uint64_t* span) {
    uint32_t size = 0;
    if (DiskLog::RootFileExtent(BOOT83, lba, span, &size) != 0 || size != 512) return false;
    for (int i = 0; i < 512; i++) g_page[i] = 0;
    // the read lands in identity mapped ram (a pmm page), not in this static,
    // which sits in the high-half image. (satoru)
    static uint8_t* rd = nullptr;
    if (!rd) rd = (uint8_t*)PMM::AllocBytes(4096);
    if (!rd) return false;
    if (DiskLog::ReadRootFile(BOOT83, rd, 4096, nullptr) != 0) return false;
    for (int i = 0; i < 512; i++) g_page[i] = rd[i];
    return rd32(g_page) == MAGIC && rd32(g_page + 508) == fnv(g_page, 508);
}

void decode(BootCfg::State* s) {
    s->boot_count = rd32(g_page + 8);
    s->flags = rd32(g_page + 12);
    rds(g_page + 16, s->last_good);
    s->trial_tries = rd32(g_page + 48);
    rds(g_page + 52, s->trial);
    rds(g_page + 84, s->current);
    s->confirmed_boot = rd32(g_page + 116);
    s->fail_streak = rd32(g_page + 120);
    s->recovery_reason = rd32(g_page + 124);
    rds(g_page + 128, s->previous);
}

void encode(const BootCfg::State& s) {
    wr32(g_page + 12, s.flags);
    wrs(g_page + 16, s.last_good);
    wr32(g_page + 48, s.trial_tries);
    wrs(g_page + 52, s.trial);
    wrs(g_page + 84, s.current);
    wr32(g_page + 116, s.confirmed_boot);
    wr32(g_page + 120, s.fail_streak);
    wr32(g_page + 124, s.recovery_reason);
    wrs(g_page + 128, s.previous);
    wr32(g_page + 508, fnv(g_page, 508));
}

void lock() { while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) __asm__ __volatile__("pause"); }
void unlock() { __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE); }

void scopy(char* d, const char* s) { int i = 0; for (; i < 31 && s && s[i]; i++) d[i] = s[i]; d[i] = 0; }
bool seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

}  // namespace

namespace BootCfg {

bool Read(State* out) {
    lock();
    uint64_t lba, span;
    bool ok = load(&lba, &span);
    if (ok) decode(out);
    unlock();
    return ok;
}

bool Update(void (*edit)(State& s, void* ctx), void* ctx) {
    lock();
    uint64_t lba = 0, span = 0;
    bool ok = load(&lba, &span);
    if (ok) {
        State s;
        decode(&s);
        edit(s, ctx);
        encode(s);
        uint32_t lb = DiskLog::LbaSize();
        uint32_t n = lb ? (512 + lb - 1) / lb * lb : 512;
        ok = false;
        // a busy disk gives the write back: a few short tries. (satoru)
        for (int t = 0; t < 50 && !ok; t++) {
            ok = DiskLog::WriteRootExtent(lba, g_page, n);
            if (!ok) for (volatile int d = 0; d < 200000; d++) __asm__ __volatile__("pause");
        }
    }
    unlock();
    if (!ok) SerialLogger::Log("[bootcfg] KBOOT.CFG not updated (no file on the KURONO partition, or busy)\r\n");
    return ok;
}

bool RequestRecoveryOnce() {
    return Update([](State& s, void*) { s.flags |= F_RECOVERY_ONCE; s.recovery_reason = 1; }, nullptr);
}
bool RequestSafeStartOnce() {
    return Update([](State& s, void*) { s.flags |= F_SAFE_ONCE; }, nullptr);
}
bool RequestPreviousOnce() {
    return Update([](State& s, void*) { s.flags |= F_PREVIOUS_ONCE; }, nullptr);
}

void ConfirmDesktop(const char* slot) {
    bool ok = Update([](State& s, void* c) {
        const char* sl = (const char*)c;
        s.fail_streak = 0;
        s.confirmed_boot = s.boot_count;
        s.recovery_reason = 0;
        s.flags &= ~(uint32_t)F_TRIAL_FAILED;
        if (sl && sl[0] && s.trial[0] && seq(sl, s.trial)) {
            // the trial reached the desktop: it becomes the default. (satoru)
            scopy(s.previous, s.current[0] ? s.current : "EMBEDDED");
            scopy(s.current, s.trial);
            s.trial[0] = 0;
            s.trial_tries = 0;
        }
        scopy(s.last_good, sl && sl[0] ? sl : "EMBEDDED");
    }, (void*)slot);
    if (ok) SerialLogger::Log("[bootcfg] desktop confirmed\r\n");
}

bool SetTrial(const char* name83, uint32_t tries) {
    struct A { const char* n; uint32_t t; } a = { name83, tries };
    return Update([](State& s, void* c) {
        A* x = (A*)c;
        scopy(s.trial, x->n);
        s.trial_tries = x->t;
        s.flags &= ~(uint32_t)F_TRIAL_FAILED;
    }, &a);
}

namespace {
char g_slot[32];
volatile bool g_desktop = false;

// a kernel process entry must never return (there is no exit path behind
// it): park here once the job is done. (satoru)
[[noreturn]] void park() { for (;;) Scheduler::SleepMs(600000); }

volatile bool g_kcrash_disk = false;   // kurono.kcrash=1 (satoru)

[[noreturn]] void watch_entry() {
    KCrash::BootInit(g_kcrash_disk);   // creates KBOOT.CFG with kurono.kcrash=1 (fix 4) (satoru)
    State s;
    if (!Read(&s)) {
        SerialLogger::Log("[bootwatch] no KBOOT.CFG: nothing to confirm\r\n");
        park();
    }
    bool trial = g_slot[0] && s.trial[0] && seq(g_slot, s.trial);
    SerialLogger::Log("[bootwatch] slot=");
    SerialLogger::Log(g_slot[0] ? g_slot : "(none)");
    SerialLogger::Log(trial ? " (trial)\r\n" : "\r\n");
    uint32_t waited = 0;
    while (!g_desktop) {
        Scheduler::SleepMs(1000);
        if (++waited >= kTrialDeadlineS && trial) {
            // the new kernel never reached the desktop: back to the previous one. (satoru)
            SerialLogger::Log("[bootwatch] trial kernel has no desktop after the deadline, restarting\r\n");
            DiskLog::Flush();
            HAL::Reboot();
        }
    }
    for (uint32_t t = 0; t < kConfirmAfterS; t++) Scheduler::SleepMs(1000);
    ConfirmDesktop(g_slot);
    park();
}
}  // namespace

void StartWatch(const char* slot) {
    scopy(g_slot, slot ? slot : "");
    Scheduler::SpawnKernelProcess("bootwatch", watch_entry, PRIO_NORMAL, 16, 64);
}

void NoteDesktop() { g_desktop = true; }
void SetCrashStoreDisk(bool on) { g_kcrash_disk = on; }

}  // namespace BootCfg

// end (satoru)
