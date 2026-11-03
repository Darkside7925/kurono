//  uaccess implementation. See uaccess.h for why this exists and for the
//  mandatory ordering (primitives -> sweep -> SMAP).
//
//  end (satoru)

#include "uaccess.h"
#include "../drivers/serial.h"

namespace UAccess {

namespace {
Stats g_stats = {};

constexpr uint64_t PG = 4096ull;

// is one page of the CURRENT address space present (and writable if asked)?
// QueryMappingInAddressSpace returns 0 for a not-present leaf, which is exactly
// the "would fault" condition we need to reject. (satoru)
bool page_ok(uint64_t as, uint64_t va, bool need_write) {
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(as, va & ~(PG - 1));
    if (phys == 0) return false;
    if (need_write) {
        uint64_t fl = KernelVMM::QueryPageFlagsInAddressSpace(as, va & ~(PG - 1));
        // a CoW page is mapped read-only on purpose; writing it is legitimate and
        // the fault handler resolves it, so do NOT reject on !WRITABLE when the
        // CoW marker is set - that would break fork'd children. (satoru)
        if (!(fl & PTE_WRITABLE) && !(fl & PTE_COW)) return false;
    }
    return true;
}

void note_range_reject(uint64_t p, uint64_t l) {
    g_stats.range_rejects++;
    g_stats.last_bad_ptr = p;
    g_stats.last_bad_len = l;
}

void note_unmapped_reject(uint64_t p, uint64_t l) {
    g_stats.unmapped_rejects++;
    g_stats.last_bad_ptr = p;
    g_stats.last_bad_len = l;
}
}  // namespace

bool RangeOk(uint64_t ptr, uint64_t len, bool need_write) {
    if (!AccessOk(ptr, len)) { note_range_reject(ptr, len); return false; }
    if (len == 0) return true;
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    if (as == 0) return false;
    for (uint64_t va = ptr & ~(PG - 1); va < ptr + len; va += PG) {
        if (!page_ok(as, va, need_write)) { note_unmapped_reject(ptr, len); return false; }
    }
    return true;
}

uint64_t CopyFromUser(void* dst, uint64_t user_src, uint64_t len) {
    if (!AccessOk(user_src, len)) { note_range_reject(user_src, len); return len; }
    if (len == 0) return 0;
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    if (as == 0) return len;

    uint8_t* d = (uint8_t*)dst;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va = user_src + done;
        if (!page_ok(as, va, false)) { note_unmapped_reject(va, len - done); break; }
        // copy up to the end of this page, so a partially-mapped range copies
        // what it can and reports the remainder - matching linux. (satoru)
        uint64_t page_end = (va & ~(PG - 1)) + PG;
        uint64_t chunk = page_end - va;
        if (chunk > len - done) chunk = len - done;
        const uint8_t* s = (const uint8_t*)(uintptr_t)va;
        for (uint64_t i = 0; i < chunk; i++) d[done + i] = s[i];
        done += chunk;
    }
    return len - done;
}

uint64_t CopyToUser(uint64_t user_dst, const void* src, uint64_t len) {
    if (!AccessOk(user_dst, len)) { note_range_reject(user_dst, len); return len; }
    if (len == 0) return 0;
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    if (as == 0) return len;

    const uint8_t* s = (const uint8_t*)src;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va = user_dst + done;
        if (!page_ok(as, va, true)) { note_unmapped_reject(va, len - done); break; }
        uint64_t page_end = (va & ~(PG - 1)) + PG;
        uint64_t chunk = page_end - va;
        if (chunk > len - done) chunk = len - done;
        uint8_t* d = (uint8_t*)(uintptr_t)va;
        for (uint64_t i = 0; i < chunk; i++) d[i] = s[done + i];
        done += chunk;
    }
    return len - done;
}

int64_t StrNCopyFromUser(char* dst, uint64_t user_src, uint64_t max) {
    if (max == 0) return -14;                      // -EFAULT: nowhere to put a NUL
    if (!AccessOk(user_src, 1)) { note_range_reject(user_src, 1); return -14; }
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    if (as == 0) return -14;

    uint64_t i = 0;
    while (i < max - 1) {
        uint64_t va = user_src + i;
        if (va > USER_SPACE_TOP) { note_range_reject(va, 1); dst[i] = 0; return -14; }
        if (!page_ok(as, va, false)) { note_unmapped_reject(va, 1); dst[i] = 0; return -14; }
        char c = *(const char*)(uintptr_t)va;
        dst[i] = c;
        if (c == 0) return (int64_t)i;
        i++;
    }
    dst[max - 1] = 0;                              // always terminated (satoru)
    return (int64_t)(max - 1);
}

const Stats& GetStats() { return g_stats; }

void LogStats() {
    SerialLogger::Log("[uaccess] range_rejects=");
    SerialLogger::LogDec((int)g_stats.range_rejects);
    SerialLogger::Log(" unmapped_rejects=");
    SerialLogger::LogDec((int)g_stats.unmapped_rejects);
    if (g_stats.range_rejects || g_stats.unmapped_rejects) {
        SerialLogger::Log(" last_bad_ptr=");
        SerialLogger::LogHex64(g_stats.last_bad_ptr);
    }
    SerialLogger::Log("\r\n");
}

}  // namespace UAccess

// end (satoru)
