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
    for (size_t i = 0; i < n; i++) { if (x[i] != y[i]) return (int)x[i] - (int)y[i]; }
    return 0;
}
extern "C" void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else if (d > s) { for (size_t i = n; i > 0; i--) d[i-1] = s[i-1]; }
    return dst;
}
extern "C" size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
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
