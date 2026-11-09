// editor_menu.inc: the menu bar, tab strip, file sidebar and their hit tests. included by
// text_editor.cpp only. (satoru)

void TextEditorApp::ResetMenuRects() {
    for (int i = 0; i < 4; i++) {
        menu_x[i] = 0; menu_y[i] = menu_h; menu_w[i] = 0;
        int n = 0; menu_items(i, &n); menu_n[i] = n;
    }
}

void TextEditorApp::DrawMenuBar(kui::Canvas& c) {
    c.FillRect(0, 0, cw, menu_h, col.panel);
    c.FillRect(0, menu_h - 1, cw, 1, col.line);
    static const char* labels[4] = { "File", "Edit", "View", "Help" };
    int x = pad + 2;
    int hover_m = -1;
    for (int i = 0; i < 4; i++) {
        int lw = UiWidth(labels[i]);
        int bw = lw + 18;
        menu_x[i] = x;
        menu_y[i] = menu_h;
        int n = 0; menu_items(i, &n); menu_n[i] = n;
        bool over = In(x, 2, bw, menu_h - 4, mx, my);
        if (over) hover_m = i;
        if (menu_open == i) c.RoundRect(x, 2, bw, menu_h - 4, 6, col.panel3, 255);
        else if (over) c.RoundRect(x, 2, bw, menu_h - 4, 6, col.tab_hover, 255);
        uint32_t tc = (menu_open == i) ? col.accent : col.text;
        int ty = (menu_h - (int)px_ui) / 2 - 2;
        if (ty < 2) ty = 2;
        UiText(c, x + 9, ty, labels[i], tc);
        x += bw + 4;
    }
    if (hover_m >= 0 && menu_open >= 0 && menu_open != hover_m) menu_open = hover_m;
    char right[64];
    if (ntabs > 1) { int_to_str(ntabs, right, sizeof(right)); sapp(right, " tabs", sizeof(right)); }
    else scpy(right, "Text Editor", sizeof(right));
    int rw = UiWidth(right);
    int ry = (menu_h - (int)px_ui) / 2 - 2;
    if (ry < 2) ry = 2;
    UiText(c, cw - rw - pad - 2, ry, right, col.text2);
}

void TextEditorApp::DrawDropdown(kui::Canvas& c) {
    if (menu_open < 0 || menu_open > 3 || dlg != ED_DLG_NONE) return;
    int n = 0;
    const EdMenuItem* items = menu_items(menu_open, &n);
    int w = 220;
    for (int i = 0; i < n; i++) {
        if (items[i].act == A_NONE) continue;
        int tw = UiWidth(items[i].label) + UiWidth(items[i].key) + 58;
        if (tw > w) w = tw;
    }
    int rh = row_h + 2;
    int h = 8;
    for (int i = 0; i < n; i++) h += items[i].act == A_NONE ? 9 : rh;
    int x = menu_x[menu_open];
    if (x + w > cw - 4) x = cw - w - 4;
    if (x < 4) x = 4;
    int y = menu_h;
    menu_y[menu_open] = y;
    menu_w[menu_open] = w;
    c.Shadow(x, y, w, h, 10, 18, 4, 70);
    c.RoundRect(x, y, w, h, 10, col.panel2, 255);
    c.RoundOutline(x, y, w, h, 10, col.line, 255);
    int iy = y + 4;
    menu_hover = -1;
    for (int i = 0; i < n; i++) {
        if (items[i].act == A_NONE) {
            c.FillRect(x + 10, iy + 4, w - 20, 1, col.line);
            iy += 9;
            continue;
        }
        bool hot = In(x + 4, iy, w - 8, rh, mx, my);
        if (hot) {
            menu_hover = i;
            c.RoundRect(x + 4, iy, w - 8, rh, 6, col.accent, col.dark ? 70 : 46);
        }
        int ty = iy + (rh - (int)px_ui) / 2 - 2;
        if (ty < iy) ty = iy + 2;
        UiText(c, x + 14, ty, items[i].label, col.text);
        if (items[i].key[0]) {
            int kw = UiWidth(items[i].key);
            UiText(c, x + w - kw - 14, ty, items[i].key, col.text2);
        }
        iy += rh;
    }
}

void TextEditorApp::RunMenuAction(int menu, int item) {
    int n = 0;
    const EdMenuItem* items = menu_items(menu, &n);
    if (item < 0 || item >= n) return;
    int a = items[item].act;
    if (a == A_NONE) return;
    kui::Window* w = win;
    if (!w) return;
    switch (a) {
    case A_NEW: AddDoc(nullptr); break;
    case A_OPEN: OpenDialog(ED_DLG_OPEN, active, ED_ACT_NONE); break;
    case A_FOLDER: OpenDialog(ED_DLG_FOLDER, active, ED_ACT_NONE); break;
    case A_SAVE: SaveDoc(active, nullptr, false); break;
    case A_SAVEAS: OpenDialog(ED_DLG_SAVEAS, active, ED_ACT_NONE); break;
    case A_CLOSE: PromptCloseDoc(*w, active, false); break;
    case A_EXIT: RequestExit(*w); break;
    case A_UNDO: Undo(); break;
    case A_REDO: Redo(); break;
    case A_CUT: CutSel(); break;
    case A_COPY: CopySel(); break;
    case A_PASTE: PasteClip(); break;
    case A_SELECT: SelectAll(); break;
    case A_FIND: FindOpen(false); break;
    case A_REPLACE: FindOpen(true); break;
    case A_GOTO: GotoLine(*w); break;
    case A_SIDE: sidebar = !sidebar; break;
    case A_WRAP: wrap = !wrap; if (wrap) D()->xcol = 0; EnsureCaretVisible(); break;
    case A_LINES: show_numbers = !show_numbers; break;
    case A_BIGGER: if (zoom_pct < 200) zoom_pct += 10; UpdateMetrics(); EnsureCaretVisible(); break;
    case A_SMALLER: if (zoom_pct > 70) zoom_pct -= 10; UpdateMetrics(); EnsureCaretVisible(); break;
    case A_KEYS: OpenDialog(ED_DLG_HELP, 0, ED_ACT_NONE); break;
    case A_ABOUT: SetStatus("Kurono Text Editor", kui::NowMs()); break;
    default: break;
    }
    SyncTitle(*w);
}

bool TextEditorApp::MenuClick(int x, int y) {
    if (menu_open >= 0) {
        int m = menu_open;
        int w = menu_w[m], ry = menu_y[m];
        int n = 0;
        const EdMenuItem* items = menu_items(m, &n);
        int rh = row_h + 2;
        int iy = ry + 4;
        for (int i = 0; i < n; i++) {
            int ih = items[i].act == A_NONE ? 9 : rh;
            if (items[i].act != A_NONE && In(menu_x[m], iy, w, ih, x, y)) {
                menu_open = -1;
                RunMenuAction(m, i);
                return true;
            }
            iy += ih;
        }
        if (In(menu_x[m], ry, w, 8 + n * rh, x, y)) { menu_open = -1; return true; }
        menu_open = -1;
        if (y < menu_h) {
            static const char* labels[4] = { "File", "Edit", "View", "Help" };
            for (int i = 0; i < 4; i++) {
                int bw = UiWidth(labels[i]) + 18;
                if (In(menu_x[i], 2, bw, menu_h - 4, x, y)) { menu_open = i; return true; }
            }
        }
        return true;
    }
    if (y >= 0 && y < menu_h) {
        static const char* labels[4] = { "File", "Edit", "View", "Help" };
        for (int i = 0; i < 4; i++) {
            int bw = UiWidth(labels[i]) + 18;
            if (In(menu_x[i], 2, bw, menu_h - 4, x, y)) { menu_open = i; return true; }
        }
    }
    return false;
}

void TextEditorApp::DrawTabBar(kui::Canvas& c) {
    int y = menu_h;
    tabbar_y = y;
    c.FillRect(0, y, cw, tab_h, col.tab_bg);
    c.FillRect(0, y + tab_h - 1, cw, 1, col.line);
    int x = pad;
    for (int i = 0; i < ntabs; i++) {
        ed::Doc* d = &docs[i];
        char name[96];
        if (d->named) scpy(name, base_name(d->path), sizeof(name));
        else scpy(name, "Untitled", sizeof(name));
        if (d->modified) sapp(name, " *", sizeof(name));
        int tw = UiWidth(name) + 52;
        if (tw < 120) tw = 120;
        if (tw > 240) tw = 240;
        tab_x[i] = x;
        tab_w[i] = tw;
        bool act = (i == active);
        bool over = In(x, y + 4, tw - 4, tab_h - 6, mx, my);
        uint32_t bg = act ? col.tab_active : (over ? col.tab_hover : col.tab_bg);
        if (act || over) c.RoundRect(x, y + 4, tw - 4, tab_h - 6, 8, bg, 255);
        if (act) {
            c.FillRect(x + 8, y + tab_h - 4, tw - 20, 2, col.accent);
            c.FillRect(x + 1, y + tab_h - 2, tw - 6, 2, col.tab_active);
        }
        uint32_t tc = act ? col.tab_text : col.tab_dim;
        int ty = y + (tab_h - (int)px_ui) / 2 - 2;
        if (ty < y + 4) ty = y + 4;
        UiTextFit(c, x + 12, ty, name, tc, tw - 40);
        int cx = x + tw - 21;
        bool ch = In(cx - 5, y + (tab_h - 18) / 2, 18, 18, mx, my);
        close_hover[i] = ch;
        if (ch) c.Circle(cx, y + tab_h / 2, 8, col.panel3, 255);
        uint32_t xc = ch ? col.text : col.tab_dim;
        c.DrawLine(cx - 3, y + tab_h / 2 - 3, cx + 3, y + tab_h / 2 + 3, xc, 1);
        c.DrawLine(cx + 3, y + tab_h / 2 - 3, cx - 3, y + tab_h / 2 + 3, xc, 1);
        x += tw;
    }
    int pw = 26;
    plus_x = x + 4;
    plus_w = pw;
    bool over = In(plus_x, y + 4, pw, tab_h - 6, mx, my);
    if (over) c.RoundRect(plus_x, y + 4, pw, tab_h - 6, 7, col.tab_hover, 255);
    int cx = plus_x + pw / 2, cy = y + tab_h / 2;
    c.FillRect(cx - 5, cy - 1, 11, 2, col.text2);
    c.FillRect(cx - 1, cy - 5, 2, 11, col.text2);
}

int TextEditorApp::TabAt(int x, int y, bool* close) {
    if (y < menu_h || y >= menu_h + tab_h) return -1;
    for (int i = 0; i < ntabs; i++) {
        if (x >= tab_x[i] && x < tab_x[i] + tab_w[i]) {
            if (close) *close = (x >= tab_x[i] + tab_w[i] - 28);
            return i;
        }
    }
    if (close) *close = false;
    return -1;
}

void TextEditorApp::LoadFolder(const char* path) {
    char p[ED_PATH_MAX];
    scpy(p, path && path[0] ? path : "/home/user", ED_PATH_MAX);
    int n = kui::Fs::ListDir(p, side_entries, ED_SIDE_MAX);
    if (n < 0) {
        scpy(p, "/home/user", ED_PATH_MAX);
        n = kui::Fs::ListDir(p, side_entries, ED_SIDE_MAX);
    }
    if (n < 0) n = 0;
    sort_entries(side_entries, n);
    scpy(folder, p, ED_PATH_MAX);
    side_count = n;
    if (side_sel >= side_count) side_sel = -1;
    side_scroll = 0;
    side_loaded_ms = kui::NowMs();
}

void TextEditorApp::DrawSidebar(kui::Canvas& c) {
    if (!sidebar) { side_w = 0; return; }
    side_x = 0;
    side_y = body_y;
    side_w = sidebar_w;
    side_h = ch - body_y - status_h;
    c.FillRect(side_x, side_y, side_w, side_h, col.panel);
    c.FillRect(side_x + side_w - 1, side_y, 1, side_h, col.line);
    int hdr = 34;
    char name[96];
    scpy(name, base_name(folder), sizeof(name));
    if (!name[0]) scpy(name, "/", sizeof(name));
    int ty = side_y + 9;
    UiTextBold(c, side_x + 12, ty, name, col.text);
    UiTextFit(c, side_x + 12 + UiWidth(name) + 8, ty + 2, folder, col.text2, side_w - 40 - UiWidth(name));
    c.FillRect(side_x + 8, side_y + hdr - 1, side_w - 16, 1, col.line);
    int list_y = side_y + hdr;
    int list_h = side_h - hdr - 30;
    int rows = list_h / row_h;
    int total = side_count + 1;
    if (side_scroll > total - rows) side_scroll = total - rows;
    if (side_scroll < 0) side_scroll = 0;
    for (int i = 0; i < rows && side_scroll + i < total; i++) {
        int idx = side_scroll + i;
        int ry = list_y + i * row_h;
        bool sel = (idx > 0 && idx - 1 == side_sel);
        bool hot = In(side_x + 6, ry, side_w - 12, row_h, mx, my);
        if (sel) c.RoundRect(side_x + 6, ry, side_w - 12, row_h, 6, col.accent, col.dark ? 60 : 38);
        else if (hot) c.RoundRect(side_x + 6, ry, side_w - 12, row_h, 6, col.tab_hover, 255);
        int ix = side_x + 12;
        int iy = ry + row_h / 2;
        if (idx == 0) {
            c.FillRect(ix + 1, iy - 5, 10, 2, col.text2);
            c.FillRect(ix + 1, iy - 5, 2, 10, col.text2);
            c.DrawLine(ix + 3, iy - 3, ix + 8, iy, col.text2, 1);
            c.DrawLine(ix + 8, iy, ix + 3, iy + 3, col.text2, 1);
            UiText(c, ix + 18, ry + (row_h - (int)px_ui) / 2 - 2, "..", col.text2);
            continue;
        }
        kui::DirEntry* e = &side_entries[idx - 1];
        if (e->dir) {
            c.RoundRect(ix, iy - 5, 14, 10, 2, col.accent, 220);
            c.RoundRect(ix, iy - 7, 7, 4, 1, col.accent, 220);
        } else {
            c.RoundRect(ix + 2, iy - 7, 10, 14, 2, col.text2, 130);
            c.FillRect(ix + 4, iy - 4, 6, 1, col.panel);
            c.FillRect(ix + 4, iy - 1, 6, 1, col.panel);
            c.FillRect(ix + 4, iy + 2, 6, 1, col.panel);
        }
        int tx = ix + 22;
        int tw = side_w - (tx - side_x) - 12;
        char nm[96];
        scpy(nm, e->name, sizeof(nm));
        if (e->dir) sapp(nm, "/", sizeof(nm));
        UiTextFit(c, tx, ry + (row_h - (int)px_ui) / 2 - 2, nm, e->dir ? col.text : col.text2, tw);
    }
    int by = side_y + side_h - 27;
    bool bhot = In(side_x + 8, by, side_w - 16, 22, mx, my);
    c.RoundRect(side_x + 8, by, side_w - 16, 22, 6, bhot ? col.panel3 : col.panel2, 255);
    int bw = UiWidth("Open Folder");
    UiText(c, side_x + (side_w - bw) / 2, by + 3, "Open Folder", bhot ? col.text : col.text2);
}

bool TextEditorApp::SidebarClick(int x, int y) {
    if (!sidebar) return false;
    if (x < 0 || x >= side_w || y < body_y) return false;
    int hdr = 34;
    if (y >= side_y + side_h - 27 && y < side_y + side_h - 5) {
        OpenDialog(ED_DLG_FOLDER, active, ED_ACT_NONE);
        return true;
    }
    if (y < body_y + hdr) return true;
    int i = (y - (body_y + hdr)) / row_h;
    int idx = side_scroll + i;
    if (i < 0) return true;
    if (idx == 0) {
        char up[ED_PATH_MAX];
        path_parent(folder, up, ED_PATH_MAX);
        LoadFolder(up);
        return true;
    }
    if (idx < 1 || idx > side_count) return true;
    kui::DirEntry* e = &side_entries[idx - 1];
    side_sel = idx - 1;
    if (e->dir) {
        char p[ED_PATH_MAX];
        path_join(folder, e->name, p, ED_PATH_MAX);
        LoadFolder(p);
    } else {
        char p[ED_PATH_MAX];
        path_join(folder, e->name, p, ED_PATH_MAX);
        if (win) OpenPath(*win, p);
    }
    return true;
}

// end (satoru)
