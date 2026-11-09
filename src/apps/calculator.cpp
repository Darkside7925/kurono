// the calculator on libkurono-ui: this one file builds into the kernel (kui kernel backend) and as the (satoru)
// ring-3 program /system/apps/calculator (KUI_BACKEND_WAYLAND). the drawing and the logic are the (satoru)
// in-kernel calculator's, moved into a kui::AppHandler so each window owns its own state. (satoru)
#include "calculator.h"
#include "../kui/kui.h"

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

static bool calc_modern() { return kui::CurrentTheme().style == kui::Style::Modern; }

static bool calc_get_button_rect(int x0, int y0, int w, int h,
                                 int idx, int& bx, int& by, int& btn_w, int& btn_h) {
    if (idx < 0 || idx >= 16) return false;

    // modern: a tall display and round keys with room around them (satoru)
    const bool modern = calc_modern();
    int disp_h = modern ? 92 : 36;
    int pad = modern ? 14 : 6;
    int grid_y = y0 + disp_h + 10;
    int avail_w = w - pad * 2;
    int avail_h = h - (disp_h + 10) - pad;
    int gap = modern ? 10 : 4;
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

// the 8x16 / classic text helpers drew at a BASELINE y (FontTTF::DrawString); kui text takes the (satoru)
// top of the line box, so lift by the ascent to keep the classic look. (satoru)
static void calc_text_base(kui::Canvas& c, int x, int y, float px, const char* s, uint32_t col) {
    int top = kui::FontsReady() ? y - kui::Ascent(kui::Face::Sans, px) : y;
    c.Text(x, top, kui::Face::Sans, px, s, col);
}

static uint32_t calc_bg() {
    if (!calc_modern()) return 0;   // classic keeps the wm's default body colour (satoru)
    return kui::CurrentTheme().pal.dark ? 0xFF1C1C21 : 0xFFF5F5F7;
}

struct CalcApp : kui::AppHandler {
    char display_buffer[32];
    int current_val = 0;
    int active_op = 0;      // 0: none, 1: +, 2: -, 3: *, 4: /
    int stored_val = 0;
    bool new_entry = true;
    int selected_button = 0;
    int hover_button = -1;        // -1 when none
    uint32_t press_ms = 0;        // time of last button press (for flash anim)
    int press_button = -1;        // last pressed button index
    int error_state = 0;    // 0=ok, 1=div-by-zero flash

    CalcApp() { display_buffer[0] = '0'; display_buffer[1] = 0; }

    void OnCreate(kui::Window& w) override {
        (void)w;
        kui::Log("calculator", "open");
    }

    void ClearAll() {
        current_val = 0;
        stored_val = 0;
        active_op = 0;
        new_entry = true;
        error_state = 0;
        display_buffer[0] = '0';
        display_buffer[1] = 0;
    }

    void Backspace() {
        if (new_entry) return;
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

    void SetError() {
        error_state = 1;
        display_buffer[0] = 'E'; display_buffer[1] = 'r'; display_buffer[2] = 'r'; display_buffer[3] = 0;
        stored_val = 0; current_val = 0; active_op = 0;
        new_entry = true;
    }

    void ApplyButton(int idx) {
        if (idx < 0 || idx >= 16) return;
        const char* lbl = calc_labels[idx];
        if (error_state) ClearAll();
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
        if (lbl[0] == 'C') { ClearAll(); return; }
        if (lbl[0] == '=') {
            if (!calc_execute(stored_val, current_val, active_op)) { SetError(); return; }
            current_val = stored_val;
            active_op = 0;
            new_entry = true;
            int_to_str(stored_val, display_buffer, 32);
            return;
        }
        if (!calc_execute(stored_val, current_val, active_op)) { SetError(); return; }
        if (lbl[0] == '+') active_op = 1;
        else if (lbl[0] == '-') active_op = 2;
        else if (lbl[0] == '*') active_op = 3;
        else if (lbl[0] == '/') active_op = 4;
        new_entry = true;
        int_to_str(stored_val, display_buffer, 32);
    }

    static int FindButtonForKey(int key) {
        switch (key) {
            case '0': return 13; case '1': return 8;  case '2': return 9;  case '3': return 10;
            case '4': return 4;  case '5': return 5;  case '6': return 6;  case '7': return 0;
            case '8': return 1;  case '9': return 2;  case '/': return 3;  case '*': return 7;
            case '-': return 11; case '+': return 15; case '=': return 14;
            case 'c': case 'C': return 12;
            default: return -1;
        }
    }

    void Press(kui::Window& w, int idx) {
        selected_button = idx;
        press_button = idx;
        press_ms = kui::NowMs();
        ApplyButton(idx);
        w.RequestAnimationFrame();   // the press flash settles over 220 ms (satoru)
    }

    bool OnEvent(kui::Window& w, const kui::Event& e) override {
        kui::Size sz = w.ContentSize();
        switch (e.type) {
        case kui::EventType::PointerMove: {
            int h = calc_hit_test_button(0, 0, sz.w, sz.h, e.x, e.y);
            if (h == hover_button) return false;
            hover_button = h;
            return true;
        }
        case kui::EventType::PointerLeave:
            if (hover_button < 0) return false;
            hover_button = -1;
            return true;
        case kui::EventType::Click: {
            int idx = calc_hit_test_button(0, 0, sz.w, sz.h, e.x, e.y);
            if (idx < 0) return false;
            Press(w, idx);
            return true;
        }
        case kui::EventType::KeyDown: {
            int key = e.key;
            if (key == kui::K_UP)    { if (selected_button >= 4) selected_button -= 4; return true; }
            if (key == kui::K_DOWN)  { if (selected_button < 12) selected_button += 4; return true; }
            if (key == kui::K_LEFT)  { if ((selected_button % 4) > 0) selected_button--; return true; }
            if (key == kui::K_RIGHT) { if ((selected_button % 4) < 3) selected_button++; return true; }
            if (key == kui::K_ENTER || key == '\r' || key == ' ') { Press(w, selected_button); return true; }
            if (key == kui::K_BS || key == kui::K_DEL_ASCII || key == kui::K_DEL) { Backspace(); return true; }
            if (key == kui::K_ESC) { ClearAll(); return true; }
            int idx = FindButtonForKey(key);
            if (idx >= 0) { Press(w, idx); return true; }
            return false;
        }
        case kui::EventType::ThemeChanged:
            w.SetBackground(calc_bg());
            return true;
        default:
            return false;
        }
    }

    void OnFrame(kui::Window& win, kui::Canvas& c, uint32_t now) override {
        int x0 = 0, y0 = 0;
        int w = c.Width(), h = c.Height();

        if (calc_modern()) {
            // modern: big right-aligned display, round keys: graphite digits,
            // light grey clear, amber operators (the settings palette, light or
            // dark). (satoru)
            const kui::Palette& P = kui::CurrentTheme().pal;
            uint32_t bg = P.dark ? 0xFF1C1C21 : 0xFFF5F5F7;
            c.FillRect(x0, y0, w, h, bg);
            uint32_t dig = P.dark ? 0xFF34353D : 0xFFFFFFFF, dig_t = P.dark ? 0xFFF2F3F8 : 0xFF1C1C1E;
            uint32_t fn = P.dark ? 0xFFA5A6AE : 0xFFD9D9DE, fn_t = 0xFF1C1C1E;
            uint32_t op = 0xFFFF9F0A, op_t = 0xFFFFFFFF;
            uint32_t txt = error_state ? 0xFFFF6B61 : (P.dark ? 0xFFFFFFFF : 0xFF1C1C1E);
            float px = 46.0f;
            while (px > 18.0f && kui::MeasureWidth(kui::Face::Sans, px, display_buffer) > w - 36) px -= 2.0f;
            int tw = kui::MeasureWidth(kui::Face::Sans, px, display_buffer);
            c.Text(x0 + w - 18 - tw, y0 + 92 - (int)(px * 1.25f), kui::Face::Sans, px, display_buffer, txt);
            for (int idx = 0; idx < 16; idx++) {
                int bx, by, bw, bh;
                if (!calc_get_button_rect(x0, y0, w, h, idx, bx, by, bw, bh)) continue;
                bool is_op = (idx % 4) == 3 || idx == 14;
                bool is_fn = idx == 12;
                uint32_t col = is_op ? op : (is_fn ? fn : dig);
                uint32_t tc = is_op ? op_t : (is_fn ? fn_t : dig_t);
                int s = bw < bh ? bw : bh;
                int cx = bx + bw / 2, cy = by + bh / 2;
                int r = s / 2;
                // hover lifts, a press flashes and settles over 220 ms (satoru)
                int lift = 0;
                if (idx == press_button && now - press_ms < 220) lift = (int)(90 * (220 - (now - press_ms)) / 220);
                else if (idx == hover_button) lift = 22;
                if (lift) col = kui::Mix(col, 0xFFFFFFFFu, lift);
                if (!P.dark && !is_op && !is_fn)
                    c.SoftShadow((float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r), (float)r, 4.0f, 1.0f, 40);
                c.RoundRect(cx - r, cy - r, 2 * r, 2 * r, r, col, 255);
                if (idx == selected_button && press_button != idx)
                    c.RoundOutline(cx - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, r + 2, P.accent, 160);
                const char* lb = calc_labels[idx];
                // operators as drawn glyphs (crisper than the font's) (satoru)
                int g = s / 5;
                switch (lb[0]) {
                case '+': c.RoundRect(cx - g, cy - 1, 2 * g, 3, 1, tc, 255); c.RoundRect(cx - 1, cy - g, 3, 2 * g, 1, tc, 255); break;
                case '-': c.RoundRect(cx - g, cy - 1, 2 * g, 3, 1, tc, 255); break;
                case '=': c.RoundRect(cx - g, cy - 4, 2 * g, 3, 1, tc, 255); c.RoundRect(cx - g, cy + 2, 2 * g, 3, 1, tc, 255); break;
                case '/': c.RoundRect(cx - g, cy - 1, 2 * g, 3, 1, tc, 255);
                          c.Circle(cx, cy - g + 1, 2, tc, 255); c.Circle(cx, cy + g - 1, 2, tc, 255); break;
                case '*': for (int k = -g * 3 / 4; k <= g * 3 / 4; k++) {
                              c.RoundRect(cx + k - 1, cy + k - 1, 3, 3, 1, tc, 255);
                              c.RoundRect(cx + k - 1, cy - k - 1, 3, 3, 1, tc, 255);
                          } break;
                default: {
                    int lw = kui::MeasureWidth(kui::Face::Sans, 26.0f, lb);
                    c.Text(cx - lw / 2, cy - 16, kui::Face::Sans, 26.0f, lb, tc);
                } }
            }
            if (press_button >= 0 && now - press_ms < 240) win.RequestAnimationFrame();
            return;
        }

        // background
        c.FillRect(x0, y0, w, h, 0xFF1E1E2E);

        // display area - error flashes red briefly
        int disp_h = 36;
        unsigned int disp_bg = 0xFF2A2A3C;
        if (error_state && (now - press_ms) < 600) { disp_bg = 0xFF552430; win.RequestFrameAt(press_ms + 600); }
        c.FillRoundedRect(x0+6, y0+4, w-12, disp_h, 4, disp_bg);
        calc_text_base(c, x0+12, y0+10, 20.0f, display_buffer,
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
                    win.RequestAnimationFrame();
                }
            } else if (idx == hover_button) {
                // subtle lift on hover
                unsigned int r = (col>>16)&0xFF, g=(col>>8)&0xFF, b=col&0xFF;
                r = (r+18>255)?255:r+18;
                g = (g+18>255)?255:g+18;
                b = (b+18>255)?255:b+18;
                col = 0xFF000000|(r<<16)|(g<<8)|b;
            }
            c.FillRoundedRect(bx, by, btn_w, btn_h, 4, col);
            if (idx == selected_button) {
                c.DrawRect(bx, by, btn_w, btn_h, 0xFFFFFFFF);
            }
            int lw = kui::MeasureWidth(kui::Face::Sans, 16.0f, calc_labels[idx]);
            calc_text_base(c, bx + btn_w / 2 - lw / 2, by + btn_h / 2 - 6, 16.0f,
                           calc_labels[idx], 0xFFFFFFFF);
        }
    }
};

static kui::AppHandler* calc_make() { return new CalcApp; }

// modern keys want room: a taller window. sizes are the old wm window (300x470 / 200x250) minus the (satoru)
// title bar and border, so the keys land where they always did. (satoru)
static kui::WindowSpec calc_spec() {
    const bool cm = calc_modern();
    kui::WindowSpec s;
    s.title = "Calculator";
    s.app_id = "calculator";
    s.w = cm ? 298 : 198;
    s.h = cm ? 433 : 213;
    s.min_w = 160; s.min_h = 200;
    s.x = -1; s.y = -1;
    s.resizable = true;
    s.closable = true;
    s.bg = calc_bg();
    s.app_icon = -1;
    return s;
}

KUI_APP(calculator, calc_spec(), calc_make)

// end (satoru)
