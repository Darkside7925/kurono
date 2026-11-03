// printk -> Kurono serial, with Linux level mapping.
//
// A driver's printk output is the single most valuable diagnostic during
// bring-up, so this is one of the few things implemented for real in phase 1
// rather than stubbed. Supports the subset of printf that kernel code actually
// uses; anything unrecognised is emitted verbatim so nothing is silently lost.
//
// end (satoru)

#include "lx_emul.h"
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

// Linux prefixes a printk format with "\001" + a level digit (KERN_ERR etc).
// map it to a readable tag so serial logs are scannable. (satoru)
static const char* level_tag(char d) {
    switch (d) {
        case '0': return "[EMERG] ";
        case '1': return "[ALERT] ";
        case '2': return "[CRIT ] ";
        case '3': return "[ERR  ] ";
        case '4': return "[WARN ] ";
        case '5': return "[NOTE ] ";
        case '6': return "[INFO ] ";
        case '7': return "[DEBUG] ";
        default:  return "";
    }
}

static void emit(const char* s, size_t n) { if (n) (void)!write(2, s, n); }

static void emit_u(unsigned long long v, int base, int upper) {
    char t[32];
    int k = 0;
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = d[v % (unsigned)base]; v /= (unsigned)base; }
    char b[32];
    int n = 0;
    while (k) b[n++] = t[--k];
    emit(b, (size_t)n);
}

static void emit_d(long long v) {
    if (v < 0) { emit("-", 1); emit_u((unsigned long long)(-v), 10, 0); }
    else emit_u((unsigned long long)v, 10, 0);
}

// the printf subset kernel code uses in practice: %s %d %i %u %x %X %p %lu %ld
// %llu %lld %zu %c %%. width/precision/flags are parsed and ignored - they only
// affect cosmetics, never correctness of the message. (satoru)
int lx_vprintk(const char* fmt, va_list ap) {
    if (!fmt) return 0;
    // leading level marker
    if (fmt[0] == '\001' && fmt[1]) {
        emit(level_tag(fmt[1]), strlen(level_tag(fmt[1])));
        fmt += 2;
    } else {
        emit("[INFO ] ", 8);
    }

    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            const char* start = p;
            while (*p && *p != '%') p++;
            emit(start, (size_t)(p - start));
            continue;
        }
        p++;                                   // skip '%'
        if (*p == '%') { emit("%", 1); p++; continue; }
        // skip flags/width/precision - cosmetic only
        while (*p && (strchr("-+ #0123456789.*", *p) != 0)) p++;
        // length modifiers
        int lcount = 0, zmod = 0;
        while (*p == 'l') { lcount++; p++; }
        if (*p == 'z') { zmod = 1; p++; }
        if (*p == 'h') { p++; if (*p == 'h') p++; }

        switch (*p) {
            case 'd': case 'i':
                if (lcount >= 2)      emit_d(va_arg(ap, long long));
                else if (lcount == 1) emit_d(va_arg(ap, long));
                else if (zmod)        emit_d((long long)va_arg(ap, ptrdiff_t));
                else                  emit_d(va_arg(ap, int));
                break;
            case 'u':
                if (lcount >= 2)      emit_u(va_arg(ap, unsigned long long), 10, 0);
                else if (lcount == 1) emit_u(va_arg(ap, unsigned long), 10, 0);
                else if (zmod)        emit_u((unsigned long long)va_arg(ap, size_t), 10, 0);
                else                  emit_u(va_arg(ap, unsigned int), 10, 0);
                break;
            case 'x': case 'X': {
                int up = (*p == 'X');
                if (lcount >= 2)      emit_u(va_arg(ap, unsigned long long), 16, up);
                else if (lcount == 1) emit_u(va_arg(ap, unsigned long), 16, up);
                else if (zmod)        emit_u((unsigned long long)va_arg(ap, size_t), 16, up);
                else                  emit_u(va_arg(ap, unsigned int), 16, up);
                break;
            }
            case 'p': {
                void* v = va_arg(ap, void*);
                emit("0x", 2);
                emit_u((unsigned long long)(uintptr_t)v, 16, 0);
                // %pM (mac), %pI4 (ip) etc take a trailing spec char; consume it
                // so it is not printed as literal text. (satoru)
                if (p[1] && strchr("MmI4i6bBhHRrSsFfVaExXdDgGOoNnCcUuTtLl", p[1])) {
                    p++;
                    if (p[1] == '4' || p[1] == '6') p++;
                }
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                emit(s, strlen(s));
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                emit(&c, 1);
                break;
            }
            case 0:
                return 0;
            default:
                // unrecognised conversion: show it rather than drop it, so the
                // gap is visible instead of silently mangling the message. (satoru)
                emit("%", 1);
                emit(p, 1);
                break;
        }
        if (*p) p++;
    }
    return 0;
}

// the symbol the driver actually calls. Linux's printk is a variadic macro in
// newer kernels but resolves to a real function; providing both names is
// harmless and covers either expansion. (satoru)
int lx_printk(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = lx_vprintk(fmt, ap);
    va_end(ap);
    return r;
}

int _printk(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = lx_vprintk(fmt, ap);
    va_end(ap);
    return r;
}

// end (satoru)
