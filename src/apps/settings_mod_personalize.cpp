//  kurono os - settings module: appearance (satoru)
//  light / dark appearance (ui.appearance, followed by settings and the window
//  titlebars), interface style, icon style, interface size, accent colour,
//  wallpaper (the two embedded pictures or a solid colour), desktop icon size,
//  window animations and shadows, and the classic taskbar. every control
//  persists through UIConfig and applies live. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../system/ui_config.h"
#include "../ui/desktop.h"
#include "../ui/window_manager.h"
#include "../ui/style.h"

using namespace SettingsUI;

static const uint32_t kAccent[] = {
    0xFF0A84FF, 0xFF5E5CE6, 0xFFBF5AF2, 0xFFFF375F, 0xFFFF453A,
    0xFFFF9F0A, 0xFFFFCC00, 0xFF30D158, 0xFF8E8E93,
};
static const int kAccentCount = 9;

static const uint32_t kBars[] = { 0xFF0C0C14, 0xFF14141E, 0xFF1A1A28, 0xFF0A0E18, 0xFF181020 };
static const int kBarsCount = 5;

// wallpaper choices: 0, 1 = embedded pictures, 2.. = solid colours (satoru)
static const uint32_t kWallColors[] = { 0xFF0B0F1A, 0xFF1B2A4A, 0xFF2D3142, 0xFF3B1F4A, 0xFF16372B, 0xFF1C1C1E };
static const char* kWallNames[] = { "Mist", "Peaks", "Midnight", "Ocean", "Slate", "Plum", "Pine", "Graphite" };
static const int kWallCount = 8;

static const char* kThemeNames[] = { "Light", "Dark" };
static const char* kStyles[] = { "Modern", "Classic" };
static const char* kIconSets[] = { "Colorful", "Minimal" };
static const char* kSizes[] = { "100%", "125%", "150%", "200%" };
static const int   kSizeVals[] = { 100, 125, 150, 200 };
static const char* kBarPos[] = { "Bottom", "Top" };

static int  s_theme = 1, s_style = 0, s_icons = 0, s_size = 0, s_accent = 0, s_wall = 0;
static int  s_icon_size = 56, s_anim_ms = 180, s_bar_h = 44, s_bar_pos = 0, s_bar_bg = -1;
static bool s_anim = true, s_shadow = true, s_shadow_drag = false;

// ── thumbnails of the embedded wallpapers (box filtered once) ───────────────
static const int TH_W = 132, TH_H = 80;
static uint32_t s_thumb[2][TH_W * TH_H];
static bool     s_thumb_ok[2];

static void build_thumb(int idx){
    int sw = 0, sh = 0;
    const uint8_t* src = Desktop::BuiltinWallpaperRGBA(idx, &sw, &sh);
    if(!src || sw <= 0 || sh <= 0) return;
    // crop to the tile's aspect, then average each destination cell. (satoru)
    int cw = sw, ch = sw * TH_H / TH_W;
    if(ch > sh){ ch = sh; cw = sh * TH_W / TH_H; }
    int ox = (sw - cw) / 2, oy = (sh - ch) / 2;
    for(int y = 0; y < TH_H; y++){
        int y0 = oy + y * ch / TH_H, y1 = oy + (y + 1) * ch / TH_H; if(y1 <= y0) y1 = y0 + 1;
        for(int x = 0; x < TH_W; x++){
            int x0 = ox + x * cw / TH_W, x1 = ox + (x + 1) * cw / TH_W; if(x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for(int yy = y0; yy < y1; yy += 2)
                for(int xx = x0; xx < x1; xx += 2){
                    const uint8_t* p = src + ((size_t)yy * sw + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            if(!n) n = 1;
            s_thumb[idx][y * TH_W + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    s_thumb_ok[idx] = true;
}

static void paint_wall(int i, int x, int y, int w, int h){
    if(i < 2){
        if(!s_thumb_ok[i]) build_thumb(i);
        if(s_thumb_ok[i]){ Image(s_thumb[i], TH_W, TH_H, x + (w - TH_W) / 2, y + (h - TH_H) / 2); return; }
    }
    uint32_t c = kWallColors[(i - 2) % 6];
    FillRound(x, y, w, h, 0, c);
    FillRound(x, y, w, h / 2, 0, 0xFFFFFFFFu, 14);
}

static void paint_theme(int i, int x, int y, int w, int h){
    bool light = i == 0;
    uint32_t bg = light ? 0xFFF6F6F8 : 0xFF18181B, side = light ? 0xFFE6E6EB : 0xFF222227;
    uint32_t card = light ? 0xFFFFFFFF : 0xFF2A2A30, line = light ? 0xFFCFCFD6 : 0xFF45454E;
    FillRound(x, y, w, h, 0, bg);
    FillRound(x, y, w * 30 / 100, h, 0, side);
    FillRound(x + 8, y + 12, w * 30 / 100 - 16, 8, 4, Accent(), 210);
    for(int k = 0; k < 3; k++) FillRound(x + 8, y + 26 + k * 12, w * 30 / 100 - 22, 5, 2, line);
    int cx = x + w * 30 / 100 + 10, cw = w - (cx - x) - 10;
    FillRound(cx, y + 12, cw, 26, 5, card);
    FillRound(cx, y + 44, cw, 26, 5, card);
    FillRound(cx + 6, y + 21, cw / 2, 6, 3, line);
    FillRound(cx + 6, y + 53, cw / 3, 6, 3, line);
    FillRound(cx + cw - 22, y + 19, 16, 10, 5, Accent());
    FillRound(cx + cw - 22, y + 51, 16, 10, 5, line);
}

static void apply_live(){
    Taskbar::ReloadFromConfig();
    Desktop::ReloadFromConfig();
    WindowManager::ReloadFromConfig();
    Graphics::MarkUIDirty();
}

static void personalize_on_show(){
    const char* ap = UIConfig::Str("ui.appearance", "dark");
    s_theme = (ap && (ap[0] == 'l' || ap[0] == 'L')) ? 0 : 1;
    s_style = UIStyle::Modern() ? 0 : 1;
    s_icons = UIStyle::MinimalIcons() ? 1 : 0;
    int sc = Graphics::GetUIScale();
    s_size = 0;
    for(int i = 0; i < 4; i++) if(kSizeVals[i] <= sc) s_size = i;
    uint32_t acc = UIConfig::Color("theme.accent", 0xFF0A84FF) | 0xFF000000u;
    s_accent = -1;
    for(int i = 0; i < kAccentCount; i++) if(kAccent[i] == acc) s_accent = i;
    int wi = UIConfig::Int("desktop.wallpaper_index", 0);
    if(wi == 2){
        uint32_t c = UIConfig::Color("desktop.bg", 0) | 0xFF000000u;
        s_wall = -1;
        for(int i = 0; i < 6; i++) if(kWallColors[i] == c) s_wall = 2 + i;
    } else s_wall = wi == 1 ? 1 : 0;
    s_icon_size = UIConfig::Int("desktop.icon_size", 56);
    if(s_icon_size < 32) s_icon_size = 32;
    if(s_icon_size > 96) s_icon_size = 96;
    s_anim = UIConfig::Bool("compositor.window_animations", true);
    s_anim_ms = UIConfig::Int("compositor.animation_speed_ms", 180);
    if(s_anim_ms < 60) s_anim_ms = 60;
    if(s_anim_ms > 400) s_anim_ms = 400;
    s_shadow = UIConfig::Bool("compositor.shadow_enabled", true);
    s_shadow_drag = UIConfig::Bool("compositor.shadow_during_drag", false);
    const char* pos = UIConfig::Str("taskbar.position", "bottom");
    s_bar_pos = (pos && (pos[0] == 't' || pos[0] == 'T')) ? 1 : 0;
    s_bar_h = UIConfig::Int("taskbar.height", 44);
    if(s_bar_h < 32) s_bar_h = 32;
    if(s_bar_h > 72) s_bar_h = 72;
    uint32_t bb = UIConfig::Color("taskbar.bg", 0xFF0C0C14) | 0xFF000000u;
    s_bar_bg = -1;
    for(int i = 0; i < kBarsCount; i++) if(kBars[i] == bb) s_bar_bg = i;
}

static void personalize_build(){
    char buf[32];

    Group("Style");
    if(Tiles(kThemeNames, 2, &s_theme, 150, 92, paint_theme)){
        UIConfig::Set("ui.appearance", s_theme == 0 ? "light" : "dark", true);
        UIConfig::Save();
        Graphics::MarkUIDirty();
    }
    if(Segmented("Interface style", kStyles, 2, &s_style)){
        UIStyle::Set(s_style == 0);
        DesktopEnvironment::ReloadFromConfig();
    }
    if(s_style == 0 && Segmented("Icon style", kIconSets, 2, &s_icons)){
        UIStyle::SetMinimalIcons(s_icons == 1);
        DesktopEnvironment::ReloadFromConfig();
    }
    if(Segmented("Interface size", kSizes, 4, &s_size, "Scales windows, text, dock and the cursor")){
        UIConfig::SetInt("display.ui_scale", kSizeVals[s_size], true);
        UIConfig::Save();
        Graphics::RequestUIScale(kSizeVals[s_size]);
    }
    EndGroup();

    Group("Accent color");
    if(Swatches("Accent", kAccent, kAccentCount, &s_accent) && s_accent >= 0){
        uint32_t a = kAccent[s_accent];
        uint32_t dim = 0xFF000000u | ((((a >> 16) & 0xFF) * 7 / 8) << 16) | ((((a >> 8) & 0xFF) * 7 / 8) << 8) | ((a & 0xFF) * 7 / 8);
        UIConfig::SetColor("theme.accent", a, true);
        UIConfig::SetColor("taskbar.start_btn_bg", a, false);
        UIConfig::SetColor("taskbar.start_btn_hover", dim, false);
        UIConfig::Save();
        apply_live();
    }
    EndGroup();

    Group("Wallpaper");
    if(Tiles(kWallNames, kWallCount, &s_wall, 132, 80, paint_wall) && s_wall >= 0){
        if(s_wall < 2){
            UIConfig::SetInt("desktop.wallpaper_index", s_wall, true);
            UIConfig::Save();
            Desktop::ApplyBuiltinWallpaper(s_wall);
        } else {
            uint32_t c = kWallColors[s_wall - 2];
            UIConfig::SetInt("desktop.wallpaper_index", 2, true);
            UIConfig::SetColor("desktop.bg", c, true);
            UIConfig::Save();
            Desktop::SetWallpaper(c);
        }
        apply_live();
    }
    EndGroup();

    Group("Desktop");
    IntToStr(s_icon_size, buf, 32); StrApp(buf, " px", 32);
    if(Slider("Icon size", &s_icon_size, 32, 96, 4, buf)){
        UIConfig::SetInt("desktop.icon_size", s_icon_size, true);
        if(Committed()) UIConfig::Save();
        Desktop::ReloadFromConfig();
        Graphics::MarkUIDirty();
    }
    EndGroup();

    Group("Windows");
    if(Toggle("Animations", &s_anim, "Window and interface motion")){
        UIConfig::SetInt("compositor.window_animations", s_anim ? 1 : 0, true);
        UIConfig::Save();
        apply_live();
    }
    if(s_anim){
        const char* sp = s_anim_ms < 140 ? "Fast" : (s_anim_ms <= 240 ? "Normal" : "Slow");
        if(Slider("Animation speed", &s_anim_ms, 60, 400, 10, sp)){
            UIConfig::SetInt("compositor.animation_speed_ms", s_anim_ms, true);
            if(Committed()) UIConfig::Save();
            WindowManager::ReloadFromConfig();
        }
    }
    if(Toggle("Window shadows", &s_shadow)){
        UIConfig::SetInt("compositor.shadow_enabled", s_shadow ? 1 : 0, true);
        UIConfig::Save();
        apply_live();
    }
    if(s_shadow && Toggle("Shadows while dragging", &s_shadow_drag, "Costs a little speed when moving windows")){
        UIConfig::SetInt("compositor.shadow_during_drag", s_shadow_drag ? 1 : 0, true);
        UIConfig::Save();
        apply_live();
    }
    EndGroup();

    // the classic style has a taskbar; the modern style uses the menu bar + dock (satoru)
    if(s_style == 1){
        Group("Taskbar");
        if(Segmented("Position", kBarPos, 2, &s_bar_pos)){
            UIConfig::Set("taskbar.position", s_bar_pos ? "top" : "bottom", true);
            UIConfig::Save();
            apply_live();
        }
        IntToStr(s_bar_h, buf, 32); StrApp(buf, " px", 32);
        if(Slider("Height", &s_bar_h, 32, 72, 2, buf)){
            UIConfig::SetInt("taskbar.height", s_bar_h, true);
            if(Committed()) UIConfig::Save();
            apply_live();
        }
        if(Swatches("Background", kBars, kBarsCount, &s_bar_bg) && s_bar_bg >= 0){
            UIConfig::SetColor("taskbar.bg", kBars[s_bar_bg], true);
            UIConfig::Save();
            apply_live();
        }
        EndGroup();
    }
}

extern const SettingsModule g_personalize_module = {
    "personalize", "Appearance", "", personalize_on_show, nullptr, nullptr, nullptr, personalize_build
};
// end (satoru)
