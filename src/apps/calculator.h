#pragma once
#include "../kernel/types.h"

class Calculator {
public:
    static void Init(int x, int y);
    static void Draw();
    static void Update();
    static bool Input(int mx, int my, bool clicked, char key);
    static bool IsActive();
    static void SetActive(bool active);
    static void SetPosition(int x, int y);
    static void SetSize(int w, int h);
    
private:
    static int window_x, window_y, window_w, window_h;
    static bool active;
    static char display_buffer[32];
    static int current_val;
    static int active_op; // 0: none, 1: +, 2: -, 3: *, 4: /
    static int stored_val;
    static bool new_entry;
    static int selected_button;
    static int hover_button;        // -1 when none
    static unsigned int press_ms;   // time of last button press (for flash anim)
    static int press_button;        // last pressed button index
    static int error_state;         // 0=ok, 1=div-by-zero flash
    static void ClearAll();
    static void Backspace();
    static void ApplyButton(int idx);
    static int FindButtonForKey(char key);
};
