//  kurono os - settings shell: window, sidebar with search, page header, smooth
//  scrolling content pane with spring page transitions, keyboard navigation,
//  module registry + dispatch, and boot-time restore of persisted settings. the
//  controls themselves live in settings_kit.cpp. (satoru)
#include "system_settings.h"
#include "settings_kit.h"
#include "../ui/window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/audio.h"
#include "../drivers/audio_mixer.h"
#include "../ui/font.h"
#include "../ui/desktop.h"
#include "../hal/cpufreq.h"
#include "../system/ui_config.h"
#include "../system/logging.h"
#include "../virt/vm_host.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
#include "../kui/kui_kernel.h"   // ring-3 routing behind kurono.r3apps (satoru)
#include "../fs/kvfs.h"          // the page handoff file (satoru)
#endif

using namespace SKit;

// ── module registry ─────────────────────────────────────────────────────────
// each module defines `extern const SettingsModule g_<x>_module` in its own
// .cpp; the shell lists pointers here in sidebar order. kGroupBreak marks the
// first module of each sidebar group. (satoru)
extern const SettingsModule g_display_module;
extern const SettingsModule g_about_module;
extern const SettingsModule g_audio_module;
extern const SettingsModule g_personalize_module;
extern const SettingsModule g_network_module;
extern const SettingsModule g_power_module;
extern const SettingsModule g_storage_module;
extern const SettingsModule g_devices_module;
extern const SettingsModule g_a11y_module;
extern const SettingsModule g_security_module;
extern const SettingsModule g_system_module;
extern const SettingsModule g_linux_module;   // (satoru)
extern const SettingsModule g_language_module;   // (satoru)

static const SettingsModule* g_modules[] = {
    &g_network_module,
    &g_display_module,
    &g_audio_module,
    &g_power_module,
    &g_personalize_module,
    &g_a11y_module,
    &g_language_module,
    &g_security_module,
    &g_storage_module,
    &g_devices_module,
    &g_system_module,
    &g_linux_module,
    &g_about_module,
};
static const int g_module_count = (int)(sizeof(g_modules) / sizeof(g_modules[0]));
static const bool kGroupBreak[16] = { false, true, false, false, true, false, false, true, false, false, true, false, false };

static int s_eq(const char* a, const char* b){
    if(!a || !b) return 0;
    while(*a && *a == *b){ a++; b++; }
    return *a == 0 && *b == 0;
}

// sidebar icon + search keywords per module id. (satoru)
struct ModMeta { const char* id; int icon; const char* keywords; };
static const ModMeta kMeta[] = {
    { "network",     IC_NETWORK,    "wifi wi-fi wireless ethernet internet ip address dns gateway mac bluetooth" },
    { "display",     IC_DISPLAY,    "screen monitor displays resolution refresh rate hz brightness vsync" },
    { "audio",       IC_SOUND,      "sound volume speaker output mute audio test" },
    { "power",       IC_POWER,      "battery energy cpu performance balanced power saver governor" },
    { "personalize", IC_APPEARANCE, "appearance theme dark light mode accent colour color wallpaper background desktop icons animations shadows style dock taskbar interface size scale" },
    { "a11y",        IC_A11Y,       "accessibility contrast reduce motion sticky slow bounce keys color filter screen reader" },
    { "language",    IC_GLOBE,      "language region keyboard layout input source qwerty azerty qwertz clock 24 hour date format week" },
    { "security",    IC_SECURITY,   "users account accounts password privacy sign in login administrator" },
    { "storage",     IC_STORAGE,    "disk disks drive drives memory ram space nvme sata files persistence" },
    { "devices",     IC_DEVICES,    "hardware device manager usb gpu graphics pci drivers keyboard mouse" },
    { "system",      IC_SYSTEM,     "software update updates linux subsystem packages memory compression kmemx" },
    { "linux",       IC_LINUX,      "linux alpine debian virtual machine vm guest distro distribution terminal apk start stop restart" },
    { "about",       IC_ABOUT,      "about version info hardware cpu processor kernel hostname" },
};
static const ModMeta* meta_for(const SettingsModule* m){
    for(unsigned i = 0; i < sizeof(kMeta) / sizeof(kMeta[0]); i++) if(s_eq(kMeta[i].id, m->id)) return &kMeta[i];
    return nullptr;
}

// ── layout constants ────────────────────────────────────────────────────────
static const int SB_W_FULL   = 236;
static const int SB_W_COMPACT= 64;
static const int SB_TOP      = 60;    // first sidebar item below the search field (satoru)
static const int SB_ITEM     = 34;
static const int SB_GAP      = 12;
static const int HEADER_H    = 62;
static const int COL_MAX     = 680;
static const int LEGACY_PAD  = 28;

// ── shell state (constant-initialised statics, ctor-free) ───────────────────
static int   s_win_id = -1;
static int   s_selected = 0;
static int   s_scroll_t[16];          // scroll target per module (satoru)
static float s_scroll_v = 0;          // the drawn scroll offset of the active page (satoru)
static int   s_content_h = 0, s_view_h = 0;
static int   s_focus_count = 0;
static bool  s_scroll_to_focus = false;
static bool  s_scroll_to_hit = false;
static uint32_t s_scroll_ms = 0;
static bool  s_bar_drag = false;
static int   s_bar_grab = 0;
static int   s_hover_item = -1;
static bool  s_down = false;
static int   s_lmx = -1, s_lmy = -1;  // pointer, content-local (satoru)
static bool  s_ptr_in_pop = false;

// search (satoru)
static char  s_query[40];
static bool  s_search_focus = false;
static uint32_t s_search_ms = 0;
static bool  s_hit[16];
static bool  s_any_hit = true;

// geometry of the last frame (screen coords) for the input passes (satoru)
struct Geo { int cx, cy, cw, ch, sb_w, bx, bw, view_y, view_h, col_x, col_w; bool legacy; };
static Geo s_geo;

// raw key queue fed from the input manager (another kernel proc) (satoru)
static volatile int s_kq[32];
static volatile uint32_t s_kq_head = 0, s_kq_tail = 0;

static bool anim_on(){
    return UIConfig::Bool("compositor.window_animations", true) &&
           !UIConfig::Bool("compositor.reduced_motion", false);
}
static uint32_t scroll_id(int page){ return 0x5C011000u + (uint32_t)page; }

// ── search ──────────────────────────────────────────────────────────────────
static void run_search(){
    s_any_hit = false;
    s_scroll_to_hit = s_query[0] != 0;
    for(int i = 0; i < g_module_count; i++){
        const SettingsModule* m = g_modules[i];
        bool hit = !s_query[0] || Match(m->title, s_query);
        const ModMeta* mm = meta_for(m);
        if(!hit && mm) hit = Match(mm->keywords, s_query);
        if(!hit && m->build){
            int saved_page = C.page;
            const char* sq = C.query;
            C.page = i; C.query = s_query;
            C.col_x = 0; C.col_w = 600; C.origin_y = 0; C.view_y = 0; C.view_h = 0;
            C.pointer_ok = false;
            BeginPass(P_SEARCH);
            m->build();
            EndPass();
            hit = C.search_hit;
            C.page = saved_page; C.query = sq;
        }
        s_hit[i] = hit;
        if(hit) s_any_hit = true;
    }
}

// ── page switching ──────────────────────────────────────────────────────────
static void show_module(int idx){
    if(idx < 0 || idx >= g_module_count) return;
    int old = s_selected;
    s_selected = idx;
    C.dir = idx >= old ? 1 : -1;
    C.page = idx;
    C.page_ms = Timer::GetRealMs();
    C.focus = -1;
    C.active = 0;
    C.edit_id = 0;
    ClosePopover();
    s_bar_drag = false;
    const SettingsModule* m = g_modules[idx];
    if(m->on_show) m->on_show();
    Graphics::MarkUIDirty();
}

static int module_index(const char* id){
    for(int i = 0; i < g_module_count; i++) if(s_eq(g_modules[i]->id, id)) return i;
    return -1;
}

// ── event passes against the last frame's geometry ──────────────────────────
static void setup_ctx_from_geo(){
    C.col_x = s_geo.col_x; C.col_w = s_geo.col_w;
    C.view_x = s_geo.bx; C.view_y = s_geo.view_y; C.view_w = s_geo.bw; C.view_h = s_geo.view_h;
    C.origin_y = s_geo.view_y - (int)(s_scroll_v + 0.5f);
    C.page = s_selected;
    C.query = s_query[0] ? s_query : nullptr;
}

static bool event_pass(Ev ev, int sx, int sy, int key){
    const SettingsModule* m = g_modules[s_selected];
    if(!m->build) return false;
    setup_ctx_from_geo();
    C.ev = ev; C.ev_x = sx; C.ev_y = sy; C.ev_key = key;
    C.pointer_ok = false;
    BeginPass(P_EVENT);
    m->build();
    EndPass();
    bool changed = C.changed;
    C.ev = EV_NONE;
    if(C.goto_id[0]){
        int gi = module_index(C.goto_id);
        C.goto_id[0] = 0;
        if(gi >= 0) show_module(gi);
    }
    Graphics::MarkUIDirty();
    return changed || C.consumed;
}

static void clamp_scroll_target(){
    int maxsc = s_content_h - s_view_h;
    if(maxsc < 0) maxsc = 0;
    int& t = s_scroll_t[s_selected & 15];
    if(t < 0) t = 0;
    if(t > maxsc) t = maxsc;
}

// ── keyboard ────────────────────────────────────────────────────────────────
static void handle_key(int k){
    const SettingsModule* m = g_modules[s_selected];
    C.now = Timer::GetRealMs();
    // popover first (satoru)
    if(C.pop.open){
        PopoverEvent(EV_KEY, 0, 0, k);
        if(C.pick_id){ event_pass(EV_PICK, 0, 0, 0); C.pick_id = 0; }
        Graphics::MarkUIDirty();
        return;
    }
    // legacy page (displays): it sees esc, enter and printable keys first, as
    // the old shell gave them; only unhandled keys fall through to navigation. (satoru)
    if(!m->build && m->input && k > 0 && k < 128 && !s_search_focus){
        if(m->input(0, 0, false, (char)k, (int)(s_scroll_v + 0.5f))){ Graphics::MarkUIDirty(); return; }
    }
    // text field editing (satoru)
    if(C.edit_id && m->build && k != K_TAB && k != K_BTAB){
        event_pass(EV_KEY, 0, 0, k);
        return;
    }
    // search field (satoru)
    if(s_search_focus){
        int n = 0; while(s_query[n]) n++;
        if(k == K_ESC){ s_query[0] = 0; s_search_focus = false; run_search(); }
        else if(k == K_BS){ if(n > 0){ s_query[n - 1] = 0; run_search(); } }
        else if(k == K_ENTER || k == K_DOWN || k == K_TAB){
            s_search_focus = false;
            if(!s_hit[s_selected]) for(int i = 0; i < g_module_count; i++) if(s_hit[i]){ show_module(i); break; }
            if(k != K_ENTER){ C.focus = 0; C.focus_visible = true; s_scroll_to_focus = true; }
        }
        else if(k >= 32 && k <= 126 && n < 38){ s_query[n] = (char)k; s_query[n + 1] = 0; run_search(); }
        if(s_query[0] && s_any_hit && !s_hit[s_selected])
            for(int i = 0; i < g_module_count; i++) if(s_hit[i]){ show_module(i); break; }
        s_search_ms = C.now;
        Graphics::MarkUIDirty();
        return;
    }
    if(k == K_TAB || k == K_BTAB){
        if(s_focus_count > 0){
            if(k == K_TAB) C.focus = (C.focus + 1) % s_focus_count;
            else C.focus = C.focus <= 0 ? s_focus_count - 1 : C.focus - 1;
            C.focus_visible = true; s_scroll_to_focus = true;
        }
        C.edit_id = 0;
        Graphics::MarkUIDirty();
        return;
    }
    if(k == K_UP || k == K_DOWN){
        if(C.focus >= 0 && s_focus_count > 0 && m->build){
            int nf = C.focus + (k == K_DOWN ? 1 : -1);
            if(nf < 0){ C.focus = -1; }
            else C.focus = nf >= s_focus_count ? s_focus_count - 1 : nf;
            C.focus_visible = true; s_scroll_to_focus = true;
        } else {
            // walk the sidebar (skipping pages hidden by the search) (satoru)
            int i = s_selected;
            for(int n = 0; n < g_module_count; n++){
                i += (k == K_DOWN) ? 1 : -1;
                if(i < 0 || i >= g_module_count) break;
                if(s_hit[i]){ show_module(i); break; }
            }
        }
        Graphics::MarkUIDirty();
        return;
    }
    if(k == K_PGUP || k == K_PGDN || k == K_HOME || k == K_END){
        int& t = s_scroll_t[s_selected & 15];
        if(k == K_PGUP) t -= s_view_h - 60;
        if(k == K_PGDN) t += s_view_h - 60;
        if(k == K_HOME) t = 0;
        if(k == K_END)  t = s_content_h;
        clamp_scroll_target();
        s_scroll_ms = C.now;
        Graphics::MarkUIDirty();
        return;
    }
    if(k == K_ESC){
        if(C.focus >= 0){ C.focus = -1; }
        else if(s_query[0]){ s_query[0] = 0; run_search(); }
        Graphics::MarkUIDirty();
        return;
    }
    if(m->build){
        if(C.focus >= 0 && (k == K_ENTER || k == K_SPACE || k == K_LEFT || k == K_RIGHT)){
            event_pass(EV_KEY, 0, 0, k);
            return;
        }
        if(C.focus >= 0 && k >= 32 && k <= 126){
            // a focused text field takes typing; otherwise start a search (satoru)
            if(event_pass(EV_KEY, 0, 0, k)) return;
        }
    }
    if(k > 32 && k <= 126 && s_geo.sb_w >= SB_W_FULL){
        s_search_focus = true;
        int n = 0; while(s_query[n]) n++;
        if(n < 38){ s_query[n] = (char)k; s_query[n + 1] = 0; }
        s_search_ms = C.now;
        run_search();
        if(s_any_hit && !s_hit[s_selected])
            for(int i = 0; i < g_module_count; i++) if(s_hit[i]){ show_module(i); break; }
        Graphics::MarkUIDirty();
    }
}

static void drain_keys(){
    while(s_kq_tail != s_kq_head){
        int k = s_kq[s_kq_tail % 32];
        s_kq_tail = s_kq_tail + 1;
        handle_key(k);
    }
}

void SystemSettings::KeyEvent(int key, bool shift, bool ctrl, bool alt){
    (void)ctrl;
    if(alt) return;
    if(s_win_id < 0) return;
    Window* fw = WindowManager::GetFocusedWindow();
    if(!fw || fw->id != s_win_id) return;
    int k = 0;
    switch((Key)key){
        case KEY_UP:       k = K_UP; break;
        case KEY_DOWN:     k = K_DOWN; break;
        case KEY_LEFT:     k = K_LEFT; break;
        case KEY_RIGHT:    k = K_RIGHT; break;
        case KEY_HOME:     k = K_HOME; break;
        case KEY_END:      k = K_END; break;
        case KEY_PAGEUP:   k = K_PGUP; break;
        case KEY_PAGEDOWN: k = K_PGDN; break;
        case KEY_ESC:      k = K_ESC; break;
        case KEY_TAB:      k = shift ? K_BTAB : K_TAB; break;
        default: return;
    }
    KeyCode(k, shift);
}

void SystemSettings::KeyCode(int k, bool shift){
    if(s_win_id < 0) return;
    if(k == K_TAB && shift) k = K_BTAB;
    uint32_t h = s_kq_head;
    if(h - s_kq_tail >= 32) return;
    s_kq[h % 32] = k;
    __atomic_store_n(&s_kq_head, h + 1, __ATOMIC_RELEASE);
    Graphics::MarkUIDirty();
}

// ════════════════════════════════════════════════════════════════════════════
//  drawing
// ════════════════════════════════════════════════════════════════════════════
static void draw_sidebar(int x, int y, int sb_w, int h){
    const SettingsUI::Theme& t = SettingsUI::T();
    bool compact = sb_w < SB_W_FULL;
    C.alpha = 255; C.under = t.sidebar;
    SettingsUI::FillRound(x, y, sb_w, h, 0, t.sidebar);
    SettingsUI::FillRound(x + sb_w - 1, y, 1, h, 0, t.sep);
    PushClip(x, y, sb_w - 1, h);

    // search field (satoru)
    if(!compact){
        int fx = x + 12, fy = y + 14, fw = sb_w - 24, fh = 32;
        float f = SettingsUI::Spring(0x5EA2C001u, s_search_focus ? 1.0f : 0.0f, 0);
        bool hov = s_lmx >= 12 && s_lmx < 12 + fw && s_lmy >= 14 && s_lmy < 14 + fh;
        float hv = SettingsUI::Spring(0x5EA2C002u, hov ? 1.0f : 0.0f, 0);
        uint32_t bg = SettingsUI::Mix(SettingsUI::Mix(t.sidebar, t.field, 200), t.card, (int)(f * 255.0f));
        bg = SettingsUI::Mix(bg, t.text, (int)(hv * 10.0f));
        SettingsUI::FillRound(fx, fy, fw, fh, 9, bg);
        SettingsUI::StrokeRound(fx, fy, fw, fh, 9, SettingsUI::Mix(t.field_line, t.accent, (int)(f * 255.0f)), (int)(110 + f * 145));
        if(f > 0.02f) SettingsUI::StrokeRound(fx - 1, fy - 1, fw + 2, fh + 2, 10, t.accent, (int)(f * 80.0f));
        SettingsUI::Icon(IC_SEARCH, fx + 9, fy + 8, 16, t.text3);
        C.under = bg;
        PushClip(fx + 30, fy, fw - 58, fh);
        if(s_query[0]) SettingsUI::Text(fx + 32, fy + 7, 14.0f, s_query, t.text);
        else SettingsUI::Text(fx + 32, fy + 7, 14.0f, "Search", t.text3);
        if(s_search_focus){
            uint32_t el = C.now - s_search_ms;
            bool on = el > 10000u || ((el / 530u) % 2u) == 0;
            if(el <= 10000u) Graphics::RequestFrameAt(C.now + 530u - (el % 530u));
            int tw = s_query[0] ? SettingsUI::TextWidth(14.0f, s_query) : 0;
            if(on) SettingsUI::FillRound(fx + 33 + tw, fy + 8, 2, 17, 1, t.accent);
        }
        PopClip();
        float cv = SettingsUI::Spring(0x5EA2C003u, s_query[0] ? 1.0f : 0.0f, 1);
        if(cv > 0.02f){
            int sz = (int)(16 * (0.6f + 0.4f * cv));
            SettingsUI::Icon(IC_CLEAR, fx + fw - 8 - sz / 2 - 8, fy + 16 - sz / 2, sz, t.text3, (int)(cv * 255.0f));
        }
        C.under = t.sidebar;
    }

    // items: visibility springs drive height + alpha so search filtering animates (satoru)
    float iy = (float)(y + (compact ? 14 : SB_TOP));
    float sel_top = -1, sel_bot = -1;
    float item_y[16], item_h[16];
    int last_vis = -1;
    for(int i = 0; i < g_module_count; i++){
        bool vis = s_hit[i];
        float v = SettingsUI::Spring(0x51DE1000u + (uint32_t)i, vis ? 1.0f : 0.0f, 3);
        if(v < 0) v = 0;
        if(kGroupBreak[i] && last_vis >= 0 && !s_query[0]) iy += SB_GAP;
        item_y[i] = iy; item_h[i] = SB_ITEM * (v > 1.0f ? 1.0f : v);
        if(i == s_selected){ sel_top = iy; sel_bot = iy + SB_ITEM; }
        iy += (SB_ITEM + 2) * (v > 1.0f ? 1.0f : v);
        if(vis) last_vis = i;
    }
    // sliding selection pill: two springs (top / bottom edge) so it stretches
    // toward where it is going and snaps in behind (satoru)
    if(sel_top >= 0 && s_hit[s_selected]){
        static float s_top = -1, s_bot = -1;
        bool moving_down = sel_top > s_top + 0.5f;
        bool moving_up = sel_top < s_top - 0.5f;
        s_top = SettingsUI::Spring(0x5E1E0001u, sel_top, moving_up ? 1 : 3);
        s_bot = SettingsUI::Spring(0x5E1E0002u, sel_bot, moving_down ? 1 : 3);
        float top = s_top, bot = s_bot;
        if(bot - top < SB_ITEM * 0.6f) bot = top + SB_ITEM * 0.6f;
        SettingsUI::FillRound(x + 8, (int)(top + 0.5f), sb_w - 16, (int)(bot - top + 0.5f), 9, SettingsUI::Mix(t.sidebar, t.text, t.dark ? 30 : 22));
    }
    for(int i = 0; i < g_module_count; i++){
        if(item_h[i] < 1.0f) continue;
        int iyy = (int)(item_y[i] + 0.5f);
        float v = item_h[i] / SB_ITEM;
        bool sel = i == s_selected;
        bool hov = i == s_hover_item && !sel;
        float hv = SettingsUI::Spring(0x40E71000u + (uint32_t)i, hov ? 1.0f : 0.0f, 0);
        if(hv > 0.01f) SettingsUI::FillRound(x + 8, iyy, sb_w - 16, SB_ITEM, 9, SettingsUI::Mix(t.sidebar, t.text, 16), (int)(hv * 255.0f * v));
        float sv = SettingsUI::Spring(0x5E1E1000u + (uint32_t)i, sel ? 1.0f : 0.0f, 0);
        const ModMeta* mm = meta_for(g_modules[i]);
        uint32_t ic = SettingsUI::Mix(t.text2, t.accent, (int)(sv * 255.0f));
        uint32_t tc = t.text;
        C.alpha = (int)(v * 255.0f);
        C.under = sel ? SettingsUI::Mix(t.sidebar, t.text, t.dark ? 30 : 22) : t.sidebar;
        int icon_x = compact ? x + (sb_w - 20) / 2 : x + 18;
        PushClip(x, iyy, sb_w, (int)(item_h[i] + 0.5f));
        SettingsUI::Icon(mm ? mm->icon : IC_GENERIC, icon_x, iyy + 7, 20, ic);
        if(!compact) TextFit(x + 48, iyy + 8, 14.5f, g_modules[i]->title, tc, sb_w - 60, sel);
        PopClip();
        C.alpha = 255;
    }
    if(!s_any_hit && !compact){
        C.under = t.sidebar;
        SettingsUI::Text(x + 20, y + SB_TOP + 8, 13.5f, "No results", t.text3);
    }
    PopClip();
}

static int sidebar_item_at(int lx, int ly){
    if(lx < 0 || lx >= s_geo.sb_w) return -1;
    bool compact = s_geo.sb_w < SB_W_FULL;
    int iy = compact ? 14 : SB_TOP;
    int last_vis = -1;
    for(int i = 0; i < g_module_count; i++){
        if(!s_hit[i]) continue;
        if(kGroupBreak[i] && last_vis >= 0 && !s_query[0]) iy += SB_GAP;
        if(ly >= iy && ly < iy + SB_ITEM) return i;
        iy += SB_ITEM + 2;
        last_vis = i;
    }
    return -1;
}

static void render_page(int bx, int by, int bw, int bh){
    const SettingsUI::Theme& t = SettingsUI::T();
    const SettingsModule* m = g_modules[s_selected];
    int view_y = by + HEADER_H, view_h = bh - HEADER_H;
    if(view_h < 0) view_h = 0;
    int col_w = bw - 56; if(col_w > COL_MAX) col_w = COL_MAX; if(col_w < 200) col_w = bw - 16;
    int col_x = bx + (bw - col_w) / 2;
    s_geo.bx = bx; s_geo.bw = bw; s_geo.view_y = view_y; s_geo.view_h = view_h;
    s_geo.col_x = col_x; s_geo.col_w = col_w; s_geo.legacy = (m->build == nullptr);
    s_view_h = view_h;

    // content height + scroll (satoru)
    if(m->build){
        C.col_x = col_x; C.col_w = col_w;
        C.view_x = bx; C.view_y = view_y; C.view_w = bw; C.view_h = view_h;
        C.origin_y = view_y - (int)(s_scroll_v + 0.5f);
        C.page = s_selected;
        C.query = s_query[0] ? s_query : nullptr;
        C.pointer_ok = false;
        BeginPass(P_LAYOUT);
        m->build();
        EndPass();
        s_content_h = C.content_h;
        s_focus_count = C.focus_count;
        if(C.focus >= s_focus_count) C.focus = s_focus_count - 1;
        if(s_scroll_to_focus && C.focus_y0 >= 0){
            int& tg = s_scroll_t[s_selected & 15];
            if(C.focus_y0 - 24 < tg) tg = C.focus_y0 - 24;
            if(C.focus_y1 + 24 > tg + view_h) tg = C.focus_y1 + 24 - view_h;
        }
        s_scroll_to_focus = false;
        if(s_scroll_to_hit){
            s_scroll_to_hit = false;
            if(C.first_hit_y >= 0 && (C.first_hit_y > s_scroll_t[s_selected & 15] + view_h - 80 || C.first_hit_y < s_scroll_t[s_selected & 15]))
                s_scroll_t[s_selected & 15] = C.first_hit_y - 16;
        }
    } else {
        s_content_h = m->content_height ? m->content_height() + 24 : 0;
        s_focus_count = 0;
    }
    clamp_scroll_target();
    s_scroll_v = s_bar_drag ? (float)s_scroll_t[s_selected & 15]
                            : SettingsUI::Spring(scroll_id(s_selected), (float)s_scroll_t[s_selected & 15], 2);

    // page body (satoru)
    PushClip(bx, view_y, bw, view_h);
    if(m->build){
        C.origin_y = view_y - (int)(s_scroll_v + 0.5f);
        int amx = s_geo.cx + s_lmx, amy = s_geo.cy + s_lmy;
        C.mx = amx; C.my = amy; C.down = s_down;
        C.pointer_ok = s_lmx >= 0 && amx >= bx && amx < bx + bw && amy >= view_y && amy < view_y + view_h
                       && !s_ptr_in_pop && !s_bar_drag;
        BeginPass(P_DRAW);
        m->build();
        EndPass();
        C.alpha = 255; C.dy = 0; C.under = t.bg;
    } else {
        // legacy module: its own coordinates; fade + rise in on page open (satoru)
        float e = Ease(C.page_ms, 20, 320);
        int rise = (int)((1.0f - e) * 18.0f);
        if(m->render) m->render(col_x + 4, view_y + rise, col_w - 8, view_h, (int)(s_scroll_v + 0.5f));
        if(e < 1.0f){
            C.alpha = 255;
            SettingsUI::FillRound(bx, view_y, bw, view_h, 0, t.bg, (int)((1.0f - e) * 255.0f));
        }
    }
    PopClip();

    // header: title slides in with the page, hairline appears once scrolled (satoru)
    {
        float e = Ease(C.page_ms, 0, 300);
        int off = (int)((1.0f - e) * 12.0f) * C.dir;
        C.alpha = (int)(e * 255.0f); C.under = t.bg;
        PushClip(bx, by, bw, HEADER_H);
        SettingsUI::Text(col_x + 2, by + 20 + off, 22.0f, m->title, t.text, true);
        PopClip();
        C.alpha = 255;
        float sh = SettingsUI::Spring(0x4EAD0001u, s_scroll_v > 2.0f ? 1.0f : 0.0f, 0);
        if(sh > 0.01f){
            SettingsUI::FillRound(bx, view_y - 1, bw, 1, 0, t.sep, (int)(sh * 255.0f));
            for(int i = 0; i < 4; i++)
                SettingsUI::FillRound(bx, view_y + i, bw, 1, 0, 0xFF000000u, (int)(sh * (t.dark ? 26 : 10) * (4 - i) / 4));
        }
    }

    // overlay scrollbar: fades in while scrolling or hovered (satoru)
    int maxsc = s_content_h - view_h;
    if(maxsc > 0 && view_h > 40){
        int bar_x = bx + bw - 12;
        int amx = s_geo.cx + s_lmx, amy = s_geo.cy + s_lmy;
        bool near = s_lmx >= 0 && amx >= bar_x - 6 && amx < bx + bw && amy >= view_y && amy < view_y + view_h;
        bool show = s_bar_drag || near || (C.now - s_scroll_ms) < 900u;
        if(show && !s_bar_drag && !near) Graphics::RequestFrameAt(s_scroll_ms + 905u);
        float a = SettingsUI::Spring(0x5C0BA001u, show ? 1.0f : 0.0f, 0);
        float wv = SettingsUI::Spring(0x5C0BA002u, (near || s_bar_drag) ? 1.0f : 0.0f, 0);
        if(a > 0.01f){
            int th = view_h * view_h / (s_content_h > 0 ? s_content_h : 1);
            if(th < 36) th = 36;
            int travel = view_h - 8 - th;
            float sv = s_scroll_v < 0 ? 0 : (s_scroll_v > maxsc ? (float)maxsc : s_scroll_v);
            int ty = view_y + 4 + (int)(sv * travel / maxsc);
            int bwid = 6 + (int)(wv * 3.0f);
            SettingsUI::FillRound(bx + bw - 4 - bwid, ty, bwid, th, bwid / 2, t.text, (int)(a * (90 + wv * 60)));
        }
    }
    (void)t;
}

void SystemSettings::Render(Window* w, int x, int y, int wd, int ht){
    C.now = Timer::GetRealMs();
    C.animating = false;
    C.reduced = !anim_on();
    s_geo.cx = x; s_geo.cy = y; s_geo.cw = wd; s_geo.ch = ht;
    // the window only hears pointer moves while the pointer is over it; drop
    // hover state once it has left so nothing stays lit. (satoru)
    if(!s_down && (Mouse::mx < x || Mouse::mx >= x + wd || Mouse::my < y || Mouse::my >= y + ht)){
        s_lmx = s_lmy = -1; s_hover_item = -1; s_ptr_in_pop = false;
    }
    ResetClip(x, y, wd, ht);
    drain_keys();
    ThemeUpdate();
    const SettingsUI::Theme& t = SettingsUI::T();
    if(w){ w->bg_color = t.bg; w->open_tint = t.bg; }
    C.alpha = 255; C.dy = 0; C.under = t.bg;

    int sb_w = wd >= 760 ? SB_W_FULL : SB_W_COMPACT;
    s_geo.sb_w = sb_w;
    SettingsUI::FillRound(x + sb_w, y, wd - sb_w, ht, 0, t.bg);
    render_page(x + sb_w, y, wd - sb_w, ht);
    draw_sidebar(x, y, sb_w, ht);
    C.alpha = 255;
    DrawPopover(x, y, wd, ht);

    static uint32_t s_sweep = 0;
    if(C.now - s_sweep > 2000u){ s_sweep = C.now; AnimSweep(); }
    if(C.animating || ThemeAnimating()) Graphics::MarkUIDirty();
}

// ════════════════════════════════════════════════════════════════════════════
//  input (events from the window manager, content-local coordinates)
// ════════════════════════════════════════════════════════════════════════════
static bool in_scrollbar(int lx, int ly){
    int maxsc = s_content_h - s_view_h;
    if(maxsc <= 0) return false;
    int ax = s_geo.cx + lx, ay = s_geo.cy + ly;
    return ax >= s_geo.bx + s_geo.bw - 14 && ax < s_geo.bx + s_geo.bw && ay >= s_geo.view_y && ay < s_geo.view_y + s_geo.view_h;
}
static void bar_drag_to(int ly){
    int maxsc = s_content_h - s_view_h;
    if(maxsc <= 0) return;
    int th = s_view_h * s_view_h / (s_content_h > 0 ? s_content_h : 1);
    if(th < 36) th = 36;
    int travel = s_view_h - 8 - th;
    if(travel < 1) travel = 1;
    int pos = (s_geo.cy + ly) - s_bar_grab - (s_geo.view_y + 4);
    s_scroll_t[s_selected & 15] = pos * maxsc / travel;
    clamp_scroll_target();
    s_scroll_ms = Timer::GetRealMs();
}

void SystemSettings::Input(Window* w, int ev, int p1, int p2){
    if(!w) return;
    C.now = Timer::GetRealMs();
    const SettingsModule* m = g_modules[s_selected];
    int ax = s_geo.cx + p1, ay = s_geo.cy + p2;

    if(ev == 5){                                   // pointer move (satoru)
        if(p1 == s_lmx && p2 == s_lmy) return;
        s_lmx = p1; s_lmy = p2;
        C.mx = ax; C.my = ay;
        s_ptr_in_pop = C.pop.open && PopoverEvent(EV_MOVE, ax, ay, 0);
        int hi = sidebar_item_at(p1, p2);
        if(hi != s_hover_item){ s_hover_item = hi; }
        if(s_bar_drag){ bar_drag_to(p2); Graphics::MarkUIDirty(); return; }
        if(C.active && s_down && m->build) event_pass(EV_MOVE, ax, ay, 0);
        if(C.focus_visible){ C.focus_visible = false; }
        return;
    }
    if(ev == 6){                                   // button edge (satoru)
        if(p1 != 0) return;
        bool pressed = p2 != 0;
        s_down = pressed;
        C.down = pressed;
        int lx = s_lmx, ly = s_lmy;
        if(pressed){
            if(C.pop.open){ PopoverEvent(EV_DOWN, s_geo.cx + lx, s_geo.cy + ly, 0); return; }
            if(in_scrollbar(lx, ly)){
                int maxsc = s_content_h - s_view_h;
                int th = s_view_h * s_view_h / (s_content_h > 0 ? s_content_h : 1);
                if(th < 36) th = 36;
                int travel = s_view_h - 8 - th;
                int ty = s_geo.view_y + 4 + (maxsc > 0 ? (int)(s_scroll_v * travel / maxsc) : 0);
                int ay2 = s_geo.cy + ly;
                s_bar_grab = (ay2 >= ty && ay2 < ty + th) ? ay2 - ty : th / 2;
                s_bar_drag = true;
                bar_drag_to(ly);
                Graphics::MarkUIDirty();
                return;
            }
            if(m->build && lx >= s_geo.sb_w && s_geo.cy + ly >= s_geo.view_y)
                event_pass(EV_DOWN, s_geo.cx + lx, s_geo.cy + ly, 0);
        } else {
            if(s_bar_drag){ s_bar_drag = false; Graphics::MarkUIDirty(); return; }
            if(C.pop.open){ PopoverEvent(EV_UP, 0, 0, 0); }
            if(C.active && m->build) event_pass(EV_UP, s_geo.cx + lx, s_geo.cy + ly, 0);
            C.active = 0;
            Graphics::MarkUIDirty();
        }
        return;
    }
    if(ev == 1){                                   // click (satoru)
        s_lmx = p1; s_lmy = p2;
        C.focus_visible = false;
        if(C.pop.open){
            PopoverEvent(EV_CLICK, ax, ay, 0);
            if(C.pick_id){ event_pass(EV_PICK, 0, 0, 0); C.pick_id = 0; }
            s_ptr_in_pop = false;
            Graphics::MarkUIDirty();
            return;
        }
        if(s_bar_drag || in_scrollbar(p1, p2)) return;
        if(p1 < s_geo.sb_w){
            C.edit_id = 0;
            bool compact = s_geo.sb_w < SB_W_FULL;
            if(!compact && p2 >= 14 && p2 < 46){
                // search field; the clear button empties it (satoru)
                if(s_query[0] && p1 >= s_geo.sb_w - 44){ s_query[0] = 0; run_search(); s_search_focus = false; }
                else { s_search_focus = true; s_search_ms = C.now; }
                Graphics::MarkUIDirty();
                return;
            }
            s_search_focus = false;
            int t = sidebar_item_at(p1, p2);
            if(t >= 0 && t != s_selected) show_module(t);
            Graphics::MarkUIDirty();
            return;
        }
        s_search_focus = false;
        if(ay < s_geo.view_y){ Graphics::MarkUIDirty(); return; }
        if(m->build){
            C.edit_id = 0;
            event_pass(EV_CLICK, ax, ay, 0);
        } else if(m->input){
            int rel_x = ax - (s_geo.col_x + 4);
            int rel_y = ay - s_geo.view_y;
            if(m->input(rel_x, rel_y, true, 0, (int)(s_scroll_v + 0.5f))) Graphics::MarkUIDirty();
        }
        return;
    }
    if(ev == 3){                                   // wheel (satoru)
        if(C.pop.open){ ClosePopover(); Graphics::MarkUIDirty(); return; }
        // wheel delta is positive for scrolling down (the same sign firefox gets
        // through wl_pointer.axis). (satoru)
        s_scroll_t[s_selected & 15] += p1 * 64;
        clamp_scroll_target();
        s_scroll_ms = C.now;
        Graphics::MarkUIDirty();
        return;
    }
    if(ev == 2){                                   // character (satoru)
        int k = (unsigned char)(char)p1;
        if(k == '\t') return;                      // tab comes via KeyEvent with shift state (satoru)
        if(k == '\r') k = K_ENTER;
        if(k == 127) k = K_BS;
        handle_key(k);
        return;
    }
}

// ── public api ──────────────────────────────────────────────────────────────
static void render_cb(Window* w, int cx, int cy, int cw, int ch){ SystemSettings::Render(w, cx, cy, cw, ch); }
static void input_cb(Window* w, int ev, int p1, int p2){ SystemSettings::Input(w, ev, p1, p2); }

bool SystemSettings::IsOpen(){
    return s_win_id >= 0 && WindowManager::GetWindow(s_win_id) != nullptr;
}

void SystemSettings::Open(){
#if !defined(KUI_BACKEND_WAYLAND)
    // the desktop, the dock and the control center all come through here: run the
    // ring-3 Settings when asked for it and the ELF is there, else the same shell
    // built into the kernel. (satoru)
    if(KuiKernel::Ring3Wanted("settings") && KuiKernel::LaunchRing3("settings")) return;
#endif
    OpenKernel();
}

void SystemSettings::OpenKernel(){
    TaskMem::AppScope _tms(TaskMem::APP_SYSSETTINGS);   // kurono.taskmem=1: charge to the app (satoru)
    if(IsOpen()){
        WindowManager::Focus(s_win_id);
        WindowManager::BringToFront(s_win_id);
        return;
    }
    RuntimeLog::LogAppEvent("system_settings", "open");
    s_hover_item = -1;
    s_query[0] = 0; s_search_focus = false;
    for(int i = 0; i < 16; i++){ s_hit[i] = true; }
    s_any_hit = true;
    C.focus = -1; C.focus_visible = false; C.pop.open = false; C.edit_id = 0; C.active = 0;
    s_lmx = s_lmy = -1;
    int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    int ww = 960, wh = 680;
    if(ww > sw - 40) ww = sw - 40;
    if(wh > sh - 120) wh = sh - 120;
    s_win_id = WindowManager::CreateWindow("Settings", -1, -1, ww, wh, render_cb, input_cb);
    if(Window* w = WindowManager::GetWindow(s_win_id)){
        ThemeUpdate();
        w->bg_color = SettingsUI::T().bg;
        w->open_tint = SettingsUI::T().bg;
    }
    show_module(s_selected);
}

void SystemSettings::OpenPage(const char* id){
    TaskMem::AppScope _tms(TaskMem::APP_SYSSETTINGS);   // kurono.taskmem=1: charge to the app (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
    if(id && id[0] && KuiKernel::Ring3Wanted("settings")){
        KVFS::Mkdirs("/system/run/kui");
        KVFS::WriteString("/system/run/kui/settings-page", id);
        if(KuiKernel::LaunchRing3("settings")) return;
        KVFS::Unlink("/system/run/kui/settings-page");
    }
#endif
    Open();
    if(!IsOpen()) return;   // a ring-3 launch owns the page selection now (satoru)
    if(!id) return;
    int i = module_index(id);
    if(i >= 0 && i != s_selected) show_module(i);
}

void SystemSettings::RestoreAtBoot(){
    // solid colour wallpaper: index 2 means "no image, use desktop.bg" (satoru)
    if(UIConfig::Int("desktop.wallpaper_index", 0) == 2)
        Desktop::SetWallpaper(UIConfig::Color("desktop.bg", 0xFF0B0F1A));
    // master volume + mute (the mixer and the legacy audio path) (satoru)
    int vol = UIConfig::Int("audio.master_volume", -1);
    if(vol >= 0 && vol <= 100){
        AudioMixer::SetMasterVolume(vol);
        if(Audio::IsAvailable()) Audio::SetMasterVolume(vol);
    }
    if(UIConfig::Int("audio.muted", 0)){
        AudioMixer::SetMasterMute(true);
        if(Audio::IsAvailable()) Audio::SetMuted(true);
    }
    // power mode -> cpufreq governor on every managed cpu (satoru)
    int plan = UIConfig::Int("power.plan", -1);
    int n = CPUFreq::CPUCount();
    if(plan >= 0 && plan <= 2 && n > 0){
        CPUFreq::Governor g = plan == 0 ? CPUFreq::GOV_PERFORMANCE
                            : (plan == 2 ? CPUFreq::GOV_POWERSAVE : CPUFreq::GOV_ONDEMAND);
        for(int i = 0; i < n; i++) CPUFreq::SetGovernor((uint32_t)i, g);
    }
    // settings > linux: start the vm in the background when asked to (satoru)
    VMHost::AutostartAtBoot();
}
// end (satoru)
