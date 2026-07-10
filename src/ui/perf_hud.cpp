#include "perf_hud.h"
#include "window_manager.h"
#include "../drivers/graphics.h"
#include "../kernel/heap.h"
#include "../drivers/timer.h"

//  performance hud implementation (satoru)

namespace PerfHUD {

// visibility flag; hidden by default. (satoru)
static bool hud_visible = false;

// hud colors - opaque dark panel to match this codebase's style. (satoru)
#define HUD_BG       0xFF101418       // panel background (satoru)
#define HUD_BORDER   0xFF2A3138       // 1px frame (satoru)
#define HUD_ACCENT   0xFF3A7BD5       // header bar (satoru)
#define HUD_TEXT     0xFFE0E6EC       // body text (satoru)
#define HUD_TITLE    0xFFFFFFFF       // header text (satoru)
#define HUD_TRANSP   0x00000000       // transparent text bg (satoru)

// layout constants. (satoru)
#define HUD_X        12
#define HUD_Y        12
#define HUD_W        168
#define HUD_PAD      8
#define HUD_LINE_H   14               // font is ~12px tall (satoru)
#define HUD_HDR_H    18

// tiny unsigned int -> decimal string helper; writes into buf and returns
// the number of chars written (excluding the nul). no heap. (satoru)
static int u_to_str(uint32_t v, char* buf) {
    char tmp[12];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    // reverse into buf (satoru)
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

// append source string to dst starting at offset; returns new offset. (satoru)
static int append(char* dst, int off, const char* src) {
    int i = 0;
    while (src[i]) dst[off + i] = src[i], i++;
    dst[off + i] = 0;
    return off + i;
}

// append a decimal number; returns new offset. (satoru)
static int append_num(char* dst, int off, uint32_t v) {
    off += u_to_str(v, dst + off);
    return off;
}

void Toggle() {
    hud_visible = !hud_visible;
}

bool IsVisible() {
    return hud_visible;
}

void Render(uint32_t fps, uint32_t frame_ms) {
    if (!hud_visible) return;

    // count open windows (state != WIN_CLOSED). (satoru)
    uint32_t win_count = 0;
    Window* wins = WindowManager::GetWindows();
    if (wins) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (wins[i].state != WIN_CLOSED) win_count++;
        }
    }

    // heap stats: bytes -> mb (one decimal not needed, keep it simple). (satoru)
    size_t used_b  = KernelHeap::GetUsed();
    size_t total_b = KernelHeap::GetTotal();
    uint32_t used_mb  = (uint32_t)(used_b  / (1024u * 1024u));
    uint32_t total_mb = (uint32_t)(total_b / (1024u * 1024u));
    // fall back to kb when below 1 mb so the line is never just "0/0". (satoru)
    bool use_kb = (total_mb == 0);
    uint32_t used_disp  = use_kb ? (uint32_t)(used_b  / 1024u) : used_mb;
    uint32_t total_disp = use_kb ? (uint32_t)(total_b / 1024u) : total_mb;
    const char* unit = use_kb ? " KB" : " MB";

    // uptime from real ms. (satoru)
    uint32_t ms = Timer::GetRealMs();
    uint32_t total_s = ms / 1000u;
    uint32_t hh = total_s / 3600u;
    uint32_t mm = (total_s % 3600u) / 60u;
    uint32_t ss = total_s % 60u;

    // 6 body lines + header. (satoru)
    const int body_lines = 6;
    int panel_h = HUD_HDR_H + HUD_PAD + body_lines * HUD_LINE_H + HUD_PAD;

    // panel background + frame. (satoru)
    Graphics::FillRoundedRect(HUD_X, HUD_Y, HUD_W, panel_h, 6, HUD_BG);
    Graphics::DrawRect(HUD_X, HUD_Y, HUD_W, panel_h, HUD_BORDER);
    // header accent bar. (satoru)
    Graphics::FillRect(HUD_X, HUD_Y, HUD_W, HUD_HDR_H, HUD_ACCENT);
    Graphics::DrawString(HUD_X + HUD_PAD, HUD_Y + 3, "PERF", HUD_TITLE, HUD_TRANSP);

    char line[64];
    int tx = HUD_X + HUD_PAD;
    int ty = HUD_Y + HUD_HDR_H + HUD_PAD;
    int off;

    // line 1: fps. (satoru)
    off = append(line, 0, "FPS: ");
    off = append_num(line, off, fps);
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);
    ty += HUD_LINE_H;

    // line 2: frame time in ms. (satoru)
    off = append(line, 0, "frame: ");
    off = append_num(line, off, frame_ms);
    off = append(line, off, " ms");
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);
    ty += HUD_LINE_H;

    // line 3: heap used/total. (satoru)
    off = append(line, 0, "heap: ");
    off = append_num(line, off, used_disp);
    off = append(line, off, "/");
    off = append_num(line, off, total_disp);
    off = append(line, off, unit);
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);
    ty += HUD_LINE_H;

    // line 4: open window count. (satoru)
    off = append(line, 0, "windows: ");
    off = append_num(line, off, win_count);
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);
    ty += HUD_LINE_H;

    // line 5: uptime h/m/s. (satoru)
    off = append(line, 0, "uptime: ");
    off = append_num(line, off, hh);
    off = append(line, off, "h ");
    off = append_num(line, off, mm);
    off = append(line, off, "m ");
    off = append_num(line, off, ss);
    off = append(line, off, "s");
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);
    ty += HUD_LINE_H;

    // line 6: screen resolution as a small extra stat. (satoru)
    off = append(line, 0, "res: ");
    off = append_num(line, off, (uint32_t)Graphics::GetWidth());
    off = append(line, off, "x");
    off = append_num(line, off, (uint32_t)Graphics::GetHeight());
    Graphics::DrawString(tx, ty, line, HUD_TEXT, HUD_TRANSP);

    // damage the panel region so the compositor repaints it. (satoru)
    Graphics::MarkDirty(HUD_X, HUD_Y, HUD_W, panel_h);
}

} // namespace PerfHUD

// end (satoru)
