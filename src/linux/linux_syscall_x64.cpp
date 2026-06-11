// ═══════════════════════════════════════════════════════════════════════════
//  x86_64 SYSCALL fast-path dispatcher (Linux ABI)
//
//  This file complements the existing int 0x80 / i386-numbered path.
//  The asm entry stub `syscall_entry_x64` (in src/hal/syscall_entry.asm)
//  switches stacks and calls `SyscallEntryX64Handler` defined here.
//
//  We translate the x86_64 Linux syscall number to the i386 number used
//  by `LinuxSyscall::Dispatch`, then route through the existing handlers.
//  Arg registers carry full 64-bit user pointers/addresses and are passed
//  through untruncated, so dynamic/PIE binaries that ld-kurono places
//  above 4GB reach the handlers with intact pointers.  The result is kept
//  64-bit so a >4GB mmap return reaches userspace in rax intact (satoru).
// ═══════════════════════════════════════════════════════════════════════════

#include "linux_syscall.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../kernel/userspace.h"

// Globals shared with syscall_entry.asm.
extern "C" {
    volatile uint64_t g_kernel_syscall_rsp        = 0;
    volatile uint64_t g_user_syscall_rsp_save     = 0;
    volatile uint64_t g_user_syscall_rip_save     = 0;
    volatile uint64_t g_user_syscall_rflags_save  = 0;
}

namespace {

// x86_64 → i386 syscall number translation.
// Only values we actively support are listed; anything else returns -ENOSYS.
struct NrMap { uint32_t x64; uint32_t i386; };

constexpr NrMap kNrMap[] = {
    {  0,   3 },  // read
    {  1,   4 },  // write
    {  2,   5 },  // open
    {  3,   6 },  // close
    // nr 5 (fstat) handled directly in SyscallEntryX64Handler  -  it must fill
    // the 64-bit struct stat, not the i386 layout the LSYS_FSTAT handler writes.
    {  8,  19 },  // lseek
    {  9,  90 },  // mmap (anon ok, file-backed not supported)
    { 10, 125 },  // mprotect (stubbed in i386 dispatch)
    { 11,  91 },  // munmap
    { 12,  45 },  // brk
    { 16,  54 },  // ioctl
    { 20, 146 },  // writev
    { 21,  33 },  // access
    { 32,  41 },  // dup
    { 33,  63 },  // dup2
    { 35, 162 },  // nanosleep
    { 39,  20 },  // getpid
    { 60,   1 },  // exit
    { 63, 122 },  // uname
    { 79, 183 },  // getcwd
    { 80,  12 },  // chdir
    { 83,  39 },  // mkdir
    { 84,  40 },  // rmdir
    { 87,  10 },  // unlink
    {102,  24 },  // getuid
    {104,  47 },  // getgid
    {107,  49 },  // geteuid
    {108,  50 },  // getegid
    {110,  64 },  // getppid
    {217, 220 },  // getdents64
    {228, 265 },  // clock_gettime
    {231, 252 },  // exit_group
    // ── broadened static-musl / ffmpeg syscall surface. these route to
    //    i386 dispatch handlers verified to exist; lsys_ macros guarantee the
    //    correct internal id even where x64 and i386 numbers collide. (satoru)
    // nr 4 (stat), 6 (lstat), 262 (newfstatat) handled directly in
    // SyscallEntryX64Handler so they fill the 64-bit struct stat layout.
    {   7, LSYS_POLL },          // poll
    {  17, LSYS_PREAD64 },       // pread64
    {  18, LSYS_PWRITE64 },      // pwrite64
    {  23, LSYS_SELECT },        // select
    {  26, LSYS_MSYNC },         // msync
    {  28, LSYS_MADVISE },       // madvise
    {  40, LSYS_SENDFILE },      // sendfile
    {  56, LSYS_CLONE },         // clone (thread flags fail soft -> single threaded)
    {  72, LSYS_FCNTL },         // fcntl
    {  74, LSYS_FSYNC },         // fsync
    {  75, LSYS_FDATASYNC },     // fdatasync
    {  77, LSYS_FTRUNCATE },     // ftruncate
    {  99, LSYS_SYSINFO },       // sysinfo
    { 186, LSYS_GETTID },        // gettid
    { 202, LSYS_FUTEX },         // futex (critical: musl locks/once/tls)
    { 229, LSYS_CLOCK_GETRES },  // clock_getres
    { 234, LSYS_TGKILL },        // tgkill
    { 332, LSYS_STATX },         // statx
};

constexpr int kNrMapCount = sizeof(kNrMap) / sizeof(kNrMap[0]);

// Stubs that just return success (0)  -  needed by musl/CPython startup
// but harmless if we no-op them.  Listed by x86_64 nr.
constexpr uint32_t kStubOk[] = {
    13,   // rt_sigaction
    14,   // rt_sigprocmask
    15,   // rt_sigreturn (no signals delivered, never really invoked) (satoru)
    24,   // sched_yield (single-threaded: yield is a no-op success) (satoru)
    73,   // flock (advisory lock pretend-success) (satoru)
    131,  // sigaltstack
    273,  // set_robust_list
    324,  // membarrier (single-threaded: no-op success) (satoru)
    334,  // rseq
    302,  // prlimit64  (we'll fail soft)
    157,  // prctl
};

constexpr int kStubOkCount = sizeof(kStubOk) / sizeof(kStubOk[0]);

bool find_stub_ok(uint64_t nr) {
    for (int i = 0; i < kStubOkCount; i++) {
        if (kStubOk[i] == (uint32_t)nr) return true;
    }
    return false;
}

bool translate_nr(uint64_t x64_nr, uint32_t* out_i386) {
    for (int i = 0; i < kNrMapCount; i++) {
        if (kNrMap[i].x64 == (uint32_t)x64_nr) {
            *out_i386 = kNrMap[i].i386;
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" int64_t SyscallEntryX64Handler(uint64_t nr,
                                          uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
    (void)a5;  // 6th arg unused for now

    // ── Direct x86_64 syscalls that need full 64-bit args or special
    //    handling and have no i386 equivalent we can route to. ──
    switch (nr) {
        case 158: {  // arch_prctl(int code, unsigned long addr)
            // Used by musl to install TLS via FS base.
            // ARCH_SET_FS=0x1002, ARCH_SET_GS=0x1001,
            // ARCH_GET_FS=0x1003, ARCH_GET_GS=0x1004.
            constexpr uint32_t MSR_FS_BASE = 0xC0000100;
            constexpr uint32_t MSR_GS_BASE = 0xC0000101;
            auto wrmsr = [](uint32_t msr, uint64_t v) {
                uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
                asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
            };
            auto rdmsr = [](uint32_t msr) -> uint64_t {
                uint32_t lo, hi;
                asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
                return ((uint64_t)hi << 32) | lo;
            };
            switch ((uint32_t)a0) {
                case 0x1002: wrmsr(MSR_FS_BASE, a1); return 0;
                case 0x1001: wrmsr(MSR_GS_BASE, a1); return 0;
                case 0x1003: {
                    uint64_t v = rdmsr(MSR_FS_BASE);
                    if (a1) *(uint64_t*)(uintptr_t)a1 = v;
                    return 0;
                }
                case 0x1004: {
                    uint64_t v = rdmsr(MSR_GS_BASE);
                    if (a1) *(uint64_t*)(uintptr_t)a1 = v;
                    return 0;
                }
            }
            return -22;  // -EINVAL
        }
        case 218: {  // set_tid_address(int* tidptr)
            if (a0) *(int*)(uintptr_t)a0 = 1;
            return 1;
        }
        case 318: {  // getrandom(void* buf, size_t buflen, uint flags)
            // Weak entropy via TSC; fine for hash-seed bootstrap.
            uint8_t* dst = (uint8_t*)(uintptr_t)a0;
            size_t   n   = (size_t)a1;
            if (!dst || n == 0) return 0;
            uint32_t lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t s = ((uint64_t)hi << 32) | lo;
            for (size_t i = 0; i < n; ++i) {
                s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                dst[i] = (uint8_t)(s >> 33);
            }
            return (int64_t)n;
        }
        case 89: {  // readlink  -  just fail; CPython tolerates ENOENT.
            return -2;
        }
        case 257: {  // openat: ignore dirfd if AT_FDCWD, route to open.
            if ((int)a0 == -100 /* AT_FDCWD */) {
                // pass the pathname pointer (a1) full-width  -  it may live
                // above 4gb in a pie process (satoru)
                return LinuxSyscall::Dispatch(5 /* open */,
                    a1, a2, a3, 0, 0);
            }
            return -38;
        }
        case 230: {  // clock_nanosleep(clockid, flags, req, rem)  -  route to
            // nanosleep(req, rem); clockid/flags ignored (satoru)
            return LinuxSyscall::Dispatch(LSYS_NANOSLEEP, a2, a3, 0, 0, 0);
        }
        // stat/fstat/lstat/newfstatat must NOT route through the i386 LSYS_*
        // handlers: those fill the 32-bit `struct LinuxStat`, but an x86_64
        // binary expects the 144-byte `struct LinuxStat64`.  Call the 64-bit
        // handlers directly so they format the correct layout (and validate the
        // user statbuf).  Resolution may touch ext4, so allow IRQs first to
        // match the translated path's HAL::EnableInterrupts(). (satoru)
        case 4: {    // stat(const char* path, struct stat* statbuf)
            HAL::EnableInterrupts();
            int64_t r = LinuxSyscall::sys_stat64(a0, a1);
            HAL::DisableInterrupts();
            return r;
        }
        case 5: {    // fstat(int fd, struct stat* statbuf)
            HAL::EnableInterrupts();
            int64_t r = LinuxSyscall::sys_fstat64((int)a0, a1);
            HAL::DisableInterrupts();
            return r;
        }
        case 6: {    // lstat(const char* path, struct stat* statbuf)
            // no symlinks in kvfs → identical to stat
            HAL::EnableInterrupts();
            int64_t r = LinuxSyscall::sys_stat64(a0, a1);
            HAL::DisableInterrupts();
            return r;
        }
        case 262: {  // newfstatat(int dirfd, const char* path,
                     //            struct stat* statbuf, int flags)
            HAL::EnableInterrupts();
            int64_t r = LinuxSyscall::sys_fstatat64((int)a0, a1, a2, (int)a3);
            HAL::DisableInterrupts();
            return r;
        }
        // statx (332) falls through to the kNrMap translation → LSYS_STATX.
        case 302: {  // prlimit64  -  fail soft.
            return -38;
        }
    }

    if (find_stub_ok(nr)) {
        return 0;
    }

    uint32_t i386_nr = 0;
    if (!translate_nr(nr, &i386_nr)) {
        SerialLogger::Log("[syscall_x64] unimplemented nr=");
        SerialLogger::LogDec((int)nr);
        SerialLogger::Log("\r\n");
        return -38;  // -ENOSYS
    }

    // SYSCALL enters with IF masked via SFMASK so the stub can safely switch
    // off the user stack. Once we're in C on the kernel stack, allow timer
    // IRQs and GUI pumping during longer syscall work.
    HAL::EnableInterrupts();
    // pass arg registers untruncated; a >4gb user pointer must survive, and
    // keep the result 64-bit so a high mmap address is not mangled (satoru)
    int64_t r = LinuxSyscall::Dispatch(
        i386_nr,
        a0,
        a1,
        a2,
        a3,
        a4
    );

    // exit / exit_group: never return to userspace.  Tear the
    // process down via Userspace::HandleProcessExit which longjmps
    // out of the enclosing Userspace::RunProcess call.  This mirrors
    // what the int 0x80 path does via resume_userspace_session.
    if (nr == 60 /* exit */ || nr == 231 /* exit_group */) {
        Userspace::HandleProcessExit((int)a0);
        // Unreachable, but be defensive.
        for (;;) {
            asm volatile("hlt");
        }
    }

    HAL::DisableInterrupts();
    return (int64_t)r;
}
