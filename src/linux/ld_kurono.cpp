// ld-kurono.cpp  -  production dynamic linker (see ld_kurono.h).
//
// This module is intentionally self-contained except for KVFS, PMM/VMM
// and the scheduler.  It does NOT depend on the userspace_entry asm  - 
// the lazy-binding trampoline is emitted as raw machine code into a
// per-process scratch page.

#include "ld_kurono.h"

#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../kernel/time.h"

namespace {

// ============================================================
// ELF64 structure definitions (subset we actually consume).
// ============================================================

#pragma pack(push, 1)
struct Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};
struct Dyn {
    int64_t  d_tag;
    uint64_t d_un;
};
struct Sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};
struct Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};
struct Verdef {
    uint16_t vd_version;
    uint16_t vd_flags;
    uint16_t vd_ndx;
    uint16_t vd_cnt;
    uint32_t vd_hash;
    uint32_t vd_aux;
    uint32_t vd_next;
};
struct Verdaux {
    uint32_t vda_name;
    uint32_t vda_next;
};
struct Verneed {
    uint16_t vn_version;
    uint16_t vn_cnt;
    uint32_t vn_file;
    uint32_t vn_aux;
    uint32_t vn_next;
};
struct Vernaux {
    uint32_t vna_hash;
    uint16_t vna_flags;
    uint16_t vna_other;
    uint32_t vna_name;
    uint32_t vna_next;
};
#pragma pack(pop)

// ELF / ABI constants we use ----------------------------------------
constexpr uint32_t PT_LOAD       = 1;
constexpr uint32_t PT_DYNAMIC    = 2;
constexpr uint32_t PT_INTERP     = 3;
constexpr uint32_t PT_NOTE       = 4;
constexpr uint32_t PT_PHDR       = 6;
constexpr uint32_t PT_TLS        = 7;
constexpr uint32_t PT_GNU_RELRO  = 0x6474e552;
constexpr uint32_t PT_GNU_STACK  = 0x6474e551;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN  = 3;
constexpr uint16_t EM_X86_64 = 62;

constexpr int64_t DT_NULL       = 0;
constexpr int64_t DT_NEEDED     = 1;
constexpr int64_t DT_PLTRELSZ   = 2;
constexpr int64_t DT_PLTGOT     = 3;
constexpr int64_t DT_HASH       = 4;
constexpr int64_t DT_STRTAB     = 5;
constexpr int64_t DT_SYMTAB     = 6;
constexpr int64_t DT_RELA       = 7;
constexpr int64_t DT_RELASZ     = 8;
constexpr int64_t DT_RELAENT    = 9;
constexpr int64_t DT_STRSZ      = 10;
constexpr int64_t DT_SYMENT     = 11;
constexpr int64_t DT_INIT       = 12;
constexpr int64_t DT_FINI       = 13;
constexpr int64_t DT_SONAME     = 14;
constexpr int64_t DT_RPATH      = 15;
constexpr int64_t DT_SYMBOLIC   = 16;
constexpr int64_t DT_REL        = 17;
constexpr int64_t DT_RELSZ      = 18;
constexpr int64_t DT_RELENT     = 19;
constexpr int64_t DT_PLTREL     = 20;
constexpr int64_t DT_DEBUG      = 21;
constexpr int64_t DT_TEXTREL    = 22;
constexpr int64_t DT_JMPREL     = 23;
constexpr int64_t DT_BIND_NOW   = 24;
constexpr int64_t DT_INIT_ARRAY = 25;
constexpr int64_t DT_FINI_ARRAY = 26;
constexpr int64_t DT_INIT_ARRAYSZ = 27;
constexpr int64_t DT_FINI_ARRAYSZ = 28;
constexpr int64_t DT_RUNPATH    = 29;
constexpr int64_t DT_FLAGS      = 30;
constexpr int64_t DT_GNU_HASH   = 0x6FFFFEF5;
constexpr int64_t DT_VERSYM     = 0x6FFFFFF0;
constexpr int64_t DT_VERDEF     = 0x6FFFFFFC;
constexpr int64_t DT_VERDEFNUM  = 0x6FFFFFFD;
constexpr int64_t DT_VERNEED    = 0x6FFFFFFE;
constexpr int64_t DT_VERNEEDNUM = 0x6FFFFFFF;
constexpr int64_t DT_RELACOUNT  = 0x6FFFFFF9;
constexpr int64_t DT_FLAGS_1    = 0x6FFFFFFB;
constexpr uint64_t DF_BIND_NOW  = 0x8;
constexpr uint64_t DF_1_NOW     = 0x1;
constexpr uint64_t DF_1_PIE     = 0x08000000;

// Relocation types  -  full x86_64 set ------------------------------
constexpr uint32_t R_X86_64_NONE        = 0;
constexpr uint32_t R_X86_64_64          = 1;
constexpr uint32_t R_X86_64_PC32        = 2;
constexpr uint32_t R_X86_64_GOT32       = 3;
constexpr uint32_t R_X86_64_PLT32       = 4;
constexpr uint32_t R_X86_64_COPY        = 5;
constexpr uint32_t R_X86_64_GLOB_DAT    = 6;
constexpr uint32_t R_X86_64_JUMP_SLOT   = 7;
constexpr uint32_t R_X86_64_RELATIVE    = 8;
constexpr uint32_t R_X86_64_GOTPCREL    = 9;
constexpr uint32_t R_X86_64_32          = 10;
constexpr uint32_t R_X86_64_32S         = 11;
constexpr uint32_t R_X86_64_PC64        = 24;
constexpr uint32_t R_X86_64_GOTOFF64    = 25;
constexpr uint32_t R_X86_64_GOTPC32     = 26;
constexpr uint32_t R_X86_64_TLSGD       = 19;
constexpr uint32_t R_X86_64_TLSLD       = 20;
constexpr uint32_t R_X86_64_DTPOFF32    = 21;
constexpr uint32_t R_X86_64_GOTTPOFF    = 22;
constexpr uint32_t R_X86_64_TPOFF32     = 23;
constexpr uint32_t R_X86_64_TPOFF64     = 18;
constexpr uint32_t R_X86_64_DTPMOD64    = 16;
constexpr uint32_t R_X86_64_DTPOFF64    = 17;
constexpr uint32_t R_X86_64_TLSDESC     = 36;
constexpr uint32_t R_X86_64_IRELATIVE   = 37;
constexpr uint32_t R_X86_64_GOTPCRELX   = 41;
constexpr uint32_t R_X86_64_REX_GOTPCRELX = 42;

inline uint32_t R_TYPE(uint64_t i) { return (uint32_t)(i & 0xFFFFFFFFu); }
inline uint32_t R_SYM (uint64_t i) { return (uint32_t)(i >> 32); }

// Symbol info macros --------------------------------------------------
inline uint8_t ST_BIND(uint8_t i) { return i >> 4; }
inline uint8_t ST_TYPE(uint8_t i) { return i & 0xF; }
constexpr uint8_t STB_LOCAL  = 0;
constexpr uint8_t STB_GLOBAL = 1;
constexpr uint8_t STB_WEAK   = 2;
constexpr uint8_t STT_NOTYPE = 0;
constexpr uint8_t STT_OBJECT = 1;
constexpr uint8_t STT_FUNC   = 2;
constexpr uint8_t STT_TLS    = 6;
constexpr uint8_t STT_GNU_IFUNC = 10;
inline uint8_t ST_VISIBILITY(uint8_t o) { return o & 0x3; }
constexpr uint8_t STV_DEFAULT   = 0;
constexpr uint8_t STV_INTERNAL  = 1;
constexpr uint8_t STV_HIDDEN    = 2;
constexpr uint8_t STV_PROTECTED = 3;

// VMM helpers --------------------------------------------------------
constexpr uint32_t PF_X = 1;
constexpr uint32_t PF_W = 2;
constexpr uint32_t PF_R = 4;

// ============================================================
// Library descriptor + per-process linker state.
// ============================================================

struct LibVersion {
    char     name[32];
    uint32_t hash;
    uint16_t ndx;
};

struct Lib {
    bool      in_use;
    bool      relocated;
    bool      init_called;
    int       refcount;

    char      soname[LdKurono::LDK_SONAME_LEN];
    char      path  [LdKurono::LDK_PATH_LEN];

    uint64_t  load_base;        // user-mode base address
    uint64_t  load_size;        // total size in bytes
    uint64_t  entry;            // for the linker / main exec only
    uint64_t  phdr_va;          // user-mode address of phdrs
    uint16_t  phnum;
    uint16_t  phentsize;

    // Pointers into the kernel-side scratch image (we keep the original
    // file in heap memory for the lifetime of the lib; cheap because
    // libraries are <16 MB each).
    uint8_t*  image;
    uint64_t  image_size;
    Phdr*     phdrs;

    // Dynamic info (kernel pointers into the in-image .dynamic).
    Dyn*      dyn;
    int       dyn_count;

    // Tables.
    const char* strtab;
    Sym*        symtab;
    int         symcount;

    uint32_t*   sysv_hash;      // [nbuckets, nchains, buckets..., chains...]
    uint32_t*   gnu_hash;

    Rela*       rela;
    int         rela_count;
    Rela*       jmprel;
    int         jmprel_count;

    int64_t     reloc_count;    // RELACOUNT optimisation hint
    bool        bind_now;

    uint64_t    init_func;
    uint64_t    fini_func;
    uint64_t*   init_array;
    int         init_array_n;
    uint64_t*   fini_array;
    int         fini_array_n;

    uint64_t    relro_va;
    uint64_t    relro_sz;

    // TLS template.
    uint8_t*    tls_template;
    uint64_t    tls_filesz;
    uint64_t    tls_memsz;
    uint64_t    tls_align;
    uint64_t    tls_offset;     // negative offset from FS base (variant 2)
    uint32_t    tls_modid;

    // Versioning.
    uint16_t*   versym;
    Verdef*     verdef;
    int         verdef_num;
    Verneed*    verneed;
    int         verneed_num;

    // Dependency graph.
    int         deps[LdKurono::LDK_MAX_DEPS];
    int         dep_count;

    bool        global;         // RTLD_GLOBAL
    bool        deepbind;
    bool        nodelete;
    bool        is_main_exec;
    bool        is_linker;
    bool        is_vdso;
};

struct ProcLinkerState {
    bool      in_use;
    Process*  proc;

    int       libs[LdKurono::LDK_MAX_LIBS_PER_PROC];   // indices into g_libs
    int       lib_count;

    // Search paths after merging RPATH/LD_LIBRARY_PATH/cache.
    char      searchpaths[LdKurono::LDK_MAX_SEARCHPATHS][LdKurono::LDK_PATH_LEN];
    int       searchpath_count;

    char      preload[LdKurono::LDK_MAX_PRELOADS][LdKurono::LDK_PATH_LEN];
    int       preload_count;

    uint32_t  debug_mask;       // LD_DEBUG
    bool      verbose;          // LD_VERBOSE
    bool      bind_now;         // LD_BIND_NOW or DF_BIND_NOW
    bool      secure;           // setuid/setgid

    char      last_error[256];

    // TLS.
    uint64_t  tls_block_va;     // user-mode TLS block base
    uint64_t  tls_block_size;
    uint32_t  next_tls_modid;

    // r_debug rendezvous (kernel-side mirror; copied to user via DT_DEBUG).
    uint64_t  r_debug_user_va;

    // vDSO.
    uint64_t  vdso_va;
};

// ============================================================
// Globals
// ============================================================
constexpr int MAX_PROCS = 64;
Lib              g_libs [LdKurono::LDK_MAX_LIBS_GLOBAL];
ProcLinkerState  g_procs[MAX_PROCS];

uint64_t         g_aslr_state = 0xC0FFEE5A4B3C2D1EULL;

// PRNG seeded from RDTSC + initial state ---------------------------
uint64_t aslr_rand() {
    uint64_t x;
    __asm__ volatile("rdtsc" : "=A"(x));
    g_aslr_state ^= x;
    g_aslr_state ^= g_aslr_state << 13;
    g_aslr_state ^= g_aslr_state >> 7;
    g_aslr_state ^= g_aslr_state << 17;
    return g_aslr_state;
}

// ASLR base in the canonical lower-half user range
// 0x0000_4000_0000_0000 .. 0x0000_7F00_0000_0000 (~64TB..~127TB).  placing
// pie images this high exercises the 64-bit syscall abi end to end; the
// arena stays below the 0x0000_8000_0000_0000 non-canonical boundary so
// every base+size remains a valid sign-extended user address (satoru)
constexpr uint64_t ASLR_BASE_LO = 0x0000400000000000ULL;
constexpr uint64_t ASLR_BASE_HI = 0x00007F0000000000ULL;
uint64_t pick_aslr_base(uint64_t size) {
    uint64_t span = (ASLR_BASE_HI - ASLR_BASE_LO) - ((size + 0xFFFFF) & ~0xFFFFFULL);
    uint64_t off  = (aslr_rand() % span) & ~0xFFFFFULL;
    return ASLR_BASE_LO + off;
}

// String helpers ---------------------------------------------------
int  s_len(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
bool s_eq (const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
void s_cpy(char* d, const char* s, int max) {
    int i; for (i = 0; i < max - 1 && s && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
bool s_starts(const char* a, const char* p) {
    while (*p) { if (*a++ != *p++) return false; }
    return true;
}

// ============================================================
// Logging.  Writes to /system/log/ldso.log when LD_DEBUG is on.
// ============================================================
ProcLinkerState* find_pls_by_proc(Process* p) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].in_use && g_procs[i].proc == p) return &g_procs[i];
    return nullptr;
}
ProcLinkerState* alloc_pls(Process* p) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (!g_procs[i].in_use) {
            g_procs[i] = ProcLinkerState{};
            g_procs[i].in_use = true;
            g_procs[i].proc = p;
            return &g_procs[i];
        }
    return nullptr;
}

void dlog(ProcLinkerState* pls, uint32_t channel, const char* msg) {
    if (!pls || !(pls->debug_mask & channel)) return;
    SerialLogger::Log("[ldso] ");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
    // Append to /system/log/ldso.log (truncate-safe).
    static char buf[16384];
    int n = KVFS::ReadFile("/system/log/ldso.log",
                           (uint8_t*)buf, sizeof(buf) - 256);
    if (n < 0) n = 0;
    int avail = (int)sizeof(buf) - n - 256;
    int ml = s_len(msg);
    if (ml > avail) ml = avail;
    if (ml > 0) {
        for (int i = 0; i < ml; i++) buf[n + i] = msg[i];
        buf[n + ml] = '\n';
        KVFS::WriteFile("/system/log/ldso.log",
                        (uint8_t*)buf, n + ml + 1);
    }
}

void set_error(ProcLinkerState* pls, const char* msg) {
    if (!pls) return;
    s_cpy(pls->last_error, msg, sizeof(pls->last_error));
    SerialLogger::Log("[ldso] error: ");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
}

// ============================================================
// Library table accessors.
// ============================================================
Lib* alloc_lib() {
    for (int i = 0; i < LdKurono::LDK_MAX_LIBS_GLOBAL; i++) {
        if (!g_libs[i].in_use) {
            g_libs[i] = Lib{};
            g_libs[i].in_use = true;
            g_libs[i].refcount = 1;
            return &g_libs[i];
        }
    }
    return nullptr;
}
int lib_index(Lib* l) {
    if (!l) return -1;
    return (int)(l - g_libs);
}
Lib* lib_by_soname_proc(ProcLinkerState* pls, const char* soname) {
    if (!pls || !soname) return nullptr;
    for (int i = 0; i < pls->lib_count; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        if (s_eq(l->soname, soname)) return l;
    }
    return nullptr;
}

// ============================================================
// File search.
// ============================================================
bool file_exists(const char* p) {
    KVFSNode* n = KVFS::Resolve(p);
    return n && n->type == KVFS_FILE;
}

bool resolve_lib_path(ProcLinkerState* pls, const char* name,
                      char* out, int max_len) {
    // Absolute path?
    if (name[0] == '/') {
        if (file_exists(name)) { s_cpy(out, name, max_len); return true; }
        return false;
    }
    // Search in pls->searchpaths.
    for (int i = 0; i < pls->searchpath_count; i++) {
        char tmp[LdKurono::LDK_PATH_LEN];
        int p = 0;
        for (int k = 0; pls->searchpaths[i][k] && p < max_len - 1; k++)
            tmp[p++] = pls->searchpaths[i][k];
        if (p && tmp[p - 1] != '/') tmp[p++] = '/';
        for (int k = 0; name[k] && p < max_len - 1; k++) tmp[p++] = name[k];
        tmp[p] = 0;
        if (file_exists(tmp)) { s_cpy(out, tmp, max_len); return true; }
    }
    return false;
}

// ============================================================
// ELF parsing helpers.
// ============================================================
bool valid_elf64(const uint8_t* d, uint64_t sz) {
    if (sz < sizeof(Ehdr)) return false;
    if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') return false;
    if (d[4] != 2 || d[5] != 1) return false;
    const Ehdr* e = (const Ehdr*)d;
    if (e->e_machine != EM_X86_64) return false;
    if (e->e_phentsize != sizeof(Phdr)) return false;
    if (e->e_phoff + (uint64_t)e->e_phnum * e->e_phentsize > sz) return false;
    return true;
}

uint8_t* vaddr_to_image(Lib* l, uint64_t va) {
    for (int i = 0; i < l->phnum; i++) {
        const Phdr& p = l->phdrs[i];
        if (p.p_type != PT_LOAD) continue;
        if (va >= p.p_vaddr && va < p.p_vaddr + p.p_filesz)
            return l->image + p.p_offset + (va - p.p_vaddr);
    }
    return nullptr;
}

void parse_dynamic(Lib* l) {
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type == PT_DYNAMIC) {
            l->dyn = (Dyn*)(l->image + l->phdrs[i].p_offset);
            l->dyn_count = (int)(l->phdrs[i].p_filesz / sizeof(Dyn));
            break;
        }
    }
    if (!l->dyn) return;

    uint64_t strtab_va = 0, symtab_va = 0;
    uint64_t hash_va = 0, gnu_hash_va = 0;
    uint64_t rela_va = 0, jmprel_va = 0, versym_va = 0;
    uint64_t verdef_va = 0, verneed_va = 0;
    uint64_t init_arr_va = 0, fini_arr_va = 0;
    uint64_t init_arr_sz = 0, fini_arr_sz = 0;
    uint64_t rela_sz = 0, jmprel_sz = 0;

    for (int i = 0; i < l->dyn_count && l->dyn[i].d_tag != DT_NULL; i++) {
        switch (l->dyn[i].d_tag) {
            case DT_STRTAB:    strtab_va = l->dyn[i].d_un; break;
            case DT_SYMTAB:    symtab_va = l->dyn[i].d_un; break;
            case DT_HASH:      hash_va   = l->dyn[i].d_un; break;
            case DT_GNU_HASH:  gnu_hash_va = l->dyn[i].d_un; break;
            case DT_RELA:      rela_va   = l->dyn[i].d_un; break;
            case DT_RELASZ:    rela_sz   = l->dyn[i].d_un; break;
            case DT_JMPREL:    jmprel_va = l->dyn[i].d_un; break;
            case DT_PLTRELSZ:  jmprel_sz = l->dyn[i].d_un; break;
            case DT_INIT:      l->init_func = l->dyn[i].d_un; break;
            case DT_FINI:      l->fini_func = l->dyn[i].d_un; break;
            case DT_INIT_ARRAY:   init_arr_va = l->dyn[i].d_un; break;
            case DT_FINI_ARRAY:   fini_arr_va = l->dyn[i].d_un; break;
            case DT_INIT_ARRAYSZ: init_arr_sz = l->dyn[i].d_un; break;
            case DT_FINI_ARRAYSZ: fini_arr_sz = l->dyn[i].d_un; break;
            case DT_VERSYM:    versym_va = l->dyn[i].d_un; break;
            case DT_VERDEF:    verdef_va = l->dyn[i].d_un; break;
            case DT_VERDEFNUM: l->verdef_num = (int)l->dyn[i].d_un; break;
            case DT_VERNEED:   verneed_va = l->dyn[i].d_un; break;
            case DT_VERNEEDNUM:l->verneed_num = (int)l->dyn[i].d_un; break;
            case DT_RELACOUNT: l->reloc_count = (int64_t)l->dyn[i].d_un; break;
            case DT_BIND_NOW:  l->bind_now = true; break;
            case DT_FLAGS:     if (l->dyn[i].d_un & DF_BIND_NOW) l->bind_now = true; break;
            case DT_FLAGS_1:   if (l->dyn[i].d_un & DF_1_NOW)    l->bind_now = true; break;
            default: break;
        }
    }

    if (strtab_va) l->strtab = (const char*)vaddr_to_image(l, strtab_va);
    if (symtab_va) l->symtab = (Sym*)vaddr_to_image(l, symtab_va);
    if (hash_va)   l->sysv_hash = (uint32_t*)vaddr_to_image(l, hash_va);
    if (gnu_hash_va) l->gnu_hash = (uint32_t*)vaddr_to_image(l, gnu_hash_va);
    if (rela_va) {
        l->rela = (Rela*)vaddr_to_image(l, rela_va);
        l->rela_count = (int)(rela_sz / sizeof(Rela));
    }
    if (jmprel_va) {
        l->jmprel = (Rela*)vaddr_to_image(l, jmprel_va);
        l->jmprel_count = (int)(jmprel_sz / sizeof(Rela));
    }
    if (versym_va) l->versym = (uint16_t*)vaddr_to_image(l, versym_va);
    if (verdef_va) l->verdef = (Verdef*)vaddr_to_image(l, verdef_va);
    if (verneed_va)l->verneed= (Verneed*)vaddr_to_image(l, verneed_va);
    if (init_arr_va) {
        l->init_array = (uint64_t*)vaddr_to_image(l, init_arr_va);
        l->init_array_n = (int)(init_arr_sz / 8);
    }
    if (fini_arr_va) {
        l->fini_array = (uint64_t*)vaddr_to_image(l, fini_arr_va);
        l->fini_array_n = (int)(fini_arr_sz / 8);
    }

    // SONAME extraction.
    if (!l->soname[0] && l->strtab) {
        for (int i = 0; i < l->dyn_count && l->dyn[i].d_tag != DT_NULL; i++) {
            if (l->dyn[i].d_tag == DT_SONAME) {
                s_cpy(l->soname, l->strtab + l->dyn[i].d_un,
                      sizeof(l->soname));
                break;
            }
        }
    }

    // Symbol count: GNU_HASH gives the largest live index; SYSV gives nchain.
    if (l->sysv_hash && l->symcount == 0) {
        l->symcount = (int)l->sysv_hash[1];
    }
    if (l->gnu_hash && l->symcount == 0) {
        // walk buckets to find max chain index
        uint32_t nbuckets = l->gnu_hash[0];
        uint32_t symoff   = l->gnu_hash[1];
        uint32_t bloom_sz = l->gnu_hash[2];
        uint32_t* buckets = l->gnu_hash + 4 + bloom_sz * 2;
        uint32_t* chain   = buckets + nbuckets;
        uint32_t max = symoff;
        for (uint32_t b = 0; b < nbuckets; b++) {
            uint32_t idx = buckets[b];
            if (idx >= symoff) {
                while (true) {
                    if (idx > max) max = idx;
                    if (chain[idx - symoff] & 1) break;
                    idx++;
                }
            }
        }
        l->symcount = (int)max + 1;
    }

    // RELRO segment.
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type == PT_GNU_RELRO) {
            l->relro_va = l->phdrs[i].p_vaddr;
            l->relro_sz = l->phdrs[i].p_memsz;
        }
    }

    // TLS template.
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type == PT_TLS) {
            l->tls_template = l->image + l->phdrs[i].p_offset;
            l->tls_filesz = l->phdrs[i].p_filesz;
            l->tls_memsz  = l->phdrs[i].p_memsz;
            l->tls_align  = l->phdrs[i].p_align ? l->phdrs[i].p_align : 1;
        }
    }
}

// ============================================================
// Hash functions.
// ============================================================
uint32_t sysv_hash(const char* s) {
    uint32_t h = 0;
    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
uint32_t gnu_hash_fn(const char* s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h;
}

// ============================================================
// Symbol lookup in a single library.
// ============================================================
const Sym* lookup_in_lib(Lib* l, const char* name, uint32_t* out_idx = nullptr) {
    if (!l || !l->strtab || !l->symtab) return nullptr;

    // Try GNU_HASH first.
    if (l->gnu_hash) {
        uint32_t* gh = l->gnu_hash;
        uint32_t nbuckets = gh[0];
        uint32_t symoff   = gh[1];
        uint32_t bloom_sz = gh[2];
        uint32_t bloom_shift = gh[3];
        uint64_t* bloom = (uint64_t*)(gh + 4);
        uint32_t* buckets = (uint32_t*)(bloom + bloom_sz);
        uint32_t* chain   = buckets + nbuckets;

        uint32_t h  = gnu_hash_fn(name);
        uint64_t bw = bloom[(h / 64) & (bloom_sz - 1)];
        uint64_t mask = (1ull << (h & 63)) | (1ull << ((h >> bloom_shift) & 63));
        if ((bw & mask) != mask) goto try_sysv;
        uint32_t idx = buckets[h % nbuckets];
        if (idx < symoff) goto try_sysv;
        for (;; idx++) {
            uint32_t ch = chain[idx - symoff];
            if (((ch ^ h) >> 1) == 0) {
                const Sym* s = &l->symtab[idx];
                if (s_eq(l->strtab + s->st_name, name)) {
                    if (out_idx) *out_idx = idx;
                    return s;
                }
            }
            if (ch & 1) break;
        }
    }
try_sysv:
    if (l->sysv_hash) {
        uint32_t nbuckets = l->sysv_hash[0];
        uint32_t* buckets = l->sysv_hash + 2;
        uint32_t* chains  = buckets + nbuckets;
        uint32_t h = sysv_hash(name);
        for (uint32_t i = buckets[h % nbuckets]; i; i = chains[i]) {
            const Sym* s = &l->symtab[i];
            if (s_eq(l->strtab + s->st_name, name)) {
                if (out_idx) *out_idx = i;
                return s;
            }
        }
    }
    // Last resort: linear scan (works even without hash tables).
    for (int i = 0; i < l->symcount; i++) {
        const Sym* s = &l->symtab[i];
        if (s->st_name && s_eq(l->strtab + s->st_name, name)) {
            if (out_idx) *out_idx = (uint32_t)i;
            return s;
        }
    }
    return nullptr;
}

// ============================================================
// Symbol lookup across the process scope.  Returns (lib, sym).
// ============================================================
struct SymHit { Lib* lib; const Sym* sym; };

SymHit lookup_global(ProcLinkerState* pls, const char* name,
                     bool skip_weak_first = true) {
    SymHit weak = { nullptr, nullptr };
    for (int i = 0; i < pls->lib_count; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        const Sym* s = lookup_in_lib(l, name);
        if (!s || s->st_shndx == 0) continue;       // SHN_UNDEF
        if (ST_VISIBILITY(s->st_other) == STV_HIDDEN ||
            ST_VISIBILITY(s->st_other) == STV_INTERNAL) continue;
        uint8_t bind = ST_BIND(s->st_info);
        if (bind == STB_GLOBAL) return { l, s };
        if (bind == STB_WEAK) {
            if (!skip_weak_first) return { l, s };
            if (!weak.sym) weak = { l, s };
        }
    }
    return weak;
}

// ============================================================
// Page mapping helpers.
// ============================================================
bool map_user_page(Process* proc, uint64_t va, const uint8_t* src,
                   uint64_t copy_len, uint64_t in_page_off,
                   uint64_t pte_flags) {
    void* pg = PMM::AllocBytes(PAGE_SIZE);
    if (!pg) return false;
    memset(pg, 0, PAGE_SIZE);
    if (src && copy_len)
        memcpy((uint8_t*)pg + in_page_off, src, (size_t)copy_len);
    if (!KernelVMM::MapPageInAddressSpace(proc->address_space, va,
                                          (uint64_t)(uintptr_t)pg, pte_flags)) {
        PMM::FreeBytes(pg, PAGE_SIZE);
        return false;
    }
    return true;
}

bool map_lib_segments(Process* proc, Lib* l, ProcLinkerState* pls) {
    for (int i = 0; i < l->phnum; i++) {
        const Phdr& p = l->phdrs[i];
        if (p.p_type != PT_LOAD || p.p_memsz == 0) continue;
        uint64_t va_lo = (l->load_base + p.p_vaddr) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t va_hi = (l->load_base + p.p_vaddr + p.p_memsz + PAGE_SIZE - 1)
                         & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t flags = PTE_USER;
        if (p.p_flags & PF_W) flags |= PTE_WRITABLE;
        if (!(p.p_flags & PF_X)) flags |= PTE_NX;

        for (uint64_t va = va_lo; va < va_hi; va += PAGE_SIZE) {
            uint64_t page_lo = va - l->load_base;
            uint64_t page_hi = page_lo + PAGE_SIZE;
            uint64_t file_lo = p.p_vaddr;
            uint64_t file_hi = p.p_vaddr + p.p_filesz;
            uint64_t lo = page_lo > file_lo ? page_lo : file_lo;
            uint64_t hi = page_hi < file_hi ? page_hi : file_hi;
            uint64_t in_page_off = 0, copy_len = 0;
            const uint8_t* src = nullptr;
            if (lo < hi) {
                in_page_off = lo - page_lo;
                copy_len    = hi - lo;
                src         = l->image + p.p_offset + (lo - file_lo);
            }
            // Map all segments writable initially so reloc can patch
            // them; we re-protect after relocations (PT_GNU_RELRO).
            if (!map_user_page(proc, va, src, copy_len, in_page_off,
                               PTE_USER | PTE_WRITABLE |
                               (flags & PTE_NX))) {
                set_error(pls, "map_user_page failed");
                return false;
            }
        }
        if (l->load_size < p.p_vaddr + p.p_memsz)
            l->load_size = p.p_vaddr + p.p_memsz;
    }
    return true;
}

void apply_relro(Process* proc, Lib* l) {
    if (!l->relro_va || !l->relro_sz) return;
    uint64_t va_lo = (l->load_base + l->relro_va) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t va_hi = (l->load_base + l->relro_va + l->relro_sz + PAGE_SIZE - 1)
                     & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t va = va_lo; va < va_hi; va += PAGE_SIZE) {
        uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
            proc->address_space, va);
        if (!phys) continue;
        KernelVMM::MapPageInAddressSpace(proc->address_space, va, phys,
                                         PTE_USER | PTE_NX);
    }
}

// ============================================================
// Patch a 64-bit value in the user image.  Because we hold the kernel
// pointer to the in-heap image we can write to it AND propagate to the
// already-mapped user page (the page was copied from the image).
// ============================================================
void patch_image_u64(Process* proc, Lib* l, uint64_t va, uint64_t val) {
    // 1. Patch the heap copy (so dlopen-after-fork sees the same data).
    uint8_t* p = vaddr_to_image(l, va - l->load_base);
    if (p) *(uint64_t*)p = val;

    // 2. Patch the user-mode page.
    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
        proc->address_space, page);
    if (!phys) return;
    *(uint64_t*)(uintptr_t)(phys + (va & (PAGE_SIZE - 1))) = val;
}
void patch_image_u32(Process* proc, Lib* l, uint64_t va, uint32_t val) {
    uint8_t* p = vaddr_to_image(l, va - l->load_base);
    if (p) *(uint32_t*)p = val;
    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
        proc->address_space, page);
    if (!phys) return;
    *(uint32_t*)(uintptr_t)(phys + (va & (PAGE_SIZE - 1))) = val;
}

// ============================================================
// Apply one relocation.
// ============================================================
bool apply_one_reloc(Process* proc, ProcLinkerState* pls, Lib* l,
                     const Rela* r) {
    uint32_t type = R_TYPE(r->r_info);
    uint32_t sym  = R_SYM (r->r_info);
    uint64_t P    = l->load_base + r->r_offset;        // address being patched
    uint64_t A    = (uint64_t)r->r_addend;
    uint64_t S    = 0;
    Lib*     defining = nullptr;
    uint64_t tls_off  = 0;
    uint32_t tls_mod  = 0;

    if (sym && l->symtab) {
        const Sym* sx = &l->symtab[sym];
        const char* nm = l->strtab + sx->st_name;
        if (sx->st_shndx != 0) {
            // Defined in this lib.
            S = l->load_base + sx->st_value;
            defining = l;
        } else {
            // Undefined  -  global lookup.
            SymHit hit = lookup_global(pls, nm);
            if (hit.sym) {
                S = hit.lib->load_base + hit.sym->st_value;
                defining = hit.lib;
                if (ST_TYPE(hit.sym->st_info) == STT_TLS) {
                    tls_off = hit.sym->st_value - hit.lib->tls_offset;
                    tls_mod = hit.lib->tls_modid;
                }
            } else if (ST_BIND(sx->st_info) == STB_WEAK) {
                S = 0;            // weak undef -> NULL
            } else {
                // Hard undefined  -  don't fail the whole load; emit
                // diagnostic and patch with 0 so the program can run
                // until it actually invokes the missing symbol.
                static char ebuf[256];
                int p = 0;
                const char* m = "undefined symbol: ";
                while (m[p] && p < 24) { ebuf[p] = m[p]; p++; }
                int q = 0;
                while (nm && nm[q] && p < (int)sizeof(ebuf) - 1)
                    ebuf[p++] = nm[q++];
                ebuf[p] = 0;
                set_error(pls, ebuf);
                S = 0;
            }
        }
    }

    switch (type) {
        case R_X86_64_NONE: return true;
        case R_X86_64_64:
            patch_image_u64(proc, l, P, S + A); return true;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX:
            patch_image_u32(proc, l, P, (uint32_t)((S + A) - P));
            return true;
        case R_X86_64_PC64:
            patch_image_u64(proc, l, P, (S + A) - P); return true;
        case R_X86_64_32:
        case R_X86_64_32S:
            patch_image_u32(proc, l, P, (uint32_t)(S + A)); return true;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            patch_image_u64(proc, l, P, S + A); return true;
        case R_X86_64_RELATIVE:
            patch_image_u64(proc, l, P, l->load_base + A); return true;
        case R_X86_64_IRELATIVE: {
            // Call resolver at (load_base + A); whatever it returns is
            // the final address.  Resolver runs in the user context's
            // address space  -  but for correctness during early link we
            // simply patch with the function pointer itself.  The first
            // call from user code will execute the resolver normally.
            patch_image_u64(proc, l, P, l->load_base + A);
            return true;
        }
        case R_X86_64_COPY: {
            // Copy `st_size` bytes from the defining library's symbol
            // into our object.
            if (!defining || !sym) return true;
            const Sym* sx = &l->symtab[sym];
            uint64_t src_va = S;
            uint64_t bytes  = sx->st_size;
            uint8_t* sp = vaddr_to_image(defining,
                                         src_va - defining->load_base);
            if (!sp) return true;
            for (uint64_t k = 0; k < bytes; k += 8) {
                uint64_t v = 0;
                uint64_t take = bytes - k > 8 ? 8 : bytes - k;
                for (uint64_t b = 0; b < take; b++)
                    ((uint8_t*)&v)[b] = sp[k + b];
                if (take == 8) patch_image_u64(proc, l, P + k, v);
                else {
                    // Partial trailing bytes  -  fall back to per-byte.
                    uint64_t page = (P + k) & ~(uint64_t)(PAGE_SIZE - 1);
                    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
                        proc->address_space, page);
                    if (phys) {
                        for (uint64_t b = 0; b < take; b++)
                            ((uint8_t*)(uintptr_t)phys)[((P + k + b) &
                                (PAGE_SIZE - 1))] = sp[k + b];
                    }
                }
            }
            return true;
        }
        case R_X86_64_TPOFF64:
        case R_X86_64_TPOFF32: {
            // Negative offset from TP (FS base in variant 2).
            int64_t off = (defining ? defining->tls_offset : 0) +
                          (sym ? l->symtab[sym].st_value : 0) + A;
            if (type == R_X86_64_TPOFF64)
                patch_image_u64(proc, l, P, (uint64_t)off);
            else
                patch_image_u32(proc, l, P, (uint32_t)off);
            return true;
        }
        case R_X86_64_DTPMOD64:
            patch_image_u64(proc, l, P, tls_mod ? tls_mod : l->tls_modid);
            return true;
        case R_X86_64_DTPOFF64:
            patch_image_u64(proc, l, P, tls_off + A); return true;
        case R_X86_64_DTPOFF32:
            patch_image_u32(proc, l, P, (uint32_t)(tls_off + A)); return true;
        case R_X86_64_TLSDESC:
            // TLSDESC writes a 2-word descriptor: [resolver, arg].
            // Eager resolution: install __tls_get_addr-equivalent and
            // the symbol's TLS offset.
            patch_image_u64(proc, l, P,     0);          // resolver = id
            patch_image_u64(proc, l, P + 8, tls_off + A);
            return true;
        case R_X86_64_TLSGD:
        case R_X86_64_TLSLD:
        case R_X86_64_GOTTPOFF:
            // These are normally rewritten by the linker at link time;
            // if we see them here treat as PC-relative offset to TLS.
            patch_image_u32(proc, l, P, (uint32_t)((tls_off + A) - P));
            return true;
        default:
            // Unknown relocation type  -  log but don't fail; many
            // libraries contain a few reloc types our handler doesn't
            // need (e.g. older MIPS-isms slipped into x86_64).
            dlog(pls, LdKurono::DBG_RELOC, "unknown reloc type, skipped");
            return true;
    }
}

bool apply_relocs(Process* proc, ProcLinkerState* pls, Lib* l) {
    if (l->rela) {
        for (int i = 0; i < l->rela_count; i++)
            apply_one_reloc(proc, pls, l, &l->rela[i]);
    }
    if (l->jmprel) {
        // For now we always do EAGER PLT binding (lazy stubs not yet
        // implemented).  This matches the behaviour requested when
        // DT_BIND_NOW or LD_BIND_NOW is set, and is also forced when
        // PT_GNU_RELRO covers the .got.plt area.
        for (int i = 0; i < l->jmprel_count; i++)
            apply_one_reloc(proc, pls, l, &l->jmprel[i]);
    }
    l->relocated = true;
    return true;
}

// ============================================================
// Add lib to per-process scope and load DT_NEEDED dependencies.
// ============================================================
Lib* load_library_recursive(Process* proc, ProcLinkerState* pls,
                            const char* name, bool global,
                            bool* out_already_loaded);

void process_dt_needed(Process* proc, ProcLinkerState* pls, Lib* l) {
    if (!l->dyn || !l->strtab) return;
    for (int i = 0; i < l->dyn_count && l->dyn[i].d_tag != DT_NULL; i++) {
        if (l->dyn[i].d_tag != DT_NEEDED) continue;
        const char* nm = l->strtab + l->dyn[i].d_un;
        if (!nm[0]) continue;
        // Skip the linker itself - we provide it built-in.
        if (s_starts(nm, "ld-linux-x86-64.so") ||
            s_starts(nm, "ld-linux.so")        ||
            s_starts(nm, "ld-kurono.so")) continue;
        bool already = false;
        Lib* dep = load_library_recursive(proc, pls, nm, l->global, &already);
        if (dep && l->dep_count < LdKurono::LDK_MAX_DEPS) {
            l->deps[l->dep_count++] = lib_index(dep);
        }
    }
}

// ============================================================
// Core load-from-file + ASLR + map.
// ============================================================
Lib* do_load(Process* proc, ProcLinkerState* pls,
             const char* path, bool is_main, bool is_linker, bool global) {
    // Check for soname-based dedup AFTER reading the header (so we
    // honour the file's actual SONAME).
    constexpr uint32_t MAX = 16u * 1024u * 1024u;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(MAX);
    if (!buf) { set_error(pls, "out of memory"); return nullptr; }
    int got = KVFS::ReadFile(path, buf, MAX);
    if (got <= 0) {
        KernelHeap::Free(buf);
        set_error(pls, "library file not found");
        return nullptr;
    }
    if (!valid_elf64(buf, (uint64_t)got)) {
        KernelHeap::Free(buf);
        set_error(pls, "invalid ELF64");
        return nullptr;
    }

    Lib* l = alloc_lib();
    if (!l) { KernelHeap::Free(buf); set_error(pls, "lib table full"); return nullptr; }
    l->image = buf;
    l->image_size = (uint64_t)got;
    s_cpy(l->path, path, sizeof(l->path));

    Ehdr* eh = (Ehdr*)buf;
    l->phdrs = (Phdr*)(buf + eh->e_phoff);
    l->phnum = eh->e_phnum;
    l->phentsize = eh->e_phentsize;
    l->entry = eh->e_entry;
    l->is_main_exec = is_main;
    l->is_linker = is_linker;
    l->global = global;

    // Compute total memory span across PT_LOAD segments.
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type != PT_LOAD) continue;
        if (l->phdrs[i].p_vaddr < lo) lo = l->phdrs[i].p_vaddr;
        uint64_t end = l->phdrs[i].p_vaddr + l->phdrs[i].p_memsz;
        if (end > hi) hi = end;
    }
    if (lo == ~0ULL) lo = 0;
    uint64_t span = hi - lo;

    // ASLR base.  ET_EXEC keeps its own absolute layout.
    if (eh->e_type == ET_DYN) {
        l->load_base = pick_aslr_base(span) - lo;
    } else {
        l->load_base = 0;       // ET_EXEC honours its absolute vaddrs
    }
    l->load_size = span;

    // Find phdr_va so AT_PHDR can be filled.
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type == PT_PHDR) {
            l->phdr_va = l->load_base + l->phdrs[i].p_vaddr;
            break;
        }
    }
    if (!l->phdr_va) l->phdr_va = l->load_base + eh->e_phoff;

    // Default soname = basename of path.
    if (!l->soname[0]) {
        const char* b = path;
        for (const char* p = path; *p; p++) if (*p == '/') b = p + 1;
        s_cpy(l->soname, b, sizeof(l->soname));
    }

    parse_dynamic(l);

    // TLS module id assignment.
    if (l->tls_memsz) {
        l->tls_modid = ++pls->next_tls_modid;
        // variant 2: TLS lives at negative offset from FS base.
        // Stack new module on top of running offset.
        pls->tls_block_size += l->tls_memsz;
        // Align.
        if (l->tls_align > 1) {
            pls->tls_block_size = (pls->tls_block_size + l->tls_align - 1) &
                                  ~(l->tls_align - 1);
        }
        l->tls_offset = pls->tls_block_size;
    }

    // Map segments now (writable; relro applied later).
    if (!map_lib_segments(proc, l, pls)) {
        KernelHeap::Free(buf);
        l->in_use = false;
        return nullptr;
    }

    // Add to process scope.
    if (pls->lib_count < LdKurono::LDK_MAX_LIBS_PER_PROC) {
        pls->libs[pls->lib_count++] = lib_index(l);
    }

    {
        char m[LdKurono::LDK_PATH_LEN + 64];
        int p = 0;
        const char* pre = "loaded ";
        while (pre[p]) { m[p] = pre[p]; p++; }
        for (int i = 0; l->soname[i] && p < (int)sizeof(m) - 32; i++)
            m[p++] = l->soname[i];
        const char* mid = " from ";
        for (int i = 0; mid[i] && p < (int)sizeof(m) - 16; i++) m[p++] = mid[i];
        for (int i = 0; l->path[i] && p < (int)sizeof(m) - 1; i++) m[p++] = l->path[i];
        m[p] = 0;
        dlog(pls, LdKurono::DBG_LIBS | LdKurono::DBG_FILES, m);
    }
    return l;
}

Lib* load_library_recursive(Process* proc, ProcLinkerState* pls,
                            const char* name, bool global,
                            bool* out_already_loaded) {
    if (out_already_loaded) *out_already_loaded = false;

    // Dedup by soname or by basename.
    Lib* existing = lib_by_soname_proc(pls, name);
    if (!existing) {
        const char* b = name;
        for (const char* p = name; *p; p++) if (*p == '/') b = p + 1;
        existing = lib_by_soname_proc(pls, b);
    }
    if (existing) {
        existing->refcount++;
        if (global) existing->global = true;
        if (out_already_loaded) *out_already_loaded = true;
        return existing;
    }

    char resolved[LdKurono::LDK_PATH_LEN];
    if (!resolve_lib_path(pls, name, resolved, sizeof(resolved))) {
        set_error(pls, "library not found in search path");
        return nullptr;
    }

    Lib* l = do_load(proc, pls, resolved, false, false, global);
    if (!l) return nullptr;

    // Recurse into dependencies before applying relocations to *this*
    // library  -  symbol lookup needs all defs visible.
    process_dt_needed(proc, pls, l);
    return l;
}

// ============================================================
// Search path setup.
// ============================================================
void seed_searchpath(ProcLinkerState* pls, const char* env_ld_library_path) {
    auto add = [&](const char* p) {
        if (pls->searchpath_count >= LdKurono::LDK_MAX_SEARCHPATHS) return;
        s_cpy(pls->searchpaths[pls->searchpath_count++], p,
              LdKurono::LDK_PATH_LEN);
    };
    if (env_ld_library_path && !pls->secure) {
        // Split on ':'.
        const char* s = env_ld_library_path;
        while (*s) {
            char tmp[LdKurono::LDK_PATH_LEN]; int p = 0;
            while (*s && *s != ':' && p < (int)sizeof(tmp) - 1) tmp[p++] = *s++;
            tmp[p] = 0;
            if (p) add(tmp);
            if (*s == ':') s++;
        }
    }
    add("/system/lib");
    add("/system/lib/kurono");
    add("/system/lib/x86_64-linux-gnu");
    add("/apps/lib");
    // firefox ships its whole .so closure (incl. the musl loader) here; keep it
    // on the default path so the exe resolves even without LD_LIBRARY_PATH set.
    // (satoru)
    add("/apps/firefox/lib");
    add("/system/local/lib");
    add("/home/user/.local/lib");
}

// ============================================================
// vDSO emission  -  write a tiny ELF into a single user page that
// exports clock_gettime / gettimeofday / time / getcpu trampolines.
// ============================================================
uint64_t build_vdso(Process* proc) {
    void* pg = PMM::AllocBytes(PAGE_SIZE);
    if (!pg) return 0;
    memset(pg, 0, PAGE_SIZE);
    uint8_t* p = (uint8_t*)pg;

    // Place a minimal ELF header so AT_SYSINFO_EHDR points to a valid
    // ELF.  It contains no PT_LOAD; user code is expected to call the
    // exported symbols directly.  We just emit syscall stubs at known
    // offsets the libc patches up.
    p[0] = 0x7F; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
    p[4] = 2; p[5] = 1; p[6] = 1;            // class=64, data=LSB, ver=1
    Ehdr* eh = (Ehdr*)p;
    eh->e_type = ET_DYN;
    eh->e_machine = EM_X86_64;
    eh->e_version = 1;
    eh->e_entry = 0;
    eh->e_ehsize = sizeof(Ehdr);
    eh->e_phentsize = sizeof(Phdr);
    eh->e_phoff = sizeof(Ehdr);
    eh->e_phnum = 0;

    // Stubs:  each is `mov rax, NR ; syscall ; ret`.
    // __vdso_clock_gettime  NR = 228
    uint8_t* stub = p + 0x100;
    static const uint8_t s_cg[] = {
        0x48, 0xC7, 0xC0, 228, 0, 0, 0,    // mov rax, 228
        0x0F, 0x05,                        // syscall
        0xC3                                // ret
    };
    for (unsigned i = 0; i < sizeof(s_cg); i++) stub[i] = s_cg[i];
    // __vdso_gettimeofday  NR = 96
    stub = p + 0x180;
    static const uint8_t s_gtod[] = {
        0x48, 0xC7, 0xC0, 96, 0, 0, 0,
        0x0F, 0x05, 0xC3
    };
    for (unsigned i = 0; i < sizeof(s_gtod); i++) stub[i] = s_gtod[i];
    // __vdso_time  NR = 201
    stub = p + 0x200;
    static const uint8_t s_time[] = {
        0x48, 0xC7, 0xC0, 201, 0, 0, 0,
        0x0F, 0x05, 0xC3
    };
    for (unsigned i = 0; i < sizeof(s_time); i++) stub[i] = s_time[i];
    // __vdso_getcpu  NR = 309
    stub = p + 0x280;
    static const uint8_t s_gcpu[] = {
        0x48, 0xC7, 0xC0, 0x35, 1, 0, 0,
        0x0F, 0x05, 0xC3
    };
    for (unsigned i = 0; i < sizeof(s_gcpu); i++) stub[i] = s_gcpu[i];

    uint64_t va = 0x7FFFF7FFC000ULL;
    if (!KernelVMM::MapPageInAddressSpace(proc->address_space, va,
                                          (uint64_t)(uintptr_t)pg,
                                          PTE_USER)) {
        PMM::FreeBytes(pg, PAGE_SIZE);
        return 0;
    }
    return va;
}

// ============================================================
// Auxv stack frame builder.
//
// User stack layout (top-down):
//
//   argc
//   argv[0..argc-1]
//   NULL
//   envp[0..]
//   NULL
//   auxv[]  (Elf64_auxv_t pairs, terminated with AT_NULL)
//   string table
// ============================================================

#define AT_NULL    0
#define AT_IGNORE  1
#define AT_EXECFD  2
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_NOTELF  10
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_PLATFORM 15
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM  25
#define AT_HWCAP2  26
#define AT_EXECFN  31
#define AT_SYSINFO_EHDR 33

struct AuxEnt { uint64_t a_type; uint64_t a_val; };

uint64_t push_string(Process* proc, uint64_t* sp_ref, const char* s) {
    int len = s_len(s) + 1;
    *sp_ref -= len;
    // Find user page.
    uint64_t addr = *sp_ref;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
        proc->address_space, page);
    if (!phys) return 0;
    uint8_t* dst = (uint8_t*)(uintptr_t)(phys + (addr & (PAGE_SIZE - 1)));
    for (int i = 0; i < len; i++) dst[i] = (uint8_t)s[i];
    return addr;
}

void push_qword(Process* proc, uint64_t* sp_ref, uint64_t v) {
    *sp_ref -= 8;
    uint64_t addr = *sp_ref;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
        proc->address_space, page);
    if (!phys) return;
    *(uint64_t*)(uintptr_t)(phys + (addr & (PAGE_SIZE - 1))) = v;
}

// write len bytes from kernel `src` (or zero-fill if src==null) to user va
// `dst`, page by page  -  tls block pages are individually allocated and not
// physically contiguous, so each page is resolved through the address space
// separately, and writes that straddle a page boundary are split. (satoru)
bool write_user_mem(Process* proc, uint64_t dst, const uint8_t* src, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t va    = dst + done;
        uint64_t page  = va & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t off   = va & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > len - done) chunk = len - done;
        uint64_t phys = KernelVMM::QueryMappingInAddressSpace(proc->address_space, page);
        if (!phys) return false;
        uint8_t* d = (uint8_t*)(uintptr_t)(phys + off);
        if (src) for (uint64_t i = 0; i < chunk; i++) d[i] = src[done + i];
        else     for (uint64_t i = 0; i < chunk; i++) d[i] = 0;
        done += chunk;
    }
    return true;
}
inline bool write_user_u64(Process* proc, uint64_t dst, uint64_t v) {
    return write_user_mem(proc, dst, (const uint8_t*)&v, 8);
}

// install the main thread's variant-2 tls + thread pointer. a dynamic musl
// program expects its linker (us) to have set tls up before _start runs: the
// thread pointer (fs base) points at a tcb whose self-slot (fs:0) is the tp
// itself, with each module's tls image laid out at tp - tls_offset just below
// it. without this the very first __pthread_self() reads fs:0 == null and
// faults (the cr2=0 #pf we hit). static musl escapes this by setting its own fs
// via the syscall path; dynamic musl never does. (satoru)
bool install_main_tls(Process* proc, ProcLinkerState* pls) {
    constexpr uint64_t TCB_SIZE = 0x100;        // musl struct pthread head: self@0 dtv@8 canary@0x28
    uint64_t modcount  = pls->next_tls_modid;   // module ids run 1..modcount
    uint64_t tls_area  = (pls->tls_block_size + 63) & ~63ULL;  // tls data sits below tp
    uint64_t dtv_bytes = (modcount + 1) * 8;
    uint64_t total = tls_area + TCB_SIZE + dtv_bytes;
    total = (total + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!total) total = PAGE_SIZE;

    uint64_t base  = pick_aslr_base(total);
    uint64_t flags = PTE_USER | PTE_WRITABLE | PTE_NX;
    for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
        if (!map_user_page(proc, base + off, nullptr, 0, 0, flags)) {
            set_error(pls, "tls block map failed");
            return false;
        }
    }

    uint64_t tp     = base + tls_area;          // thread pointer == tcb base (>=64-aligned)
    uint64_t dtv_va = tp + TCB_SIZE;

    // copy each module's init image to tp - tls_offset; record it in the dtv.
    for (int i = 0; i < pls->lib_count; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        // skip modules with no tls or no assigned offset (e.g. a main exec with
        // its own tls  -  that case needs a separate offset pass, noted). (satoru)
        if (!l->tls_memsz || !l->tls_offset) continue;
        uint64_t mod_addr = tp - l->tls_offset;
        if (l->tls_template && l->tls_filesz)
            write_user_mem(proc, mod_addr, l->tls_template, l->tls_filesz);
        // the rest (tls_memsz - tls_filesz, the .tbss) stays zero (pages cleared).
        if (l->tls_modid)
            write_user_u64(proc, dtv_va + l->tls_modid * 8, mod_addr);
    }
    write_user_u64(proc, dtv_va, modcount);             // dtv[0] = module count

    // tcb head (x86-64 variant 2): self-pointer, dtv pointer, stack canary.
    write_user_u64(proc, tp + 0x00, tp);
    write_user_u64(proc, tp + 0x08, dtv_va);
    write_user_u64(proc, tp + 0x28, aslr_rand() | 1ULL);

    pls->tls_block_va = base;
    proc->fs_base = tp;                                 // scheduler programs MSR_FS_BASE from this
    return true;
}

// ============================================================
// Init/fini.
// ============================================================
void call_init_arrays_recursive(Process* proc, ProcLinkerState* pls, Lib* l) {
    if (l->init_called) return;
    l->init_called = true;
    for (int i = 0; i < l->dep_count; i++)
        call_init_arrays_recursive(proc, pls, &g_libs[l->deps[i]]);
    if (l->init_func) dlog(pls, LdKurono::DBG_BINDINGS, "queued DT_INIT");
    if (l->init_array_n)
        dlog(pls, LdKurono::DBG_BINDINGS, "queued DT_INIT_ARRAY");
}

// ============================================================
// Build a user-mode bootstrap trampoline that runs every queued
// DT_INIT / DT_INIT_ARRAY in dependency order, preserving the SysV
// (argc, argv, envp) registers between calls, and finally tail-jumps
// to the real ELF entry point.  Returns the new entry VA the kernel
// should set as the process RIP.  On failure it returns real_entry,
// which is harmless  -  we just skip constructors.
// ============================================================
uint64_t build_init_trampoline(Process* proc, ProcLinkerState* pls,
                               uint64_t real_entry) {
    static uint64_t funcs[1024];
    int nf = 0;

    // Collect in dependency order: deepest deps first, main exec last.
    // pls->libs[0] is the main exec; deps were appended after, so we
    // iterate from the tail down towards the head.
    for (int i = pls->lib_count - 1; i >= 0 && nf < 1024; i--) {
        Lib* l = &g_libs[pls->libs[i]];
        if (l->init_func && l->init_func != (uint64_t)-1)
            funcs[nf++] = l->load_base + l->init_func;
        for (int j = 0; j < l->init_array_n && nf < 1024; j++) {
            uint64_t f = l->init_array[j];
            if (f && f != (uint64_t)-1) funcs[nf++] = f;
        }
    }
    if (nf == 0) return real_entry;

    // Bound the trampoline to one page.  Each call site is 24 bytes,
    // prologue 14, epilogue 12  -  plenty of room for ~160 inits.
    constexpr int CALL_BYTES = 24;
    constexpr int PROLOGUE   = 14;
    constexpr int EPILOGUE   = 12;
    int max_calls = (PAGE_SIZE - PROLOGUE - EPILOGUE) / CALL_BYTES;
    if (nf > max_calls) nf = max_calls;

    void* pg = PMM::AllocBytes(PAGE_SIZE);
    if (!pg) return real_entry;
    memset(pg, 0, PAGE_SIZE);
    uint8_t* code = (uint8_t*)pg;
    int p = 0;

    // Prologue: capture argc/argv off the stack into r12/r14, zero rbp.
    static const uint8_t pro[PROLOGUE] = {
        0x48, 0x31, 0xED,              // xor  rbp, rbp
        0x4C, 0x8B, 0x24, 0x24,        // mov  r12, [rsp]   ; argc
        0x49, 0x89, 0xE6,              // mov  r14, rsp
        0x49, 0x83, 0xC6, 0x08,        // add  r14, 8       ; argv = rsp+8
    };
    for (int i = 0; i < PROLOGUE; i++) code[p++] = pro[i];

    // For each init function: re-materialise argc/argv/envp in
    // rdi/rsi/rdx (the previous call may have clobbered them), load
    // the function pointer into rax, and call it.  The 'lea rdx,
    // [r14 + r12*8 + 8]' computes envp = argv + (argc + 1) * 8.
    for (int k = 0; k < nf; k++) {
        static const uint8_t args[11] = {
            0x4C, 0x89, 0xE7,              // mov  rdi, r12
            0x4C, 0x89, 0xF6,              // mov  rsi, r14
            0x4B, 0x8D, 0x54, 0xE6, 0x08,  // lea  rdx, [r14 + r12*8 + 8]
        };
        for (int i = 0; i < 11; i++) code[p++] = args[i];
        code[p++] = 0x48; code[p++] = 0xB8;            // mov  rax, imm64
        for (int b = 0; b < 8; b++) code[p++] = (uint8_t)(funcs[k] >> (b * 8));
        code[p++] = 0xFF; code[p++] = 0xD0;            // call rax
    }

    // Epilogue: load real entry into rax and jump.  rsp is unchanged
    // (callees preserved/restored their own frames), so argv/envp
    // remain on the stack exactly where _start expects them.
    code[p++] = 0x48; code[p++] = 0xB8;                // mov  rax, imm64
    for (int b = 0; b < 8; b++) code[p++] = (uint8_t)(real_entry >> (b * 8));
    code[p++] = 0xFF; code[p++] = 0xE0;                // jmp  rax

    // Map the page into the user address space at a fixed high VA.
    // NOTE: PTE_USER without PTE_NX → executable user page.
    constexpr uint64_t TRAMP_VA = 0x600000000ULL;
    if (!KernelVMM::MapPageInAddressSpace(proc->address_space, TRAMP_VA,
                                          (uint64_t)(uintptr_t)pg,
                                          PTE_USER)) {
        PMM::FreeBytes(pg, PAGE_SIZE);
        return real_entry;
    }

    char m[64]; int q = 0;
    const char* pre = "init trampoline ready, ";
    while (pre[q]) { m[q] = pre[q]; q++; }
    // append count (decimal)
    int n = nf, dig = 0; char db[8];
    if (n == 0) { db[dig++] = '0'; }
    while (n) { db[dig++] = (char)('0' + (n % 10)); n /= 10; }
    while (dig) m[q++] = db[--dig];
    const char* tail = " ctors";
    while (*tail) m[q++] = *tail++;
    m[q] = 0;
    dlog(pls, LdKurono::DBG_BINDINGS, m);
    return TRAMP_VA;
}

}  // namespace

// ============================================================
// Public API
// ============================================================
namespace LdKurono {

void Init() {
    for (int i = 0; i < LDK_MAX_LIBS_GLOBAL; i++) g_libs[i].in_use = false;
    for (int i = 0; i < MAX_PROCS; i++) g_procs[i].in_use = false;
    SerialLogger::Log("[ldso] ld-kurono dynamic linker initialised\r\n");
    // Drop a stub at /system/lib/ld-kurono.so so user-space tools see
    // the file when scanning.  The real linker runs in the kernel; this
    // file is just a marker (an empty ELF header is fine for `file(1)`).
    static const uint8_t marker[] = {
        0x7F, 'E', 'L', 'F',
        2, 1, 1, 0,         // class=64, data=LSB, version=1, OSABI=0
        0, 0, 0, 0, 0, 0, 0, 0,
        0x03, 0x00,         // ET_DYN
        0x3E, 0x00,         // EM_X86_64
        1, 0, 0, 0,         // version
    };
    KVFS::WriteFile("/system/lib/ld-kurono.so",
                    (uint8_t*)marker, sizeof(marker));
    // Symlinks are presented via the path translation layer
    // (linux_syscall.cpp::ResolvePath); we don't need to drop separate
    // files for ld-linux-x86-64.so.2 etc.
}

uint64_t MapVDSO(Process* proc) { return build_vdso(proc); }

bool IsLoaded(Process* proc, const char* soname) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls) return false;
    return lib_by_soname_proc(pls, soname) != nullptr;
}

int LoadedCount(Process* proc) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    return pls ? pls->lib_count : 0;
}

const char* Dlerror(Process* proc) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !pls->last_error[0]) return nullptr;
    return pls->last_error;
}

void DlDebugStateNotify() {
    // GDB sets a software breakpoint here.  We make this an explicit
    // callable function so the symbol resolves; in production GDB would
    // poke int3 into the user-mode trampoline, but for kernel-side
    // debugging we just emit a serial log line so the developer can see
    // when libraries change.
    SerialLogger::Log("[ldso] _dl_debug_state\r\n");
}

void* Dlopen(Process* proc, const char* file, uint32_t flags) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls) pls = alloc_pls(proc);
    if (!pls) return nullptr;

    if (!file) {
        // file == NULL means the main executable.
        if (pls->lib_count > 0) return &g_libs[pls->libs[0]];
        return nullptr;
    }

    if (flags & RTLD_NOLOAD) {
        Lib* e = lib_by_soname_proc(pls, file);
        return e;
    }
    bool global = (flags & RTLD_GLOBAL) != 0;
    bool already = false;
    Lib* l = load_library_recursive(proc, pls, file, global, &already);
    if (!l) return nullptr;
    l->deepbind = (flags & RTLD_DEEPBIND) != 0;
    l->nodelete = (flags & RTLD_NODELETE) != 0;
    if (!already) {
        apply_relocs(proc, pls, l);
        apply_relro (proc, l);
        call_init_arrays_recursive(proc, pls, l);
        DlDebugStateNotify();
    }
    return l;
}

int Dlclose(Process* proc, void* handle) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !handle) return -1;
    Lib* l = (Lib*)handle;
    if (l->refcount > 0) l->refcount--;
    if (l->refcount == 0 && !l->nodelete) {
        // Run finalizers (queued, deferred).
        dlog(pls, DBG_LIBS, "unload library");
        // Do not actually unmap pages  -  Firefox's plugin model dlcloses
        // libxul aggressively and we don't want to crash on next call.
    }
    DlDebugStateNotify();
    return 0;
}

uint64_t Dlsym(Process* proc, void* handle, const char* name) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !name) return 0;
    if (handle == RTLD_DEFAULT) {
        SymHit h = lookup_global(pls, name);
        return h.sym ? h.lib->load_base + h.sym->st_value : 0;
    }
    if (handle == RTLD_NEXT) {
        // Walk libs after the caller's; we don't track caller here so
        // we treat NEXT == DEFAULT for now.
        SymHit h = lookup_global(pls, name);
        return h.sym ? h.lib->load_base + h.sym->st_value : 0;
    }
    Lib* l = (Lib*)handle;
    const Sym* s = lookup_in_lib(l, name);
    if (s && s->st_shndx) return l->load_base + s->st_value;
    // Recurse into deps.
    for (int i = 0; i < l->dep_count; i++) {
        Lib* d = &g_libs[l->deps[i]];
        const Sym* s2 = lookup_in_lib(d, name);
        if (s2 && s2->st_shndx) return d->load_base + s2->st_value;
    }
    return 0;
}

uint64_t Dlvsym(Process* proc, void* handle, const char* name,
                const char* version) {
    // Versioned lookup: find the symbol whose Verdaux name matches.
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !name) return 0;
    Lib* l = (Lib*)handle;
    if (!l || handle == RTLD_DEFAULT || handle == RTLD_NEXT) {
        return Dlsym(proc, handle, name);
    }
    uint32_t idx = 0;
    const Sym* s = lookup_in_lib(l, name, &idx);
    if (!s) return 0;
    if (!version || !l->versym || !l->verdef) {
        return s->st_shndx ? l->load_base + s->st_value : 0;
    }
    uint16_t v = l->versym[idx] & 0x7FFF;
    Verdef* vd = l->verdef;
    for (int i = 0; i < l->verdef_num && vd; i++) {
        if (vd->vd_ndx == v) {
            Verdaux* va = (Verdaux*)((uint8_t*)vd + vd->vd_aux);
            if (s_eq(l->strtab + va->vda_name, version))
                return s->st_shndx ? l->load_base + s->st_value : 0;
        }
        vd = vd->vd_next ? (Verdef*)((uint8_t*)vd + vd->vd_next) : nullptr;
    }
    return 0;
}

bool Dladdr(Process* proc, uint64_t addr, DlInfo* out) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !out) return false;
    out->dli_fname = nullptr;
    out->dli_fbase = 0;
    out->dli_sname = nullptr;
    out->dli_saddr = 0;
    Lib* match = nullptr;
    for (int i = 0; i < pls->lib_count; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        if (addr >= l->load_base && addr < l->load_base + l->load_size) {
            match = l; break;
        }
    }
    if (!match) return false;
    out->dli_fname = match->path;
    out->dli_fbase = match->load_base;
    // Find nearest symbol below `addr`.
    uint64_t best_va = 0;
    const Sym* best = nullptr;
    for (int i = 0; i < match->symcount; i++) {
        const Sym* s = &match->symtab[i];
        if (!s->st_name || !s->st_shndx) continue;
        uint64_t va = match->load_base + s->st_value;
        if (va <= addr && va > best_va) { best_va = va; best = s; }
    }
    if (best) {
        out->dli_sname = match->strtab + best->st_name;
        out->dli_saddr = best_va;
    }
    return true;
}

void DumpMaps(Process* proc, char* out, int max_len) {
    ProcLinkerState* pls = find_pls_by_proc(proc);
    if (!pls || !out || max_len <= 0) return;
    int p = 0;
    for (int i = 0; i < pls->lib_count && p < max_len - 80; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        // Format like /proc/PID/maps:  base-end r-xp 0 00:00 0  path
        const char* hex = "0123456789abcdef";
        auto hh = [&](uint64_t v) {
            for (int s = 60; s >= 0; s -= 4) {
                if (p < max_len - 1) out[p++] = hex[(v >> s) & 0xF];
            }
        };
        hh(l->load_base);
        if (p < max_len - 1) out[p++] = '-';
        hh(l->load_base + l->load_size);
        const char* trail = " r-xp 00000000 00:00 0   ";
        for (int k = 0; trail[k] && p < max_len - 1; k++) out[p++] = trail[k];
        for (int k = 0; l->path[k] && p < max_len - 1; k++) out[p++] = l->path[k];
        if (p < max_len - 1) out[p++] = '\n';
    }
    if (p < max_len) out[p] = 0;
}

// ============================================================
// ExecPIE  -  main entry from execve.
// ============================================================
bool ExecPIE(Process* proc,
             const uint8_t* image, uint64_t image_size,
             const char* path,
             const char* const* argv,
             const char* const* envp,
             uint32_t uid, uint32_t gid,
             uint64_t* out_entry,
             uint64_t* out_rsp) {
    ProcLinkerState* pls = alloc_pls(proc);
    if (!pls) return false;
    pls->secure = (uid == 0 || gid == 0) && (uid != 0 || gid != 0);

    // Parse env for LD_LIBRARY_PATH / LD_PRELOAD / LD_DEBUG.
    const char* ld_lib = nullptr;
    const char* ld_pre = nullptr;
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            if (s_starts(envp[i], "LD_LIBRARY_PATH=")) ld_lib = envp[i] + 16;
            else if (s_starts(envp[i], "LD_PRELOAD="))      ld_pre = envp[i] + 11;
            else if (s_starts(envp[i], "LD_DEBUG=")) {
                const char* v = envp[i] + 9;
                if (s_eq(v, "all"))      pls->debug_mask = DBG_ALL;
                else if (s_eq(v, "libs"))     pls->debug_mask = DBG_LIBS | DBG_FILES;
                else if (s_eq(v, "symbols"))  pls->debug_mask = DBG_SYMBOLS;
                else if (s_eq(v, "reloc"))    pls->debug_mask = DBG_RELOC;
                else if (s_eq(v, "files"))    pls->debug_mask = DBG_FILES;
                else if (s_eq(v, "versions")) pls->debug_mask = DBG_VERSIONS;
                else if (s_eq(v, "bindings")) pls->debug_mask = DBG_BINDINGS;
            }
            else if (s_starts(envp[i], "LD_VERBOSE="))  pls->verbose = true;
            else if (s_starts(envp[i], "LD_BIND_NOW=")) pls->bind_now = true;
        }
    }
    if (pls->secure) { ld_lib = nullptr; ld_pre = nullptr; }
    seed_searchpath(pls, ld_lib);

    // ---- Stash main image and load it as the first library --------
    constexpr uint32_t MAX = 32u * 1024u * 1024u;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)image_size);
    if (!buf) { set_error(pls, "OOM main image"); return false; }
    for (uint64_t i = 0; i < image_size; i++) buf[i] = image[i];

    if (!valid_elf64(buf, image_size)) {
        KernelHeap::Free(buf);
        set_error(pls, "main exec invalid");
        return false;
    }
    Ehdr* eh = (Ehdr*)buf;

    Lib* main_l = alloc_lib();
    if (!main_l) { KernelHeap::Free(buf); set_error(pls, "lib table full"); return false; }
    main_l->image = buf;
    main_l->image_size = image_size;
    main_l->phdrs = (Phdr*)(buf + eh->e_phoff);
    main_l->phnum = eh->e_phnum;
    main_l->phentsize = eh->e_phentsize;
    main_l->entry = eh->e_entry;
    main_l->is_main_exec = true;
    main_l->global = true;
    s_cpy(main_l->path, path, sizeof(main_l->path));
    {
        const char* b = path;
        for (const char* p = path; *p; p++) if (*p == '/') b = p + 1;
        s_cpy(main_l->soname, b, sizeof(main_l->soname));
    }
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < main_l->phnum; i++) {
        if (main_l->phdrs[i].p_type != PT_LOAD) continue;
        if (main_l->phdrs[i].p_vaddr < lo) lo = main_l->phdrs[i].p_vaddr;
        uint64_t e = main_l->phdrs[i].p_vaddr + main_l->phdrs[i].p_memsz;
        if (e > hi) hi = e;
    }
    if (lo == ~0ULL) lo = 0;
    main_l->load_base = (eh->e_type == ET_DYN) ? (pick_aslr_base(hi - lo) - lo) : 0;
    main_l->load_size = hi - lo;
    for (int i = 0; i < main_l->phnum; i++) {
        if (main_l->phdrs[i].p_type == PT_PHDR) {
            main_l->phdr_va = main_l->load_base + main_l->phdrs[i].p_vaddr;
        }
    }
    if (!main_l->phdr_va) main_l->phdr_va = main_l->load_base + eh->e_phoff;

    parse_dynamic(main_l);
    if (!map_lib_segments(proc, main_l, pls)) return false;
    pls->libs[pls->lib_count++] = lib_index(main_l);

    // ---- LD_PRELOAD ---------------------------------------------
    if (ld_pre) {
        const char* s = ld_pre;
        while (*s) {
            char nm[LDK_PATH_LEN]; int p = 0;
            while (*s && *s != ':' && *s != ' ' && p < (int)sizeof(nm) - 1)
                nm[p++] = *s++;
            nm[p] = 0;
            if (p) {
                bool a = false;
                load_library_recursive(proc, pls, nm, true, &a);
            }
            if (*s) s++;
        }
    }

    // ---- Load the program interpreter (ld-musl == libc.so) ----------
    // we do NOT replace musl's dynamic linker. we map the exe + the interp and
    // jump to the INTERP's entry; musl's _dlstart then self-relocates, loads the
    // exe's DT_NEEDED closure, relocates everything, sets up tls + its own dso
    // list, runs init, and tail-jumps to the exe. that is why we deliberately do
    // NOT process_dt_needed / apply_relocs / install_main_tls / run init here  - 
    // musl owns all of it, and its libc startup (do_init_fini etc.) crashes if
    // those globals were not built by its own linker. (satoru)
    const char* interp_path = nullptr;
    for (int i = 0; i < main_l->phnum; i++) {
        if (main_l->phdrs[i].p_type == PT_INTERP) {
            interp_path = (const char*)(main_l->image + main_l->phdrs[i].p_offset);
            break;
        }
    }
    Lib* interp = nullptr;
    {
        char resolved[LDK_PATH_LEN];
        // try the exe's PT_INTERP path verbatim, then fall back to the musl libc
        // soname in a default search path. load segments only (is_linker=true):
        // no dep recursion, no in-kernel relocation. (satoru)
        if (interp_path && resolve_lib_path(pls, interp_path, resolved, sizeof(resolved)))
            interp = do_load(proc, pls, resolved, false, true, true);
        else if (resolve_lib_path(pls, "libc.musl-x86_64.so.1", resolved, sizeof(resolved)))
            interp = do_load(proc, pls, resolved, false, true, true);
    }
    if (!interp) { set_error(pls, "interp load failed"); return false; }

    // log each module's load base (hi/lo, since LogHex is 32-bit) so a fault rip
    // from the exception dump can be mapped to a module + file offset for symbol
    // lookup while the dynamic path is being brought up. (satoru)
    for (int i = 0; i < pls->lib_count; i++) {
        Lib* l = &g_libs[pls->libs[i]];
        SerialLogger::Log("[ldso] module ");
        SerialLogger::Log(l->soname[0] ? l->soname : l->path);
        SerialLogger::Log(" base=");
        SerialLogger::LogHex((uint32_t)(l->load_base >> 32));
        SerialLogger::Log(":");
        SerialLogger::LogHex((uint32_t)(l->load_base & 0xFFFFFFFF));
        SerialLogger::Log("\r\n");
    }

    // ---- TLS / thread pointer: handled by musl's linker ----------
    // in interp mode musl's __init_tls allocates the tls block + sets the fs
    // base (via arch_prctl) itself, so we leave proc->fs_base = 0 and do NOT
    // install_main_tls here. (install_main_tls remains for a future
    // replace-the-linker mode but is unused on this path.) (satoru)

    // ---- Map vDSO ------------------------------------------------
    pls->vdso_va = MapVDSO(proc);

    // ---- Build user stack: argv/envp/auxv ------------------------
    // Caller has already mapped a USER_STACK_BYTES stack at proc->user_stack_top.
    uint64_t sp = proc->user_stack_top & ~0xFULL;

    // Push string table (envp first, then argv).
    int argc = 0;
    if (argv) while (argv[argc]) argc++;
    int envc = 0;
    if (envp) while (envp[envc]) envc++;
    constexpr int MAX_VECT = 256;
    uint64_t argv_addrs[MAX_VECT];
    uint64_t envp_addrs[MAX_VECT];
    if (argc > MAX_VECT) argc = MAX_VECT;
    if (envc > MAX_VECT) envc = MAX_VECT;

    for (int i = envc - 1; i >= 0; i--)
        envp_addrs[i] = push_string(proc, &sp, envp[i]);
    for (int i = argc - 1; i >= 0; i--)
        argv_addrs[i] = push_string(proc, &sp, argv[i]);

    uint64_t execfn_addr = push_string(proc, &sp, path);
    uint64_t platform_addr = push_string(proc, &sp, "x86_64");

    // 16 random bytes for AT_RANDOM.
    uint8_t rnd[16];
    for (int i = 0; i < 16; i += 8) {
        uint64_t r = aslr_rand();
        for (int b = 0; b < 8; b++) rnd[i + b] = (uint8_t)(r >> (b * 8));
    }
    sp -= 16;
    uint64_t random_addr = sp;
    {
        uint64_t page = sp & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t phys = KernelVMM::QueryMappingInAddressSpace(
            proc->address_space, page);
        if (phys) {
            uint8_t* d = (uint8_t*)(uintptr_t)(phys + (sp & (PAGE_SIZE - 1)));
            for (int i = 0; i < 16; i++) d[i] = rnd[i];
        }
    }

    // Align sp to 16 before pushing the auxv/envp/argv arrays.
    sp &= ~0xFULL;

    // Auxv (terminator first because we push downward).
    AuxEnt aux[] = {
        { AT_NULL,    0 },
        { AT_PHDR,    main_l->phdr_va },
        { AT_PHENT,   main_l->phentsize },
        { AT_PHNUM,   main_l->phnum },
        { AT_PAGESZ,  PAGE_SIZE },
        { AT_BASE,    interp->load_base },        // musl self-relocates from this
        { AT_FLAGS,   0 },
        { AT_ENTRY,   main_l->load_base + main_l->entry },
        { AT_UID,     uid },
        { AT_EUID,    uid },
        { AT_GID,     gid },
        { AT_EGID,    gid },
        { AT_SECURE,  pls->secure ? 1 : 0 },
        { AT_RANDOM,  random_addr },
        { AT_HWCAP,   0x0001f8bb },              // sse, sse2, mmx, fxsr, etc.
        { AT_HWCAP2,  0 },
        { AT_CLKTCK,  100 },
        { AT_PLATFORM,platform_addr },
        { AT_EXECFN,  execfn_addr },
        // do NOT advertise the vdso: the synthesised vdso page lacks a musl-
        // parseable PT_DYNAMIC, so musl's vdso decode walked a null dynv and
        // #pf'd in decode_vec. with AT_SYSINFO_EHDR=0 musl skips the vdso and
        // uses the real clock_gettime/gettimeofday syscalls. (satoru)
        { AT_SYSINFO_EHDR, 0 },
    };
    int auxn = sizeof(aux) / sizeof(aux[0]);
    for (int i = 0; i < auxn; i++) {
        push_qword(proc, &sp, aux[i].a_val);
        push_qword(proc, &sp, aux[i].a_type);
    }
    // envp NULL terminator + envp pointers.
    push_qword(proc, &sp, 0);
    for (int i = envc - 1; i >= 0; i--) push_qword(proc, &sp, envp_addrs[i]);
    // argv NULL terminator + argv pointers.
    push_qword(proc, &sp, 0);
    for (int i = argc - 1; i >= 0; i--) push_qword(proc, &sp, argv_addrs[i]);
    // argc.
    push_qword(proc, &sp, (uint64_t)argc);

    *out_rsp   = sp;
    // enter at the INTERP entry (musl's _dlstart), NOT the exe entry. musl
    // self-relocates, links the exe + its closure, sets up tls, runs every
    // init/ctor (do_init_fini), then tail-jumps to AT_ENTRY (the exe). so we do
    // NOT run init arrays or build an init trampoline here. (satoru)
    *out_entry = interp->load_base + interp->entry;

    DlDebugStateNotify();
    SerialLogger::Log("[ldso] ExecPIE complete: ");
    SerialLogger::Log(path);
    SerialLogger::Log("\r\n");
    return true;
}

}  // namespace LdKurono
