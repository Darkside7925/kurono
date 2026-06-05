#include "types.h"
#include "heap.h"

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    // fast path: use 64-bit copies for bulk data (framebuffer, etc.)
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    // align to 8 bytes
    while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
    // 64-bit bulk copy
    uint64_t* d64 = (uint64_t*)d;
    const uint64_t* s64 = (const uint64_t*)s;
    size_t qwords = n >> 3;
    for (size_t i = 0; i < qwords; i++) d64[i] = s64[i];
    // remainder
    d = (uint8_t*)(d64 + qwords);
    s = (const uint8_t*)(s64 + qwords);
    n &= 7;
    while (n--) *d++ = *s++;
    return dst;
}
extern "C" void* memset(void* dst, int v, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t b = (uint8_t)v;
    // align to 8 bytes
    while (n && ((uintptr_t)d & 7)) { *d++ = b; n--; }
    // 64-bit bulk set
    uint64_t w = (uint64_t)b;
    w |= (w << 8); w |= (w << 16); w |= (w << 32);
    uint64_t* d64 = (uint64_t*)d;
    size_t qwords = n >> 3;
    for (size_t i = 0; i < qwords; i++) d64[i] = w;
    // remainder
    d = (uint8_t*)(d64 + qwords);
    n &= 7;
    while (n--) *d++ = b;
    return dst;
}
extern "C" int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    // 64-bit bulk compare while both pointers share 8-byte alignment; on the
    // first differing word fall through to a byte scan for the exact mismatch. (satoru)
    while (n >= 8 && (((uintptr_t)x | (uintptr_t)y) & 7) == 0) {
        if (*(const uint64_t*)x != *(const uint64_t*)y) break;
        x += 8; y += 8; n -= 8;
    }
    for (size_t i = 0; i < n; i++) { if (x[i] != y[i]) return (int)x[i] - (int)y[i]; }
    return 0;
}
extern "C" void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    // non-overlapping ranges can use the word-optimized memcpy fast path. (satoru)
    if (d + n <= s || s + n <= d) return memcpy(dst, src, n);
    // overlapping: copy in the safe direction, byte-wise. (satoru)
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (size_t i = n; i > 0; i--) d[i-1] = s[i-1]; }
    return dst;
}
extern "C" size_t strlen(const char* s) {
    const char* p = s;
    // scan byte-wise until 8-byte aligned, then test 8 bytes per step with the
    // classic has-zero-byte swar trick; an aligned 8-byte read never crosses a
    // page, so reading past the terminator within the word is safe. (satoru)
    while ((uintptr_t)p & 7) { if (!*p) return (size_t)(p - s); ++p; }
    for (const uint64_t* w = (const uint64_t*)p; ; ++w) {
        uint64_t v = *w;
        if ((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL) {
            const char* b = (const char*)w;
            while (*b) ++b;
            return (size_t)(b - s);
        }
    }
}
extern "C" void __cxa_pure_virtual() { while(1); }

void* operator new(size_t size) { 
    return KernelHeap::Alloc(size);
}
void* operator new[](size_t size) { 
    return KernelHeap::Alloc(size);
}
void operator delete(void* p) { 
    KernelHeap::Free(p);
}
void operator delete(void* p, size_t size) { 
    (void)size;
    KernelHeap::Free(p);
}
void operator delete[](void* p) { 
    KernelHeap::Free(p);
}
void operator delete[](void* p, size_t size) { 
    (void)size;
    KernelHeap::Free(p);
}
