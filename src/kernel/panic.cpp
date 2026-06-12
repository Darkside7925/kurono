#include "panic.h"
#include "multiboot.h"
#include "../hal/hal.h"
#include "../drivers/serial.h"
#include "../drivers/rtc.h"
#include "../fs/kvfs.h"
#include "../proc/kernel_locks.h"   // g_vfs_lock  -  panic-path deadlock guard
#include "../../logo.h"

namespace {
    volatile bool g_panicking = false;

    struct PanicFramebuffer {
        uint64_t addr;
        uint32_t pitch;
        uint32_t width;
        uint32_t height;
        uint8_t bpp;
        bool valid;
    };

    struct PanicDump {
        uint32_t stop_code;
        uint32_t vector;
        uint64_t rip;
        uint64_t cs;
        uint64_t rsp;
        uint64_t ss;
        uint64_t rflags;
        uint64_t cr2;
        uint64_t error_code;
        uint64_t rax;
        uint64_t rbx;
        uint64_t rcx;
        uint64_t rdx;
        uint64_t rsi;
        uint64_t rdi;
        uint64_t rbp;
        // r8-r15 captured from the interrupt frame so the minidump records the
        // true fault-context extended registers (not the dump path's live
        // values); zero for direct kebugcheckex calls. (satoru)
        uint64_t r8;
        uint64_t r9;
        uint64_t r10;
        uint64_t r11;
        uint64_t r12;
        uint64_t r13;
        uint64_t r14;
        uint64_t r15;
        bool     have_extended;   // 1 when r8-r15 above came from a frame (satoru)
        uint64_t param1;
        uint64_t param2;
        uint64_t param3;
        uint64_t param4;
        const char* reason;
        const char* file;
        uint32_t line;
    };

    PanicFramebuffer g_fb = {};
    PanicDump g_dump = {};

    static inline void outb(uint16_t port, uint8_t value) {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    static inline uint64_t rdmsr(uint32_t msr) {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }

    static inline void mmio_write32(uint64_t addr, uint32_t value) {
        *((volatile uint32_t*)(uintptr_t)addr) = value;
    }

    static void busy_delay(uint32_t loops) {
        for (volatile uint32_t i = 0; i < loops; i++) {
            asm volatile("pause");
        }
    }

    static void try_halt_other_cpus() {
        const uint64_t apic_base = rdmsr(0x1B);
        if ((apic_base & (1ULL << 11)) == 0) {
            return;
        }
        const uint64_t lapic = apic_base & 0xFFFFF000ULL;
        const uint64_t icr_low = lapic + 0x300;
        const uint32_t delivery_mode_nmi = (4u << 8);
        const uint32_t level_assert = (1u << 14);
        const uint32_t trigger_edge = 0u;
        const uint32_t shorthand_all_but_self = (3u << 18);
        mmio_write32(icr_low, delivery_mode_nmi | level_assert | trigger_edge | shorthand_all_but_self);
    }

    static inline uint32_t compose_color(uint8_t r, uint8_t g, uint8_t b) {
        return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    // write a single pixel using a volatile pointer.
    // for individual pixels (text glyphs), this is fine.
    static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
        if (!g_fb.valid || x >= g_fb.width || y >= g_fb.height) return;
        volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)
            (g_fb.addr + (uint64_t)y * g_fb.pitch + (uint64_t)x * (g_fb.bpp / 8));
        uint8_t b = (uint8_t)(color & 0xFF);
        uint8_t g = (uint8_t)((color >> 8) & 0xFF);
        uint8_t r = (uint8_t)((color >> 16) & 0xFF);
        if (g_fb.bpp == 32) {
            // use movnti (non-temporal 32-bit store)  -  works on wb, wc, and uc memory.
            uint32_t bgra = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            __asm__ __volatile__("movnti %1, (%0)" :: "r"((void*)p), "r"(bgra) : "memory");
        } else if (g_fb.bpp == 24) {
            p[0] = b; p[1] = g; p[2] = r;
        } else if (g_fb.bpp == 16) {
            uint16_t px = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
            *((volatile uint16_t*)p) = px;
        }
    }

    // fill rectangle using nt stores (row at a time) + sfence per row.
    // this ensures writes bypass cache and reach the display on wc memory.
    static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        if (!g_fb.valid) return;
        uint8_t b = (uint8_t)(color & 0xFF);
        uint8_t g_c = (uint8_t)((color >> 8) & 0xFF);
        uint8_t r = (uint8_t)((color >> 16) & 0xFF);
        uint32_t bgra = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g_c << 8) | b;

        for (uint32_t yy = 0; yy < h; yy++) {
            uint32_t py = y + yy;
            if (py >= g_fb.height) break;
            volatile uint8_t* row = (volatile uint8_t*)(uintptr_t)
                (g_fb.addr + (uint64_t)py * g_fb.pitch + (uint64_t)x * (g_fb.bpp / 8));

            if (g_fb.bpp == 32) {
                volatile uint32_t* row32 = (volatile uint32_t*)row;
                for (uint32_t xx = 0; xx < w && (x + xx) < g_fb.width; xx++) {
                    __asm__ __volatile__("movnti %1, (%0)" :: "r"((void*)&row32[xx]), "r"(bgra) : "memory");
                }
            } else if (g_fb.bpp == 24) {
                for (uint32_t xx = 0; xx < w && (x + xx) < g_fb.width; xx++) {
                    volatile uint8_t* p = row + xx * 3;
                    p[0] = b; p[1] = g_c; p[2] = r;
                }
            } else if (g_fb.bpp == 16) {
                uint16_t px16 = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g_c >> 2) << 5) | (uint16_t)(b >> 3);
                volatile uint16_t* row16 = (volatile uint16_t*)row;
                for (uint32_t xx = 0; xx < w && (x + xx) < g_fb.width; xx++) {
                    row16[xx] = px16;
                }
            }
            // sfence after each row  -  drains wc buffer so display sees the writes
            __asm__ __volatile__("sfence" ::: "memory");
        }
    }

    static const uint8_t GLYPH_SPACE[7] = {0,0,0,0,0,0,0};
    static const uint8_t GLYPH_DASH[7]  = {0,0,0,0x1F,0,0,0};
    static const uint8_t GLYPH_DOT[7]   = {0,0,0,0,0,0x0C,0x0C};
    static const uint8_t GLYPH_COLON[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0};
    static const uint8_t GLYPH_SLASH[7] = {0x01,0x02,0x04,0x08,0x10,0,0};
    static const uint8_t GLYPH_HASH[7]  = {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0};
    static const uint8_t GLYPH_LP[7]    = {0x06,0x08,0x10,0x10,0x10,0x08,0x06};
    static const uint8_t GLYPH_RP[7]    = {0x0C,0x02,0x01,0x01,0x01,0x02,0x0C};
    static const uint8_t GLYPH_EQ[7]    = {0,0x1F,0,0x1F,0,0,0};
    static const uint8_t GLYPH_Q[7]     = {0x0E,0x11,0x01,0x02,0x04,0,0x04};

    static const uint8_t GLYPH_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t GLYPH_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t GLYPH_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t GLYPH_3[7] = {0x1F,0x01,0x02,0x06,0x01,0x11,0x0E};
    static const uint8_t GLYPH_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t GLYPH_5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t GLYPH_6[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t GLYPH_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t GLYPH_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t GLYPH_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};

    static const uint8_t GLYPH_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t GLYPH_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t GLYPH_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t GLYPH_D[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t GLYPH_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t GLYPH_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t GLYPH_G[7] = {0x0E,0x11,0x10,0x13,0x11,0x11,0x0E};
    static const uint8_t GLYPH_H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t GLYPH_I[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t GLYPH_J[7] = {0x01,0x01,0x01,0x01,0x01,0x11,0x0E};
    static const uint8_t GLYPH_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t GLYPH_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t GLYPH_M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t GLYPH_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t GLYPH_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t GLYPH_P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t GLYPH_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t GLYPH_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t GLYPH_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t GLYPH_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t GLYPH_V[7] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04};
    static const uint8_t GLYPH_W[7] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
    static const uint8_t GLYPH_X[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t GLYPH_Y[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    static const uint8_t GLYPH_Z[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};

    static const uint8_t* glyph_for(char c) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        switch (c) {
            case ' ': return GLYPH_SPACE;
            case '-': return GLYPH_DASH;
            case '.': return GLYPH_DOT;
            case ':': return GLYPH_COLON;
            case '/': return GLYPH_SLASH;
            case '#': return GLYPH_HASH;
            case '(': return GLYPH_LP;
            case ')': return GLYPH_RP;
            case '=': return GLYPH_EQ;
            case '?': return GLYPH_Q;
            case '0': return GLYPH_0;
            case '1': return GLYPH_1;
            case '2': return GLYPH_2;
            case '3': return GLYPH_3;
            case '4': return GLYPH_4;
            case '5': return GLYPH_5;
            case '6': return GLYPH_6;
            case '7': return GLYPH_7;
            case '8': return GLYPH_8;
            case '9': return GLYPH_9;
            case 'A': return GLYPH_A;
            case 'B': return GLYPH_B;
            case 'C': return GLYPH_C;
            case 'D': return GLYPH_D;
            case 'E': return GLYPH_E;
            case 'F': return GLYPH_F;
            case 'G': return GLYPH_G;
            case 'H': return GLYPH_H;
            case 'I': return GLYPH_I;
            case 'J': return GLYPH_J;
            case 'K': return GLYPH_K;
            case 'L': return GLYPH_L;
            case 'M': return GLYPH_M;
            case 'N': return GLYPH_N;
            case 'O': return GLYPH_O;
            case 'P': return GLYPH_P;
            case 'R': return GLYPH_R;
            case 'S': return GLYPH_S;
            case 'T': return GLYPH_T;
            case 'U': return GLYPH_U;
            case 'V': return GLYPH_V;
            case 'W': return GLYPH_W;
            case 'X': return GLYPH_X;
            case 'Y': return GLYPH_Y;
            case 'Z': return GLYPH_Z;
            default: return GLYPH_Q;
        }
    }

    static void draw_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale) {
        const uint8_t* glyph = glyph_for(c);
        for (uint32_t row = 0; row < 7; row++) {
            for (uint32_t col = 0; col < 5; col++) {
                if (glyph[row] & (1u << (4 - col))) {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }

    static void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t color, uint32_t scale) {
        if (!text) return;
        uint32_t cursor = x;
        for (size_t i = 0; text[i]; i++) {
            draw_char(cursor, y, text[i], color, scale);
            cursor += (6 * scale);
        }
    }

    static void to_hex16(uint64_t value, char out[17]) {
        const char* hex = "0123456789ABCDEF";
        for (int i = 15; i >= 0; i--) {
            out[i] = hex[value & 0xF];
            value >>= 4;
        }
        out[16] = 0;
    }

    static void to_hex8(uint32_t value, char out[9]) {
        const char* hex = "0123456789ABCDEF";
        for (int i = 7; i >= 0; i--) {
            out[i] = hex[value & 0xF];
            value >>= 4;
        }
        out[8] = 0;
    }

    static void to_dec(uint32_t value, char out[12]) {
        if (value == 0) {
            out[0] = '0'; out[1] = 0; return;
        }
        char temp[12];
        int idx = 0;
        while (value && idx < 11) {
            temp[idx++] = (char)('0' + (value % 10));
            value /= 10;
        }
        int outi = 0;
        while (idx > 0) out[outi++] = temp[--idx];
        out[outi] = 0;
    }

    static void render_panic_status(uint32_t countdown_secs) {
        if (!g_fb.valid) return;

        const uint32_t text_color = compose_color(170, 170, 170);
        const uint32_t accent_color = compose_color(120, 170, 255);
        const uint32_t ok_color = compose_color(120, 220, 120);
        const uint32_t bg_color = compose_color(0, 0, 0);

        uint32_t x = 112;
        uint32_t y = (g_fb.height > 600) ? (g_fb.height - 120) : (g_fb.height - 90);
        uint32_t w = g_fb.width > 260 ? (g_fb.width - 224) : (g_fb.width - 20);
        if ((int32_t)w < 120) w = 120;

        fill_rect(x - 12, y - 20, w + 24, 92, bg_color);
        draw_text(x, y - 18, "SYSTEM PANIC RECOVERY", accent_color, 2);

        char secs[12];
        to_dec(countdown_secs, secs);

        draw_text(x, y + 18, "REBOOTING IN", text_color, 2);
        draw_text(x + 156, y + 18, secs, ok_color, 2);
        draw_text(x + 180, y + 18, "SECONDS", text_color, 2);
        __asm__ __volatile__("wbinvd" ::: "memory");
    }

    static void render_evacuation_screen() {
        if (!g_fb.valid) return;

        const uint32_t bg_color = compose_color(0, 56, 132);
        const uint32_t title_color = compose_color(255, 255, 255);
        const uint32_t text_color = compose_color(220, 235, 255);
        const uint32_t accent_color = compose_color(255, 235, 140);

        fill_rect(0, 0, g_fb.width, g_fb.height, bg_color);

        uint32_t left = (g_fb.width > 960) ? 84 : 28;
        uint32_t top  = (g_fb.height > 720) ? 72 : 32;

        draw_text(left, top, "KURONO BSOD", title_color, 4);
        draw_text(left, top + 52, "KERNEL PANIC EVACUATION", accent_color, 2);
        draw_text(left, top + 86, "THE KERNEL CRASHED - EMERGENCY RECOVERY ACTIVE", text_color, 2);

        char stop_hex[9];
        to_hex8(g_dump.stop_code, stop_hex);
        draw_text(left, top + 124, "STOP CODE:", title_color, 2);
        draw_text(left + 118, top + 124, stop_hex, text_color, 2);

        draw_text(left, top + 158, "LOADING CRASH SCREEN AND FORCING REBOOT", text_color, 2);
        draw_text(left, top + 190, "IF REBOOT STALLS, HOLD POWER BUTTON", text_color, 2);

        __asm__ __volatile__("sfence; mfence; wbinvd" ::: "memory");
    }

    // log a full 64-bit value as 16 hex digits (seriallogger only has 32-bit loghex)
    static void serial_log_hex64(uint64_t val) {
        SerialLogger::LogHex((uint32_t)(val >> 32));
        SerialLogger::LogHex((uint32_t)(val & 0xFFFFFFFF));
    }

    static void serial_dump() {
        SerialLogger::Log("\r\n=== KURONO KERNEL PANIC ===\r\n");
        SerialLogger::Log("StopCode: 0x");
        SerialLogger::LogHex(g_dump.stop_code);
        SerialLogger::Log(" Vector: ");
        SerialLogger::LogDec((int)g_dump.vector);
        SerialLogger::Log("\r\nRIP=0x");
        serial_log_hex64(g_dump.rip);
        SerialLogger::Log(" RSP=0x");
        serial_log_hex64(g_dump.rsp);
        SerialLogger::Log("\r\nRFLAGS=0x");
        serial_log_hex64(g_dump.rflags);
        SerialLogger::Log(" CS=0x");
        serial_log_hex64(g_dump.cs);
        SerialLogger::Log(" SS=0x");
        serial_log_hex64(g_dump.ss);
        SerialLogger::Log(" CR2=0x");
        serial_log_hex64(g_dump.cr2);
        SerialLogger::Log(" ERR=0x");
        serial_log_hex64(g_dump.error_code);
        SerialLogger::Log("\r\nRAX=0x");
        serial_log_hex64(g_dump.rax);
        SerialLogger::Log(" RBX=0x");
        serial_log_hex64(g_dump.rbx);
        SerialLogger::Log(" RCX=0x");
        serial_log_hex64(g_dump.rcx);
        SerialLogger::Log(" RDX=0x");
        serial_log_hex64(g_dump.rdx);
        SerialLogger::Log("\r\nRSI=0x");
        serial_log_hex64(g_dump.rsi);
        SerialLogger::Log(" RDI=0x");
        serial_log_hex64(g_dump.rdi);
        SerialLogger::Log(" RBP=0x");
        serial_log_hex64(g_dump.rbp);
        SerialLogger::Log("\r\nParams: 0x");
        serial_log_hex64(g_dump.param1);
        SerialLogger::Log(" 0x");
        serial_log_hex64(g_dump.param2);
        SerialLogger::Log(" 0x");
        serial_log_hex64(g_dump.param3);
        SerialLogger::Log(" 0x");
        serial_log_hex64(g_dump.param4);
        SerialLogger::Log("\r\nReason: ");
        if (g_dump.reason) SerialLogger::Log(g_dump.reason);
        if (g_dump.file) {
            SerialLogger::Log("\r\nFile: ");
            SerialLogger::Log(g_dump.file);
            SerialLogger::Log(" Line: ");
            SerialLogger::LogDec((int)g_dump.line);
        }
        SerialLogger::Log("\r\n=== END PANIC ===\r\n");
    }

    // ---- persistent minidump support ----------------------------------------
    // we write a flat KuronoMiniDump to a fixed identity-mapped physical page
    // so a freshly-booted kernel can recover it from ram (kvfs is wiped on the
    // reboot the panic triggers). everything here is freestanding and avoids the
    // heap. (satoru)

    // freestanding byte fill (no libc in the kernel). (satoru)
    static void dump_memset(void* dst, uint8_t val, uint32_t len) {
        volatile uint8_t* p = (volatile uint8_t*)dst;
        for (uint32_t i = 0; i < len; i++) p[i] = val;
    }

    // freestanding byte copy. (satoru)
    static void dump_memcpy(void* dst, const void* src, uint32_t len) {
        volatile uint8_t* d = (volatile uint8_t*)dst;
        const uint8_t* s = (const uint8_t*)src;
        for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    }

    // copy a c-string into a fixed field, truncating and zero-padding. (satoru)
    static void dump_copy_field(char* dst, uint32_t cap, const char* src) {
        uint32_t i = 0;
        if (src) {
            for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
        }
        for (; i < cap; i++) dst[i] = 0;
    }

    // x86-64 canonical-address test: bits 63..47 must all match bit 47, so a
    // valid pointer is either <= 0x00007FFF_FFFFFFFF or >= 0xFFFF8000_00000000.
    // used to stop the stack walk on garbage. (satoru)
    static bool dump_is_canonical(uint64_t a) {
        uint64_t hi = a >> 47;
        return hi == 0 || hi == 0x1FFFFu;
    }

    // days-from-epoch helpers, mirrored from timemanager::to_unix_s so the dump
    // is self-contained and does not depend on timer state that may be unsafe to
    // touch mid-panic. (satoru)
    static bool dump_is_leap(uint16_t y) {
        return ((y % 4) == 0 && (y % 100) != 0) || ((y % 400) == 0);
    }
    static uint16_t dump_month_days(uint16_t y, uint8_t m) {
        static const uint8_t t[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2) return (uint16_t)(t[1] + (dump_is_leap(y) ? 1 : 0));
        if (m >= 1 && m <= 12) return t[m - 1];
        return 30;
    }
    static uint32_t dump_to_unix(uint16_t year, uint8_t mon, uint8_t dom,
                                 uint8_t h, uint8_t m, uint8_t s) {
        if (year < 1970) return 0;
        uint32_t days = 0;
        for (uint16_t y = 1970; y < year; y++) days += (dump_is_leap(y) ? 366u : 365u);
        for (uint8_t mm = 1; mm < mon; mm++) days += dump_month_days(year, mm);
        if (dom > 0) days += (uint32_t)(dom - 1);
        return days * 86400u + (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s;
    }

    // pull the tail of the mirrored serial log out of kvfs (still intact at
    // panic time  -  it is only lost on the reboot we are about to trigger). this
    // is the best "recent serial lines" source available because seriallogger
    // exposes no in-memory ring buffer. returns bytes copied; sets *truncated if
    // older bytes were dropped. (satoru)
    static uint32_t dump_collect_serial_tail(char* out, uint32_t cap, uint8_t* truncated) {
        *truncated = 0;
        if (cap == 0) return 0;
        out[0] = 0;

        // This runs from the panic path, which can fire from an exception that
        // interrupted a process holding g_vfs_lock. Every KVFS call below now
        // takes that (non-recursive) lock, so we'd self-deadlock the dying CPU
        // and never finish the minidump. Probe with TryLock: IF is already
        // cleared by KeBugCheckEx (cli) on this uniprocessor, so if the probe
        // succeeds nothing can grab the lock under us  -  release immediately and
        // let the normal locked KVFS calls re-acquire. If the probe fails the
        // lock is held by the interrupted context, so skip the (best-effort)
        // serial tail rather than hang. (satoru)
        if (!g_vfs_lock.TryLock()) return 0;
        g_vfs_lock.Unlock();

        if (!KVFS::GetRoot()) return 0;

        // the runtime logger mirrors all seriallogger output to these files;
        // try the in-os path first, then the system path. (satoru)
        const char* candidates[2] = { "/kurono/var/log/serial.log", "/system/logs/serial.log" };
        for (int c = 0; c < 2; c++) {
            int sz = KVFS::GetFileSize(candidates[c]);
            if (sz <= 0) continue;

            uint32_t fsize = (uint32_t)sz;
            uint32_t want = cap - 1;
            uint32_t start = 0;
            if (fsize > want) { start = fsize - want; *truncated = 1; }

            int fd = KVFS::Open(candidates[c], 1 /* read */);
            if (fd < 0) continue;
            KVFS::Seek(fd, (int32_t)start, 0 /* set */);
            int got = KVFS::Read(fd, out, want);
            KVFS::Close(fd);
            if (got < 0) got = 0;
            out[got] = 0;
            return (uint32_t)got;
        }
        return 0;
    }

    // walk the rbp chain: frame layout is [rbp] = saved rbp, [rbp+8] = return
    // address. stop on a null/non-canonical/non-increasing frame pointer. (satoru)
    static uint32_t dump_walk_stack(uint64_t start_rbp, uint64_t* out, uint32_t max) {
        uint32_t n = 0;
        uint64_t rbp = start_rbp;
        uint64_t prev = 0;
        while (n < max && rbp != 0 && dump_is_canonical(rbp) && (rbp & 0x7) == 0) {
            // frame pointers grow downward as we recurse, so each saved rbp must
            // be strictly greater than the previous one; a non-increasing value
            // means we've fallen off into garbage. (satoru)
            if (prev != 0 && rbp <= prev) break;
            uint64_t ret = ((const uint64_t*)(uintptr_t)rbp)[1];
            if (ret == 0 || !dump_is_canonical(ret)) break;
            out[n++] = ret;
            prev = rbp;
            rbp = ((const uint64_t*)(uintptr_t)rbp)[0];
        }
        return n;
    }

    // populate and persist the minidump. called from kebugcheckex after the
    // panic screen renders but before reboot. registers that g_dump already
    // captured from the interrupt frame are preferred (they reflect the actual
    // fault context); r8-r15 and cr0/cr3/cr4 are read live here because g_dump
    // does not carry them. (satoru)
    static void write_minidump() {
        // live register capture  -  used for r8-r15 unconditionally, and as a
        // fallback for the gp regs / rsp / rbp when this is a direct
        // kebugcheckex call (no interrupt frame, so g_dump's gp regs are 0). (satoru)
        uint64_t l_rax, l_rbx, l_rcx, l_rdx, l_rsi, l_rdi, l_rbp, l_rsp;
        uint64_t l_r8, l_r9, l_r10, l_r11, l_r12, l_r13, l_r14, l_r15;
        __asm__ __volatile__("movq %%rax, %0" : "=r"(l_rax));
        __asm__ __volatile__("movq %%rbx, %0" : "=r"(l_rbx));
        __asm__ __volatile__("movq %%rcx, %0" : "=r"(l_rcx));
        __asm__ __volatile__("movq %%rdx, %0" : "=r"(l_rdx));
        __asm__ __volatile__("movq %%rsi, %0" : "=r"(l_rsi));
        __asm__ __volatile__("movq %%rdi, %0" : "=r"(l_rdi));
        __asm__ __volatile__("movq %%rbp, %0" : "=r"(l_rbp));
        __asm__ __volatile__("movq %%rsp, %0" : "=r"(l_rsp));
        __asm__ __volatile__("movq %%r8,  %0" : "=r"(l_r8));
        __asm__ __volatile__("movq %%r9,  %0" : "=r"(l_r9));
        __asm__ __volatile__("movq %%r10, %0" : "=r"(l_r10));
        __asm__ __volatile__("movq %%r11, %0" : "=r"(l_r11));
        __asm__ __volatile__("movq %%r12, %0" : "=r"(l_r12));
        __asm__ __volatile__("movq %%r13, %0" : "=r"(l_r13));
        __asm__ __volatile__("movq %%r14, %0" : "=r"(l_r14));
        __asm__ __volatile__("movq %%r15, %0" : "=r"(l_r15));

        // control registers read live (cr2 also lives in g_dump). (satoru)
        uint64_t l_cr0, l_cr2, l_cr3, l_cr4;
        __asm__ __volatile__("movq %%cr0, %0" : "=r"(l_cr0));
        __asm__ __volatile__("movq %%cr2, %0" : "=r"(l_cr2));
        __asm__ __volatile__("movq %%cr3, %0" : "=r"(l_cr3));
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(l_cr4));

        // the dump lives at a fixed identity-mapped physical address. (satoru)
        KuronoMiniDump* d = (KuronoMiniDump*)(uintptr_t)MiniDump::PHYS_ADDR;
        dump_memset(d, 0, sizeof(KuronoMiniDump));

        d->version   = 1;
        d->size      = (uint32_t)sizeof(KuronoMiniDump);
        d->stop_code = g_dump.stop_code;

        // timestamp from the rtc  -  self-contained cmos read. (satoru)
        RTC::Date rd; RTC::Time rt;
        if (RTC::ReadDateTime(rd, rt)) {
            d->time_valid = 1;
            d->year   = rd.year;
            d->month  = rd.mon;
            d->day    = rd.dom;
            d->hour   = rt.h;
            d->minute = rt.m;
            d->second = rt.s;
            d->unix_time = dump_to_unix(rd.year, rd.mon, rd.dom, rt.h, rt.m, rt.s);
        }

        dump_copy_field(d->message, MiniDump::MSG_LEN, g_dump.reason);

        // prefer fault-context gp regs from g_dump (populated from the interrupt
        // frame); fall back to live values for direct bugchecks where they are
        // zero. (satoru)
        const bool have_frame = (g_dump.rip != 0 || g_dump.rsp != 0);
        d->rax = have_frame ? g_dump.rax : l_rax;
        d->rbx = have_frame ? g_dump.rbx : l_rbx;
        d->rcx = have_frame ? g_dump.rcx : l_rcx;
        d->rdx = have_frame ? g_dump.rdx : l_rdx;
        d->rsi = have_frame ? g_dump.rsi : l_rsi;
        d->rdi = have_frame ? g_dump.rdi : l_rdi;
        d->rbp = have_frame ? g_dump.rbp : l_rbp;
        d->rsp = have_frame ? g_dump.rsp : l_rsp;
        // r8-r15 come from the fault frame when bugcheckfrominterrupt captured
        // them; otherwise fall back to the live values read above. (satoru)
        const bool have_ext = g_dump.have_extended;
        d->r8  = have_ext ? g_dump.r8  : l_r8;
        d->r9  = have_ext ? g_dump.r9  : l_r9;
        d->r10 = have_ext ? g_dump.r10 : l_r10;
        d->r11 = have_ext ? g_dump.r11 : l_r11;
        d->r12 = have_ext ? g_dump.r12 : l_r12;
        d->r13 = have_ext ? g_dump.r13 : l_r13;
        d->r14 = have_ext ? g_dump.r14 : l_r14;
        d->r15 = have_ext ? g_dump.r15 : l_r15;
        d->rip    = g_dump.rip;
        d->rflags = have_frame ? g_dump.rflags : 0;

        d->cr0 = l_cr0;
        d->cr2 = g_dump.cr2 ? g_dump.cr2 : l_cr2;
        d->cr3 = l_cr3;
        d->cr4 = l_cr4;

        // stack trace: walk from the fault-context rbp when we have a frame,
        // else from the live rbp captured above. (satoru)
        uint64_t walk_rbp = have_frame ? g_dump.rbp : l_rbp;
        d->frame_count = dump_walk_stack(walk_rbp, d->stack_trace, MiniDump::STACK_FRAMES);

        // recent serial log tail (best-effort; see helper). (satoru)
        d->serial_len = dump_collect_serial_tail(d->serial_tail,
                                                 MiniDump::SERIAL_BYTES,
                                                 &d->serial_truncated);

        // publish the magic last so a reader never sees a half-written dump, and
        // flush caches so the post-reboot kernel observes the bytes. (satoru)
        __asm__ __volatile__("sfence" ::: "memory");
        d->magic = MiniDump::MAGIC;
        __asm__ __volatile__("sfence; wbinvd" ::: "memory");

        SerialLogger::Log("KeBugCheckEx: minidump written to phys 0x1000000 (frames=");
        SerialLogger::LogDec((int)d->frame_count);
        SerialLogger::Log(", serial=");
        SerialLogger::LogDec((int)d->serial_len);
        SerialLogger::Log(")\r\n");
    }

    static void draw_logo_small(uint32_t x, uint32_t y, uint32_t size) {
        if (size == 0) return;
        for (uint32_t dy = 0; dy < size; dy++) {
            uint32_t sy = (dy * LOGO_HEIGHT) / size;
            for (uint32_t dx = 0; dx < size; dx++) {
                uint32_t sx = (dx * LOGO_WIDTH) / size;
                uint32_t pixel = logo_data[sy * LOGO_WIDTH + sx];
                uint8_t alpha = (uint8_t)(pixel >> 24);
                if (alpha > 70) {
                    uint8_t r = (uint8_t)((pixel >> 16) & 0xFF);
                    uint8_t g = (uint8_t)((pixel >> 8) & 0xFF);
                    uint8_t b = (uint8_t)(pixel & 0xFF);
                    put_pixel(x + dx, y + dy, compose_color(r, g, b));
                }
            }
        }
    }

    // vga text-mode fallback: write panic info directly to 0xb8000
    // this is visible if the gpu is in vga text mode (or if qemu's internal
    // console uses text mode).  it's a last-resort safety net.
    static void vga_panic_fallback() {
        volatile uint16_t* vga = (volatile uint16_t*)(uintptr_t)0xB8000;
        const uint8_t attr_title = 0x4F;  // white on red
        const uint8_t attr_text  = 0x07;  // grey on black
        // clear screen
        for (int i = 0; i < 80*25; i++) vga[i] = (uint16_t)(attr_text << 8) | ' ';
        // row 0: title
        const char* title = "*** KURONO KERNEL PANIC ***";
        for (int i = 0; title[i]; i++) vga[i] = (uint16_t)(attr_title << 8) | (uint8_t)title[i];
        // row 2: stop code
        const char* sc_label = "StopCode: 0x";
        int col = 0;
        for (int i = 0; sc_label[i]; i++) vga[80*2 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)sc_label[i];
        char sc_hex[9]; to_hex8(g_dump.stop_code, sc_hex);
        for (int i = 0; sc_hex[i]; i++) vga[80*2 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)sc_hex[i];
        // row 3: reason
        if (g_dump.reason) {
            const char* r_label = "Reason: ";
            col = 0;
            for (int i = 0; r_label[i]; i++) vga[80*3 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)r_label[i];
            for (int i = 0; g_dump.reason[i] && col < 79; i++) vga[80*3 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)g_dump.reason[i];
        }
        // row 5: rip
        char rip_hex[17]; to_hex16(g_dump.rip, rip_hex);
        const char* rip_label = "RIP: 0x";
        col = 0;
        for (int i = 0; rip_label[i]; i++) vga[80*5 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)rip_label[i];
        for (int i = 0; rip_hex[i]; i++) vga[80*5 + col++] = (uint16_t)(attr_text << 8) | (uint8_t)rip_hex[i];
        // row 7: system halted
        const char* halted = "SYSTEM HALTED - Rebooting soon";
        for (int i = 0; halted[i]; i++) vga[80*7 + i] = (uint16_t)(attr_title << 8) | (uint8_t)halted[i];
        asm volatile("wbinvd" ::: "memory");
    }

    static void render_panic_screen() {
        if (!g_fb.valid) {
            SerialLogger::Log("render_panic_screen: SKIPPED  -  fb not valid\r\n");
            return;
        }

        SerialLogger::Log("render_panic_screen: starting, addr=0x");
        SerialLogger::LogHex((uint32_t)(g_fb.addr >> 32));
        SerialLogger::LogHex((uint32_t)g_fb.addr);
        SerialLogger::Log("\r\n");

        // fill background black
        fill_rect(0, 0, g_fb.width, g_fb.height, compose_color(0, 0, 0));
        SerialLogger::Log("render_panic_screen: bg filled\r\n");

        uint32_t logo_size = (g_fb.width > 1280) ? 84 : 64;
        uint32_t left = 30;
        uint32_t top = 28;
        draw_logo_small(left, top, logo_size);

        char stop_hex[9];
        char rip_hex[17];
        char rsp_hex[17];
        char cr2_hex[17];
        char err_hex[17];
        char cs_hex[17];
        char rflags_hex[17];
        char rax_hex[17];
        char rbx_hex[17];
        char vec_dec[12];
        char line_dec[12];
        to_hex8(g_dump.stop_code, stop_hex);
        to_hex16(g_dump.rip, rip_hex);
        to_hex16(g_dump.rsp, rsp_hex);
        to_hex16(g_dump.cr2, cr2_hex);
        to_hex16(g_dump.error_code, err_hex);
        to_hex16(g_dump.cs, cs_hex);
        to_hex16(g_dump.rflags, rflags_hex);
        to_hex16(g_dump.rax, rax_hex);
        to_hex16(g_dump.rbx, rbx_hex);
        to_dec(g_dump.vector, vec_dec);
        to_dec(g_dump.line, line_dec);

        const uint32_t title_color = compose_color(210, 210, 210);
        const uint32_t text_color = compose_color(170, 170, 170);
        const uint32_t accent_color = compose_color(120, 170, 255);

        uint32_t tx = left + logo_size + 18;
        uint32_t ty = top + 4;

        for (int fade = 0; fade < 4; fade++) {
            uint8_t scale_v = (uint8_t)(80 + fade * 50);
            uint32_t ftitle = compose_color(scale_v, scale_v, scale_v);
            uint32_t ftext = compose_color(scale_v - 20, scale_v - 20, scale_v - 20);
            uint32_t faccent = compose_color((uint8_t)(40 + fade * 35), (uint8_t)(80 + fade * 35), (uint8_t)(120 + fade * 35));

            fill_rect(0, 0, g_fb.width, g_fb.height, compose_color(0, 0, 0));
            draw_logo_small(left, top, logo_size);

            draw_text(tx, ty, "KURONO KERNEL PANIC", ftitle, 3);
            draw_text(tx, ty + 30, "A FATAL KERNEL ERROR OCCURRED", ftext, 2);
            draw_text(tx, ty + 54, "STOP CODE:", faccent, 2);
            draw_text(tx + 132, ty + 54, stop_hex, ftitle, 2);

            // drain wc buffer so qemu's display refresh sees this frame
            __asm__ __volatile__("sfence; mfence" ::: "memory");
            busy_delay(4500000);
        }

        SerialLogger::Log("render_panic_screen: fade done, drawing final\r\n");
        draw_text(tx, ty, "KURONO KERNEL PANIC", title_color, 3);
        draw_text(tx, ty + 30, "A FATAL KERNEL ERROR OCCURRED", text_color, 2);
        draw_text(tx, ty + 54, "STOP CODE:", accent_color, 2);
        draw_text(tx + 132, ty + 54, stop_hex, title_color, 2);

        draw_text(tx, ty + 82, "REASON:", accent_color, 2);
        if (g_dump.reason) draw_text(tx + 94, ty + 82, g_dump.reason, text_color, 2);

        draw_text(tx, ty + 106, "RIP:", accent_color, 2);
        draw_text(tx + 52, ty + 106, rip_hex, text_color, 2);

        draw_text(tx, ty + 130, "RSP:", accent_color, 2);
        draw_text(tx + 52, ty + 130, rsp_hex, text_color, 2);

        draw_text(tx, ty + 154, "CR2:", accent_color, 2);
        draw_text(tx + 52, ty + 154, cr2_hex, text_color, 2);

        draw_text(tx, ty + 178, "VECTOR:", accent_color, 2);
        draw_text(tx + 88, ty + 178, vec_dec, text_color, 2);

        draw_text(tx + 156, ty + 178, "ERROR:", accent_color, 2);
        draw_text(tx + 238, ty + 178, err_hex, text_color, 2);

        draw_text(tx, ty + 202, "CS:", accent_color, 2);
        draw_text(tx + 40, ty + 202, cs_hex, text_color, 2);

        draw_text(tx + 240, ty + 202, "RFLAGS:", accent_color, 2);
        draw_text(tx + 336, ty + 202, rflags_hex, text_color, 2);

        draw_text(tx, ty + 226, "RAX:", accent_color, 2);
        draw_text(tx + 52, ty + 226, rax_hex, text_color, 2);

        draw_text(tx + 240, ty + 226, "RBX:", accent_color, 2);
        draw_text(tx + 292, ty + 226, rbx_hex, text_color, 2);

        draw_text(tx, ty + 250, "FILE:", accent_color, 2);
        if (g_dump.file) draw_text(tx + 64, ty + 250, g_dump.file, text_color, 2);

        draw_text(tx, ty + 274, "LINE:", accent_color, 2);
        draw_text(tx + 64, ty + 274, line_dec, text_color, 2);

        draw_text(tx, ty + 306, "SYSTEM PANIC RECOVERY", title_color, 2);

        // drain wc buffer + full memory fence  -  makes all pixel writes visible
        __asm__ __volatile__("sfence; mfence" ::: "memory");

        // issue wbinvd as an extra measure for whpx/hypervisor environments
        __asm__ __volatile__("wbinvd" ::: "memory");
    }

    [[noreturn]] static void reboot_or_halt() {
        for (uint32_t secs = 5; secs > 0; secs--) {
            render_panic_status(secs);
            busy_delay(32000000);
        }

        render_panic_status(0);
        render_evacuation_screen();
        busy_delay(4000000);

        SerialLogger::Log("KeBugCheckEx: rebooting now\r\n");
        HAL::Reboot();
        while (true) {
            vga_panic_fallback();
            render_evacuation_screen();
            HAL::Reboot();
            asm volatile("pause");
        }
    }
}

namespace KernelPanic {
    void Initialize(multiboot_info_t* mbi) {
        g_fb = {};
        if (!mbi) {
            SerialLogger::Log("Panic::Init FAIL: mbi=null\r\n");
            return;
        }
        if ((mbi->flags & (1u << 12)) == 0) {
            SerialLogger::Log("Panic::Init FAIL: flags bit12 not set, flags=0x");
            SerialLogger::LogHex(mbi->flags);
            SerialLogger::Log("\r\n");
            return;
        }
        if (mbi->framebuffer_addr == 0) {
            SerialLogger::Log("Panic::Init FAIL: fb_addr=0\r\n");
            return;
        }
        if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0) {
            SerialLogger::Log("Panic::Init FAIL: fb dimensions 0\r\n");
            return;
        }
        if (mbi->framebuffer_type == 2) {
            SerialLogger::Log("Panic::Init FAIL: EGA text mode\r\n");
            return;
        }
        if (mbi->framebuffer_bpp != 16 && mbi->framebuffer_bpp != 24 && mbi->framebuffer_bpp != 32) {
            SerialLogger::Log("Panic::Init FAIL: bad bpp=");
            SerialLogger::LogDec(mbi->framebuffer_bpp);
            SerialLogger::Log("\r\n");
            return;
        }
        g_fb.addr = mbi->framebuffer_addr;
        g_fb.pitch = mbi->framebuffer_pitch;
        g_fb.width = mbi->framebuffer_width;
        g_fb.height = mbi->framebuffer_height;
        g_fb.bpp = mbi->framebuffer_bpp;
        g_fb.valid = true;
        SerialLogger::Log("Panic::Init OK: fb=0x");
        SerialLogger::LogHex((uint32_t)(g_fb.addr >> 32));
        SerialLogger::LogHex((uint32_t)(g_fb.addr));
        SerialLogger::Log(" ");
        SerialLogger::LogDec(g_fb.width);
        SerialLogger::Log("x");
        SerialLogger::LogDec(g_fb.height);
        SerialLogger::Log("x");
        SerialLogger::LogDec(g_fb.bpp);
        SerialLogger::Log("\r\n");
    }

    void UpdateFramebuffer(uint64_t addr, uint32_t pitch,
                           uint32_t width, uint32_t height, uint8_t bpp) {
        if (addr == 0 || width == 0 || height == 0) return;
        if (bpp != 16 && bpp != 24 && bpp != 32) return;
        g_fb.addr = addr;
        g_fb.pitch = pitch;
        g_fb.width = width;
        g_fb.height = height;
        g_fb.bpp = bpp;
        g_fb.valid = true;
        SerialLogger::Log("Panic::UpdateFB: 0x");
        SerialLogger::LogHex((uint32_t)(addr >> 32));
        SerialLogger::LogHex((uint32_t)addr);
        SerialLogger::Log(" ");
        SerialLogger::LogDec(width);
        SerialLogger::Log("x");
        SerialLogger::LogDec(height);
        SerialLogger::Log("x");
        SerialLogger::LogDec(bpp);
        SerialLogger::Log("\r\n");
    }

    [[noreturn]] void KeBugCheckEx(uint32_t stop_code,
                                   uint64_t param1,
                                   uint64_t param2,
                                   uint64_t param3,
                                   uint64_t param4,
                                   const char* reason,
                                   const char* file,
                                   uint32_t line) {
        asm volatile("cli");

        g_dump.stop_code = stop_code;
        g_dump.param1 = param1;
        g_dump.param2 = param2;
        g_dump.param3 = param3;
        g_dump.param4 = param4;
        g_dump.reason = reason;
        g_dump.file = file;
        g_dump.line = line;

        if (g_panicking) {
            g_dump.stop_code = StopCode::TRIPLE_FAULT_IMMINENT;
            g_dump.reason = "PANIC REENTRY - EMERGENCY EVACUATION";
            vga_panic_fallback();
            render_evacuation_screen();
            reboot_or_halt();
        }
        g_panicking = true;

        SerialLogger::Log("KeBugCheckEx: entered, stop=0x");
        SerialLogger::LogHex(stop_code);
        SerialLogger::Log("\r\n");

        // evacuation marker: get a bsod on screen immediately, before any
        // slower logging or secondary panic work begins.
        vga_panic_fallback();
        render_evacuation_screen();

        try_halt_other_cpus();
        SerialLogger::Log("KeBugCheckEx: serial_dump\r\n");
        serial_dump();

        // always write to vga text buffer as a fallback (visible in text mode)
        vga_panic_fallback();

        SerialLogger::Log("KeBugCheckEx: render_panic_screen (fb.valid=");
        SerialLogger::Log(g_fb.valid ? "YES" : "NO");
        SerialLogger::Log(")\r\n");
        render_panic_screen();

        // persist a full minidump to the reserved physical page so the next
        // boot can recover it after kvfs (and this panic's reboot) wipe ram. (satoru)
        write_minidump();

        // Persist the dump to /var/crash/last.dmp via KVFS so the next
        // boot can pick it up and present it to the user.  KVFS lives in
        // RAM, but the installer wires it through to the real disk on
        // shutdown  -  and the dump is also visible to the in-RAM
        // /var/crash/ tree exposed via the /proc-style virtual fs.
        //
        // Use TryWriteCrashDump: this panic can fire from an exception that
        // interrupted a process holding g_vfs_lock. The KVFS public API now
        // takes that (non-recursive) lock, so a normal WriteFile here would
        // spin forever on the dying CPU and silently lose the BSOD. The Try
        // variant skips persistence if the lock is held (the physical-RAM
        // minidump from write_minidump() above is the real recovery path) so
        // we always reach reboot_or_halt() and show the bugcheck screen. (satoru)
        if (KVFS::TryWriteCrashDump("/var/crash/last.dmp", &g_dump, sizeof(g_dump))) {
            SerialLogger::Log("KeBugCheckEx: dump written to /var/crash/last.dmp\r\n");
        } else {
            SerialLogger::Log("KeBugCheckEx: KVFS dump skipped (lock held / no root)\r\n");
        }

        SerialLogger::Log("KeBugCheckEx: render done, halting\r\n");
        reboot_or_halt();
    }

    [[noreturn]] void BugCheckFromInterrupt(InterruptFrame* frame, const char* exception_name) {
        uint32_t code = StopCode::KERNEL_FATAL;
        if (!frame) {
            KeBugCheckEx(code, 0, 0, 0, 0, "UNKNOWN EXCEPTION", __FILE__, (uint32_t)__LINE__);
        }

        switch ((uint32_t)frame->vector) {
            case 0:  code = StopCode::DIVIDE_BY_ZERO; break;
            case 1:  code = StopCode::DEBUG_EXCEPTION; break;
            case 2:  code = StopCode::NMI; break;
            case 3:  code = StopCode::BREAKPOINT; break;
            case 4:  code = StopCode::OVERFLOW; break;
            case 5:  code = StopCode::BOUND_RANGE_EXCEEDED; break;
            case 6:  code = StopCode::INVALID_OPCODE; break;
            case 7:  code = StopCode::DEVICE_NOT_AVAILABLE; break;
            case 8:  code = StopCode::DOUBLE_FAULT; break;
            case 10: code = StopCode::INVALID_TSS; break;
            case 11: code = StopCode::SEGMENT_NOT_PRESENT; break;
            case 12: code = StopCode::STACK_SEGMENT_FAULT; break;
            case 13: code = StopCode::GENERAL_PROTECTION; break;
            case 14: code = StopCode::PAGE_FAULT; break;
            case 16: code = StopCode::X87_FP_EXCEPTION; break;
            case 17: code = StopCode::ALIGNMENT_CHECK; break;
            case 18: code = StopCode::MACHINE_CHECK; break;
            case 19: code = StopCode::SIMD_FP_EXCEPTION; break;
            case 20: code = StopCode::VIRT_EXCEPTION; break;
            case 21: code = StopCode::CONTROL_PROTECTION; break;
            case 30: code = StopCode::SECURITY_EXCEPTION; break;
            default: code = StopCode::KERNEL_FATAL; break;
        }

        g_dump.vector = (uint32_t)frame->vector;
        g_dump.rip = frame->rip;
        g_dump.cs = frame->cs;
        g_dump.rsp = frame->rsp;
        g_dump.ss = frame->ss;
        g_dump.rflags = frame->rflags;
        g_dump.cr2 = frame->cr2;
        g_dump.error_code = frame->error_code;
        g_dump.rax = frame->rax;
        g_dump.rbx = frame->rbx;
        g_dump.rcx = frame->rcx;
        g_dump.rdx = frame->rdx;
        g_dump.rsi = frame->rsi;
        g_dump.rdi = frame->rdi;
        g_dump.rbp = frame->rbp;
        // extended regs from the fault frame for the minidump. (satoru)
        g_dump.r8  = frame->r8;
        g_dump.r9  = frame->r9;
        g_dump.r10 = frame->r10;
        g_dump.r11 = frame->r11;
        g_dump.r12 = frame->r12;
        g_dump.r13 = frame->r13;
        g_dump.r14 = frame->r14;
        g_dump.r15 = frame->r15;
        g_dump.have_extended = true;

        KeBugCheckEx(code,
                     frame->rip,
                     frame->error_code,
                     frame->cr2,
                     frame->vector,
                     exception_name ? exception_name : "CPU EXCEPTION",
                     __FILE__,
                     (uint32_t)__LINE__);
    }

    bool ScanCrashDumpAtBoot() {
        // the dump (if any) survives at this fixed identity-mapped physical
        // address across the panic-triggered reboot. (satoru)
        volatile KuronoMiniDump* d = (volatile KuronoMiniDump*)(uintptr_t)MiniDump::PHYS_ADDR;

        // make sure we observe whatever the crashing kernel flushed. (satoru)
        __asm__ __volatile__("mfence" ::: "memory");
        if (d->magic != MiniDump::MAGIC) {
            return false;  // no prior crash to recover (satoru)
        }

        // sanity-clamp the recorded size so a corrupt header can't make us copy
        // past the reserved region. (satoru)
        uint32_t copy_len = d->size;
        if (copy_len == 0 || copy_len > MiniDump::MAX_BYTES) {
            copy_len = (uint32_t)sizeof(KuronoMiniDump);
        }

        // build /var/log/kurono_crash_<timestamp>.dmp. fall back to a fixed
        // suffix when the rtc timestamp was not valid at panic time. (satoru)
        char ts[12];
        if (d->time_valid && d->unix_time != 0) {
            to_dec(d->unix_time, ts);
        } else {
            ts[0] = '0'; ts[1] = 0;
        }

        char path[64];
        const char* prefix = "/var/log/kurono_crash_";
        uint32_t pi = 0;
        for (uint32_t i = 0; prefix[i] && pi < sizeof(path) - 1; i++) path[pi++] = prefix[i];
        for (uint32_t i = 0; ts[i] && pi < sizeof(path) - 1; i++) path[pi++] = ts[i];
        const char* suffix = ".dmp";
        for (uint32_t i = 0; suffix[i] && pi < sizeof(path) - 1; i++) path[pi++] = suffix[i];
        path[pi] = 0;

        bool written = false;
        if (KVFS::GetRoot()) {
            KVFS::Mkdirs("/var/log");
            // KuronoMiniDump is a flat pod  -  cast away volatile for the copy; the
            // crashing kernel has long since finished writing it. (satoru)
            int r = KVFS::WriteFile(path, (const void*)(uintptr_t)MiniDump::PHYS_ADDR, copy_len);
            written = (r >= 0);
            if (written) {
                SerialLogger::Log("ScanCrashDumpAtBoot: recovered crash dump to ");
                SerialLogger::Log(path);
                SerialLogger::Log("\r\n");
            } else {
                SerialLogger::Log("ScanCrashDumpAtBoot: KVFS write FAILED for ");
                SerialLogger::Log(path);
                SerialLogger::Log("\r\n");
            }
        } else {
            SerialLogger::Log("ScanCrashDumpAtBoot: KVFS not ready, skipping copy\r\n");
        }

        // clear the magic so the dump is recovered exactly once, regardless of
        // whether the kvfs copy succeeded  -  leaving a stale magic would re-fire
        // the recovery notice on every subsequent boot. a store fence is enough
        // here: a full cache flush is only needed on the write side (which must
        // reach ram before the panic reboot). (satoru)
        d->magic = 0;
        __asm__ __volatile__("sfence" ::: "memory");

        return true;
    }
}

// ----- Stack canary support -----
// Compiled freestanding code uses -fno-stack-protector, but third-party
// objects (precompiled crt0, llvm runtimes, etc.) may emit canary checks.
// Provide the symbols so linking succeeds and any actual canary failure
// routes to the kernel panic path with a real BSOD.
extern "C" {
    uintptr_t __stack_chk_guard = 0xDEADC0DE5A5A5A5AULL;

    void __stack_chk_fail(void) {
        KernelPanic::KeBugCheckEx(StopCode::SECURITY_EXCEPTION,
                                   0, 0, 0, 0,
                                   "stack smashing detected (__stack_chk_fail)",
                                   __FILE__, (uint32_t)__LINE__);
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    void __stack_chk_fail_local(void) { __stack_chk_fail(); }
}

// ----- KASLR offset bookkeeping -----
// We don't yet relocate the kernel image at boot (linker script fixes us
// at -2 GB), but we expose a synthetic random offset so /proc/kallsyms and
// similar consumers see varying addresses across boots.  Real KASLR will
// happen in kurono_boot.asm before jumping to kernel_main.
extern "C" uint64_t kernel_kaslr_offset(void) {
    static uint64_t off = 0;
    if (!off) {
        uint64_t tsc;
        __asm__ __volatile__("rdtsc" : "=A"(tsc));
        off = (tsc & 0x1FFFFFu) & ~0xFFFu;
        if (!off) off = 0x100000;
    }
    return off;
}
