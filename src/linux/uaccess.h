#pragma once
#include "../kernel/types.h"
#include "../kernel/vmm.h"

//  uaccess - validated user-pointer access for the syscall layer.
//
//  WHY THIS EXISTS (the audit's #1 finding): the syscall layer had NO
//  copy_from_user / copy_to_user / access_ok anywhere. Every handler
//  dereferenced user pointers directly, which means any ring-3 process could
//  pass a KERNEL address into a syscall and have the kernel read or write it on
//  its behalf. That is a straightforward privilege-escalation and info-leak
//  surface, and it is also exactly why SMAP is switched off in
//  kurono_kernel.cpp - SMAP would #PF on the first such access.
//
//  The order of operations matters and is deliberate:
//    1. add these primitives                     <- this file
//    2. route user-pointer accesses through them (incremental, per syscall)
//    3. ONLY THEN enable CR4.SMAP
//  Enabling SMAP before (2) is complete turns every un-migrated access into an
//  instant fault, so SMAP stays off until the sweep is done. Do not shortcut it.
//
//  -- MIGRATION STATE (keep this list honest; it is the phase-0 tracker) ------
//  MIGRATED (validated):
//    sys_read           AccessOk gate  (bulk path, range check only by design)
//    sys_write          AccessOk gate  (info-leak direction)
//    sys_uname          CopyToUser     (was: memset through a raw user pointer)
//    sys_getcwd         CopyToUser     (was: unbounded ls_scpy to user pointer)
//    sys_clock_gettime  RangeOk        (had a MAPPING check but no RANGE check,
//                                       and kernel addresses ARE mapped - so a
//                                       kernel pointer passed and got written)
//    sys_stat/sys_fstat AccessOk gate  (had NO check at all)
//    sys_nanosleep      AccessOk on req (was only NULL-checked)
//    sys_readv/writev   AccessOk on the iovec ARRAY + UIO_MAXIOV cap. Each
//                       iov_base was already covered because every vector
//                       routes through sys_read/sys_write, but the ARRAY read
//                       was unchecked - a kernel `iov` made the kernel read its
//                       own memory as iovecs and use those as ptr/len.
//    stat64 family      AccessOk gate inside stat_buf_writable - that helper
//                       checked the region table but FELL BACK to "are the pages
//                       mapped", which accepts a kernel address. Same flaw class
//                       as clock_gettime. One fix covers all 4 stat64 syscalls.
//
//  NOT YET MIGRATED - known unvalidated user-pointer sites, in rough risk order.
//  Every one of these is a live instance of the same hole:
//    sys_getdents64              dirent stream write
//    sys_ioctl                   per-request argp, many shapes
//    path arguments generally    (const char*)pathname reads - StrNCopyFromUser
//                                exists for these and nothing uses it yet
//
//  CR4.SMAP STAYS OFF until that list is empty. Turning it on sooner converts
//  every un-migrated access into an instant fault. The ordering is not optional.
//
//  end (satoru)

namespace UAccess {

// ── the cheap mandatory gate ────────────────────────────────────────────────
// Is [ptr, ptr+len) entirely inside the canonical lower-half user range, with no
// wraparound? This is what stops a user handing the kernel 0xFFFF'FFFF'8000'0000
// and having it dereferenced. It does NOT prove the range is mapped - see
// RangeOk for that - but it is the check that closes the privilege boundary, so
// it is the one that must never be skipped. (satoru)
static inline bool AccessOk(uint64_t ptr, uint64_t len) {
    if (len == 0) return true;                 // an empty range is vacuously fine
    if (ptr > USER_SPACE_TOP) return false;    // kernel or non-canonical
    if (len > TASK_SIZE) return false;         // absurd length
    uint64_t end = ptr + len;                  // cannot overflow: both bounded above
    if (end < ptr) return false;               // wrap
    if (end > TASK_SIZE) return false;         // straddles into the kernel half
    return true;
}

// convenience for a typed user pointer. (satoru)
template <typename T>
static inline bool AccessOkT(uint64_t ptr) { return AccessOk(ptr, sizeof(T)); }

// ── mapping validation ──────────────────────────────────────────────────────
// AccessOk is necessary but not sufficient: an in-range but UNMAPPED user
// address dereferenced from kernel context faults in the kernel, which on this
// kernel means a panic rather than a clean -EFAULT. RangeOk walks the range
// page-by-page in the CURRENT address space and reports whether every page is
// present (and writable, if asked). Costs one page-table walk per page, so use
// it for control-structure reads, not for bulk data paths where the copy helpers
// below already do it. (satoru)
bool RangeOk(uint64_t ptr, uint64_t len, bool need_write);

// an OUTPUT buffer the kernel will write for the caller (review g1): the
// AccessOk gate for everyone, plus, for a kld container only, every page
// present and writable in its own page tables. a container's read-only broker
// pages (a bar under kurono.kld.bar=ro|window, the copy-protected pages) can
// then not be written by the kernel on its behalf. ordinary processes pay one
// container-table lookup. (satoru)
bool OutOk(uint64_t ptr, uint64_t len);

// ── the copy helpers ────────────────────────────────────────────────────────
// Linux convention: return the number of bytes NOT copied (0 == full success),
// so callers can do `if (copy_from_user(...)) return -EFAULT;`. Both validate
// with AccessOk first and then per-page as they go, so a range that becomes
// unmapped midway stops cleanly instead of faulting. (satoru)
uint64_t CopyFromUser(void* dst, uint64_t user_src, uint64_t len);
uint64_t CopyToUser(uint64_t user_dst, const void* src, uint64_t len);

// bounded string copy from user. returns the length copied (excluding NUL), or
// negative on fault. Never reads past `max` and always NUL-terminates, so a
// user string with no terminator cannot walk the kernel off the end. (satoru)
int64_t StrNCopyFromUser(char* dst, uint64_t user_src, uint64_t max);

// diagnostics: how many rejections have happened, and the last bad pointer.
// A nonzero reject count on a normal boot means something is passing bad
// pointers and is worth investigating rather than ignoring. (satoru)
struct Stats {
    uint64_t range_rejects;    // AccessOk said no (the privilege boundary) (satoru)
    uint64_t unmapped_rejects; // in range but not mapped (satoru)
    uint64_t last_bad_ptr;
    uint64_t last_bad_len;
};
const Stats& GetStats();
void LogStats();

}  // namespace UAccess

// end (satoru)
