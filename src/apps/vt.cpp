#include "vt.h"

// see vt.h. xterm-compatible enough for line editors, ink/react tuis and curses
// programs; every sequence a tui can send that we do not implement is consumed and
// ignored so it never leaks onto the screen. (satoru)

static inline void vt_mcpy(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
static inline void vt_mmove(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    if (dd < ss) { for (size_t i = 0; i < n; i++) dd[i] = ss[i]; }
    else { for (size_t i = n; i > 0; i--) dd[i - 1] = ss[i - 1]; }
}

enum { S_GROUND = 0, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC, S_DESIG };

static const uint32_t ANSI16[16] = {
    0xFF000000, 0xFFCD3131, 0xFF0DBC79, 0xFFE5E510, 0xFF2472C8, 0xFFBC3FBC, 0xFF11A8CD, 0xFFE5E5E5,
    0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF5F543, 0xFF3B8EEA, 0xFFD670D6, 0xFF29B8DB, 0xFFFFFFFF
};

uint32_t Vt::palette(int i) const {
    if (i < 0) i = 0;
    if (i < 16) return ANSI16[i];
    if (i < 232) {
        int v = i - 16;
        int r = v / 36, g = (v / 6) % 6, b = v % 6;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return 0xFF000000u | ((uint32_t)lv[r] << 16) | ((uint32_t)lv[g] << 8) | (uint32_t)lv[b];
    }
    int g = 8 + (i - 232) * 10;
    if (g > 255) g = 255;
    return 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
}

void Vt::blank_row(VtCell* row) {
    for (int c = 0; c < VT_MAXC; c++) { row[c].cp = 0; row[c].fg = def_fg_; row[c].bg = bg_; row[c].attr = 0; }
}

void Vt::reset() {
    fg_ = def_fg_; bg_ = def_bg_; attr_ = 0;
    cx_ = cy_ = 0; top_ = 0; bot_ = rows_ - 1;
    wrap_pending_ = false; autowrap_ = true; origin_ = false; cursor_vis_ = true;
    bpaste_ = false; appcur_ = false; insert_ = false;
    g0_gfx_ = g1_gfx_ = shift_out_ = 0;
    state_ = S_GROUND; utf_need_ = 0; np_ = 0;
    for (int i = 0; i < VT_MAXC; i++) tabs_[i] = (i % 8) == 0;
    scr_ = main_; alt_ = false;
    for (int r = 0; r < VT_MAXR; r++) { blank_row(main_ + r * VT_MAXC); blank_row(alt_buf_ + r * VT_MAXC); }
    hist_head_ = hist_n_ = 0;
    dirty_ = true;
}

void Vt::Init(int cols, int rows, uint32_t def_fg, uint32_t def_bg) {
    def_fg_ = def_fg; def_bg_ = def_bg;
    if (cols < 10) cols = 10; if (cols > VT_MAXC) cols = VT_MAXC;
    if (rows < 2) rows = 2;   if (rows > VT_MAXR) rows = VT_MAXR;
    cols_ = cols; rows_ = rows;
    reply_n_ = 0;
    reset();
}

void Vt::SetDefaults(uint32_t def_fg, uint32_t def_bg) {
    // recolour cells that still carry the old defaults so a live re-theme looks right (satoru)
    uint32_t ofg = def_fg_, obg = def_bg_;
    if (ofg == def_fg && obg == def_bg) return;
    for (int b = 0; b < 2; b++) {
        VtCell* s = b ? alt_buf_ : main_;
        for (int i = 0; i < VT_MAXR * VT_MAXC; i++) {
            if (s[i].fg == ofg) s[i].fg = def_fg;
            if (s[i].bg == obg) s[i].bg = def_bg;
        }
    }
    if (fg_ == ofg) fg_ = def_fg;
    if (bg_ == obg) bg_ = def_bg;
    def_fg_ = def_fg; def_bg_ = def_bg;
    dirty_ = true;
}

const VtCell* Vt::HistLine(int i) const {
    if (i < 0 || i >= hist_n_) return nullptr;
    int idx = (hist_head_ - 1 - i + VT_SCROLLBACK * 2) % VT_SCROLLBACK;
    return hist_ + idx * VT_MAXC;
}

int Vt::TakeReply(uint8_t* out, int max) {
    int n = reply_n_ < max ? reply_n_ : max;
    for (int i = 0; i < n; i++) out[i] = reply_[i];
    if (n < reply_n_) { for (int i = n; i < reply_n_; i++) reply_[i - n] = reply_[i]; reply_n_ -= n; }
    else reply_n_ = 0;
    return n;
}

void Vt::put_reply_n(const char* s, int n) {
    for (int i = 0; i < n && reply_n_ < (int)sizeof(reply_); i++) reply_[reply_n_++] = (uint8_t)s[i];
}
void Vt::put_reply(const char* s) { int n = 0; while (s[n]) n++; put_reply_n(s, n); }

static int vt_itoa(char* d, int v) {
    char t[12]; int n = 0;
    if (v <= 0) { d[0] = '0'; return 1; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) d[i] = t[n - 1 - i];
    return n;
}

void Vt::move_to(int r, int c) {
    int lo = origin_ ? top_ : 0, hi = origin_ ? bot_ : rows_ - 1;
    if (r < lo) r = lo; if (r > hi) r = hi;
    if (c < 0) c = 0;   if (c > cols_ - 1) c = cols_ - 1;
    cy_ = r; cx_ = c; wrap_pending_ = false;
}

void Vt::scroll_up(int n) {
    // scroll the region up by n lines; only a full-width main-screen region feeds the history (satoru)
    if (n <= 0) return;
    int h = bot_ - top_ + 1;
    if (n > h) n = h;
    if (!alt_ && top_ == 0) {
        for (int i = 0; i < n; i++) {
            vt_mcpy(hist_ + hist_head_ * VT_MAXC, scr_ + i * VT_MAXC, sizeof(VtCell) * VT_MAXC);
            hist_head_ = (hist_head_ + 1) % VT_SCROLLBACK;
            if (hist_n_ < VT_SCROLLBACK) hist_n_++;
        }
    }
    if (h > n) vt_mmove(scr_ + top_ * VT_MAXC, scr_ + (top_ + n) * VT_MAXC, sizeof(VtCell) * VT_MAXC * (size_t)(h - n));
    for (int r = bot_ - n + 1; r <= bot_; r++) blank_row(scr_ + r * VT_MAXC);
    dirty_ = true;
}

void Vt::scroll_down(int n) {
    if (n <= 0) return;
    int h = bot_ - top_ + 1;
    if (n > h) n = h;
    if (h > n) vt_mmove(scr_ + (top_ + n) * VT_MAXC, scr_ + top_ * VT_MAXC, sizeof(VtCell) * VT_MAXC * (size_t)(h - n));
    for (int r = top_; r < top_ + n; r++) blank_row(scr_ + r * VT_MAXC);
    dirty_ = true;
}

void Vt::line_feed() {
    if (cy_ == bot_) scroll_up(1);
    else if (cy_ < rows_ - 1) cy_++;
}

void Vt::reverse_index() {
    if (cy_ == top_) scroll_down(1);
    else if (cy_ > 0) cy_--;
}

void Vt::erase_cells(int r, int c0, int c1) {   // [c0, c1) on row r, background colour erase (satoru)
    if (r < 0 || r >= rows_) return;
    if (c0 < 0) c0 = 0; if (c1 > cols_) c1 = cols_;
    VtCell* row = scr_ + r * VT_MAXC;
    for (int c = c0; c < c1; c++) { row[c].cp = 0; row[c].fg = def_fg_; row[c].bg = bg_; row[c].attr = 0; }
    dirty_ = true;
}

void Vt::clear_screen_all() {
    for (int r = 0; r < rows_; r++) erase_cells(r, 0, cols_);
}

void Vt::save_cursor() {
    sx_ = cx_; sy_ = cy_; sattr_ = attr_; sfg_ = fg_; sbg_ = bg_; swrap_ = wrap_pending_;
}
void Vt::restore_cursor() {
    cx_ = sx_; cy_ = sy_; attr_ = sattr_; fg_ = sfg_ ? sfg_ : def_fg_; bg_ = sbg_ ? sbg_ : def_bg_;
    wrap_pending_ = swrap_;
    if (cx_ > cols_ - 1) cx_ = cols_ - 1;
    if (cy_ > rows_ - 1) cy_ = rows_ - 1;
}

void Vt::switch_screen(bool to_alt) {
    if (to_alt == alt_) return;
    if (to_alt) { mx_ = cx_; my_ = cy_; scr_ = alt_buf_; alt_ = true; }
    else { scr_ = main_; alt_ = false; cx_ = mx_; cy_ = my_; if (cx_ > cols_ - 1) cx_ = cols_ - 1; if (cy_ > rows_ - 1) cy_ = rows_ - 1; }
    top_ = 0; bot_ = rows_ - 1;
    dirty_ = true;
}

void Vt::Resize(int cols, int rows) {
    if (cols < 10) cols = 10; if (cols > VT_MAXC) cols = VT_MAXC;
    if (rows < 2) rows = 2;   if (rows > VT_MAXR) rows = VT_MAXR;
    if (cols == cols_ && rows == rows_) return;
    // main screen: if the cursor would fall off the bottom, the top lines go to history (satoru)
    if (!alt_ && rows < rows_) {
        int cut = cy_ - rows + 1;
        if (cut > 0) {
            int ob = bot_, ot = top_;
            top_ = 0; bot_ = rows_ - 1;
            scroll_up(cut);
            top_ = ot; bot_ = ob;
            cy_ -= cut;
        }
    }
    int old_rows = rows_;
    cols_ = cols; rows_ = rows;
    for (int b = 0; b < 2; b++) {
        VtCell* s = b ? alt_buf_ : main_;
        for (int r = (rows > old_rows ? old_rows : rows); r < VT_MAXR; r++) blank_row(s + r * VT_MAXC);
    }
    top_ = 0; bot_ = rows_ - 1;
    if (cy_ > rows_ - 1) cy_ = rows_ - 1;
    if (cx_ > cols_ - 1) cx_ = cols_ - 1;
    wrap_pending_ = false;
    dirty_ = true;
}

static inline int vt_width(uint32_t cp) {
    if (cp < 0x300) return 1;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) || cp == 0xFE0F || cp == 0xFE0E ||
        cp == 0x20E3 || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;
    return 1;
}

// dec special graphics (ESC ( 0): the vt100 line drawing set (satoru)
static uint32_t vt_gfx(uint32_t c) {
    switch (c) {
    case 'j': return 0x2518; case 'k': return 0x2510; case 'l': return 0x250C; case 'm': return 0x2514;
    case 'n': return 0x253C; case 'q': return 0x2500; case 't': return 0x251C; case 'u': return 0x2524;
    case 'v': return 0x2534; case 'w': return 0x252C; case 'x': return 0x2502; case 'a': return 0x2592;
    case '`': return 0x25C6; case 'f': return 0x00B0; case 'g': return 0x00B1; case '~': return 0x00B7;
    case 'o': return 0x23BA; case 's': return 0x23BD;
    default: return c;
    }
}

void Vt::print_cp(uint32_t cp) {
    bool gfx = shift_out_ ? g1_gfx_ : g0_gfx_;
    if (gfx && cp >= 0x5F && cp <= 0x7E) cp = vt_gfx(cp);
    int w = vt_width(cp);
    if (w == 0) return;
    if (wrap_pending_ && autowrap_) { cx_ = 0; line_feed(); }
    wrap_pending_ = false;
    if (w == 2 && cx_ >= cols_ - 1) {                 // a wide glyph does not fit: wrap first (satoru)
        if (autowrap_) { erase_cells(cy_, cx_, cx_ + 1); cx_ = 0; line_feed(); }
        else w = 1;
    }
    VtCell* row = scr_ + cy_ * VT_MAXC;
    if (insert_) {
        for (int c = cols_ - 1; c >= cx_ + w; c--) row[c] = row[c - w];
    }
    row[cx_].cp = cp; row[cx_].fg = fg_; row[cx_].bg = bg_; row[cx_].attr = attr_;
    if (w == 2) {
        row[cx_ + 1].cp = 0xFFFFFFFFu; row[cx_ + 1].fg = fg_; row[cx_ + 1].bg = bg_; row[cx_ + 1].attr = attr_;
    }
    cx_ += w;
    if (cx_ >= cols_) { cx_ = cols_ - 1; wrap_pending_ = true; }
    dirty_ = true;
}

void Vt::exec_c0(uint8_t c) {
    switch (c) {
    case 0x08: if (cx_ > 0) cx_--; wrap_pending_ = false; break;                      // BS
    case 0x09: {                                                                       // HT
        int x = cx_ + 1;
        while (x < cols_ - 1 && !tabs_[x]) x++;
        cx_ = x > cols_ - 1 ? cols_ - 1 : x; wrap_pending_ = false; break;
    }
    case 0x0A: case 0x0B: case 0x0C: line_feed(); wrap_pending_ = false; break;      // LF VT FF
    case 0x0D: cx_ = 0; wrap_pending_ = false; break;                                  // CR
    case 0x0E: shift_out_ = 1; break;                                                  // SO
    case 0x0F: shift_out_ = 0; break;                                                  // SI
    default: break;                                                                    // BEL etc
    }
}

void Vt::do_sgr() {
    if (np_ == 0) { params_[0] = 0; np_ = 1; }
    for (int i = 0; i < np_; i++) {
        int p = params_[i];
        switch (p) {
        case 0: attr_ = 0; fg_ = def_fg_; bg_ = def_bg_; break;
        case 1: attr_ |= VA_BOLD; break;
        case 2: attr_ |= VA_DIM; break;
        case 3: attr_ |= VA_ITALIC; break;
        case 4: attr_ |= VA_UNDER; break;
        case 7: attr_ |= VA_INVERSE; break;
        case 8: attr_ |= VA_HIDDEN; break;
        case 9: attr_ |= VA_STRIKE; break;
        case 21: attr_ |= VA_UNDER; break;
        case 22: attr_ &= (uint8_t)~(VA_BOLD | VA_DIM); break;
        case 23: attr_ &= (uint8_t)~VA_ITALIC; break;
        case 24: attr_ &= (uint8_t)~VA_UNDER; break;
        case 27: attr_ &= (uint8_t)~VA_INVERSE; break;
        case 28: attr_ &= (uint8_t)~VA_HIDDEN; break;
        case 29: attr_ &= (uint8_t)~VA_STRIKE; break;
        case 39: fg_ = def_fg_; break;
        case 49: bg_ = def_bg_; break;
        case 38: case 48: {
            uint32_t col = 0; bool ok = false;
            if (i + 1 < np_ && params_[i + 1] == 5 && i + 2 < np_) { col = palette(params_[i + 2]); i += 2; ok = true; }
            else if (i + 1 < np_ && params_[i + 1] == 2 && i + 4 < np_) {
                // "38;2;r;g;b" (a colon form carries an empty colourspace id; tolerated by the 5-value rule) (satoru)
                int r = params_[i + 2], g = params_[i + 3], b = params_[i + 4];
                col = 0xFF000000u | ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
                i += 4; ok = true;
            }
            if (ok) { if (p == 38) fg_ = col; else bg_ = col; }
            break;
        }
        default:
            if (p >= 30 && p <= 37) fg_ = ANSI16[p - 30];
            else if (p >= 40 && p <= 47) bg_ = ANSI16[p - 40];
            else if (p >= 90 && p <= 97) fg_ = ANSI16[p - 90 + 8];
            else if (p >= 100 && p <= 107) bg_ = ANSI16[p - 100 + 8];
            break;
        }
    }
}

void Vt::do_mode(bool set) {
    for (int i = 0; i < np_; i++) {
        int m = params_[i];
        if (priv_ == '?') {
            switch (m) {
            case 1: appcur_ = set; break;
            case 6: origin_ = set; move_to(0, 0); break;
            case 7: autowrap_ = set; break;
            case 25: cursor_vis_ = set; break;
            case 47: case 1047: switch_screen(set); if (set) clear_screen_all(); break;
            case 1048: if (set) save_cursor(); else restore_cursor(); break;
            case 1049:
                if (set) { save_cursor(); switch_screen(true); clear_screen_all(); }
                else { switch_screen(false); restore_cursor(); }
                break;
            case 2004: bpaste_ = set; break;
            default: break;              // mouse tracking, focus events, sync output: accepted, unused (satoru)
            }
        } else {
            if (m == 4) insert_ = set;
        }
    }
    dirty_ = true;
}

#define P(i, d) ((i) < np_ && params_[i] > 0 ? params_[i] : (d))

void Vt::do_csi(char f) {
    char buf[40];
    if (inter_ == '$' && f == 'p') {                   // DECRQM: report a mode we know (satoru)
        if (priv_ == '?' && np_ > 0) {
            int m = params_[0], st = 0;
            switch (m) {
            case 1: st = appcur_ ? 1 : 2; break; case 6: st = origin_ ? 1 : 2; break;
            case 7: st = autowrap_ ? 1 : 2; break; case 25: st = cursor_vis_ ? 1 : 2; break;
            case 2004: st = bpaste_ ? 1 : 2; break; case 1049: case 47: case 1047: st = alt_ ? 1 : 2; break;
            default: st = 0; break;
            }
            int n = 0; buf[n++] = 0x1b; buf[n++] = '['; buf[n++] = '?';
            n += vt_itoa(buf + n, m); buf[n++] = ';'; n += vt_itoa(buf + n, st); buf[n++] = '$'; buf[n++] = 'y';
            put_reply_n(buf, n);
        }
        return;
    }
    if (inter_ != 0 && inter_ != ' ') return;          // other intermediates: ignored (satoru)
    if (priv_ == '>' || priv_ == '=' || priv_ == '<') {
        if (f == 'c') put_reply(priv_ == '>' ? "\x1b[>1;10;0c" : "");
        return;
    }
    if (priv_ == '?') {
        if (f == 'h') do_mode(true);
        else if (f == 'l') do_mode(false);
        return;                                        // ?u kitty query etc: no reply = unsupported (satoru)
    }
    switch (f) {
    case 'A': move_to(cy_ - P(0, 1), cx_); break;
    case 'B': case 'e': move_to(cy_ + P(0, 1), cx_); break;
    case 'C': case 'a': move_to(cy_, cx_ + P(0, 1)); break;
    case 'D': move_to(cy_, cx_ - P(0, 1)); break;
    case 'E': move_to(cy_ + P(0, 1), 0); break;
    case 'F': move_to(cy_ - P(0, 1), 0); break;
    case 'G': case '`': move_to(cy_, P(0, 1) - 1); break;
    case 'H': case 'f': {
        int r = P(0, 1) - 1, c = P(1, 1) - 1;
        if (origin_) r += top_;
        move_to(r, c); break;
    }
    case 'd': { int r = P(0, 1) - 1; if (origin_) r += top_; move_to(r, cx_); break; }
    case 'I': { int n = P(0, 1); while (n-- > 0) exec_c0(0x09); break; }
    case 'Z': { int n = P(0, 1); while (n-- > 0) { int x = cx_ - 1; while (x > 0 && !tabs_[x]) x--; cx_ = x < 0 ? 0 : x; } wrap_pending_ = false; break; }
    case 'J': {
        int m = np_ > 0 ? params_[0] : 0;
        if (m == 0) { erase_cells(cy_, cx_, cols_); for (int r = cy_ + 1; r < rows_; r++) erase_cells(r, 0, cols_); }
        else if (m == 1) { for (int r = 0; r < cy_; r++) erase_cells(r, 0, cols_); erase_cells(cy_, 0, cx_ + 1); }
        else if (m == 2) clear_screen_all();
        else if (m == 3) { hist_n_ = 0; hist_head_ = 0; }
        wrap_pending_ = false; break;
    }
    case 'K': {
        int m = np_ > 0 ? params_[0] : 0;
        if (m == 0) erase_cells(cy_, cx_, cols_);
        else if (m == 1) erase_cells(cy_, 0, cx_ + 1);
        else if (m == 2) erase_cells(cy_, 0, cols_);
        wrap_pending_ = false; break;
    }
    case 'L': if (cy_ >= top_ && cy_ <= bot_) { int ot = top_; top_ = cy_; scroll_down(P(0, 1)); top_ = ot; } cx_ = 0; wrap_pending_ = false; break;
    case 'M': if (cy_ >= top_ && cy_ <= bot_) { int ot = top_; top_ = cy_; bool sa = alt_; alt_ = true; scroll_up(P(0, 1)); alt_ = sa; top_ = ot; } cx_ = 0; wrap_pending_ = false; break;
    case '@': {
        int n = P(0, 1); VtCell* row = scr_ + cy_ * VT_MAXC;
        if (n > cols_ - cx_) n = cols_ - cx_;
        for (int c = cols_ - 1; c >= cx_ + n; c--) row[c] = row[c - n];
        erase_cells(cy_, cx_, cx_ + n); wrap_pending_ = false; break;
    }
    case 'P': {
        int n = P(0, 1); VtCell* row = scr_ + cy_ * VT_MAXC;
        if (n > cols_ - cx_) n = cols_ - cx_;
        for (int c = cx_; c + n < cols_; c++) row[c] = row[c + n];
        erase_cells(cy_, cols_ - n, cols_); wrap_pending_ = false; break;
    }
    case 'X': erase_cells(cy_, cx_, cx_ + P(0, 1)); break;
    case 'S': { bool sa = alt_; alt_ = true; scroll_up(P(0, 1)); alt_ = sa; break; }
    case 'T': scroll_down(P(0, 1)); break;
    case 'm': do_sgr(); break;
    case 'n': {
        int m = np_ > 0 ? params_[0] : 0;
        if (m == 5) put_reply("\x1b[0n");
        else if (m == 6) {
            int n = 0; buf[n++] = 0x1b; buf[n++] = '[';
            n += vt_itoa(buf + n, (origin_ ? cy_ - top_ : cy_) + 1); buf[n++] = ';';
            n += vt_itoa(buf + n, (cx_ >= cols_ ? cols_ - 1 : cx_) + 1); buf[n++] = 'R';
            put_reply_n(buf, n);
        }
        break;
    }
    case 'c': if (np_ == 0 || params_[0] == 0) put_reply("\x1b[?62;22c"); break;     // DA1: vt220 + colour (satoru)
    case 'r': {
        int t = P(0, 1) - 1, b = P(1, rows_) - 1;
        if (b > rows_ - 1) b = rows_ - 1;
        if (t < b) { top_ = t; bot_ = b; }
        move_to(origin_ ? top_ : 0, 0); break;
    }
    case 's': if (np_ == 0) save_cursor(); break;
    case 'u': restore_cursor(); break;
    case 'h': do_mode(true); break;
    case 'l': do_mode(false); break;
    case 'g': { int m = np_ > 0 ? params_[0] : 0; if (m == 3) { for (int i = 0; i < VT_MAXC; i++) tabs_[i] = 0; } else if (m == 0 && cx_ < VT_MAXC) tabs_[cx_] = 0; break; }
    case 't': {
        int m = np_ > 0 ? params_[0] : 0;
        if (m == 18 || m == 19) {
            int n = 0; buf[n++] = 0x1b; buf[n++] = '['; buf[n++] = '8'; buf[n++] = ';';
            n += vt_itoa(buf + n, rows_); buf[n++] = ';'; n += vt_itoa(buf + n, cols_); buf[n++] = 't';
            put_reply_n(buf, n);
        }
        break;
    }
    default: break;                                   // q (cursor style), p, etc (satoru)
    }
    dirty_ = true;
}

void Vt::do_esc(char c) {
    switch (c) {
    case '7': save_cursor(); break;
    case '8': restore_cursor(); break;
    case 'D': line_feed(); wrap_pending_ = false; break;
    case 'E': cx_ = 0; line_feed(); wrap_pending_ = false; break;
    case 'M': reverse_index(); wrap_pending_ = false; break;
    case 'H': if (cx_ < VT_MAXC) tabs_[cx_] = 1; break;
    case 'c': reset(); break;
    default: break;                                   // = > (keypad), \ (ST), etc (satoru)
    }
    dirty_ = true;
}

void Vt::do_osc() {
    osc_[osc_n_ < (int)sizeof(osc_) - 1 ? osc_n_ : (int)sizeof(osc_) - 1] = 0;
    // OSC 10 / 11 ; ? : colour queries (tuis use them to pick a light or dark theme) (satoru)
    int code = 0, i = 0;
    while (osc_[i] >= '0' && osc_[i] <= '9') { code = code * 10 + (osc_[i] - '0'); i++; }
    if ((code == 10 || code == 11) && osc_[i] == ';' && osc_[i + 1] == '?') {
        uint32_t col = code == 10 ? def_fg_ : def_bg_;
        static const char hx[] = "0123456789abcdef";
        char r[64]; int n = 0;
        r[n++] = 0x1b; r[n++] = ']'; n += vt_itoa(r + n, code);
        const char* p = ";rgb:"; while (*p) r[n++] = *p++;
        for (int k = 2; k >= 0; k--) {
            uint32_t v = (col >> (k * 8)) & 0xFF;
            r[n++] = hx[v >> 4]; r[n++] = hx[v & 15]; r[n++] = hx[v >> 4]; r[n++] = hx[v & 15];
            if (k) r[n++] = '/';
        }
        r[n++] = 0x1b; r[n++] = '\\';
        put_reply_n(r, n);
    }
}

void Vt::Feed(const uint8_t* d, int n) {
    for (int k = 0; k < n; k++) {
        uint8_t b = d[k];
        switch (state_) {
        case S_GROUND:
            if (utf_need_ > 0) {
                if ((b & 0xC0) == 0x80) {
                    utf_cp_ = (utf_cp_ << 6) | (b & 0x3F);
                    if (--utf_need_ == 0) print_cp(utf_cp_);
                    break;
                }
                utf_need_ = 0; print_cp(0xFFFD);       // truncated sequence: fall through and reprocess b (satoru)
            }
            if (b >= 0x20 && b < 0x7F) { print_cp(b); break; }
            if (b == 0x1B) { state_ = S_ESC; break; }
            if (b < 0x20) { exec_c0(b); break; }
            if (b >= 0xC2 && b <= 0xDF) { utf_cp_ = b & 0x1F; utf_need_ = 1; }
            else if (b >= 0xE0 && b <= 0xEF) { utf_cp_ = b & 0x0F; utf_need_ = 2; }
            else if (b >= 0xF0 && b <= 0xF4) { utf_cp_ = b & 0x07; utf_need_ = 3; }
            else if (b >= 0x80) print_cp(0xFFFD);
            break;
        case S_ESC:
            if (b == '[') { state_ = S_CSI; np_ = 0; have_param_ = false; priv_ = 0; inter_ = 0; for (int i = 0; i < 32; i++) params_[i] = 0; }
            else if (b == ']') { state_ = S_OSC; osc_n_ = 0; }
            else if (b == 'P' || b == '_' || b == '^' || b == 'X') state_ = S_STR;
            else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%' || b == ' ') {
                inter_ = (char)b; state_ = S_DESIG;
            }
            else if (b == 0x1B) { /* stay */ }
            else if (b < 0x20) { exec_c0(b); state_ = S_GROUND; }
            else { do_esc((char)b); state_ = S_GROUND; }
            break;
        case S_DESIG:
            if (inter_ == '(') g0_gfx_ = (b == '0');
            else if (inter_ == ')') g1_gfx_ = (b == '0');
            else if (inter_ == '#' && b == '8') {       // DECALN: fill with E (satoru)
                for (int r = 0; r < rows_; r++) for (int c = 0; c < cols_; c++) { VtCell& x = scr_[r * VT_MAXC + c]; x.cp = 'E'; x.fg = def_fg_; x.bg = def_bg_; x.attr = 0; }
            }
            state_ = S_GROUND; break;
        case S_CSI:
            if (b >= '0' && b <= '9') {
                if (np_ == 0) np_ = 1;
                if (np_ <= 32) { int v = params_[np_ - 1]; if (v < 100000) params_[np_ - 1] = v * 10 + (b - '0'); }
                have_param_ = true;
            } else if (b == ';' || b == ':') {
                if (np_ == 0) np_ = 1;
                if (np_ < 32) np_++;
            } else if (b >= '<' && b <= '?') {
                priv_ = (char)b;
            } else if (b >= 0x20 && b <= 0x2F) {
                inter_ = (char)b;
            } else if (b >= 0x40 && b <= 0x7E) {
                if (have_param_ == false && np_ > 0) { /* trailing empty param counts as 0 */ }
                do_csi((char)b); state_ = S_GROUND;
            } else if (b == 0x1B) { state_ = S_ESC; }
            else if (b == 0x18 || b == 0x1A) { state_ = S_GROUND; }
            else if (b < 0x20) { exec_c0(b); }
            break;
        case S_OSC:
            if (b == 0x07) { do_osc(); state_ = S_GROUND; }
            else if (b == 0x1B) state_ = S_OSC_ESC;
            else if (b == 0x18 || b == 0x1A) state_ = S_GROUND;
            else if (osc_n_ < (int)sizeof(osc_) - 1) osc_[osc_n_++] = (char)b;
            break;
        case S_OSC_ESC:
            if (b == '\\') { do_osc(); state_ = S_GROUND; }
            else { state_ = S_ESC; k--; }               // a new escape: reprocess (satoru)
            break;
        case S_STR:
            if (b == 0x1B) state_ = S_STR_ESC;
            else if (b == 0x07 || b == 0x18 || b == 0x1A) state_ = S_GROUND;
            break;
        case S_STR_ESC:
            if (b == '\\') state_ = S_GROUND;
            else { state_ = S_ESC; k--; }
            break;
        }
    }
}

// end (satoru)
