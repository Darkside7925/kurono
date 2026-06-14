#pragma once
//  kurono os  -  taskbar & desktop environment

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
#define DESKTOP_ICON_PATH_MAX 128

struct KVFSNode;

struct DesktopIcon {
    char name[32];
    char path[DESKTOP_ICON_PATH_MAX];
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
    static void ReloadFromConfig();          // re-read uiconfig colors/sizes

    static int  GetHeight();
    static int  GetY();

    // true while the start menu or volume popup is open OR still mid
    // open/close animation (phase not yet settled). (satoru)
    static bool IsAnimating();

    // notification area
    static void SetClock(int h, int m);
    static void SetBattery(int percent);
    static void SetWiFiConnected(bool connected, int strength);
    static void SetVolume(int percent);

    // volume popup (public for drag handling in desktopenvironment)
    static bool volume_popup_open;
    static int  volume_slider_val;     // 0-100
    static bool volume_slider_dragging;
    static int screen_width, screen_height;
    static int y_pos;

    // search
    static bool search_active;
    static char search_buf[64];
    static int  search_len;

    // these mirror the uiconfig keys and are re-read by reloadfromconfig.
    // the taskbar_height macro remains the fallback used by external code
    // that does not route through taskbar::getheight().
    static int cfg_height;
    static uint32_t cfg_col_bg;
    static uint32_t cfg_col_top;
    static uint32_t cfg_col_text;
    static uint32_t cfg_col_start_btn;
    static uint32_t cfg_col_start_hover;
    static bool cfg_show_clock;
    static bool cfg_show_battery;
    static bool cfg_show_wifi;
    static bool cfg_show_volume;
    static bool cfg_show_search;
    static bool cfg_position_top;    // false=bottom, true=top

private:
    static bool start_menu_open;
    static int hover_button;

    // system tray
    static int clock_h, clock_m;
    // cached date + once-per-second refresh gate so the clock is not
    // recomputed from the rtc every render frame (satoru)
    static int clock_mon, clock_dom, clock_year;
    static uint32_t clock_last_update_ms;
    static int battery_pct;
    static bool wifi_connected;
    static int wifi_strength;
    static int volume_pct;

    // animation state  -  all phases are time-driven (Timer::GetRealMs).
    static uint32_t start_menu_anim_start_ms;  // 0 = idle
    static bool     start_menu_anim_opening;   // true=opening, false=closing
    static float    start_menu_phase;          // 0..1 eased
    static uint32_t volume_popup_anim_start_ms;
    static bool     volume_popup_anim_opening;
    static float    volume_popup_phase;

    // per-pinned-icon hover scale (time-eased 0..1).
    static float    pinned_hover_phase[8];     // sized for tb_pinned cap
    static int      pinned_hover_target;       // index currently hovered, -1 none
    static uint32_t pinned_anim_last_ms;
    // ms timestamp of the last launch tap on each pinned icon; 0 = idle. drives a
    // spring "bounce" pop when the app is launched. (satoru)
    static uint32_t pinned_launch_ms[8];

    // cursor blink driven by real time.
    static uint32_t search_cursor_t0_ms;

public:
    // Called by DesktopEnvironment each frame with delta_ms since last frame.
    static void Tick(uint32_t delta_ms, int mx, int my);

    // bounds-checked read access to per-pinned-icon hover phase so file-scope
    // taskbar renderers can read it without touching private anim state (satoru)
    static float PinnedHoverPhase(int i){ return (i >= 0 && i < 8) ? pinned_hover_phase[i] : 0.0f; }
    // record a launch tap so the icon bounces; read the bounce timestamp. (satoru)
    static void  PinnedLaunched(int i);
    static uint32_t PinnedLaunchMs(int i){ return (i >= 0 && i < 8) ? pinned_launch_ms[i] : 0; }

private:
    static void StartMenuSet(bool open);
    static void VolumePopupSet(bool open);
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
    static void RefreshFiles();
    static void RemoveIcon(int index);            // delete icon + backing file
    static bool CreateFolderInteractive();        // "new folder n"
    static bool CreateFileInteractive();          // "new file n.txt"
    static void ReloadFromConfig();               // reapply uiconfig sizes/colors

    // wallpaper
    static void SetWallpaper(unsigned int color);
    static void SetWallpaperImage(const MediaDecoder::Image& img);
    static bool ApplyBuiltinWallpaper(int idx);   // 0 = primary, 1 = secondary embedded png (satoru)

    // true while any desktop-icon hover-pop is still easing (~160ms). The
    // damage-gated GUI loop renders continuously while this holds so the
    // hover lift never freezes mid-transition once the pointer stops. (satoru)
    static bool IsAnimating();

private:
    static int screen_width, screen_height;
    static unsigned int wallpaper_color;
    static DesktopIcon icons[DESKTOP_MAX_ICONS];
    static int icon_count;
    static int selected_icon;
    static bool context_menu_open;
    static int context_menu_x, context_menu_y;
    static int context_menu_target;         // icon index right-clicked, or -1
    static uint32_t context_menu_open_ms;   // ms the menu opened; drives fade+scale-in (satoru)
    static int new_folder_counter;
    static int new_file_counter;
    static bool icon_dragging;
    static bool icon_drag_moved;
    static int drag_icon;
    static int drag_offset_x;
    static int drag_offset_y;
    static int drag_start_x;
    static int drag_start_y;
    static uint32_t last_file_sync_ms;

    // runtime-sized copies of the desktop grid constants (driven by uiconfig).
    static int cfg_icon_size;
    static int cfg_spacing_x;
    static int cfg_spacing_y;
    static int cfg_margin_x;
    static int cfg_margin_y;
    static uint32_t cfg_col_desk_bg;
    static uint32_t cfg_col_icon_text;
    static uint32_t cfg_col_icon_sel;
    static uint32_t cfg_col_ctx_bg;
    static uint32_t cfg_col_ctx_border;
    static uint32_t cfg_col_ctx_text;
    static int cfg_ctx_item_h;
    static int cfg_ctx_width;
    static bool cfg_allow_edit;

    // cached wallpaper pixels (full screen)
    static uint32_t* gradient_cache;  // full screen pixel cache
    static int gradient_cache_h;
    static size_t gradient_cache_bytes; // for pmm freeing
    static bool have_image_wallpaper;  // true if we have a real image

    // retained source image for the wallpaper so it can be re-scaled when the
    // screen resolution changes (otherwise a mode switch drops the image and
    // falls back to the procedural gradient). (satoru)
    static uint8_t* wallpaper_src;
    static int      wallpaper_src_w;
    static int      wallpaper_src_h;
    static int      wallpaper_src_order;     // 0 = rgba, 1 = bgra
    static size_t   wallpaper_src_bytes;
    static bool ScaleWallpaperCache(int w, int h);  // (re)build gradient_cache from wallpaper_src

    // Per-icon hover-scale phase (0..1)  -  used by RenderIcon for an
    // ease-out cubic pop on hover.  Updated by Tick().
    static float    icon_hover_phase[DESKTOP_MAX_ICONS];
    static int      icon_hover_target;
    static uint32_t icon_anim_last_ms;
    static uint32_t last_render_ms;
    static int      hovered_icon;       // last frame's hovered icon (for damage)

    static void RenderWallpaper();
    static void RenderIcon(DesktopIcon* icon, float hover_t);
    static void RenderContextMenu();
    static int  IconAt(int mx, int my);
    static int  FindIconByPath(const char* path);
    static bool IsDesktopFileIcon(const DesktopIcon* icon);
    static void AddOrUpdateDesktopFile(KVFSNode* node);
    static void PlaceIcon(int index);
    static void ClampIcon(int index);

public:
    static void Tick(uint32_t delta_ms, int mx, int my);
};

// combined desktop environment
class DesktopEnvironment {
public:
    static void Init(int screen_w, int screen_h);
    static void Render();
    static void HandleInput(int mx, int my, bool mouse_down, bool clicked, char key);
    static void Update();
    static void ReloadFromConfig();          // called by `kurono reload`

    // app launchers
    static void LaunchTerminal();
    static void LaunchFileBrowser();
    static void LaunchCalculator();
    static void LaunchTextEditor();
    static void LaunchSettings();
    static void LaunchTaskManager();
    static void LaunchBrowser();
    static void LaunchMediaPlayer();

    // session control
    static void RequestLogout();
    static bool ConsumeLogoutRequest();
};
