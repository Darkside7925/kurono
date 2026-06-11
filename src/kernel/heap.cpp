#include "heap.h"

uint8_t KernelHeap::buffer[2ULL * 1024 * 1024 * 1024]; // 2 GB heap
size_t KernelHeap::offset = 0;

void* KernelHeap::Alloc(size_t n) {
    size_t aligned = (n + 15) & ~((size_t)15);
    if (offset + aligned > sizeof(buffer)) return (void*)0;
    void* p = (void*)(buffer + offset);
    offset += aligned;
    return p;
}

void* KernelHeap::Realloc(void* oldp, size_t n) {
    if (!oldp) return Alloc(n);
    void* np = Alloc(n);
    if (!np) return (void*)0;
    memmove(np, oldp, n);
    return np;
}

void KernelHeap::Free(void* p) { (void)p; }
void KernelHeap::Reset() { offset = 0; }
