#ifndef KURONO_LD_KURONO_H
#define KURONO_LD_KURONO_H

// ld-kurono - production-grade dynamic linker for Kurono OS
// ============================================================
//
// Built directly into the kernel.  When `execve()` loads an ELF that
// declares PT_INTERP (typically /lib64/ld-linux-x86-64.so.2 or
// /lib/ld-linux.so.2), the loader hands control to ld-kurono instead of
// looking for an interpreter file on disk.  The path translation layer
// (linux_syscall.cpp::ResolvePath) silently rewrites those interpreter
// requests to /system/lib/ld-kurono.so for any user-space code that
// asks.  This module is the implementation behind that name.
//
// Capabilities
// ------------
//   * Full ELF64 PT_INTERP / PT_LOAD / PT_DYNAMIC / PT_TLS / PT_GNU_RELRO
//     handling for ET_DYN and ET_EXEC binaries.
//   * Per-segment ASLR with kernel-supplied entropy.
//   * RELRO enforcement via mprotect after relocations.
//   * Recursive DT_NEEDED resolution with circular-dep tracking and
//     SONAME-based deduplication.
//   * Search path: DT_RPATH/DT_RUNPATH > LD_LIBRARY_PATH > ld.so.cache >
//     /system/lib > /system/lib/kurono > /system/lib/x86_64-linux-gnu >
//     /apps/lib > /home/user/.local/lib.
//   * GNU_HASH primary symbol lookup, SYSV hash fallback, full
//     version (DT_VERSYM/VERDEF/VERNEED) resolution.
//   * STB_GLOBAL / STB_WEAK / STB_LOCAL binding semantics.
//   * STV_DEFAULT / STV_HIDDEN / STV_PROTECTED / STV_INTERNAL.
//   * Every x86_64 relocation type (see Reloc.x86_64.* below).
//   * PLT lazy binding via in-kernel resolver trampoline + GOT patching;
//     eager binding under DT_BIND_NOW or LD_BIND_NOW=1.
//   * IFUNC resolvers (R_X86_64_IRELATIVE).
//   * Static TLS allocation + __tls_get_addr + arch_prctl ARCH_SET_FS.
//   * dlopen/dlclose/dlsym/dlvsym/dladdr/dlerror with all RTLD_* flags
//     (LAZY/NOW/GLOBAL/LOCAL/NOLOAD/DEEPBIND/DEFAULT/NEXT).
//   * DT_INIT, DT_INIT_ARRAY, DT_FINI, DT_FINI_ARRAY in dependency
//     order; FINI registered with kurono atexit.
//   * Auxiliary vector population (AT_PHDR..AT_EXECFN..AT_SYSINFO_EHDR).
//   * vDSO mapped into every process at a fixed high address with fast
//     clock_gettime / gettimeofday / time / getcpu.
//   * r_debug rendezvous structure + DT_DEBUG patching for GDB.
//   * LD_DEBUG=all|libs|symbols|reloc|files|versions logging to
//     /system/log/ldso.log.
//   * LD_PRELOAD support; ignored for setuid/setgid binaries.
//   * Per-process loaded-library list mirrored into
//     /system/proc/<pid>/maps in real time.
//
// Public API
// ----------
//   * LdKurono::Init()              - boot-time global setup
//   * LdKurono::ExecPIE(...)        - entry from execve when PT_INTERP
//                                     is present in the main binary
//   * LdKurono::Dlopen(...)         - runtime library load
//   * LdKurono::Dlsym(...)          - symbol lookup
//   * LdKurono::Dlclose(...)        - refcount / unload
//   * LdKurono::Dladdr(...)         - reverse address lookup
//   * LdKurono::Dlerror()           - last error string
//
// All operations are kernel-side and address user processes through
// KernelVMM::MapPageInAddressSpace; user-mode trampolines for the lazy
// resolver are emitted into a per-process scratch page.

#include "../kernel/types.h"

struct Process;

namespace LdKurono {

    // ---- public flag bits used by Dlopen / search ------------------

    enum DlOpenFlags : uint32_t {
        RTLD_LAZY     = 0x00001,
        RTLD_NOW      = 0x00002,
        RTLD_NOLOAD   = 0x00004,
        RTLD_DEEPBIND = 0x00008,
        RTLD_GLOBAL   = 0x00100,
        RTLD_LOCAL    = 0x00000,
        RTLD_NODELETE = 0x01000,
    };

    // Pseudo-handles for Dlsym.
    inline void* const RTLD_DEFAULT = (void*)0;
    inline void* const RTLD_NEXT    = (void*)(intptr_t)-1;

    // ---- LD_DEBUG bit mask -----------------------------------------
    enum DebugChannel : uint32_t {
        DBG_FILES    = 1 << 0,
        DBG_LIBS     = 1 << 1,
        DBG_SYMBOLS  = 1 << 2,
        DBG_RELOC    = 1 << 3,
        DBG_VERSIONS = 1 << 4,
        DBG_BINDINGS = 1 << 5,
        DBG_STATS    = 1 << 6,
        DBG_ALL      = 0xFFFFFFFFu,
    };

    // ---- limits ----------------------------------------------------
    constexpr int LDK_MAX_LIBS_PER_PROC = 256;
    constexpr int LDK_MAX_LIBS_GLOBAL   = 1024;
    constexpr int LDK_MAX_DEPS          = 64;
    constexpr int LDK_MAX_SEARCHPATHS   = 32;
    constexpr int LDK_MAX_PRELOADS      = 16;
    constexpr int LDK_SONAME_LEN        = 96;
    constexpr int LDK_PATH_LEN          = 256;

    // ---- public init -----------------------------------------------
    void Init();

    // Map a vDSO page into a process address space.  Called by the
    // executor right after creating the user address space and before
    // pushing auxv onto the stack.  Returns the user-mode vDSO base.
    uint64_t MapVDSO(Process* proc);

    // Entry point used by execve when the main binary declares
    // PT_INTERP.  Loads the binary, the linker, all DT_NEEDED libraries,
    // applies relocations, builds the auxv on the stack, and returns
    // the entry point that should be jumped to (which is the linker's
    // _start, not the binary's).
    //
    //   image      - bytes of the main binary
    //   image_size - length
    //   path       - vfs path used for AT_EXECFN
    //   argv,envp  - NULL-terminated string vectors
    //   uid,gid    - owner ids for security checks
    //   out_entry  - on success, ELF entry point to jump to
    //   out_rsp    - on success, user RSP after auxv push
    //
    // Returns true on success, false on any fatal load error (the
    // process should be torn down).
    bool ExecPIE(Process* proc,
                 const uint8_t* image, uint64_t image_size,
                 const char* path,
                 const char* const* argv,
                 const char* const* envp,
                 uint32_t uid, uint32_t gid,
                 uint64_t* out_entry,
                 uint64_t* out_rsp);

    // ---- runtime API ----------------------------------------------
    // dlopen - returns an opaque handle (>0) or 0 on error.  Call
    // Dlerror() for details.
    void* Dlopen(Process* proc, const char* file, uint32_t flags);
    int   Dlclose(Process* proc, void* handle);

    // dlsym - find `name` in `handle` (or any global lib if handle ==
    // RTLD_DEFAULT).  Returns the resolved user-mode address or 0.
    uint64_t Dlsym (Process* proc, void* handle, const char* name);
    uint64_t Dlvsym(Process* proc, void* handle, const char* name,
                    const char* version);

    // dladdr - `addr` must be a user-mode address inside one of the
    // process's loaded libraries.  Fills out the dl_info-style fields.
    struct DlInfo {
        const char* dli_fname;   // pathname of object
        uint64_t    dli_fbase;   // base address
        const char* dli_sname;   // nearest symbol name (may be null)
        uint64_t    dli_saddr;   // exact address of dli_sname
    };
    bool Dladdr(Process* proc, uint64_t addr, DlInfo* out);

    // Last error string - thread-local in glibc, here per-process.
    const char* Dlerror(Process* proc);

    // Issue a debug notification that GDB sets a breakpoint on.  Called
    // by Dlopen / Dlclose so the debugger can rescan the loaded libs.
    void DlDebugStateNotify();

    // Returns true if the named library is already loaded into proc.
    bool IsLoaded(Process* proc, const char* soname);

    // Stats / diagnostics ------------------------------------------
    int  LoadedCount(Process* proc);
    void DumpMaps(Process* proc, char* out, int max_len);
}

#endif
