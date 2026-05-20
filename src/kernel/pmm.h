#pragma once
#include "types.h"
#include "multiboot.h"

//  physical memory manager  -  bitmap-based frame allocator
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

    // increment the reference count of an allocated frame.
    static void RetainFrame(uint64_t phys_addr);

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

    // statistics
    static uint64_t GetTotalFrames()  { return total_frames; }
    static uint64_t GetUsedFrames()   { return used_frames; }
    static uint64_t GetFreeFrames()   { return total_frames - used_frames; }
    static uint64_t GetTotalMemory()  { return total_frames * PAGE_SIZE; }
    static uint64_t GetFreeMemory()   { return GetFreeFrames() * PAGE_SIZE; }

    // test whether a frame is allocated
    static bool IsFrameUsed(uint64_t phys_addr);

private:
    static void SetFrame(uint64_t frame_index);
    static void ClearFrame(uint64_t frame_index);
    static bool TestFrame(uint64_t frame_index);

    // find first free frame starting from hint. returns frame index, or ~0ull on failure.
    static uint64_t FindFreeFrame();

    static uint64_t* bitmap;          // bitmap array (1 bit per frame)
    static uint16_t* refs;            // reference count per frame
    static uint64_t  bitmap_size;     // number of uint64_t entries in bitmap
    static uint64_t  total_frames;
    static uint64_t  used_frames;
    static uint64_t  search_hint;     // next-fit starting index
};
