#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Basic Web Browser Application
//  Text-mode HTTP/1.1 browser with URL bar, back/forward, bookmarks
// ═══════════════════════════════════════════════════════════════════════════
#include "../ui/window_manager.h"
#include "../kernel/types.h"

#define BROWSER_MAX_URL      256
#define BROWSER_MAX_CONTENT  16384
#define BROWSER_MAX_HISTORY  32
#define BROWSER_MAX_LINKS    64
#define BROWSER_MAX_LINES    512

struct BrowserLink {
    int  x, y, w, h;           // Click area
    char url[BROWSER_MAX_URL];
    int  line;                  // Line number in content
};

class BrowserApp {
public:
    static void Open();
    static void Open(const char* url);
    static void Close();
    static bool IsOpen();

    // Navigation
    static void Navigate(const char* url);
    static void GoBack();
    static void GoForward();
    static void Refresh();
    static void GoHome();

    // Window callbacks
    static void OnRender(Window* w);
    static void OnInput(Window* w, int event, int a, int b);

private:
    static int  win_id;

    // URL bar
    static char current_url[BROWSER_MAX_URL];
    static char url_input[BROWSER_MAX_URL];
    static int  url_cursor;
    static bool url_focused;

    // Content
    static char page_title[128];
    static char page_content[BROWSER_MAX_CONTENT];
    static int  content_length;
    static int  scroll_y;
    static int  total_lines;
    static bool loading;
    static bool error;
    static char error_msg[128];

    // History
    static char history[BROWSER_MAX_HISTORY][BROWSER_MAX_URL];
    static int  history_count;
    static int  history_pos;

    // Links
    static BrowserLink links[BROWSER_MAX_LINKS];
    static int  link_count;

    // Tab state
    static int  active_tab;   // 0 = browser, 1 = bookmarks

    // Rendering
    static void RenderToolbar(Window* w);
    static void RenderContent(Window* w);
    static void RenderStatusBar(Window* w);
    static void RenderBookmarks(Window* w);

    // HTTP
    static bool FetchPage(const char* url);
    static void ParseHTML(const char* html, int len);
    static void RenderBuiltinPage(const char* page_name);

    // Helpers
    static void PushHistory(const char* url);
};
