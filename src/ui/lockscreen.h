#pragma once
#include "../kernel/types.h"

class LockScreen {
public:
    static void Show();
    static bool frame_clicked;
    static bool frame_has_input;
    
    enum State {
        Idle,       // clock and date
        Transition, // blurring/moving
        Login,      // user login
        Setup,      // create user
        FileBrowse  // pick profile pic
    };
    
private:
    static State current_state;
    static float transition_t; // 0.0 to 1.0
    
    static void DrawIdle(int w, int h);
    static void DrawLogin(int w, int h);
    static void DrawSetup(int w, int h);
    static void Update();
    
    // helpers
    static const char* DowName(uint8_t v);
    static const char* MonName(uint8_t v);
    static void FormatDate(char* out, size_t cap, const char* dow, const char* mon, uint8_t dom);
    static void DrawStringCenter(int cx, int y, int s, const char* str, uint32_t color);
    static void DrawString(int x, int y, int s, const char* str, uint32_t color);
    static const uint8_t* GlyphLS(char c);
    static void DrawDigit(int x, int y, int s, int d, uint32_t color);
    static void DrawColon(int x, int y, int s, uint32_t color, bool on);
    static void DrawTime(int tx, int ty, int s, uint8_t hh, uint8_t mm, uint32_t color, bool colon);
};
