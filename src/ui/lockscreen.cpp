 #include "lockscreen.h"
#include "../drivers/graphics.h"
#include "../drivers/rtc.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../media/mediadecoder.h"
#include "font.h"
#include "../kernel/time.h"
#include "gui.h"
#include "ui_elements.h"
#include "../system/user_mgmt.h"
#include "file_browser.h"
#include "../drivers/serial.h"
#include "../system/input_manager.h"
#include "../fs/kvfs.h"

bool Widget::high_contrast_mode = false;
LockScreen::State LockScreen::current_state = LockScreen::Idle;
float LockScreen::transition_t = 0.0f;
static uint32_t s_frame_ms_accum = 0;  // time-based frame pacing accumulator

// ui elements for login/setup
static Button* btn_login = nullptr;
static Button* btn_setup = nullptr;
static Button* btn_browse = nullptr;
static InputField* input_user = nullptr;
static InputField* input_pass = nullptr;
static Button* btn_create = nullptr;
static Button* btn_hc = nullptr;
static bool users_initialized = false;
static bool login_success = false;

static void AttemptLogin() {
    if (input_user && input_pass) {
        login_success = UserManager::Login(input_user->buffer, input_pass->buffer);
    }
}

static void ProcessAuthTextInput() {
    while (Keyboard::HasChar()) {
        char c = Keyboard::GetChar();
        if (c == '\t') {
            if (input_user && input_pass) {
                bool next_user_focus = !input_user->focused;
                input_user->focused = next_user_focus;
                input_pass->focused = !next_user_focus;
            }
        } else {
            if (input_user && input_user->focused) input_user->OnKey(c);
            else if (input_pass && input_pass->focused) input_pass->OnKey(c);
        }
    }
}

void LockScreen::Show() {
    SerialLogger::Log("LockScreen::Show() called\r\n");
    Keyboard::FlushBuffers(); // clear any stale input
    if (!users_initialized) {
        UserManager::Init();
        users_initialized = true;
    }
    InputManager::Init(); // init new input system
    current_state = (UserManager::GetUserCount() == 0) ? Setup : Idle;
    transition_t = 0.0f;
    login_success = false;
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    
    // ui scaling factors (base 1080p)
    float scale = (float)h / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    
    SerialLogger::Log("LockScreen: Scale="); 
    // manual float log
    int scale_int = (int)scale;
    int scale_frac = (int)((scale - scale_int) * 100);
    SerialLogger::LogDec(scale_int); SerialLogger::Log("."); SerialLogger::LogDec(scale_frac);
    SerialLogger::Log("\r\n");
    
    // init ui with scaled sizes
    int cx = w/2; int cy = h/2;
    int btn_w = (int)(120 * scale);
    int btn_h = (int)(40 * scale);
    int input_w = (int)(300 * scale);
    int input_h = (int)(40 * scale);
    int font_sz = (int)(24 * scale);
    
    bool running = true;

    if (!btn_login) {
        btn_login = new Button(cx - btn_w/2, cy + (int)(60*scale), btn_w, btn_h, "Login", [](){
            AttemptLogin();
        });
        btn_login->font_size = (float)font_sz;
    }
    if (!btn_setup) {
        btn_setup = new Button(cx - btn_w/2, cy + (int)(100*scale), btn_w, btn_h, "Setup User", [](){
            LockScreen::current_state = LockScreen::Setup;
            if (input_user) input_user->focused = true; // auto-focus username
            if (input_pass) input_pass->focused = false;
        });
        btn_setup->font_size = (float)font_sz;
    }
    
    if (!input_user) {
        input_user = new InputField(cx - input_w/2, cy - (int)(30*scale), input_w, input_h, "Username");
        input_user->font_size = (float)font_sz;
    }
    if (!input_pass) { 
        input_pass = new InputField(cx - input_w/2, cy + (int)(20*scale), input_w, input_h, "Password"); 
        input_pass->is_password = true; 
        input_pass->font_size = (float)font_sz;
    }
    
    if (!btn_create) {
        btn_create = new Button(cx - btn_w/2, cy + (int)(120*scale), btn_w, btn_h, "Create", [](){
            if (UserManager::AddUser(input_user->buffer, input_pass->buffer)) {
                char home_path[96];
                int pos = 0;
                const char* prefix = "/home/";
                while (prefix[pos] && pos < 95) {
                    home_path[pos] = prefix[pos];
                    pos++;
                }
                for (int i = 0; input_user->buffer[i] && pos < 95; i++) {
                    home_path[pos++] = input_user->buffer[i];
                }
                home_path[pos] = 0;
                KVFS::Mkdirs(home_path);
                LockScreen::current_state = LockScreen::Login;
                login_success = true;
            }
        });
        btn_create->font_size = (float)font_sz;
    }
    
    if (!btn_hc) btn_hc = new Button(w - (int)(50*scale), (int)(10*scale), (int)(40*scale), (int)(40*scale), "HC", [](){
        Widget::high_contrast_mode = !Widget::high_contrast_mode;
    });
    
    if (!btn_browse) {
        btn_browse = new Button(cx - btn_w/2, cy + (int)(70*scale), btn_w, btn_h, "Profile Pic", [](){
            FileBrowser::Show();
        });
        btn_browse->font_size = (float)font_sz;
    }

    // ensure wallpaper is initially clean
    if (GUI::wallpaper.valid) GUI::SetWallpaper(GUI::wallpaper);

    s_frame_ms_accum = 0;
    Timer::ElapsedSinceLast(); // reset timer baseline

    while (running) {
        // poll hardware (keyboard + mouse io  -  necessary)
        InputManager::Poll();

        // capture edge-detected input state for this frame
        bool clicked        = Mouse::LeftClicked();
        bool space_pressed  = Keyboard::IsKeyPressed(KEY_SPACE);
        bool enter_pressed  = Keyboard::IsKeyPressed(KEY_ENTER);
        bool has_chars       = Keyboard::HasChar(); // peek before consuming

        // advance wall-clock from pit (single read per frame)
        uint32_t dt = Timer::ElapsedSinceLast();
        if (dt > 0) {
            TimeManager::AdvanceByMs(dt);
            s_frame_ms_accum += dt;
        }

        // idle → transition on any interaction
        if (current_state == Idle && (clicked || space_pressed || enter_pressed || has_chars)) {
            current_state = Transition;
            transition_t = 0.0f;
            GUI::BlurWallpaper();
        }

        // feed chars to focused inputfield
        if (current_state == Login || current_state == Setup) {
            ProcessAuthTextInput();
        }

        // render frame (time-gated inside update to ~60 fps,
        // but immediate on input/click so typing feels instant)
        frame_clicked   = clicked;
        frame_has_input = has_chars;
        Update();

        // login attempt check
        if (current_state == Login) {
            bool attempt = (clicked && btn_login &&
                            btn_login->Contains(Mouse::mx, Mouse::my)) ||
                           enter_pressed;
            if (attempt) btn_login->OnClick(Mouse::mx, Mouse::my);
            if (login_success) running = false;
        }

        // yield cpu without io-heavy busy-wait
        __asm__ __volatile__("pause");
    }
}

bool LockScreen::frame_clicked = false;
bool LockScreen::frame_has_input = false;

void LockScreen::Update() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();

    static bool first_frame = true;
    bool should_draw = false;

    if (first_frame)                      { should_draw = true; first_frame = false; }
    if (current_state == Transition)       should_draw = true;
    if (frame_has_input || frame_clicked)  should_draw = true;   // instant on input
    if (s_frame_ms_accum >= 16)            should_draw = true;   // ~60 fps fallback

    if (should_draw) {
        s_frame_ms_accum = 0;

        GUI::UpdateBackbuffer();
        uint8_t* screen = Graphics::GetBuffer();
        if (GUI::backbuffer) Graphics::SetBuffer(GUI::backbuffer);

        if (Widget::high_contrast_mode) {
            Graphics::FillRect(0, 0, w, h, 0xFF000000);
        }

        if (current_state == Idle) {
            DrawIdle(w, h);
        } else if (current_state == Transition) {
            transition_t += 0.05f;
            if (transition_t >= 1.0f) {
                transition_t = 1.0f;
                current_state = (UserManager::GetUserCount() > 0) ? Login : Setup;
                if (input_user) input_user->focused = true;
                if (input_pass) input_pass->focused = false;
            }
            Graphics::FillRectAlpha(0, 0, w, h,
                                    (uint8_t)(transition_t * 100), 0x00000000);
        } else if (current_state == Login) {
            DrawLogin(w, h);
        } else if (current_state == Setup) {
            DrawSetup(w, h);
        }

        if (FileBrowser::visible) {
            FileBrowser::Draw();
            if (frame_clicked) FileBrowser::OnClick(Mouse::mx, Mouse::my);
        }

        if (GUI::backbuffer) Graphics::SetBuffer(screen);
        GUI::DrawDesktop();
    }

    // ── mouse cursor  -  only touch framebuffer when something changed ─
    static int old_mx = -1, old_my = -1;
    bool mouse_moved = (Mouse::mx != old_mx || Mouse::my != old_my);

    if (should_draw || mouse_moved) {
        if (old_mx != -1)
            GUI::RestoreRegion(old_mx, old_my, 24, 32);

        Mouse::DrawAt(Mouse::mx, Mouse::my);
        Graphics::SwapBuffers();

        old_mx = Mouse::mx;
        old_my = Mouse::my;
    }
}

void LockScreen::DrawIdle(int w, int h) {
    int ty = h / 2 - h / 14;
    int cx = w / 2;
    auto dt_loc = TimeManager::NowLocalDateTime();
    RTC::Time t; t.h = dt_loc.h; t.m = dt_loc.m; t.s = dt_loc.s;
    uint32_t now_us_low = (uint32_t)TimeManager::NowUTC().us;
    bool colon = ((now_us_low / 500000u) % 2) == 0;

    int digit_size = w / 90;
    if (digit_size < 10) digit_size = 10;
    if (digit_size > 40) digit_size = 40;

    int time_width = 17 * digit_size;
    int padding = digit_size;
    int panel_height = digit_size * 6;
    int panel_y = ty - panel_height / 2 - digit_size;
    Graphics::FillRect(cx - time_width / 2 - padding, panel_y, time_width + padding * 2, panel_height + padding, 0xCCFFFFFF);
    DrawTime((w - time_width) / 2, ty - digit_size / 2, digit_size, t.h, t.m, 0xFF000000, colon);

    FontTTF::DrawStringCenter(cx, ty + panel_height / 2 + digit_size, 20.0f, "Tap Space to Login", 0xFF000000);

    char time_ref[32];
    int ref_pos = 0;
    auto append_str = [&](const char* s) {
        while (*s && ref_pos < (int)sizeof(time_ref) - 1) time_ref[ref_pos++] = *s++;
    };
    auto append_two = [&](int value) {
        if (ref_pos < (int)sizeof(time_ref) - 1) time_ref[ref_pos++] = (value < 10) ? '0' : (char)('0' + (value / 10));
        if (ref_pos < (int)sizeof(time_ref) - 1) time_ref[ref_pos++] = (char)('0' + (value % 10));
    };
    append_str("Current Time: ");
    append_two(t.h);
    if (ref_pos < (int)sizeof(time_ref) - 1) time_ref[ref_pos++] = ':';
    append_two(t.m);
    time_ref[ref_pos] = 0;
    FontTTF::DrawStringCenter(cx, ty + panel_height / 2 + digit_size + 26, 14.0f, time_ref, 0xFF000000);
    
    // draw status bar
    int status_y = h - 30;
    Graphics::FillRect(0, status_y, w, 30, 0xFF222222);
    
    // draw input device info
    char dev_buf[64];
    int dev_count = InputManager::GetDeviceCount();
    const char* layout = InputManager::GetLayoutName();
    
    // manual snprintf replacement
    auto append = [](char* b, const char* s) { while(*s) *b++ = *s++; return b; };
    char* p = dev_buf;
    p = append(p, "Devices: ");
    if (dev_count >= 10) { *p++='1'; *p++='0'+(dev_count%10); } else { *p++='0'+dev_count; }
    p = append(p, " | Layout: ");
    p = append(p, layout);
    *p = 0;
    
    FontTTF::DrawString(10, status_y + 8, 14.0f, dev_buf, 0xFFAAAAAA);
    
    // visualize key
    if (InputManager::last_key_pressed) {
        Graphics::FillRect(w - 100, status_y + 5, 20, 20, 0xFF00FF00);
        FontTTF::DrawString(w - 70, status_y + 8, 14.0f, "KEY", 0xFFFFFFFF);
    } else {
        Graphics::FillRect(w - 100, status_y + 5, 20, 20, 0xFF555555);
    }
    
    btn_hc->Draw();
    if (frame_clicked) {
        if (btn_hc->Contains(Mouse::mx, Mouse::my)) btn_hc->OnClick(Mouse::mx, Mouse::my);
    }
}

void LockScreen::DrawLogin(int w, int h) {
    int cx = w/2; int cy = h/2;
    
    // frosted panel behind login
    Graphics::FillRoundedRect(cx - 160, cy - 140, 320, 260, 16, 0xFF141428);
    
    // avatar circle
    Graphics::FillCircle(cx, cy - 100, 32, 0xFF3D3D5C);
    Graphics::FillCircle(cx, cy - 100, 28, 0xFF5C5C8A);
    // user icon silhouette
    Graphics::FillCircle(cx, cy - 108, 10, 0xFFCCCCDD);
    Graphics::FillRoundedRect(cx - 14, cy - 92, 28, 18, 8, 0xFFCCCCDD);
    
    // username/password inputs
    input_user->Draw();
    input_pass->Draw();
    
    // login button
    btn_login->Draw();
    btn_hc->Draw();
    
    if (frame_clicked) {
        int mx, my; Mouse::GetPosition(mx, my);
        input_user->focused = input_user->Contains(mx, my);
        input_pass->focused = input_pass->Contains(mx, my);
        // button click handled in main loop or here?
        // btn_login->onclick(mx, my); // logic in show() loop for break
    }
}

void LockScreen::DrawSetup(int w, int h) {
    int cx = w/2; int cy = h/2;
    
    FontTTF::DrawStringCenter(cx, cy - 150, 32.0f, "Welcome", 0xFFFFFFFF);
    FontTTF::DrawStringCenter(cx, cy - 120, 20.0f, "Create your first user", 0xFFAAAAAA);
    
    // frosted panel
    Graphics::FillRoundedRect(cx - 180, cy - 90, 360, 280, 16, 0xFF141428);
    
    input_user->Draw();
    input_pass->Draw();
    btn_browse->Draw();
    btn_create->Draw();
    
    if (FileBrowser::selected_file[0]) {
        FontTTF::DrawString(cx + 70, cy + 68, 14.0f, FileBrowser::selected_file, 0xFF00FF00);
    }
    
    if (frame_clicked) {
        int mx, my; Mouse::GetPosition(mx, my);
        bool user_f = input_user->Contains(mx, my);
        bool pass_f = input_pass->Contains(mx, my);
        
        // only change focus if clicked inside one of them
        // this prevents losing focus when clicking buttons or background (optional ux choice)
        // for now, let's allow background click to clear focus to be standard.
        input_user->focused = user_f;
        input_pass->focused = pass_f;
        
        SerialLogger::Log("Setup Click: "); SerialLogger::LogDec(mx); SerialLogger::Log(","); SerialLogger::LogDec(my);
        if (user_f) SerialLogger::Log(" -> User Focus");
        if (pass_f) SerialLogger::Log(" -> Pass Focus");
        SerialLogger::Log("\r\n");

        btn_browse->OnClick(mx, my);
        btn_create->OnClick(mx, my);
        if (btn_hc->Contains(mx, my)) btn_hc->OnClick(mx, my);
    }
}

// helpers from previous implementation
const char* LockScreen::DowName(uint8_t v) {
    switch (v) { case 1: return "Monday"; case 2: return "Tuesday"; case 3: return "Wednesday"; case 4: return "Thursday"; case 5: return "Friday"; case 6: return "Saturday"; case 7: return "Sunday"; default: return ""; }
}
const char* LockScreen::MonName(uint8_t v) {
    switch (v) { case 1: return "January"; case 2: return "February"; case 3: return "March"; case 4: return "April"; case 5: return "May"; case 6: return "June"; case 7: return "July"; case 8: return "August"; case 9: return "September"; case 10: return "October"; case 11: return "November"; case 12: return "December"; default: return ""; }
}
void LockScreen::FormatDate(char* out, size_t cap, const char* dow, const char* mon, uint8_t dom) {
    char* p = out; size_t r = cap;
    auto putc = [&](char c){ if (r>1){ *p++=c; r--; } };
    auto puts = [&](const char* s){ while (*s) { putc(*s++); } };
    auto putnum = [&](uint8_t n){ char buf[4]; int i=0; if (n>=100){ buf[i++] = (char)('0'+(n/100)%10); } if (n>=10 || i){ buf[i++] = (char)('0'+(n/10)%10); } buf[i++] = (char)('0'+(n%10)); for (int j=0;j<i;j++) putc(buf[j]); };
    puts(dow); putc(','); putc(' ');
    puts(mon); putc(' '); putnum(dom);
    *p = 0;
}
void LockScreen::DrawStringCenter(int cx, int y, int s, const char* str, uint32_t color) {
    int w = 0; const char* p = str; while (*p) { w += (s*3 + s); p++; }
    int x = cx - w/2;
    DrawString(x, y, s, str, color);
}
void LockScreen::DrawString(int x, int y, int s, const char* str, uint32_t color) {
    while (*str) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ') { x += s * 3 + s; continue; }
        const uint8_t* rows = GlyphLS(c);
        if (!rows) { x += s * 3 + s; continue; }
        for (int ry = 0; ry < 5; ry++) {
            uint8_t row = rows[ry];
            for (int rx = 0; rx < 3; rx++) {
                if (row & (1 << (2 - rx))) Graphics::FillRect(x + rx * s, y + ry * s, s, s, color);
            }
        }
        x += s * 3 + s;
    }
}
const uint8_t* LockScreen::GlyphLS(char c) {
    switch (c) {
        case 'A': { static const uint8_t r[5]={0x02,0x05,0x07,0x05,0x05}; return r; }
        case 'B': { static const uint8_t r[5]={0x06,0x05,0x06,0x05,0x06}; return r; }
        case 'C': { static const uint8_t r[5]={0x03,0x04,0x04,0x04,0x03}; return r; }
        case 'D': { static const uint8_t r[5]={0x06,0x05,0x05,0x05,0x06}; return r; }
        case 'E': { static const uint8_t r[5]={0x07,0x04,0x06,0x04,0x07}; return r; }
        case 'F': { static const uint8_t r[5]={0x07,0x04,0x06,0x04,0x04}; return r; }
        case 'G': { static const uint8_t r[5]={0x03,0x04,0x05,0x05,0x03}; return r; }
        case 'H': { static const uint8_t r[5]={0x05,0x05,0x07,0x05,0x05}; return r; }
        case 'I': { static const uint8_t r[5]={0x07,0x02,0x02,0x02,0x07}; return r; }
        case 'J': { static const uint8_t r[5]={0x01,0x01,0x01,0x05,0x02}; return r; }
        case 'K': { static const uint8_t r[5]={0x05,0x05,0x06,0x05,0x05}; return r; }
        case 'L': { static const uint8_t r[5]={0x04,0x04,0x04,0x04,0x07}; return r; }
        case 'M': { static const uint8_t r[5]={0x05,0x07,0x05,0x05,0x05}; return r; }
        case 'N': { static const uint8_t r[5]={0x05,0x07,0x07,0x05,0x05}; return r; }
        case 'O': { static const uint8_t r[5]={0x02,0x05,0x05,0x05,0x02}; return r; }
        case 'P': { static const uint8_t r[5]={0x06,0x05,0x06,0x04,0x04}; return r; }
        case 'Q': { static const uint8_t r[5]={0x02,0x05,0x05,0x06,0x03}; return r; }
        case 'R': { static const uint8_t r[5]={0x06,0x05,0x06,0x05,0x05}; return r; }
        case 'S': { static const uint8_t r[5]={0x03,0x04,0x02,0x01,0x06}; return r; }
        case 'T': { static const uint8_t r[5]={0x07,0x02,0x02,0x02,0x02}; return r; }
        case 'U': { static const uint8_t r[5]={0x05,0x05,0x05,0x05,0x07}; return r; }
        case 'V': { static const uint8_t r[5]={0x05,0x05,0x05,0x05,0x02}; return r; }
        case 'W': { static const uint8_t r[5]={0x05,0x05,0x05,0x07,0x05}; return r; }
        case 'X': { static const uint8_t r[5]={0x05,0x05,0x02,0x05,0x05}; return r; }
        case 'Y': { static const uint8_t r[5]={0x05,0x05,0x02,0x02,0x02}; return r; }
        case 'Z': { static const uint8_t r[5]={0x07,0x01,0x02,0x04,0x07}; return r; }
        case '0': { static const uint8_t r[5]={0x02,0x05,0x05,0x05,0x02}; return r; }
        case '1': { static const uint8_t r[5]={0x02,0x06,0x02,0x02,0x07}; return r; }
        case '2': { static const uint8_t r[5]={0x07,0x01,0x07,0x04,0x07}; return r; }
        case '3': { static const uint8_t r[5]={0x07,0x01,0x07,0x01,0x07}; return r; }
        case '4': { static const uint8_t r[5]={0x05,0x05,0x07,0x01,0x01}; return r; }
        case '5': { static const uint8_t r[5]={0x07,0x04,0x07,0x01,0x07}; return r; }
        case '6': { static const uint8_t r[5]={0x07,0x04,0x07,0x05,0x07}; return r; }
        case '7': { static const uint8_t r[5]={0x07,0x01,0x01,0x01,0x01}; return r; }
        case '8': { static const uint8_t r[5]={0x07,0x05,0x07,0x05,0x07}; return r; }
        case '9': { static const uint8_t r[5]={0x07,0x05,0x07,0x01,0x07}; return r; }
        case '.': { static const uint8_t r[5]={0x00,0x00,0x00,0x00,0x02}; return r; }
        case ',': { static const uint8_t r[5]={0x00,0x00,0x00,0x02,0x04}; return r; }
        case '!': { static const uint8_t r[5]={0x02,0x02,0x02,0x00,0x02}; return r; }
        case ':': { static const uint8_t r[5]={0x00,0x02,0x00,0x02,0x00}; return r; }
        default: return 0;
    }
}

void LockScreen::DrawDigit(int x, int y, int s, int d, uint32_t color) {
    static const uint8_t map[10][5] = {
        {0x07,0x05,0x05,0x05,0x07}, {0x02,0x06,0x02,0x02,0x07}, {0x07,0x01,0x07,0x04,0x07}, {0x07,0x01,0x07,0x01,0x07}, {0x05,0x05,0x07,0x01,0x01},
        {0x07,0x04,0x07,0x01,0x07}, {0x07,0x04,0x07,0x05,0x07}, {0x07,0x01,0x01,0x01,0x01}, {0x07,0x05,0x07,0x05,0x07}, {0x07,0x05,0x07,0x01,0x07}
    };
    for (int ry = 0; ry < 5; ry++) {
        uint8_t row = map[d][ry];
        for (int rx = 0; rx < 3; rx++) {
            if (row & (1 << (2 - rx))) Graphics::FillRect(x + rx * s, y + ry * s, s, s, color);
        }
    }
}
void LockScreen::DrawColon(int x, int y, int s, uint32_t color, bool on) {
    uint32_t bg = 0x00000000;
    Graphics::FillRect(x, y + s, s, s, on ? color : bg);
    Graphics::FillRect(x, y + 3 * s, s, s, on ? color : bg);
}
void LockScreen::DrawTime(int tx, int ty, int s, uint8_t hh, uint8_t mm, uint32_t color, bool colon) {
    int dig_w = 3 * s; int gap = s; int cx = tx;
    DrawDigit(cx, ty, s, (hh / 10), color); cx += dig_w + gap;
    DrawDigit(cx, ty, s, (hh % 10), color); cx += dig_w + gap;
    DrawColon(cx, ty, s, color, colon); cx += s * 2;
    DrawDigit(cx, ty, s, (mm / 10), color); cx += dig_w + gap;
    DrawDigit(cx, ty, s, (mm % 10), color);
}
