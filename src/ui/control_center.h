#pragma once
//  kurono os - control center popup (quick settings, media, power)
//  opened from the menu bar cluster or the classic tray on the primary output.

#include "../kernel/types.h"

class ControlCenter {
public:
    static void Init(int screen_w, int screen_h);
    static void OnScreenResize(int screen_w, int screen_h);

    static void Toggle();
    static void Open();
    static void OpenAt(int anchor_x, int anchor_y); // anchor is the tray/menu point
    static void ToggleAt(int anchor_x, int anchor_y);
    static void Close();
    static bool IsOpen();
    // true while the open/close spring is still moving. (satoru)
    static bool IsAnimating();

    // returns true if the click was consumed by the panel
    static bool HandleClick(int mx, int my);
    // pointer state every frame (slider drag while the button is held). (satoru)
    static void HandleMouse(int mx, int my, bool down);

    // raw key edge from the input manager. returns true while the panel owns
    // the keyboard so nothing leaks to the focused window. (satoru)
    static bool HandleKeyEvent(int key, bool shift, bool ctrl, bool alt, bool pressed);
    // printable character from the gui loop (wifi password field). (satoru)
    static bool HandleChar(char c);
    static bool CapturesKeyboard();

    // render is called every frame after the taskbar so it floats on top
    static void Render();
    // per-frame housekeeping: spring, recording capture, live refresh. (satoru)
    static void Tick();

    // test hook: run the exact same power action the confirm button runs.
    // action is the internal PA_* value (1 sleep, 2 restart, 3 shutdown). (satoru)
    static void ExecutePowerActionForTest(int action);

    // pixel rect of the panel (for hit-testing externally if needed)
    static int GetX();
    static int GetY();
    static int GetW();
    static int GetH();

    // geometry: the shell reads these for hit testing and the draw helpers in
    // the implementation share them. (satoru)
    static bool open;
    static int  screen_w, screen_h;     // virtual desktop fallback
    static int  panel_x, panel_y, panel_w, panel_h;
};
