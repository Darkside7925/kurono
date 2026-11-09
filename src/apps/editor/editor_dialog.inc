// editor_dialog.inc: the modal open, save as, folder, go to line, help and unsaved change
// prompts. included by text_editor.cpp only. (satoru)

void TextEditorApp::InputSet(EdInput* in, const char* s) {
    scpy(in->text, s ? s : "", (int)sizeof(in->text));
    in->len = slen(in->text);
    in->cursor = in->len;
}

bool TextEditorApp::InputKey(EdInput* in, const kui::Event& e) {
    int key = e.key;
    if (key == kui::K_LEFT) { if (in->cursor > 0) in->cursor--; return true; }
    if (key == kui::K_RIGHT) { if (in->cursor < in->len) in->cursor++; return true; }
    if (key == kui::K_HOME) { in->cursor = 0; return true; }
    if (key == kui::K_END) { in->cursor = in->len; return true; }
    if (key == kui::K_BS || key == kui::K_DEL_ASCII) {
        if (in->cursor > 0) {
            memmove(in->text + in->cursor - 1, in->text + in->cursor, (size_t)(in->len - in->cursor + 1));
            in->cursor--; in->len--;
        }
        return true;
    }
    if (key == kui::K_DEL) {
        if (in->cursor < in->len) {
            memmove(in->text + in->cursor, in->text + in->cursor + 1, (size_t)(in->len - in->cursor));
            in->len--;
        }
        return true;
    }
    if (e.mods & (kui::M_CTRL | kui::M_ALT)) return false;
    const char* t = e.text;
    int n = slen(t);
    char one[2];
    if (n <= 0) {
        if (key >= 32 && key < 127) { one[0] = (char)key; one[1] = 0; t = one; n = 1; }
        else return false;
    }
    if (in->len + n >= (int)sizeof(in->text)) return true;
    memmove(in->text + in->cursor + n, in->text + in->cursor, (size_t)(in->len - in->cursor + 1));
    memcpy(in->text + in->cursor, t, (size_t)n);
    in->cursor += n;
    in->len += n;
    return true;
}

bool TextEditorApp::InputClick(EdInput* in, int x, int y, int w) {
    (void)y; (void)w;
    int cx = 0;
    for (int i = 0; i < in->len; i++) {
        char t[2] = { in->text[i], 0 };
        int cw = UiWidth(t);
        if (x < cx + cw / 2) { in->cursor = i; return true; }
        cx += cw;
    }
    in->cursor = in->len;
    return true;
}

void TextEditorApp::OpenDialog(int kind, int target, int action) {
    dlg = kind;
    dlg_target = target;
    dlg_action = action;
    dlg_field = 0;
    dlg_sel = -1;
    dlg_scroll = 0;
    dlg_msg[0] = 0;
    menu_open = -1;
    if (kind == ED_DLG_OPEN || kind == ED_DLG_SAVEAS) {
        ed::Doc* d = (target >= 0 && target < ntabs) ? &docs[target] : D();
        if (d->named) {
            dir_name(d->path, dlg_dir, ED_PATH_MAX);
            InputSet(&dlg_input, d->path);
        } else {
            scpy(dlg_dir, folder, ED_PATH_MAX);
            if (kind == ED_DLG_SAVEAS) {
                char p[ED_PATH_MAX];
                path_join(folder, "untitled.txt", p, ED_PATH_MAX);
                InputSet(&dlg_input, p);
            } else InputSet(&dlg_input, "");
        }
        DialogLoadDir(dlg_dir);
    } else if (kind == ED_DLG_FOLDER) {
        scpy(dlg_dir, folder, ED_PATH_MAX);
        InputSet(&dlg_input, folder);
        DialogLoadDir(folder);
    } else if (kind == ED_DLG_GOTO || kind == ED_DLG_UNSAVED) {
        InputSet(&dlg_input, "");
    }
    last_key_ms = kui::NowMs();
}

void TextEditorApp::CloseDialog() {
    dlg = ED_DLG_NONE;
    dlg_msg[0] = 0;
    dlg_sel = -1;
    dlg_scroll = 0;
    dlg_action = ED_ACT_NONE;
    close_window_after = false;
    if (win) SyncTitle(*win);
}

void TextEditorApp::DialogLoadDir(const char* dir) {
    char p[ED_PATH_MAX];
    scpy(p, dir && dir[0] ? dir : "/home/user", ED_PATH_MAX);
    int n = kui::Fs::ListDir(p, dlg_entries, ED_DLG_ENTRIES);
    if (n < 0) {
        scpy(dlg_msg, "Cannot open this folder", sizeof(dlg_msg));
        n = 0;
    }
    sort_entries(dlg_entries, n);
    dlg_count = n;
    scpy(dlg_dir, p, ED_PATH_MAX);
    dlg_sel = -1;
    dlg_scroll = 0;
}

void TextEditorApp::LayoutDialog(int content_w, int content_h) {
    int scale = kui::CurrentTheme().ui_scale;
    if (scale < 60) scale = 60;
    int w = 0, h = 0;
    if (dlg == ED_DLG_HELP) { w = content_w - 80; h = content_h - 80; if (w > 620) w = 620; if (h > 520) h = 520; }
    else if (dlg == ED_DLG_GOTO) { w = 440 * scale / 100; h = 180 * scale / 100; }
    else if (dlg == ED_DLG_UNSAVED) { w = 500 * scale / 100; h = 210 * scale / 100; }
    else { w = 720 * scale / 100; h = 480 * scale / 100; }
    if (w > content_w - 32) w = content_w - 32;
    if (h > content_h - 32) h = content_h - 32;
    if (w < 260) w = content_w - 20;
    if (h < 130) h = content_h - 20;
    dl.x = (content_w - w) / 2;
    dl.y = (content_h - h) / 2;
    dl.w = w;
    dl.h = h;
    dl.in_x = dl.x + 20;
    dl.in_y = dl.y + 58;
    dl.in_w = w - 40;
    dl.in_h = 30 * scale / 100;
    if (dl.in_h < 24) dl.in_h = 24;
    dl.list_x = dl.in_x;
    dl.list_y = dl.in_y + dl.in_h + 10;
    dl.list_w = dl.in_w;
    dl.list_h = (dl.y + h - 58) - dl.list_y;
    if (dl.list_h < 0) dl.list_h = 0;
    dl.nbtn = (dlg == ED_DLG_UNSAVED) ? 3 : (dlg == ED_DLG_HELP || dlg == ED_DLG_GOTO) ? 2 : 2;
    dl.btn_h = 30 * scale / 100;
    if (dl.btn_h < 24) dl.btn_h = 24;
    int bw = 100 * scale / 100;
    if (bw < 78) bw = 78;
    int by = dl.y + h - dl.btn_h - 16;
    int totalw = dl.nbtn * bw + (dl.nbtn - 1) * 10;
    int bx = dl.x + w - 20 - totalw;
    if (bx < dl.x + 20) bx = dl.x + 20;
    for (int i = 0; i < dl.nbtn; i++) {
        dl.btn_x[i] = bx + i * (bw + 10);
        dl.btn_y[i] = by;
        dl.btn_w[i] = bw;
    }
}

void TextEditorApp::DrawInput(kui::Canvas& c, EdInput* in, int x, int y, int w, int h, bool active_field) {
    c.RoundRect(x, y, w, h, 7, col.panel3, 255);
    if (active_field) c.RoundOutline(x, y, w, h, 7, col.accent, 255);
    else c.RoundOutline(x, y, w, h, 7, col.line, 255);
    int n = in->cursor < in->len ? in->cursor : in->len;
    char pre[ED_PATH_MAX];
    if (n > (int)sizeof(pre) - 1) n = (int)sizeof(pre) - 1;
    for (int i = 0; i < n; i++) pre[i] = in->text[i];
    pre[n] = 0;
    int curx = UiWidth(pre);
    int shift = 0;
    if (10 + curx > w - 10) shift = 10 + curx - (w - 10);
    c.PushClip(x + 2, y + 2, w - 4, h - 4);
    int ty = y + (h - (int)px_ui) / 2 - 2;
    if (ty < y + 2) ty = y + 2;
    UiText(c, x + 10 - shift, ty, in->text, col.text);
    if (active_field) {
        uint32_t blink = ((frame_now - last_key_ms) / 500) & 1;
        if (blink == 0) c.FillRect(x + 10 - shift + curx, y + 6, 2, h - 12, col.accent);
    }
    c.PopClip();
}

void TextEditorApp::Button(kui::Canvas& c, int x, int y, int w, int h, const char* label, bool primary, bool hover) {
    uint32_t bg = primary ? col.button : col.panel3;
    if (hover) bg = primary ? kui::Mix(col.button, 0xFFFFFFFF, 30) : kui::Mix(col.panel3, col.text, 26);
    c.RoundRect(x, y, w, h, 7, bg, 255);
    if (!primary) c.RoundOutline(x, y, w, h, 7, col.line, 255);
    int tw = UiWidth(label);
    uint32_t tc = primary ? col.button_text : col.text;
    UiText(c, x + (w - tw) / 2, y + (h - (int)px_ui) / 2 - 2, label, tc);
}

void TextEditorApp::DialogDraw(kui::Canvas& c) {
    if (dlg == ED_DLG_NONE) return;
    LayoutDialog(cw, ch);
    c.FillRectAlpha(0, 0, cw, ch, 120, col.scrim);
    c.Shadow(dl.x, dl.y, dl.w, dl.h, 14, 22, 6, 90);
    c.RoundRect(dl.x, dl.y, dl.w, dl.h, 14, col.panel, 255);
    c.RoundOutline(dl.x, dl.y, dl.w, dl.h, 14, col.line, 255);
    const char* t = "Open File";
    if (dlg == ED_DLG_SAVEAS) t = "Save As";
    else if (dlg == ED_DLG_FOLDER) t = "Open Folder";
    else if (dlg == ED_DLG_GOTO) t = "Go to Line";
    else if (dlg == ED_DLG_UNSAVED) t = "Unsaved Changes";
    else if (dlg == ED_DLG_HELP) t = "Keyboard Shortcuts";
    UiTextBold(c, dl.x + 20, dl.y + 16, t, col.text);

    if (dlg == ED_DLG_UNSAVED) {
        char msg[160];
        scpy(msg, "Save changes to ", sizeof(msg));
        ed::Doc* d = (dlg_target >= 0 && dlg_target < ntabs) ? &docs[dlg_target] : D();
        if (d->named) sapp(msg, base_name(d->path), sizeof(msg));
        else sapp(msg, "Untitled", sizeof(msg));
        sapp(msg, "?", sizeof(msg));
        UiText(c, dl.x + 20, dl.y + 54, msg, col.text);
        UiText(c, dl.x + 20, dl.y + 82, "Your changes will be lost if you don't save them.", col.text2);
        bool h0 = In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, mx, my);
        bool h1 = In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, mx, my);
        bool h2 = In(dl.btn_x[2], dl.btn_y[2], dl.btn_w[2], dl.btn_h, mx, my);
        Button(c, dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, "Save", true, h0);
        Button(c, dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, "Discard", false, h1);
        Button(c, dl.btn_x[2], dl.btn_y[2], dl.btn_w[2], dl.btn_h, "Cancel", false, h2);
        return;
    }
    if (dlg == ED_DLG_HELP) {
        int y = dl.y + 54;
        for (int i = 0; kShortcutHelp[i] && y < dl.y + dl.h - 60; i++) {
            UiText(c, dl.x + 24, y, kShortcutHelp[i], i == 0 ? col.text : col.text2);
            y += (int)(px_ui + 10);
        }
        bool h0 = In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, mx, my);
        Button(c, dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, "Close", true, h0);
        return;
    }
    if (dlg == ED_DLG_GOTO) {
        UiText(c, dl.x + 20, dl.in_y - 20, "Line number", col.text2);
        DrawInput(c, &dlg_input, dl.in_x, dl.in_y, dl.in_w, dl.in_h, true);
        bool h0 = In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, mx, my);
        bool h1 = In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, mx, my);
        Button(c, dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, "Go", true, h0);
        Button(c, dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, "Cancel", false, h1);
        return;
    }

    const char* label = (dlg == ED_DLG_FOLDER) ? "Folder path" : (dlg == ED_DLG_SAVEAS ? "File path" : "Path");
    UiText(c, dl.x + 20, dl.in_y - 20, label, col.text2);
    DrawInput(c, &dlg_input, dl.in_x, dl.in_y, dl.in_w, dl.in_h, true);
    if (dl.list_h > 8) {
        c.RoundRect(dl.list_x, dl.list_y, dl.list_w, dl.list_h, 8, col.panel2, 255);
        c.RoundOutline(dl.list_x, dl.list_y, dl.list_w, dl.list_h, 8, col.line, 255);
        c.PushClip(dl.list_x + 1, dl.list_y + 1, dl.list_w - 2, dl.list_h - 2);
        int rows = dl.list_h / row_h;
        for (int i = 0; i < rows && i + dlg_scroll < dlg_count; i++) {
            int idx = i + dlg_scroll;
            kui::DirEntry* e = &dlg_entries[idx];
            int ry = dl.list_y + i * row_h;
            bool sel = (idx == dlg_sel);
            bool hot = In(dl.list_x + 4, ry, dl.list_w - 8, row_h, mx, my);
            if (sel) c.RoundRect(dl.list_x + 4, ry, dl.list_w - 8, row_h, 6, col.accent, col.dark ? 60 : 38);
            else if (hot) c.RoundRect(dl.list_x + 4, ry, dl.list_w - 8, row_h, 6, col.tab_hover, 255);
            int ix = dl.list_x + 12, iy = ry + row_h / 2;
            if (e->dir) {
                c.RoundRect(ix, iy - 5, 14, 10, 2, col.accent, 220);
                c.RoundRect(ix, iy - 7, 7, 4, 1, col.accent, 220);
            } else {
                c.RoundRect(ix + 2, iy - 7, 10, 14, 2, col.text2, 130);
            }
            char nm[96];
            scpy(nm, e->name, sizeof(nm));
            if (e->dir) sapp(nm, "/", sizeof(nm));
            UiTextFit(c, ix + 22, ry + (row_h - (int)px_ui) / 2 - 2, nm,
                      e->dir ? col.text : col.text2, dl.list_w - (ix - dl.list_x) - 30);
        }
        c.PopClip();
    }
    if (dlg_msg[0]) UiTextFit(c, dl.x + 20, dl.y + dl.h - (int)px_ui - 44, dlg_msg, col.err, dl.w - 240);
    const char* ok = (dlg == ED_DLG_SAVEAS) ? "Save" : (dlg == ED_DLG_FOLDER ? "Open Folder" : "Open");
    bool h0 = In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, mx, my);
    bool h1 = In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, mx, my);
    Button(c, dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, ok, true, h0);
    Button(c, dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, "Cancel", false, h1);
}

void TextEditorApp::DialogCancel() {
    CloseDialog();
}

void TextEditorApp::DialogAccept(kui::Window& w) {
    if (dlg == ED_DLG_GOTO) {
        int v = 0, any = 0;
        for (int i = 0; i < dlg_input.len; i++) {
            char c = dlg_input.text[i];
            if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); any = 1; if (v > 10000000) v = 10000000; }
        }
        ed::Doc* d = D();
        int row = any ? clampi(v - 1, 0, d->nlines - 1) : 0;
        ed::edx_select(d, ed::edx_line_start(d, row), false);
        d->top = row;
        CloseDialog();
        EnsureCaretVisible();
        return;
    }
    if (dlg == ED_DLG_HELP) { CloseDialog(); return; }
    if (dlg == ED_DLG_UNSAVED) {
        ed::Doc* d = (dlg_target >= 0 && dlg_target < ntabs) ? &docs[dlg_target] : nullptr;
        if (!d) { CloseDialog(); return; }
        if (d->named) {
            if (SaveDocContent(dlg_target, d->path)) {
                int act = dlg_action;
                int target = dlg_target;
                dlg_action = act;
                dlg_target = target;
                dlg = ED_DLG_NONE;
                AfterSaveAction(w);
            } else {
                scpy(dlg_msg, "Save failed", sizeof(dlg_msg));
            }
        } else {
            int act = dlg_action;
            int target = dlg_target;
            dlg = ED_DLG_NONE;
            OpenDialog(ED_DLG_SAVEAS, target, act);
        }
        return;
    }
    if (dlg == ED_DLG_OPEN || dlg == ED_DLG_SAVEAS || dlg == ED_DLG_FOLDER) {
        char path[ED_PATH_MAX];
        scpy(path, dlg_input.text, ED_PATH_MAX);
        trim_tail(path);
        if (dlg_sel >= 0 && dlg_sel < dlg_count) {
            char selp[ED_PATH_MAX];
            path_join(dlg_dir, dlg_entries[dlg_sel].name, selp, ED_PATH_MAX);
            if (!path[0] || seq(path, selp)) scpy(path, selp, ED_PATH_MAX);
        }
        if (!path[0]) { scpy(dlg_msg, "Enter a path", sizeof(dlg_msg)); return; }
        if (dlg == ED_DLG_FOLDER) {
            if (!kui::Fs::IsDir(path)) { scpy(dlg_msg, "Not a folder", sizeof(dlg_msg)); return; }
            LoadFolder(path);
            CloseDialog();
            return;
        }
        if (dlg == ED_DLG_OPEN) {
            if (kui::Fs::IsDir(path)) { DialogLoadDir(path); InputSet(&dlg_input, path); dlg_msg[0] = 0; return; }
            if (!kui::Fs::Exists(path)) { scpy(dlg_msg, "No such file", sizeof(dlg_msg)); return; }
            ed::Doc* d = D();
            bool blank = !d->named && !d->modified && ed::edx_len(d) == 0;
            if (blank) {
                if (!LoadInto(active, path)) { scpy(dlg_msg, "Cannot read this file", sizeof(dlg_msg)); return; }
            } else {
                AddDoc(path);
            }
            char dir[ED_PATH_MAX];
            dir_name(path, dir, ED_PATH_MAX);
            if (!seq(dir, folder)) LoadFolder(dir);
            CloseDialog();
            SyncTitle(w);
            return;
        }
        if (dlg == ED_DLG_SAVEAS) {
            if (kui::Fs::IsDir(path)) {
                char p[ED_PATH_MAX];
                path_join(path, "untitled.txt", p, ED_PATH_MAX);
                scpy(path, p, ED_PATH_MAX);
            }
            int act = dlg_action;
            int target = dlg_target;
            if (!SaveDocContent(target, path)) { scpy(dlg_msg, "Save failed", sizeof(dlg_msg)); return; }
            CloseDialog();
            dlg_action = act;
            dlg_target = target;
            AfterSaveAction(w);
            return;
        }
    }
}

bool TextEditorApp::DialogClick(kui::Window& w, int x, int y, int clicks) {
    if (dlg == ED_DLG_NONE) return false;
    LayoutDialog(cw, ch);
    if (dlg == ED_DLG_GOTO) {
        if (In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, x, y)) { DialogAccept(w); return true; }
        if (In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, x, y)) { DialogCancel(); return true; }
        if (In(dl.in_x, dl.in_y, dl.in_w, dl.in_h, x, y)) { InputClick(&dlg_input, x - (dl.in_x + 10), y, dl.in_w); return true; }
        if (!In(dl.x, dl.y, dl.w, dl.h, x, y)) DialogCancel();
        return true;
    }
    if (dlg == ED_DLG_UNSAVED) {
        if (In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, x, y)) { DialogAccept(w); return true; }
        if (In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, x, y)) {
            int act = dlg_action, target = dlg_target;
            dlg_action = act; dlg_target = target;
            dlg = ED_DLG_NONE;
            AfterSaveAction(w);
            return true;
        }
        if (In(dl.btn_x[2], dl.btn_y[2], dl.btn_w[2], dl.btn_h, x, y)) { DialogCancel(); return true; }
        return true;
    }
    if (dlg == ED_DLG_HELP) {
        if (In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, x, y)) CloseDialog();
        else if (!In(dl.x, dl.y, dl.w, dl.h, x, y)) CloseDialog();
        return true;
    }
    if (!In(dl.x, dl.y, dl.w, dl.h, x, y)) { DialogCancel(); return true; }
    if (In(dl.in_x, dl.in_y, dl.in_w, dl.in_h, x, y)) {
        dlg_field = 0;
        InputClick(&dlg_input, x - (dl.in_x + 10), y, dl.in_w);
        return true;
    }
    if (In(dl.list_x, dl.list_y, dl.list_w, dl.list_h, x, y)) {
        int i = (y - dl.list_y) / row_h;
        int idx = dlg_scroll + i;
        if (idx >= 0 && idx < dlg_count) {
            dlg_field = 1;
            dlg_sel = idx;
            char p[ED_PATH_MAX];
            path_join(dlg_dir, dlg_entries[idx].name, p, ED_PATH_MAX);
            InputSet(&dlg_input, p);
            if (clicks >= 2) DialogAccept(w);
        }
        return true;
    }
    if (In(dl.btn_x[0], dl.btn_y[0], dl.btn_w[0], dl.btn_h, x, y)) { DialogAccept(w); return true; }
    if (In(dl.btn_x[1], dl.btn_y[1], dl.btn_w[1], dl.btn_h, x, y)) { DialogCancel(); return true; }
    return true;
}

bool TextEditorApp::DialogKey(kui::Window& w, const kui::Event& e, bool* redraw) {
    *redraw = true;
    if (dlg == ED_DLG_NONE) return false;
    int key = e.key;
    if (key == kui::K_ESC) { DialogCancel(); return true; }
    if (dlg == ED_DLG_UNSAVED) {
        if (key == kui::K_ENTER) { DialogAccept(w); return true; }
        char c = lowerc(e.text[0] ? e.text[0] : (char)key);
        if (c == 's') { DialogAccept(w); return true; }
        if (c == 'd') {
            int act = dlg_action, target = dlg_target;
            dlg_action = act; dlg_target = target;
            dlg = ED_DLG_NONE;
            AfterSaveAction(w);
            return true;
        }
        if (c == 'c') { DialogCancel(); return true; }
        return true;
    }
    if (dlg == ED_DLG_HELP) {
        if (key == kui::K_ENTER || key == kui::K_ESC) CloseDialog();
        return true;
    }
    if (dlg == ED_DLG_GOTO) {
        if (key >= '0' && key <= '9') {
            if (dlg_input.len < 12) {
                dlg_input.text[dlg_input.len++] = (char)key;
                dlg_input.text[dlg_input.len] = 0;
                dlg_input.cursor = dlg_input.len;
            }
            return true;
        }
        if (key == kui::K_BS || key == kui::K_DEL_ASCII) {
            if (dlg_input.len > 0) { dlg_input.len--; dlg_input.text[dlg_input.len] = 0; dlg_input.cursor = dlg_input.len; }
            return true;
        }
        if (key == kui::K_ENTER) { DialogAccept(w); return true; }
        return true;
    }
    if (key == kui::K_UP) {
        if (dlg_count > 0) {
            if (dlg_sel < 0) dlg_sel = 0; else if (dlg_sel > 0) dlg_sel--;
            if (dlg_sel < dlg_scroll) dlg_scroll = dlg_sel;
            char p[ED_PATH_MAX];
            path_join(dlg_dir, dlg_entries[dlg_sel].name, p, ED_PATH_MAX);
            InputSet(&dlg_input, p);
            dlg_field = 1;
        }
        return true;
    }
    if (key == kui::K_DOWN) {
        if (dlg_count > 0) {
            if (dlg_sel < 0) dlg_sel = 0; else if (dlg_sel + 1 < dlg_count) dlg_sel++;
            int rows = dl.list_h / row_h;
            if (rows < 1) rows = 1;
            if (dlg_sel >= dlg_scroll + rows) dlg_scroll = dlg_sel - rows + 1;
            char p[ED_PATH_MAX];
            path_join(dlg_dir, dlg_entries[dlg_sel].name, p, ED_PATH_MAX);
            InputSet(&dlg_input, p);
            dlg_field = 1;
        }
        return true;
    }
    if (key == kui::K_TAB) { dlg_field = dlg_field ? 0 : 1; return true; }
    if (key == kui::K_ENTER) { DialogAccept(w); return true; }
    dlg_field = 0;
    if (InputKey(&dlg_input, e)) { dlg_sel = -1; return true; }
    return true;
}

void TextEditorApp::PromptCloseDoc(kui::Window& w, int idx, bool window_after) {
    if (idx < 0 || idx >= ntabs) return;
    ed::Doc* d = &docs[idx];
    if (!d->modified) {
        if (window_after) {
            for (int i = 0; i < ntabs; i++) {
                if (docs[i].modified) { PromptCloseDoc(w, i, true); return; }
            }
            w.Close();
        } else {
            CloseDoc(idx);
        }
        return;
    }
    dlg = ED_DLG_UNSAVED;
    dlg_target = idx;
    dlg_action = window_after ? ED_ACT_CLOSE_WINDOW : ED_ACT_CLOSE_TAB;
    close_window_after = window_after;
    dlg_msg[0] = 0;
    menu_open = -1;
    last_key_ms = kui::NowMs();
}

void TextEditorApp::AfterSaveAction(kui::Window& w) {
    int act = dlg_action;
    int target = dlg_target;
    dlg_action = ED_ACT_NONE;
    if (act == ED_ACT_CLOSE_TAB) {
        CloseDoc(target);
    } else if (act == ED_ACT_CLOSE_WINDOW) {
        for (int i = 0; i < ntabs; i++) {
            if (docs[i].modified) { PromptCloseDoc(w, i, true); return; }
        }
        w.Close();
        return;
    }
    SyncTitle(w);
}

void TextEditorApp::RequestExit(kui::Window& w) {
    for (int i = 0; i < ntabs; i++) {
        if (docs[i].modified) { PromptCloseDoc(w, i, true); return; }
    }
    w.Close();
}

bool TextEditorApp::OnClose(kui::Window& w) {
    for (int i = 0; i < ntabs; i++) {
        if (docs[i].modified) {
            PromptCloseDoc(w, i, true);
            return false;   // ring 3: keep the window for the prompt; the kernel closes anyway. (satoru)
        }
    }
    return true;
}

// end (satoru)
