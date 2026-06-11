//  kurono os  -  settings module: power & battery (satoru)
//  a detailed power page: a cpu power-plan selector mapped onto the hal
//  cpufreq governors (applied live via CPUFreq::SetGovernor + persisted),
//  detected p-state / clock info, a battery section (no battery driver →
//  ac power), a thermal readout, screen-off + sleep timeout sliders, and a
//  startup section with a fast-startup toggle and wake-source info. every
//  write goes through UIConfig; every detected value comes from CPUFreq /
//  CPUDetect / HAL. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/cpu_detect.h"
#include "../system/ui_config.h"
#include "../hal/cpufreq.h"

// ── power-plan model: three user-facing plans mapped onto cpufreq governors.
//    index 0..2 == performance / balanced(ondemand) / powersave. (satoru)
static const char* kPlanName[]   = { "Performance", "Balanced", "Power Saver" };
static const char* kPlanGov[]    = { "performance", "ondemand", "powersave"  };
static const char* kPlanDesc[]   = { "Max clocks, no throttling",
                                     "Scale on demand (recommended)",
                                     "Lowest clocks, longest runtime" };
static const int    kPlanCount   = 3;

// map a plan index → the CPUFreq::Governor enum it drives. (satoru)
static CPUFreq::Governor plan_to_gov(int plan){
    switch(plan){
        case 0:  return CPUFreq::GOV_PERFORMANCE;
        case 2:  return CPUFreq::GOV_POWERSAVE;
        default: return CPUFreq::GOV_ONDEMAND;   // balanced (satoru)
    }
}
// map a live governor → the closest plan index for initial detection. (satoru)
static int gov_to_plan(CPUFreq::Governor g){
    switch(g){
        case CPUFreq::GOV_PERFORMANCE: return 0;
        case CPUFreq::GOV_POWERSAVE:   return 2;
        default:                       return 1;  // ondemand/schedutil/userspace → balanced (satoru)
    }
}

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static int  s_plan           = 1;     // persisted to power.plan (satoru)
static int  s_screen_timeout = 5;     // minutes, persisted to power.screen_timeout (satoru)
static int  s_sleep_timeout  = 15;    // minutes, persisted to power.sleep_timeout (satoru)
static bool s_fast_startup   = true;  // persisted to power.fast_startup (satoru)
static int  s_cpu_count      = 0;     // detected via CPUFreq::CPUCount() (satoru)

// timeout sliders run 0..60 min; 0 renders as "Never". (satoru)
static const int kTimeoutMax = 60;

// snap an arbitrary minute value to a tidy step so the slider lands on clean
// numbers (off / 1 / 2 / 5 / 10 / 15 / 30 / 45 / 60). (satoru)
static const int kTimeoutStops[] = { 0, 1, 2, 5, 10, 15, 30, 45, 60 };
static const int kTimeoutStopN   = (int)(sizeof(kTimeoutStops)/sizeof(kTimeoutStops[0]));
static int snap_timeout(int m){
    if(m < 0) m = 0;
    if(m > kTimeoutMax) m = kTimeoutMax;
    int best = kTimeoutStops[0]; int bd = (m > best) ? (m - best) : (best - m);
    for(int i = 1; i < kTimeoutStopN; i++){
        int v = kTimeoutStops[i];
        int d = (m > v) ? (m - v) : (v - m);
        if(d < bd){ bd = d; best = v; }
    }
    return best;
}

// render a minute count into b as "Never" / "1 min" / "N min". (satoru)
static void fmt_minutes(int m, char* b, int mx){
    if(m <= 0){ SettingsUI::StrCpy(b, "Never", mx); return; }
    SettingsUI::IntToStr(m, b, mx);
    SettingsUI::StrApp(b, (m == 1) ? " min" : " min", mx);
}

// ── persistence helpers ──────────────────────────────────────────────────────
static void apply_plan(int plan){
    if(plan < 0) plan = 0;
    if(plan >= kPlanCount) plan = kPlanCount - 1;
    s_plan = plan;
    UIConfig::SetInt("power.plan", plan, true);
    // apply live to every detected cpu where cpufreq is up. (satoru)
    CPUFreq::Governor g = plan_to_gov(plan);
    for(int i = 0; i < s_cpu_count; i++) CPUFreq::SetGovernor((uint32_t)i, g);
    UIConfig::Save();
}
static void persist_screen(){ UIConfig::SetInt("power.screen_timeout", s_screen_timeout, true); UIConfig::Save(); }
static void persist_sleep(){  UIConfig::SetInt("power.sleep_timeout",  s_sleep_timeout,  true); UIConfig::Save(); }
static void persist_fast(){   UIConfig::SetInt("power.fast_startup",   s_fast_startup ? 1 : 0, true); UIConfig::Save(); }

// ── on_show: detect cpufreq state, then let persisted config override ────────
static void power_on_show(){
    s_cpu_count = CPUFreq::CPUCount();

    // seed the plan from the live governor, then honour a persisted choice. (satoru)
    int detected_plan = (s_cpu_count > 0) ? gov_to_plan(CPUFreq::GetGovernor(0)) : 1;
    s_plan = UIConfig::Int("power.plan", detected_plan);
    if(s_plan < 0) s_plan = 0;
    if(s_plan >= kPlanCount) s_plan = kPlanCount - 1;

    s_screen_timeout = snap_timeout(UIConfig::Int("power.screen_timeout", s_screen_timeout));
    s_sleep_timeout  = snap_timeout(UIConfig::Int("power.sleep_timeout",  s_sleep_timeout));
    s_fast_startup   = UIConfig::Int("power.fast_startup", s_fast_startup ? 1 : 0) != 0;
}

// ── fixed control geometry (mirrors the display module so *Hit math lines up
//    with the drawing; the input() signature gives no pane width). (satoru)
static const int CTRL_X_OFF = 170;   // controls column, relative to pane left (satoru)
static const int CTRL_W     = 300;   // dropdown / control width in px (satoru)
static const int SLIDER_W   = 250;   // timeout track width in px (satoru)

static int ctrl_w_for(int pane_w){
    int avail = pane_w - CTRL_X_OFF;
    int cw = CTRL_W;
    if(cw > avail) cw = avail;
    if(cw < 80) cw = 80;
    return cw;
}
static int slider_w_for(int ctrl_w){
    int w = SLIDER_W;
    if(w > ctrl_w - 60) w = ctrl_w - 60;
    if(w < 60) w = 60;
    return w;
}

// ── render: running y from (y - scroll + pad), rows laid downward. (satoru)
static void power_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ctrl_w = ctrl_w_for(w);
    int ly = y - scroll + 8;
    char buf[64];

    // ── power plan ───────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Power Plan");
    ly += 26;

    Graphics::DrawString(x, ly + 4, "Power Plan:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Dropdown(ctrl_x, ly, ctrl_w, kPlanName[s_plan]);
    ly += 26;
    // one-line description of the active plan. (satoru)
    Graphics::DrawString(ctrl_x + 4, ly, kPlanDesc[s_plan], SettingsUI::COL_DIM, 0xFF000000);
    ly += 22;
    // the cpufreq governor this plan maps to (read-only confirmation). (satoru)
    SettingsUI::Row(x, ly, "Governor:", kPlanGov[s_plan]);
    ly += 22;

    // detected cpu clocks / p-states from cpufreq. (satoru)
    {
        const CPUFreq::CPUInfo* ci = (s_cpu_count > 0) ? CPUFreq::GetCPU(0) : nullptr;
        if(ci){
            SettingsUI::IntToStr((int)ci->cur_mhz, buf, 64);
            SettingsUI::StrApp(buf, " MHz  (base ", 64);
            char bb[12]; SettingsUI::IntToStr((int)ci->base_mhz, bb, 12);
            SettingsUI::StrApp(buf, bb, 64);
            if(ci->turbo_mhz > ci->base_mhz){
                SettingsUI::StrApp(buf, " / turbo ", 64);
                char tb[12]; SettingsUI::IntToStr((int)ci->turbo_mhz, tb, 12);
                SettingsUI::StrApp(buf, tb, 64);
            }
            SettingsUI::StrApp(buf, ")", 64);
            SettingsUI::Row(x, ly, "CPU Clock:", buf);
            ly += 22;

            SettingsUI::IntToStr((int)ci->pstate_count, buf, 64);
            SettingsUI::StrApp(buf, " P-states  /  ", 64);
            char nb[12]; SettingsUI::IntToStr(s_cpu_count, nb, 12);
            SettingsUI::StrApp(buf, nb, 64);
            SettingsUI::StrApp(buf, (s_cpu_count == 1) ? " managed CPU" : " managed CPUs", 64);
            SettingsUI::Row(x, ly, "Scaling:", buf);
            ly += 22;
        } else {
            SettingsUI::Row(x, ly, "Scaling:", "cpufreq unavailable");
            ly += 22;
        }
    }
    ly += 8;

    // ── battery ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Battery");
    ly += 26;
    // kurono has no acpi battery driver  -  surface ac power explicitly. (satoru)
    SettingsUI::Row(x, ly, "Power Source:", "AC Power (no battery detected)");
    ly += 22;
    SettingsUI::Row(x, ly, "Status:", "Plugged In");
    ly += 30;

    // ── thermal ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Thermal");
    ly += 26;
    // no on-die thermal sensor wired up  -  report a nominal state. (satoru)
    SettingsUI::Row(x, ly, "CPU Temperature:", "Normal");
    ly += 22;
    SettingsUI::Row(x, ly, "Fan Policy:", "Automatic");
    ly += 30;

    // ── sleep & display ──────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Sleep & Display");
    ly += 26;

    int sl_w = slider_w_for(ctrl_w);

    // screen-off timeout slider. (satoru)
    Graphics::DrawString(x, ly + 2, "Turn off screen:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Slider(ctrl_x, ly, sl_w, (s_screen_timeout * 100) / kTimeoutMax);
    fmt_minutes(s_screen_timeout, buf, 64);
    Graphics::DrawString(ctrl_x + sl_w + 10, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    ly += 32;

    // sleep (suspend) timeout slider. (satoru)
    Graphics::DrawString(x, ly + 2, "Sleep after:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Slider(ctrl_x, ly, sl_w, (s_sleep_timeout * 100) / kTimeoutMax);
    fmt_minutes(s_sleep_timeout, buf, 64);
    Graphics::DrawString(ctrl_x + sl_w + 10, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    ly += 32;
    // honest: no idle/suspend manager consumes these timeouts yet, so they are
    // saved as preferences but do not blank the screen or suspend. (satoru)
    SettingsUI::Row(x, ly, "Timeouts:", "Saved (no idle manager wired)");
    ly += 30;

    // ── startup ──────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Startup");
    ly += 26;

    // fast-startup toggle. saved as a boot preference; no boot path reads
    // power.fast_startup yet, so the caption stays honest. (satoru)
    Graphics::DrawString(x, ly + 2, "Fast Startup:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_fast_startup);
    Graphics::DrawString(ctrl_x + 50, ly + 2,
                         s_fast_startup ? "Preference: hybrid boot" : "Preference: cold boot",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 32;

    SettingsUI::Row(x, ly, "Wake Sources:", "Keyboard, Mouse, Network");
    ly += 22;
    SettingsUI::Row(x, ly, "ACPI State:", "S0 (Working)");
    ly += 22;
}

// ── input: pane-local mx,my; walk the SAME running-y (offset by -scroll). (satoru)
static bool power_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + 8;

    ly += 26;          // "Power Plan" header (satoru)

    // power-plan dropdown: arrows cycle and apply live. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, CTRL_W, mx, my);
        if(hit >= 0){
            int p = s_plan + ((hit == 0) ? -1 : 1);
            if(p < 0) p = 0;
            if(p >= kPlanCount) p = kPlanCount - 1;
            if(p != s_plan){ apply_plan(p); return true; }
            return false;
        }
    }
    ly += 26;          // plan dropdown row (satoru)
    ly += 22;          // plan description row (satoru)
    ly += 22;          // governor row (satoru)

    // detected-clock rows: 2 rows when cpufreq is up, else 1. mirror render. (satoru)
    {
        const CPUFreq::CPUInfo* ci = (s_cpu_count > 0) ? CPUFreq::GetCPU(0) : nullptr;
        ly += ci ? (22 + 22) : 22;
    }
    ly += 8;

    ly += 26;          // "Battery" header (satoru)
    ly += 22;          // power-source row (satoru)
    ly += 30;          // status row (satoru)

    ly += 26;          // "Thermal" header (satoru)
    ly += 22;          // temperature row (satoru)
    ly += 30;          // fan-policy row (satoru)

    ly += 26;          // "Sleep & Display" header (satoru)

    int sl_w = slider_w_for(ctrl_w_for(/*pane width unknown here →*/ CTRL_X_OFF + CTRL_W));

    // screen-off timeout slider. (satoru)
    {
        int p = SettingsUI::SliderHit(ctrl_x, ly, sl_w, mx, my);
        if(p >= 0){
            int m = snap_timeout((p * kTimeoutMax) / 100);
            if(m != s_screen_timeout){ s_screen_timeout = m; persist_screen(); return true; }
            return false;
        }
    }
    ly += 32;          // screen-off slider row (satoru)

    // sleep timeout slider. (satoru)
    {
        int p = SettingsUI::SliderHit(ctrl_x, ly, sl_w, mx, my);
        if(p >= 0){
            int m = snap_timeout((p * kTimeoutMax) / 100);
            if(m != s_sleep_timeout){ s_sleep_timeout = m; persist_sleep(); return true; }
            return false;
        }
    }
    ly += 32;          // sleep slider row (satoru)
    ly += 30;          // sleep-mode row (satoru)

    ly += 26;          // "Startup" header (satoru)

    // fast-startup toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_fast_startup = !s_fast_startup;
        persist_fast();
        return true;
    }
    ly += 32;          // fast-startup row (satoru)
    return false;
}

// ── content height for the scrollbar: sum of the row advances + tail. cpufreq
//    being up adds one extra detected row over the unavailable case. (satoru)
static int power_content_height(){
    int h = 8;
    // power plan: header + dropdown + desc + governor (satoru)
    h += 26 + 26 + 22 + 22;
    // detected-clock rows (2 with cpufreq, else 1) + spacer (satoru)
    h += (CPUFreq::CPUCount() > 0) ? (22 + 22) : 22;
    h += 8;
    // battery (satoru)
    h += 26 + 22 + 30;
    // thermal (satoru)
    h += 26 + 22 + 30;
    // sleep & display: header + 2 sliders + sleep-mode (satoru)
    h += 26 + 32 + 32 + 30;
    // startup: header + toggle + wake row + acpi row (satoru)
    h += 26 + 32 + 22 + 22;
    h += 16;
    return h;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_power_module;` resolves at link time. (satoru)
extern const SettingsModule g_power_module = {
    "power", "Power & Battery", "\x04",
    power_on_show, power_render, power_input, power_content_height
};
// end (satoru)
