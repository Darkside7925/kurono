#pragma once
#include "types.h"

class KernelHeap {
public:
    static void* Alloc(size_t n);
    static void* Realloc(void* oldp, size_t n);
    static void Free(void* p);
    static void Reset();
    static size_t GetUsed() { return offset; }
private:
    static uint8_t buffer[2ULL * 1024 * 1024 * 1024]; // 2 GB heap
    static size_t offset;
};
