#include "pmm.h"
#include "../drivers/serial.h"

//  physical memory manager  -  bitmap frame allocator implementation

// static member definitions
uint64_t* PMM::bitmap       = nullptr;
uint16_t* PMM::refs         = nullptr;
uint64_t  PMM::bitmap_size  = 0;
uint64_t  PMM::total_frames = 0;
uint64_t  PMM::used_frames  = 0;
uint64_t  PMM::search_hint  = 0;

extern "C" uint8_t kernel_start;
extern "C" uint8_t kernel_end;       // after .boot_tables  -  safe to place bitmap here

void PMM::SetFrame(uint64_t idx) {
    bitmap[idx / 64] |= (1ULL << (idx % 64));
}

void PMM::ClearFrame(uint64_t idx) {
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

bool PMM::TestFrame(uint64_t idx) {
    return (bitmap[idx / 64] & (1ULL << (idx % 64))) != 0;
}

bool PMM::IsFrameUsed(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return true;
    return TestFrame(idx);
}

uint64_t PMM::FindFreeFrame() {
    // start from hint, wrap around once
    uint64_t qwords = bitmap_size;

    for (uint64_t pass = 0; pass < qwords; pass++) {
        uint64_t qi = (search_hint / 64 + pass) % qwords;
        if (bitmap[qi] == ~0ULL) continue;  // all 64 bits set → skip

        // find first zero bit in this qword
        uint64_t val = bitmap[qi];
        for (int bit = 0; bit < 64; bit++) {
            if (!(val & (1ULL << bit))) {
                uint64_t frame = qi * 64 + bit;
                if (frame < total_frames) {
                    search_hint = frame + 1;
                    return frame;
                }
            }
        }
    }
    return ~0ULL;  // out of memory
}

uint64_t PMM::AllocFrame() {
    uint64_t idx = FindFreeFrame();
    if (idx == ~0ULL) {
        SerialLogger::Log("PMM: OUT OF MEMORY!\r\n");
        return 0;
    }
    SetFrame(idx);
    refs[idx] = 1;
    used_frames++;
    return idx * PAGE_SIZE;
}

void PMM::RetainFrame(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;

    refs[idx]++;
}

uint32_t PMM::GetFrameRefCount(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return 0;
    return refs[idx];
}

void PMM::FreeFrame(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_frames) return;
    if (!TestFrame(idx)) return;  // double-free guard

    if (refs[idx] > 1) {
        refs[idx]--;
        return;
    }

    refs[idx] = 0;
    ClearFrame(idx);
    used_frames--;

    // move hint backward for better locality
    if (idx < search_hint) search_hint = idx;
}

static void bulk_set_frames(uint64_t start, uint64_t count,
                            uint64_t* bmp, uint64_t& used) {
    uint64_t end = start + count;
    uint64_t i = start;

    // align to qword boundary
    while (i < end && (i % 64) != 0) {
        bmp[i / 64] |= (1ULL << (i % 64));
        used++;
        i++;
    }
    // set full qwords
    while (i + 64 <= end) {
        bmp[i / 64] = ~0ULL;
        used += 64;
        i += 64;
    }
    // remaining bits
    while (i < end) {
        bmp[i / 64] |= (1ULL << (i % 64));
        used++;
        i++;
    }
}

uint64_t PMM::AllocContiguous(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return AllocFrame();

    // optimized scan: skip full qwords (64 frames at a time) where possible.
    // for large requests (>64 frames), first scan qword-at-a-time to find
    // candidate regions of all-zero qwords, then verify edges bit-by-bit.
    uint64_t qwords = bitmap_size;  // number of uint64_t entries

    // scan for runs of free frames
    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (uint64_t qi = 0; qi < qwords; qi++) {
        if (bitmap[qi] == ~0ULL) {
            // all 64 frames used  -  reset run
            run_start = (qi + 1) * 64;
            run_len = 0;
            continue;
        }
        if (bitmap[qi] == 0ULL) {
            // all 64 frames free  -  extend run by 64
            uint64_t frame_base = qi * 64;
            if (run_len == 0) run_start = frame_base;
            run_len += 64;
            if (run_len >= count) {
                // we have enough  -  mark and return
                bulk_set_frames(run_start, count, bitmap, used_frames);
                    for (uint64_t j = 0; j < count; j++) refs[run_start + j] = 1;
                search_hint = run_start + count;
                return run_start * PAGE_SIZE;
            }
            continue;
        }
        // partial qword  -  scan bit by bit
        for (int bit = 0; bit < 64; bit++) {
            uint64_t fi = qi * 64 + bit;
            if (fi >= total_frames) break;
            if (bitmap[qi] & (1ULL << bit)) {
                // used  -  reset run
                run_start = fi + 1;
                run_len = 0;
            } else {
                if (run_len == 0) run_start = fi;
                run_len++;
                if (run_len >= count) {
                    bulk_set_frames(run_start, count, bitmap, used_frames);
                    for (uint64_t j = 0; j < count; j++) refs[run_start + j] = 1;
                    search_hint = run_start + count;
                    return run_start * PAGE_SIZE;
                }
            }
        }
    }
    SerialLogger::Log("PMM: AllocContiguous failed (");
    SerialLogger::LogDec((uint32_t)count);
    SerialLogger::Log(" frames requested)\r\n");
    return 0;
}

void* PMM::AllocBytes(size_t bytes) {
    if (bytes == 0) return nullptr;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = AllocContiguous(pages);
    if (!phys) return nullptr;
    // zero the allocation (callers expect clean memory)
    memset((void*)(uintptr_t)phys, 0, pages * PAGE_SIZE);
    return (void*)(uintptr_t)phys;
}

void PMM::FreeBytes(void* addr, size_t bytes) {
    if (!addr || bytes == 0) return;
    uint64_t phys = (uint64_t)(uintptr_t)addr;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        FreeFrame(phys + i * PAGE_SIZE);
    }
}

static void mark_range_used(uint64_t base, uint64_t length) {
    uint64_t start_frame = base / PAGE_SIZE;
    uint64_t end_frame   = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (f < PMM::GetTotalFrames() && !PMM::IsFrameUsed(f * PAGE_SIZE)) {
            // we can't call setframe directly (private), so we use allocframe semantics
            // actually, let's just inline it  -  we're in the .cpp so we have access
        }
    }
    // direct access since we're in the class impl file:
    // we'll do it inline below in init()
    (void)base; (void)length;
}

void PMM::Init(multiboot_info_t* mbi) {
    SerialLogger::Log("PMM: Initializing physical memory manager...\r\n");

    uint64_t max_addr = 0;

    if (mbi->flags & (1 << 6)) {
        // bit 6: memory map is valid
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        uint64_t offset = mbi->mmap_addr;

        while (offset < mmap_end) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;
            uint64_t region_end = entry->base_addr + entry->length;
            if (region_end > max_addr) max_addr = region_end;
            offset += entry->size + 4;  // +4 because 'size' doesn't include itself
        }
    } else if (mbi->flags & (1 << 0)) {
        // bit 0: mem_lower + mem_upper (less accurate)
        max_addr = ((uint64_t)mbi->mem_upper + 1024) * 1024;
    } else {
        // fallback: assume 128 mb
        max_addr = 128ULL * 1024 * 1024;
    }

    // cap at 16 gb (our identity map in boot asm covers this)
    if (max_addr > 16ULL * 1024 * 1024 * 1024) {
        max_addr = 16ULL * 1024 * 1024 * 1024;
    }

    total_frames = max_addr / PAGE_SIZE;
    used_frames  = total_frames;  // start with everything "used"

    // each bit = 1 frame. we need total_frames bits = total_frames/8 bytes.
    uint64_t bitmap_bytes = (total_frames + 63) / 64 * 8;  // round up to qword
    bitmap_size = bitmap_bytes / 8;
    bitmap = (uint64_t*)(uintptr_t)( ((uint64_t)&kernel_end + 4095) & ~4095ULL );
    refs = (uint16_t*)(uintptr_t)(bitmap + bitmap_size);
    uint64_t refs_bytes = total_frames * sizeof(uint16_t);

    // initialize bitmap: all frames marked as used (all bits = 1)
    uint8_t* bp = (uint8_t*)bitmap;
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        bp[i] = 0xFF;
    }
    uint8_t* rp = (uint8_t*)refs;
    for (uint64_t i = 0; i < refs_bytes; i++) {
        rp[i] = 0;
    }

    SerialLogger::Log("PMM: Bitmap at 0x");
    SerialLogger::LogHex((uint64_t)bitmap);
    SerialLogger::Log(", ");
    SerialLogger::LogHex(bitmap_bytes);
    SerialLogger::Log(" bytes for ");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" frames\r\n");

    if (mbi->flags & (1 << 6)) {
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        uint64_t offset = mbi->mmap_addr;

        while (offset < mmap_end) {
            MultibootMmapEntry* entry = (MultibootMmapEntry*)(uintptr_t)offset;

            if (entry->type == 1) {
                // type 1 = available ram  -  mark frames as free
                uint64_t base = (entry->base_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); // align up
                uint64_t end  = (entry->base_addr + entry->length) & ~(PAGE_SIZE - 1); // align down

                for (uint64_t addr = base; addr < end; addr += PAGE_SIZE) {
                    uint64_t idx = addr / PAGE_SIZE;
                    if (idx < total_frames && TestFrame(idx)) {
                        ClearFrame(idx);
                        used_frames--;
                    }
                }
            }
            offset += entry->size + 4;
        }
    } else {
        // no memory map available (efi boot-services path, or very old bios).
        // assume all ram from 1 mb to max_addr is usable.  step 4 below will
        // re-reserve the kernel image, bitmap, first 1 mb, and framebuffer.
        SerialLogger::Log("PMM: No mmap  -  using fallback (1MB..max_addr free)\r\n");
        for (uint64_t addr = 0x100000; addr < max_addr; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && TestFrame(idx)) {
                ClearFrame(idx);
                used_frames--;
            }
        }
    }

    // 4a. first 1 mb: bios/ivt/vga/rom  -  never allocate
    for (uint64_t f = 0; f < (1024 * 1024) / PAGE_SIZE; f++) {
        if (!TestFrame(f)) {
            SetFrame(f);
            used_frames++;
        }
    }

    // 4b. kernel image: kernel_start..kernel_bss_end
    uint64_t kern_start_phys = (uint64_t)&kernel_start;
    uint64_t kern_end_phys   = (uint64_t)&kernel_end;
    for (uint64_t addr = kern_start_phys & ~(PAGE_SIZE - 1);
         addr < kern_end_phys; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }

    // 4c. the bitmap itself
    uint64_t bm_start = (uint64_t)bitmap;
    uint64_t bm_end   = bm_start + bitmap_bytes;
    uint64_t refs_start = (uint64_t)refs;
    uint64_t refs_end = refs_start + refs_bytes;
    for (uint64_t addr = bm_start & ~(PAGE_SIZE - 1);
         addr < bm_end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }
    for (uint64_t addr = refs_start & ~(PAGE_SIZE - 1);
         addr < refs_end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < total_frames && !TestFrame(idx)) {
            SetFrame(idx);
            used_frames++;
        }
    }

    // 4d. framebuffer region (if present)
    if ((mbi->flags & (1 << 12)) && mbi->framebuffer_addr != 0) {
        uint64_t fb_size = (uint64_t)mbi->framebuffer_pitch * mbi->framebuffer_height;
        uint64_t fb_base = mbi->framebuffer_addr & ~(PAGE_SIZE - 1);
        uint64_t fb_end  = mbi->framebuffer_addr + fb_size;
        for (uint64_t addr = fb_base; addr < fb_end; addr += PAGE_SIZE) {
            uint64_t idx = addr / PAGE_SIZE;
            if (idx < total_frames && !TestFrame(idx)) {
                SetFrame(idx);
                used_frames++;
            }
        }
    }

    for (uint64_t idx = 0; idx < total_frames; idx++) {
        refs[idx] = TestFrame(idx) ? 1 : 0;
    }

    uint64_t allocator_end = refs_end > bm_end ? refs_end : bm_end;
    search_hint = (allocator_end + PAGE_SIZE - 1) / PAGE_SIZE;

    SerialLogger::Log("PMM: Total=");
    SerialLogger::LogHex(total_frames);
    SerialLogger::Log(" Used=");
    SerialLogger::LogHex(used_frames);
    SerialLogger::Log(" Free=");
    SerialLogger::LogHex(total_frames - used_frames);
    SerialLogger::Log("\r\n");
}
