//  kurono os  -  buddy physical-memory allocator implementation
#include "buddy.h"
#include "pmm.h"
#include "../drivers/serial.h"

#define BUDDY_MAGIC 0xBDD0

bool        Buddy::ready = false;
uint64_t    Buddy::pool_base = 0;
uint64_t    Buddy::pool_bytes = 0;
uint64_t    Buddy::used_bytes = 0;
BuddyBlock* Buddy::free_lists[BUDDY_ORDER_MAX + 1] = {nullptr};
uint32_t    Buddy::free_counts[BUDDY_ORDER_MAX + 1] = {0};
uint32_t    Buddy::split_count = 0;
uint32_t    Buddy::merge_count = 0;
uint32_t    Buddy::alloc_count = 0;
uint32_t    Buddy::free_count  = 0;

// helpers --------------------------------------------------------------------

static int log2_ceil_pages(uint64_t pages) {
    int o = 0;
    uint64_t v = 1;
    while (v < pages) { v <<= 1; o++; }
    return o;
}

bool Buddy::InPool(uint64_t addr) {
    return addr >= pool_base && addr < pool_base + pool_bytes;
}

BuddyBlock* Buddy::BuddyOf(BuddyBlock* b, int order) {
    uint64_t off = (uint64_t)b - pool_base;
    uint64_t buddy_off = off ^ ((uint64_t)BUDDY_PAGE_SIZE << order);
    if (buddy_off >= pool_bytes) return nullptr;
    return (BuddyBlock*)(pool_base + buddy_off);
}

void Buddy::PushFree(BuddyBlock* b, int order) {
    b->order  = (uint8_t)order;
    b->in_use = 0;
    b->magic  = BUDDY_MAGIC;

    // address-sorted insertion: keeps the freelist ordered so that buddies
    // are usually adjacent in list order, which makes coalesce-on-free
    // cache-friendly and helps the higher-order list stay clean.
    BuddyBlock* cur  = free_lists[order];
    BuddyBlock* prev = nullptr;
    while (cur && cur < b) { prev = cur; cur = cur->next; }

    b->prev = prev;
    b->next = cur;
    if (cur)  cur->prev = b;
    if (prev) prev->next = b;
    else      free_lists[order] = b;
    free_counts[order]++;
}

BuddyBlock* Buddy::PopFree(int order) {
    BuddyBlock* b = free_lists[order];
    if (!b) return nullptr;
    free_lists[order] = b->next;
    if (b->next) b->next->prev = nullptr;
    free_counts[order]--;
    b->next = b->prev = nullptr;
    return b;
}

void Buddy::Unlink(BuddyBlock* b, int order) {
    if (b->prev) b->prev->next = b->next;
    else         free_lists[order] = b->next;
    if (b->next) b->next->prev = b->prev;
    free_counts[order]--;
    b->next = b->prev = nullptr;
}

int Buddy::OrderForBytes(uint64_t bytes) {
    if (bytes == 0) return 0;
    uint64_t pages = (bytes + BUDDY_PAGE_SIZE - 1) / BUDDY_PAGE_SIZE;
    return log2_ceil_pages(pages);
}

// init -----------------------------------------------------------------------

bool Buddy::Init() {
    // Pool size capped to whatever PMM can give us in one contiguous chunk.
    uint64_t want_pages = BUDDY_POOL_BYTES / BUDDY_PAGE_SIZE;
    uint64_t base = 0;
    while (want_pages > 1024) {
        base = PMM::AllocContiguous(want_pages);
        if (base) break;
        want_pages /= 2;                       // back off
    }
    if (!base) {
        SerialLogger::Log("[Buddy] Init FAILED: no contiguous pool\r\n");
        return false;
    }

    pool_base  = base;
    pool_bytes = want_pages * BUDDY_PAGE_SIZE;

    // round pool_bytes down to next power-of-two-pages so we can populate
    // top-order blocks cleanly.
    uint64_t bytes_left = pool_bytes;
    uint64_t cur        = pool_base;
    while (bytes_left >= BUDDY_PAGE_SIZE) {
        // largest power-of-two block that fits AND aligns
        int order = BUDDY_ORDER_MAX;
        while (order > 0) {
            uint64_t bsz = (uint64_t)BUDDY_PAGE_SIZE << order;
            if (bsz <= bytes_left && (cur & (bsz - 1)) == 0) break;
            order--;
        }
        uint64_t bsz = (uint64_t)BUDDY_PAGE_SIZE << order;
        BuddyBlock* b = (BuddyBlock*)cur;
        PushFree(b, order);
        cur        += bsz;
        bytes_left -= bsz;
    }

    ready = true;
    char msg[96];
    int n = 0;
    auto put = [&](const char* s){ while (*s && n < 95) msg[n++] = *s++; };
    auto puti = [&](uint64_t v){
        char t[32]; int ti=0; if (v==0) t[ti++]='0';
        while(v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
        while(ti) msg[n++] = t[--ti];
    };
    put("[Buddy] pool="); puti(pool_bytes/(1024*1024)); put(" MB ready\r\n");
    msg[n] = 0;
    SerialLogger::Log(msg);
    return true;
}

// allocation -----------------------------------------------------------------

void* Buddy::AllocPages(int order) {
    if (!ready)            return nullptr;
    if (order < 0)         order = 0;
    // overflow guard: order beyond what fits in uint64_t pages
    if (order >= 52)       return nullptr;
    if (order > BUDDY_ORDER_MAX) {
        // larger than our pool's max block  -  fall through to PMM contiguous.
        uint64_t pages = 1ull << order;
        uint64_t addr  = PMM::AllocContiguous(pages);
        if (addr) used_bytes += pages * BUDDY_PAGE_SIZE;
        return (void*)(uintptr_t)addr;
    }

    int o = order;
    while (o <= BUDDY_ORDER_MAX && !free_lists[o]) o++;
    if (o > BUDDY_ORDER_MAX) return nullptr;

    BuddyBlock* b = PopFree(o);
    if (!b) return nullptr;

    while (o > order) {
        o--;
        uint64_t half_size = (uint64_t)BUDDY_PAGE_SIZE << o;
        BuddyBlock* second = (BuddyBlock*)((uint64_t)b + half_size);
        PushFree(second, o);
        split_count++;
    }

    b->order  = (uint8_t)order;
    b->in_use = 1;
    b->magic  = BUDDY_MAGIC;
    used_bytes += (uint64_t)BUDDY_PAGE_SIZE << order;
    alloc_count++;
    return (void*)b;
}

void Buddy::FreePages(void* ptr, int order) {
    if (!ptr) return;
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (!InPool(addr)) {
        if (order >= 0) {
            uint64_t pages = 1ull << order;
            PMM::FreeContiguous(addr, pages);
            used_bytes -= pages * BUDDY_PAGE_SIZE;
        }
        return;
    }
    if (order < 0 || order > BUDDY_ORDER_MAX) return;

    BuddyBlock* b = (BuddyBlock*)ptr;
    // double-free / corruption guard
    if (b->magic != BUDDY_MAGIC) return;
    if (!b->in_use)              return;
    if (b->order != (uint8_t)order) return;

    used_bytes -= (uint64_t)BUDDY_PAGE_SIZE << order;
    free_count++;

    b->in_use = 0;

    // coalesce upward
    while (order < BUDDY_ORDER_MAX) {
        BuddyBlock* buddy = BuddyOf(b, order);
        if (!buddy) break;
        // buddy must be in pool
        if ((uint64_t)buddy < pool_base ||
            (uint64_t)buddy >= pool_base + pool_bytes) break;
        if (buddy->magic != BUDDY_MAGIC) break;
        if (buddy->in_use) break;
        if (buddy->order != (uint8_t)order) break;

        Unlink(buddy, order);
        if (buddy < b) b = buddy;
        order++;
        merge_count++;
    }
    PushFree(b, order);
}

void* Buddy::AllocBytes(uint64_t bytes) {
    if (!bytes) return nullptr;
    int order = OrderForBytes(bytes);
    return AllocPages(order);
}

void Buddy::FreeBytes(void* ptr, uint64_t bytes) {
    if (!ptr || !bytes) return;
    int order = OrderForBytes(bytes);
    FreePages(ptr, order);
}

void* Buddy::AllocHugePage() {
    // 2 MB = 512 4 KB pages = order 9
    return AllocPages(9);
}

void Buddy::FreeHugePage(void* ptr) {
    FreePages(ptr, 9);
}

uint32_t Buddy::GetFreeCount(int order) {
    if (order < 0 || order > BUDDY_ORDER_MAX) return 0;
    return free_counts[order];
}

// /proc/buddyinfo: Node 0, zone DMA   12 5 3 1 0 0 0 0 0 0 0 0
int Buddy::DumpProcInfo(char* out, int max_len) {
    int p = 0;
    auto put = [&](const char* s){ while (*s && p < max_len-1) out[p++] = *s++; };
    auto puti = [&](uint32_t v){
        char t[16]; int ti=0; if (v==0) t[ti++]='0';
        while (v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
        while (ti) out[p++] = t[--ti];
    };
    put("Node 0, zone   Normal  ");
    for (int o = 0; o <= BUDDY_ORDER_MAX; o++) {
        puti(free_counts[o]);
        put(o == BUDDY_ORDER_MAX ? "" : " ");
    }
    put("\n");
    if (p < max_len) out[p] = 0;
    return p;
}

int Buddy::DumpInfo(char* out, int max_len) {
    int p = 0;
    auto put = [&](const char* s){ while (*s && p < max_len-1) out[p++] = *s++; };
    auto puti = [&](uint64_t v){
        char t[24]; int ti=0; if (v==0) t[ti++]='0';
        while (v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
        while (ti) out[p++] = t[--ti];
    };
    put("Buddy allocator\n  pool_bytes=");
    puti(pool_bytes);
    put("\n  used_bytes=");
    puti(used_bytes);
    put("\n  alloc=");        puti(alloc_count);
    put(" free=");            puti(free_count);
    put(" split=");           puti(split_count);
    put(" merge=");           puti(merge_count);
    put("\n  free per order: ");
    for (int o = 0; o <= BUDDY_ORDER_MAX; o++) {
        put("[");
        char ob[3]; int oi=0; int v=o; if (v==0) ob[oi++]='0';
        while(v){ ob[oi++]=(char)('0'+(v%10)); v/=10; }
        while(oi) out[p++]=ob[--oi];
        put("]=");
        puti(free_counts[o]);
        put(" ");
    }
    put("\n");
    if (p < max_len) out[p] = 0;
    return p;
}
