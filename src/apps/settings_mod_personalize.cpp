//  kurono os  -  settings module: personalization (satoru)
//  a rich, fully-live personalization page: accent swatches (theme.accent),
//  taskbar position/height/background, desktop icon size + wallpaper tint,
//  window effects (animations, shadows, corner radius) and a ui font scale.
//  every control persists through UIConfig and applies live by re-driving the
//  per-frame ReloadFromConfig() paths on the taskbar, desktop and wm. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../system/ui_config.h"
#include "../ui/desktop.h"            // taskbar / desktop reloadfromconfig + setwallpaper (satoru)
#include "../ui/window_manager.h"     // compositor reloadfromconfig (satoru)
#include "settings.h"                 // SettingsApp::state.font_scale is the live font consumer (satoru)

// ── shared palettes ─────────────────────────────────────────────────────────
// accent choices mirror the legacy settings app so a user's pick lines up with
// the rest of the shell chrome. (satoru)
static const unsigned int kAccent[] = {
    0xFF3498DB, 0xFF9B59B6, 0xFF1ABC9C, 0xFFE74C3C,
    0xFFF39C12, 0xFF2ECC71, 0xFFE91E63, 0xFF00BCD4,
};
static const int kAccentCount = (int)(sizeof(kAccent) / sizeof(kAccent[0]));

// candidate taskbar background tints (dark chrome variants). (satoru)
static const unsigned int kBars[] = {
    0xFF0C0C14, 0xFF101018, 0xFF14141E, 0xFF1A1A28, 0xFF0A0E18, 0xFF181020,
};
static const int kBarsCount = (int)(sizeof(kBars) / sizeof(kBars[0]));

// wallpaper tint choices; stored to desktop.bg and pushed live via
// Desktop::SetWallpaper so the solid-colour wallpaper updates immediately. (satoru)
static const unsigned int kWalls[] = {
    0xFF0C0818, 0xFF0B0F1A, 0xFF101827, 0xFF141022, 0xFF0A1410, 0xFF181014,
};
static const char* kWallNames[] = {
    "Midnight", "Deep Blue", "Slate", "Plum", "Forest", "Maroon",
};
static const int kWallsCount = (int)(sizeof(kWalls) / sizeof(kWalls[0]));

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static unsigned int s_accent      = 0xFF3498DB;  // theme.accent (satoru)
static bool         s_bar_top      = false;       // taskbar.position (satoru)
static int          s_bar_height   = 44;          // taskbar.height (satoru)
static unsigned int s_bar_bg       = 0xFF0C0C14;  // taskbar.bg (satoru)
static int          s_icon_size    = 56;          // desktop.icon_size (satoru)
static unsigned int s_wall         = 0xFF0C0818;  // desktop.bg (wallpaper tint) (satoru)
static int          s_wall_img     = 0;           // desktop.wallpaper_index (builtin image) (satoru)
static bool         s_anim         = true;        // compositor.window_animations (satoru)
static int          s_anim_ms      = 90;          // compositor.animation_speed_ms (satoru)
static bool         s_shadow       = true;        // compositor.shadow_enabled (satoru)
static bool         s_shadow_drag  = false;       // compositor.shadow_during_drag (satoru)
static int          s_corner       = 10;          // window.corner_radius (satoru)
static int          s_font_scale   = 1;           // display.font_scale (1..3) (satoru)

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly. the input() signature carries no pane width, so the
//    control geometry is fixed here. (satoru)
static const int CTRL_X     = 190;   // controls column, pane-relative (satoru)
static const int SWATCH     = 26;    // swatch box side (satoru)
static const int SWATCH_GAP = 8;     // gap between swatches (satoru)
static const int SLIDER_W   = 220;   // slider track width (satoru)
static const int DROP_W     = 200;   // dropdown pill width (satoru)

// ── helpers ─────────────────────────────────────────────────────────────────
// derive an 88%-brightness hover colour from an accent (matches legacy). (satoru)
static unsigned int dim88(unsigned int argb){
    unsigned int r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return 0xFF000000u | (((r*7)/8) << 16) | (((g*7)/8) << 8) | ((b*7)/8);
}

// push the live reloads so every chrome surface repaints from config. (satoru)
static void apply_live(){
    Taskbar::ReloadFromConfig();
    Desktop::ReloadFromConfig();
    WindowManager::ReloadFromConfig();
    Graphics::MarkUIDirty();
}

static void persist_accent(unsigned int argb){
    s_accent = argb;
    // theme.accent is read by the wm focus border + the shell; the taskbar
    // start button + hover are bumped too so the accent shows up at once. (satoru)
    UIConfig::SetColor("theme.accent",            argb,        true);
    UIConfig::SetColor("taskbar.start_btn_bg",    argb,        false);
    UIConfig::SetColor("taskbar.start_btn_hover", dim88(argb), false);
    UIConfig::Save();
    apply_live();
}

static void persist_bar_position(){
    UIConfig::Set("taskbar.position", s_bar_top ? "top" : "bottom", true);
    apply_live();
}

static void persist_bar_height(){
    if(s_bar_height < 32)  s_bar_height = 32;
    if(s_bar_height > 72)  s_bar_height = 72;
    UIConfig::SetInt("taskbar.height", s_bar_height, true);
    UIConfig::Save();
    apply_live();
}

static void persist_bar_bg(unsigned int argb){
    s_bar_bg = argb;
    UIConfig::SetColor("taskbar.bg", argb, true);
    UIConfig::Save();
    apply_live();
}

static void persist_icon_size(){
    if(s_icon_size < 32) s_icon_size = 32;
    if(s_icon_size > 96) s_icon_size = 96;
    UIConfig::SetInt("desktop.icon_size", s_icon_size, true);
    UIConfig::Save();
    apply_live();
}

static void persist_wallpaper(unsigned int argb){
    s_wall = argb;
    // store the tint and push it to the live solid-colour wallpaper. ReloadFromConfig
    // only re-reads the desktop chrome colour, so SetWallpaper does the visible swap. (satoru)
    UIConfig::SetColor("desktop.bg", argb, true);
    UIConfig::Save();
    Desktop::SetWallpaper(argb);
    apply_live();
}

static void persist_wall_img(){
    if(s_wall_img < 0) s_wall_img = 0;
    if(s_wall_img > 1) s_wall_img = 1;
    // persist the chosen builtin image and switch it live. (satoru)
    UIConfig::SetInt("desktop.wallpaper_index", s_wall_img, true);
    UIConfig::Save();
    Desktop::ApplyBuiltinWallpaper(s_wall_img);
    apply_live();
}

static void persist_anim(){
    UIConfig::SetInt("compositor.window_animations", s_anim ? 1 : 0, true);
    UIConfig::Save();
    apply_live();
}

static void persist_anim_ms(){
    if(s_anim_ms < 0)   s_anim_ms = 0;
    if(s_anim_ms > 400) s_anim_ms = 400;
    UIConfig::SetInt("compositor.animation_speed_ms", s_anim_ms, true);
    UIConfig::Save();
    apply_live();
}

static void persist_shadow(){
    UIConfig::SetInt("compositor.shadow_enabled", s_shadow ? 1 : 0, true);
    UIConfig::Save();
    apply_live();
}

static void persist_shadow_drag(){
    UIConfig::SetInt("compositor.shadow_during_drag", s_shadow_drag ? 1 : 0, true);
    UIConfig::Save();
    apply_live();
}

static void persist_corner(){
    if(s_corner < 0)  s_corner = 0;
    if(s_corner > 20) s_corner = 20;
    UIConfig::SetInt("window.corner_radius", s_corner, true);
    UIConfig::Save();
    apply_live();
}

static void persist_font_scale(){
    if(s_font_scale < 1) s_font_scale = 1;
    if(s_font_scale > 3) s_font_scale = 3;
    UIConfig::SetInt("display.font_scale", s_font_scale, true);
    UIConfig::Save();
    // the live text renderer (graphics.cpp) scales off SettingsApp::state.font_scale,
    // so push it through and repaint to make the change take effect now. (satoru)
    SettingsApp::state.font_scale = s_font_scale;
    Graphics::MarkUIDirty();
}

// index of the currently-selected colour within a palette, else -1. (satoru)
static int palette_index(const unsigned int* pal, int n, unsigned int cur){
    for(int i = 0; i < n; i++) if(pal[i] == cur) return i;
    return -1;
}

// draw a horizontal row of selectable colour swatches; the selected one gets a
// bright ring. returns nothing  -  hit-testing recomputes the same x steps. (satoru)
static void draw_swatches(int x, int y, const unsigned int* pal, int n, unsigned int cur){
    for(int i = 0; i < n; i++){
        int sx = x + i * (SWATCH + SWATCH_GAP);
        Graphics::FillRoundedRect(sx, y, SWATCH, SWATCH, 6, pal[i]);
        if(pal[i] == cur){
            Graphics::DrawRect(sx - 2, y - 2, SWATCH + 4, SWATCH + 4, SettingsUI::COL_WHITE);
            Graphics::DrawRect(sx - 3, y - 3, SWATCH + 6, SWATCH + 6, SettingsUI::Accent());
        } else {
            Graphics::DrawRect(sx, y, SWATCH, SWATCH, SettingsUI::COL_BORDER);
        }
    }
}

// hit-test a swatch row; returns the clicked index or -1. (satoru)
static int swatch_hit(int x, int y, int n, int mx, int my){
    if(my < y || my >= y + SWATCH) return -1;
    for(int i = 0; i < n; i++){
        int sx = x + i * (SWATCH + SWATCH_GAP);
        if(mx >= sx && mx < sx + SWATCH) return i;
    }
    return -1;
}

// ── on_show: (re)load every persisted value with sane clamps ────────────────
static void personalize_on_show(){
    s_accent     = UIConfig::Color("theme.accent", 0xFF3498DB);

    const char* pos = UIConfig::Str("taskbar.position", "bottom");
    s_bar_top    = (pos && (pos[0] == 't' || pos[0] == 'T'));
    s_bar_height = UIConfig::Int("taskbar.height", 44);
    if(s_bar_height < 32) s_bar_height = 32;
    if(s_bar_height > 72) s_bar_height = 72;
    s_bar_bg     = UIConfig::Color("taskbar.bg", 0xFF0C0C14);

    s_icon_size  = UIConfig::Int("desktop.icon_size", 56);
    if(s_icon_size < 32) s_icon_size = 32;
    if(s_icon_size > 96) s_icon_size = 96;
    s_wall       = UIConfig::Color("desktop.bg", 0xFF0C0818);
    s_wall_img   = UIConfig::Int("desktop.wallpaper_index", 0);
    if(s_wall_img < 0) s_wall_img = 0;
    if(s_wall_img > 1) s_wall_img = 1;

    s_anim       = UIConfig::Bool("compositor.window_animations", true);
    s_anim_ms    = UIConfig::Int("compositor.animation_speed_ms", 90);
    if(s_anim_ms < 0)   s_anim_ms = 0;
    if(s_anim_ms > 400) s_anim_ms = 400;
    s_shadow     = UIConfig::Bool("compositor.shadow_enabled", true);
    s_shadow_drag= UIConfig::Bool("compositor.shadow_during_drag", false);
    s_corner     = UIConfig::Int("window.corner_radius", 10);
    if(s_corner < 0)  s_corner = 0;
    if(s_corner > 20) s_corner = 20;

    s_font_scale = UIConfig::Int("display.font_scale", 1);
    if(s_font_scale < 1) s_font_scale = 1;
    if(s_font_scale > 3) s_font_scale = 3;
    // re-assert the persisted scale onto the live renderer's source of truth on
    // open, so a value saved last session is actually in effect. (satoru)
    SettingsApp::state.font_scale = s_font_scale;
}

// ── render: keep a running y; controls sit at pane-relative x = CTRL_X. ──────
static void personalize_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int cx = x + CTRL_X;
    int ly = y - scroll + 8;
    char buf[48];

    // ── accent colour ────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Accent Color");
    ly += 26;
    Graphics::DrawString(x, ly + 6, "Theme accent:", SettingsUI::COL_TEXT, 0xFF000000);
    draw_swatches(cx, ly, kAccent, kAccentCount, s_accent);
    ly += 38;
    // live hex readout of the active accent. (satoru)
    {
        static const char* hexd = "0123456789ABCDEF";
        buf[0]='#';
        for(int i = 0; i < 6; i++) buf[1+i] = hexd[(s_accent >> ((5 - i) * 4)) & 0xF];
        buf[7] = 0;
        SettingsUI::Row(x, ly, "Selected:", buf);
    }
    ly += 30;

    // ── taskbar ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Taskbar");
    ly += 26;
    Graphics::DrawString(x, ly + 4, "Position:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(cx, ly, DROP_W, s_bar_top ? "Top" : "Bottom");
    ly += 30;
    Graphics::DrawString(x, ly + 2, "Height:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        // map 32..72 px onto a 0..100 slider percentage. (satoru)
        int pct = ((s_bar_height - 32) * 100) / 40;
        SettingsUI::Slider(cx, ly, SLIDER_W, pct);
        SettingsUI::IntToStr(s_bar_height, buf, 48); SettingsUI::StrApp(buf, " px", 48);
        Graphics::DrawString(cx + SLIDER_W + 12, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    }
    ly += 32;
    Graphics::DrawString(x, ly + 6, "Background:", SettingsUI::COL_TEXT, 0xFF000000);
    draw_swatches(cx, ly, kBars, kBarsCount, s_bar_bg);
    ly += 40;

    // ── desktop ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Desktop");
    ly += 26;
    Graphics::DrawString(x, ly + 2, "Icon size:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        int pct = ((s_icon_size - 32) * 100) / 64;   // 32..96 (satoru)
        SettingsUI::Slider(cx, ly, SLIDER_W, pct);
        SettingsUI::IntToStr(s_icon_size, buf, 48); SettingsUI::StrApp(buf, " px", 48);
        Graphics::DrawString(cx + SLIDER_W + 12, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    }
    ly += 32;
    Graphics::DrawString(x, ly + 4, "Wallpaper:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        int wi = palette_index(kWalls, kWallsCount, s_wall);
        SettingsUI::Dropdown(cx, ly, DROP_W, wi >= 0 ? kWallNames[wi] : "Custom");
    }
    ly += 30;
    // a live tint preview swatch beside the wallpaper choice. (satoru)
    Graphics::DrawString(x, ly + 4, "Tint preview:", SettingsUI::COL_TEXT, 0xFF000000);
    Graphics::FillRoundedRect(cx, ly, 60, 18, 5, s_wall);
    Graphics::DrawRect(cx, ly, 60, 18, SettingsUI::COL_BORDER);
    ly += 30;
    // builtin image wallpaper picker (cycles the embedded wallpapers). (satoru)
    Graphics::DrawString(x, ly + 4, "Background:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(cx, ly, DROP_W, s_wall_img == 1 ? "Wallpaper 2" : "Wallpaper 1");
    ly += 30;

    // ── window effects ───────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Window Effects");
    ly += 26;
    Graphics::DrawString(x, ly + 2, "Animations:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(cx, ly, s_anim);
    ly += 32;
    Graphics::DrawString(x, ly + 2, "Anim speed:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        int pct = (s_anim_ms * 100) / 400;            // 0..400 ms (satoru)
        SettingsUI::Slider(cx, ly, SLIDER_W, pct);
        SettingsUI::IntToStr(s_anim_ms, buf, 48); SettingsUI::StrApp(buf, " ms", 48);
        Graphics::DrawString(cx + SLIDER_W + 12, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    }
    ly += 32;
    Graphics::DrawString(x, ly + 2, "Window shadows:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(cx, ly, s_shadow);
    ly += 32;
    Graphics::DrawString(x, ly + 2, "Shadow on drag:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(cx, ly, s_shadow_drag);
    ly += 32;
    Graphics::DrawString(x, ly + 2, "Corner radius:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        int pct = (s_corner * 100) / 20;              // 0..20 px (satoru)
        SettingsUI::Slider(cx, ly, SLIDER_W, pct);
        SettingsUI::IntToStr(s_corner, buf, 48); SettingsUI::StrApp(buf, " px", 48);
        Graphics::DrawString(cx + SLIDER_W + 12, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    }
    ly += 34;

    // ── text ─────────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Text");
    ly += 26;
    Graphics::DrawString(x, ly + 4, "UI font scale:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        SettingsUI::IntToStr(s_font_scale, buf, 48); SettingsUI::StrApp(buf, "x", 48);
        SettingsUI::Dropdown(cx, ly, DROP_W, buf);
    }
    ly += 30;
    Graphics::DrawString(x, ly + 2,
        "Applies to apps that honour display.font_scale.",
        SettingsUI::COL_DIM, 0xFF000000);
    ly += 24;
}

// ── input: pane-local mx,my. walk the SAME running y the render used, but
//    starting at (-scroll + 8) so the hit rects line up exactly. (satoru)
static bool personalize_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int cx = CTRL_X;
    int ly = -scroll + 8;

    // accent header + swatch row. (satoru)
    ly += 26;
    {
        int i = swatch_hit(cx, ly, kAccentCount, mx, my);
        if(i >= 0){ persist_accent(kAccent[i]); return true; }
    }
    ly += 38;
    ly += 30;          // selected-hex row (satoru)

    // taskbar header. (satoru)
    ly += 26;
    {
        int hit = SettingsUI::DropdownHit(cx, ly, DROP_W, mx, my);
        if(hit >= 0){ s_bar_top = !s_bar_top; persist_bar_position(); return true; }
    }
    ly += 30;          // position row (satoru)
    {
        int p = SettingsUI::SliderHit(cx, ly, SLIDER_W, mx, my);
        if(p >= 0){ s_bar_height = 32 + (p * 40) / 100; persist_bar_height(); return true; }
    }
    ly += 32;          // height row (satoru)
    {
        int i = swatch_hit(cx, ly, kBarsCount, mx, my);
        if(i >= 0){ persist_bar_bg(kBars[i]); return true; }
    }
    ly += 40;          // background swatch row (satoru)

    // desktop header. (satoru)
    ly += 26;
    {
        int p = SettingsUI::SliderHit(cx, ly, SLIDER_W, mx, my);
        if(p >= 0){ s_icon_size = 32 + (p * 64) / 100; persist_icon_size(); return true; }
    }
    ly += 32;          // icon-size row (satoru)
    {
        int hit = SettingsUI::DropdownHit(cx, ly, DROP_W, mx, my);
        if(hit >= 0){
            int wi = palette_index(kWalls, kWallsCount, s_wall);
            if(wi < 0) wi = 0;
            wi += (hit == 0) ? -1 : 1;
            if(wi < 0) wi = kWallsCount - 1;
            if(wi >= kWallsCount) wi = 0;
            persist_wallpaper(kWalls[wi]);
            return true;
        }
    }
    ly += 30;          // wallpaper row (satoru)
    ly += 30;          // tint-preview row (satoru)
    {
        int hit = SettingsUI::DropdownHit(cx, ly, DROP_W, mx, my);
        if(hit >= 0){ s_wall_img = s_wall_img ? 0 : 1; persist_wall_img(); return true; }
    }
    ly += 30;          // background image row (satoru)

    // window effects header. (satoru)
    ly += 26;
    if(SettingsUI::ToggleHit(cx, ly, mx, my)){ s_anim = !s_anim; persist_anim(); return true; }
    ly += 32;          // animations row (satoru)
    {
        int p = SettingsUI::SliderHit(cx, ly, SLIDER_W, mx, my);
        if(p >= 0){ s_anim_ms = (p * 400) / 100; persist_anim_ms(); return true; }
    }
    ly += 32;          // anim-speed row (satoru)
    if(SettingsUI::ToggleHit(cx, ly, mx, my)){ s_shadow = !s_shadow; persist_shadow(); return true; }
    ly += 32;          // shadows row (satoru)
    if(SettingsUI::ToggleHit(cx, ly, mx, my)){ s_shadow_drag = !s_shadow_drag; persist_shadow_drag(); return true; }
    ly += 32;          // shadow-on-drag row (satoru)
    {
        int p = SettingsUI::SliderHit(cx, ly, SLIDER_W, mx, my);
        if(p >= 0){ s_corner = (p * 20) / 100; persist_corner(); return true; }
    }
    ly += 34;          // corner-radius row (satoru)

    // text header. (satoru)
    ly += 26;
    {
        int hit = SettingsUI::DropdownHit(cx, ly, DROP_W, mx, my);
        if(hit >= 0){
            s_font_scale += (hit == 0) ? -1 : 1;
            if(s_font_scale < 1) s_font_scale = 1;
            if(s_font_scale > 3) s_font_scale = 3;
            persist_font_scale();
            return true;
        }
    }
    return false;
}

// total content height for the scrollbar (sum of row advances + tail). (satoru)
static int personalize_content_height(){
    return 8
         + 26 + 38 + 30                     // accent (satoru)
         + 26 + 30 + 32 + 40                // taskbar (satoru)
         + 26 + 32 + 30 + 30                // desktop (satoru)
         + 26 + 32 + 32 + 32 + 32 + 34      // window effects (satoru)
         + 26 + 30 + 24                     // text (satoru)
         + 16;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_personalize_module;` resolves at link time. (satoru)
extern const SettingsModule g_personalize_module = {
    "personalize", "Personalization", "\x0f",
    personalize_on_show, personalize_render, personalize_input, personalize_content_height
};
// end (satoru)
