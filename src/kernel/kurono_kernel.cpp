// kurono os enhanced kernel - full desktop edition
// complete bare-metal os with hybrid kernel, desktop environment and apps

#include "types.h"
#include "multiboot.h"
#include "system.h"
#include "heap.h"
#include "time.h"
#include "memory_mgr.h"
#include "panic.h"
#include "../drivers/serial.h"
#include "../drivers/display.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../media/mediadecoder.h"
#include "../ui/gui.h"
#include "../ui/lockscreen.h"
#include "../ui/font.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../apps/calculator.h"
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../apps/task_manager.h"
#include "../hal/hal.h"
#include "../fs/vfs.h"
#include "../fs/kvfs.h"
#include "../proc/scheduler.h"
#include "../tests/test_suite.h"
#include "../system/input_manager.h"
#include "../system/logging.h"
#include "../system/installer.h"
#include "../system/ui_config.h"
#include "../shell/shell.h"
#include "../ui/wallpaper.h"
#include "../shell/linux_cmds.h"
#include "../shell/windows_cmds.h"
#include "../kcl/kcl.h"
#include "../security/supr.h"
#include "../packages/pkgmgr.h"
#include "../net/network.h"
#include "../drivers/audio.h"
#include "../drivers/e1000.h"
#include "../linux/dual_boot.h"
#include "../linux/linux_init.h"
#include "../linux/linux_netbridge.h"
#include "../linux/linux_drivers.h"
#include "../virt/vmm.h"
#include "../virt/vdevices.h"
#include "../virt/iommu.h"
#include "../virt/hypervisor.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/display_mgr.h"
#include "../drivers/ac97.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/gpu_probe.h"
#include "../../logo.h"
#include "../media/embedded_media.h"

// helper: string comparison (no libc)
static bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool stcontains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (const char* h = haystack; *h; h++) {
        const char* p = h;
        const char* n = needle;
        while (*p && *n && *p == *n) { p++; n++; }
        if (*n == 0) return true;
    }
    return false;
}

static int stlen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void stcopy(char* dst, const char* src, int max_len) {
    if (!dst || max_len < 1) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void stappend(char* dst, const char* src, int max_len) {
    if (!dst || max_len < 1) return;
    int n = stlen(dst);
    int i = 0;
    if (src) {
        while (src[i] && n < max_len - 1) {
            dst[n++] = src[i++];
        }
    }
    dst[n] = 0;
}

// works on all x86 hardware in text mode. used before graphics init.
static volatile uint16_t* const VGA_TEXT = (volatile uint16_t*)0xB8000;
static int vga_row = 0, vga_col = 0;
static const int VGA_COLS = 80, VGA_ROWS = 25;

static void vga_clear() {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_TEXT[i] = 0x0F00 | ' '; // white on black
    vga_row = 0; vga_col = 0;
}

static void vga_puts(const char* s, uint8_t color = 0x0F) {
    while (*s) {
        if (*s == '\n') {
            vga_col = 0; vga_row++;
        } else {
            if (vga_row < VGA_ROWS && vga_col < VGA_COLS)
                VGA_TEXT[vga_row * VGA_COLS + vga_col] = ((uint16_t)color << 8) | (uint8_t)*s;
            vga_col++;
        }
        if (vga_row >= VGA_ROWS) vga_row = VGA_ROWS - 1; // stop scrolling, stay on last line
        s++;
    }
}

static void vga_puthex(uint32_t val) {
    const char* hex = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { buf[i] = hex[val & 0xF]; val >>= 4; }
    vga_puts(buf);
}

static void vga_u32_to_dec(uint32_t val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) { out[0] = '0'; out[1] = 0; return; }
    char rev[16];
    int ri = 0;
    while (val && ri < 15) { rev[ri++] = (char)('0' + (val % 10)); val /= 10; }
    int oi = 0;
    while (ri > 0 && oi < max_len - 1) out[oi++] = rev[--ri];
    out[oi] = 0;
}

static void vga_write_line(int row, const char* s, uint8_t color = 0x0F) {
    if (row < 0 || row >= VGA_ROWS) return;
    int base = row * VGA_COLS;
    for (int i = 0; i < VGA_COLS; i++) VGA_TEXT[base + i] = ((uint16_t)color << 8) | ' ';
    if (!s) return;
    for (int i = 0; s[i] && i < VGA_COLS; i++) VGA_TEXT[base + i] = ((uint16_t)color << 8) | (uint8_t)s[i];
}

struct EmergencyLine {
    char text[VGA_COLS + 1];
    uint8_t color;
};

static EmergencyLine g_emergency_lines[18];
static int g_emergency_line_count = 0;

static void emergency_clear_lines() {
    g_emergency_line_count = 0;
    for (int i = 0; i < 18; i++) {
        g_emergency_lines[i].text[0] = 0;
        g_emergency_lines[i].color = 0x0F;
    }
}

static void emergency_push_line(const char* text, uint8_t color = 0x0F) {
    if (g_emergency_line_count >= 18) {
        for (int i = 1; i < 18; i++) g_emergency_lines[i - 1] = g_emergency_lines[i];
        g_emergency_line_count = 17;
    }
    stcopy(g_emergency_lines[g_emergency_line_count].text, text ? text : "", VGA_COLS + 1);
    g_emergency_lines[g_emergency_line_count].color = color;
    g_emergency_line_count++;
}

static void emergency_push_output(const char* text, uint8_t color = 0x0F) {
    char line[VGA_COLS + 1];
    int pos = 0;
    if (!text || !*text) {
        emergency_push_line("(ok)", 0x0A);
        return;
    }

    for (int i = 0; text[i]; i++) {
        char c = text[i];

        if (c == '\x1b') {
            i++;
            if (text[i] == '[') {
                while (text[i] && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) i++;
            }
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = 0;
            emergency_push_line(line, color);
            pos = 0;
            line[0] = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        line[pos++] = c;
        if (pos >= VGA_COLS) {
            line[pos] = 0;
            emergency_push_line(line, color);
            pos = 0;
            line[0] = 0;
        }
    }
    if (pos > 0) {
        line[pos] = 0;
        emergency_push_line(line, color);
    }
}

static void emergency_render(const char* input) {
    vga_clear();
    vga_write_line(0, "Kurono Emergency Kernel", 0x0C);
    vga_write_line(1, "Minimal recovery shell | same kernel, emergency boot profile", 0x0F);
    vga_write_line(2, "Commands: help ls cat tail dmesg reboot | shortcuts: bootlog syslog", 0x0B);
    vga_write_line(3, "seriallog clear", 0x0B);
    for (int i = 0; i < 18; i++) {
        if (i < g_emergency_line_count) vga_write_line(4 + i, g_emergency_lines[i].text, g_emergency_lines[i].color);
        else vga_write_line(4 + i, "", 0x07);
    }
    char prompt[VGA_COLS + 1];
    stcopy(prompt, "emergency> ", sizeof(prompt));
    stappend(prompt, input ? input : "", sizeof(prompt));
    vga_write_line(22, prompt, 0x0F);
    vga_write_line(23, "Logs: /system/boot/boot.log | /system/logs/system.log | /system/logs/serial.log", 0x08);
    vga_write_line(24, "Enter=run  Backspace=edit  Reboot command exits recovery", 0x08);
}

static void emergency_run_shell() {
    char input[256];
    int input_len = 0;
    input[0] = 0;

    emergency_clear_lines();
    emergency_push_line("Recovery shell ready.", 0x0A);
    emergency_push_line("Use 'bootlog', 'syslog', or 'seriallog' to inspect logs.", 0x07);
    emergency_render(input);

    while (true) {
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);
        Scheduler::Tick();
        Keyboard::Poll();

        bool changed = false;
        while (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\r' || c == '\n') {
                changed = true;
                if (input_len > 0) {
                    char cmd[512];
                    char out[SHELL_OUTPUT_BUF];
                    char cmdline[VGA_COLS + 1];
                    stcopy(cmd, input, sizeof(cmd));
                    stcopy(cmdline, "emergency> ", sizeof(cmdline));
                    stappend(cmdline, input, sizeof(cmdline));
                    emergency_push_line(cmdline, 0x0F);
                    if (streq(cmd, "clear")) {
                        emergency_clear_lines();
                    } else {
                        if (streq(cmd, "bootlog")) stcopy(cmd, "tail -n 40 /system/boot/boot.log", sizeof(cmd));
                        else if (streq(cmd, "syslog")) stcopy(cmd, "tail -n 40 /system/logs/system.log", sizeof(cmd));
                        else if (streq(cmd, "seriallog")) stcopy(cmd, "tail -n 40 /system/logs/serial.log", sizeof(cmd));
                        out[0] = 0;
                        KuronoShell::Execute(cmd, out, sizeof(out));
                        emergency_push_output(out, 0x07);
                    }
                }
                input_len = 0;
                input[0] = 0;
            } else if (c == 8 || c == 127) {
                if (input_len > 0) {
                    input[--input_len] = 0;
                    changed = true;
                }
            } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
                if (input_len < (int)sizeof(input) - 1) {
                    input[input_len++] = c;
                    input[input_len] = 0;
                    changed = true;
                }
            }
        }

        if (changed) emergency_render(input);
    }
}

// early framebuffer pixel writer - works before graphics::init
// handles 16, 24, and 32 bpp for maximum hardware compatibility
static void early_fb_fill(uint64_t fb_addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp, uint32_t color) {
    if (!fb_addr || !w || !h || bpp < 15) return;
    uint8_t* fb = (uint8_t*)(uintptr_t)fb_addr;
    uint32_t bytes_pp = bpp / 8;
    uint8_t r = (uint8_t)(color >> 16);
    uint8_t g = (uint8_t)(color >> 8);
    uint8_t b = (uint8_t)(color);
    for (uint32_t y = 0; y < h && y < 32; y++) { // top 32 rows = color bar
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = fb + y * pitch + x * bytes_pp;
            if (bytes_pp == 4) {
                p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
            } else if (bytes_pp == 3) {
                p[0] = b; p[1] = g; p[2] = r;
            } else if (bytes_pp == 2) {
                // rgb565: rrrrrggggggbbbbb
                uint16_t px = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
                *(uint16_t*)p = px;
            }
        }
    }
    // flush cpu caches so pixels reach gpu vram on real hardware
    __asm__ __volatile__("wbinvd" ::: "memory");
}

// minimal 8x8 built-in font bitmap for boot diagnostics.
// works before any subsystem init  -  writes directly to the framebuffer.
// this is essential for bare-metal debugging when there's no serial port.
static uint64_t _efb_addr = 0;
static uint32_t _efb_pitch = 0, _efb_w = 0, _efb_h = 0;
static uint8_t  _efb_bpp = 0;
static int _efb_cx = 4, _efb_cy = 36; // cursor position (below the color bar)

// tiny 8x8 font  -  covers printable ascii 0x20-0x7e only
// each char is 8 bytes (1 byte per row, msb-first)
static const uint8_t _efb_font[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (0x20)
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, // '0'
    {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00}, // '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '='
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // '>'
    {0x7C,0xC6,0x06,0x0C,0x18,0x00,0x18,0x00}, // '?'
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'a'
    {0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00}, // 'b'
    {0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00}, // 'd'
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00}, // 'e'
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00}, // 'f'
    {0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7E,0x00}, // 'g'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'h'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'i'
    {0x1E,0x06,0x06,0x06,0xC6,0xC6,0x7C,0x00}, // 'j'
    {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00}, // 'k'
    {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00}, // 'l'
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 'm'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'n'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00}, // 'p'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 'q'
    {0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00}, // 'r'
    {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, // 's'
    {0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 't'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'u'
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'v'
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 'w'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 'x'
    {0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'y'
    {0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00}, // 'z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE}, // '_'
    {0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00}, // 'a'
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}, // 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}, // 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e'
    {0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x00}, // 'f'
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C}, // 'g'
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x0E,0x06,0x06,0xC6,0xC6,0x7C}, // 'j'
    {0xC0,0xC0,0xCC,0xD8,0xF0,0xD8,0xCC,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xCC,0xFE,0xD6,0xC6,0xC6,0x00}, // 'm'
    {0x00,0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0x00,0x00,0xFC,0xC6,0xC6,0xFC,0xC0,0xC0}, // 'p'
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x06}, // 'q'
    {0x00,0x00,0xDC,0xE6,0xC0,0xC0,0xC0,0x00}, // 'r'
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 's'
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00}, // 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'v'
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x'
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C}, // 'y'
    {0x00,0x00,0xFE,0x0C,0x38,0x60,0xFE,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // '|'
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

static void early_fb_putpx(int x, int y, uint32_t color) {
    if (!_efb_addr || x < 0 || y < 0 || (uint32_t)x >= _efb_w || (uint32_t)y >= _efb_h) return;
    uint8_t* fb = (uint8_t*)(uintptr_t)_efb_addr;
    uint32_t bpp = _efb_bpp / 8;
    uint8_t* p = fb + y * _efb_pitch + x * bpp;
    if (bpp == 4) { p[0]=(uint8_t)color; p[1]=(uint8_t)(color>>8); p[2]=(uint8_t)(color>>16); p[3]=0xFF; }
    else if (bpp == 3) { p[0]=(uint8_t)color; p[1]=(uint8_t)(color>>8); p[2]=(uint8_t)(color>>16); }
}

static void early_fb_putc(char c, uint32_t fg) {
    if (c == '\n') { _efb_cx = 4; _efb_cy += 10; return; }
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t* glyph = _efb_font[c - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                early_fb_putpx(_efb_cx + col, _efb_cy + row, fg);
        }
    }
    _efb_cx += 8;
    if ((uint32_t)_efb_cx >= _efb_w - 8) { _efb_cx = 4; _efb_cy += 10; }
}

static void early_fb_puts(const char* s, uint32_t fg = 0x00FF00) {
    while (*s) early_fb_putc(*s++, fg);
}

static void early_fb_puthex32(uint32_t val, uint32_t fg = 0x00FF00) {
    const char* hex = "0123456789ABCDEF";
    early_fb_putc('0', fg); early_fb_putc('x', fg);
    for (int i = 28; i >= 0; i -= 4)
        early_fb_putc(hex[(val >> i) & 0xF], fg);
}

// initialize the early fb text context after we know the fb parameters
static void early_fb_init_text(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp) {
    _efb_addr = addr; _efb_pitch = pitch; _efb_w = w; _efb_h = h; _efb_bpp = bpp;
    _efb_cx = 4; _efb_cy = 36;
}

// flush the early fb writes to the display  -  tries both wbinvd and sfence
static void early_fb_flush() {
    __asm__ __volatile__("wbinvd" ::: "memory");
}

//  multiboot2 → multiboot1 compatibility shim
//  converts mb2 tagged info into a static multiboot_info_t so all existing
//  kernel code works unchanged with either boot protocol.
static multiboot_info_t mb2_compat_info;
// buffer for converted mb1-format mmap entries (each 24 bytes, room for 170)
static uint8_t mb2_mmap_buf[4096];

static void convert_mb2_to_mb1(uint64_t mb_addr) {
    // mb2 boot info starts with: uint32_t total_size, uint32_t reserved
    // then tags follow, each 8-byte aligned
    uint8_t* base = (uint8_t*)(uintptr_t)mb_addr;
    uint32_t total_size = *(uint32_t*)base;
    uint8_t* tag = base + 8;
    uint8_t* end = base + total_size;

    for (int i = 0; i < (int)sizeof(mb2_compat_info); i++)
        ((uint8_t*)&mb2_compat_info)[i] = 0;

    while (tag < end) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag + 4);
        if (type == 0 || size < 8) break; // end tag or malformed

        switch (type) {
            case 1: { // boot command line
                mb2_compat_info.flags |= (1u << 2);
                mb2_compat_info.cmdline = (uint32_t)(uintptr_t)(tag + 8);
                break;
            }
            case 4: { // basic memory info
                mb2_compat_info.flags |= (1u << 0);
                mb2_compat_info.mem_lower = *(uint32_t*)(tag + 8);
                mb2_compat_info.mem_upper = *(uint32_t*)(tag + 12);
                break;
            }
            case 6: { // memory map
                // convert mb2 mmap entries to mb1 format in our static buffer
                // mb2 entry: {u64 base, u64 length, u32 type, u32 reserved} = variable size
                // mb1 entry: {u32 size=20, u64 base, u64 length, u32 type} = 24 bytes
                uint32_t entry_size = *(uint32_t*)(tag + 8);
                if (entry_size == 0) entry_size = 24; // sane default
                uint8_t* src = tag + 16;
                uint8_t* src_end = tag + size;
                uint8_t* dst = mb2_mmap_buf;
                uint8_t* dst_end = mb2_mmap_buf + sizeof(mb2_mmap_buf);
                while (src + entry_size <= src_end && dst + 24 <= dst_end) {
                    uint64_t mbase  = *(uint64_t*)(src + 0);
                    uint64_t mlen   = *(uint64_t*)(src + 8);
                    uint32_t mtype  = *(uint32_t*)(src + 16);
                    // write mb1-format entry: size(4) + base(8) + length(8) + type(4)
                    *(uint32_t*)(dst + 0)  = 20;    // size of rest
                    *(uint64_t*)(dst + 4)  = mbase;
                    *(uint64_t*)(dst + 12) = mlen;
                    *(uint32_t*)(dst + 20) = mtype;
                    src += entry_size;
                    dst += 24;
                }
                mb2_compat_info.flags |= (1u << 6);
                mb2_compat_info.mmap_addr = (uint32_t)(uintptr_t)mb2_mmap_buf;
                mb2_compat_info.mmap_length = (uint32_t)(dst - mb2_mmap_buf);
                break;
            }
            case 8: { // framebuffer info
                mb2_compat_info.flags |= (1u << 12);
                mb2_compat_info.framebuffer_addr  = *(uint64_t*)(tag + 8);
                mb2_compat_info.framebuffer_pitch  = *(uint32_t*)(tag + 16);
                mb2_compat_info.framebuffer_width  = *(uint32_t*)(tag + 20);
                mb2_compat_info.framebuffer_height = *(uint32_t*)(tag + 24);
                mb2_compat_info.framebuffer_bpp    = *(uint8_t*)(tag + 28);
                mb2_compat_info.framebuffer_type   = *(uint8_t*)(tag + 29);
                break;
            }
            // module tags (type 3) have variable format  -  skip for now.
            // boot loader name (type 2), etc.  -  not needed.
        }

        // advance to next tag (8-byte aligned)
        uint32_t advance = (size + 7u) & ~7u;
        tag += advance;
    }
}

extern "C" void kernel_main(uint64_t magic, uint64_t mb_addr) {
    SerialLogger::Init();
    SerialLogger::Log("Kurono OS Starting...\r\n");
    RuntimeLog::LogBoot("kernel_main entered");

    // if booted via mb2, convert the tagged info to mb1 format so all
    // existing kernel code works unchanged.
    if (magic == 0x36d76289) {
        SerialLogger::Log("Multiboot2 detected  -  converting to MB1 compat\r\n");
        RuntimeLog::LogBoot("multiboot2 detected");
        convert_mb2_to_mb1(mb_addr);
        mb_addr = (uint64_t)(uintptr_t)&mb2_compat_info;
        magic = 0x2BADB002;     // pretend it's mb1 from here on
    }

    vga_clear();
    vga_puts("Kurono OS booting...\n", 0x0A);

    // this gives immediate visual feedback even before any subsystem init
    multiboot_info_t* mbi_early = (multiboot_info_t*)(uintptr_t)mb_addr;
    if (magic == 0x2BADB002) {
        KernelPanic::Initialize(mbi_early);
    }
    bool has_early_fb = false;
    if (magic == 0x2BADB002 && (mbi_early->flags & (1u << 12)) && mbi_early->framebuffer_addr != 0
        && mbi_early->framebuffer_width > 0 && mbi_early->framebuffer_height > 0
        && mbi_early->framebuffer_type != 2) {
        uint64_t fb = mbi_early->framebuffer_addr;
        uint32_t pitch = mbi_early->framebuffer_pitch;
        uint32_t fw = mbi_early->framebuffer_width;
        uint32_t fh = mbi_early->framebuffer_height;
        uint8_t  fbpp = mbi_early->framebuffer_bpp;

        // draw a bright green bar at top of screen (32px)  -  proves we're alive
        uint8_t* fbp = (uint8_t*)(uintptr_t)fb;
        uint32_t bpp = fbpp / 8;
        for (uint32_t y = 0; y < 32 && y < fh; y++) {
            for (uint32_t x = 0; x < fw; x++) {
                uint8_t* p = fbp + y * pitch + x * bpp;
                if (bpp == 4) { p[0]=0x00; p[1]=0xFF; p[2]=0x00; p[3]=0xFF; }
                else if (bpp == 3) { p[0]=0x00; p[1]=0xFF; p[2]=0x00; }
            }
        }
        __asm__ __volatile__("wbinvd" ::: "memory");

        // init early text renderer and show diagnostic info on the fb
        early_fb_init_text(fb, pitch, fw, fh, fbpp);
        early_fb_puts("Kurono OS booting...\n", 0xFFFFFF);
        early_fb_puts("FB: ", 0x00FF00);
        early_fb_puthex32((uint32_t)(fb >> 32), 0x00FF00);
        early_fb_puthex32((uint32_t)(fb & 0xFFFFFFFF), 0x00FF00);
        early_fb_puts(" ", 0x00FF00);
        early_fb_puthex32(fw, 0x00FF00);
        early_fb_putc('x', 0x00FF00);
        early_fb_puthex32(fh, 0x00FF00);
        early_fb_putc('\n', 0);
        early_fb_flush();

        has_early_fb = true;
        SerialLogger::Log("Early FB: drew green bar + text\r\n");
    } else {
        SerialLogger::Log("Early FB: SKIPPED (flags=0x");
        if (magic == 0x2BADB002) {
            SerialLogger::LogHex(mbi_early->flags);
            SerialLogger::Log(" addr=0x");
            SerialLogger::LogHex((uint32_t)(mbi_early->framebuffer_addr & 0xFFFFFFFF));
            SerialLogger::Log(" type=");
            SerialLogger::LogDec((int)mbi_early->framebuffer_type);
        } else {
            SerialLogger::Log("BAD_MAGIC");
        }
        SerialLogger::Log(")\r\n");
    }

    // check multiboot magic
    if (magic != 0x2BADB002) {
        vga_puts("FATAL: Bad multiboot magic!\n", 0x0C);
        if (has_early_fb) { early_fb_puts("FATAL: Bad multiboot magic!\n", 0xFF0000); early_fb_flush(); }
        SerialLogger::Log("FATAL: Invalid Multiboot Magic!\r\n");
        while (true) { __asm__ __volatile__("cli; hlt"); }
    }

    bool boot_text_only = false;
    bool boot_console_realtime = false;
    bool boot_emergency = false;
    const char* boot_cmdline = nullptr;
    if (mbi_early && (mbi_early->flags & (1u << 2)) && mbi_early->cmdline != 0) {
        boot_cmdline = (const char*)(uintptr_t)mbi_early->cmdline;
        if (stcontains(boot_cmdline, "kurono_mode=text") || stcontains(boot_cmdline, "kurono.text=1")) {
            boot_text_only = true;
        }
        if (stcontains(boot_cmdline, "kurono_mode=console") || stcontains(boot_cmdline, "kurono.console=1")) {
            boot_console_realtime = true;
        }
        if (stcontains(boot_cmdline, "kurono_mode=emergency") || stcontains(boot_cmdline, "kurono.emergency=1")) {
            boot_emergency = true;
        }
    }
    bool force_text_mode = boot_text_only || boot_console_realtime || boot_emergency;

    vga_puts("Multiboot OK\n");
    if (has_early_fb) { early_fb_puts("[OK] Multiboot\n", 0x00FF00); early_fb_flush(); }
    SerialLogger::Log("Multiboot OK\r\n");
    RuntimeLog::LogBoot("multiboot handshake complete");
    if (boot_cmdline) {
        SerialLogger::Log("Boot cmdline: ");
        SerialLogger::Log(boot_cmdline);
        SerialLogger::Log("\r\n");
    }
    if (boot_text_only) {
        SerialLogger::Log("Boot mode: PURE TEXT (GRUB console path)\r\n");
        vga_puts("Mode: PURE TEXT\n", 0x0B);
    } else if (boot_emergency) {
        SerialLogger::Log("Boot mode: EMERGENCY RECOVERY\r\n");
        vga_puts("Mode: EMERGENCY RECOVERY\n", 0x0C);
    } else if (boot_console_realtime) {
        SerialLogger::Log("Boot mode: REALTIME CONSOLE\r\n");
        vga_puts("Mode: REALTIME CONSOLE\n", 0x0B);
    }

    // initialize core subsystems
    vga_puts("HAL init...\n");
    if (has_early_fb) { early_fb_puts("[..] HAL ", 0xFFFF00); early_fb_flush(); }
    SerialLogger::Log("[1] HAL::Init\r\n");
    HAL::Init();
    if (has_early_fb) { early_fb_puts("OK\n", 0x00FF00); early_fb_flush(); }
    vga_puts("Memory init...\n");
    if (has_early_fb) { early_fb_puts("[..] Memory ", 0xFFFF00); early_fb_flush(); }
    SerialLogger::Log("[2] MemoryManager::Init\r\n");
    MemoryManager::Init(mb_addr);
    if (has_early_fb) { early_fb_puts("OK\n", 0x00FF00); early_fb_flush(); }
    vga_puts("Scheduler init...\n");
    SerialLogger::Log("[3] Scheduler::Init\r\n");
    Scheduler::Init();
    vga_puts("VFS init...\n");
    SerialLogger::Log("[4] VFS::Init\r\n");
    VFS::Init();
    if (has_early_fb) { early_fb_puts("[OK] Scheduler + VFS\n", 0x00FF00); early_fb_flush(); }
    vga_puts("Running tests...\n");
    SerialLogger::Log("[5] TestSuite::Run\r\n");
    // run diagnostics (after core init, before display)
    TestSuite::Run();
    vga_puts("Tests passed\n", 0x0A);
    if (has_early_fb) { early_fb_puts("[OK] Tests PASSED\n", 0x00FF00); early_fb_flush(); }
    SerialLogger::Log("[6] Post-TestSuite\r\n");

    multiboot_info_t* mbi = (multiboot_info_t*)mb_addr;

    Timer::Init(1000);
    TimeManager::SelectPIT(1000);
    TimeManager::Init();

    // note: interrupts stay disabled. the kernel is fully polling-based
    // (pit counter read, keyboard/mouse i/o ports). this avoids whpx
    // compatibility issues with hardware interrupt delivery.

    SerialLogger::Log("Initializing display...\r\n");
    vga_puts("Display init...\n");
    if (has_early_fb) { early_fb_puts("[..] Display init...\n", 0xFFFF00); early_fb_flush(); }

    if (!force_text_mode) {
        SerialLogger::Log("[GpuProbe] Starting early GPU probe...\r\n");
        GpuProbe::ScanAll();
        GpuProbe::LogAll();
        if (has_early_fb) {
            const GpuProbeResult& gpr = GpuProbe::GetResult();
            early_fb_puts("[OK] GPUs found: ", 0x00FF00);
            char cnt = '0' + (char)(gpr.count < 10 ? gpr.count : 9);
            early_fb_putc(cnt, 0x00FF00);
            early_fb_puts("\n", 0);
            if (GpuProbe::IsOptimus()) {
                early_fb_puts("[!!] OPTIMUS detected\n", 0xFFFF00);
            } else if (GpuProbe::IsPowerXpress()) {
                early_fb_puts("[!!] PowerXpress detected\n", 0xFFFF00);
            }
            if (gpr.primary_idx >= 0) {
                early_fb_puts("Primary: ", 0xFFFFFF);
                early_fb_puts(gpr.gpus[gpr.primary_idx].desc, 0x00FF00);
                early_fb_puts("\n", 0);
            }
            early_fb_flush();
        }
    } else {
        SerialLogger::Log("Display: forced text mode from boot option\r\n");
        vga_puts("Display: forced text mode\n", 0x0E);
        if (has_early_fb) { early_fb_puts("[OK] Text mode (console)\n", 0x00FF00); early_fb_flush(); }
    }

    bool has_display = false;

    // dump all multiboot framebuffer fields to serial for debugging
    SerialLogger::Log("  MB flags=0x");
    SerialLogger::LogHex(mbi->flags);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("  fb_addr=0x");
    SerialLogger::LogHex((uint32_t)((mbi->framebuffer_addr >> 32) & 0xFFFFFFFF));
    SerialLogger::LogHex((uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF));
    SerialLogger::Log("\r\n");
    SerialLogger::Log("  fb_w=");
    SerialLogger::LogDec((int)mbi->framebuffer_width);
    SerialLogger::Log(" fb_h=");
    SerialLogger::LogDec((int)mbi->framebuffer_height);
    SerialLogger::Log(" bpp=");
    SerialLogger::LogDec((int)mbi->framebuffer_bpp);
    SerialLogger::Log(" type=");
    SerialLogger::LogDec((int)mbi->framebuffer_type);
    SerialLogger::Log(" pitch=");
    SerialLogger::LogDec((int)mbi->framebuffer_pitch);
    SerialLogger::Log("\r\n");

    // also dump vbe info if available (bit 11)
    if (mbi->flags & (1u << 11)) {
        SerialLogger::Log("  VBE: ctrl=0x");
        SerialLogger::LogHex(mbi->vbe_control_info);
        SerialLogger::Log(" mode_info=0x");
        SerialLogger::LogHex(mbi->vbe_mode_info);
        SerialLogger::Log(" mode=0x");
        SerialLogger::LogHex(mbi->vbe_mode);
        SerialLogger::Log("\r\n");
    }

    // 1. multiboot-provided framebuffer (grub on real hardware via efi gop)
    //    check bit 12 for framebuffer info. accept any type with valid dimensions.
    if (!force_text_mode && !has_display && (mbi->flags & (1u << 12)) && mbi->framebuffer_addr != 0
        && mbi->framebuffer_width > 0 && mbi->framebuffer_height > 0) {

        if (mbi->framebuffer_type == 2) {
            // type 2 = ega text mode framebuffer. don't use for graphics.
            SerialLogger::Log("Display: MB FB is text mode (type 2), skipping\r\n");
            vga_puts("Display: text mode FB (type 2)\n", 0x0E);
            if (has_early_fb) { early_fb_puts("[!!] FB type=2 (text mode)\n", 0xFF0000); early_fb_flush(); }
        } else {
            // type 0=indexed, 1=rgb, other=unknown but try anyway
            uintptr_t fb_phys = (uintptr_t)mbi->framebuffer_addr;
            SerialLogger::Log("Display: Using MB FB at 0x");
            SerialLogger::LogHex((uint32_t)(fb_phys >> 32));
            SerialLogger::LogHex((uint32_t)(fb_phys & 0xFFFFFFFF));
            SerialLogger::Log("\r\n");

            // on hybrid gpu laptops (optimus/powerxpress), the multiboot fb
            // address might not point to the gpu that drives the display.
            // gpuprobe reads the intel igpu's dspsurf register to cross-check.
            uintptr_t validated_fb = GpuProbe::ValidateFramebuffer(
                fb_phys, mbi->framebuffer_width, mbi->framebuffer_height,
                mbi->framebuffer_pitch, mbi->framebuffer_bpp);

            if (validated_fb != fb_phys) {
                SerialLogger::Log("Display: FB ADDRESS CORRECTED by GpuProbe!\r\n");
                SerialLogger::Log("Display:   Was: 0x");
                SerialLogger::LogHex((uint32_t)(fb_phys >> 32));
                SerialLogger::LogHex((uint32_t)(fb_phys & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                SerialLogger::Log("Display:   Now: 0x");
                SerialLogger::LogHex((uint32_t)(validated_fb >> 32));
                SerialLogger::LogHex((uint32_t)(validated_fb & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                fb_phys = validated_fb;

                if (has_early_fb) {
                    early_fb_puts("[!!] FB CORRECTED: ", 0xFFFF00);
                    early_fb_puthex32((uint32_t)(fb_phys >> 32), 0xFFFF00);
                    early_fb_puthex32((uint32_t)(fb_phys & 0xFFFFFFFF), 0xFFFF00);
                    early_fb_puts("\n", 0);
                    early_fb_flush();
                }
            }

            if (has_early_fb) {
                early_fb_puts("[OK] FB type=", 0x00FF00);
                early_fb_puthex32(mbi->framebuffer_type, 0x00FF00);
                early_fb_puts("\n", 0);
                early_fb_flush();
            }

            // init graphics first  -  this remaps fb pages to write-combining,
            // so the subsequent test fill doesn't need wbinvd.
            Graphics::Init(fb_phys, mbi->framebuffer_width,
                          mbi->framebuffer_height, mbi->framebuffer_pitch, mbi->framebuffer_bpp);
            has_display = true;

            if (has_early_fb) {
                early_fb_puts("[OK] Graphics::Init  WC=", 0x00FF00);
                early_fb_puts(Graphics::IsFramebufferWC() ? "YES" : "NO", 
                    Graphics::IsFramebufferWC() ? 0x00FF00 : 0xFF4444);
                early_fb_puts("\n", 0);
                early_fb_flush();
            }

            // immediate visual test: fill entire screen dark blue using
            // non-temporal stores to guarantee pixels reach gpu vram.
            // this replaces the old byte-by-byte loop that could be cached.
            uint8_t* fb = (uint8_t*)fb_phys;
            uint32_t fb_total = mbi->framebuffer_pitch * mbi->framebuffer_height;
            // fill a temp scanline with dark blue, then nt-copy each row
            {
                uint32_t bytes_pp = mbi->framebuffer_bpp / 8;
                uint32_t row_bytes = mbi->framebuffer_width * bytes_pp;
                // use a stack buffer for one scanline (max ~8kb for 1920x4)
                uint8_t scanline[8192];
                uint32_t fill_w = mbi->framebuffer_width;
                if (row_bytes > sizeof(scanline)) fill_w = sizeof(scanline) / bytes_pp;
                row_bytes = fill_w * bytes_pp;
                for (uint32_t x = 0; x < fill_w; x++) {
                    uint8_t* p = scanline + x * bytes_pp;
                    if (bytes_pp == 4) { p[0]=0x80; p[1]=0x00; p[2]=0x00; p[3]=0xFF; } // dark blue bgra
                    else if (bytes_pp == 3) { p[0]=0x80; p[1]=0x00; p[2]=0x00; }
                }
                // use nt copy to blit each scanline to the fb
                for (uint32_t y = 0; y < mbi->framebuffer_height; y++) {
                    // use inline nt stores for each row
                    uint8_t* dst = fb + y * mbi->framebuffer_pitch;
                    uint8_t* src = scanline;
                    uint32_t sz = row_bytes;
                    // 16-byte aligned nt copy
                    while (sz >= 16 && ((uintptr_t)dst & 15)) { *dst++ = *src++; sz--; }
                    while (sz >= 16) {
                        __asm__ __volatile__(
                            "movdqu (%0), %%xmm0; movntdq %%xmm0, (%1);"
                            :: "r"(src), "r"(dst) : "xmm0", "memory"
                        );
                        dst += 16; src += 16; sz -= 16;
                    }
                    while (sz > 0) { *dst++ = *src++; sz--; }
                }
                __asm__ __volatile__("sfence" ::: "memory");
            }
            (void)fb_total;

            if (has_early_fb) {
                // redraw text on the blue screen so user sees progress
                early_fb_init_text(fb_phys, mbi->framebuffer_pitch,
                    mbi->framebuffer_width, mbi->framebuffer_height, mbi->framebuffer_bpp);
                early_fb_puts("Kurono OS - Display OK\n", 0x00FF00);
                early_fb_puts("WC remap: ", 0xFFFFFF);
                early_fb_puts(Graphics::IsFramebufferWC() ? "SUCCESS" : "FAILED", 
                    Graphics::IsFramebufferWC() ? 0x00FF00 : 0xFF4444);
                early_fb_puts("\n", 0);
                early_fb_puts("Loading desktop...\n", 0xFFFF00);
                early_fb_flush();
            }

            SerialLogger::Log("Display: Multiboot framebuffer OK\r\n");
        }
    }

    // 2. vbe mode info fallback: if bit 11 is set (vbe info available) but
    //    bit 12 is not set, try reading vbe mode info structure directly
    if (!force_text_mode && !has_display && (mbi->flags & (1u << 11)) && mbi->vbe_mode_info != 0) {
        vbe_mode_info_t* vmi = (vbe_mode_info_t*)(uintptr_t)mbi->vbe_mode_info;
        if (vmi->physbase != 0 && vmi->Xres > 0 && vmi->Yres > 0 && vmi->bpp >= 15) {
            uint32_t pitch = vmi->pitch ? vmi->pitch : (vmi->Xres * (vmi->bpp / 8));
            Graphics::Init((uintptr_t)vmi->physbase, vmi->Xres, vmi->Yres, pitch, vmi->bpp);
            has_display = true;
            SerialLogger::Log("Display: VBE mode info fallback OK\r\n");
        }
    }

    // 3. bga (qemu/bochs only  -  not present on real hardware)
    //    try 24bpp first (bochs rfb safe), then 32bpp (qemu preferred)
    if (!force_text_mode && !has_display) {
        bool bga_ok = false;
        // check if bga hardware exists before attempting mode set
        if (BGA::IsAvailable()) {
            SerialLogger::Log("Display: BGA detected, trying modes...\r\n");
            if (BGA::Init(1024, 768, 32)) {
                bga_ok = true;
            }
            if (!bga_ok && BGA::Init(1024, 768, 24)) {
                bga_ok = true;
            }
            if (bga_ok) {
                Graphics::Init(BGA_FRAMEBUFFER_ADDR, BGA::width, BGA::height, BGA::pitch, (uint8_t)BGA::bpp);
                has_display = true;
                SerialLogger::Log("Display: BGA ");
                SerialLogger::LogDec(BGA::width);
                SerialLogger::Log("x");
                SerialLogger::LogDec(BGA::height);
                SerialLogger::Log("x");
                SerialLogger::LogDec(BGA::bpp);
                SerialLogger::Log(" OK\r\n");
            }
        } else {
            SerialLogger::Log("Display: BGA not available (real hardware?)\r\n");
        }
    }

    if (!has_display) {
        SerialLogger::Log("Display: No framebuffer! Showing diagnostics on VGA text.\r\n");
        // when no graphics framebuffer is available, use vga text mode
        // (0xb8000) to show diagnostic info so the user isn't staring
        // at pure black.
        vga_clear();
        vga_puts("=== Kurono OS  -  No Graphics Framebuffer ===", 0x0C);
        vga_puts("\n", 0x0C);
        vga_puts("\nMultiboot flags: ", 0x0E);
        vga_puthex(mbi->flags);
        vga_puts("\nFB addr:  0x", 0x0E);
        vga_puthex((uint32_t)(mbi->framebuffer_addr >> 32));
        vga_puthex((uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF));
        vga_puts("\nFB size:  ", 0x0E);
        vga_puthex(mbi->framebuffer_width);
        vga_puts(" x ", 0x0E);
        vga_puthex(mbi->framebuffer_height);
        vga_puts("\nFB bpp:   ", 0x0E);
        vga_puthex(mbi->framebuffer_bpp);
        vga_puts("\nFB type:  ", 0x0E);
        vga_puthex(mbi->framebuffer_type);
        vga_puts("\nFB pitch: ", 0x0E);
        vga_puthex(mbi->framebuffer_pitch);
        vga_puts("\n", 0x07);
        vga_puts("\n", 0x07);
        vga_puts("GRUB did not deliver a graphics framebuffer.", 0x0F);
        vga_puts("\nTry selecting 'Kurono OS (1024x768)' from GRUB menu.", 0x0B);
        vga_puts("\nOr check BIOS: enable CSM/Legacy or EFI GOP.", 0x0B);
        vga_puts("\n\nBit 12 (FB info): ", 0x0E);
        vga_puts((mbi->flags & (1u << 12)) ? "SET" : "NOT SET", 0x0F);
        vga_puts("\nBit 11 (VBE info): ", 0x0E);
        vga_puts((mbi->flags & (1u << 11)) ? "SET" : "NOT SET", 0x0F);
        if (mbi->flags & (1u << 11)) {
            vga_puts("\nVBE mode_info: 0x", 0x0E);
            vga_puthex(mbi->vbe_mode_info);
            vga_puts("\nVBE mode:      0x", 0x0E);
            vga_puthex(mbi->vbe_mode);
        }
        vga_puts("\n\nSystem halted  -  reboot and try a different GRUB entry.", 0x0C);
        // don't halt  -  let kernel continue so serial diag still works
    }

    // enable double buffering if we have a display
    if (has_display) {
        // detect actual display refresh rate via vga vsync timing.
        // on bare metal this gives the real panel hz; on qemu it returns ~60.
        uint32_t detected_hz = Graphics::DetectRefreshRate();
        SerialLogger::Log("Display: Detected refresh rate: ");
        SerialLogger::LogDec((int)detected_hz);
        SerialLogger::Log(" Hz\r\n");

        // match the monitor's native refresh rate exactly
        Graphics::SetTargetFPS(detected_hz);
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);

        // verify display works  -  write directly to framebuffer
        SerialLogger::Log("Display: RenderMode=");
        SerialLogger::LogDec((int)Graphics::GetRenderMode());
        SerialLogger::Log(" backbuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBackBuffer());
        SerialLogger::Log(" activebuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBuffer());
        SerialLogger::Log("\r\n");
    }

    // some laptops share a fragile 8042/ec path between keyboard and
    // touchpad. bring the keyboard up, initialize the auxiliary device, then
    // re-arm keyboard scanning so the mouse path does not leave the keyboard
    // port wedged.
    Keyboard::Init();
    Keyboard::InitUSB();

    Mouse::Init();
    Keyboard::Init();
    Keyboard::InitUSB();
    Mouse::SetDPIScaling(800, 800);
    InputManager::Init();

    if (has_display) {
        int sw = Graphics::GetWidth();
        int sh = Graphics::GetHeight();

        // pitch black screen
        Graphics::Clear(0xFF000000);

        // draw embedded logo centered (scaled to 200x200)
        int logo_sw = 200, logo_sh = 200;
        int logo_lx = (sw - logo_sw) / 2;
        int logo_ly = (sh / 3) - (logo_sh / 2);

        for (int dy = 0; dy < logo_sh; dy++) {
            int src_y = (dy * LOGO_HEIGHT) / logo_sh;
            for (int dx = 0; dx < logo_sw; dx++) {
                int src_x = (dx * LOGO_WIDTH) / logo_sw;
                uint32_t pixel = logo_data[src_y * LOGO_WIDTH + src_x];
                uint8_t alpha = (pixel >> 24) & 0xFF;
                if (alpha > 64) {
                    Graphics::DrawPixel(logo_lx + dx, logo_ly + dy, pixel | 0xFF000000);
                }
            }
        }

        // "kurono" text centered below logo
        const char* brand = "K U R O N O";
        int brand_w = 11 * 8; // approximate width
        Graphics::DrawString((sw - brand_w) / 2, logo_ly + logo_sh + 24, brand, 0xFFAAAAAA, 0xFF000000);

        // loading bar dimensions
        int bar_w = 180, bar_h = 3;
        int bar_x = (sw - bar_w) / 2;
        int bar_y = logo_ly + logo_sh + 52;

        // bar track (dark gray)
        Graphics::FillRect(bar_x, bar_y, bar_w, bar_h, 0xFF222222);
        Graphics::SwapBuffers();
        SerialLogger::Log("Boot splash: logo displayed\r\n");

        // animate loading bar over ~3 seconds (60 steps x 50ms)
        for (int step = 1; step <= 60; step++) {
            int fill_w = (step * bar_w) / 60;
            // smooth gradient fill: blue to cyan
            for (int px = 0; px < fill_w; px++) {
                int r = 0x30 + (px * 0x30) / bar_w;
                int g = 0x80 + (px * 0x60) / bar_w;
                int b = 0xFF;
                uint32_t c = 0xFF000000 | (r << 16) | (g << 8) | b;
                for (int py = 0; py < bar_h; py++)
                    Graphics::DrawPixel(bar_x + px, bar_y + py, c);
            }
            // three pulsing dots after the bar
            int dot_y = bar_y + bar_h + 16;
            for (int d = 0; d < 3; d++) {
                int dot_x = (sw / 2) - 16 + d * 16;
                int bright = 80 + ((step + d * 8) % 20) * 8;
                if (bright > 255) bright = 255;
                uint32_t dc = 0xFF000000 | (bright << 16) | (bright << 8) | bright;
                Graphics::FillRect(dot_x, dot_y, 4, 4, dc);
            }
            Graphics::SwapBuffers();
            Timer::WaitMs(50);
        }

        // brief pause then smooth clear
        Timer::WaitMs(200);
        Graphics::Clear(0xFF000000);
        Graphics::SwapBuffers();
        Timer::WaitMs(150);
        SerialLogger::Log("Boot splash complete\r\n");
    }

    System::Initialize();

    MediaDecoder::Image wallpaper = {0, 0, 0, false, 0, false};
    if (mbi->flags & (1u << 3) && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;

        // try named wallpaper
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            if (name && streq(name, "wallpaper")) {
                MediaDecoder::Image candidate = MediaDecoder::DecodeModule(mods[i].mod_start, mods[i].mod_end);
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }

        // load font module
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            SerialLogger::Log("Module: ");
            if (name) SerialLogger::Log(name); else SerialLogger::Log("(null)");
            SerialLogger::Log("\r\n");

            if (name && streq(name, "font")) {
                const uint8_t* fptr = (const uint8_t*)mods[i].mod_start;
                int fsize = (int)(mods[i].mod_end - mods[i].mod_start);
                FontTTF::Init(fptr, fsize);
                if (FontTTF::ok) SerialLogger::Log("Font OK\r\n");
                else SerialLogger::Log("Font FAIL\r\n");
                break;
            }
        }

        // fallback wallpaper: try any remaining image
        if (!wallpaper.valid) {
            for (uint32_t i = 0; i < mbi->mods_count; i++) {
                uint32_t start = mods[i].mod_start;
                uint32_t end = mods[i].mod_end;
                const uint8_t* d = (const uint8_t*)start;
                size_t n = (size_t)(end - start);
                MediaDecoder::Image candidate;
                if (MediaDecoder::IsPNG(d, n) || MediaDecoder::IsJPEG(d, n)) {
                    candidate = MediaDecoder::DecodeModule(start, end);
                } else {
                    candidate = MediaDecoder::DecodeRaw(start);
                }
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }
    }

    // fallback: use embedded wallpaper if no module wallpaper loaded
    if (!wallpaper.valid) {
        SerialLogger::Log("Loading embedded wallpaper...\r\n");
        uint32_t wp_start = (uint32_t)(uintptr_t)wallpaper_png_data;
        uint32_t wp_end = wp_start + wallpaper_png_size;
        wallpaper = MediaDecoder::DecodeModule(wp_start, wp_end);
        if (wallpaper.valid) {
            SerialLogger::Log("Embedded wallpaper decoded OK\r\n");
        } else {
            SerialLogger::Log("Embedded wallpaper decode FAILED\r\n");
        }
    }

    GUI::SetWallpaper(wallpaper);
    if (wallpaper.valid) {
        Desktop::SetWallpaperImage(wallpaper);
        SerialLogger::Log("Desktop wallpaper image set\r\n");
    }

    TimeManager::SetTimezoneMinutes(0);
    TimeManager::EnableDST(false);

    SerialLogger::Log("[KVFS] Init...\r\n");
    KVFS::Init();
    RuntimeLog::InitFilesystem();
    RuntimeLog::LogBoot("kvfs online");
    RuntimeLog::LogSystem("kernel", "runtime filesystem layout created");

    // uiconfig must be initialized before any ui subsystem reads colors/sizes.
    // it writes /etc/kurono/ui.conf with defaults on first boot.
    SerialLogger::Log("[UIConfig] Init...\r\n");
    UIConfig::Init();

    SerialLogger::Log("[Shell] Init...\r\n");
    KuronoShell::Init();
    KuronoShell shell_instance;   // trivial object  -  all methods are static

    SerialLogger::Log("[LinuxCmds] Register...\r\n");
    LinuxCmds::RegisterAll(&shell_instance);

    if (!boot_emergency) {
        SerialLogger::Log("[WindowsCmds] Register...\r\n");
        WindowsCmds::RegisterAll(&shell_instance);
    }

    if (boot_emergency) {
        KVFS::Mkdirs("/home/user/Documents");
        KVFS::Mkdirs("/home/user/Desktop");
        KVFS::Mkdirs("/usr/bin");
        KVFS::Mkdirs("/etc");
        KVFS::Mkdirs("/tmp");
        KVFS::Mkdirs("/var/log");
        KVFS::WriteString("/etc/hostname", "kurono-emergency");
        KVFS::WriteString("/etc/os-release", "Kurono Emergency Kernel\nMODE=emergency\n");
        KVFS::WriteString("/system/boot/emergency.txt",
            "Kurono emergency mode\n"
            "Commands:\n"
            "  help\n"
            "  ls /system\n"
            "  cat /system/boot/boot.log\n"
            "  tail -n 40 /system/logs/system.log\n"
            "  dmesg\n"
            "  reboot\n");
        SerialLogger::Log("[Emergency] Initializing keyboard/input\r\n");
        Keyboard::Init();
        Keyboard::InitUSB();
        InputManager::Init();
        RuntimeLog::LogBoot("emergency recovery shell ready");
        RuntimeLog::LogSystem("kernel", "entered emergency recovery mode");
        emergency_run_shell();
    }

    SerialLogger::Log("[SUPR] Init...\r\n");
    SUPR::Init();

    SerialLogger::Log("[PackageManager] Init...\r\n");
    PackageManager::Init();
    PackageManager::RegisterCommands(&shell_instance);

    SerialLogger::Log("[Installer] Init...\r\n");
    Installer::Init();
    Installer::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Network] Init...\r\n");
    Network::Init();
    WiFi::Init();

    SerialLogger::Log("[KCL] Init...\r\n");
    KCL::Init(&shell_instance);

    SerialLogger::Log("[DualBoot] Init...\r\n");
    DualBootManager::Init();

    SerialLogger::Log("[DualBoot] Starting integrated boot...\r\n");
    DualBootManager::BootIntegrated();

    SerialLogger::Log("[LinuxNet] Init...\r\n");
    LinuxNetBridge::Init();

    // register linux subsystem shell commands
    LinuxInit::RegisterShellCommands(&shell_instance);
    DualBootManager::RegisterShellCommands(&shell_instance);
    LinuxNetBridge::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[LinuxDrivers] Init...\r\n");
    LinuxDriverFramework::Init();
    LinuxDriverFramework::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Linux] Subsystem fully integrated\r\n");
    RuntimeLog::LogSystem("linux", "subsystem integrated");

    KVFS::Mkdirs("/home/user/Documents");
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::Mkdirs("/home/user/Desktop");
    KVFS::Mkdirs("/home/user/Music");
    KVFS::Mkdirs("/usr/bin");
    KVFS::Mkdirs("/etc");
    KVFS::Mkdirs("/tmp");
    KVFS::Mkdirs("/var/log");
    KVFS::WriteString("/etc/hostname", "kurono");
    KVFS::WriteString("/etc/os-release", "Kurono OS v1.0\nARCH=x86\nKERNEL=kurono\n");
    KVFS::WriteString("/home/user/readme.txt", "Welcome to Kurono OS!\n\nThis is a bare-metal operating system.\nType 'help' in the terminal for available commands.\n");
    if (EmbeddedMedia::HasDenjiMP4()) {
        KVFS::WriteFile("/home/user/Documents/denji.mp4",
                        EmbeddedMedia::DenjiMP4Data(),
                        EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log("[KVFS] Embedded denji.mp4: ");
        SerialLogger::LogDec(EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log(" bytes\r\n");
    } else {
        KVFS::WriteString("/home/user/Documents/denji.mp4",
                          "[MP4 stub - asset not embedded]");
    }
    KVFS::WriteString("/home/user/Music/startup.wav", "[WAV PCM 22050Hz 16-bit stereo 0:05]");
    KVFS::WriteString("/home/user/Music/notification.wav", "[WAV PCM 22050Hz 16-bit mono 0:02]");
    KVFS::WriteString("/home/user/hello.kcl", "# KCL Script\nprint \"Hello from Kurono!\"\nset x 42\nprint x\n");
    KVFS::WriteString("/home/user/math.kcl", "# Math demo\nset a 16\nset b sqrt(a)\nprint \"sqrt(16) = \"\nprint b\nset r rand()\nprint \"random = \"\nprint r\n");
    KVFS::WriteString("/home/user/loop.kcl", "# Loop demo\nset sum 0\nfor i in 1 10 do\n  set sum sum + i\nend\nprint \"Sum 1..10 = \"\nprint sum\n");
    KVFS::WriteString("/home/user/fib.kcl", "# Fibonacci\nset a 0\nset b 1\nfor i in 1 10 do\n  set c a + b\n  print c\n  set a b\n  set b c\nend\n");
    SerialLogger::Log("[KVFS] Filesystem populated\r\n");
    RuntimeLog::LogSystem("kernel", "default filesystem populated");
    RuntimeLog::LogBoot("default files populated");

    // initialize audio driver (sb16) early so apps can use it immediately
    Audio::Init();

    if (has_display) {
        SerialLogger::Log("[Desktop] Init...\r\n");
        RuntimeLog::LogBoot("desktop initialization started");
        DesktopEnvironment::Init(Graphics::GetWidth(), Graphics::GetHeight());
        if (wallpaper.valid) {
            Desktop::SetWallpaperImage(wallpaper);
        }

        LockScreen::Show();
        RuntimeLog::LogBoot("desktop ready");
    }

    // calculator is now launched via start menu → wm, no standalone init

    const uint32_t TARGET_FPS = Graphics::GetMonitorHz() > 0 ? Graphics::GetMonitorHz() : 60;
    const uint32_t TARGET_FRAME_MS = 1000 / TARGET_FPS;

    uint32_t frame_counter = 0;
    uint32_t fps_counter = 0;
    uint32_t last_fps_ms = Timer::GetRealMs();
    uint32_t displayed_fps = 0;

    // virtualization is manual-only during normal boot.
    // this avoids risky msr/vmx/svm bring-up on bare-metal laptops until the
    // user explicitly requests vm features from the shell.
    SerialLogger::Log("[VMM] Deferred  -  virtualization initializes on demand only\r\n");
    VirtualDevices::Init();
    RuntimeLog::LogBoot("boot sequence complete");

    const GpuProbeResult& gpr = GpuProbe::GetResult();
    int igpu_count = 0;
    int dgpu_count = 0;
    int vgpu_count = 0;
    for (int i = 0; i < gpr.count; i++) {
        if (!gpr.gpus[i].present) continue;
        if (gpr.gpus[i].role == GPU_ROLE_VIRTUAL) vgpu_count++;
        else if (gpr.gpus[i].is_igpu) igpu_count++;
        else dgpu_count++;
    }

    SerialLogger::Log("[GPU] Inventory: total=");
    SerialLogger::LogDec(gpr.count);
    SerialLogger::Log(" iGPU=");
    SerialLogger::LogDec(igpu_count);
    SerialLogger::Log(" dGPU=");
    SerialLogger::LogDec(dgpu_count);
    SerialLogger::Log(" virtual=");
    SerialLogger::LogDec(vgpu_count);
    SerialLogger::Log("\r\n");

    if (igpu_count > 0 && dgpu_count > 0) {
        SerialLogger::Log("[GPU] Hybrid graphics detected during kernel load\r\n");
    } else if (igpu_count > 0) {
        SerialLogger::Log("[GPU] Integrated graphics configuration detected\r\n");
    } else if (dgpu_count > 0) {
        SerialLogger::Log("[GPU] Discrete graphics configuration detected\r\n");
    }

    if (gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) {
        SerialLogger::Log("[GPU] Primary display adapter: ");
        SerialLogger::Log(gpr.gpus[gpr.primary_idx].desc);
        SerialLogger::Log("\r\n");
    }

    // initialize nvidia gpu driver
    if (GpuProbe::HasNvidiaGPU()) {
        SerialLogger::Log("[GPU] Initializing NVIDIA driver...\r\n");
        NvidiaGPU::Init();
        if (NvidiaGPU::IsDetected()) {
            const NvidiaGPUInfo& gi = NvidiaGPU::GetInfo();
            SerialLogger::Log("[GPU] ");
            SerialLogger::Log(gi.name);
            SerialLogger::Log(" detected\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] NVIDIA driver skipped (no NVIDIA dGPU present)\r\n");
    }

    // initialize iommu (vt-d / amd-vi) for device passthrough
    SerialLogger::Log("[IOMMU] Detecting IOMMU...\r\n");
    IOMMU::Init();
    if (IOMMU::IsSupported() && NvidiaGPU::IsDetected()) {
        SerialLogger::Log("[IOMMU] VT-d available  -  GPU passthrough possible\r\n");
    }

    // initialize amd gpu driver
    if (GpuProbe::HasAmdGPU()) {
        SerialLogger::Log("[GPU] Initializing AMD driver...\r\n");
        AmdGPU::Init();
        if (AmdGPU::IsAvailable()) {
            const AmdGPUInfo& ag = AmdGPU::GetInfo();
            SerialLogger::Log("[GPU] AMD: ");
            SerialLogger::Log(ag.name);
            SerialLogger::Log(" detected\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] AMD driver skipped (no AMD GPU present)\r\n");
    }

    // initialize intel igpu driver
    if (GpuProbe::HasIntelIGPU()) {
        SerialLogger::Log("[GPU] Initializing Intel iGPU driver...\r\n");
        IntelGPU::Init();
        if (IntelGPU::IsDetected()) {
            const IntelGPUInfo& ig = IntelGPU::GetInfo();
            SerialLogger::Log("[GPU] Intel: ");
            SerialLogger::Log(ig.name);
            SerialLogger::Log(" (");
            SerialLogger::Log(IntelGPU::GetGenName());
            SerialLogger::Log(")\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] Intel iGPU driver skipped (no Intel iGPU present)\r\n");
    }

    // initialize unified display manager
    // detects the active backend and enables runtime resolution switching.
    SerialLogger::Log("[DisplayMgr] Init...\r\n");
    if (DisplayManager::Init()) {
        SerialLogger::Log("[DisplayMgr] Backend: ");
        SerialLogger::Log(DisplayManager::GetBackendName());
        SerialLogger::Log("  ");
        SerialLogger::LogDec(DisplayManager::GetWidth());
        SerialLogger::Log("x");
        SerialLogger::LogDec(DisplayManager::GetHeight());
        SerialLogger::Log("@");
        SerialLogger::LogDec(DisplayManager::GetBpp());
        SerialLogger::Log("bpp\r\n");
    } else {
        SerialLogger::Log("[DisplayMgr] Init failed (no backend)\r\n");
    }

    // initialize ac97 audio controller
    SerialLogger::Log("[AC97] Init...\r\n");
    AC97::Init();
    if (AC97::IsAvailable()) {
        SerialLogger::Log("[AC97] AC97 audio controller ready\r\n");
    }

    // initialize cpu feature detection
    SerialLogger::Log("[CPU] Detecting CPU features...\r\n");
    CPUDetect::Init();
    CPUDetect::PrintInfo();

    SerialLogger::Log("Entering main loop\r\n");

    Mouse::SetAutoDraw(false);

    while (true) {
        // advance system time by real elapsed ms (pit-polled)
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) {
            TimeManager::AdvanceByMs(real_elapsed);
        }

        // check for deferred resolution change (between frames, before rendering)
        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // tick audio drivers (poll-based buffer management)
        Audio::Tick();
        AC97::Tick();

        // poll e1000 nic  -  every 4th frame to reduce overhead
        if (E1000::IsDetected() && (frame_counter & 3) == 0) {
            E1000::Poll();
        }

        // poll input
        InputManager::Poll();
        Scheduler::Tick();

        if (has_display) {
            // drain all mouse events from the ring buffer
            int scroll_delta = 0;
            while (Mouse::HasEvent()) {
                Mouse::Event mevt = Mouse::GetEvent();
                if (mevt.type == 3) scroll_delta += mevt.dz; // scroll
            }

            // forward input to desktop environment
            bool mouse_clicked = Mouse::LeftClicked();
            bool mouse_down = Mouse::IsLeftDown(); // true while button held  -  enables dragging

            // drain all keyboard chars per frame to eliminate input lag
            // first call includes mouse state, subsequent calls are keyboard-only
            char kb_char = 0;
            if (Keyboard::HasChar()) kb_char = Keyboard::GetChar();
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, mouse_down, mouse_clicked, kb_char);
            while (Keyboard::HasChar()) {
                kb_char = Keyboard::GetChar();
                DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, false, false, kb_char);
            }

            // forward scroll to focused window
            if (scroll_delta != 0) {
                Window* fw = WindowManager::GetFocusedWindow();
                if (fw && fw->input) {
                    fw->input(fw, 3, scroll_delta, 0); // event 3 = scroll
                }
            }

            DesktopEnvironment::Update();

            // refresh task manager periodically  -  every 300 frames (~2s)
            if (frame_counter % 300 == 0) {
                TaskManagerApp::RefreshProcesses();
            }

            // redraw every frame for full smoothness
            DesktopEnvironment::Render();

            // fps counter update  -  using real pit-polled time
            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
            }

            // draw fps overlay  -  sleek rounded pill
            {
                char fps_str[16] = "FPS ";
                char num_buf[8];
                int val = (int)displayed_fps;
                if (val == 0) { num_buf[0] = '0'; num_buf[1] = 0; }
                else {
                    char tmp[8]; int n = 0;
                    while (val > 0 && n < 7) { tmp[n++] = '0' + (val % 10); val /= 10; }
                    for (int i = 0; i < n; i++) num_buf[i] = tmp[n - 1 - i];
                    num_buf[n] = 0;
                }
                int si = 4;
                for (int i = 0; num_buf[i] && si < 14; i++) fps_str[si++] = num_buf[i];
                fps_str[si] = 0;
                int sw = Graphics::GetWidth();
                int pill_w = si * 8 + 16;
                int tx = sw - pill_w - 6;
                // semi-transparent dark pill with accent border
                Graphics::FillRoundedRect(tx, 6, pill_w, 22, 11, 0xB0101020);
                Graphics::DrawString(tx + 8, 10, fps_str, 0xFF00E676, 0xFF000000);
            }

            Mouse::DrawAt(Mouse::mx, Mouse::my);
            Graphics::SwapBuffers();
        } else if (boot_console_realtime || boot_text_only) {
            static uint32_t last_console_ms = 0;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_console_ms >= 1000) {
                last_console_ms = now_ms;

                char up[16];
                char proc[16];
                vga_u32_to_dec(now_ms / 1000, up, sizeof(up));
                vga_u32_to_dec((uint32_t)Scheduler::GetProcessCount(), proc, sizeof(proc));

                char line0[81] = "Kurono Console Mode | realtime status";
                char line1[81] = "Uptime: ";
                int p = 8;
                for (int i = 0; up[i] && p < 78; i++) line1[p++] = up[i];
                line1[p++] = 's'; line1[p] = 0;

                char line2[81] = "Processes: ";
                p = 11;
                for (int i = 0; proc[i] && p < 78; i++) line2[p++] = proc[i];
                line2[p] = 0;

                char line3[81] = "Mouse: x=";
                char mx[16], my[16];
                vga_u32_to_dec((uint32_t)(Mouse::mx < 0 ? 0 : Mouse::mx), mx, sizeof(mx));
                vga_u32_to_dec((uint32_t)(Mouse::my < 0 ? 0 : Mouse::my), my, sizeof(my));
                p = 9;
                for (int i = 0; mx[i] && p < 78; i++) line3[p++] = mx[i];
                if (p < 78) line3[p++] = ' ';
                if (p < 78) line3[p++] = 'y';
                if (p < 78) line3[p++] = '=';
                for (int i = 0; my[i] && p < 78; i++) line3[p++] = my[i];
                line3[p] = 0;

                vga_write_line(20, line0, 0x0B);
                vga_write_line(21, line1, 0x0F);
                vga_write_line(22, line2, 0x0F);
                vga_write_line(23, line3, 0x0F);
                vga_write_line(24, "Press reboot if needed. Running poll-only kernel loop.", 0x07);
            }
        }

        // tight pit-polling pacing loops can throttle whpx heavily.
        // keep rendering loop effectively uncapped for high-refresh smoothness.
        (void)TARGET_FRAME_MS;
    }
}
