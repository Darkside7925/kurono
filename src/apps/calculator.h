#pragma once
#include "../kernel/types.h"

class Calculator {
public:
    static void Init(int x, int y);
    static void Draw();
    static void Update();
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
};
