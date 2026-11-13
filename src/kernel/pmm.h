#pragma once
#include "types.h"
#include "multiboot.h"

//  physical memory manager - bitmap-based frame allocator
//
//  manages physical memory in 4 kb frames using a bitmap. each bit in the
//  bitmap represents one 4 kb frame: 0 = free, 1 = used.
//
//  initialization:
//    1. parse the multiboot 1 memory map (mmap_addr / mmap_length)
//    2. mark all frames as used by default
//    3. free frames that the bios/grub says are available
//    4. re-mark the kernel image, framebuffer, and bitmap itself as used

#define PAGE_SIZE  4096ULL

// multiboot 1 memory map entry (variable-size, size field is pre-pended)
struct MultibootMmapEntry {
    uint32_t size;      // size of entry minus this field (usually 20)
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;      // 1 = available, everything else = reserved
} __attribute__((packed));

class PMM {
public:
    // initialize from multiboot info. must be called very early.
    static void Init(multiboot_info_t* mbi);

    // allocate a single 4 kb physical frame. returns physical address, or 0 on failure.
    static uint64_t AllocFrame();
    // claim one specific free frame (the fixed user image backs page va with the
    // frame at the same physical address, so the identity alias in that process
    // is the page itself). false if the frame is taken. (satoru)
    static bool AllocFrameAt(uint64_t phys_addr);

    // increment the reference count of an allocated frame.
    static void RetainFrame(uint64_t phys_addr);
    // task 26 free-while-mapped guard: vmm leaf writers mark frames that have a
    // live USER pte; AllocFrame refuses to re-hand such a frame ([DBLMAP]). (satoru)
    static void MarkFrameMapped(uint64_t phys_addr, bool m);
    static bool IsFrameMapped(uint64_t phys_addr);

    // inspect the current reference count of a frame.
    static uint32_t GetFrameRefCount(uint64_t phys_addr);

    // free a single 4 kb physical frame.
    static void FreeFrame(uint64_t phys_addr);

    // allocate n contiguous frames. returns base physical address, or 0 on failure.
    static uint64_t AllocContiguous(uint64_t count);

    // convenience: allocate/free a byte-sized buffer using contiguous frames.
    // uses identity mapping (phys == virt) so the returned pointer is directly usable.
    static void* AllocBytes(size_t bytes);
    static void  FreeBytes(void* addr, size_t bytes);

    // ── high-half direct map (kurono.directmap=1) ───────────────────────────
    // when enabled, AllocBytes/AllocBytesBelow return DIRECT_BASE + phys and
    // the kernel no longer dereferences ordinary frames through the low
    // identity map (page tables, zero-on-alloc and the heap all translate).
    // AllocFrame/AllocContiguous still return physical frame numbers.
    static bool     DirectMapRequested();
    static bool     DirectMapOn();
    static void     EnableDirectMap();
    static void*    PhysToVirt(uint64_t phys);
    static uint64_t VirtToPhys(uint64_t addr);
    static uint64_t VirtToPhys(const void* ptr) { return VirtToPhys((uint64_t)(uintptr_t)ptr); }

    // free a contiguous range of frames previously obtained via AllocContiguous.
    static void  FreeContiguous(uint64_t phys_addr, uint64_t count);

    // contiguous frames that all lie below max_phys, for devices with a narrow
    // dma address width (iwlwifi rings take 36-bit addresses). first-fit from
    // frame 0. free with FreeContiguous / FreeBytes as usual. (satoru)
    static uint64_t AllocContiguousBelow(uint64_t count, uint64_t max_phys);
    static void*    AllocBytesBelow(size_t bytes, uint64_t max_phys);

    // ── managed range above the old 16 gb cap ───────────────────────────────
    // the pmm used to stop at 16 gb. it now manages ram up to the top of the
    // boot identity map (512 gb of 2 mb pages, kurono_boot.asm NUM_PDS), or to
    // kurono.memcap=N gb when that is set (kurono.memcap=16 = the old cap).
    // every user address space still carries the identity map in pml4[0], so a
    // user va inside [16 gb, ram top) would shadow a frame the kernel can now
    // own; the vmm refuses those maps unless the va is a fixed overlay window
    // whose frames are reserved here. (satoru)
    static constexpr uint64_t LEGACY_CAP_BYTES     = 16ULL << 30;
    static constexpr uint64_t IDENTITY_CEIL_BYTES  = 512ULL << 30;
    static uint64_t LegacyCapBytes()  { return LEGACY_CAP_BYTES; }
    // true if [lo, hi) sits inside one fixed user overlay window (main stack,
    // ld-kurono init trampoline). those frames are never handed out. (satoru)
    static bool     IsUserOverlayRange(uint64_t lo, uint64_t hi);
    // used bitmap frames at or above phys (holes count as used: compare deltas). (satoru)
    static uint64_t CountUsedFramesFrom(uint64_t phys);
    // frames at or above 16 gb handed out right now / peak since ResetHighPeak.
    // 0 on a machine whose ram ends at or below 16 gb. test reporting. (satoru)
    // kurono.memcensus=1 leak census: per-frame allocation-site tags (the
    // allocator caller's return address). CensusSite is 0 when off or untagged;
    // reads are unlocked (the census tolerates a racy snapshot). (satoru)
    static bool     CensusOn();
    // kurono.ptaudit=1: paging-structure frame lifetime audit (debug). (satoru)
    static bool     PtAudit();
    static bool     PtTrace();   // per-frame leaf/table event history (debug) (satoru)
    static void     SetFreeGuard(bool (*fn)(uint64_t phys, uint64_t ra));
    static void     SetBulkFreeGuard(bool (*fn)(uint64_t phys));   // kurono.ptaudit (satoru)
    static uint32_t CensusSite(uint64_t idx);
    static bool     CensusFrameUsed(uint64_t idx);
    static uint64_t HighFramesInUse();
    static uint64_t HighFramesPeak();
    static void     ResetHighPeak();

    // statistics
    // total/used count usable ram only. the bitmap also spans the holes
    // between usable regions (pci hole, acpi, bios); those frames are pinned
    // as used forever and are left out here, so a 512mb machine reports
    // ~512mb instead of the bitmap range. (satoru)
    static uint64_t GetTotalFrames()  { return usable_frames; }
    static uint64_t GetUsedFrames()   { return used_frames - (total_frames - usable_frames); }
    static uint64_t GetFreeFrames()   { return total_frames - used_frames; }
    static uint64_t GetTotalMemory()  { return usable_frames * PAGE_SIZE; }
    // top of ram (bitmap range). bound for identity-mapped frames, not GetTotalMemory. (satoru)
    static uint64_t GetPhysTop()      { return total_frames * PAGE_SIZE; }
    static uint64_t GetFreeMemory()   { return GetFreeFrames() * PAGE_SIZE; }
    // boot reservations the census reports: the mbi framebuffer span, the fixed
    // user-image window below a high-linked kernel, and the overlay windows. all
    // accounting only. (satoru)
    static uint64_t GetReservedFramebufferBytes();
    static uint64_t GetImageWindowBytes();
    static uint64_t GetOverlayBytes();
    // a fixed-address user image (claude) can be launched at any time only if
    // the whole low window is reserved. when no such image is installed on the
    // disk, boot releases the part above 64 mb (busybox/musl tools need only
    // 1..64 mb) so the frames are usable ram. these two calls are the only way
    // the window changes after Init; both refuse to touch a frame that is
    // already allocated. (satoru)
    static bool ReleaseImageWindowAbove(uint64_t keep_bytes);
    // drop the boot-time physical reservation of the window. pages are claimed
    // one at a time by AllocFrameAt when a fixed image is mapped. (satoru)
    static bool ReleaseImageWindow();
    // fixed identity page(s) the panic minidump is written to. chosen once in
    // Init from the managed memory map; panic.h and the reservation both use
    // this so they can never disagree. (satoru)
    static uint64_t MinidumpSlot();
    // end-of-boot policy: keep the boot reservation or release it to demand
    // backing. claude_present is ClaudeHost::Installed(). returns true if the
    // window stayed reserved. (satoru)
    static bool FinalizeImageWindow(bool claude_present);
    static bool TryReserveImageWindow();
    static bool ImageWindowFullyReserved();
    static bool IsInImageWindow(uint64_t va);
    static uint64_t ImageWindowHi();
    // a frame whose identity range the kernel reads for its whole life (kernel
    // image, boot stub, panic dump). a user mapping may shadow it. (satoru)
    static bool ImageSafePhys(uint64_t pa);

    // test whether a frame is allocated
    static bool IsFrameUsed(uint64_t phys_addr);

    // helpers for the contiguous-run scan (kept public so the file-static scan
    // can share the bitmap without duplicating allocator state). (satoru)
    static uint64_t GetTotalFrameCount();
    static uint64_t GetBitmapQwords();
    static uint64_t GetBitmapQword(uint64_t i);
    static void CommitContiguous(uint64_t run_start, uint64_t count);

private:
    static void SetFrame(uint64_t frame_index);
    static void ClearFrame(uint64_t frame_index);
    static bool TestFrame(uint64_t frame_index);

    // find first free frame starting from hint. returns frame index, or ~0ull on failure.
    static uint64_t FindFreeFrame();

    static uint64_t* bitmap;          // bitmap array (1 bit per frame)
    static uint16_t* refs;            // reference count per frame
    static uint8_t*  mapped_bits;     // task 26: 1 bit/frame, live user pte (satoru)
    static uint64_t  bitmap_size;     // number of uint64_t entries in bitmap
    static uint64_t  total_frames;    // bitmap range (highest usable frame + 1)
    static uint64_t  usable_frames;   // frames the firmware reported as usable ram
    static uint64_t  used_frames;
    static uint64_t  search_hint;     // next-fit starting index
};
