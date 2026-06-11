// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Basic Web Browser Implementation
//  Built-in pages + simulated HTTP browsing
// ═══════════════════════════════════════════════════════════════════════════
#include "browser.h"
#include "../drivers/graphics.h"
#include "../net/network.h"
#include <string.h>

// ── Static member initialization ──
int  BrowserApp::win_id         = -1;
char BrowserApp::current_url[BROWSER_MAX_URL] = "kurono://home";
char BrowserApp::url_input[BROWSER_MAX_URL]   = "kurono://home";
int  BrowserApp::url_cursor     = 0;
bool BrowserApp::url_focused    = false;
char BrowserApp::page_title[128]     = "Home";
char BrowserApp::page_content[BROWSER_MAX_CONTENT] = "";
int  BrowserApp::content_length = 0;
int  BrowserApp::scroll_y       = 0;
int  BrowserApp::total_lines    = 0;
bool BrowserApp::loading        = false;
bool BrowserApp::error          = false;
char BrowserApp::error_msg[128] = "";
char BrowserApp::history[BROWSER_MAX_HISTORY][BROWSER_MAX_URL];
int  BrowserApp::history_count  = 0;
int  BrowserApp::history_pos    = -1;
BrowserLink BrowserApp::links[BROWSER_MAX_LINKS];
int  BrowserApp::link_count     = 0;
int  BrowserApp::active_tab     = 0;

// ── Helpers ──
static int br_slen(const char* s) { int n=0; if(s) while(s[n]) n++; return n; }
static void br_scpy(char* d, const char* s, int mx) {
    int i=0; if(s) while(s[i] && i<mx-1) { d[i]=s[i]; i++; } d[i]=0;
}
static void br_scat(char* d, const char* s, int mx) {
    int n=br_slen(d), i=0; if(s) while(s[i] && n<mx-1) { d[n++]=s[i++]; } d[n]=0;
}
static bool br_starts(const char* s, const char* pf) {
    if (!s || !pf) return false;
    while (*pf) { if (*s != *pf) return false; s++; pf++; }
    return true;
}
static bool br_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

// ── Colors ──
static const uint32_t BR_BG          = 0xFF101018;
static const uint32_t BR_TOOLBAR     = 0xFF1A1A30;
static const uint32_t BR_URL_BG      = 0xFF0D0D1A;
static const uint32_t BR_URL_BORDER  = 0xFF333355;
static const uint32_t BR_URL_FOCUS   = 0xFF5C8AFF;
static const uint32_t BR_TEXT        = 0xFFE0E0F0;
static const uint32_t BR_TEXT_DIM    = 0xFF888899;
static const uint32_t BR_LINK_COL    = 0xFF5C8AFF;
static const uint32_t BR_LINK_VISIT  = 0xFF9B59B6;
static const uint32_t BR_HEADING     = 0xFFF0F0FF;
static const uint32_t BR_STATUS_BG   = 0xFF151530;
static const uint32_t BR_BTN_BG      = 0xFF252540;
static const uint32_t BR_BTN_HOVER   = 0xFF353560;
static const uint32_t BR_ACCENT      = 0xFF5C8AFF;
static const uint32_t BR_WHITE       = 0xFFFFFFFF;

// ═══════════════════════════════════════════════════════════════════════════
//  Window Management
// ═══════════════════════════════════════════════════════════════════════════

void BrowserApp::Open() {
    if (win_id >= 0) return;

    win_id = WindowManager::CreateWindow("Kurono Browser", 80, 60, 640, 440,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)cx; (void)cy; (void)cw; (void)ch;
            BrowserApp::OnRender(w);
        },
        (WindowInputFunc)BrowserApp::OnInput
    );
    if (win_id < 0) return;

    // Navigate to home page
    Navigate("kurono://home");
}

void BrowserApp::Open(const char* url) {
    Open();
    if (url) Navigate(url);
}

void BrowserApp::Close() {
    if (win_id >= 0) {
        WindowManager::CloseWindow(win_id);
        win_id = -1;
    }
}

bool BrowserApp::IsOpen() {
    return win_id >= 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Navigation
// ═══════════════════════════════════════════════════════════════════════════

void BrowserApp::PushHistory(const char* url) {
    // Truncate forward history
    history_pos++;
    if (history_pos >= BROWSER_MAX_HISTORY) {
        // Shift everything down
        for (int i = 0; i < BROWSER_MAX_HISTORY - 1; i++) {
            br_scpy(history[i], history[i+1], BROWSER_MAX_URL);
        }
        history_pos = BROWSER_MAX_HISTORY - 1;
    }
    br_scpy(history[history_pos], url, BROWSER_MAX_URL);
    history_count = history_pos + 1;
}

void BrowserApp::Navigate(const char* url) {
    if (!url) return;

    br_scpy(current_url, url, BROWSER_MAX_URL);
    br_scpy(url_input, url, BROWSER_MAX_URL);
    url_cursor = br_slen(url_input);
    scroll_y = 0;
    link_count = 0;
    error = false;
    loading = true;

    PushHistory(url);

    // Check for built-in pages
    if (br_starts(url, "kurono://")) {
        const char* page = url + 9;  // Skip "kurono://"
        RenderBuiltinPage(page);
        loading = false;
        return;
    }

    // HTTP/HTTPS fetch (simulated)
    if (!FetchPage(url)) {
        error = true;
        br_scpy(error_msg, "Could not connect to server", 128);
        br_scpy(page_title, "Error", 128);
        content_length = 0;
        page_content[0] = 0;
    }

    loading = false;
}

void BrowserApp::GoBack() {
    if (history_pos <= 0) return;
    history_pos--;
    // Navigate without pushing to history
    const char* url = history[history_pos];
    br_scpy(current_url, url, BROWSER_MAX_URL);
    br_scpy(url_input, url, BROWSER_MAX_URL);
    url_cursor = br_slen(url_input);
    scroll_y = 0;
    link_count = 0;
    error = false;

    if (br_starts(url, "kurono://")) {
        RenderBuiltinPage(url + 9);
    } else {
        FetchPage(url);
    }
}

void BrowserApp::GoForward() {
    if (history_pos >= history_count - 1) return;
    history_pos++;
    const char* url = history[history_pos];
    br_scpy(current_url, url, BROWSER_MAX_URL);
    br_scpy(url_input, url, BROWSER_MAX_URL);
    url_cursor = br_slen(url_input);
    scroll_y = 0;
    link_count = 0;
    error = false;

    if (br_starts(url, "kurono://")) {
        RenderBuiltinPage(url + 9);
    } else {
        FetchPage(url);
    }
}

void BrowserApp::Refresh() {
    Navigate(current_url);
}

void BrowserApp::GoHome() {
    Navigate("kurono://home");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Built-in Pages
// ═══════════════════════════════════════════════════════════════════════════

void BrowserApp::RenderBuiltinPage(const char* page) {
    page_content[0] = 0;
    content_length = 0;
    link_count = 0;

    if (br_eq(page, "home")) {
        br_scpy(page_title, "Kurono Browser — Home", 128);
        br_scpy(page_content,
            "=== Welcome to Kurono Browser ===\n"
            "\n"
            "Your gateway to the web on Kurono OS.\n"
            "\n"
            "--- Quick Links ---\n"
            "\n"
            "> kurono://about        About this browser\n"
            "> kurono://bookmarks    Your bookmarks\n"
            "> kurono://settings     Browser settings\n"
            "> kurono://network      Network status\n"
            "\n"
            "--- Search ---\n"
            "\n"
            "Type a URL in the address bar above to browse.\n"
            "Try: google.com, github.com, example.com\n"
            "\n"
            "--- Features ---\n"
            "\n"
            "* Text-mode web rendering\n"
            "* DNS hostname resolution\n"
            "* Built-in page system (kurono://)\n"
            "* Navigation history (back/forward)\n"
            "* Keyboard shortcuts\n"
            "\n"
            "Kurono Browser v1.0 — Built into Kurono OS\n",
            BROWSER_MAX_CONTENT);
    }
    else if (br_eq(page, "about")) {
        br_scpy(page_title, "About Kurono Browser", 128);
        br_scpy(page_content,
            "=== About Kurono Browser ===\n"
            "\n"
            "Version: 1.0.0\n"
            "Engine:  KuronoText/1.0\n"
            "OS:      Kurono OS v1.0\n"
            "\n"
            "A lightweight text-mode web browser built\n"
            "directly into the Kurono OS kernel.\n"
            "\n"
            "Features:\n"
            "  - HTTP/1.1 protocol support\n"
            "  - DNS hostname resolution\n"
            "  - Text-mode HTML rendering\n"
            "  - Navigation history\n"
            "  - Built-in quick pages\n"
            "\n"
            "Keyboard Shortcuts:\n"
            "  Alt+Left   — Go back\n"
            "  Alt+Right  — Go forward\n"
            "  F5         — Refresh\n"
            "  Alt+Home   — Go home\n"
            "\n"
            "> kurono://home  Return to home\n",
            BROWSER_MAX_CONTENT);
    }
    else if (br_eq(page, "bookmarks")) {
        br_scpy(page_title, "Bookmarks", 128);
        br_scpy(page_content,
            "=== Bookmarks ===\n"
            "\n"
            "> kurono://home        Home Page\n"
            "> kurono://about       About Browser\n"
            "> kurono://network     Network Status\n"
            "\n"
            "--- Websites ---\n"
            "\n"
            "> http://example.com   Example Domain\n"
            "> http://google.com    Google\n"
            "> http://github.com    GitHub\n"
            "\n",
            BROWSER_MAX_CONTENT);
    }
    else if (br_eq(page, "network")) {
        br_scpy(page_title, "Network Status", 128);
        // Build network info page
        char buf[BROWSER_MAX_CONTENT];
        br_scpy(buf, "=== Network Status ===\n\n", BROWSER_MAX_CONTENT);

        NetworkInterface* iface_ptr = Network::GetInterface("eth0");
        br_scat(buf, "Interface: eth0\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Status: ", BROWSER_MAX_CONTENT);
        br_scat(buf, (iface_ptr && iface_ptr->state == NIC_UP) ? "UP" : "DOWN", BROWSER_MAX_CONTENT);
        br_scat(buf, "\n", BROWSER_MAX_CONTENT);

        // IP address
        char ip_str[32];
        br_scpy(ip_str, "", 32);
        if(iface_ptr){
            for(int i=0; i<4; i++){
                int val = iface_ptr->ip.bytes[i];
                char t[4]; int n=0;
                do{t[n++]='0'+(val%10);val/=10;}while(val);
                int pos=br_slen(ip_str);
                while(n>0 && pos<30) ip_str[pos++]=t[--n];
                if(i<3 && pos<30) ip_str[pos++]='.';
                ip_str[pos]=0;
            }
        } else {
            br_scpy(ip_str, "0.0.0.0", 32);
        }
        br_scat(buf, "IP: ", BROWSER_MAX_CONTENT);
        br_scat(buf, ip_str, BROWSER_MAX_CONTENT);
        br_scat(buf, "\n\n", BROWSER_MAX_CONTENT);

        // DNS test
        br_scat(buf, "--- DNS Resolution Test ---\n\n", BROWSER_MAX_CONTENT);
        const char* test_hosts[] = {"google.com", "github.com", "example.com", "satorut.com"};
        for(int h=0; h<4; h++){
            IPv4Address resolved;
            Network::Resolve(test_hosts[h], &resolved);
            br_scat(buf, test_hosts[h], BROWSER_MAX_CONTENT);
            br_scat(buf, " -> ", BROWSER_MAX_CONTENT);
            char rip[32]; br_scpy(rip, "", 32);
            for(int i=0;i<4;i++){
                int val=resolved.bytes[i]; char t[4]; int n=0;
                do{t[n++]='0'+(val%10);val/=10;}while(val);
                int pos=br_slen(rip);
                while(n>0&&pos<30) rip[pos++]=t[--n];
                if(i<3&&pos<30) rip[pos++]='.';
                rip[pos]=0;
            }
            br_scat(buf, rip, BROWSER_MAX_CONTENT);
            br_scat(buf, "\n", BROWSER_MAX_CONTENT);
        }

        br_scat(buf, "\n> kurono://home  Return to home\n", BROWSER_MAX_CONTENT);
        br_scpy(page_content, buf, BROWSER_MAX_CONTENT);
    }
    else if (br_eq(page, "settings")) {
        br_scpy(page_title, "Browser Settings", 128);
        br_scpy(page_content,
            "=== Browser Settings ===\n"
            "\n"
            "Home Page: kurono://home\n"
            "Search Engine: KuronoSearch\n"
            "Default Encoding: UTF-8\n"
            "JavaScript: Not supported\n"
            "CSS: Not supported (text-mode only)\n"
            "Images: Not supported\n"
            "\n"
            "> kurono://home  Return to home\n",
            BROWSER_MAX_CONTENT);
    }
    else {
        br_scpy(page_title, "Not Found", 128);
        br_scpy(page_content,
            "=== Page Not Found ===\n"
            "\n"
            "The page kurono://",
            BROWSER_MAX_CONTENT);
        br_scat(page_content, page, BROWSER_MAX_CONTENT);
        br_scat(page_content,
            " could not be found.\n"
            "\n"
            "> kurono://home  Return to home\n",
            BROWSER_MAX_CONTENT);
    }

    content_length = br_slen(page_content);
}

// ═══════════════════════════════════════════════════════════════════════════
//  HTTP Fetch (simulated)
// ═══════════════════════════════════════════════════════════════════════════

bool BrowserApp::FetchPage(const char* url) {
    // Strip protocol prefix
    const char* host = url;
    if (br_starts(url, "http://")) host = url + 7;
    else if (br_starts(url, "https://")) host = url + 8;

    // Extract hostname (up to / or end)
    char hostname[128];
    int hi = 0;
    while (host[hi] && host[hi] != '/' && hi < 127) {
        hostname[hi] = host[hi];
        hi++;
    }
    hostname[hi] = 0;

    // Resolve hostname
    IPv4Address ip;
    if (!Network::Resolve(hostname, &ip)) {
        br_scpy(error_msg, "DNS resolution failed", 128);
        error = true;
        return false;
    }

    // Simulated page content based on hostname
    br_scpy(page_title, hostname, 128);
    char buf[BROWSER_MAX_CONTENT];
    br_scpy(buf, "=== ", BROWSER_MAX_CONTENT);
    br_scat(buf, hostname, BROWSER_MAX_CONTENT);
    br_scat(buf, " ===\n\n", BROWSER_MAX_CONTENT);

    // Generate simulated content based on the hostname
    if (br_starts(hostname, "google") || br_starts(hostname, "www.google")) {
        br_scat(buf, "Google\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "[Search]  _________________________  [Go]\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Feeling lucky? Try searching for something.\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://google.com/about   About Google\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://google.com/mail    Gmail\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://google.com/maps    Maps\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Google Search — I'm Feeling Lucky\n", BROWSER_MAX_CONTENT);
    }
    else if (br_starts(hostname, "github") || br_starts(hostname, "www.github")) {
        br_scat(buf, "GitHub: Let's build from here\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "The world's leading software development platform.\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Features:\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Code hosting & collaboration\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Issues & project management\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Actions CI/CD automation\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Copilot AI-powered coding\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://github.com/explore   Explore\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://github.com/trending   Trending\n", BROWSER_MAX_CONTENT);
    }
    else if (br_starts(hostname, "example") || br_starts(hostname, "www.example")) {
        br_scat(buf, "Example Domain\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "This domain is for use in illustrative examples\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "in documents. You may use this domain in\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "literature without prior coordination or asking\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "for permission.\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://www.iana.org   More information...\n", BROWSER_MAX_CONTENT);
    }
    else if (br_starts(hostname, "satorut") || br_starts(hostname, "server.satorut")) {
        br_scat(buf, "Satorut — Kurono OS Project\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Welcome to the official Kurono OS website.\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Downloads:\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Kurono OS v1.0 — Latest stable release\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "  * Documentation & API reference\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "> http://satorut.com/docs   Documentation\n", BROWSER_MAX_CONTENT);
    }
    else {
        br_scat(buf, "Connected to ", BROWSER_MAX_CONTENT);
        br_scat(buf, hostname, BROWSER_MAX_CONTENT);
        br_scat(buf, "\n\n", BROWSER_MAX_CONTENT);

        // Show resolved IP
        br_scat(buf, "Resolved IP: ", BROWSER_MAX_CONTENT);
        char rip[32]; br_scpy(rip, "", 32);
        for(int i=0;i<4;i++){
            int val=ip.bytes[i]; char t[4]; int n=0;
            do{t[n++]='0'+(val%10);val/=10;}while(val);
            int pos=br_slen(rip);
            while(n>0&&pos<30) rip[pos++]=t[--n];
            if(i<3&&pos<30) rip[pos++]='.';
            rip[pos]=0;
        }
        br_scat(buf, rip, BROWSER_MAX_CONTENT);
        br_scat(buf, "\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Server response: 200 OK\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "Content-Type: text/html\n\n", BROWSER_MAX_CONTENT);
        br_scat(buf, "[Page content would be rendered here]\n", BROWSER_MAX_CONTENT);
    }

    br_scat(buf, "\n--- End of page ---\n", BROWSER_MAX_CONTENT);
    br_scpy(page_content, buf, BROWSER_MAX_CONTENT);
    content_length = br_slen(page_content);
    return true;
}

void BrowserApp::ParseHTML(const char* html, int len) {
    // Simple HTML to text conversion — strip tags
    (void)html; (void)len;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Rendering
// ═══════════════════════════════════════════════════════════════════════════

void BrowserApp::RenderToolbar(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;

    // Toolbar background
    Graphics::FillRect(cx, cy, cw, 36, BR_TOOLBAR);

    // Back button
    int bx = cx + 4;
    Graphics::FillRoundedRect(bx, cy + 4, 28, 28, 4, BR_BTN_BG);
    Graphics::DrawString(bx + 8, cy + 10, "<", BR_TEXT, 0xFF000000);
    bx += 32;

    // Forward button
    Graphics::FillRoundedRect(bx, cy + 4, 28, 28, 4, BR_BTN_BG);
    Graphics::DrawString(bx + 8, cy + 10, ">", BR_TEXT, 0xFF000000);
    bx += 32;

    // Refresh button
    Graphics::FillRoundedRect(bx, cy + 4, 28, 28, 4, BR_BTN_BG);
    Graphics::DrawString(bx + 6, cy + 10, "O", BR_TEXT, 0xFF000000);
    bx += 32;

    // Home button
    Graphics::FillRoundedRect(bx, cy + 4, 28, 28, 4, BR_BTN_BG);
    Graphics::DrawString(bx + 6, cy + 10, "H", BR_TEXT, 0xFF000000);
    bx += 36;

    // URL bar
    int url_w = cw - bx - 4 + cx;
    uint32_t border = url_focused ? BR_URL_FOCUS : BR_URL_BORDER;
    Graphics::FillRect(bx, cy + 6, url_w, 24, BR_URL_BG);
    Graphics::DrawRect(bx, cy + 6, url_w, 24, border);

    // URL text (possibly truncated)
    char display_url[80];
    int url_len = br_slen(url_input);
    int max_chars = (url_w - 12) / 8;
    if (max_chars > 79) max_chars = 79;
    int start = 0;
    if (url_len > max_chars) start = url_len - max_chars;
    br_scpy(display_url, url_input + start, 80);

    // Secure indicator
    uint32_t url_col = BR_TEXT;
    if (br_starts(url_input, "kurono://")) {
        url_col = BR_ACCENT;
    }
    Graphics::DrawString(bx + 6, cy + 12, display_url, url_col, 0xFF000000);

    // Cursor
    if (url_focused) {
        int cursor_x = bx + 6 + (url_cursor - start) * 8;
        if (cursor_x > bx + 6 && cursor_x < bx + url_w - 4) {
            Graphics::FillRect(cursor_x, cy + 10, 1, 16, BR_WHITE);
        }
    }
}

void BrowserApp::RenderContent(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y + 40;  // Below toolbar
    int cw = w->content_w;
    int ch = w->content_h - 60;  // Leave room for status bar

    if (ch <= 0) return;

    // Content background
    Graphics::FillRect(cx, cy, cw, ch, BR_BG);

    if (loading) {
        Graphics::DrawString(cx + cw/2 - 40, cy + ch/2, "Loading...", BR_TEXT_DIM, 0xFF000000);
        return;
    }

    if (error) {
        // Error page
        Graphics::DrawString(cx + 20, cy + 20, "Error", 0xFFE74C3C, 0xFF000000);
        Graphics::DrawString(cx + 20, cy + 44, error_msg, BR_TEXT_DIM, 0xFF000000);
        Graphics::DrawString(cx + 20, cy + 68, "Check the URL and try again.", BR_TEXT_DIM, 0xFF000000);
        return;
    }

    // Render page content as text lines
    int line_h = 18;
    int y = cy + 8 - scroll_y;
    int x_margin = 12;
    int line = 0;
    int i = 0;

    while (i < content_length && page_content[i]) {
        // Find end of line
        int line_start = i;
        while (i < content_length && page_content[i] && page_content[i] != '\n') i++;
        int line_len = i - line_start;
        if (i < content_length) i++;  // Skip newline

        // Only render visible lines
        if (y + line_h > cy && y < cy + ch) {
            // Extract line text
            char line_buf[128];
            int copy_len = (line_len > 127) ? 127 : line_len;
            for (int j = 0; j < copy_len; j++) line_buf[j] = page_content[line_start + j];
            line_buf[copy_len] = 0;

            // Style based on line content
            uint32_t color = BR_TEXT;
            int x = cx + x_margin;

            if (line_buf[0] == '=' && line_buf[1] == '=') {
                // Heading (=== text ===)
                color = BR_HEADING;
            }
            else if (line_buf[0] == '-' && line_buf[1] == '-') {
                // Subheading (--- text ---)
                color = BR_ACCENT;
            }
            else if (line_buf[0] == '>') {
                // Link line
                color = BR_LINK_COL;
                // Store link info for click detection
                if (link_count < BROWSER_MAX_LINKS) {
                    BrowserLink* lnk = &links[link_count];
                    lnk->x = x;
                    lnk->y = y;
                    lnk->w = copy_len * 8;
                    lnk->h = line_h;
                    lnk->line = line;
                    // Extract URL from line (after "> ")
                    const char* url_start = line_buf + 2;
                    while (*url_start == ' ') url_start++;
                    int ui = 0;
                    while (url_start[ui] && url_start[ui] != ' ' && ui < BROWSER_MAX_URL-1) {
                        lnk->url[ui] = url_start[ui];
                        ui++;
                    }
                    lnk->url[ui] = 0;
                    link_count++;
                }
            }
            else if (line_buf[0] == '*') {
                // List item
                color = BR_TEXT;
            }

            Graphics::DrawString(x, y, line_buf, color, 0xFF000000);
        }

        y += line_h;
        line++;
    }

    total_lines = line;

    // Scrollbar
    if (total_lines * line_h > ch) {
        int sb_h = (ch * ch) / (total_lines * line_h);
        if (sb_h < 20) sb_h = 20;
        int sb_y = cy + (scroll_y * (ch - sb_h)) / (total_lines * line_h - ch);
        if (sb_y < cy) sb_y = cy;
        if (sb_y + sb_h > cy + ch) sb_y = cy + ch - sb_h;
        Graphics::FillRect(cx + cw - 6, sb_y, 4, sb_h, 0xFF444466);
    }
}

void BrowserApp::RenderStatusBar(Window* w) {
    int cx = w->content_x;
    int cw = w->content_w;
    int ch = w->content_h;
    int sy = w->content_y + ch - 20;

    Graphics::FillRect(cx, sy, cw, 20, BR_STATUS_BG);
    Graphics::DrawString(cx + 8, sy + 4, page_title, BR_TEXT_DIM, 0xFF000000);

    // Connection status
    const char* status = loading ? "Loading..." : (error ? "Error" : "Done");
    int sw = br_slen(status) * 8;
    Graphics::DrawString(cx + cw - sw - 8, sy + 4, status, BR_TEXT_DIM, 0xFF000000);
}

void BrowserApp::OnRender(Window* w) {
    if (!w) return;

    RenderToolbar(w);
    RenderContent(w);
    RenderStatusBar(w);
}

void BrowserApp::OnInput(Window* w, int event, int a, int b) {
    if (!w) return;

    if (event == 1) {
        // Mouse click
        int cx = w->content_x;
        int cy = w->content_y;
        int cw = w->content_w;

        // Toolbar clicks
        if (b >= cy && b < cy + 36) {
            int bx = cx + 4;
            // Back
            if (a >= bx && a < bx + 28) { GoBack(); return; }
            bx += 32;
            // Forward
            if (a >= bx && a < bx + 28) { GoForward(); return; }
            bx += 32;
            // Refresh
            if (a >= bx && a < bx + 28) { Refresh(); return; }
            bx += 32;
            // Home
            if (a >= bx && a < bx + 28) { GoHome(); return; }
            bx += 36;

            // URL bar click
            if (a >= bx) {
                url_focused = true;
                // Position cursor
                int rel = a - bx - 6;
                url_cursor = rel / 8;
                int len = br_slen(url_input);
                if (url_cursor > len) url_cursor = len;
                if (url_cursor < 0) url_cursor = 0;
                return;
            }
        }
        else {
            url_focused = false;
        }

        // Content area link clicks
        int content_y = cy + 40;
        if (b >= content_y) {
            for (int i = 0; i < link_count; i++) {
                BrowserLink* lnk = &links[i];
                if (a >= lnk->x && a < lnk->x + lnk->w &&
                    b >= lnk->y && b < lnk->y + lnk->h) {
                    Navigate(lnk->url);
                    return;
                }
            }
        }
    }

    if (event == 2) {
        // Keyboard
        if (url_focused) {
            if (a == '\n' || a == '\r') {
                url_focused = false;
                Navigate(url_input);
                return;
            }
            if (a == '\b' || a == 127) {
                if (url_cursor > 0) {
                    int len = br_slen(url_input);
                    for (int i = url_cursor - 1; i < len; i++) {
                        url_input[i] = url_input[i+1];
                    }
                    url_cursor--;
                }
                return;
            }
            // Printable character
            if (a >= 32 && a < 127) {
                int len = br_slen(url_input);
                if (len < BROWSER_MAX_URL - 1) {
                    for (int i = len + 1; i > url_cursor; i--) {
                        url_input[i] = url_input[i-1];
                    }
                    url_input[url_cursor] = (char)a;
                    url_cursor++;
                }
                return;
            }
        }
    }

    if (event == 3) {
        // Scroll
        scroll_y -= a * 18;
        if (scroll_y < 0) scroll_y = 0;
        int max_scroll = total_lines * 18 - (w->content_h - 60);
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
    }
}
