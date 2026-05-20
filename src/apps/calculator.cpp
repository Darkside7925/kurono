#include "calculator.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
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

static void calc_execute(int& stored, int& current, int op);

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
    display_buffer[0] = '0';
    display_buffer[1] = 0;
}

void Calculator::Backspace() {
    if (new_entry) {
        return;
    }
    if (current_val < 10) {
        current_val = 0;
        new_entry = true;
    } else {
        current_val /= 10;
    }
    int_to_str(current_val, display_buffer, 32);
}

void Calculator::ApplyButton(int idx) {
    if (idx < 0 || idx >= 16) return;

    const char* lbl = calc_labels[idx];
    if (lbl[0] >= '0' && lbl[0] <= '9') {
        int digit = lbl[0] - '0';
        if (new_entry) {
            current_val = digit;
            new_entry = false;
        } else {
            current_val = current_val * 10 + digit;
        }
        int_to_str(current_val, display_buffer, 32);
        return;
    }

    if (lbl[0] == 'C') {
        ClearAll();
        return;
    }

    if (lbl[0] == '=') {
        calc_execute(stored_val, current_val, active_op);
        current_val = stored_val;
        active_op = 0;
        new_entry = true;
        int_to_str(stored_val, display_buffer, 32);
        return;
    }

    calc_execute(stored_val, current_val, active_op);
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

static void calc_execute(int& stored, int& current, int op) {
    switch(op) {
        case 1: stored = stored + current; break;
        case 2: stored = stored - current; break;
        case 3: stored = stored * current; break;
        case 4: if(current!=0) stored = stored / current; break;
        default: stored = current; break;
    }
}

void Calculator::Draw() {
    if (!active) return;

    int x0 = window_x, y0 = window_y;
    int w = window_w, h = window_h;

    // background
    Graphics::FillRect(x0, y0, w, h, 0xFF1E1E2E);

    // display area
    int disp_h = 36;
    Graphics::FillRoundedRect(x0+6, y0+4, w-12, disp_h, 4, 0xFF2A2A3C);
    // display text (right-aligned)
    FontTTF::DrawString(x0+12, y0+10, 20.0f, display_buffer, 0xFFFFFFFF);

    for (int idx = 0; idx < 16; idx++) {
        int bx, by, btn_w, btn_h;
        if (!calc_get_button_rect(x0, y0, w, h, idx, bx, by, btn_w, btn_h)) continue;
        Graphics::FillRoundedRect(bx, by, btn_w, btn_h, 4, calc_btn_colors[idx]);
        if (idx == selected_button) {
            Graphics::DrawRect(bx, by, btn_w, btn_h, 0xFFFFFFFF);
        }
        FontTTF::DrawStringCenter(bx + btn_w / 2, by + btn_h / 2 - 6, 16.0f,
                                  calc_labels[idx], 0xFFFFFFFF);
    }
}

void Calculator::Update() {
    if (!active) return;

    if (Mouse::LeftClicked()) {
        int mx, my;
        Mouse::GetPosition(mx, my);
        Input(mx, my, true, 0);
    }
}

bool Calculator::Input(int mx, int my, bool clicked, char key) {
    if (!active) return false;

    if (clicked) {
        int idx = calc_hit_test_button(window_x, window_y, window_w, window_h, mx, my);
        if (idx >= 0) {
            selected_button = idx;
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
        ApplyButton(idx);
        return true;
    }

    return false;
}

bool Calculator::IsActive() { return active; }
void Calculator::SetActive(bool a) { active = a; }
