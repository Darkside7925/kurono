// kurono os  -  denji video viewer (window glue around VideoPlayer)
// =====================================================================
#include "denji_app.h"
#include "../media/video_player.h"
#include "../media/embedded_media.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"
#include "../system/logging.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"

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
// heap buffer backing a kvid loaded from the vfs (OpenFile). owned here,
// freed on Close. null when playing an embedded asset. (satoru)
static uint8_t* g_file_buffer = nullptr;

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

    // decode is now split from blit. PumpDecode only does the heavy jpeg
    // work when the frame actually advances (true video fps, not the ~60/s
    // paint rate); on a steady frame it is just a binary search + return.
    // Render then blits the cached scaled frame (a row memcpy), so a static
    // frame costs almost nothing even though we repaint every compositor
    // pass. see decouple note in video_player.h: the only remaining stall is
    // the single decode on the paint where the frame turns over; moving that
    // decode onto its own scheduler-step cadence needs a pump call from a
    // process file (not owned here). (satoru)
    VideoPlayer::PumpDecode(g_state);
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

bool OpenFile(const char* path) {
    if (!path) return false;
    int sz = KVFS::GetFileSize(path);
    if (sz <= 16) return false;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)sz);
    if (!buf) return false;
    int got = KVFS::ReadFile(path, buf, (uint32_t)sz);
    // validate the KVID magic before handing it to the player. (satoru)
    if (got < 16 || buf[0] != 'K' || buf[1] != 'V' || buf[2] != 'I' || buf[3] != 'D') {
        KernelHeap::Free(buf);
        SerialLogger::Log("[DenjiApp] OpenFile: not a kvid: ");
        SerialLogger::Log(path);
        SerialLogger::Log("\r\n");
        return false;
    }
    // close any current video (frees the previous file buffer) then take
    // ownership of the new one. (satoru)
    Close();
    g_file_buffer = buf;
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '/') base = p + 1;
    OpenBuffer(buf, (uint32_t)got, base);
    return true;
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
    if (g_file_buffer) {
        KernelHeap::Free(g_file_buffer);
        g_file_buffer = nullptr;
    }
    g_open_failed = false;
    ResetPendingOpen();
}

bool IsOpen() { return g_win_id >= 0; }

} // namespace DenjiApp
