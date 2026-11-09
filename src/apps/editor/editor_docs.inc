// editor_docs.inc: construction, theme metrics, document loading and saving. included by
// text_editor.cpp only. (satoru)

TextEditorApp::TextEditorApp() {
    ntabs = 0;
    active = 0;
    re_ready = false;
    memset(&re, 0, sizeof(re));
    re_ready = ed::re_init(&re);
    wrap = false;
    show_numbers = true;
    zoom_pct = 100;
    goal_col = -1;
    mx = -1; my = -1;
    pointer_inside = false;
    dragging = false;
    drag_in_text = false;
    last_key_ms = kui::NowMs();
    focused = true;
    frame_now = last_key_ms;
    win = nullptr;
    cw = 960; ch = 620;
    bg_applied = 0;
    menu_open = -1;
    menu_hover = -1;
    for (int i = 0; i < 4; i++) { menu_x[i] = 0; menu_y[i] = 0; menu_w[i] = 0; menu_n[i] = 0; }
    for (int i = 0; i < ED_MAX_TABS; i++) { tab_x[i] = 0; tab_w[i] = 0; close_hover[i] = false; }
    plus_x = 0; plus_w = 0;
    sidebar = true;
    scpy(folder, "/home/user", ED_PATH_MAX);
    side_count = 0; side_sel = -1; side_scroll = 0; side_loaded_ms = 0;
    find_open = false; find_replace = false; find_regex = false; find_case = false;
    find_field = 0; find_valid = false; find_err[0] = 0;
    match_a = match_b = 0; have_match = false; find_wrapped = false;
    memset(&find_q, 0, sizeof(find_q));
    memset(&find_repl, 0, sizeof(find_repl));
    status_msg[0] = 0; status_ms = 0;
    dlg = ED_DLG_NONE; dlg_action = ED_ACT_NONE; dlg_target = 0; close_window_after = false;
    memset(&dlg_input, 0, sizeof(dlg_input));
    dlg_dir[0] = 0; dlg_msg[0] = 0; dlg_sel = -1; dlg_scroll = 0; dlg_field = 0;
    dlg_count = 0;
    memset(&dl, 0, sizeof(dl));
    bracket_a = bracket_b = -1;
    ed_x = 100; ed_y = 100; ed_w = 600; ed_h = 400;
    gutter_w = 48; tabbar_y = 28; body_y = 62; side_x = 0; side_y = 62; side_w = 200; side_h = 400;
    fb_y = fb_h = fb_qx = fb_qy = fb_qw = fb_rx = fb_rw = 0;
    fb_regex_x = fb_case_x = fb_prev_x = fb_next_x = fb_close_x = 0;
    ctrl_key = shift_key = false;
    px_ui = 14.0f; px_mono = 15.0f; line_h = 19; char_w = 8; menu_h = 28; tab_h = 34;
    status_h = 24; sidebar_w = 200; row_h = 24; pad = 8; mono_ascent = 12;
    mono_face = kui::Face::Mono; ui_face = kui::Face::Sans; ui_bold = kui::Face::SansBold;

    UpdateTheme();
    UpdateMetrics();
    AddDoc(nullptr);
}

TextEditorApp::~TextEditorApp() {
    for (int i = 0; i < ntabs; i++) ed::edx_free(&docs[i]);
    if (re_ready) { ed::re_free(&re); re_ready = false; }
}

void TextEditorApp::OnCreate(kui::Window& w) {
    win = &w;
    UpdateTheme();
    UpdateMetrics();
    if (side_count == 0) LoadFolder(folder);
    SyncTitle(w);
    w.SetBackground(col.panel);
    bg_applied = col.panel;
}

void TextEditorApp::UpdateTheme() {
    const kui::Theme& t = kui::CurrentTheme();
    const kui::Palette& P = t.pal;
    col.modern = (t.style == kui::Style::Modern);
    col.dark = P.dark;
    if (!col.modern) {
        col.bg = 0xFF0A0A18; col.panel = 0xFF12122A; col.panel2 = 0xFF1A1A2E; col.panel3 = 0xFF1E1E32;
        col.line = 0xFF333355; col.text = 0xFFD4D4D4; col.text2 = 0xFF8888AA; col.accent = 0xFF007ACC;
        col.sel = 0xFF264F78; col.curline = 0xFF141430;
        col.gutter_bg = 0xFF12122A; col.gutter_text = 0xFF555588; col.gutter_active = 0xFFD4D4D4;
        col.tab_bg = 0xFF0A0A18; col.tab_active = 0xFF1A1A2E; col.tab_text = 0xFFCCCCCC;
        col.tab_dim = 0xFF8888AA; col.tab_hover = 0xFF222244;
        col.keyword = 0xFF569CD6; col.type = 0xFF4EC9B0; col.string = 0xFFCE9178; col.comment = 0xFF6A9955;
        col.number = 0xFFB5CEA8; col.preproc = 0xFFC586C0; col.func = 0xFFDCDCAA; col.op = 0xFFD4D4D4;
        col.punct = 0xFFCCCCCC; col.heading = 0xFF569CD6; col.link = 0xFF4FC1FF;
        col.modified = 0xFFE74C3C; col.match = 0xFF4A4A22; col.match_cur = 0xFF6A6A2A;
        col.match_line = 0xFF2A2A18; col.bracket = 0xFF3A6EA5; col.err = 0xFFE74C3C;
        col.scrim = 0xFF000000; col.button = 0xFF007ACC; col.button_text = 0xFFFFFFFF;
        return;
    }
    if (col.dark) {
        col.bg = 0xFF1C1C21; col.panel = 0xFF202026; col.panel2 = 0xFF26262C; col.panel3 = 0xFF2E2E35;
        col.line = 0xFF34343C; col.text = 0xFFE4E5EA; col.text2 = 0xFF9EA0AB; col.accent = P.accent;
        col.sel = kui::Mix(0xFF1C1C21, P.accent, 92); col.curline = 0xFF24242A;
        col.gutter_bg = 0xFF1C1C21; col.gutter_text = 0xFF5C5E68; col.gutter_active = 0xFFE4E5EA;
        col.tab_bg = 0xFF202026; col.tab_active = 0xFF1C1C21; col.tab_text = 0xFFE4E5EA;
        col.tab_dim = 0xFF9EA0AB; col.tab_hover = 0xFF2A2A31;
        col.keyword = 0xFFFF7AB2; col.type = 0xFF7FDBCA; col.string = 0xFFFF8170; col.comment = 0xFF7F8C98;
        col.number = 0xFFD2A8FF; col.preproc = 0xFFFFB86C; col.func = 0xFF82AAFF; col.op = 0xFFC8CAD3;
        col.punct = 0xFFB8BAC4; col.heading = P.accent; col.link = 0xFF58A6FF;
        col.modified = 0xFFFF9F0A; col.match = kui::Mix(0xFF1C1C21, 0xFFFFC857, 40);
        col.match_cur = kui::Mix(0xFF1C1C21, P.accent, 95); col.match_line = kui::Mix(0xFF1C1C21, 0xFFFFC857, 18);
        col.bracket = kui::Mix(0xFF1C1C21, P.accent, 120); col.err = 0xFFFF6B61;
        col.scrim = 0xFF000000; col.button = P.accent; col.button_text = 0xFFFFFFFF;
    } else {
        col.bg = 0xFFFFFFFF; col.panel = 0xFFF4F5F9; col.panel2 = 0xFFECEEF4; col.panel3 = 0xFFE4E6EE;
        col.line = 0xFFE2E2E8; col.text = 0xFF1C1C1E; col.text2 = 0xFF6E6E76; col.accent = P.accent;
        col.sel = kui::Mix(0xFFFFFFFF, P.accent, 70); col.curline = 0xFFF4F5F9;
        col.gutter_bg = 0xFFFFFFFF; col.gutter_text = 0xFFA4A6AE; col.gutter_active = 0xFF1C1C1E;
        col.tab_bg = 0xFFF4F5F9; col.tab_active = 0xFFFFFFFF; col.tab_text = 0xFF1C1C1E;
        col.tab_dim = 0xFF6E6E76; col.tab_hover = 0xFFE8EAF0;
        col.keyword = 0xFFAD3DA4; col.type = 0xFF0B7285; col.string = 0xFFC41A16; col.comment = 0xFF707F8C;
        col.number = 0xFF815AC0; col.preproc = 0xFFB35C00; col.func = 0xFF0055AA; col.op = 0xFF4A4A52;
        col.punct = 0xFF5C5C64; col.heading = P.accent; col.link = 0xFF0A66C2;
        col.modified = 0xFFFF9F0A; col.match = kui::Mix(0xFFFFFFFF, 0xFFFFC857, 55);
        col.match_cur = kui::Mix(0xFFFFFFFF, P.accent, 70); col.match_line = kui::Mix(0xFFFFFFFF, 0xFFFFC857, 25);
        col.bracket = kui::Mix(0xFFFFFFFF, P.accent, 110); col.err = 0xFFD70015;
        col.scrim = 0xFF000000; col.button = P.accent; col.button_text = 0xFFFFFFFF;
    }
}

void TextEditorApp::UpdateMetrics() {
    int scale = kui::CurrentTheme().ui_scale;
    if (scale < 60) scale = 60;
    if (scale > 250) scale = 250;
    px_ui = 13.0f * (float)scale / 100.0f;
    px_mono = 14.0f * (float)scale / 100.0f * (float)zoom_pct / 100.0f;
    menu_h = 28 * scale / 100; if (menu_h < 22) menu_h = 22;
    tab_h = 34 * scale / 100; if (tab_h < 28) tab_h = 28;
    status_h = 24 * scale / 100; if (status_h < 20) status_h = 20;
    sidebar_w = 200 * scale / 100; if (sidebar_w < 150) sidebar_w = 150;
    row_h = 24 * scale / 100; if (row_h < 20) row_h = 20;
    pad = 8 * scale / 100; if (pad < 5) pad = 5;
    ui_face = col.modern ? kui::Face::Sans : kui::Face::Bitmap8x16;
    ui_bold = col.modern ? kui::Face::SansBold : kui::Face::Bitmap8x16;
    mono_face = col.modern ? kui::Face::Mono : kui::Face::Bitmap8x16;
    if (!col.modern) {
        px_ui = (float)(16 * scale / 100);
        px_mono = (float)(16 * scale / 100);
        char_w = 8;
        line_h = 16;
        mono_ascent = 12;
        return;
    }
    kui::TextMetrics tm = kui::Measure(mono_face, px_mono, "Mg");
    line_h = tm.line_h;
    if (line_h < (int)(px_mono + 3)) line_h = (int)(px_mono + 5);
    char_w = kui::MeasureWidth(mono_face, px_mono, "M");
    if (char_w < 3) char_w = 8;
    mono_ascent = kui::Ascent(mono_face, px_mono);
    if (mono_ascent <= 0) mono_ascent = (int)(px_mono * 0.8f);
}

int TextEditorApp::FindDocByPath(const char* path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < ntabs; i++) {
        if (docs[i].named && seq(docs[i].path, path)) return i;
    }
    return -1;
}

bool TextEditorApp::LoadInto(int idx, const char* path) {
    if (idx < 0 || idx >= ED_MAX_TABS) return false;
    ed::Doc* d = &docs[idx];
    int fsz = kui::Fs::FileSize(path);
    if (fsz < 0) return false;
    if (fsz > ED_FILE_MAX) fsz = ED_FILE_MAX;
    char* raw = (char*)kui::Alloc((size_t)fsz + 1);
    if (!raw) return false;
    int got = kui::Fs::ReadFile(path, raw, fsz);
    if (got < 0) { kui::Free(raw); return false; }
    char* norm = (char*)kui::Alloc((size_t)got + 1);
    if (!norm) { kui::Free(raw); return false; }
    int n = 0;
    for (int i = 0; i < got; i++) {
        char c = raw[i];
        if (c == '\r') {
            if (i + 1 < got && raw[i + 1] == '\n') continue;
            norm[n++] = '\n';
        } else if (c != '\0') {
            norm[n++] = c;
        }
    }
    norm[n] = 0;
    ed::edx_clear(d);
    ed::edx_raw_insert(d, 0, norm, n);
    d->top = 0;
    d->xcol = 0;
    d->cur = 0;
    d->anchor = 0;
    d->modified = false;
    d->saved_hash = ed::edx_hash(d);
    d->lang = ed::lang_from_path(path);
    d->named = true;
    scpy(d->path, path, EDX_PATH_MAX);
    char msg[96]; msg[0] = 0;
    sapp(msg, "Opened ", sizeof(msg));
    sapp(msg, base_name(path), sizeof(msg));
    SetStatus(msg, kui::NowMs());
    kui::Free(raw);
    kui::Free(norm);
    ED_APPLOG("open-file", path);
    return true;
}

void TextEditorApp::AddDoc(const char* path) {
    if (ntabs >= ED_MAX_TABS) { SetStatus("Too many open tabs", kui::NowMs()); return; }
    ed::edx_init(&docs[ntabs]);
    docs[ntabs].named = false;
    docs[ntabs].path[0] = 0;
    docs[ntabs].lang = ed::LANG_PLAIN;
    int idx = ntabs++;
    active = idx;
    if (path && path[0]) {
        if (!LoadInto(idx, path)) {
            char m[96]; scpy(m, "Cannot open ", sizeof(m)); sapp(m, path, sizeof(m));
            SetStatus(m, kui::NowMs());
        }
    }
    if (win) SyncTitle(*win);
}

void TextEditorApp::OpenPath(kui::Window& w, const char* path) {
    if (!path || !path[0]) return;
    int existing = FindDocByPath(path);
    if (existing >= 0) { active = existing; SyncTitle(w); return; }
    ed::Doc* d = D();
    bool blank = !d->named && !d->modified && ed::edx_len(d) == 0 && d->undo_n == 0;
    if (blank) {
        if (!LoadInto(active, path)) {
            char m[96]; scpy(m, "Cannot open ", sizeof(m)); sapp(m, path, sizeof(m));
            SetStatus(m, kui::NowMs());
            return;
        }
    } else {
        AddDoc(path);
    }
    ed::Doc* nd = D();
    char dir[ED_PATH_MAX];
    dir_name(nd->named ? nd->path : folder, dir, ED_PATH_MAX);
    if (!seq(dir, folder)) LoadFolder(dir);
    SyncTitle(w);
}

void TextEditorApp::CloseDoc(int idx) {
    if (idx < 0 || idx >= ntabs) return;
    ed::edx_free(&docs[idx]);
    for (int i = idx; i < ntabs - 1; i++) docs[i] = docs[i + 1];
    memset(&docs[ntabs - 1], 0, sizeof(ed::Doc));
    ntabs--;
    if (ntabs <= 0) {
        ntabs = 0;
        AddDoc(nullptr);
        active = 0;
    } else if (active >= ntabs) {
        active = ntabs - 1;
    }
    if (active < 0) active = 0;
    have_match = false;
    if (win) SyncTitle(*win);
    EnsureCaretVisible();
}

bool TextEditorApp::SaveDocContent(int idx, const char* path) {
    if (idx < 0 || idx >= ntabs || !path || !path[0]) return false;
    ed::Doc* d = &docs[idx];
    int need = 1;
    for (int i = 0; i < d->nlines; i++) need += ed::edx_line_len(d, i) + 1;
    if (need > ED_FILE_MAX) {
        SetStatus("File is too large to save", kui::NowMs());
        return false;
    }
    char* out = (char*)kui::Alloc((size_t)need);
    if (!out) return false;
    int p = 0;
    for (int i = 0; i < d->nlines; i++) {
        int ll = ed::edx_line_len(d, i);
        ed::edx_copy(d, ed::edx_line_start(d, i), ll, out + p);
        p += ll;
        if (i + 1 < d->nlines) out[p++] = '\n';
    }
    out[p] = 0;
    bool ok = kui::Fs::WriteFile(path, out, p);
    kui::Free(out);
    if (!ok) {
        SetStatus("Save failed", kui::NowMs());
        return false;
    }
    scpy(d->path, path, EDX_PATH_MAX);
    d->named = true;
    d->modified = false;
    d->saved_hash = ed::edx_hash(d);
    d->lang = ed::lang_from_path(path);
    char m[96]; scpy(m, "Saved ", sizeof(m)); sapp(m, base_name(path), sizeof(m));
    SetStatus(m, kui::NowMs());
    ED_APPLOG("save-file", path);
    if (win) SyncTitle(*win);
    return true;
}

bool TextEditorApp::SaveDoc(int idx, const char* path, bool save_as) {
    if (idx < 0 || idx >= ntabs) return false;
    ed::Doc* d = &docs[idx];
    char p[ED_PATH_MAX];
    if (path && path[0]) scpy(p, path, ED_PATH_MAX);
    else if (d->named && !save_as) scpy(p, d->path, ED_PATH_MAX);
    else {
        OpenDialog(ED_DLG_SAVEAS, idx, ED_ACT_NONE);
        return false;
    }
    return SaveDocContent(idx, p);
}

void TextEditorApp::SyncTitle(kui::Window& w) {
    char name[80];
    if (ntabs <= 0) { scpy(name, "Untitled", sizeof(name)); }
    else if (D()->named) { scpy(name, base_name(D()->path), sizeof(name)); }
    else scpy(name, "Untitled", sizeof(name));
    char title[120];
    scpy(title, name, sizeof(title));
    if (ntabs > 0 && D()->modified) sapp(title, " *", sizeof(title));
    sapp(title, " - Text Editor", sizeof(title));
    w.SetTitle(title);
}

void TextEditorApp::SetStatus(const char* msg, uint32_t now) {
    scpy(status_msg, msg ? msg : "", sizeof(status_msg));
    status_ms = now ? now : kui::NowMs();
}

bool TextEditorApp::LoadFile(const char* path) {
    if (!path || !path[0]) return false;
    int existing = FindDocByPath(path);
    if (existing >= 0) { active = existing; if (win) SyncTitle(*win); return true; }
    ed::Doc* d = D();
    bool blank = !d->named && !d->modified && ed::edx_len(d) == 0;
    if (blank) {
        if (!LoadInto(active, path)) return false;
    } else {
        AddDoc(path);
    }
    if (win) SyncTitle(*win);
    return true;
}

// end (satoru)
