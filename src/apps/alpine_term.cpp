//  kurono os - "Alpine" terminal (see alpine_term.h)
#include "alpine_term.h"
#include "vt.h"
#include "../virt/vm_host.h"
#include "../ui/window_manager.h"
#include "../ui/kss.h"
#include "../ui/font.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../system/clipboard.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)

namespace {
    // the vt grid is ~1.7 mb (two screens + 400 lines of scrollback). it is
    // allocated with the terminal window and freed when the window is really
    // gone, so a desktop with no linux terminal keeps none of it. (satoru)
    Vt*      g_vt = nullptr;
    int      g_wid = -1;
    uint64_t g_cursor = 0;
    uint32_t g_gen = 0;
    int      g_off = 0;                 // lines scrolled back
    bool     g_inited = false;

    Vt* ensure_vt() {
        if (!g_vt) {
            g_vt = (Vt*)KernelHeap::Alloc(sizeof(Vt));
            if (g_vt) memset(g_vt, 0, sizeof(Vt));
        }
        return g_vt;
    }

    void release_vt(Window* w) {
        (void)w;
        if (g_vt) { KernelHeap::Free(g_vt); g_vt = nullptr; }
        g_inited = false;
        g_wid = -1;
    }

    // the same monospace grid as the Terminal app (satoru)
    float cell_pxh = 16.0f;
    int   CELL_W = 8, CELL_H = 20;
    const int PAD_X = 8, PAD_Y = 6;
    bool  metrics_ok = false, metrics_ttf = false;
    unsigned int T_BG = 0xFF101012, T_FG = 0xFFE6E6EA, T_CURSOR = 0xFF3D7DFF, T_DIM = 0xFF8A8A92;

    void sync_theme() {
        const KSS::Theme& t = KSS::T();
        T_BG = t.bg; T_FG = t.text; T_CURSOR = KSS::Accent(); T_DIM = t.text_dim;
    }

    // the distro the terminal would start: the one that ran last, else the
    // settings choice before the first boot (satoru)
    int target_distro() {
        VMHost::Info vi; VMHost::GetInfo(&vi);
        if (vi.boots || vi.state != VMHost::VMH_OFF) return vi.distro;
        return VMHost::SelectedDistro();
    }

    void ensure_metrics() {
        if (metrics_ok && metrics_ttf == FontTTF::ok) return;
        metrics_ttf = FontTTF::ok;
        float base = KSS::BodyPx();
        if (base < 8.0f) base = 16.0f;
        cell_pxh = base;
        if (FontTTF::ok) {
            for (int k = 0; k < 3; k++) {
                int m = FontTTF::MeasureMono(cell_pxh, "M");
                if (m <= 0 || m == 8) break;
                cell_pxh = cell_pxh * 8.0f / (float)m;
            }
        }
        CELL_W = 8;
        CELL_H = (int)(base * 1.30f + 0.5f);
        if (CELL_H < 10) CELL_H = 10;
        metrics_ok = true;
    }

    void draw_run(int sx, int sy, const char* s, unsigned int fg, bool bold) {
        if (!s || !s[0]) return;
        unsigned int c = fg;
        if (bold) {
            unsigned int r = (fg >> 16) & 0xFF, g = (fg >> 8) & 0xFF, b = fg & 0xFF;
            r = r + 50 > 255 ? 255 : r + 50; g = g + 50 > 255 ? 255 : g + 50; b = b + 50 > 255 ? 255 : b + 50;
            c = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
        int gy = sy + (CELL_H - (int)cell_pxh) / 2;
        if (gy < sy) gy = sy;
        if (FontTTF::ok) {
            FontTTF::DrawMono(sx, gy, cell_pxh, s, c);
            if (bold) FontTTF::DrawMono(sx + 1, gy, cell_pxh, s, c);
        } else {
            Graphics::DrawString(sx, gy, s, c, T_BG);
        }
    }

    int utf8(unsigned int cp, char* o) {
        if (cp < 0x80) { o[0] = (char)cp; o[1] = 0; return 1; }
        if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); o[2] = 0; return 2; }
        if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); o[3] = 0; return 3; }
        o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); o[4] = 0; return 4;
    }

    unsigned int blend(unsigned int a, unsigned int b, int num) {
        unsigned int r = (((a >> 16) & 255) * num + ((b >> 16) & 255) * (8 - num)) / 8;
        unsigned int g = (((a >> 8) & 255) * num + ((b >> 8) & 255) * (8 - num)) / 8;
        unsigned int bl = ((a & 255) * num + (b & 255) * (8 - num)) / 8;
        return 0xFF000000u | (r << 16) | (g << 8) | bl;
    }

    // box drawing / block elements, so htop meters and borders join (satoru)
    bool draw_block(int x, int y, unsigned int cp, unsigned int fg) {
        const int W = CELL_W, H = CELL_H;
        if (cp >= 0x2581 && cp <= 0x2588) { int h = H * (int)(cp - 0x2580) / 8; Graphics::FillRect(x, y + H - h, W, h, fg); return true; }
        if (cp >= 0x2589 && cp <= 0x258F) { int w = W * (int)(0x2590 - cp) / 8; if (w < 1) w = 1; Graphics::FillRect(x, y, w, H, fg); return true; }
        if (cp == 0x2580) { Graphics::FillRect(x, y, W, H / 2, fg); return true; }
        int cx = x + W / 2, cy = y + H / 2;
        bool up = false, dn = false, lf = false, rt = false;
        switch (cp) {
            case 0x2500: case 0x2501: case 0x2550: lf = rt = true; break;
            case 0x2502: case 0x2503: case 0x2551: up = dn = true; break;
            case 0x250C: case 0x256D: case 0x2554: dn = rt = true; break;
            case 0x2510: case 0x256E: case 0x2557: dn = lf = true; break;
            case 0x2514: case 0x2570: case 0x255A: up = rt = true; break;
            case 0x2518: case 0x256F: case 0x255D: up = lf = true; break;
            case 0x251C: case 0x2560: up = dn = rt = true; break;
            case 0x2524: case 0x2563: up = dn = lf = true; break;
            case 0x252C: case 0x2566: lf = rt = dn = true; break;
            case 0x2534: case 0x2569: lf = rt = up = true; break;
            case 0x253C: case 0x256C: up = dn = lf = rt = true; break;
            default: return false;
        }
        if (up) Graphics::FillRect(cx, y, 1, cy - y + 1, fg);
        if (dn) Graphics::FillRect(cx, cy, 1, y + H - cy, fg);
        if (lf) Graphics::FillRect(x, cy, cx - x + 1, 1, fg);
        if (rt) Graphics::FillRect(cx, cy, x + W - cx, 1, fg);
        return true;
    }

    const VtCell* row_ptr(int r) {
        int hn = g_vt->HistCount();
        int off = g_off > hn ? hn : g_off;
        int v = hn - off + r;
        if (v < hn) return g_vt->HistLine(hn - 1 - v);
        return &g_vt->Cell(v - hn, 0);
    }

    void send(const uint8_t* b, int n) { VMHost::ConsoleWrite((const char*)b, n); }

    // paste text into the guest console, bracketed when the guest asked. (satoru)
    void paste_text(const char* s) {
        if (!s) return;
        if (g_vt->BracketedPaste()) send((const uint8_t*)"\x1b[200~", 6);
        uint8_t cb[256]; int cn = 0, total = 0;
        for (int i = 0; s[i] && total < 65536; i++, total++) {
            uint8_t c = (uint8_t)s[i];
            if (c == '\n') c = '\r';
            else if (c < 0x20 && c != '\t' && c != '\r') continue;
            cb[cn++] = c;
            if (cn == (int)sizeof(cb)) { send(cb, cn); cn = 0; }
        }
        if (cn) send(cb, cn);
        if (g_vt->BracketedPaste()) send((const uint8_t*)"\x1b[201~", 6);
        g_off = 0;
    }

    void paste_primary() {
        if (ClipboardManager::HasPrimaryText()) paste_text(ClipboardManager::GetPrimaryText());
    }

    // true when the screen contents changed (satoru)
    bool pump() {
        bool changed = false;
        // a new boot starts on a clean screen (satoru)
        if (VMHost::Generation() != g_gen) {
            changed = true;
            g_gen = VMHost::Generation();
            g_vt->Init(g_vt->Cols(), g_vt->Rows(), T_FG, T_BG);
            g_off = 0;
        }
        static char buf[4096];
        int total = 0, n;
        while (total < (1 << 18) && (n = VMHost::ConsoleRead(&g_cursor, buf, sizeof(buf))) > 0) {
            g_vt->Feed((const uint8_t*)buf, n);
            total += n;
        }
        if (total) changed = true;
        uint8_t rep[256];
        int rn;
        while ((rn = g_vt->TakeReply(rep, sizeof(rep))) > 0) send(rep, rn);
        if (VMHost::ConsoleHead() != g_cursor) Graphics::MarkUIDirty();
        return changed;
    }

    void render(Window* w, int cx, int cy, int cw, int ch) {
    if (!g_vt) return;
        sync_theme();
        ensure_metrics();
        g_vt->SetDefaults(T_FG, T_BG);
        int ox = cx + PAD_X, oy = cy + PAD_Y;
        int cols = (cw - 2 * PAD_X) / CELL_W, rows = (ch - 2 * PAD_Y) / CELL_H;
        if (cols < 20) cols = 20;
        if (cols > VT_MAXC) cols = VT_MAXC;
        if (rows < 4) rows = 4;
        if (rows > VT_MAXR) rows = VT_MAXR;
        if (cols != g_vt->Cols() || rows != g_vt->Rows()) g_vt->Resize(cols, rows);
        VMHost::ConsoleResize(cols, rows);          // no-op unless it changed (satoru)
        // under kurono.gfx.scissor=1 this runs under an empty clip when the window
        // is outside the frame's damage; new guest output has to damage the
        // window itself or it only showed up once the window moved. (satoru)
        if (pump()) Graphics::MarkDirty(cx, cy, cw, ch);
        Graphics::FillRect(cx, cy, cw, ch, T_BG);
        int hn = g_vt->HistCount();
        if (g_off > hn) g_off = hn;
        for (int r = 0; r < rows; r++) {
            const VtCell* row = row_ptr(r);
            if (!row) continue;
            int sy = oy + r * CELL_H;
            int c = 0;
            while (c < cols) {
                unsigned int bg = (row[c].attr & VA_INVERSE) ? row[c].fg : row[c].bg;
                if (bg == T_BG) { c++; continue; }
                int s = c;
                while (c < cols) {
                    unsigned int b2 = (row[c].attr & VA_INVERSE) ? row[c].fg : row[c].bg;
                    if (b2 != bg) break;
                    c++;
                }
                Graphics::FillRect(ox + s * CELL_W, sy, (c - s) * CELL_W, CELL_H, bg);
            }
            for (c = 0; c < cols; c++) {
                const VtCell& k = row[c];
                unsigned int cp = k.cp;
                if (cp == 0 || cp == ' ' || cp == 0xFFFFFFFFu || (k.attr & VA_HIDDEN)) continue;
                unsigned int fg = k.fg, bg = k.bg;
                if (k.attr & VA_INVERSE) { unsigned int t = fg; fg = bg; bg = t; }
                if (k.attr & VA_DIM) fg = blend(fg, bg, 5);
                int x = ox + c * CELL_W;
                if (cp < 0x80) { char s1[2] = { (char)cp, 0 }; draw_run(x, sy, s1, fg, (k.attr & VA_BOLD) != 0); continue; }
                if (!draw_block(x, sy, cp, fg)) { char u[8]; utf8(cp, u); draw_run(x, sy, u, fg, (k.attr & VA_BOLD) != 0); }
            }
            for (c = 0; c < cols; c++) {
                unsigned char at = row[c].attr;
                if (!(at & (VA_UNDER | VA_STRIKE))) continue;
                unsigned int fg = (at & VA_INVERSE) ? row[c].bg : row[c].fg;
                if (at & VA_UNDER) Graphics::FillRect(ox + c * CELL_W, sy + CELL_H - 3, CELL_W, 1, fg);
                if (at & VA_STRIKE) Graphics::FillRect(ox + c * CELL_W, sy + CELL_H / 2, CELL_W, 1, fg);
            }
        }
        VMHost::State st = VMHost::GetState();
        if (g_vt->CursorVisible() && g_off == 0 && st == VMHost::VMH_RUNNING) {
            int r = g_vt->CurRow(), c = g_vt->CurCol();
            if (r >= 0 && r < rows && c >= 0 && c < cols) {
                int x = ox + c * CELL_W, y = oy + r * CELL_H;
                if (w && w->focused) {
                    Graphics::FillRect(x, y, CELL_W, CELL_H, T_CURSOR);
                    const VtCell& k = g_vt->Cell(r, c);
                    if (k.cp > ' ' && k.cp < 0x80) { char s[2] = { (char)k.cp, 0 }; draw_run(x, y, s, T_BG, false); }
                } else {
                    Graphics::FillRect(x, y, CELL_W, 1, T_CURSOR); Graphics::FillRect(x, y + CELL_H - 1, CELL_W, 1, T_CURSOR);
                    Graphics::FillRect(x, y, 1, CELL_H, T_CURSOR); Graphics::FillRect(x + CELL_W - 1, y, 1, CELL_H, T_CURSOR);
                }
            }
        }
        // vm state line when it is not simply running (satoru)
        if (st != VMHost::VMH_RUNNING) {
            const char* msg = "";
            char eb[192];
            const char* dn = VMHost::GetDistro(target_distro())->name;
            switch (st) {
                case VMHost::VMH_STARTING: {
                    int p = 0; const char* a = "Starting "; while (*a) eb[p++] = *a++;
                    a = dn; while (*a && p < 180) eb[p++] = *a++;
                    a = "..."; while (*a && p < 186) eb[p++] = *a++;
                    eb[p] = 0; msg = eb; break;
                }
                case VMHost::VMH_STOPPING: {
                    int p = 0; const char* a = dn; while (*a && p < 160) eb[p++] = *a++;
                    a = " is shutting down..."; while (*a && p < 184) eb[p++] = *a++;
                    eb[p] = 0; msg = eb; break;
                }
                case VMHost::VMH_FAILED: {
                    int p = 0; const char* a = dn; while (*a && p < 60) eb[p++] = *a++;
                    a = " failed: "; while (*a && p < 150) eb[p++] = *a++;
                    a = VMHost::LastError(); while (*a && p < 150) eb[p++] = *a++;
                    a = "  (Enter: retry)"; while (*a && p < 188) eb[p++] = *a++;
                    eb[p] = 0; msg = eb; break;
                }
                default:
                    if (VMHost::Enabled()) {
                        int p = 0; const char* a = dn; while (*a && p < 140) eb[p++] = *a++;
                        a = " is stopped. Press Enter to start it."; while (*a && p < 188) eb[p++] = *a++;
                        eb[p] = 0; msg = eb;
                    } else {
                        msg = "Linux is turned off. Turn it on in Settings > Linux.";   // (satoru)
                    }
                    break;
            }
            int bh = CELL_H + 8;
            Graphics::FillRect(cx, cy + ch - bh, cw, bh, blend(T_CURSOR, T_BG, 2));
            draw_run(cx + PAD_X, cy + ch - bh + 4, msg, T_FG, false);
            Graphics::RequestFrameAt(Timer::GetRealMs() + 250);
        }
    }

    int csi(uint8_t* o, int n, int num, char fin, int mod, bool app) {
        o[n++] = 0x1B;
        if (mod > 1) {
            o[n++] = '[';
            if (num == 0) o[n++] = '1'; else { if (num >= 10) o[n++] = (char)('0' + num / 10); o[n++] = (char)('0' + num % 10); }
            o[n++] = ';'; o[n++] = (char)('0' + mod); o[n++] = fin;
            return n;
        }
        if (num == 0) { o[n++] = app ? 'O' : '['; o[n++] = fin; return n; }
        o[n++] = '[';
        if (num >= 10) o[n++] = (char)('0' + num / 10);
        o[n++] = (char)('0' + num % 10);
        o[n++] = fin;
        return n;
    }
}

bool AlpineTerminal::IsOpen() {
    if (g_wid < 0) return false;
    Window* w = WindowManager::GetWindow(g_wid);
    return w && w->state != WIN_CLOSED;
}

int AlpineTerminal::Open() {
    TaskMem::AppScope _tms(TaskMem::APP_ALPINE);   // kurono.taskmem=1: charge to the app (satoru)
    if (IsOpen()) {
        Window* w = WindowManager::GetWindow(g_wid);
        if (w && w->state == WIN_MINIMIZED) WindowManager::Unminimize(g_wid);
        WindowManager::BringToFront(g_wid);
        WindowManager::Focus(g_wid);      // raised but unfocused took no keys (satoru)
        if (VMHost::GetState() == VMHost::VMH_OFF) VMHost::Start(target_distro(), 0);
        return g_wid;
    }
    sync_theme();
    ensure_metrics();
    if (!ensure_vt()) return -1;
    if (!g_inited) { g_vt->Init(100, 30, T_FG, T_BG); g_inited = true; }
    // show what the guest printed so far: replay the scrollback from the start
    // of the current boot (the ring keeps the last 256 kb) (satoru)
    g_cursor = 0;
    g_gen = VMHost::Generation();
    g_vt->Init(g_vt->Cols(), g_vt->Rows(), T_FG, T_BG);
    g_off = 0;
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    int ww = 100 * CELL_W + 2 * PAD_X + 2, wh = 30 * CELL_H + 2 * PAD_Y + 34;
    if (ww > sw - 40) ww = sw - 40;
    if (wh > sh - 120) wh = sh - 120;
    g_wid = WindowManager::CreateWindow(VMHost::GetDistro(target_distro())->name, -1, -1, ww, wh,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) { render(w, cx, cy, cw, ch); },
        (WindowInputFunc)[](Window* w, int ev, int p1, int p2) {
            (void)w; (void)p2;
            if (ev == 3) {                          // wheel: scrollback (satoru)
                g_off += (p1 > 0) ? 3 : (p1 < 0 ? -3 : 0);
                if (g_off < 0) g_off = 0;
                if (g_off > g_vt->HistCount()) g_off = g_vt->HistCount();
                Graphics::MarkUIDirty();
            }
            else if (ev == 6 && p2 && p1 == 2) paste_primary();   // middle click (satoru)
            // characters come through KeyEvent as raw keys, not here (satoru)
        });
    {
        Window* w = WindowManager::GetWindow(g_wid);
        if (w) w->on_close = release_vt;
    }
    if (VMHost::GetState() == VMHost::VMH_OFF || VMHost::GetState() == VMHost::VMH_FAILED)
        VMHost::Start(target_distro(), 0);
    Graphics::MarkUIDirty();
    return g_wid;
}

void AlpineTerminal::KeyEvent(int key, bool shift, bool ctrl, bool alt) {
    if (g_wid < 0) return;
    Window* fw = WindowManager::GetFocusedWindow();
    if (!fw || fw->id != g_wid) return;
    Key k = (Key)key;
    if (k == KEY_M && ctrl && alt) { WindowManager::ToggleMaximize(g_wid); return; }
    if (alt && k == KEY_TAB) return;
    if (k == KEY_LSHIFT || k == KEY_RSHIFT || k == KEY_LCTRL || k == KEY_RCTRL || k == KEY_LALT || k == KEY_RALT ||
        k == KEY_LSUPER || k == KEY_RSUPER || k == KEY_CAPSLOCK || k == KEY_NUMLOCK || k == KEY_SCROLLLOCK) return;
    VMHost::State st = VMHost::GetState();
    if (st != VMHost::VMH_RUNNING) {
        if ((k == KEY_ENTER || k == KEY_KP_ENTER) && (st == VMHost::VMH_OFF || st == VMHost::VMH_FAILED)) {
            VMHost::Start(target_distro(), 0);
            Graphics::MarkUIDirty();
        }
        return;
    }
    if (shift && !ctrl && !alt && (k == KEY_PAGEUP || k == KEY_PAGEDOWN)) {
        g_off += (k == KEY_PAGEUP) ? 10 : -10;
        if (g_off < 0) g_off = 0;
        if (g_off > g_vt->HistCount()) g_off = g_vt->HistCount();
        Graphics::MarkUIDirty();
        return;
    }
    if (ctrl && shift && k == KEY_V) {                 // paste, bracketed when asked for (satoru)
        if (ClipboardManager::HasText()) paste_text(ClipboardManager::GetText());
        g_off = 0;
        return;
    }
    if (ctrl && shift && k == KEY_C) return;
    g_off = 0;
    uint8_t o[64]; int n = 0;
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    bool app = g_vt->AppCursorKeys();
    switch (k) {
        case KEY_UP:    n = csi(o, n, 0, 'A', mod, app); break;
        case KEY_DOWN:  n = csi(o, n, 0, 'B', mod, app); break;
        case KEY_RIGHT: n = csi(o, n, 0, 'C', mod, app); break;
        case KEY_LEFT:  n = csi(o, n, 0, 'D', mod, app); break;
        case KEY_HOME:  n = csi(o, n, 0, 'H', mod, app); break;
        case KEY_END:   n = csi(o, n, 0, 'F', mod, app); break;
        case KEY_INSERT:   n = csi(o, n, 2, '~', mod, false); break;
        case KEY_DELETE:   n = csi(o, n, 3, '~', mod, false); break;
        case KEY_PAGEUP:   n = csi(o, n, 5, '~', mod, false); break;
        case KEY_PAGEDOWN: n = csi(o, n, 6, '~', mod, false); break;
        case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4: {
            char fin = (char)('P' + (k - KEY_F1));
            if (mod > 1) n = csi(o, n, 0, fin, mod, false); else { o[n++] = 0x1B; o[n++] = 'O'; o[n++] = (uint8_t)fin; }
            break;
        }
        case KEY_F5: n = csi(o, n, 15, '~', mod, false); break;
        case KEY_F6: n = csi(o, n, 17, '~', mod, false); break;
        case KEY_F7: n = csi(o, n, 18, '~', mod, false); break;
        case KEY_F8: n = csi(o, n, 19, '~', mod, false); break;
        case KEY_F9: n = csi(o, n, 20, '~', mod, false); break;
        case KEY_F10: n = csi(o, n, 21, '~', mod, false); break;
        case KEY_F11: n = csi(o, n, 23, '~', mod, false); break;
        case KEY_F12: n = csi(o, n, 24, '~', mod, false); break;
        case KEY_ENTER: case KEY_KP_ENTER: if (alt) o[n++] = 0x1B; o[n++] = '\r'; break;
        case KEY_BACKSPACE: if (alt) o[n++] = 0x1B; o[n++] = ctrl ? 0x08 : 0x7F; break;
        case KEY_TAB: if (shift) { o[n++] = 0x1B; o[n++] = '['; o[n++] = 'Z'; } else { if (alt) o[n++] = 0x1B; o[n++] = '\t'; } break;
        case KEY_ESC: o[n++] = 0x1B; break;
        default: {
            char c = Keyboard::KeyToChar(k, shift, Keyboard::GetState().caps_lock);
            if (!c) return;
            unsigned char u = (unsigned char)c;
            if (ctrl) {
                if (u >= 'a' && u <= 'z') u = (unsigned char)(u - 'a' + 1);
                else if (u >= 'A' && u <= 'Z') u = (unsigned char)(u - 'A' + 1);
                else if (u == '[' || u == '{') u = 27;
                else if (u == '\\' || u == '|') u = 28;
                else if (u == ']' || u == '}') u = 29;
                else if (u == '^' || u == '6') u = 30;
                else if (u == '_' || u == '-' || u == '/') u = 31;
                else if (u == '@' || u == '2' || u == ' ') u = 0;
                else return;
            }
            if (alt) o[n++] = 0x1B;
            o[n++] = u;
            break;
        }
    }
    if (n > 0) send(o, n);
}

// end (satoru)
