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

bool Widget::high_contrast_mode = false;
LockScreen::State LockScreen::current_state = LockScreen::Idle;
float LockScreen::transition_t = 0.0f;

// UI Elements for Login/Setup
static Button* btn_login = nullptr;
static Button* btn_setup = nullptr;
static Button* btn_browse = nullptr;
static InputField* input_user = nullptr;
static InputField* input_pass = nullptr;
static Button* btn_create = nullptr;
static Button* btn_hc = nullptr;

void LockScreen::Show() {
    SerialLogger::Log("LockScreen::Show() called\r\n");
    UserManager::Init();
    InputManager::Init(); // Init new input system
    current_state = Idle;
    transition_t = 0.0f;
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    
    // UI Scaling Factors (Base 1080p)
    float scale = (float)h / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    
    SerialLogger::Log("LockScreen: Scale="); 
    // Manual float log
    int scale_int = (int)scale;
    int scale_frac = (int)((scale - scale_int) * 100);
    SerialLogger::LogDec(scale_int); SerialLogger::Log("."); SerialLogger::LogDec(scale_frac);
    SerialLogger::Log("\r\n");
    
    // Init UI with scaled sizes
    int cx = w/2; int cy = h/2;
    int btn_w = (int)(120 * scale);
    int btn_h = (int)(40 * scale);
    int input_w = (int)(300 * scale);
    int input_h = (int)(40 * scale);
    int font_sz = (int)(24 * scale);
    
    // Delete old if exists (simple leak fix for re-entry)
    // In real OS, use proper lifecycle
    
    if (!btn_login) {
        btn_login = new Button(cx - btn_w/2, cy + (int)(60*scale), btn_w, btn_h, "Login", [](){ 
            if (UserManager::Validate(input_user->buffer, input_pass->buffer)) {
                // Success
            }
        });
        btn_login->font_size = (float)font_sz;
    }
    if (!btn_setup) {
        btn_setup = new Button(cx - btn_w/2, cy + (int)(100*scale), btn_w, btn_h, "Setup User", [](){
            LockScreen::current_state = LockScreen::Setup;
            if (input_user) input_user->focused = true; // Auto-focus Username
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
                LockScreen::current_state = LockScreen::Login;
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

    // Ensure wallpaper is initially clean
    if (GUI::wallpaper.valid) GUI::SetWallpaper(GUI::wallpaper); 

    // We use a simpler loop that doesn't rely on WaitMs busy-waiting on IO.
    // IO access (inb/outb) is very slow in QEMU.
    // Instead we process input as fast as possible, but only redraw when needed or every N iterations.
    
    bool running = true;
    int poll_counter = 0;
    while (running) {
        // Poll Input frequently
        InputManager::Poll(); // Replaces direct Keyboard/Mouse poll
        
        // Cache mouse click state for this frame
    bool clicked = Mouse::LeftClicked();
    
    poll_counter++;
        
        // Redraw at ~60Hz equivalent
        // Without a high-res timer interrupt, we approximate by iteration count or RTC change.
        // Using iteration count is unreliable but better than busy-wait lag.
        // Assuming QEMU runs fast, 1000 iterations might be a few ms.
        // Let's try redrawing every 1000 polls? No, that depends on CPU speed.
        // Better: Use RTC seconds change or just a small delay loop that doesn't touch IO.
        
        // Simple software delay to avoid 100% CPU lockup if it's too fast, 
        // but we want responsiveness.
        // Actually, let's try to render every frame but skipping WaitMs.
        // The previous code had WaitMs(16) which forced a 16ms IO-heavy delay.
        
        // Let's try a hybrid: 
        // Process input. 
        // If mouse moved, redraw mouse immediately (or mark dirty).
        // Update Game/UI logic every ~16ms (simulated).
        
        // Frame pacing using real PIT timing
        Timer::WaitMs(8);  // ~120fps cap for lockscreen
        
        // Advance system time by real elapsed time (PIT-polled)
        uint32_t real_dt = Timer::ElapsedSinceLast();
        if (real_dt > 0) TimeManager::AdvanceByMs(real_dt);
        
        // Handle Global Input
    if (current_state == Idle) {
        if (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == ' ') {
                current_state = Transition;
                transition_t = 0.0f;
                GUI::BlurWallpaper();
            }
        }
        if (clicked) {
             current_state = Transition;
             transition_t = 0.0f;
             GUI::BlurWallpaper();
        }
    }
    
    // Update logic, passing click state
    if (current_state == Login && clicked) {
         if (btn_login->Contains(Mouse::mx, Mouse::my)) {
             btn_login->OnClick(Mouse::mx, Mouse::my);
             if (UserManager::Validate(input_user->buffer, input_pass->buffer)) {
                 running = false;
             }
         }
         // Pass click to other elements if needed, or rely on Update() to handle drawing/interaction
         // But Wait, DrawLogin checks Mouse::LeftClicked() too! 
         // We need to change Update() signature or set a static flag.
         // Let's use a static flag in LockScreen for the frame.
    }
    
    LockScreen::frame_clicked = clicked;
    Update();
    
    // Check for exit condition (e.g. successful login)
        if (current_state == Login) {
             // Polling login button logic handled inside Draw/Update
             // But we need a way to break the loop.
             // For now, let's say if Login button callback was fired successfully.
             // We can check user count > 0 and if validated.
             // Simplified: if user clicks "Login" and it validates, we break.
             // We need to move the callback logic out or use a static flag.
             if (btn_login->Contains(Mouse::mx, Mouse::my) && Mouse::LeftClicked()) {
                 if (UserManager::Validate(input_user->buffer, input_pass->buffer)) {
                     running = false;
                 }
             }
        }
    }
    
    // Clean up or reset for next lock
    // Don't delete UI elements if we want to reuse them, but for now it's fine.
}

bool LockScreen::frame_clicked = false;

void LockScreen::Update() {
    static int frame_count = 0;
    frame_count++;
    
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    
    bool needs_redraw = false;
    if (current_state == Transition) needs_redraw = true;
    if (current_state == Login && (frame_count % 30 == 0)) needs_redraw = true; // Blink cursor?
    if (current_state == Idle && (frame_count % 60 == 0)) needs_redraw = true; // Time update
    
    // For now, force redraw to ensure correctness until we have robust dirty rects for UI
    needs_redraw = true; 
    
    if (needs_redraw) {
        // Draw Background (Backbuffer has blurred or normal wallpaper)
        GUI::UpdateBackbuffer(); // Ensure backbuffer is up to date
        
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
            }
            // Fade out idle elements?
            int alpha = (int)(255 * (1.0f - transition_t));
            (void)alpha;
            Graphics::FillRectAlpha(0, 0, w, h, (uint8_t)(transition_t * 100), 0x00000000); // Fade to dark
        } else if (current_state == Login) {
            DrawLogin(w, h);
        } else if (current_state == Setup) {
            DrawSetup(w, h);
        }
        
        if (FileBrowser::visible) {
            FileBrowser::Draw();
            if (frame_clicked) {
                 FileBrowser::OnClick(Mouse::mx, Mouse::my);
                 // Consume click so underlying elements don't get it?
                 // Simple approach: if FileBrowser handled it, we might want to know.
                 // For now, let it fall through or rely on z-order.
            }
        }
        
        if (GUI::backbuffer) Graphics::SetBuffer(screen);
        GUI::DrawDesktop(); // Blit full screen
    }

    // Draw Mouse
    static int old_mx = -1, old_my = -1;
    
    // 1. Restore background under old mouse position
    if (old_mx != -1) {
        // Assume mouse cursor size is roughly 16x24
        // To be safe, restore slightly larger area
        GUI::RestoreRegion(old_mx, old_my, 24, 32); 
    }
    
    // 2. Draw new mouse (directly to screen)
    // We must ensure we are drawing to SCREEN buffer, not backbuffer
    uint8_t* current_buf = Graphics::GetBuffer();
    (void)current_buf;
    // Assuming GetBuffer returns current target.
    // LockScreen::Update usually leaves buffer at Screen.
    
    Mouse::DrawAt(Mouse::mx, Mouse::my);
    Graphics::SwapBuffers();
    
    old_mx = Mouse::mx;
    old_my = Mouse::my;
}

void LockScreen::DrawIdle(int w, int h) {
    int ty = h / 2 - h / 14;
    auto dt_loc = TimeManager::NowLocalDateTime();
    
    // Use the optimized DrawTime or DrawString
    char timebuf[16];
    (void)timebuf;
    // ... format time ...
    // Reuse existing logic
    RTC::Time t; t.h = dt_loc.h; t.m = dt_loc.m; t.s = dt_loc.s;
    // Fix __udivdi3 undefined reference by casting to uint32_t first (we don't need 64-bit division here)
    uint32_t now_us_low = (uint32_t)TimeManager::NowUTC().us;
    bool colon = ((now_us_low / 500000u) % 2) == 0;
    
    if (FontTTF::ok) {
        // ...
    } else {
        // Fallback
        int scale = w / 100; if (scale < 8) scale = 8;
        // ...
        DrawTime((w - (4*3*scale + 3*scale + 2*scale))/2, ty, scale, t.h, t.m, colon);
    }
    
    // Draw "Press Space to Unlock"
    const char* msg = "Press Space to Unlock";
    FontTTF::DrawStringCenter(w/2, h - 100, 24.0f, msg, 0xFFFFFFFF);
    
    // Draw Status Bar
    int status_y = h - 30;
    Graphics::FillRect(0, status_y, w, 30, 0xFF222222);
    
    // Draw Input Device Info
    char dev_buf[64];
    int dev_count = InputManager::GetDeviceCount();
    const char* layout = InputManager::GetLayoutName();
    
    // Manual snprintf replacement
    auto append = [](char* b, const char* s) { while(*s) *b++ = *s++; return b; };
    char* p = dev_buf;
    p = append(p, "Devices: ");
    if (dev_count >= 10) { *p++='1'; *p++='0'+(dev_count%10); } else { *p++='0'+dev_count; }
    p = append(p, " | Layout: ");
    p = append(p, layout);
    *p = 0;
    
    FontTTF::DrawString(10, status_y + 8, 14.0f, dev_buf, 0xFFAAAAAA);
    
    // Visualize Key
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
    
    // Frosted panel behind login
    Graphics::FillRoundedRect(cx - 160, cy - 140, 320, 260, 16, 0xD0141428);
    
    // Avatar circle
    Graphics::FillCircle(cx, cy - 100, 32, 0xFF3D3D5C);
    Graphics::FillCircle(cx, cy - 100, 28, 0xFF5C5C8A);
    // User icon silhouette
    Graphics::FillCircle(cx, cy - 108, 10, 0xFFCCCCDD);
    Graphics::FillRoundedRect(cx - 14, cy - 92, 28, 18, 8, 0xFFCCCCDD);
    
    // Username/Password inputs
    input_user->Draw();
    input_pass->Draw();
    
    // Login Button
    btn_login->Draw();
    btn_hc->Draw();
    
    // Handle Input
    if (Keyboard::HasChar()) {
        char c = Keyboard::GetChar();
        if (input_user->focused) input_user->OnKey(c);
        if (input_pass->focused) input_pass->OnKey(c);
        if (c == '\t') {
            input_user->focused = !input_user->focused;
            input_pass->focused = !input_pass->focused;
        }
    }
    
    if (frame_clicked) {
        int mx, my; Mouse::GetPosition(mx, my);
        input_user->focused = input_user->Contains(mx, my);
        input_pass->focused = input_pass->Contains(mx, my);
        // Button click handled in main loop or here?
        // btn_login->OnClick(mx, my); // logic in Show() loop for break
    }
}

void LockScreen::DrawSetup(int w, int h) {
    int cx = w/2; int cy = h/2;
    
    FontTTF::DrawStringCenter(cx, cy - 150, 32.0f, "Welcome", 0xFFFFFFFF);
    FontTTF::DrawStringCenter(cx, cy - 120, 20.0f, "Create your first user", 0xFFAAAAAA);
    
    // Frosted panel
    Graphics::FillRoundedRect(cx - 180, cy - 90, 360, 280, 16, 0xD0141428);
    
    input_user->Draw();
    input_pass->Draw();
    btn_browse->Draw();
    btn_create->Draw();
    
    if (FileBrowser::selected_file[0]) {
        FontTTF::DrawString(cx + 70, cy + 68, 14.0f, FileBrowser::selected_file, 0xFF00FF00);
    }
    
    // Handle Input
    if (Keyboard::HasChar()) {
        char c = Keyboard::GetChar();
        if (input_user->focused) input_user->OnKey(c);
        if (input_pass->focused) input_pass->OnKey(c);
    }
    
    if (frame_clicked) {
        int mx, my; Mouse::GetPosition(mx, my);
        bool user_f = input_user->Contains(mx, my);
        bool pass_f = input_pass->Contains(mx, my);
        
        // Only change focus if clicked inside one of them
        // This prevents losing focus when clicking buttons or background (optional UX choice)
        // For now, let's allow background click to clear focus to be standard.
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

// Helpers from previous implementation
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
void LockScreen::DrawTime(int tx, int ty, int s, uint8_t hh, uint8_t mm, bool colon) {
    uint32_t color = 0xFFFFFFFF;
    int dig_w = 3 * s; int gap = s; int cx = tx;
    DrawDigit(cx, ty, s, (hh / 10), color); cx += dig_w + gap;
    DrawDigit(cx, ty, s, (hh % 10), color); cx += dig_w + gap;
    DrawColon(cx, ty, s, color, colon); cx += s * 2;
    DrawDigit(cx, ty, s, (mm / 10), color); cx += dig_w + gap;
    DrawDigit(cx, ty, s, (mm % 10), color);
}
