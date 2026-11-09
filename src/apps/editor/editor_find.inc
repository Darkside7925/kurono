// editor_find.inc: literal and regex search across the document, find next/previous,
// replace current and replace all. included by text_editor.cpp only. (satoru)

static int find_in_line(const ed::Regex* re, bool use_re, bool icase, const char* pat, int pn,
                        const char* line, int len, int from, int* a, int* b) {
    if (use_re) {
        if (!re || !re->ok) return 0;
        if (!ed::re_find(re, line, len, from, a, b)) return 0;
        return 1;
    }
    int m = ed::lit_find(line, len, pat, pn, icase, from);
    if (m < 0) return 0;
    *a = m;
    *b = m + pn;
    return 1;
}

static bool doc_find_forward(const ed::Doc* d, const ed::Regex* re, bool use_re, bool icase,
                             const char* pat, int pn, int start, int* ma, int* mb) {
    if (start < 0) start = 0;
    int len = ed::edx_len(d);
    if (start > len) return false;
    int row = ed::edx_line_of(d, start);
    static char line[8192];
    for (int r = row; r < d->nlines; r++) {
        int ll = ed::edx_line_len(d, r);
        if (ll > (int)sizeof(line) - 1) ll = (int)sizeof(line) - 1;
        ed::edx_copy(d, ed::edx_line_start(d, r), ll, line);
        int from = (r == row) ? (start - ed::edx_line_start(d, r)) : 0;
        if (from < 0) from = 0;
        int a, b;
        while (find_in_line(re, use_re, icase, pat, pn, line, ll, from, &a, &b)) {
            int abs = ed::edx_line_start(d, r) + a;
            if (abs >= start) { *ma = abs; *mb = abs + (b - a); return true; }
            from = (b > a) ? b : a + 1;
            if (from > ll) break;
        }
    }
    return false;
}

static bool doc_find_backward(const ed::Doc* d, const ed::Regex* re, bool use_re, bool icase,
                              const char* pat, int pn, int start, int* ma, int* mb) {
    int len = ed::edx_len(d);
    if (start > len) start = len;
    int row = ed::edx_line_of(d, start);
    static char line[8192];
    for (int r = row; r >= 0; r--) {
        int ls = ed::edx_line_start(d, r);
        int ll = ed::edx_line_len(d, r);
        if (ll > (int)sizeof(line) - 1) ll = (int)sizeof(line) - 1;
        ed::edx_copy(d, ls, ll, line);
        int with_limit = (r == row) ? (start - ls) : ll + 1;
        int from = 0, a, b, best_a = -1, best_b = -1;
        while (from <= ll && find_in_line(re, use_re, icase, pat, pn, line, ll, from, &a, &b)) {
            if (ls + a >= start) break;
            if (a < with_limit) { best_a = a; best_b = b; }
            from = (b > a) ? b : a + 1;
            if (from > ll) break;
        }
        if (best_a >= 0) { *ma = ls + best_a; *mb = ls + best_b; return true; }
    }
    return false;
}

void TextEditorApp::FindCompile() {
    find_err[0] = 0;
    if (find_q.len <= 0) { find_valid = false; return; }
    if (find_regex) {
        if (!re_ready) { find_valid = false; scpy(find_err, "regex unavailable", sizeof(find_err)); return; }
        find_valid = ed::re_compile(&re, find_q.text, !find_case);
        if (!find_valid) scpy(find_err, re.err[0] ? re.err : "invalid regex", sizeof(find_err));
    } else {
        find_valid = true;
    }
}

void TextEditorApp::FindOpen(bool replace) {
    bool was_open = find_open;
    find_open = true;
    find_replace = replace;
    find_field = 0;
    if (!was_open) {
        find_q.text[0] = 0;
        find_q.len = 0;
        find_q.cursor = 0;
        if (ed::edx_has_sel(D())) {
            int a, b;
            ed::edx_sel(D(), &a, &b);
            bool one_line = true;
            for (int i = a; i < b; i++) if (ed::edx_at(D(), i) == '\n') { one_line = false; break; }
            if (one_line && b - a > 0 && b - a < 80) {
                ed::edx_copy(D(), a, b - a, find_q.text);
                find_q.len = b - a;
                find_q.cursor = find_q.len;
            }
        }
    }
    FindCompile();
    have_match = false;
    match_a = match_b = 0;
    last_key_ms = kui::NowMs();
}

void TextEditorApp::FindClose() {
    find_open = false;
    find_replace = false;
    have_match = false;
    match_a = match_b = 0;
    find_err[0] = 0;
}

int TextEditorApp::FindOnLine(const ed::Doc* d, int row, int from, int* a, int* b) {
    int ll = ed::edx_line_len(d, row);
    static char line[8192];
    if (ll > (int)sizeof(line) - 1) ll = (int)sizeof(line) - 1;
    ed::edx_copy(d, ed::edx_line_start(d, row), ll, line);
    if (!find_in_line(&re, find_regex, !find_case, find_q.text, find_q.len, line, ll, from, a, b)) return 0;
    return 1;
}

bool TextEditorApp::FindNext(bool backward, bool select) {
    ed::Doc* d = D();
    if (!find_open || find_q.len <= 0) { SetStatus("Nothing to find", kui::NowMs()); return false; }
    FindCompile();
    if (!find_valid) { SetStatus(find_err[0] ? find_err : "Invalid pattern", kui::NowMs()); return false; }
    int start;
    if (have_match) start = backward ? match_a : match_b;
    else if (ed::edx_has_sel(d)) { int a, b; ed::edx_sel(d, &a, &b); start = backward ? a : b; }
    else start = d->cur;
    int a = 0, b = 0;
    bool got = false;
    find_wrapped = false;
    if (backward) {
        got = doc_find_backward(d, &re, find_regex, !find_case, find_q.text, find_q.len, start, &a, &b);
        if (!got) {
            got = doc_find_backward(d, &re, find_regex, !find_case, find_q.text, find_q.len, ed::edx_len(d), &a, &b);
            find_wrapped = got;
        }
    } else {
        got = doc_find_forward(d, &re, find_regex, !find_case, find_q.text, find_q.len, start, &a, &b);
        if (!got) {
            got = doc_find_forward(d, &re, find_regex, !find_case, find_q.text, find_q.len, 0, &a, &b);
            find_wrapped = got;
        }
    }
    if (!got) {
        have_match = false;
        SetStatus("No matches", kui::NowMs());
        return false;
    }
    have_match = true;
    match_a = a;
    match_b = b;
    if (select) {
        d->anchor = a;
        d->cur = b;
    }
    EnsureCaretVisible();
    if (find_wrapped) SetStatus("Search wrapped", kui::NowMs());
    return true;
}

void TextEditorApp::ReplaceCurrent() {
    ed::Doc* d = D();
    if (!find_open || !have_match) { FindNext(false, false); return; }
    if (match_b < match_a || match_a < 0 || match_b > ed::edx_len(d)) return;
    ed::edx_replace(d, match_a, match_b - match_a, find_repl.text, find_repl.len, false);
    have_match = false;
    FindNext(false, true);
    SetStatus("Replaced", kui::NowMs());
    if (win) SyncTitle(*win);
}

void TextEditorApp::ReplaceAll() {
    ed::Doc* d = D();
    if (!find_open || find_q.len <= 0) return;
    FindCompile();
    if (!find_valid) { SetStatus(find_err[0] ? find_err : "Invalid pattern", kui::NowMs()); return; }
    int pos = 0, count = 0;
    int a, b;
    while (count < 50000 && doc_find_forward(d, &re, find_regex, !find_case, find_q.text, find_q.len, pos, &a, &b)) {
        if (b < a) break;
        ed::edx_replace(d, a, b - a, find_repl.text, find_repl.len, false);
        pos = a + find_repl.len;
        if (b == a) pos = a + 1;
        count++;
    }
    char m[64]; scpy(m, "Replaced ", sizeof(m));
    char nb[16]; int_to_str(count, nb, sizeof(nb)); sapp(m, nb, sizeof(m));
    sapp(m, " matches", sizeof(m));
    SetStatus(m, kui::NowMs());
    have_match = false;
    if (win) SyncTitle(*win);
}

// end (satoru)
