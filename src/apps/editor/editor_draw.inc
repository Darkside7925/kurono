// editor_draw.inc: frame composition, the text viewport with syntax colours, selections,
// search hits, brackets and caret, the find bar, status line and scroll indicator. included
// by text_editor.cpp only. (satoru)

uint32_t TextEditorApp::TokColor(int kind) {
    switch (kind) {
    case ed::TK_KEYWORD: return col.keyword;
    case ed::TK_TYPE: return col.type;
    case ed::TK_STRING: return col.string;
    case ed::TK_COMMENT: return col.comment;
    case ed::TK_NUMBER: return col.number;
    case ed::TK_PREPROC: return col.preproc;
    case ed::TK_FUNC: return col.func;
    case ed::TK_OP: return col.op;
    case ed::TK_PUNCT: return col.punct;
    case ed::TK_HEADING: return col.heading;
    case ed::TK_LINK: return col.link;
    default: return col.text;
    }
}

void TextEditorApp::DrawScrollbar(kui::Canvas& c, int x, int y, int w, int h) {
    ed::Doc* d = D();
    if (d->nlines <= 1 || h < 20) return;
    int total = d->nlines;
    int thumb = h * (h / line_h) / total;
    if (thumb < 24) thumb = 24;
    if (thumb > h) thumb = h;
    int max_top = total - (h / line_h);
    if (max_top < 1) max_top = 1;
    int pos = (h - thumb) * clampi(d->top, 0, max_top) / max_top;
    c.RoundRect(x, y, w, h, 3, col.line, 90);
    c.RoundRect(x, y + pos, w, thumb, 3, col.text2, 150);
}

void TextEditorApp::DrawEditor(kui::Canvas& c) {
    ed::Doc* d = D();
    int area_x = sidebar ? side_w : 0;
    int area_w = cw - area_x;
    body_y = menu_h + tab_h;
    int body_h = ch - body_y - status_h;
    if (body_h < 10) body_h = 10;
    c.FillRect(area_x, body_y, area_w, body_h, col.bg);

    int nd = 1;
    int mxline = d->nlines;
    while (mxline >= 10) { nd++; mxline /= 10; }
    gutter_w = show_numbers ? (pad * 2 + nd * UiWidth("0") + 6) : 0;
    if (gutter_w < 38) gutter_w = 38;
    if (!show_numbers) gutter_w = 0;

    int fb_reserve = find_open ? (38 * kui::CurrentTheme().ui_scale / 100) : 0;
    if (fb_reserve < 32 && find_open) fb_reserve = 32;
    fb_y = body_y;
    fb_h = fb_reserve;
    ed_y = body_y + fb_reserve + 4;
    ed_x = area_x + gutter_w + pad;
    ed_w = cw - ed_x - 10;
    ed_h = ch - status_h - ed_y - 4;
    if (ed_w < 40) ed_w = 40;
    if (ed_h < line_h) ed_h = line_h;

    if (show_numbers) {
        c.FillRect(area_x, body_y, gutter_w, body_h, col.gutter_bg);
        c.FillRect(area_x + gutter_w - 1, body_y, 1, body_h, col.line);
    }
    if (find_open) {
        c.FillRect(area_x, body_y, area_w, fb_reserve, col.panel2);
        c.FillRect(area_x, body_y + fb_reserve - 1, area_w, 1, col.line);
    }

    EnsureCaretVisible();
    FindBrackets();

    int max_cols = (ed_w - 2) / char_w;
    if (max_cols < 2) max_cols = 2;
    int cur_row = ed::edx_line_of(d, d->cur);
    int cur_disp = CursorDisplayCol(d);
    bool sel = ed::edx_has_sel(d);
    int sel_a = 0, sel_b = 0;
    if (sel) ed::edx_sel(d, &sel_a, &sel_b);
    int srow = sel ? ed::edx_line_of(d, sel_a) : -1;
    int erow = sel ? ed::edx_line_of(d, sel_b) : -1;
    int scol = sel ? sel_a - ed::edx_line_start(d, srow) : 0;
    int ecol = sel ? sel_b - ed::edx_line_start(d, erow) : 0;

    static char disp[16384];
    static int bmap[16384];
    static ed::Tok toks[HL_MAX_SPANS];
    char rawline[8192];
    int y = ed_y;
    int rows_drawn = 0;
    int caret_drawn = 0;
    bool caret_on = focused && (((frame_now - last_key_ms) < 500 || (((frame_now - last_key_ms) / 500) & 1) == 0));

    for (int row = d->top; row < d->nlines && rows_drawn < 4000; row++) {
        if (y >= ed_y + ed_h) break;
        int len = BuildDisplay(d, row, disp, sizeof(disp), bmap);
        ed::hl_ensure(d, row);
        int hlstate = (d->hl && row < d->hl_cap) ? d->hl[row] : 0;
        int ntok = ed::hl_line(d->lang, disp, len, hlstate, toks, HL_MAX_SPANS, nullptr);
        int segstart = wrap ? 0 : (d->xcol < len ? d->xcol : len);
        bool first = true;
        for (;;) {
            int segend = wrap ? WrapNext(disp, len, segstart, max_cols)
                              : (segstart + max_cols < len ? segstart + max_cols : len);
            if (segend < segstart) segend = segstart;

            if (row == cur_row) {
                c.FillRect(area_x + gutter_w, y, area_w - gutter_w - 1, line_h, col.curline);
            }

            // selection (satoru)
            if (sel && row >= srow && row <= erow) {
                int sl = (row == srow) ? scol : 0;
                int el = (row == erow) ? ecol : ed::edx_line_len(d, row);
                if (row == erow && ecol == 0 && erow > srow) el = 0;
                if (el > sl) {
                    int sbi = sl < 16000 ? sl : 16000;
                    int ebi = el < 16000 ? el : 16000;
                    int ds = bmap[sbi];
                    int de = bmap[ebi];
                    int a = ds > segstart ? ds : segstart;
                    int b2 = de < segend ? de : segend;
                    if (b2 > a) c.FillRect(ed_x + (a - segstart) * char_w, y, (b2 - a) * char_w, line_h, col.sel);
                }
            }

            // visible search hits (satoru)
            if (find_open && find_valid && find_q.len > 0 && len > 0) {
                int rl = ed::edx_line_len(d, row);
                if (rl > (int)sizeof(rawline) - 1) rl = (int)sizeof(rawline) - 1;
                ed::edx_copy(d, ed::edx_line_start(d, row), rl, rawline);
                int from = 0, ma, mb, hits = 0;
                while (hits < 64 && find_in_line(&re, find_regex, !find_case, find_q.text, find_q.len, rawline, rl, from, &ma, &mb)) {
                    int abs = ed::edx_line_start(d, row) + ma;
                    int ds = bmap[ma < 16000 ? ma : 16000];
                    int de = bmap[mb < 16000 ? mb : 16000];
                    int a = ds > segstart ? ds : segstart;
                    int b2 = de < segend ? de : segend;
                    if (b2 > a) {
                        uint32_t mcol = (abs == match_a) ? col.match_cur : col.match;
                        c.FillRect(ed_x + (a - segstart) * char_w, y, (b2 - a) * char_w, line_h, mcol);
                    }
                    if (mb > ma) from = mb; else from = ma + 1;
                    if (from > rl) break;
                    hits++;
                }
            }

            // bracket cells (satoru)
            if (bracket_a >= 0 && bracket_b >= 0) {
                int ls = ed::edx_line_start(d, row);
                int le = ls + ed::edx_line_len(d, row);
                int ba = -1;
                if (bracket_a >= ls && bracket_a < le) ba = bracket_a - ls;
                if (bracket_b >= ls && bracket_b < le) ba = bracket_b - ls;
                if (ba >= 0) {
                    int ds = bmap[ba < 16000 ? ba : 16000];
                    int a = ds > segstart ? ds : segstart;
                    if (a < segend) c.RoundRect(ed_x + (a - segstart) * char_w - 1, y + 2, char_w + 2, line_h - 4, 2, col.bracket, 200);
                }
            }

            // syntax coloured text (satoru)
            int ty = y + (line_h - (int)px_mono) / 2 - 1;
            if (ty < y) ty = y;
            int p = segstart;
            for (int ti = 0; ti < ntok; ti++) {
                ed::Tok* tk = &toks[ti];
                int ts = tk->start > segstart ? tk->start : segstart;
                int te = tk->start + tk->len < segend ? tk->start + tk->len : segend;
                if (te <= ts) continue;
                if (ts > p) c.TextN(ed_x + (p - segstart) * char_w, ty, mono_face, px_mono, disp + p, ts - p, col.text);
                c.TextN(ed_x + (ts - segstart) * char_w, ty, mono_face, px_mono, disp + ts, te - ts, TokColor(tk->kind));
                p = te;
            }
            if (p < segend) c.TextN(ed_x + (p - segstart) * char_w, ty, mono_face, px_mono, disp + p, segend - p, col.text);

            // caret (satoru)
            if (!caret_drawn && row == cur_row && cur_disp >= segstart && cur_disp <= segend) {
                int cx = ed_x + (cur_disp - segstart) * char_w + 1;
                if (caret_on) c.FillRect(cx, y + 3, 2, line_h - 6, col.accent);
                caret_drawn = 1;
            }

            // line number on the first visual row (satoru)
            if (first && show_numbers) {
                char num[16];
                int_to_str(row + 1, num, sizeof(num));
                int nw = UiWidth(num);
                int ny = y + (line_h - (int)px_ui) / 2 - 2;
                if (ny < y) ny = y;
                UiText(c, area_x + gutter_w - pad - nw, ny, num, row == cur_row ? col.gutter_active : col.gutter_text);
            }

            y += line_h;
            rows_drawn++;
            first = false;
            if (y >= ed_y + ed_h || rows_drawn >= 4000) break;
            if (segend >= len) break;
            segstart = segend;
        }
    }

    DrawScrollbar(c, cw - 7, body_y + 4, 5, body_h - 8);

    (void)caret_drawn;
}

void TextEditorApp::DrawFindBar(kui::Canvas& c) {
    if (!find_open) return;
    int area_x = sidebar ? side_w : 0;
    int scale = kui::CurrentTheme().ui_scale;
    int h = fb_h - 8;
    if (h < 22) h = 22;
    int y = fb_y + 4;
    int right = cw - 10;
    int close_w = 26 * scale / 100; if (close_w < 22) close_w = 22;
    int nav_w = 26 * scale / 100; if (nav_w < 22) nav_w = 22;
    int tog_w = 30 * scale / 100; if (tog_w < 24) tog_w = 24;
    int all_w = 38 * scale / 100; if (all_w < 30) all_w = 30;
    fb_close_x = right - close_w;
    fb_next_x = fb_close_x - nav_w - 4;
    fb_prev_x = fb_next_x - nav_w - 2;
    fb_regex_x = fb_prev_x - tog_w - 4;
    fb_case_x = fb_regex_x - tog_w - 2;
    int fields_right = fb_case_x - 10;
    int x = area_x + pad;
    int avail = fields_right - x;
    if (avail < 80) avail = 80;
    if (find_replace) {
        fb_qw = (avail - 16) / 2;
        fb_rw = avail - fb_qw - 8;
    } else {
        fb_qw = avail;
        fb_rw = 0;
    }
    fb_qx = x; fb_qy = y;
    fb_rx = x + fb_qw + 8;
    DrawInput(c, &find_q, fb_qx, fb_qy, fb_qw, h, find_field == 0);
    if (find_replace) DrawInput(c, &find_repl, fb_rx, fb_qy, fb_rw, h, find_field == 1);
    if (find_q.len == 0) {
        c.PushClip(fb_qx + 2, fb_qy + 2, fb_qw - 4, h - 4);
        UiText(c, fb_qx + 10, fb_qy + (h - (int)px_ui) / 2 - 2, "Find", col.text2);
        c.PopClip();
    }
    if (find_replace && find_repl.len == 0) {
        c.PushClip(fb_rx + 2, fb_qy + 2, fb_rw - 4, h - 4);
        UiText(c, fb_rx + 10, fb_qy + (h - (int)px_ui) / 2 - 2, "Replace", col.text2);
        c.PopClip();
    }

    // toggles and navigation (satoru)
    uint32_t tg = find_case ? col.accent : col.panel3;
    c.RoundRect(fb_case_x, y, tog_w, h, 6, tg, 255);
    int tw = UiWidth("Aa");
    UiText(c, fb_case_x + (tog_w - tw) / 2, y + (h - (int)px_ui) / 2 - 2, "Aa", find_case ? col.button_text : col.text2);
    tg = find_regex ? col.accent : col.panel3;
    c.RoundRect(fb_regex_x, y, tog_w, h, 6, tg, 255);
    tw = UiWidth(".*");
    UiText(c, fb_regex_x + (tog_w - tw) / 2, y + (h - (int)px_ui) / 2 - 2, ".*", find_regex ? col.button_text : col.text2);
    bool hv = In(fb_prev_x, y, nav_w, h, mx, my);
    if (hv) c.RoundRect(fb_prev_x, y, nav_w, h, 6, col.panel3, 255);
    c.DrawLine(fb_prev_x + nav_w / 2 - 4, y + h / 2, fb_prev_x + nav_w / 2, y + h / 2 - 4, col.text2, 1);
    c.DrawLine(fb_prev_x + nav_w / 2, y + h / 2 - 4, fb_prev_x + nav_w / 2 + 4, y + h / 2, col.text2, 1);
    hv = In(fb_next_x, y, nav_w, h, mx, my);
    if (hv) c.RoundRect(fb_next_x, y, nav_w, h, 6, col.panel3, 255);
    c.DrawLine(fb_next_x + nav_w / 2 - 4, y + h / 2 + 1, fb_next_x + nav_w / 2, y + h / 2 + 5, col.text2, 1);
    c.DrawLine(fb_next_x + nav_w / 2, y + h / 2 + 5, fb_next_x + nav_w / 2 + 4, y + h / 2 + 1, col.text2, 1);
    if (find_replace) {
        int ax = fb_case_x - all_w - 8;
        if (ax > fb_qx + fb_qw + 4) {
            bool ha = In(ax, y, all_w, h, mx, my);
            c.RoundRect(ax, y, all_w, h, 6, ha ? col.panel3 : col.panel2, 255);
            UiText(c, ax + (all_w - UiWidth("All")) / 2, y + (h - (int)px_ui) / 2 - 2, "All", col.text2);
        }
    }
    hv = In(fb_close_x, y, close_w, h, mx, my);
    if (hv) c.RoundRect(fb_close_x, y, close_w, h, 6, col.panel3, 255);
    c.DrawLine(fb_close_x + 8, y + h / 2 - 4, fb_close_x + close_w - 8, y + h / 2 + 4, col.text2, 1);
    c.DrawLine(fb_close_x + close_w - 8, y + h / 2 - 4, fb_close_x + 8, y + h / 2 + 4, col.text2, 1);
    if (!find_valid && find_q.len > 0 && find_err[0]) {
        int ex = fb_qx + fb_qw + 10;
        if (find_replace) ex = fb_rx + fb_rw + 10;
        UiTextFit(c, ex, y + (h - (int)px_ui) / 2 - 2, find_err, col.err, fb_case_x - ex - 8);
    }
}

void TextEditorApp::Status(kui::Canvas& c, int x, int y, int w) {
    (void)c; (void)x; (void)y; (void)w;
}

void TextEditorApp::DrawStatus(kui::Canvas& c) {
    int y = ch - status_h;
    c.FillRect(0, y, cw, status_h, col.panel2);
    c.FillRect(0, y, cw, 1, col.line);
    ed::Doc* d = D();
    int row = ed::edx_line_of(d, d->cur);
    int coln = d->cur - ed::edx_line_start(d, row);
    char left[128]; left[0] = 0;
    sapp(left, "Ln ", sizeof(left));
    char n[16]; int_to_str(row + 1, n, sizeof(n)); sapp(left, n, sizeof(left));
    sapp(left, ", Col ", sizeof(left));
    int_to_str(coln + 1, n, sizeof(n)); sapp(left, n, sizeof(left));
    if (ed::edx_has_sel(d)) {
        int a, b; ed::edx_sel(d, &a, &b);
        sapp(left, "  (", sizeof(left));
        int_to_str(b - a, n, sizeof(n)); sapp(left, n, sizeof(left));
        sapp(left, " selected)", sizeof(left));
    }
    int ty = y + (status_h - (int)px_ui) / 2 - 2;
    if (ty < y) ty = y;
    UiText(c, pad + 2, ty, left, col.text2);

    char mid[96]; mid[0] = 0;
    sapp(mid, ed::lang_name(d->lang), sizeof(mid));
    sapp(mid, "   UTF-8   LF   Spaces: 4", sizeof(mid));
    if (wrap) sapp(mid, "   Wrap", sizeof(mid));
    UiText(c, pad + 2 + UiWidth(left) + 24, ty, mid, col.text2);

    char right[160]; right[0] = 0;
    bool fresh = status_msg[0] && (frame_now - status_ms) < 3000;
    if (fresh) {
        scpy(right, status_msg, sizeof(right));
    } else {
        if (d->modified) { sapp(right, "Modified", sizeof(right)); }
        else sapp(right, "Saved", sizeof(right));
        sapp(right, "   ", sizeof(right));
        int_to_str(d->nlines, n, sizeof(n)); sapp(right, n, sizeof(right));
        sapp(right, " lines", sizeof(right));
    }
    int rw = UiWidth(right);
    int rx = cw - rw - pad - 14;
    if (fresh) UiText(c, rx, ty, right, col.accent);
    else {
        if (d->modified) c.Circle(rx - 10, y + status_h / 2, 3, col.modified, 255);
        UiText(c, rx, ty, right, col.text2);
    }
}

void TextEditorApp::PositionToXY(int row, int disp_col, int* px, int* py, bool* visible) {
    ed::Doc* d = D();
    int max_cols = (ed_w - 2) / char_w;
    if (max_cols < 2) max_cols = 2;
    static char disp[16384];
    int y = ed_y;
    *px = ed_x; *py = ed_y; *visible = false;
    for (int r = d->top; r < d->nlines; r++) {
        if (y >= ed_y + ed_h) break;
        int len = BuildDisplay(d, r, disp, sizeof(disp), nullptr);
        int segstart = wrap ? 0 : (d->xcol < len ? d->xcol : len);
        for (;;) {
            int segend = wrap ? WrapNext(disp, len, segstart, max_cols)
                              : (segstart + max_cols < len ? segstart + max_cols : len);
            if (r == row && disp_col >= segstart && disp_col <= segend) {
                *px = ed_x + (disp_col - segstart) * char_w;
                *py = y;
                *visible = (y >= ed_y && y + line_h <= ed_y + ed_h);
                return;
            }
            y += line_h;
            if (segend >= len) break;
            segstart = segend;
        }
    }
}

int TextEditorApp::XYToPos(int x, int y) {
    ed::Doc* d = D();
    int max_cols = (ed_w - 2) / char_w;
    if (max_cols < 2) max_cols = 2;
    static char disp[16384];
    static int bmap[16384];
    int cy = ed_y;
    for (int row = d->top; row < d->nlines; row++) {
        int len = BuildDisplay(d, row, disp, sizeof(disp), bmap);
        int segstart = wrap ? 0 : (d->xcol < len ? d->xcol : len);
        for (;;) {
            int segend = wrap ? WrapNext(disp, len, segstart, max_cols)
                              : (segstart + max_cols < len ? segstart + max_cols : len);
            if (y >= cy && y < cy + line_h) {
                int ccol = (x - ed_x) / char_w;
                if (ccol < 0) ccol = 0;
                int dispcol = segstart + ccol;
                if (dispcol > len) dispcol = len;
                int bi = 0;
                for (int i = 0; i <= len; i++) {
                    if (bmap[i] <= dispcol) bi = i; else break;
                }
                return ed::edx_line_start(d, row) + bi;
            }
            cy += line_h;
            if (segend >= len) break;
            segstart = segend;
        }
        if (cy >= ed_y + ed_h && y >= cy) break;
    }
    return clampi(ed::edx_len(d), 0, ed::edx_len(d));
}

void TextEditorApp::OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now_ms) {
    win = &w;
    frame_now = now_ms;
    cw = c.Width();
    ch = c.Height();
    UpdateTheme();
    UpdateMetrics();
    if (bg_applied != col.panel) {
        w.SetBackground(col.panel);
        bg_applied = col.panel;
    }
    if (ntabs <= 0 || active < 0 || active >= ntabs) { AddDoc(nullptr); }
    if (wrap) D()->xcol = 0;
    c.FillRect(0, 0, cw, ch, col.bg);
    ResetMenuRects();
    DrawMenuBar(c);
    DrawTabBar(c);
    body_y = menu_h + tab_h;
    DrawSidebar(c);
    DrawEditor(c);
    DrawFindBar(c);
    DrawStatus(c);
    if (menu_open >= 0) DrawDropdown(c);
    DialogDraw(c);
    if (focused) {
        uint32_t since = frame_now - last_key_ms;
        uint32_t next_flip = (since < 500) ? last_key_ms + 500
                                          : last_key_ms + ((since / 500) + 1) * 500;
        w.RequestFrameAt(next_flip);
    }
}

// end (satoru)
