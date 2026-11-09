//  kurono os - settings module: accessibility (satoru)
//  high contrast, reduce motion (also calms this app), colour filters, the
//  screen reader (announces focused windows), and the keyboard assists. every
//  control persists through UIConfig and is applied live; the boot path re-reads
//  the same keys in kurono_kernel.cpp. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/keyboard.h"
#include "../ui/ui_elements.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../system/ui_config.h"

using namespace SettingsUI;

static bool s_contrast = false, s_motion = false, s_reader = false;
static bool s_sticky = false, s_slow = false, s_bounce = false;
static int  s_filter = 0;
static const char* kFilters[] = { "None", "Red-green (protanopia)", "Red-green (deuteranopia)", "Blue-yellow (tritanopia)", "Grayscale" };

static void a11y_on_show(){
    s_contrast = UIConfig::Int("a11y.high_contrast", 0) != 0;
    s_motion   = UIConfig::Int("compositor.reduced_motion", 0) != 0;
    s_sticky   = UIConfig::Int("a11y.sticky_keys", 0) != 0;
    s_slow     = UIConfig::Int("a11y.slow_keys", 0) != 0;
    s_bounce   = UIConfig::Int("a11y.bounce_keys", 0) != 0;
    s_reader   = UIConfig::Int("a11y.screen_reader", 0) != 0;
    s_filter   = UIConfig::Int("a11y.color_filter", 0);
    if(s_filter < 0 || s_filter > 4) s_filter = 0;
}

static void persist_apply(){
    UIConfig::SetInt("a11y.high_contrast",        s_contrast ? 1 : 0, true);
    UIConfig::SetInt("compositor.reduced_motion", s_motion   ? 1 : 0, true);
    UIConfig::SetInt("a11y.sticky_keys",          s_sticky   ? 1 : 0, true);
    UIConfig::SetInt("a11y.slow_keys",            s_slow     ? 1 : 0, true);
    UIConfig::SetInt("a11y.bounce_keys",          s_bounce   ? 1 : 0, true);
    UIConfig::SetInt("a11y.screen_reader",        s_reader   ? 1 : 0, true);
    UIConfig::SetInt("a11y.color_filter",         s_filter,           true);
    UIConfig::Save();
    Graphics::SetColorFilter(s_filter);
    Graphics::SetHighContrast(s_contrast);
    Widget::high_contrast_mode = s_contrast;
    Keyboard::SetStickyKeys(s_sticky);
    Keyboard::SetSlowKeys(s_slow ? 250 : 0);
    Keyboard::SetBounceKeys(s_bounce ? 200 : 0);
    Keyboard::SetScreenReader(s_reader);
    WindowManager::ReloadFromConfig();
    Desktop::ReloadFromConfig();
    Taskbar::ReloadFromConfig();
    Graphics::MarkUIDirty();
}

static void a11y_build(){
    bool ch = false;
    Group("Vision");
    ch |= Toggle("Increase contrast", &s_contrast, "Stronger borders and text");
    ch |= Toggle("Reduce motion", &s_motion, "Turns off window and interface animations");
    ch |= Dropdown("Color filter", kFilters, 5, &s_filter);
    ch |= Toggle("Screen reader", &s_reader, "Announces the focused window with a sound");
    EndGroup();

    Group("Keyboard");
    ch |= Toggle("Sticky keys", &s_sticky, "Press shortcut keys one at a time");
    ch |= Toggle("Slow keys", &s_slow, "A key must be held for 250 ms to register");
    ch |= Toggle("Bounce keys", &s_bounce, "Ignore repeated presses within 200 ms");
    EndGroup();

    if(ch) persist_apply();
}

extern const SettingsModule g_a11y_module = {
    "a11y", "Accessibility", "", a11y_on_show, nullptr, nullptr, nullptr, a11y_build
};
// end (satoru)
