#pragma once
//  kurono os  -  settings application (advanced)

enum SettingsTab {
    STAB_DISPLAY      = 0,
    STAB_SOUND        = 1,
    STAB_NETWORK      = 2,
    STAB_STORAGE      = 3,
    STAB_POWER        = 4,
    STAB_PERSONALIZE  = 5,
    STAB_SECURITY     = 6,
    STAB_PACKAGES     = 7,
    STAB_UPDATES      = 8,
    STAB_SYSTEM       = 9,
    STAB_ABOUT        = 10,
    STAB_COUNT        = 11,
};

struct SettingsState {
    // display
    int  brightness;        // 0-100
    bool cursor_blink;
    bool dark_mode;
    bool animations;
    int  font_scale;        // 1, 2, 3
    int  wallpaper_idx;     // 0=default, 1=gradient, 2=solid
    int  resolution_idx;    // 0=1024x768, 1=1920x1080, 2=2560x1440
    int  refresh_rate;      // 60, 120, 144, 240
    int  mouse_sensitivity; // 1-4
    // sound
    int  master_volume;     // 0-100
    int  alert_volume;      // 0-100
    bool muted;
    int  output_device;     // 0=speakers, 1=hdmi, 2=headphones
    bool spatial_audio;
    // network
    bool wifi_enabled;
    bool bluetooth_enabled;
    int  wifi_scan_count;
    int  bt_scan_count;
    bool wifi_scanning;
    bool bt_scanning;
    // storage
    // (computed live)
    // power
    int  power_plan;        // 0=balanced, 1=performance, 2=power_saver
    int  sleep_timeout;     // minutes 0=never
    int  screen_timeout;    // minutes 0=never
    bool fast_startup;
    // personalization
    int  accent_color;      // 0-7 palette index
    bool show_desktop_icons;
    int  taskbar_pos;       // 0=bottom, 1=top
    int  icon_size;         // 0=small, 1=medium, 2=large
    bool transparency;
    // updates
    bool auto_update;
    int  update_status;     // 0=idle, 1=checking, 2=available, 3=downloading, 4=up-to-date
    int  update_progress;   // 0-100 during download
    int  last_check_mins;   // minutes since last check
    // system / linux guest
    bool linux_guest_enabled;
    int  linux_guest_profile; // 0=alpine, 1=debian
};

class SettingsApp {
public:
    static void Init();
    static int  Open();

    static void Render(void* win, int x, int y, int w, int h);
    static bool Input(void* win, int mx, int my, bool clicked, char key);

    // deferred resolution change  -  called from main loop between frames
    static void PollDeferredActions();

    static SettingsState state;
    static int pending_resolution_idx;  // -1 = none pending

private:
    static SettingsTab current_tab;
    static int scroll_offset;

    static void RenderSidebar(int x, int y, int w, int h);
    static void RenderDisplay(int x, int y, int w, int h);
    static void RenderSound(int x, int y, int w, int h);
    static void RenderNetwork(int x, int y, int w, int h);
    static void RenderStorage(int x, int y, int w, int h);
    static void RenderPower(int x, int y, int w, int h);
    static void RenderPersonalize(int x, int y, int w, int h);
    static void RenderSecurity(int x, int y, int w, int h);
    static void RenderPackages(int x, int y, int w, int h);
    static void RenderUpdates(int x, int y, int w, int h);
    static void RenderSystem(int x, int y, int w, int h);
    static void RenderAbout(int x, int y, int w, int h);

    static bool HandleDisplayInput(int rx, int ry, int pw, int ph);
    static bool HandleSoundInput(int rx, int ry, int pw, int ph);
    static bool HandleNetworkInput(int rx, int ry, int pw, int ph);
    static bool HandlePowerInput(int rx, int ry, int pw, int ph);
    static bool HandlePersonalizeInput(int rx, int ry, int pw, int ph);
    static bool HandleUpdatesInput(int rx, int ry, int pw, int ph);
    static bool HandleSystemInput(int rx, int ry, int pw, int ph);
};
