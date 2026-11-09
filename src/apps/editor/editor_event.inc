// editor_event.inc: keyboard and pointer routing, mouse selection and drag, wheel scrolling,
// tab switching, the find bar interaction and the app entry points. included by
// text_editor.cpp only. (satoru)

bool TextEditorApp::HandleShortcut(kui::Window& w, int key, bool ctrl, bool shift) {
    if (!ctrl) return false;
    ed::Doc* d = D();
    if (key == kui::K_TAB) {
        if (ntabs > 1) {
            active = shift ? (active + ntabs - 1) % ntabs : (active + 1) % ntabs;
            SyncTitle(w);
            EnsureCaretVisible();
        }
        return true;
    }
    if (key == kui::K_BTAB) {
        if (ntabs > 1) { active = (active + ntabs - 1) % ntabs; SyncTitle(w); EnsureCaretVisible(); }
        return true;
    }
    if (key == kui::K_HOME) { MoveCaret(0, shift); return true; }
    if (key == kui::K_END) { MoveCaret(ed::edx_len(d), shift); return true; }
    if (key == kui::K_LEFT) { MoveWords(-1, shift); return true; }
    if (key == kui::K_RIGHT) { MoveWords(1, shift); return true; }
    if (key == kui::K_UP) { MoveLines(-1, shift); return true; }
    if (key == kui::K_DOWN) { MoveLines(1, shift); return true; }
    switch (key) {
    case 's':
        if (shift) OpenDialog(ED_DLG_SAVEAS, active, ED_ACT_NONE);
        else SaveDoc(active, nullptr, false);
        return true;
    case 'o': OpenDialog(ED_DLG_OPEN, active, ED_ACT_NONE); return true;
    case 'n': AddDoc(nullptr); return true;
    case 'w': PromptCloseDoc(w, active, false); return true;
    case 'f': FindOpen(false); return true;
    case 'h': FindOpen(true); return true;
    case 'g': GotoLine(w); return true;
    case 'z': if (shift) Redo(); else Undo(); return true;
    case 'y': Redo(); return true;
    case 'a': SelectAll(); return true;
    case 'c': CopySel(); return true;
    case 'x': CutSel(); return true;
    case 'v': PasteClip(); return true;
    case 'b': sidebar = !sidebar; return true;
    case '=': case '+':
        if (zoom_pct < 200) { zoom_pct += 10; UpdateMetrics(); EnsureCaretVisible(); }
        return true;
    case '-':
        if (zoom_pct > 70) { zoom_pct -= 10; UpdateMetrics(); EnsureCaretVisible(); }
        return true;
    case '0': zoom_pct = 100; UpdateMetrics(); EnsureCaretVisible(); return true;
    default: break;
    }
    return false;
}

bool TextEditorApp::OnEvent(kui::Window& w, const kui::Event& e) {
    win = &w;
    switch (e.type) {
    case kui::EventType::Focus:
        focused = true;
        return true;
    case kui::EventType::Blur:
        focused = false;
        dragging = false;
        return true;
    case kui::EventType::PointerLeave:
        pointer_inside = false;
        dragging = false;
        mx = -1; my = -1;
        return true;
    case kui::EventType::ThemeChanged:
        UpdateTheme();
        UpdateMetrics();
        return true;
    case kui::EventType::Resize:
        cw = e.w; ch = e.h;
        EnsureCaretVisible();
        return true;
    case kui::EventType::PointerMove: {
        mx = e.x; my = e.y;
        pointer_inside = true;
        if (menu_open >= 0 && my < menu_h) {
            static const char* labels[4] = { "File", "Edit", "View", "Help" };
            for (int i = 0; i < 4; i++) {
                int bw = UiWidth(labels[i]) + 18;
                if (In(menu_x[i], 2, bw, menu_h - 4, mx, my)) { menu_open = i; break; }
            }
        }
        if (dragging && drag_in_text) {
            ed::Doc* d = D();
            if (my < ed_y && d->top > 0) d->top--;
            else if (my > ed_y + ed_h && d->top < d->nlines - 1) d->top++;
            int pos = XYToPos(mx, my);
            ed::edx_select(d, pos, true);
            EnsureCaretVisible();
            last_key_ms = kui::NowMs();
            return true;
        }
        return true;
    }
    case kui::EventType::PointerDown:
        mx = e.x; my = e.y;
        if (e.button == kui::Button::Middle) {
            bool cl = false;
            int t = TabAt(e.x, e.y, &cl);
            if (t >= 0) { PromptCloseDoc(w, t, false); return true; }
            if (dlg == ED_DLG_NONE && e.y >= ed_y - 4 && e.y < body_y + (ch - body_y - status_h)) {
                ed::edx_select(D(), XYToPos(e.x, e.y), false);
                PastePrimary();
                return true;
            }
        }
        return false;
    case kui::EventType::PointerUp:
        mx = e.x; my = e.y;
        if (dragging) {
            dragging = false;
            drag_in_text = false;
            if (ed::edx_has_sel(D())) CopySelTo(true);
            return true;
        }
        return false;
    case kui::EventType::Scroll: {
        if (dlg != ED_DLG_NONE) return false;
        int steps = e.scroll_steps;
        if (steps == 0) steps = e.dy > 0 ? 1 : (e.dy < 0 ? -1 : 0);
        if (steps == 0) return false;
        mx = e.x; my = e.y;
        if (sidebar && mx < side_w && my >= body_y) {
            int rows = (side_h - 64) / row_h;
            if (rows < 1) rows = 1;
            side_scroll += steps;
            int maxs = side_count + 1 - rows;
            if (maxs < 0) maxs = 0;
            if (side_scroll > maxs) side_scroll = maxs;
            if (side_scroll < 0) side_scroll = 0;
            return true;
        }
        ed::Doc* d = D();
        if (wrap) {
            d->top = clampi(d->top + steps, 0, d->nlines - 1);
        } else if (e.mods & kui::M_SHIFT) {
            d->xcol = clampi(d->xcol + steps * 4, 0, 1 << 20);
        } else {
            d->top = clampi(d->top + steps, 0, d->nlines - 1);
        }
        return true;
    }
    case kui::EventType::Click: {
        mx = e.x; my = e.y;
        pointer_inside = true;
        if (dlg != ED_DLG_NONE) return DialogClick(w, e.x, e.y, e.clicks);
        if (MenuClick(e.x, e.y)) return true;
        if (find_open && e.y >= fb_y && e.y < fb_y + fb_h) {
            int x = e.x, y = e.y;
            if (In(fb_qx, fb_qy, fb_qw, fb_h - 8, x, y)) {
                find_field = 0;
                InputClick(&find_q, x - (fb_qx + 10), y, fb_qw);
                last_key_ms = kui::NowMs();
                return true;
            }
            if (find_replace && In(fb_rx, fb_qy, fb_rw, fb_h - 8, x, y)) {
                find_field = 1;
                InputClick(&find_repl, x - (fb_rx + 10), y, fb_rw);
                last_key_ms = kui::NowMs();
                return true;
            }
            if (In(fb_case_x, fb_qy, 30, fb_h - 8, x, y)) {
                find_case = !find_case; FindCompile(); have_match = false;
                return true;
            }
            if (In(fb_regex_x, fb_qy, 30, fb_h - 8, x, y)) {
                find_regex = !find_regex; FindCompile(); have_match = false;
                return true;
            }
            if (In(fb_prev_x, fb_qy, 26, fb_h - 8, x, y)) { FindNext(true, true); return true; }
            if (In(fb_next_x, fb_qy, 26, fb_h - 8, x, y)) { FindNext(false, true); return true; }
            if (In(fb_close_x, fb_qy, 26, fb_h - 8, x, y)) { FindClose(); return true; }
            if (find_replace) {
                int scale = kui::CurrentTheme().ui_scale;
                int all_w = 38 * scale / 100; if (all_w < 30) all_w = 30;
                int ax = fb_case_x - all_w - 8;
                if (x >= ax && x < ax + all_w && y >= fb_qy && y < fb_qy + fb_h - 8) { ReplaceAll(); return true; }
            }
            return true;
        }
        if (e.y >= menu_h && e.y < menu_h + tab_h) {
            if (e.x >= plus_x && e.x < plus_x + plus_w) { AddDoc(nullptr); SyncTitle(w); return true; }
            bool cl = false;
            int t = TabAt(e.x, e.y, &cl);
            if (t >= 0) {
                if (cl) PromptCloseDoc(w, t, false);
                else { active = t; SyncTitle(w); EnsureCaretVisible(); }
                return true;
            }
            return true;
        }
        if (sidebar && e.x < side_w && e.y >= body_y) return SidebarClick(e.x, e.y);
        if (e.y >= ed_y - 4 && e.y < body_y + (ch - body_y - status_h)) {
            ed::Doc* d = D();
            int pos = XYToPos(e.x, e.y);
            if (e.clicks >= 2 && e.mods == 0) {
                ed::edx_select(d, pos, false);
                ed::edx_select_word(d, pos);
            } else {
                ed::edx_select(d, pos, (e.mods & kui::M_SHIFT) != 0);
            }
            dragging = true;
            drag_in_text = true;
            last_key_ms = kui::NowMs();
            goal_col = -1;
            FindBrackets();
            return true;
        }
        return true;
    }
    case kui::EventType::KeyDown: {
        int key = e.key;
        if (key == kui::K_SHIFT || key == kui::K_CTRL || key == kui::K_ALT || key == kui::K_SUPER) return false;
        bool ctrl = (e.mods & kui::M_CTRL) != 0;
        bool shift = (e.mods & kui::M_SHIFT) != 0;
        if (key >= 1 && key <= 26 && key != kui::K_BS && key != kui::K_TAB && key != kui::K_ENTER) { key += 96; ctrl = true; }
        if (ctrl && key >= 'A' && key <= 'Z') key += 32;
        ctrl_key = ctrl; shift_key = shift;
        if (dlg != ED_DLG_NONE) {
            bool rd = false;
            if (DialogKey(w, e, &rd)) return rd;
            return true;
        }
        if (menu_open >= 0) {
            if (key == kui::K_ESC) { menu_open = -1; return true; }
            if (key == kui::K_ENTER && menu_hover >= 0) {
                int m = menu_open;
                menu_open = -1;
                RunMenuAction(m, menu_hover);
                return true;
            }
            menu_open = -1;
            return true;
        }
        if (key == kui::K_ESC) {
            if (find_open) { FindClose(); return true; }
            ed::Doc* d = D();
            d->anchor = d->cur;
            return true;
        }
        last_key_ms = kui::NowMs();
        if (ctrl) {
            if (key == kui::K_TAB || key == kui::K_BTAB) return HandleShortcut(w, key, ctrl, shift);
            if (HandleShortcut(w, key, ctrl, shift)) return true;
            return false;
        }
        if (find_open) {
            if (key == kui::K_TAB) {
                if (find_replace) find_field = find_field ? 0 : 1;
                return true;
            }
            if (key == kui::K_ENTER) {
                if (find_replace && find_field == 1) ReplaceCurrent();
                else FindNext(shift, true);
                return true;
            }
            if (key == (kui::K_F1 + 2)) { FindNext(shift, true); return true; }
            EdInput* in = find_cur();
            bool was_q = (find_field == 0);
            if (InputKey(in, e)) {
                if (was_q) { FindCompile(); have_match = false; }
                return true;
            }
            return true;
        }
        if (key == (kui::K_F1 + 2)) { FindOpen(false); FindNext(shift, true); return true; }
        switch (key) {
        case kui::K_ENTER: InsertNewlineAuto(); return true;
        case kui::K_BS: case kui::K_DEL_ASCII: DoBackspace(); return true;
        case kui::K_TAB: DoTab(false); return true;
        case kui::K_BTAB: DoTab(true); return true;
        case kui::K_DEL: DoDelete(); return true;
        case kui::K_UP: MoveLines(-1, shift); return true;
        case kui::K_DOWN: MoveLines(1, shift); return true;
        case kui::K_LEFT: {
            ed::Doc* d = D();
            MoveCaret(ed::edx_prev_char(d, d->cur), shift);
            return true;
        }
        case kui::K_RIGHT: {
            ed::Doc* d = D();
            MoveCaret(ed::edx_next_char(d, d->cur), shift);
            return true;
        }
        case kui::K_HOME: {
            ed::Doc* d = D();
            int row = ed::edx_line_of(d, d->cur);
            int ls = ed::edx_line_start(d, row);
            int first = ls;
            while (ed::edx_at(d, first) == ' ' || ed::edx_at(d, first) == '\t') first++;
            int target = (d->cur == first) ? ls : first;
            MoveCaret(target, shift);
            return true;
        }
        case kui::K_END: {
            ed::Doc* d = D();
            int row = ed::edx_line_of(d, d->cur);
            MoveCaret(ed::edx_line_start(d, row) + ed::edx_line_len(d, row), shift);
            return true;
        }
        case kui::K_PGUP: {
            int vis = ed_h / line_h;
            if (vis < 1) vis = 1;
            MoveLines(-vis, shift);
            return true;
        }
        case kui::K_PGDN: {
            int vis = ed_h / line_h;
            if (vis < 1) vis = 1;
            MoveLines(vis, shift);
            return true;
        }
        default: break;
        }
        if (e.mods & (kui::M_ALT | kui::M_SUPER)) return false;
        const char* t = e.text;
        int n = slen(t);
        char one[2];
        if (n <= 0) {
            if (key >= 32 && key < 127) { one[0] = (char)key; one[1] = 0; t = one; n = 1; }
            else return false;
        }
        InsertTyped(w, t, n);
        return true;
    }
    default:
        break;
    }
    return false;
}

// ---------------------------------------------------------------- entry points (satoru)

static kui::AppHandler* MakeEditor() {
    TextEditorApp* app = new TextEditorApp;
    if (g_open_path[0]) {
        app->LoadFile(g_open_path);
        ED_APPLOG("open-file", g_open_path);
        g_open_path[0] = 0;
    } else {
        ED_APPLOG("open", nullptr);
    }
    return app;
}

static const kui::WindowSpec kEditorSpec = {
    "Text Editor", "texteditor", 960, 620, 520, 340, -1, -1, true, true, 0, -1
};

#if defined(KUI_BACKEND_WAYLAND)
int main(int argc, char** argv) {
    if (argc > 1) scpy(g_open_path, argv[1], ED_PATH_MAX);
    return kui::Run(kEditorSpec, MakeEditor);
}
#else
KUI_APP(texteditor, kEditorSpec, MakeEditor)

int TextEditorApp::Open() {
    TaskMem::AppScope _tms(TaskMem::APP_EDITOR);
    g_open_path[0] = 0;
    return kui_launch_texteditor();
}

int TextEditorApp::OpenFile(const char* path) {
    TaskMem::AppScope _tms(TaskMem::APP_EDITOR);
    scpy(g_open_path, path, ED_PATH_MAX);
    return kui_launch_texteditor();
}
#endif

// end (satoru)
