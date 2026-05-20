// kurono os  -  denji video viewer (window glue around VideoPlayer)
// =====================================================================
#include "denji_app.h"
#include "../media/video_player.h"
#include "../media/embedded_media.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"
#include "../system/logging.h"

namespace DenjiApp {

// global state  -  single-instance app.  the buffer it points at is the
// embedded .rodata KVID asset, so its lifetime equals the kernel's.
static VideoPlayer::State g_state;
static int  g_win_id = -1;
static bool g_state_loaded = false;
static bool g_open_failed = false;
static char g_title_buf[64];
static const uint8_t* g_pending_data = nullptr;
static uint32_t g_pending_size = 0;
static int g_defer_open_frames = 0;

static void ResetPendingOpen() {
    g_pending_data = nullptr;
    g_pending_size = 0;
    g_defer_open_frames = 0;
}

static void RenderLoading(Window* w, const char* line1, const char* line2) {
    if (!w) return;
    Graphics::FillRect(w->content_x, w->content_y, w->content_w, w->content_h, 0xFF101018);
    Graphics::FillRect(w->content_x + 24, w->content_y + 24,
                       w->content_w - 48, w->content_h - 48, 0xFF161626);
    Graphics::DrawString(w->content_x + 36, w->content_y + 40,
                         line1 ? line1 : "Loading video...",
                         0xFFE0E0F0, 0x00000000);
    Graphics::DrawString(w->content_x + 36, w->content_y + 64,
                         line2 ? line2 : "Preparing the embedded Denji clip.",
                         0xFF8F93A8, 0x00000000);
}

static void OnRender(Window* w) {
    if (!w) return;
    if (!g_state_loaded) {
        RenderLoading(w,
                      g_open_failed ? "Could not open Denji." : "Loading Denji...",
                      g_open_failed ? "See serial/runtime logs for details."
                                    : "Window is live; media will start in a moment.");
        if (g_open_failed || !g_pending_data || g_pending_size == 0) return;
        if (g_defer_open_frames > 0) {
            g_defer_open_frames--;
            return;
        }
        if (!VideoPlayer::Open(g_pending_data, g_pending_size, g_state)) {
            SerialLogger::Log("[DenjiApp] VideoPlayer::Open failed\r\n");
            g_open_failed = true;
            ResetPendingOpen();
            return;
        }
        g_state_loaded = true;
        ResetPendingOpen();
        VideoPlayer::Play(g_state);
        RuntimeLog::LogAppEvent("denji", "open");
        SerialLogger::Log("[DenjiApp] launched\r\n");
        return;
    }

    // pump time-driven decode each paint.  cheap if frame hasn't
    // advanced; otherwise decodes one jpeg + writes one chunk of pcm.
    VideoPlayer::Tick(g_state);
    VideoPlayer::Render(g_state, w->content_x, w->content_y,
                        w->content_w, w->content_h);
}

static void OnInput(Window* w, int event, int a, int b) {
    if (!w || !g_state_loaded) return;
    // event 1 = click, 2 = key
    if (event == 1) {
        // any click toggles play/pause for now.
        VideoPlayer::TogglePause(g_state);
        (void)a; (void)b;
        return;
    }
    if (event == 2) {
        if (a == ' ') VideoPlayer::TogglePause(g_state);
        if (a == 'r' || a == 'R') VideoPlayer::SeekMs(g_state, 0);
    }
}

static int Slen(const char* s) { int n = 0; while (s && s[n]) ++n; return n; }
static void Scpy(char* d, const char* s, int max) {
    int i = 0; while (i + 1 < max && s && s[i]) { d[i] = s[i]; ++i; } d[i] = 0;
}

void Open() {
    if (!EmbeddedMedia::HasDenjiKVID()) {
        SerialLogger::Log("[DenjiApp] no embedded denji.kvid  -  skipping\r\n");
        return;
    }
    OpenBuffer(EmbeddedMedia::DenjiKVIDData(),
               EmbeddedMedia::DenjiKVIDSize(),
               "Denji");
}

void OpenBuffer(const uint8_t* data, uint32_t size, const char* title) {
    if (g_win_id >= 0) {
        WindowManager::Focus(g_win_id);
        return;
    }
    if (!data || size == 0) return;

    Scpy(g_title_buf, title ? title : "Video", (int)sizeof(g_title_buf));
    g_pending_data = data;
    g_pending_size = size;
    g_state_loaded = false;
    g_open_failed = false;
    g_defer_open_frames = 1;

    g_win_id = WindowManager::CreateWindow(
        g_title_buf, 160, 80, 540, 560,
        (WindowRenderFunc)[](Window* w, int, int, int, int) { OnRender(w); },
        (WindowInputFunc)OnInput);

    if (g_win_id < 0) {
        SerialLogger::Log("[DenjiApp] CreateWindow failed\r\n");
        ResetPendingOpen();
        return;
    }
    (void)Slen;
}

void Close() {
    if (g_win_id >= 0) {
        WindowManager::CloseWindow(g_win_id);
        g_win_id = -1;
    }
    if (g_state_loaded) {
        VideoPlayer::Close(g_state);
        g_state_loaded = false;
    }
    g_open_failed = false;
    ResetPendingOpen();
}

bool IsOpen() { return g_win_id >= 0; }

} // namespace DenjiApp
