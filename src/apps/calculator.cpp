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
    display_buffer[0] = '0'; display_buffer[1] = 0;
    RuntimeLog::LogAppEvent("calculator", "open");
}

void Calculator::SetPosition(int x, int y) { window_x = x; window_y = y; }
void Calculator::SetSize(int w, int h) { window_w = w; window_h = h; }

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

    // button grid
    static const char* labels[16] = {
        "7","8","9","/",
        "4","5","6","*",
        "1","2","3","-",
        "C","0","=","+"
    };
    static const uint32_t btn_colors[16] = {
        0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
        0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
        0xFF3A3A4C,0xFF3A3A4C,0xFF3A3A4C,0xFFE67E22,
        0xFFE74C3C,0xFF3A3A4C,0xFF2ECC71,0xFFE67E22
    };

    int pad = 6;
    int grid_y = y0 + disp_h + 10;
    int avail_w = w - pad*2;
    int avail_h = h - (disp_h + 10) - pad;
    int gap = 4;
    int btn_w = (avail_w - gap*3) / 4;
    int btn_h = (avail_h - gap*3) / 4;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = x0 + pad + c*(btn_w+gap);
            int by = grid_y + r*(btn_h+gap);
            int idx = r*4+c;
            Graphics::FillRoundedRect(bx, by, btn_w, btn_h, 4, btn_colors[idx]);
            // center label
            FontTTF::DrawStringCenter(bx+btn_w/2, by+btn_h/2-6, 16.0f, labels[idx], 0xFFFFFFFF);
        }
    }
}

void Calculator::Update() {
    if (!active) return;

    if (Mouse::LeftClicked()) {
        int mx, my;
        Mouse::GetPosition(mx, my);

        // button grid hit test
        int x0 = window_x, y0 = window_y;
        int w = window_w, h = window_h;
        int disp_h = 36;
        int pad = 6;
        int grid_y = y0 + disp_h + 10;
        int avail_w = w - pad*2;
        int avail_h = h - (disp_h + 10) - pad;
        int gap = 4;
        int btn_w = (avail_w - gap*3) / 4;
        int btn_h = (avail_h - gap*3) / 4;

        static const char* labels[16] = {
            "7","8","9","/","4","5","6","*","1","2","3","-","C","0","=","+"
        };

        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                int bx = x0 + pad + c*(btn_w+gap);
                int by = grid_y + r*(btn_h+gap);
                if (mx >= bx && mx < bx+btn_w && my >= by && my < by+btn_h) {
                    int idx = r*4+c;
                    const char* lbl = labels[idx];
                    if (lbl[0]>='0' && lbl[0]<='9') {
                        int digit = lbl[0]-'0';
                        if (new_entry) { current_val = digit; new_entry = false; }
                        else { current_val = current_val*10 + digit; }
                        int_to_str(current_val, display_buffer, 32);
                    } else if (lbl[0]=='+') {
                        calc_execute(stored_val, current_val, active_op);
                        active_op = 1; new_entry = true;
                        int_to_str(stored_val, display_buffer, 32);
                    } else if (lbl[0]=='-') {
                        calc_execute(stored_val, current_val, active_op);
                        active_op = 2; new_entry = true;
                        int_to_str(stored_val, display_buffer, 32);
                    } else if (lbl[0]=='*') {
                        calc_execute(stored_val, current_val, active_op);
                        active_op = 3; new_entry = true;
                        int_to_str(stored_val, display_buffer, 32);
                    } else if (lbl[0]=='/') {
                        calc_execute(stored_val, current_val, active_op);
                        active_op = 4; new_entry = true;
                        int_to_str(stored_val, display_buffer, 32);
                    } else if (lbl[0]=='=') {
                        calc_execute(stored_val, current_val, active_op);
                        current_val = stored_val;
                        active_op = 0; new_entry = true;
                        int_to_str(stored_val, display_buffer, 32);
                    } else if (lbl[0]=='C') {
                        current_val = 0; stored_val = 0; active_op = 0; new_entry = true;
                        display_buffer[0]='0'; display_buffer[1]=0;
                    }
                    return;
                }
            }
        }
    }
}

bool Calculator::IsActive() { return active; }
void Calculator::SetActive(bool a) { active = a; }
