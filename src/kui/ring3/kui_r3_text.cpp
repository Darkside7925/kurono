// kui_r3_text.cpp: text, fonts, theme and small services for the ring-3 kui backend (satoru)
// the kernel's own ui/font.cpp and third_party/stb_truetype_glue.cpp are compiled unchanged into the (satoru)
// ring-3 program; this file gives them the three kernel things they call (KernelHeap, SerialLogger, (satoru)
// Graphics::BlendPixel / FillRect), pointed at the kui canvas being drawn. same rasterizer, same gamma, (satoru)
// same layout: the text is the kernel's text, pixel for pixel. (satoru)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../kui_internal.h"
#include "../../kernel/heap.h"
#include "../../drivers/serial.h"
#include "../../drivers/graphics.h"
#include "../../ui/font.h"

// ---- the c++ runtime pieces a -nostdlib++ static program needs (satoru)

void* operator new(size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void* operator new[](size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual() { abort(); }

// ---- kernel names font.cpp and the stb glue use (satoru)

void* KernelHeap::Alloc(size_t n) { return malloc(n); }
void* KernelHeap::Realloc(void* p, size_t n) { return realloc(p, n); }
void  KernelHeap::Free(void* p) { free(p); }

static bool g_debug = false;
void SerialLogger::Log(const char* s) { if (g_debug && s) fputs(s, stderr); }
void SerialLogger::LogDec(int v) { if (g_debug) fprintf(stderr, "%d", v); }

static kui::raster::Target* g_text_target = nullptr;   // set around every text draw (satoru)

namespace kui {
namespace bk {
void SetTextTarget(raster::Target* t) { g_text_target = t; }
}
}

void Graphics::BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (g_text_target) kui::raster::BlendPixel(*g_text_target, x, y, r, g, b, a);
}
void Graphics::FillRect(int x, int y, int w, int h, uint32_t color) {
    if (g_text_target) kui::raster::FillRect(*g_text_target, x, y, w, h, color);
}

namespace kui {

// ---- misc (satoru)

void* Alloc(size_t n) { return malloc(n); }
void  Free(void* p) { free(p); }
bool  IsRing3() { return true; }
uint32_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
void Log(const char* tag, const char* msg) {
    fprintf(stderr, "[kui] %s: %s\n", tag ? tag : "?", msg ? msg : "");
}

static void* load_file(const char* path, size_t* out_n) {
    *out_n = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return nullptr; }
    size_t n = (size_t)st.st_size;
    uint8_t* p = (uint8_t*)malloc(n);
    size_t got = 0;
    while (p && got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!p || got != n) { free(p); return nullptr; }
    *out_n = n;
    return p;
}

// ---- theme: /system/run/kui/theme, written by the kernel (key=hex per line) (satoru)

static Theme g_theme;
static bool g_theme_ok = false;

static void theme_defaults() {
    g_theme.appearance = Appearance::Dark;
    g_theme.style = Style::Modern;
    g_theme.minimal_icons = false;
    g_theme.reduced_motion = false;
    g_theme.high_contrast = false;
    g_theme.ui_scale = 100;
    // MDraw::Pal's dark palette and default accent (satoru)
    g_theme.pal.panel = 0xFF2A2B33; g_theme.pal.panel2 = 0xFF22232A; g_theme.pal.line = 0xFFFFFFFF;
    g_theme.pal.text = 0xFFF2F3F8; g_theme.pal.text2 = 0xFF9A9CA8; g_theme.pal.hover = 0xFF0A84FF;
    g_theme.pal.danger = 0xFFFF6B61; g_theme.pal.accent = 0xFF0A84FF; g_theme.pal.dark = true;
}

static const char* theme_path() {
    const char* p = getenv("KUI_THEME");
    return (p && p[0]) ? p : "/system/run/kui/theme";
}

// re-read the theme; true when it changed (satoru)
bool ThemeReload() {
    Theme old = g_theme;
    bool had = g_theme_ok;
    if (!g_theme_ok) theme_defaults();
    g_theme_ok = true;
    size_t n = 0;
    char* b = (char*)load_file(theme_path(), &n);
    if (b) {
        for (size_t i = 0; i < n; ) {
            size_t e = i;
            while (e < n && b[e] != '\n') e++;
            char key[24]; size_t k = 0;
            size_t j = i;
            while (j < e && b[j] != '=' && k < sizeof(key) - 1) key[k++] = b[j++];
            key[k] = 0;
            uint32_t v = 0;
            if (j < e && b[j] == '=') {
                for (j++; j < e; j++) {
                    char c = b[j];
                    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (d < 0) break;
                    v = (v << 4) | (uint32_t)d;
                }
                if (!strcmp(key, "dark")) { g_theme.pal.dark = v != 0; g_theme.appearance = v ? Appearance::Dark : Appearance::Light; }
                else if (!strcmp(key, "modern")) g_theme.style = v ? Style::Modern : Style::Classic;
                else if (!strcmp(key, "minimal_icons")) g_theme.minimal_icons = v != 0;
                else if (!strcmp(key, "reduced_motion")) g_theme.reduced_motion = v != 0;
                else if (!strcmp(key, "high_contrast")) g_theme.high_contrast = v != 0;
                else if (!strcmp(key, "ui_scale")) g_theme.ui_scale = (int)v;
                else if (!strcmp(key, "panel")) g_theme.pal.panel = v;
                else if (!strcmp(key, "panel2")) g_theme.pal.panel2 = v;
                else if (!strcmp(key, "line")) g_theme.pal.line = v;
                else if (!strcmp(key, "text")) g_theme.pal.text = v;
                else if (!strcmp(key, "text2")) g_theme.pal.text2 = v;
                else if (!strcmp(key, "hover")) g_theme.pal.hover = v;
                else if (!strcmp(key, "danger")) g_theme.pal.danger = v;
                else if (!strcmp(key, "accent")) g_theme.pal.accent = v;
            }
            i = e + 1;
        }
        free(b);
    }
    // the classic style draws the 8x16 font even with faces loaded, like the kernel (satoru)
    FontTTF::SetActive(g_theme.style == Style::Modern);
    return !had || memcmp(&old, &g_theme, sizeof(Theme)) != 0;
}

const Theme& CurrentTheme() {
    if (!g_theme_ok) ThemeReload();
    return g_theme;
}

// ---- fonts (satoru)

static bool g_fonts_tried = false;

void FontsLoad() {
    if (g_fonts_tried) return;
    g_fonts_tried = true;
    g_debug = getenv("KUI_DEBUG") != nullptr;
    size_t n = 0;
    void* reg = load_file("/system/share/fonts/kurono/Inter-Regular.otf", &n);
    if (reg) FontTTF::Init((const uint8_t*)reg, (int)n);
    void* bold = load_file("/system/share/fonts/kurono/Inter-SemiBold.otf", &n);
    if (bold) FontTTF::InitBold((const uint8_t*)bold, (int)n);
    void* mono = load_file("/system/share/fonts/kurono/DejaVuSansMono.ttf", &n);
    if (mono) FontTTF::InitMono((const uint8_t*)mono, (int)n);
    if (!reg) Log("kui", "no ui font (/system/share/fonts/kurono), using the 8x16 font");
    CurrentTheme();
    FontTTF::SetActive(g_theme.style == Style::Modern);
}

}  // namespace kui

extern "C" int tt_ascent_face(int face, float pixel_height);

namespace kui {
namespace bk {

bool FontsOk() { FontsLoad(); return FontTTF::ok; }

int TextWidth(Face f, float px, const char* s) {
    FontsLoad();
    switch (f) {
    case Face::SansBold:   return FontTTF::MeasureText(px, s, true);
    case Face::Mono:       return FontTTF::MeasureMono(px, s);
    case Face::Bitmap8x16: return raster::Bitmap8x16Width(px, s, 1 << 30);
    default:               return FontTTF::MeasureText(px, s, false);
    }
}

int TextAscent(Face f, float px) {
    FontsLoad();
    if (!FontTTF::ok || f == Face::Bitmap8x16) return 0;
    if (f == Face::Mono) return tt_ascent_face(FontTTF::HasMono() ? 2 : 0, px);
    return FontTTF::Ascent(px, f == Face::SansBold);
}

int TextDraw(Canvas::Impl& ci, int ax, int atop, Face f, float px, const char* s, Color c) {
    FontsLoad();
    raster::Target* prev = g_text_target;
    g_text_target = &ci.t;
    int adv;
    switch (f) {
    case Face::SansBold:   adv = FontTTF::DrawText(ax, atop, px, s, c, true); break;
    case Face::Mono:       adv = FontTTF::DrawMono(ax, atop, px, s, c); break;
    case Face::Bitmap8x16: adv = raster::Bitmap8x16(ci.t, ax, atop, px, s, 1 << 30, c); break;
    default:               adv = FontTTF::DrawText(ax, atop, px, s, c, false); break;
    }
    g_text_target = prev;
    return adv;
}

static uint8_t* g_icons = nullptr;
static size_t g_icons_n = 0;
static bool g_icons_tried = false;

const uint8_t* IconBlob(size_t* n) {
    if (!g_icons_tried) {
        g_icons_tried = true;
        g_icons = (uint8_t*)load_file("/system/share/kurono/modern_icons.bin", &g_icons_n);
    }
    *n = g_icons_n;
    return g_icons;
}

}  // namespace bk

// ---- clipboard lives in kui_wayland.cpp (the compositor data device) (satoru)

// ---- files through the linux abi (satoru)

namespace Fs {
int ReadFile(const char* p, void* buf, int max) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int got = 0;
    while (got < max) {
        ssize_t r = read(fd, (char*)buf + got, (size_t)(max - got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (int)r;
    }
    close(fd);
    return got;
}
bool WriteFile(const char* p, const void* d, int len) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    int put = 0;
    while (put < len) {
        ssize_t r = write(fd, (const char*)d + put, (size_t)(len - put));
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (r <= 0) break;
        put += (int)r;
    }
    close(fd);
    return put == len;
}
int  FileSize(const char* p) { struct stat st; return stat(p, &st) == 0 ? (int)st.st_size : -1; }
bool Exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
bool IsDir(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
bool Mkdirs(const char* p) {
    char b[256];
    size_t n = strlen(p);
    if (n >= sizeof(b)) return false;
    memcpy(b, p, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (b[i] == '/' || b[i] == 0) {
            char c = b[i]; b[i] = 0;
            if (mkdir(b, 0755) < 0 && errno != EEXIST) return false;
            b[i] = c;
        }
    }
    return true;
}
bool Remove(const char* p) { return IsDir(p) ? rmdir(p) == 0 : unlink(p) == 0; }
bool Rename(const char* a, const char* b) { return rename(a, b) == 0; }
int  ListDir(const char* p, DirEntry* out, int max) {
    DIR* d = opendir(p);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while (n < max && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        strncpy(out[n].name, e->d_name, 63); out[n].name[63] = 0;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", p, e->d_name);
        struct stat st;
        bool ok = stat(full, &st) == 0;
        out[n].dir = ok ? S_ISDIR(st.st_mode) : (e->d_type == DT_DIR);
        out[n].size = ok ? (uint32_t)st.st_size : 0;
        n++;
    }
    closedir(d);
    return n;
}
}

}  // namespace kui

// end (satoru)
