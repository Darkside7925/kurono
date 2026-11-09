// mplat.h: the few platform services the media engine needs, so the same sources build in the (satoru)
// kernel and in ring 3. heap, log and the monotonic clock are the only kernel things the engine used. (satoru)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../kui/kui.h"

#ifndef KUI_BACKEND_WAYLAND
#include "../kernel/types.h"
#include "../drivers/serial.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

namespace MPlat {

// kernel: kui::Alloc is KernelHeap; ring 3: malloc. (satoru)
inline void* Alloc(size_t n) { return kui::Alloc(n); }
inline void  Free(void* p) { kui::Free(p); }

// kernel: the serial log; ring 3: stderr. (satoru)
inline void Log(const char* s) {
#ifndef KUI_BACKEND_WAYLAND
    SerialLogger::Log(s);
#else
    if (s) fputs(s, stderr);
#endif
}
inline void LogDec(int v) {
#ifndef KUI_BACKEND_WAYLAND
    SerialLogger::LogDec(v);
#else
    fprintf(stderr, "%d", v);
#endif
}

// monotonic ms widened past the 32-bit wrap, so clock origins never jump back. (satoru)
inline uint64_t NowMs64() {
    static uint32_t last = 0;
    static uint64_t hi = 0;
    uint32_t n = kui::NowMs();
    if (n < last) hi += 1ull << 32;
    last = n;
    return hi | n;
}

}  // namespace MPlat

// end (satoru)
