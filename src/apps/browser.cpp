//  kurono os  -  browser removed
//  no compatible bare-metal c++ browser found on github.
//  all real browsers (netsurf, dillo, ladybird, links2) require:
//    - libc (malloc, stdio, string.h)
//    - posix (sockets, threads, file i/o)
//    - gui toolkit (x11, wayland, gtk, sdl)
//  none compile in -ffreestanding bare-metal.
#include "browser.h"
#include "../drivers/graphics.h"
#include "../system/logging.h"

int KBrowse::win_id = -1;

void KBrowse::Open() {
    if (win_id >= 0) return;
    RuntimeLog::LogAppEvent("browser", "open");
    win_id = WindowManager::CreateWindow("Browser (Removed)", 120, 80, 420, 260,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)cx; (void)cy; (void)cw; (void)ch;
            KBrowse::OnRender(w);
        },
        (WindowInputFunc)KBrowse::OnInput
    );
}

void KBrowse::Open(const char*) { Open(); }

void KBrowse::Close() {
    if (win_id >= 0) {
        WindowManager::CloseWindow(win_id);
        win_id = -1;
    }
}

bool KBrowse::IsOpen() { return win_id >= 0; }

void KBrowse::OnRender(Window* w) {
    if (!w) return;
    int cx = w->content_x, cy = w->content_y;
    int cw = w->content_w, ch = w->content_h;

    Graphics::FillRect(cx, cy, cw, ch, 0xFF0E0E1C);

    // big x icon
    int ix = cx + cw/2, iy = cy + 40;
    Graphics::FillCircle(ix, iy, 24, 0xFF2A1A1A);
    Graphics::DrawCircle(ix, iy, 24, 0xFFE74C3C);
    // x lines
    for (int d = -12; d <= 12; d++) {
        Graphics::FillRect(ix + d - 1, iy + d - 1, 3, 3, 0xFFE74C3C);
        Graphics::FillRect(ix + d - 1, iy - d - 1, 3, 3, 0xFFE74C3C);
    }

    // messages
    Graphics::DrawString(cx + cw/2 - 72, cy + 80,
        "Browser Removed", 0xFFE0E0F0, 0xFF000000);
    Graphics::DrawString(cx + cw/2 - 136, cy + 110,
        "No C++ browser on GitHub compiles", 0xFF8888AA, 0xFF000000);
    Graphics::DrawString(cx + cw/2 - 120, cy + 128,
        "for bare-metal freestanding OS.", 0xFF8888AA, 0xFF000000);
    Graphics::DrawString(cx + cw/2 - 128, cy + 158,
        "Requires: libc, POSIX, X11/GTK", 0xFF666688, 0xFF000000);
    Graphics::DrawString(cx + cw/2 - 120, cy + 176,
        "NetSurf | Dillo | Ladybird", 0xFF666688, 0xFF000000);
    Graphics::DrawString(cx + cw/2 - 112, cy + 206,
        "Use terminal: curl <url>", 0xFF5C8AFF, 0xFF000000);
}

void KBrowse::OnInput(Window* w, int event, int a, int b) {
    (void)w; (void)event; (void)a; (void)b;
}
