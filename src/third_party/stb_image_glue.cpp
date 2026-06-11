#include "../kernel/heap.h"
#include "../kernel/types.h"

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ASSERT(x)

#define STBI_MALLOC KernelHeap::Alloc
#define STBI_REALLOC KernelHeap::Realloc
#define STBI_FREE KernelHeap::Free

#define STBI_memcpy memcpy
#define STBI_memset memset
#define STBI_abs(x) ((x)<0?-(x):(x))

// Proper pow approximation for PNG gamma correction
static float stbi_pow_approx(float base, float exp) {
    if (base <= 0.0f) return 0.0f;
    if (exp == 0.0f) return 1.0f;
    if (exp == 1.0f) return base;
    // exp(exp * ln(base)) approximation using repeated squaring
    float result = 1.0f;
    float b = base;
    int n = (int)exp;
    float frac = exp - (float)n;
    // Integer part
    if (n < 0) { b = 1.0f / b; n = -n; }
    for (int i = 0; i < n; i++) result *= b;
    // Fractional part: linear interpolation for small fracs
    if (frac > 0.001f || frac < -0.001f) {
        result *= (1.0f + frac * (base - 1.0f));
    }
    return result;
}
#define STBI_pow(x,y) stbi_pow_approx(x,y)

// Proper ldexp: x * 2^n (handles negative n)
static float stbi_ldexp_impl(float x, int n) {
    if (n >= 0) {
        for (int i = 0; i < n && i < 30; i++) x *= 2.0f;
    } else {
        for (int i = 0; i < -n && i < 30; i++) x *= 0.5f;
    }
    return x;
}
#define STBI_ldexp(x,n) stbi_ldexp_impl(x,n)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// GCC fortified function stub for freestanding builds
extern "C" void* __memset_chk(void* dest, int c, size_t len, size_t destlen) {
    (void)destlen;
    return memset(dest, c, len);
}
