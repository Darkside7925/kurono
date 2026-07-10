//  kurono os - slab allocator implementation
#include "slab.h"
#include "buddy.h"
#include "panic.h"
#include "../drivers/serial.h"

#define SLAB_MAGIC 0x51AB1234u

struct SlabPage {
    uint32_t   magic;
    kmem_cache* parent;
    SlabPage*  next;
    SlabPage*  prev;
    int        list;            // 0=empty 1=partial 2=full
    uint32_t   in_use;
    uint32_t   total;
    uint32_t   bitmap[1];       // variable-length, followed by objects
};

bool        Slab::ready = false;
kmem_cache  Slab::caches[SLAB_MAX_CACHES];
int         Slab::cache_count = 0;
kmem_cache* Slab::generic[SLAB_NUM_GENERIC] = {nullptr};

// helpers --------------------------------------------------------------------

static void scpy(char* d, const char* s, int mx){
    int i = 0;
    if (s) while (s[i] && i < mx-1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static uint32_t round_up_pow2(uint32_t v){
    if (v <= 1) return 1;
    uint32_t r = 1; while (r < v) r <<= 1; return r;
}
__attribute__((unused)) static auto _slab_keep_round_up_pow2 = &round_up_pow2;

int Slab::BitmapPopcount(const uint32_t* bm, int total){
    int n = 0;
    int words = (total + 31) / 32;
    for (int i = 0; i < words; i++){
        uint32_t v = bm[i];
        while (v){ n += (int)(v & 1u); v >>= 1; }
    }
    return n;
}

int Slab::BitmapClaim(uint32_t* bm, int total){
    int words = (total + 31) / 32;
    for (int w = 0; w < words; w++){
        uint32_t v = ~bm[w];                // 1 = free
        if (!v) continue;
        int b;
#if defined(__GNUC__) || defined(__clang__)
        b = __builtin_ctz(v);
#else
        b = 0;
        uint32_t t = v;
        while (!(t & 1u)){ t >>= 1; b++; }
#endif
        // mask off bits past `total` in the last word so we don't claim slots
        // that don't exist (the bitmap is rounded up to a whole 32-bit word).
        int idx = w * 32 + b;
        if (idx >= total) continue;
        bm[w] |= (1u << b);
        return idx;
    }
    return -1;
}

void Slab::BitmapRelease(uint32_t* bm, int idx){
    bm[idx >> 5] &= ~(1u << (idx & 31));
}

void Slab::MoveToList(kmem_cache* c, SlabPage* s, int from, int to){
    SlabPage** lists[3] = { &c->empty, &c->partial, &c->full };
    // unlink from `from`
    if (s->prev) s->prev->next = s->next; else *lists[from] = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = nullptr;
    // push to head of `to`
    s->list = to;
    s->next = *lists[to];
    if (*lists[to]) (*lists[to])->prev = s;
    *lists[to] = s;
}

// init -----------------------------------------------------------------------

bool Slab::Init(){
    if (ready) return true;
    if (!Buddy::IsReady()) return false;

    cache_count = 0;
    for (int i = 0; i < SLAB_MAX_CACHES; i++){
        caches[i].name[0] = 0;
        caches[i].object_size = 0;
        caches[i].empty = caches[i].partial = caches[i].full = nullptr;
    }

    // create generic kmalloc-N caches
    for (int i = 0; i < SLAB_NUM_GENERIC; i++){
        uint32_t sz = 1u << (SLAB_MIN_SHIFT + i);
        char nm[SLAB_NAME_MAX];
        // build "kmalloc-<sz>"
        int p = 0;
        const char* pre = "kmalloc-";
        while (pre[p]){ nm[p] = pre[p]; p++; }
        // print sz decimal
        char t[12]; int ti = 0; uint32_t v = sz;
        if (v == 0) t[ti++] = '0';
        while (v){ t[ti++] = (char)('0' + (v % 10)); v /= 10; }
        while (ti) nm[p++] = t[--ti];
        nm[p] = 0;
        generic[i] = CacheCreate(nm, sz, 8);
        if (!generic[i]) {
            // a generic kmalloc cache failed to initialize. the slab allocator
            // is critical infrastructure (kmalloc depends on it), so a partially
            // built cache table is fatal rather than something to limp on. (satoru)
            KERNEL_PANIC("slab: failed to create a generic kmalloc cache (satoru)");
        }
    }

    ready = true;
    SerialLogger::Log("[Slab] generic caches ready (16B..8KB)\r\n");
    return true;
}

// cache creation -------------------------------------------------------------

kmem_cache* Slab::CacheCreate(const char* name, uint32_t object_size, uint32_t align){
    if (!Buddy::IsReady())              return nullptr;
    if (cache_count >= SLAB_MAX_CACHES) return nullptr;
    if (object_size == 0)               return nullptr;
    // overflow guard on object_size + align
    if (object_size > 0x40000000u)      return nullptr;
    if (align < 8) align = 8;
    if (align > 4096) align = 4096;

    kmem_cache* c = &caches[cache_count++];
    scpy(c->name, name ? name : "anon", SLAB_NAME_MAX);

    uint32_t osz = object_size;
    osz = (osz + align - 1) & ~(align - 1);
    if (osz == 0) { cache_count--; return nullptr; }
    c->object_size = osz;
    c->align       = align;

    // pick slab size: 4 KB for small, scale up so we get >= 8 objects
    // (or until we hit our pages_per_slab ceiling).
    uint32_t pages = 1;
    uint32_t bytes = SLAB_PAGE_BYTES;
    while (((bytes - (uint32_t)sizeof(SlabPage) - 64u) / osz) < 8u && pages < 16u){
        pages <<= 1;
        bytes <<= 1;
    }
    c->pages_per_slab = pages;

    // capacity: header + bitmap + objects. Solve for objs_per_slab using
    // closed form instead of a while-decrement that can underflow.
    uint32_t header_min   = (uint32_t)sizeof(SlabPage);
    uint32_t header_fixed = header_min - (uint32_t)sizeof(uint32_t);

    // guess + clamp. each obj costs osz bytes + 1 bit in the bitmap (~0.125B).
    // start with a generous estimate then shrink in a saturated loop.
    uint64_t free_bytes = (bytes > header_fixed) ? (bytes - header_fixed) : 0;
    uint64_t guess = free_bytes / osz;
    if (guess == 0) {
        cache_count--;
        return nullptr;
    }
    for (int safety = 0; safety < 64; safety++) {
        uint64_t bm_bytes = ((guess + 31) / 32) * 4;
        uint64_t need = header_fixed + bm_bytes + guess * osz;
        if (need <= bytes) break;
        if (guess <= 1) { guess = 0; break; }
        guess--;
    }
    if (guess == 0 || guess > 0x7FFFFFFFu) {
        cache_count--;
        return nullptr;
    }
    c->objs_per_slab = (uint32_t)guess;
    c->bitmap_words  = ((uint32_t)guess + 31) / 32;
    c->alloc_count = c->free_count = 0;
    c->slab_count  = c->total_objs = c->in_use_objs = 0;
    return c;
}

SlabPage* Slab::AllocSlabFor(kmem_cache* c){
    int order = 0; uint32_t p = c->pages_per_slab;
    while (p > 1){ p >>= 1; order++; }
    void* mem = Buddy::AllocPages(order);
    if (!mem) return nullptr;
    SlabPage* s = (SlabPage*)mem;
    s->magic   = SLAB_MAGIC;
    s->parent  = c;
    s->next = s->prev = nullptr;
    s->list = 0;
    s->in_use = 0;
    s->total  = c->objs_per_slab;
    for (uint32_t i = 0; i < c->bitmap_words; i++) s->bitmap[i] = 0;
    // push to empty list
    s->next = c->empty;
    if (c->empty) c->empty->prev = s;
    c->empty = s;
    c->slab_count++;
    c->total_objs += s->total;
    return s;
}

void Slab::ReleaseSlab(kmem_cache* c, SlabPage* s){
    // unlink
    SlabPage** lists[3] = { &c->empty, &c->partial, &c->full };
    if (s->prev) s->prev->next = s->next; else *lists[s->list] = s->next;
    if (s->next) s->next->prev = s->prev;

    int order = 0; uint32_t p = c->pages_per_slab;
    while (p > 1){ p >>= 1; order++; }
    c->slab_count--;
    c->total_objs -= s->total;
    Buddy::FreePages(s, order);
}

// allocate / free ------------------------------------------------------------

void* Slab::CacheAlloc(kmem_cache* c){
    if (!c) return nullptr;
    if (c->objs_per_slab == 0) return nullptr;

    // fast path: pull from partial first (best for cache locality, lowest
    // chance of having to allocate a new slab).
    SlabPage* s = c->partial;
    if (!s) s = c->empty;
    if (!s){
        s = AllocSlabFor(c);
        if (!s) return nullptr;
    }

    int idx = BitmapClaim(s->bitmap, (int)s->total);
    if (idx < 0) {
        // freelist exhausted mid-slab (race / corruption). Try a fresh slab
        // exactly once so callers never get a spurious null when the cache
        // is internally consistent.
        s = AllocSlabFor(c);
        if (!s) return nullptr;
        idx = BitmapClaim(s->bitmap, (int)s->total);
        if (idx < 0) return nullptr;
    }

    int from = s->list;
    s->in_use++;
    c->in_use_objs++;
    c->alloc_count++;

    int to = (s->in_use == s->total) ? 2 : 1;
    if (from != to) MoveToList(c, s, from, to);

    uint8_t* base = (uint8_t*)s
                  + (sizeof(SlabPage) - sizeof(uint32_t))
                  + c->bitmap_words * 4;
    uint8_t* obj = base + (uint32_t)idx * c->object_size;
    // zero-on-alloc so callers never see freed payload bytes.
    uint64_t* z = (uint64_t*)obj;
    uint32_t qwords = c->object_size / 8;
    for (uint32_t i = 0; i < qwords; i++) z[i] = 0;
    uint32_t tail = c->object_size & 7;
    if (tail) {
        uint8_t* t = obj + qwords * 8;
        for (uint32_t i = 0; i < tail; i++) t[i] = 0;
    }
    return obj;
}

static SlabPage* slab_of(void* obj, kmem_cache* c){
    // slabs are aligned to (pages_per_slab * 4096), find by mask.
    uint64_t bytes = (uint64_t)c->pages_per_slab * SLAB_PAGE_BYTES;
    uint64_t mask  = ~(bytes - 1);
    uint64_t base  = (uint64_t)(uintptr_t)obj & mask;
    SlabPage* s = (SlabPage*)base;
    if (s->magic != SLAB_MAGIC || s->parent != c) return nullptr;
    return s;
}

void Slab::CacheFree(kmem_cache* c, void* obj){
    if (!c || !obj) return;
    SlabPage* s = slab_of(obj, c);
    if (!s) return;
    uint8_t* base = (uint8_t*)s
                  + (sizeof(SlabPage) - sizeof(uint32_t))
                  + c->bitmap_words * 4;
    uint64_t off = (uint64_t)((uint8_t*)obj - base);
    if (c->object_size == 0) return;
    int idx = (int)(off / c->object_size);
    if (idx < 0 || (uint32_t)idx >= s->total) return;

    // double-free guard: if the slot is already free, ignore.
    uint32_t word = s->bitmap[idx >> 5];
    if (!(word & (1u << (idx & 31)))) return;

    BitmapRelease(s->bitmap, idx);
    int from = s->list;
    s->in_use--;
    c->in_use_objs--;
    c->free_count++;
    int to = (s->in_use == 0) ? 0 : 1;
    if (from != to) MoveToList(c, s, from, to);
}

// kmalloc / kfree ------------------------------------------------------------
//
// header layout: prefix 16-byte cookie before each object so kfree() can
// recover its cache.

struct kmalloc_hdr {
    uint32_t   magic;       // 0x4B4D414Cu = 'KMAL'
    uint32_t   shift;       // generic[] index
    uint64_t   pad;
};
#define KMALLOC_MAGIC 0x4B4D414Cu

void* Slab::kmalloc(uint32_t size){
    if (!ready || size == 0) return nullptr;
    // overflow guard on the +header math
    if (size > 0x7FFFFFFFu) return nullptr;
    uint32_t need = size + (uint32_t)sizeof(kmalloc_hdr);
    int shift = SLAB_MIN_SHIFT;
    uint32_t bucket = 1u << shift;
    while (bucket < need && shift < SLAB_MAX_SHIFT){
        shift++;
        bucket = 1u << shift;
    }
    if (need > bucket){
        void* p = Buddy::AllocBytes(need);
        if (!p) return nullptr;
        kmalloc_hdr* h = (kmalloc_hdr*)p;
        h->magic = KMALLOC_MAGIC;
        h->shift = 0xFFFFFFFFu;
        h->pad   = need;
        return (void*)((uint8_t*)p + sizeof(kmalloc_hdr));
    }
    int gi = shift - SLAB_MIN_SHIFT;
    if (gi < 0 || gi >= SLAB_NUM_GENERIC || !generic[gi]) return nullptr;
    void* obj = CacheAlloc(generic[gi]);
    if (!obj) return nullptr;
    kmalloc_hdr* h = (kmalloc_hdr*)obj;
    h->magic = KMALLOC_MAGIC;
    h->shift = (uint32_t)shift;
    h->pad   = 0;
    return (void*)((uint8_t*)obj + sizeof(kmalloc_hdr));
}

void Slab::kfree(void* ptr){
    if (!ptr) return;
    kmalloc_hdr* h = (kmalloc_hdr*)((uint8_t*)ptr - sizeof(kmalloc_hdr));
    if (h->magic != KMALLOC_MAGIC) return;     // double-free / foreign ptr
    if (h->shift == 0xFFFFFFFFu){
        uint64_t bytes = h->pad;
        h->magic = 0xDEADBEEFu;
        Buddy::FreeBytes(h, bytes);
        return;
    }
    if (h->shift < SLAB_MIN_SHIFT || h->shift > SLAB_MAX_SHIFT) return;
    int gi = (int)h->shift - SLAB_MIN_SHIFT;
    if (gi < 0 || gi >= SLAB_NUM_GENERIC || !generic[gi]) return;
    h->magic = 0xDEADBEEFu;
    CacheFree(generic[gi], h);
}

// procfs ---------------------------------------------------------------------

int Slab::DumpProcInfo(char* out, int max_len){
    int p = 0;
    auto put = [&](const char* s){ while (*s && p < max_len-1) out[p++] = *s++; };
    auto puti = [&](uint64_t v){
        char t[24]; int ti=0; if (v==0) t[ti++]='0';
        while (v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
        while (ti && p < max_len-1) out[p++] = t[--ti];
    };
    put("slabinfo - version: 2.1\n# name <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>\n");
    for (int i = 0; i < cache_count && p < max_len - 64; i++){
        kmem_cache* c = &caches[i];
        put(c->name); put(" ");
        puti(c->in_use_objs); put(" ");
        puti(c->total_objs);  put(" ");
        puti(c->object_size); put(" ");
        puti(c->objs_per_slab); put(" ");
        puti(c->pages_per_slab); put("\n");
    }
    if (p < max_len) out[p] = 0;
    return p;
}

int Slab::DumpInfo(char* out, int max_len){
    int p = 0;
    auto put = [&](const char* s){ while (*s && p < max_len-1) out[p++] = *s++; };
    auto puti = [&](uint64_t v){
        char t[24]; int ti=0; if (v==0) t[ti++]='0';
        while (v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
        while (ti && p < max_len-1) out[p++] = t[--ti];
    };
    put("Slab caches: ");
    puti((uint64_t)cache_count);
    put("\n");
    for (int i = 0; i < cache_count && p < max_len - 96; i++){
        kmem_cache* c = &caches[i];
        put("  "); put(c->name);
        put(" objsize="); puti(c->object_size);
        put(" inuse=");   puti(c->in_use_objs);
        put("/");         puti(c->total_objs);
        put(" slabs=");   puti(c->slab_count);
        put(" alloc=");   puti(c->alloc_count);
        put(" free=");    puti(c->free_count);
        put("\n");
    }
    if (p < max_len) out[p] = 0;
    return p;
}
