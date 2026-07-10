#include "calculator.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../ui/font.h"
#include "../drivers/serial.h"
#include "../system/logging.h"

int Calculator::window_x = 100;
int Calculator::window_y = 100;
int Calculator::window_w = 200;
int Calculator::window_h = 250;
bool Calculator::active = true;
char Calculator::display_buffer[32] = "0";
int Calculator::current_val = 0;
int Calculator::active_op = 0;
int Calculator::stored_val = 0;
bool Calculator::new_entry = true;
int Calculator::selected_button = 0;
int Calculator::hover_button = -1;
unsigned int Calculator::press_ms = 0;
int Calculator::press_button = -1;
int Calculator::error_state = 0;

static const char* calc_labels[16] = {
    "7","8","9","/",
    "4","5","6","*",
    "1","2","3","-",
    "C","0","=","+"
};

static const uint32_t calc_btn_colors[16] = {
    0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
    0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
    0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
    0xFFE74C3C,0xFF3A3A4C,0xFF2ECC71,0xFFE67E22
};

static bool calc_execute(int& stored, int& current, int op);

static bool calc_get_button_rect(int x0, int y0, int w, int h,
                                 int idx, int& bx, int& by, int& btn_w, int& btn_h) {
    if (idx < 0 || idx >= 16) return false;

    int disp_h = 36;
    int pad = 6;
    int grid_y = y0 + disp_h + 10;
    int avail_w = w - pad * 2;
    int avail_h = h - (disp_h + 10) - pad;
    int gap = 4;
    btn_w = (avail_w - gap * 3) / 4;
    btn_h = (avail_h - gap * 3) / 4;

    int row = idx / 4;
    int col = idx % 4;
    bx = x0 + pad + col * (btn_w + gap);
    by = grid_y + row * (btn_h + gap);
    return true;
}

static int calc_hit_test_button(int x0, int y0, int w, int h, int px, int py) {
    for (int idx = 0; idx < 16; idx++) {
        int bx, by, btn_w, btn_h;
        if (!calc_get_button_rect(x0, y0, w, h, idx, bx, by, btn_w, btn_h)) continue;
        if (px >= bx && px < bx + btn_w && py >= by && py < by + btn_h) {
            return idx;
        }
    }
    return -1;
}

static void int_to_str(int v, char* b, int mx) {
    if(mx<2){b[0]=0;return;}
    bool neg = false;
    if(v<0){neg=true;v=-v;}
    char t[16]; int n=0;
    do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;
    if(neg && i<mx-1) b[i++]='-';
    while(n>0&&i<mx-1)b[i++]=t[--n];
    b[i]=0;
}

void Calculator::Init(int x, int y) {
    window_x = x;
    window_y = y;
    active = true;
    current_val = 0;
    stored_val = 0;
    active_op = 0;
    new_entry = true;
    selected_button = 0;
    hover_button = -1;
    press_button = -1;
    press_ms = 0;
    error_state = 0;
    display_buffer[0] = '0'; display_buffer[1] = 0;
    RuntimeLog::LogAppEvent("calculator", "open");
}

void Calculator::SetPosition(int x, int y) { window_x = x; window_y = y; }
void Calculator::SetSize(int w, int h) { window_w = w; window_h = h; }

void Calculator::ClearAll() {
    current_val = 0;
    stored_val = 0;
    active_op = 0;
    new_entry = true;
    error_state = 0;
    display_buffer[0] = '0';
    display_buffer[1] = 0;
}

void Calculator::Backspace() {
    if (new_entry) {
        return;
    }
    int neg = (current_val < 0) ? -1 : 1;
    int abs_val = current_val * neg;
    if (abs_val < 10) {
        current_val = 0;
        new_entry = true;
    } else {
        current_val = (abs_val / 10) * neg;
    }
    int_to_str(current_val, display_buffer, 32);
}

void Calculator::ApplyButton(int idx) {
    if (idx < 0 || idx >= 16) return;

    const char* lbl = calc_labels[idx];
    if (error_state) {
        ClearAll();
    }
    if (lbl[0] >= '0' && lbl[0] <= '9') {
        int digit = lbl[0] - '0';
        if (new_entry) {
            current_val = digit;
            new_entry = false;
        } else {
            // guard against int32 overflow: cap magnitude to 9 digits
            int abs_v = current_val < 0 ? -current_val : current_val;
            if (abs_v <= 99999999) {
                current_val = current_val * 10 + (current_val < 0 ? -digit : digit);
            }
        }
        int_to_str(current_val, display_buffer, 32);
        return;
    }

    if (lbl[0] == 'C') {
        ClearAll();
        return;
    }

    if (lbl[0] == '=') {
        if (!calc_execute(stored_val, current_val, active_op)) {
            error_state = 1;
            display_buffer[0] = 'E';
            display_buffer[1] = 'r';
            display_buffer[2] = 'r';
            display_buffer[3] = 0;
            stored_val = 0; current_val = 0; active_op = 0;
            new_entry = true;
            return;
        }
        current_val = stored_val;
        active_op = 0;
        new_entry = true;
        int_to_str(stored_val, display_buffer, 32);
        return;
    }

    if (!calc_execute(stored_val, current_val, active_op)) {
        error_state = 1;
        display_buffer[0] = 'E';
        display_buffer[1] = 'r';
        display_buffer[2] = 'r';
        display_buffer[3] = 0;
        stored_val = 0; current_val = 0; active_op = 0;
        new_entry = true;
        return;
    }
    if (lbl[0] == '+') active_op = 1;
    else if (lbl[0] == '-') active_op = 2;
    else if (lbl[0] == '*') active_op = 3;
    else if (lbl[0] == '/') active_op = 4;
    new_entry = true;
    int_to_str(stored_val, display_buffer, 32);
}

int Calculator::FindButtonForKey(char key) {
    switch (key) {
        case '0': return 13;
        case '1': return 8;
        case '2': return 9;
        case '3': return 10;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 0;
        case '8': return 1;
        case '9': return 2;
        case '/': return 3;
        case '*': return 7;
        case '-': return 11;
        case '+': return 15;
        case '=': return 14;
        case 'c':
        case 'C': return 12;
        default: return -1;
    }
}

static bool calc_execute(int& stored, int& current, int op) {
    switch(op) {
        case 1: stored = stored + current; return true;
        case 2: stored = stored - current; return true;
        case 3: stored = stored * current; return true;
        case 4:
            if(current==0) return false;
            stored = stored / current; return true;
        default: stored = current; return true;
    }
}

void Calculator::Draw() {
    if (!active) return;

    int x0 = window_x, y0 = window_y;
    int w = window_w, h = window_h;
    unsigned int now = Timer::GetRealMs();

    // background
    Graphics::FillRect(x0, y0, w, h, 0xFF1E1E2E);

    // display area - error flashes red briefly
    int disp_h = 36;
    unsigned int disp_bg = 0xFF2A2A3C;
    if (error_state && (now - press_ms) < 600) disp_bg = 0xFF552430;
    Graphics::FillRoundedRect(x0+6, y0+4, w-12, disp_h, 4, disp_bg);
    FontTTF::DrawString(x0+12, y0+10, 20.0f, display_buffer,
                        error_state ? 0xFFFF6B6B : 0xFFFFFFFF);

    for (int idx = 0; idx < 16; idx++) {
        int bx, by, btn_w, btn_h;
        if (!calc_get_button_rect(x0, y0, w, h, idx, bx, by, btn_w, btn_h)) continue;
        unsigned int col = calc_btn_colors[idx];
        // press flash (200ms ramp)
        if (idx == press_button) {
            unsigned int dt = now - press_ms;
            if (dt < 200) {
                unsigned int boost = (200 - dt) * 40 / 200; // 0..40
                unsigned int r = (col>>16)&0xFF, g=(col>>8)&0xFF, b=col&0xFF;
                r = (r+boost>255)?255:r+boost;
                g = (g+boost>255)?255:g+boost;
                b = (b+boost>255)?255:b+boost;
                col = 0xFF000000|(r<<16)|(g<<8)|b;
            }
        } else if (idx == hover_button) {
            // subtle lift on hover
            unsigned int r = (col>>16)&0xFF, g=(col>>8)&0xFF, b=col&0xFF;
            r = (r+18>255)?255:r+18;
            g = (g+18>255)?255:g+18;
            b = (b+18>255)?255:b+18;
            col = 0xFF000000|(r<<16)|(g<<8)|b;
        }
        Graphics::FillRoundedRect(bx, by, btn_w, btn_h, 4, col);
        if (idx == selected_button) {
            Graphics::DrawRect(bx, by, btn_w, btn_h, 0xFFFFFFFF);
        }
        FontTTF::DrawStringCenter(bx + btn_w / 2, by + btn_h / 2 - 6, 16.0f,
                                  calc_labels[idx], 0xFFFFFFFF);
    }
}

void Calculator::Update() {
    if (!active) return;

    // hover tracking - refresh every Update so visuals react same tick
    int mx, my;
    Mouse::GetPosition(mx, my);
    hover_button = calc_hit_test_button(window_x, window_y, window_w, window_h, mx, my);

    if (Mouse::LeftClicked()) {
        Input(mx, my, true, 0);
    }
}

bool Calculator::Input(int mx, int my, bool clicked, char key) {
    if (!active) return false;

    if (clicked) {
        // mx,my arrive window-local from the wm input callback, but draw uses the
        // global window_x/window_y origin - hit-test in the same local space (0,0)
        // so clicks land on the right buttons (this was the "calculator broken" bug). (satoru)
        int idx = calc_hit_test_button(0, 0, window_w, window_h, mx, my);
        if (idx >= 0) {
            selected_button = idx;
            press_button = idx;
            press_ms = Timer::GetRealMs();
            ApplyButton(idx);
            return true;
        }
    }

    if (key == 0) return false;

    if (key == (char)0x48) {
        if (selected_button >= 4) selected_button -= 4;
        return true;
    }
    if (key == (char)0x50) {
        if (selected_button < 12) selected_button += 4;
        return true;
    }
    if (key == (char)0x4B) {
        if ((selected_button % 4) > 0) selected_button--;
        return true;
    }
    if (key == (char)0x4D) {
        if ((selected_button % 4) < 3) selected_button++;
        return true;
    }

    if (key == '\n' || key == '\r' || key == ' ') {
        ApplyButton(selected_button);
        return true;
    }

    if (key == 8 || key == 127) {
        Backspace();
        return true;
    }

    int idx = FindButtonForKey(key);
    if (idx >= 0) {
        selected_button = idx;
        press_button = idx;
        press_ms = Timer::GetRealMs();
        ApplyButton(idx);
        return true;
    }

    return false;
}

bool Calculator::IsActive() { return active; }
void Calculator::SetActive(bool a) { active = a; }
