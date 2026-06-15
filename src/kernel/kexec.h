#pragma once
#include "types.h"

//  kurono os: kexec, the kernel executive (kurono's answer to the windows nt
//  executive  -  Ex/Mm/Ps/Io/Se/Cm).
//
//  the executive is the clean, named service layer the rest of the kernel + the
//  driver frameworks (kdf/udf) call into, instead of reaching into vmm/pmm/
//  scheduler/supr internals directly. it groups the core services into five
//  namespaces mirroring nt:
//
//    KExec::Memory   (Mm)   -  page + contiguous allocation, map/unmap, query
//    KExec::Process  (Ps)   -  current pid, spawn a kernel worker, yield/sleep
//    KExec::IO       (Io)   -  post an irp (routes to the IRP executive)
//    KExec::Security (Se)   -  capability / privilege checks (routes to SUPR)
//    KExec::Config   (Cm)   -  a tiny key/value config store (the "registry")
//
//  HONEST SCOPE: this is a facade. it does not replace the subsystems; it gives
//  drivers a stable, documented api surface and a single place capability checks
//  + accounting can hang off. the Config store is a small in-memory key/value
//  table persisted to /kurono/etc nowhere yet (documented). it is the structured
//  executive layer the hybrid-kernel design calls for, kept thin + real. (satoru)

namespace KExec {

// ── Mm: memory ────────────────────────────────────────────────────────────────
namespace Memory {
    void*    AllocPages(uint64_t pages);             // contiguous, identity-mapped (satoru)
    void     FreePages(void* p, uint64_t pages);
    uint64_t PhysOf(void* virt);                     // va -> phys (0 if unmapped) (satoru)
    bool     MapMMIO(uint64_t paddr, uint64_t bytes, uint64_t flags);
    uint64_t FreeBytes();                            // free physical memory (satoru)
}

// ── Ps: process ─────────────────────────────────────────────────────────────
namespace Process {
    uint32_t CurrentPid();
    // spawn a kernel worker thread (wraps the scheduler). returns true on success.
    typedef void (*Worker)();
    bool     SpawnWorker(const char* name, Worker fn, int stack_kb);
    void     Yield();
    void     SleepMs(uint32_t ms);
}

// ── Io: i/o (routes to the IRP executive) ─────────────────────────────────────
namespace IO {
    // synchronous block read/write through the IRP stack by device name. returns
    // bytes transferred, or <0 on error. (satoru)
    int64_t  ReadBlocks(const char* device, uint64_t lba, uint32_t count, void* buf);
    int64_t  WriteBlocks(const char* device, uint64_t lba, uint32_t count, const void* buf);
}

// ── Se: security (routes to SUPR) ─────────────────────────────────────────────
namespace Security {
    enum Cap { CAP_NETWORK = 1, CAP_FILESYSTEM = 2, CAP_GUI = 4, CAP_HARDWARE = 8 };
    // is the current session permitted `cap` right now? (satoru)
    bool     CheckCapability(uint32_t cap);
    // current privilege level (0=guest,1=user,2=admin/root) from SUPR. (satoru)
    int      CurrentLevel();
}

// ── Cm: config (the "registry") ───────────────────────────────────────────────
namespace Config {
    bool        SetString(const char* key, const char* value);
    const char* GetString(const char* key, const char* def);
    bool        SetU64(const char* key, uint64_t value);
    uint64_t    GetU64(const char* key, uint64_t def);
}

// ── lifecycle / status ─────────────────────────────────────────────────────────
void Init();
int  Status(char* out, int mx);

}  // namespace KExec

// end (satoru)
