// editor_edit.inc: typing, deleting, indentation, clipboard, undo, caret movement, bracket
// matching and the layout arithmetic behind caret visibility. included by text_editor.cpp. (satoru)

void TextEditorApp::InsertTyped(kui::Window& w, const char* s, int n) {
    ed::Doc* d = D();
    // electric dedent: a closing bracket typed on an otherwise blank indented line
    // first loses one indent level, so blocks close where they opened. (satoru)
    if (!ed::edx_has_sel(d) && n == 1 && (s[0] == '}' || s[0] == ']' || s[0] == ')')) {
        int row = ed::edx_line_of(d, d->cur);
        int ls = ed::edx_line_start(d, row);
        int k = d->cur;
        while (k > ls && (ed::edx_at(d, k - 1) == ' ' || ed::edx_at(d, k - 1) == '\t')) k--;
        if (k == ls && d->cur > ls) {
            int cut = 0;
            while (cut < ED_TABSTOP && d->cur - cut > ls && ed::edx_at(d, d->cur - cut - 1) == ' ') cut++;
            if (cut == 0 && ed::edx_at(d, d->cur - 1) == '\t') cut = 1;
            if (cut > 0) ed::edx_replace(d, d->cur - cut, cut, nullptr, 0, false);
        }
    }
    ed::edx_insert_text(d, s, n, true);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    SyncTitle(w);
}

void TextEditorApp::InsertNewlineAuto() {
    ed::Doc* d = D();
    int a, b;
    if (ed::edx_has_sel(d)) ed::edx_sel(d, &a, &b); else a = b = d->cur;
    int row = ed::edx_line_of(d, a);
    int ls = ed::edx_line_start(d, row);
    char ind[128];
    int indn = 0;
    while (ls + indn < a && indn < (int)sizeof(ind) - 1) {
        char c = ed::edx_at(d, ls + indn);
        if (c != ' ' && c != '\t') break;
        ind[indn++] = c;
    }
    ind[indn] = 0;
    // a line that starts with a closing bracket carries one indent level less, so pressing
    // enter on it does not keep nesting deeper and deeper. (satoru)
    {
        int fn = 0;
        while (fn < ed::edx_line_len(d, row)) {
            char fc = ed::edx_at(d, ls + fn);
            if (fc != ' ' && fc != '\t') break;
            fn++;
        }
        if (fn < ed::edx_line_len(d, row)) {
            char fc = ed::edx_at(d, ls + fn);
            if (fc == '}' || fc == ']' || fc == ')') {
                int cut = ED_TABSTOP;
                while (cut > 0 && indn > 0) {
                    if (ind[indn - 1] == ' ') { indn--; cut--; }
                    else if (ind[indn - 1] == '\t') { indn--; cut = 0; }
                    else break;
                }
                ind[indn] = 0;
            }
        }
    }
    char before = 0, after = 0;
    int k = a - 1;
    while (k >= ls && (ed::edx_at(d, k) == ' ' || ed::edx_at(d, k) == '\t')) k--;
    if (k >= ls) before = ed::edx_at(d, k);
    if (b < ed::edx_len(d)) after = ed::edx_at(d, b);
    int lang = d->lang;
    bool extra = (before == '{' || before == '[' || before == '(');
    if (!extra && (lang == ed::LANG_PY || lang == ed::LANG_KCL) && before == ':') extra = 1;
    if (!extra && lang == ed::LANG_SH && before == ';') extra = 1;
    bool pair = (before == '{' && after == '}') || (before == '[' && after == ']') || (before == '(' && after == ')');
    char ins[256];
    int n = 0;
    ins[n++] = '\n';
    for (int i = 0; i < indn && n < (int)sizeof(ins) - 10; i++) ins[n++] = ind[i];
    if (extra) for (int i = 0; i < ED_TABSTOP; i++) ins[n++] = ' ';
    int caret = n;
    if (pair) {
        ins[n++] = '\n';
        for (int i = 0; i < indn && n < (int)sizeof(ins) - 2; i++) ins[n++] = ind[i];
    }
    ed::edx_replace(d, a, b - a, ins, n, false);
    d->cur = a + caret;
    d->anchor = d->cur;
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::DoBackspace() {
    ed::Doc* d = D();
    if (ed::edx_has_sel(d)) ed::edx_delete_sel(d);
    else if (d->cur > 0) {
        int p = ed::edx_prev_char(d, d->cur);
        ed::edx_replace(d, p, d->cur - p, nullptr, 0, true);
    }
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::DoDelete() {
    ed::Doc* d = D();
    if (ed::edx_has_sel(d)) ed::edx_delete_sel(d);
    else if (d->cur < ed::edx_len(d)) {
        int n = ed::edx_next_char(d, d->cur);
        ed::edx_replace(d, d->cur, n - d->cur, nullptr, 0, true);
    }
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::IndentSelection(bool unindent) {
    ed::Doc* d = D();
    int a, b;
    if (!ed::edx_has_sel(d)) {
        int row = ed::edx_line_of(d, d->cur);
        a = ed::edx_line_start(d, row);
        b = d->cur;
    } else {
        ed::edx_sel(d, &a, &b);
    }
    int ra = ed::edx_line_of(d, a);
    int rb = ed::edx_line_of(d, b);
    if (rb > ra && b == ed::edx_line_start(d, rb)) rb--;
    int na = a, nb = b;
    for (int row = rb; row >= ra; row--) {
        int ls = ed::edx_line_start(d, row);
        if (unindent) {
            int n = 0;
            char c = ed::edx_at(d, ls);
            if (c == '\t') n = 1;
            else while (n < ED_TABSTOP && ed::edx_at(d, ls + n) == ' ') n++;
            if (n > 0) {
                ed::edx_replace(d, ls, n, nullptr, 0, false);
                if (ls <= na) na = clampi(na - n, ls, ed::edx_len(d));
                if (ls <= nb) nb = clampi(nb - n, ls, ed::edx_len(d));
            }
        } else {
            ed::edx_replace(d, ls, 0, "    ", ED_TABSTOP, false);
            if (ls <= na) na += ED_TABSTOP;
            if (ls <= nb) nb += ED_TABSTOP;
        }
    }
    d->anchor = clampi(na, 0, ed::edx_len(d));
    d->cur = clampi(nb, 0, ed::edx_len(d));
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::DoTab(bool shift) {
    ed::Doc* d = D();
    if (shift) { IndentSelection(true); return; }
    int a, b;
    if (ed::edx_has_sel(d)) {
        ed::edx_sel(d, &a, &b);
        if (ed::edx_line_of(d, a) != ed::edx_line_of(d, b)) { IndentSelection(false); return; }
    }
    int col = CursorDisplayCol(d);
    int n = ED_TABSTOP - (col % ED_TABSTOP);
    char sp[8];
    for (int i = 0; i < n; i++) sp[i] = ' ';
    ed::edx_insert_text(d, sp, n, true);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::CopySel() {
    CopySelTo(false);
}

// copy the active selection (or current line when there is none) into the
// clipboard or the primary selection used by middle click. (satoru)
void TextEditorApp::CopySelTo(bool primary) {
    ed::Doc* d = D();
    int a, b;
    if (ed::edx_has_sel(d)) ed::edx_sel(d, &a, &b);
    else {
        int row = ed::edx_line_of(d, d->cur);
        a = ed::edx_line_start(d, row);
        b = a + ed::edx_line_len(d, row);
        if (row + 1 < d->nlines) b++;
    }
    int n = b - a;
    char* buf = (char*)kui::Alloc((size_t)n + 1);
    if (!buf) return;
    ed::edx_copy(d, a, n, buf);
    if (primary) kui::Clipboard::SetPrimaryText(buf);
    else kui::Clipboard::SetText(buf);
    kui::Free(buf);
    SetStatus(primary ? "Copied to primary" : "Copied", kui::NowMs());
}

void TextEditorApp::CutSel() {
    ed::Doc* d = D();
    int a, b;
    if (ed::edx_has_sel(d)) ed::edx_sel(d, &a, &b);
    else {
        int row = ed::edx_line_of(d, d->cur);
        a = ed::edx_line_start(d, row);
        b = a + ed::edx_line_len(d, row);
        if (row + 1 < d->nlines) b++;
    }
    int n = b - a;
    char* buf = (char*)kui::Alloc((size_t)n + 1);
    if (!buf) return;
    ed::edx_copy(d, a, n, buf);
    kui::Clipboard::SetText(buf);
    kui::Free(buf);
    ed::edx_replace(d, a, n, nullptr, 0, false);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    SetStatus("Cut", kui::NowMs());
    if (win) SyncTitle(*win);
}

void TextEditorApp::PasteClip() {
    if (!kui::Clipboard::HasText()) { SetStatus("Clipboard is empty", kui::NowMs()); return; }
    const char* s = kui::Clipboard::GetText();
    int n = slen(s);
    if (n <= 0) return;
    char* buf = (char*)kui::Alloc((size_t)n + 1);
    if (!buf) return;
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;
            c = '\n';
        }
        if (c == 0) continue;
        buf[k++] = c;
    }
    ed::Doc* d = D();
    ed::edx_insert_text(d, buf, k, false);
    kui::Free(buf);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    SetStatus("Pasted", kui::NowMs());
    if (win) SyncTitle(*win);
}

// insert the primary selection text at the cursor, used by middle click. (satoru)
void TextEditorApp::PastePrimary() {
    if (!kui::Clipboard::HasPrimaryText()) return;
    const char* s = kui::Clipboard::GetPrimaryText();
    int n = slen(s);
    if (n <= 0) return;
    char* buf = (char*)kui::Alloc((size_t)n + 1);
    if (!buf) return;
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;
            c = '\n';
        }
        if (c == 0) continue;
        buf[k++] = c;
    }
    ed::Doc* d = D();
    ed::edx_insert_text(d, buf, k, false);
    kui::Free(buf);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    SetStatus("Pasted primary", kui::NowMs());
    if (win) SyncTitle(*win);
}

void TextEditorApp::SelectAll() {
    ed::Doc* d = D();
    d->anchor = 0;
    d->cur = ed::edx_len(d);
    last_key_ms = kui::NowMs();
    EnsureCaretVisible();
}

void TextEditorApp::Undo() {
    if (ed::edx_undo(D())) SetStatus("Undo", kui::NowMs());
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::Redo() {
    if (ed::edx_redo(D())) SetStatus("Redo", kui::NowMs());
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
    if (win) SyncTitle(*win);
}

void TextEditorApp::MoveCaret(int pos, bool extend) {
    ed::Doc* d = D();
    ed::edx_select(d, pos, extend);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
}

void TextEditorApp::MoveLines(int delta, bool extend) {
    ed::Doc* d = D();
    int row = ed::edx_line_of(d, d->cur);
    int want = (goal_col >= 0) ? goal_col : ed::edx_col(d, d->cur);
    int nr = clampi(row + delta, 0, d->nlines - 1);
    int ll = ed::edx_line_len(d, nr);
    int col = want < ll ? want : ll;
    if (goal_col < 0) goal_col = want;
    int pos = ed::edx_line_start(d, nr) + col;
    ed::edx_select(d, pos, extend);
    last_key_ms = kui::NowMs();
    EnsureCaretVisible();
}

void TextEditorApp::MoveWords(int dir, bool extend) {
    ed::Doc* d = D();
    int pos = dir < 0 ? ed::edx_word_left(d, d->cur) : ed::edx_word_right(d, d->cur);
    ed::edx_select(d, pos, extend);
    last_key_ms = kui::NowMs();
    goal_col = -1;
    EnsureCaretVisible();
}

void TextEditorApp::GotoLine(kui::Window& w) {
    (void)w;
    OpenDialog(ED_DLG_GOTO, active, ED_ACT_NONE);
}

void TextEditorApp::ApplyAutoIndent() {
    // the newline path does the work; kept for the header contract. (satoru)
}

static bool is_open_bracket(char c) { return c == '(' || c == '[' || c == '{'; }
static bool is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }
static char bracket_pair(char c) {
    if (c == '(') return ')';
    if (c == '[') return ']';
    if (c == '{') return '}';
    if (c == ')') return '(';
    if (c == ']') return '[';
    return '{';
}

void TextEditorApp::FindBrackets() {
    bracket_a = bracket_b = -1;
    ed::Doc* d = D();
    int len = ed::edx_len(d);
    int cand[2];
    int nc = 0;
    if (d->cur < len) cand[nc++] = d->cur;
    if (d->cur > 0) cand[nc++] = d->cur - 1;
    for (int ci = 0; ci < nc; ci++) {
        int pos = cand[ci];
        char c = ed::edx_at(d, pos);
        if (!is_open_bracket(c) && !is_close_bracket(c)) continue;
        int depth = 0;
        int limit = 200000;
        if (is_open_bracket(c)) {
            for (int i = pos; i < len && limit-- > 0; i++) {
                char x = ed::edx_at(d, i);
                if (x == c) depth++;
                else if (x == bracket_pair(c)) {
                    depth--;
                    if (depth == 0) { bracket_a = pos; bracket_b = i; return; }
                }
            }
        } else {
            for (int i = pos; i >= 0 && limit-- > 0; i--) {
                char x = ed::edx_at(d, i);
                if (x == c) depth++;
                else if (x == bracket_pair(c)) {
                    depth--;
                    if (depth == 0) { bracket_a = i; bracket_b = pos; return; }
                }
            }
        }
    }
}

int TextEditorApp::LineDisplayCols(const ed::Doc* d, int row, bool* has_tab) {
    int len = ed::edx_line_len(d, row);
    int start = ed::edx_line_start(d, row);
    int n = 0;
    bool tab = false;
    for (int i = 0; i < len; i++) {
        char c = ed::edx_at(d, start + i);
        if (c == '\t') { n += ED_TABSTOP - (n % ED_TABSTOP); tab = true; }
        else if (c != '\r' && c != '\n') n++;
    }
    if (has_tab) *has_tab = tab;
    return n;
}

int TextEditorApp::CursorDisplayCol(const ed::Doc* d) {
    int row = ed::edx_line_of(d, d->cur);
    return DispAtByte(d, row, d->cur - ed::edx_line_start(d, row), 1 << 20);
}

int TextEditorApp::RowVisualCount(const ed::Doc* d, int row, int max_cols) {
    if (max_cols < 2) return 1;
    int len = LineDisplayCols(d, row, nullptr);
    if (len <= max_cols) return 1;
    return (len + max_cols - 1) / max_cols;
}

void TextEditorApp::EnsureCaretVisible() {
    ed::Doc* d = D();
    int row = ed::edx_line_of(d, d->cur);
    if (d->top < 0) d->top = 0;
    if (d->top >= d->nlines) d->top = d->nlines - 1;
    int max_cols = (ed_w - 2) / char_w;
    if (max_cols < 2) max_cols = 2;
    int vis = ed_h / line_h;
    if (vis < 1) vis = 1;
    if (wrap) {
        d->xcol = 0;
        if (row < d->top) d->top = row;
        int dispcol = CursorDisplayCol(d);
        int seg = dispcol / max_cols;
        int need = seg;
        for (int r = d->top; r < row; r++) need += RowVisualCount(d, r, max_cols);
        while (need >= vis && d->top < row) {
            need -= RowVisualCount(d, d->top, max_cols);
            d->top++;
        }
        if (d->top > row) d->top = row;
        if (d->top < 0) d->top = 0;
    } else {
        if (row < d->top) d->top = row;
        if (row >= d->top + vis) d->top = row - vis + 1;
        if (d->top < 0) d->top = 0;
        int dispcol = CursorDisplayCol(d);
        if (dispcol < d->xcol) d->xcol = dispcol;
        if (dispcol >= d->xcol + max_cols) d->xcol = dispcol - max_cols + 2;
        if (d->xcol < 0) d->xcol = 0;
    }
}

// end (satoru)
