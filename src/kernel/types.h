#pragma once
#include <stdint.h>
#include <stddef.h>

extern "C" void* memcpy(void* dst, const void* src, size_t n);
extern "C" void* memset(void* dst, int v, size_t n);
extern "C" int memcmp(const void* a, const void* b, size_t n);
extern "C" void* memmove(void* dst, const void* src, size_t n);
extern "C" size_t strlen(const char* s);
extern "C" void __cxa_pure_virtual();
void* operator new(size_t size);
void operator delete(void* p);
void operator delete(void* p, size_t size);
