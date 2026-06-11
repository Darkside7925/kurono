#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Taskbar & Desktop Environment
// ═══════════════════════════════════════════════════════════════════════════

#include "window_manager.h"
#include "../kernel/types.h"
#include "../media/mediadecoder.h"

#define TASKBAR_HEIGHT    44
#define TASKBAR_BTN_W     44
#define DESKTOP_MAX_ICONS 32
#define START_MENU_W      320
#define START_MENU_H      440
#define START_MAX_ITEMS   16
#define ICON_SIZE         56
#define ICON_SPACING_X    96
#define ICON_SPACING_Y    100
#define ICON_MARGIN_X     24
#define ICON_MARGIN_Y     20

struct DesktopIcon {
    char name[32];
    char path[64];
    int  x, y;
    int  icon_type;       // 0=file, 1=folder, 2=app, 3=system
    bool selected;
};

struct StartMenuItem {
    char label[32];
    char icon[8];
    int  action;          // 0=app, 1=command, 2=separator, 3=shutdown
    void (*handler)();
};

class Taskbar {
public:
    static void Init(int screen_w, int screen_h);
    static void Render();
    static bool HandleClick(int mx, int my);
    static void Update();

    static int  GetHeight();
    static int  GetY();

    // Notification area
    static void SetClock(int h, int m);
    static void SetBattery(int percent);
    static void SetWiFiConnected(bool connected, int strength);
    static void SetVolume(int percent);

    // Volume popup (public for drag handling in DesktopEnvironment)
    static bool volume_popup_open;
    static int  volume_slider_val;     // 0-100
    static bool volume_slider_dragging;
    static int screen_width, screen_height;
    static int y_pos;

    // Search
    static bool search_active;
    static char search_buf[64];
    static int  search_len;

private:
    static bool start_menu_open;
    static int hover_button;

    // System tray
    static int clock_h, clock_m;
    static int battery_pct;
    static bool wifi_connected;
    static int wifi_strength;
    static int volume_pct;

    static void RenderStartButton();
    static void RenderTaskButtons();
    static void RenderSystemTray();
    static void RenderStartMenu();
    static void RenderClock();
    static void RenderVolumePopup();
    static void RenderSearchResults();
};

class Desktop {
public:
    static void Init(int screen_w, int screen_h);
    static void Render();
    static bool HandleClick(int mx, int my);
    static void HandleDoubleClick(int mx, int my);
    static void HandleRightClick(int mx, int my);
    static void Update(int mx, int my, bool mouse_down, bool clicked);

    static void AddIcon(const char* name, const char* path, int icon_type);
    static void ArrangeIcons();

    // Wallpaper
    static void SetWallpaper(unsigned int color);
    static void SetWallpaperImage(const MediaDecoder::Image& img);

private:
    static int screen_width, screen_height;
    static unsigned int wallpaper_color;
    static DesktopIcon icons[DESKTOP_MAX_ICONS];
    static int icon_count;
    static int selected_icon;
    static bool context_menu_open;
    static int context_menu_x, context_menu_y;

    // Cached wallpaper pixels (full screen)
    static uint32_t* gradient_cache;  // full screen pixel cache
    static int gradient_cache_h;
    static bool have_image_wallpaper;  // true if we have a real image

    static void RenderWallpaper();
    static void RenderIcon(DesktopIcon* icon);
    static void RenderContextMenu();
    static int  IconAt(int mx, int my);
};

// Combined desktop environment
class DesktopEnvironment {
public:
    static void Init(int screen_w, int screen_h);
    static void Render();
    static void HandleInput(int mx, int my, bool mouse_down, bool clicked, char key);
    static void Update();

    // App launchers
    static void LaunchTerminal();
    static void LaunchFileBrowser();
    static void LaunchCalculator();
    static void LaunchTextEditor();
    static void LaunchSettings();
    static void LaunchTaskManager();
    static void LaunchBrowser();
    static void LaunchMediaPlayer();
};
