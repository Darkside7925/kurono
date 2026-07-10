#pragma once
//  kurono os - control center popup (wifi / bt / volume / brightness / quick actions)
//  triggered by clicking the system tray cluster on the taskbar.

#include "../kernel/types.h"

class ControlCenter {
public:
    static void Init(int screen_w, int screen_h);
    static void OnScreenResize(int screen_w, int screen_h);

    static void Toggle();
    static void Open();
    static void OpenAt(int anchor_x, int anchor_y); // anchor bottom-right of panel near point
    static void ToggleAt(int anchor_x, int anchor_y);
    static void Close();
    static bool IsOpen();
    // true while open OR still running the slide/fade open-close animation.
    static bool IsAnimating();

    // returns true if the click was consumed by the panel
    static bool HandleClick(int mx, int my);

    // render is called every frame after the taskbar so it floats on top
    static void Render();

    // pixel rect of the panel (for hit-testing externally if needed)
    static int  GetX();
    static int  GetY();
    static int  GetW();
    static int  GetH();

private:
    static bool open;
    static int  screen_w, screen_h;
    static int  panel_x, panel_y, panel_w, panel_h;

    // dragging the brightness/volume sliders
    static bool dragging_volume;
    static bool dragging_brightness;
    static int  brightness_pct;       // 0..100 (mirrored to settings if available)

    static void Layout();
    static void DrawHeader(int x, int y, int w);
    static void DrawTile(int x, int y, int w, int h, const char* label,
                         const char* sub, bool active, uint32_t accent);
    static void DrawSlider(int x, int y, int w, const char* label, int pct, uint32_t fill);
    static void DrawUserCard(int x, int y, int w);

    // hit-tested regions (filled in by render, used by HandleClick)
    static int wifi_x, wifi_y, wifi_w, wifi_h;
    static int bt_x, bt_y, bt_w, bt_h;
    static int air_x, air_y, air_w, air_h;
    static int night_x, night_y, night_w, night_h;
    static int focus_x, focus_y, focus_w, focus_h;
    static int dnd_x, dnd_y, dnd_w, dnd_h;
    static int bright_track_x, bright_track_y, bright_track_w;
    static int vol_track_x, vol_track_y, vol_track_w;
    static int signout_x, signout_y, signout_w, signout_h;
    static int settings_x, settings_y, settings_w, settings_h;
    static int lock_x, lock_y, lock_w, lock_h;

    // quick toggle local state (real backends wired where available)
    static bool airplane_mode;
    static bool night_light;
    static bool focus_mode;
    static bool do_not_disturb;
};
