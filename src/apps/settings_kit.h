//  kurono os - settings control kit internals shared by the kit and the shell (satoru)
#pragma once
#include "system_settings.h"

namespace SKit {

enum Pass { P_LAYOUT = 0, P_DRAW = 1, P_EVENT = 2, P_SEARCH = 3 };
enum Ev   { EV_NONE = 0, EV_CLICK, EV_DOWN, EV_UP, EV_MOVE, EV_KEY, EV_PICK };

// key codes the kit understands (printables arrive as themselves). (satoru)
enum Keys {
    K_BS = 8, K_TAB = 9, K_ENTER = 10, K_ESC = 27, K_SPACE = 32,
    K_UP = 0x90, K_DOWN = 0x91, K_LEFT = 0x92, K_RIGHT = 0x93,
    K_HOME = 0x94, K_END = 0x95, K_PGUP = 0x96, K_PGDN = 0x97, K_DEL = 0x98, K_BTAB = 0x99,
};

// icon ids (vector, drawn at any size). (satoru)
enum Icons {
    IC_NETWORK = 0, IC_DISPLAY, IC_SOUND, IC_POWER, IC_APPEARANCE, IC_A11Y, IC_SECURITY,
    IC_STORAGE, IC_DEVICES, IC_SYSTEM, IC_ABOUT, IC_SEARCH, IC_CHEV_R, IC_CHEV_UD, IC_CHECK,
    IC_LOCK, IC_CLEAR, IC_WIFI_SMALL, IC_ETHERNET, IC_USER, IC_GENERIC, IC_CHEV_L, IC_LINUX, IC_GLOBE, IC_COUNT
};

struct Pop {
    uint32_t id;
    bool     open;
    int      ax, ay, aw, ah;   // anchor (screen) (satoru)
    int      n, sel, hi;
    char     items[16][44];
    uint32_t t_open;
};

struct Ctx {
    Pass pass;
    uint32_t now;
    // content column + viewport (screen coords) (satoru)
    int col_x, col_w;
    int view_x, view_y, view_w, view_h;
    int origin_y;             // screen y of content y=0 (view_y - scroll) (satoru)
    int y;                    // running content y (satoru)
    int content_h;
    int page;
    uint32_t page_ms;
    int dir;                  // +1 page entered from below, -1 from above (satoru)
    // groups (satoru)
    bool in_group;
    int  group_seq, group_y0, row_in_group;
    int  group_h[64];
    // focus (satoru)
    int  focus_count, focus;
    bool focus_visible;       // keyboard navigation in progress: draw focus rings (satoru)
    uint32_t focus_id;
    uint32_t down_id;         // control that took the last EV_DOWN (slider drags) (satoru)
    uint32_t down_ms;
    int  focus_y0, focus_y1;  // content y span of the focused row (satoru)
    // pointer (screen coords) (satoru)
    int  mx, my;
    bool down;
    bool pointer_ok;          // pointer is over the viewport, not under the popover (satoru)
    uint32_t hot, active;
    // event (satoru)
    Ev   ev;
    int  ev_x, ev_y, ev_key;
    bool consumed, changed, committed, submitted;
    uint32_t pick_id;
    int  pick_index;
    // ids (satoru)
    uint32_t id_stack[8];
    int  id_sp;
    uint32_t group_id;
    // drawing state (satoru)
    bool animating;
    int  alpha;               // 0..255 multiplier for page fades (satoru)
    int  dy;                  // draw-only vertical offset (stagger rise) (satoru)
    uint32_t under;           // colour under the current drawing (text fades into it) (satoru)
    // search (satoru)
    const char* query;
    bool search_hit;
    int  first_hit_y;         // content y of the first row matching the query (satoru)
    // text editing (satoru)
    uint32_t edit_id;
    uint32_t edit_ms;
    // popover (satoru)
    Pop pop;
    // navigation request from GoTo (satoru)
    char goto_id[24];
    bool reduced;             // reduced motion / animations off (satoru)
};

extern Ctx C;

// theme (satoru)
void ThemeUpdate();           // per frame: reads ui.appearance + accent, crossfades (satoru)
bool ThemeAnimating();

// clip stack (also pushes onto Graphics so text clips too) (satoru)
void PushClip(int x, int y, int w, int h);
void PopClip();
void ResetClip(int x, int y, int w, int h);

// drawing (satoru)
void Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha);
int  TextFit(int x, int y, float px, const char* s, uint32_t c, int maxw, bool bold = false);
uint32_t Fade(uint32_t c);    // mix toward C.under by C.alpha (satoru)

// pass control (satoru)
void BeginPass(Pass p);
void EndPass();
bool Match(const char* hay, const char* needle);

// popover (drawn by the shell after the page; events routed first) (satoru)
void DrawPopover(int win_x, int win_y, int win_w, int win_h);
bool PopoverEvent(Ev ev, int x, int y, int key);   // true if consumed (satoru)
void ClosePopover();

// animation table upkeep (satoru)
void AnimSweep();
// spring that starts from `seed` the first time the id is seen (grow-in bars) (satoru)
float SpringSeed(uint32_t id, float target, int kind, float seed);
// eased 0..1 progress of a one-shot tween started at t0 (out-cubic), marks animating (satoru)
float Ease(uint32_t t0, int delay_ms, int dur_ms);

}
// end (satoru)
