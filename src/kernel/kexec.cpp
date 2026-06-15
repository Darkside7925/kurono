//  kurono os: kexec, the kernel executive implementation.
//
//  see kexec.h. a thin, documented facade over the existing subsystems so kdf/udf
//  drivers + the rest of the kernel call named executive services rather than
//  reaching into internals. each namespace forwards to the real implementation;
//  the only state kexec OWNS is the small Config key/value store. (satoru)

#include "kexec.h"
#include "pmm.h"
#include "vmm.h"
#include "irp.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../security/supr.h"

namespace KExec {

namespace {
int x_cat(char* o, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) o[p++] = *s++;
    if (p < mx) o[p] = 0;
    return p;
}
int x_cat_u(char* o, int p, int mx, uint64_t v) {
    char t[20]; int n = 0;
    if (v == 0) t[n++] = '0';
    else while (v && n < 20) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < mx - 1) o[p++] = t[--n];
    if (p < mx) o[p] = 0;
    return p;
}
bool x_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}
void x_cpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
bool g_inited = false;
}  // namespace

// ── Mm: memory ────────────────────────────────────────────────────────────────
namespace Memory {
void* AllocPages(uint64_t pages) {
    if (pages == 0) pages = 1;
    uint64_t phys = PMM::AllocContiguous(pages);
    return (void*)(uintptr_t)phys;   // identity-mapped below 16gb (satoru)
}
void FreePages(void* p, uint64_t pages) {
    if (!p || pages == 0) return;
    PMM::FreeContiguous((uint64_t)(uintptr_t)p, pages);
}
uint64_t PhysOf(void* virt) {
    return KernelVMM::QueryMapping((uint64_t)(uintptr_t)virt);
}
bool MapMMIO(uint64_t paddr, uint64_t bytes, uint64_t flags) {
    return KernelVMM::MapRange(paddr & ~0xFFFULL, paddr & ~0xFFFULL,
                               bytes, flags ? flags : (PTE_PRESENT | PTE_WRITABLE | PTE_PCD));
}
uint64_t FreeBytes() { return PMM::GetFreeMemory(); }
}  // namespace Memory

// ── Ps: process ─────────────────────────────────────────────────────────────
namespace Process {
uint32_t CurrentPid() {
    ::Process* p = Scheduler::GetCurrentProcess();
    return p ? p->pid : 0;
}
bool SpawnWorker(const char* name, Worker fn, int stack_kb) {
    if (!fn) return false;
    if (stack_kb < 16) stack_kb = 16;
    ::Process* p = Scheduler::SpawnKernelProcess(name ? name : "kexec-worker",
                                                 (KernelProcessEntry)fn, PRIO_NORMAL,
                                                 (uint32_t)stack_kb, (uint32_t)(stack_kb * 4));
    return p != nullptr;
}
void Yield()          { Scheduler::YieldNow(); }
void SleepMs(uint32_t ms) { Scheduler::SleepMs(ms); }
}  // namespace Process

// ── Io: i/o through the IRP executive ─────────────────────────────────────────
namespace IO {
int64_t ReadBlocks(const char* device, uint64_t lba, uint32_t count, void* buf) {
    int dev = IRP::FindDevice(device);
    if (dev < 0) return IRP::IRP_ENODEV;
    int32_t st = IRP::PostSync(dev, IRP::IRP_MJ_READ, lba, count, buf);
    return (st == IRP::IRP_SUCCESS) ? (int64_t)count : (int64_t)st;
}
int64_t WriteBlocks(const char* device, uint64_t lba, uint32_t count, const void* buf) {
    int dev = IRP::FindDevice(device);
    if (dev < 0) return IRP::IRP_ENODEV;
    int32_t st = IRP::PostSync(dev, IRP::IRP_MJ_WRITE, lba, count, (void*)(uintptr_t)buf);
    return (st == IRP::IRP_SUCCESS) ? (int64_t)count : (int64_t)st;
}
}  // namespace IO

// ── Se: security via SUPR ─────────────────────────────────────────────────────
namespace Security {
int CurrentLevel() {
    int sid = SUPR::GetCurrentSession();
    if (sid < 0) return 0;                   // no session = guest (satoru)
    return (int)SUPR::GetLevel(sid);
}
bool CheckCapability(uint32_t cap) {
    int lvl = CurrentLevel();
    // hardware access requires admin; net/fs/gui require at least a logged-in
    // user; this mirrors kinit's capability gate, centralized here. (satoru)
    if (cap & CAP_HARDWARE) return lvl >= (int)SUPR_ADMIN;
    return lvl >= (int)SUPR_USER;
}
}  // namespace Security

// ── Cm: config key/value store ────────────────────────────────────────────────
namespace Config {
namespace {
constexpr int CFG_MAX = 64;
constexpr int CFG_KEY = 48;
constexpr int CFG_VAL = 96;
struct Entry { bool used; char key[CFG_KEY]; char val[CFG_VAL]; };
Entry g_cfg[CFG_MAX];

int find(const char* key) {
    for (int i = 0; i < CFG_MAX; i++) if (g_cfg[i].used && x_eq(g_cfg[i].key, key)) return i;
    return -1;
}
int alloc(const char* key) {
    int i = find(key);
    if (i >= 0) return i;
    for (int j = 0; j < CFG_MAX; j++) if (!g_cfg[j].used) {
        g_cfg[j].used = true; x_cpy(g_cfg[j].key, key, CFG_KEY); return j;
    }
    return -1;
}
}  // namespace

bool SetString(const char* key, const char* value) {
    if (!key || !value) return false;
    int i = alloc(key);
    if (i < 0) return false;
    x_cpy(g_cfg[i].val, value, CFG_VAL);
    return true;
}
const char* GetString(const char* key, const char* def) {
    int i = find(key);
    return i >= 0 ? g_cfg[i].val : def;
}
bool SetU64(const char* key, uint64_t value) {
    char buf[24]; int n = x_cat_u(buf, 0, sizeof(buf), value);
    (void)n;
    return SetString(key, buf);
}
uint64_t GetU64(const char* key, uint64_t def) {
    int i = find(key);
    if (i < 0) return def;
    uint64_t v = 0; const char* s = g_cfg[i].val; bool any = false;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; any = true; }
    return any ? v : def;
}
}  // namespace Config

// ── lifecycle / status ─────────────────────────────────────────────────────────
void Init() {
    if (g_inited) return;
    g_inited = true;
    // record a couple of executive facts in the config store for introspection.
    // (satoru)
    Config::SetString("kexec.version", "1");
    Config::SetU64("kexec.mem.free.mb", PMM::GetFreeMemory() / (1024 * 1024));
    SerialLogger::Log("[KExec] kernel executive initialized (Mm/Ps/Io/Se/Cm)\r\n");
}

int Status(char* out, int mx) {
    if (!out || mx < 2) return 0;
    int p = 0;
    p = x_cat(out, p, mx, "KExec executive (NT-style Mm/Ps/Io/Se/Cm facade):\n");
    p = x_cat(out, p, mx, "  Mm free: ");
    p = x_cat_u(out, p, mx, Memory::FreeBytes() / (1024 * 1024));
    p = x_cat(out, p, mx, " MB\n  Se level: ");
    p = x_cat_u(out, p, mx, (uint64_t)Security::CurrentLevel());
    p = x_cat(out, p, mx, "\n  Io devices: ");
    p = x_cat_u(out, p, mx, (uint64_t)IRP::GetDeviceCount());
    p = x_cat(out, p, mx, "\n");
    return p;
}

}  // namespace KExec

// end (satoru)
