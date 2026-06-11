//  kurono os  -  settings module: display & monitors (reference module #1) (satoru)
//  shows backend/monitor, a resolution selector (from DisplayManager supported
//  modes), a refresh-rate selector that applies live + persists, a brightness
//  slider, and a vsync toggle. all writes go through UIConfig + Graphics. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../system/ui_config.h"

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static int      s_brightness = 75;     // 0..100, persisted to display.brightness (satoru)
static int      s_refresh_hz = 60;     // target fps cap, persisted to display.refresh_hz (satoru)
static bool     s_vsync      = true;   // persisted to display.vsync (satoru)
static int      s_mode_count = 0;      // populated by on_show from DisplayManager (satoru)
static DisplayMode s_modes[DISPLAY_MODE_COUNT];

// candidate refresh rates; clamped to the monitor's detected ceiling. (satoru)
static const int kRates[]   = {30, 60, 75, 120, 144, 165, 240};
static const int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));

// ── helpers ─────────────────────────────────────────────────────────────────
// index of the supported mode that matches the live resolution, else 0. (satoru)
static int current_mode_index(){
    uint32_t cw = (uint32_t)Graphics::GetWidth();
    uint32_t ch = (uint32_t)Graphics::GetHeight();
    for(int i = 0; i < s_mode_count; i++){
        if(s_modes[i].width == cw && s_modes[i].height == ch) return i;
    }
    return 0;
}

static int rate_ceiling(){
    uint32_t mon = Graphics::GetMonitorHz();
    return mon > 0 ? (int)mon : kRates[kRateCount - 1];
}

static void apply_refresh(int hz){
    if(hz <= 0) return;
    s_refresh_hz = hz;
    // persist + take effect live: the gui pacing reads GetTargetFrameTimeMs(). (satoru)
    UIConfig::SetInt("display.refresh_hz", hz, true);
    Graphics::SetTargetFPS((uint32_t)hz);
    UIConfig::Save();
}

static void persist_brightness(){
    UIConfig::SetInt("display.brightness", s_brightness, true);
    // 0..100 → 0..255 hardware gamma scale where the backend supports it. (satoru)
    DisplayManager::SetBrightness((uint8_t)((s_brightness * 255) / 100));
    UIConfig::Save();
}

static void persist_vsync(){
    UIConfig::SetInt("display.vsync", s_vsync ? 1 : 0, true);
    DisplayManager::SetVSync(s_vsync ? VSYNC_ON : VSYNC_OFF);
    UIConfig::Save();
}

// ── on_show: load persisted config + detect modes ───────────────────────────
static void display_on_show(){
    int count = 0;
    if(DisplayManager::GetSupportedModes(s_modes, DISPLAY_MODE_COUNT, &count) && count > 0){
        s_mode_count = count;
    } else {
        s_mode_count = 0;
    }
    s_brightness = UIConfig::Int("display.brightness", s_brightness);
    if(s_brightness < 0) s_brightness = 0;
    if(s_brightness > 100) s_brightness = 100;

    // seed refresh from config, else from the live target, else monitor hz. (satoru)
    int hz = UIConfig::Int("display.refresh_hz", 0);
    if(hz <= 0){
        uint32_t tgt = Graphics::GetTargetFPS();
        hz = tgt > 0 ? (int)tgt : rate_ceiling();
    }
    int ceil = rate_ceiling();
    if(hz > ceil) hz = ceil;
    s_refresh_hz = hz;
    s_vsync = UIConfig::Int("display.vsync", s_vsync ? 1 : 0) != 0;
}

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly (the SettingsModule input() signature gives no pane width,
//    so we fix the control geometry here rather than derive it). (satoru)
static const int CTRL_X_OFF = 170;   // controls column, relative to pane left (satoru)
static const int CTRL_W     = 300;   // dropdown / control width in px (satoru)
static const int SLIDER_W   = 250;   // brightness track width in px (satoru)

// clamp the control width to whatever the pane actually offers. (satoru)
static int ctrl_w_for(int pane_w){
    int avail = pane_w - CTRL_X_OFF;
    int cw = CTRL_W;
    if(cw > avail) cw = avail;
    if(cw < 80) cw = 80;
    return cw;
}

static void display_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ctrl_w = ctrl_w_for(w);
    int ly = y - scroll + 8;
    char buf[48];

    SettingsUI::SectionHeader(x, ly, "Monitor");
    ly += 26;

    const MonitorInfo& mon = DisplayManager::GetMonitorInfo();
    SettingsUI::Row(x, ly, "Backend:", DisplayManager::GetBackendName());
    ly += 22;
    SettingsUI::Row(x, ly, "Monitor:",
                    (mon.connected && mon.name[0]) ? mon.name : "Generic Display");
    ly += 22;

    // detected monitor refresh (read-only info). (satoru)
    {
        uint32_t mhz = Graphics::GetMonitorHz();
        if(mhz > 0){ SettingsUI::IntToStr((int)mhz, buf, 48); SettingsUI::StrApp(buf, " Hz", 48); }
        else SettingsUI::StrCpy(buf, "Unknown", 48);
        SettingsUI::Row(x, ly, "Detected Hz:", buf);
    }
    ly += 30;

    SettingsUI::SectionHeader(x, ly, "Resolution");
    ly += 26;

    // current resolution as "WxH". (satoru)
    {
        SettingsUI::IntToStr((int)Graphics::GetWidth(), buf, 48);
        SettingsUI::StrApp(buf, " x ", 48);
        char hb[12]; SettingsUI::IntToStr((int)Graphics::GetHeight(), hb, 12);
        SettingsUI::StrApp(buf, hb, 48);
    }
    Graphics::DrawString(x, ly + 4, "Display Mode:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(ctrl_x, ly, ctrl_w, buf);
    ly += 30;

    // a small note on whether the backend can actually switch modes. (satoru)
    {
        DisplayBackend be = DisplayManager::GetBackend();
        bool switchable = (be == DISPLAY_BACKEND_BGA || be == DISPLAY_BACKEND_VIRTIO_GPU);
        SettingsUI::Row(x, ly, "Mode Control:",
                        switchable ? "Software (BGA/virtio)" : "Firmware controlled");
    }
    ly += 30;

    SettingsUI::SectionHeader(x, ly, "Refresh & Sync");
    ly += 26;

    // refresh-rate selector. (satoru)
    SettingsUI::IntToStr(s_refresh_hz, buf, 48); SettingsUI::StrApp(buf, " Hz", 48);
    Graphics::DrawString(x, ly + 4, "Refresh Rate:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(ctrl_x, ly, ctrl_w, buf);
    ly += 30;

    // live target fps (read-only confirmation). (satoru)
    {
        SettingsUI::StrCpy(buf, "Target ", 48);
        char nb[12]; SettingsUI::IntToStr((int)Graphics::GetTargetFPS(), nb, 12);
        SettingsUI::StrApp(buf, nb, 48); SettingsUI::StrApp(buf, " fps", 48);
        SettingsUI::Row(x, ly, "Active:", buf);
    }
    ly += 30;

    // vsync toggle. (satoru)
    Graphics::DrawString(x, ly + 2, "VSync:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_vsync);
    ly += 32;

    SettingsUI::SectionHeader(x, ly, "Brightness");
    ly += 26;

    // brightness slider with a percentage readout. (satoru)
    Graphics::DrawString(x, ly + 2, "Brightness:", SettingsUI::COL_TEXT, 0xFF000000);
    int sl_w = SLIDER_W;
    if(sl_w > ctrl_w - 50) sl_w = ctrl_w - 50;
    if(sl_w < 60) sl_w = 60;
    SettingsUI::Slider(ctrl_x, ly, sl_w, s_brightness);
    SettingsUI::IntToStr(s_brightness, buf, 48); SettingsUI::StrApp(buf, "%", 48);
    Graphics::DrawString(ctrl_x + sl_w + 10, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    ly += 24;
    // honest note: the value persists, but this backend has no panel/gamma
    // dimming, so the slider does not actually change screen brightness. (satoru)
    Graphics::DrawString(x, ly, "Saved as a preference  -  this backend has no hardware dimming.",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 16;
}

// ── input: pane-local mx,my. we walk the SAME running-y layout as the render
//    (already offset by -scroll), so hit rects line up exactly. controls sit at
//    pane-local x = CTRL_X_OFF with the fixed widths declared above. (satoru)
static bool display_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + 8;

    ly += 26;          // "Monitor" header (satoru)
    ly += 22;          // backend row (satoru)
    ly += 22;          // monitor row (satoru)
    ly += 30;          // detected hz row (satoru)
    ly += 26;          // "Resolution" header (satoru)

    // resolution dropdown row. arrows cycle through supported modes; on firmware
    // backends SetResolution is a no-op and the live size simply stays. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, CTRL_W, mx, my);
        if(hit >= 0 && s_mode_count > 0){
            int idx = current_mode_index();
            idx += (hit == 0) ? -1 : 1;
            if(idx < 0) idx = 0;
            if(idx >= s_mode_count) idx = s_mode_count - 1;
            DisplayManager::SetResolution(s_modes[idx].width, s_modes[idx].height,
                                          s_modes[idx].bpp ? s_modes[idx].bpp : 32);
            return true;
        }
    }
    ly += 30;          // resolution row (satoru)
    ly += 30;          // mode-control row (satoru)
    ly += 26;          // "Refresh & Sync" header (satoru)

    // refresh-rate dropdown. step within the monitor-clamped rate list. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, CTRL_W, mx, my);
        if(hit >= 0){
            int ceil = rate_ceiling();
            if(hit == 0){
                for(int i = kRateCount - 1; i >= 0; i--){
                    if(kRates[i] < s_refresh_hz && kRates[i] <= ceil){ apply_refresh(kRates[i]); break; }
                }
            } else {
                for(int i = 0; i < kRateCount; i++){
                    if(kRates[i] > s_refresh_hz && kRates[i] <= ceil){ apply_refresh(kRates[i]); break; }
                }
            }
            return true;
        }
    }
    ly += 30;          // refresh row (satoru)
    ly += 30;          // active fps row (satoru)

    // vsync toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_vsync = !s_vsync;
        persist_vsync();
        return true;
    }
    ly += 32;          // vsync row (satoru)
    ly += 26;          // "Brightness" header (satoru)

    // brightness slider. (satoru)
    {
        int sl_w = SLIDER_W;
        int p = SettingsUI::SliderHit(ctrl_x, ly, sl_w, mx, my);
        if(p >= 0){
            s_brightness = p;
            persist_brightness();
            return true;
        }
    }
    return false;
}

// total content height for the scrollbar (sum of the row advances above + tail).
// the brightness section is header(26) + slider(24) + honest note(16). (satoru)
static int display_content_height(){
    // 8 + 26 + 22 + 22 + 30 + 26 + 30 + 30 + 26 + 30 + 30 + 32 + 26 + (24+16) (satoru)
    return 8 + 26 + 22 + 22 + 30 + 26 + 30 + 30 + 26 + 30 + 30 + 32 + 26 + 24 + 16 + 16;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_display_module;` resolves at link time. without
// it, a namespace-scope const has internal linkage and the link fails. (satoru)
extern const SettingsModule g_display_module = {
    "display", "Display & Monitors", "\x0F",
    display_on_show, display_render, display_input, display_content_height
};
// end (satoru)
