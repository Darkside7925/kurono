#pragma once
#include "types.h"

//  kurono os - slab allocator
//
//  Layered on top of Buddy.  Each kmem_cache is a fixed-size object pool
//  carved from one or more 4 KB or 8 KB "slabs" obtained via
//  Buddy::AllocPages().  Each slab carries an in-band header followed by
//  a bitmap of free slots and the object array proper.  The cache keeps
//  three slab lists: empty, partial, full.
//
//  16 generic caches (kmalloc-N) cover power-of-two sizes from 16 B
//  through 8 KB.  Subsystems may also call kmem_cache_create() for a
//  named pool, which is what task_struct, dentry, inode etc. use.
//
//  /proc/slabinfo and Slab::DumpInfo() are exported for observability.

#define SLAB_MIN_SHIFT      4           // 16 B smallest object
#define SLAB_MAX_SHIFT      13          // 8 KB largest power-of-two
#define SLAB_NUM_GENERIC    (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)
#define SLAB_PAGE_BYTES     4096
#define SLAB_NAME_MAX       24
#define SLAB_MAX_CACHES     128

struct SlabPage;
struct kmem_cache {
    char     name[SLAB_NAME_MAX];
    uint32_t object_size;       // padded to align
    uint32_t align;
    uint32_t objs_per_slab;
    uint32_t pages_per_slab;    // power-of-two
    uint32_t bitmap_words;      // 32-bit words

    SlabPage* empty;            // all free
    SlabPage* partial;          // some free, some used
    SlabPage* full;             // all used

    // counters
    uint64_t alloc_count;
    uint64_t free_count;
    uint32_t slab_count;        // total live slabs
    uint32_t total_objs;        // across all live slabs
    uint32_t in_use_objs;
};

class Slab {
public:
    static bool Init();
    static bool IsReady() { return ready; }

    // generic kmalloc-style: returns object from smallest fitting cache.
    static void* kmalloc(uint32_t size);
    static void  kfree(void* ptr);          // size inferred from header

    // create a named cache for a fixed object size.  Returns handle.
    // Caches persist for kernel lifetime.
    static kmem_cache* CacheCreate(const char* name, uint32_t object_size,
                                   uint32_t align);
    static void* CacheAlloc(kmem_cache* c);
    static void  CacheFree(kmem_cache* c, void* obj);

    // /proc/slabinfo formatter.  Linux header line first, one row per
    // cache: name <active_objs> <num_objs> <objsize> <objperslab>
    //        <pagesperslab> : tunables : slabdata <active> <num>.
    static int DumpProcInfo(char* out, int max_len);
    static int DumpInfo(char* out, int max_len);

    static int CacheCount() { return cache_count; }

private:
    friend struct kmem_cache;
    static SlabPage* AllocSlabFor(kmem_cache* c);
    static void      ReleaseSlab(kmem_cache* c, SlabPage* s);
    static void      MoveToList(kmem_cache* c, SlabPage* s, int from, int to);
    static int    BitmapClaim(uint32_t* bm, int total);
    static void   BitmapRelease(uint32_t* bm, int idx);
    static int    BitmapPopcount(const uint32_t* bm, int total);

    static bool ready;
    static kmem_cache  caches[SLAB_MAX_CACHES];
    static int         cache_count;
    static kmem_cache* generic[SLAB_NUM_GENERIC];
};
