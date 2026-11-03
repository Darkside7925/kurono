#pragma once
//  kurono os - vt100/xterm screen emulator for the desktop terminal (satoru)
//
//  a cell grid fed by a byte stream: csi cursor movement, erase, insert/delete, scroll
//  regions, sgr (16, 256 and truecolor), the alternate screen, bracketed paste, cursor
//  visibility, charset line drawing, utf-8, and the replies a tui asks for (device
//  status, device attributes, window size, osc 10/11 colour queries). no rendering in
//  here: the terminal app draws the grid. (satoru)

#include "../kernel/types.h"

#define VT_MAXC 200
#define VT_MAXR 80
#define VT_SCROLLBACK 400

enum {
    VA_BOLD = 1, VA_DIM = 2, VA_ITALIC = 4, VA_UNDER = 8, VA_INVERSE = 16, VA_STRIKE = 32, VA_HIDDEN = 64
};

struct VtCell {
    uint32_t cp;      // unicode codepoint, 0 or ' ' = blank (satoru)
    uint32_t fg;      // 0xAARRGGBB, already resolved (satoru)
    uint32_t bg;
    uint8_t  attr;
};

class Vt {
public:
    void Init(int cols, int rows, uint32_t def_fg, uint32_t def_bg);
    void SetDefaults(uint32_t def_fg, uint32_t def_bg);
    void Resize(int cols, int rows);
    void Feed(const uint8_t* d, int n);
    int  TakeReply(uint8_t* out, int max);     // bytes the terminal must send back to the program

    int  Cols() const { return cols_; }
    int  Rows() const { return rows_; }
    const VtCell& Cell(int r, int c) const { return scr_[r * VT_MAXC + c]; }
    // scrollback line `i` (0 = newest) (satoru)
    const VtCell* HistLine(int i) const;
    int  HistCount() const { return hist_n_; }

    int  CurRow() const { return cy_; }
    int  CurCol() const { return cx_ >= cols_ ? cols_ - 1 : cx_; }
    bool CursorVisible() const { return cursor_vis_; }
    bool BracketedPaste() const { return bpaste_; }
    bool AppCursorKeys() const { return appcur_; }
    bool AltScreen() const { return alt_; }
    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

private:
    VtCell* scr_;                 // active screen (satoru)
    int cols_ = 80, rows_ = 24;
    int cx_ = 0, cy_ = 0;
    int top_ = 0, bot_ = 23;      // scroll region (inclusive) (satoru)
    bool wrap_pending_ = false;
    bool autowrap_ = true, origin_ = false, cursor_vis_ = true, bpaste_ = false, appcur_ = false;
    bool alt_ = false, insert_ = false, dirty_ = true;
    uint32_t def_fg_ = 0xFFE6E6EA, def_bg_ = 0xFF101012;
    uint32_t fg_ = 0xFFE6E6EA, bg_ = 0xFF101012;
    uint8_t attr_ = 0;
    // saved cursor (decsc) + the main screen's cursor across alt-screen switches (satoru)
    int sx_ = 0, sy_ = 0; uint8_t sattr_ = 0; uint32_t sfg_ = 0, sbg_ = 0; bool swrap_ = false;
    int mx_ = 0, my_ = 0;
    uint8_t g0_gfx_ = 0, g1_gfx_ = 0, shift_out_ = 0;
    uint8_t tabs_[VT_MAXC];

    // parser (satoru)
    int state_ = 0;
    int params_[32]; int np_ = 0; bool have_param_ = false;
    char priv_ = 0; char inter_ = 0;
    uint32_t utf_cp_ = 0; int utf_need_ = 0;
    char osc_[256]; int osc_n_ = 0;
    uint8_t reply_[512]; int reply_n_ = 0;
    int hist_head_ = 0, hist_n_ = 0;

    void put_reply(const char* s);
    void put_reply_n(const char* s, int n);
    void print_cp(uint32_t cp);
    void exec_c0(uint8_t c);
    void do_csi(char final_);
    void do_esc(char c);
    void do_osc();
    void do_sgr();
    void do_mode(bool set);
    void line_feed();
    void reverse_index();
    void scroll_up(int n);
    void scroll_down(int n);
    void erase_cells(int r, int c0, int c1);
    void clear_screen_all();
    void switch_screen(bool to_alt);
    void blank_row(VtCell* row);
    void move_to(int r, int c);
    void save_cursor();
    void restore_cursor();
    void reset();
    uint32_t palette(int idx) const;
    VtCell main_[VT_MAXR * VT_MAXC];
    VtCell alt_buf_[VT_MAXR * VT_MAXC];
    VtCell hist_[VT_SCROLLBACK * VT_MAXC];
};

// end (satoru)
