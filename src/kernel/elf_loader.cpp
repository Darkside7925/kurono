#include "elf_loader.h"

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../proc/scheduler.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../linux/ld_kurono.h"

namespace {

#pragma pack(push, 1)
struct Elf64_Ehdr {
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

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};
#pragma pack(pop)

constexpr uint32_t PT_LOAD     = 1;
constexpr uint32_t PT_DYNAMIC  = 2;
constexpr uint32_t PT_INTERP   = 3;
constexpr uint32_t PT_NOTE     = 4;
constexpr uint32_t PT_PHDR     = 6;
constexpr uint32_t PT_TLS      = 7;
constexpr uint32_t PT_GNU_RELRO = 0x6474e552;
constexpr uint32_t PT_GNU_STACK = 0x6474e551;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN  = 3;
constexpr uint16_t EM_X86_64 = 62;

// Dynamic table entries
constexpr int64_t DT_NULL    = 0;
constexpr int64_t DT_RELA    = 7;
constexpr int64_t DT_RELASZ  = 8;
constexpr int64_t DT_RELAENT = 9;
constexpr int64_t DT_REL     = 17;
constexpr int64_t DT_RELSZ   = 18;
constexpr int64_t DT_RELENT  = 19;

// Relocation types (x86_64)
constexpr uint32_t R_X86_64_NONE     = 0;
constexpr uint32_t R_X86_64_64       = 1;
constexpr uint32_t R_X86_64_RELATIVE = 8;

#pragma pack(push, 1)
struct Elf64_Dyn {
    int64_t d_tag;
    uint64_t d_un;
};
struct Elf64_Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};
struct Elf64_Rel {
    uint64_t r_offset;
    uint64_t r_info;
};
#pragma pack(pop)
static inline uint32_t ELF64_R_TYPE(uint64_t i) { return (uint32_t)(i & 0xFFFFFFFFu); }

constexpr uint32_t PF_X = 1;
constexpr uint32_t PF_W = 2;
// PF_R unused  -  read is implicit.

static void log(const char* s) { SerialLogger::Log(s); }
static void logh(const char* s, uint64_t v) {
    SerialLogger::Log(s);
    SerialLogger::LogHex((uint32_t)(v >> 32));
    SerialLogger::LogHex((uint32_t)(v & 0xFFFFFFFFu));
    SerialLogger::Log("\r\n");
}

static bool valid_elf64(const uint8_t* d, uint64_t sz) {
    if (sz < sizeof(Elf64_Ehdr)) return false;
    if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') return false;
    if (d[4] != 2) return false;            // EI_CLASS = ELFCLASS64
    if (d[5] != 1) return false;            // EI_DATA  = ELFDATA2LSB
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)d;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return false;
    if (eh->e_machine != EM_X86_64) return false;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return false;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > sz) return false;
    return true;
}

// Map a single page into the process address space.  Allocates a fresh
// physical page, copies up to `copy_len` bytes from `src` into the page,
// zero-fills the rest, and maps it at `va` with PTE_USER|PTE_WRITABLE.
// (NX is left disabled for now  -  we treat all user pages as RWX while
// the loader stabilises.  Per-segment NX/RO can be added later by
// honouring p_flags.)
static bool map_one_page(Process* proc, uint64_t va,
                         const uint8_t* src, uint64_t copy_len,
                         uint64_t in_page_off) {
    void* pg = PMM::AllocBytes(PAGE_SIZE);
    if (!pg) return false;
    memset(pg, 0, PAGE_SIZE);
    if (copy_len > 0 && src) {
        memcpy((uint8_t*)pg + in_page_off, src, (size_t)copy_len);
    }
    if (!KernelVMM::MapPageInAddressSpace(proc->address_space, va,
                                          (uint64_t)(uintptr_t)pg,
                                          PTE_USER | PTE_WRITABLE)) {
        PMM::FreeBytes(pg, PAGE_SIZE);
        return false;
    }
    return true;
}

}  // namespace

Process* ElfLoader::LoadELF64(const uint8_t* data, uint64_t size, const char* name) {
    if (!valid_elf64(data, size)) {
        log("[ELF] invalid ELF64 header\r\n");
        return nullptr;
    }
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)data;
    logh("[ELF] entry  = ", eh->e_entry);
    logh("[ELF] phnum  = ", eh->e_phnum);

    // ---- PT_INTERP detection: hand off to the in-kernel dynamic linker.
    {
        const Elf64_Phdr* ph_chk = (const Elf64_Phdr*)(data + eh->e_phoff);
        bool has_interp = false;
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (ph_chk[i].p_type == PT_INTERP) { has_interp = true; break; }
        }
        if (has_interp) {
            log("[ELF] PT_INTERP found  -  routing through ld-kurono\r\n");
            Process* proc = Scheduler::CreateUserProcess(
                name ? name : "userelf", eh->e_entry, 1);
            if (!proc) {
                log("[ELF] CreateUserProcess failed\r\n");
                return nullptr;
            }
            const char* argv[] = { name ? name : "prog", nullptr };
            const char* envp[] = {
                "PATH=/system/bin:/apps/bin",
                "HOME=/home/user",
                "LD_LIBRARY_PATH=/system/lib:/system/lib/x86_64-linux-gnu",
                nullptr
            };
            uint64_t entry = 0, rsp = 0;
            if (!LdKurono::ExecPIE(proc, data, size,
                                   name ? name : "prog",
                                   argv, envp, 0, 0, &entry, &rsp)) {
                log("[ELF] LdKurono::ExecPIE failed\r\n");
                Scheduler::DestroyProcess(proc);
                return nullptr;
            }
            proc->rip = entry;
            proc->rsp = rsp;
            proc->user_frame.rip = entry;
            proc->user_frame.rsp = rsp;
            // ld-kurono already built the sysv stack (argv/envp/auxv) at rsp;
            // tell the runner to enter at rsp as-is, not rebuild it. (satoru)
            proc->flags |= PROCESS_FLAG_STACK_READY;
            log("[ELF] dynamic exec ready via ld-kurono\r\n");
            return proc;
        }
    }

    // For ET_DYN binaries we need to apply RELATIVE relocations before
    // mapping pages into user space (since user pages are reached via
    // the user CR3 and can't be written-through from kernel context).
    // The cleanest way to do this is to make a writeable scratch copy
    // of the ELF image, mutate it in place, then load from the copy.
    uint8_t* scratch = nullptr;
    const uint8_t* src_data = data;
    if (eh->e_type == ET_DYN) {
        scratch = (uint8_t*)KernelHeap::Alloc((uint32_t)size);
        if (scratch) {
            memcpy(scratch, data, (size_t)size);
            src_data = scratch;

            // Find PT_DYNAMIC and apply RELATIVE relocations.
            const Elf64_Phdr* ph0 = (const Elf64_Phdr*)(scratch + eh->e_phoff);
            const Elf64_Phdr* dynph = nullptr;
            for (uint16_t i = 0; i < eh->e_phnum; i++) {
                if (ph0[i].p_type == PT_DYNAMIC) { dynph = &ph0[i]; break; }
                if (ph0[i].p_type == PT_INTERP) {
                    log("[ELF] PT_INTERP present (dynamic linker required)  -  proceeding anyway\r\n");
                }
                if (ph0[i].p_type == PT_TLS) {
                    logh("[ELF] PT_TLS memsz = ", ph0[i].p_memsz);
                }
            }
            // reject any pt_load whose file range overruns the scratch buffer
            // before we translate vaddrs through it  -  otherwise vaddr_to_scratch
            // could hand back a pointer past the end of `scratch`. (satoru)
            bool segments_ok = true;
            for (uint16_t j = 0; j < eh->e_phnum; j++) {
                if (ph0[j].p_type != PT_LOAD) continue;
                if (ph0[j].p_offset > size ||
                    ph0[j].p_filesz > size - ph0[j].p_offset) {
                    log("[ELF] PT_LOAD file range overruns image  -  skipping relocs\r\n");
                    segments_ok = false;
                    break;
                }
            }
            if (dynph && segments_ok) {
                // Helper to translate a virtual address (vaddr-space, base 0)
                // back to a file-offset pointer in the scratch image, by
                // searching PT_LOAD segments. `need` is the access width (in
                // bytes) the caller will touch at the returned pointer; we only
                // return a pointer when the whole [vaddr, vaddr+need) window is
                // backed by file bytes inside this segment AND lands inside the
                // scratch buffer, so a reloc near a segment/EOF boundary can't
                // write past it (the caller stores a full uint64_t). (satoru)
                auto vaddr_to_scratch = [&](uint64_t vaddr, uint64_t need) -> uint8_t* {
                    for (uint16_t j = 0; j < eh->e_phnum; j++) {
                        if (ph0[j].p_type != PT_LOAD) continue;
                        if (vaddr < ph0[j].p_vaddr) continue;
                        // [vaddr, vaddr+need) must lie within this segment's
                        // file-backed bytes: vaddr + need <= p_vaddr + p_filesz.
                        uint64_t off = vaddr - ph0[j].p_vaddr;
                        if (off > ph0[j].p_filesz || need > ph0[j].p_filesz - off) {
                            continue;
                        }
                        // ...and within the scratch buffer:
                        // p_offset + off + need <= size. (segments_ok already
                        // guarantees p_offset + p_filesz <= size, so this holds,
                        // but check explicitly for defence in depth.) (satoru)
                        uint64_t file_off = ph0[j].p_offset + off;
                        if (file_off > size || need > size - file_off) continue;
                        return scratch + file_off;
                    }
                    return nullptr;
                };

                const Elf64_Dyn* dyn = (const Elf64_Dyn*)(scratch + dynph->p_offset);
                uint64_t rela_va = 0, rela_sz = 0, rela_ent = sizeof(Elf64_Rela);
                for (; dyn->d_tag != DT_NULL; dyn++) {
                    switch (dyn->d_tag) {
                        case DT_RELA:    rela_va  = dyn->d_un; break;
                        case DT_RELASZ:  rela_sz  = dyn->d_un; break;
                        case DT_RELAENT: rela_ent = dyn->d_un; break;
                        default: break;
                    }
                }
                if (rela_va && rela_sz && rela_ent >= sizeof(Elf64_Rela)) {
                    // require the WHOLE rela table to be file-backed + in-bounds
                    // so every rela_p + k*rela_ent read below is valid. (satoru)
                    uint8_t* rela_p = vaddr_to_scratch(rela_va, rela_sz);
                    if (rela_p) {
                        uint64_t count = rela_sz / rela_ent;
                        uint64_t applied = 0;
                        const uint64_t base = 0;  // we load ET_DYN at base 0
                        for (uint64_t k = 0; k < count; k++) {
                            const Elf64_Rela* r = (const Elf64_Rela*)(rela_p + k * rela_ent);
                            if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
                                // we store a full uint64_t at tgt  -  demand 8
                                // file-backed bytes so a reloc whose r_offset
                                // lands in the last 1-7 bytes of a segment is
                                // rejected instead of writing oob. (satoru)
                                uint8_t* tgt = vaddr_to_scratch(r->r_offset, 8);
                                if (tgt) {
                                    *(uint64_t*)tgt = base + (uint64_t)r->r_addend;
                                    applied++;
                                }
                            }
                        }
                        logh("[ELF] R_X86_64_RELATIVE applied = ", applied);
                    }
                }
            }
        }
    }

    Process* proc = Scheduler::CreateUserProcess(name ? name : "userelf",
                                                 eh->e_entry, 1);
    if (!proc) {
        log("[ELF] CreateUserProcess failed\r\n");
        if (scratch) KernelHeap::Free(scratch);
        return nullptr;
    }

    const Elf64_Phdr* ph = (const Elf64_Phdr*)(src_data + eh->e_phoff);

    // record program-header location in user space for the sysv auxv. prefer an
    // explicit pt_phdr; otherwise derive it from the pt_load that maps file
    // offset 0 (which covers the elf header + phdrs). musl reads at_phdr to
    // locate pt_tls / pt_gnu_relro during startup. (satoru)
    proc->user_phnum = eh->e_phnum;
    proc->user_phent = eh->e_phentsize;
    proc->user_phdr_va = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_PHDR) { proc->user_phdr_va = ph[i].p_vaddr; break; }
    }
    if (proc->user_phdr_va == 0) {
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                proc->user_phdr_va = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_GNU_STACK || ph[i].p_type == PT_NOTE ||
            ph[i].p_type == PT_PHDR     || ph[i].p_type == PT_GNU_RELRO ||
            ph[i].p_type == PT_INTERP   || ph[i].p_type == PT_DYNAMIC ||
            ph[i].p_type == PT_TLS) {
            continue;  // metadata segments  -  no mapping needed here
        }
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) {
            log("[ELF] segment overruns file\r\n");
            Scheduler::DestroyProcess(proc);
            if (scratch) KernelHeap::Free(scratch);
            return nullptr;
        }

        uint64_t va_start = ph[i].p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t va_end   = (ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1)
                            & ~(uint64_t)(PAGE_SIZE - 1);
        logh("[ELF] PT_LOAD vaddr = ", ph[i].p_vaddr);
        logh("[ELF] PT_LOAD memsz = ", ph[i].p_memsz);
        logh("[ELF] PT_LOAD flags = ", ph[i].p_flags);

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            // Skip pages that overlap the user stack we already mapped.
            // proc->user_stack_top is the top-of-stack pointer (minus a
            // small redzone).  Any *other* existing mapping (notably the
            // kernel's identity map of low memory) must be overwritten
            // with a PTE_USER entry  -  otherwise ring-3 will #PF on
            // user/supervisor mismatch.
            uint64_t stack_top  = (proc->user_stack_top + 16 + PAGE_SIZE - 1)
                                  & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t stack_base = stack_top - 8 * 1024 * 1024;  // mirrors USER_STACK_BYTES (satoru)
            if (va >= stack_base && va < stack_top) {
                continue;
            }

            // Compute slice of file data that lands in this page.
            uint64_t in_page_off = 0;
            const uint8_t* src   = nullptr;
            uint64_t copy_len    = 0;

            uint64_t page_lo = va;
            uint64_t page_hi = va + PAGE_SIZE;
            uint64_t file_lo = ph[i].p_vaddr;
            uint64_t file_hi = ph[i].p_vaddr + ph[i].p_filesz;

            uint64_t lo = (page_lo > file_lo) ? page_lo : file_lo;
            uint64_t hi = (page_hi < file_hi) ? page_hi : file_hi;
            if (lo < hi) {
                in_page_off = lo - page_lo;
                copy_len    = hi - lo;
                src         = src_data + ph[i].p_offset + (lo - file_lo);
            }
            if (!map_one_page(proc, va, src, copy_len, in_page_off)) {
                log("[ELF] map_one_page failed\r\n");
                Scheduler::DestroyProcess(proc);
                if (scratch) KernelHeap::Free(scratch);
                return nullptr;
            }
        }
    }

    if (scratch) KernelHeap::Free(scratch);
    return proc;
}

Process* ElfLoader::LoadELF64FromVFS(const char* path, const char* name) {
    if (!path) return nullptr;
    constexpr uint32_t MAX_USER_ELF = 64u * 1024u * 1024u;  // 64 mb cap (large static binaries e.g. ffmpeg) (satoru)
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(MAX_USER_ELF);
    if (!buf) return nullptr;
    int got = KVFS::ReadFile(path, buf, MAX_USER_ELF);
    if (got <= 0) {
        log("[ELF] KVFS::ReadFile failed for ");
        log(path);
        log("\r\n");
        KernelHeap::Free(buf);
        return nullptr;
    }
    Process* proc = LoadELF64(buf, (uint64_t)got, name);
    KernelHeap::Free(buf);
    return proc;
}
