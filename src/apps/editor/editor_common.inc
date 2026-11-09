// editor_common.inc: constants, string and path helpers, and the menu tables shared by the
// rest of the text editor implementation. included by text_editor.cpp only. (satoru)

#define ED_FILE_MAX (4 * 1024 * 1024)
#define ED_TABSTOP  4

static char g_open_path[ED_PATH_MAX] = "";

static int slen(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void scpy(char* d, const char* s, int mx) {
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void sapp(char* d, const char* s, int mx) {
    int n = slen(d), i = 0;
    if (s) while (s[i] && n < mx - 1) { d[n++] = s[i++]; }
    d[n] = 0;
}
static void int_to_str(int v, char* b, int mx) {
    if (mx < 2) { b[0] = 0; return; }
    if (v < 0) { b[0] = '-'; int_to_str(-v, b + 1, mx - 1); return; }
    char t[16]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 15);
    int i = 0; while (n > 0 && i < mx - 1) b[i++] = t[--n];
    b[i] = 0;
}
static bool seq(const char* a, const char* b) {
    int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return false; i++; }
    return a[i] == b[i];
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int istr_cmp(const char* a, const char* b) {
    while (*a || *b) {
        char ca = lowerc(*a), cb = lowerc(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return 0;
}
static const char* base_name(const char* p) {
    if (!p) return "";
    const char* b = p;
    for (int i = 0; p[i]; i++) if (p[i] == '/') b = p + i + 1;
    return b;
}
static void dir_name(const char* p, char* out, int mx) {
    if (!p || !p[0]) { scpy(out, "/home/user", mx); return; }
    int last = -1, n = slen(p);
    for (int i = 0; i < n; i++) if (p[i] == '/') last = i;
    if (last < 0) { scpy(out, "/home/user", mx); return; }
    if (last == 0) { scpy(out, "/", mx); return; }
    int k = 0; while (k < last && k < mx - 1) { out[k] = p[k]; k++; }
    out[k] = 0;
}
static void path_join(const char* a, const char* b, char* out, int mx) {
    if (!b || !b[0]) { scpy(out, a ? a : "", mx); return; }
    if (b[0] == '/') { scpy(out, b, mx); return; }
    int n = slen(a);
    int k = 0;
    if (a) while (k < n && k < mx - 1) { out[k] = a[k]; k++; }
    if (k > 0 && out[k - 1] != '/' && k < mx - 1) out[k++] = '/';
    int i = 0;
    while (b[i] && k < mx - 1) out[k++] = b[i++];
    out[k] = 0;
}
static void path_parent(const char* p, char* out, int mx) {
    if (!p || p[0] != '/') { scpy(out, "/home/user", mx); return; }
    int n = slen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    int last = -1;
    for (int i = 0; i < n; i++) if (p[i] == '/') last = i;
    if (last <= 0) { scpy(out, "/", mx); return; }
    int k = 0; while (k < last && k < mx - 1) { out[k] = p[k]; k++; }
    out[k] = 0;
}
static void trim_tail(char* s) {
    int n = slen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// builds the tab-expanded display string for one line. bmap maps each source byte to its
// display column (a tab maps to its first column), so selection and search ranges convert
// without guessing. returns the display length. (satoru)
static int BuildDisplay(const ed::Doc* d, int row, char* out, int cap, int* bmap) {
    int len = ed::edx_line_len(d, row);
    int start = ed::edx_line_start(d, row);
    if (len > cap - 1) len = cap - 1;
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (bmap) bmap[i] = n;
        char c = ed::edx_at(d, start + i);
        if (c == '\t') {
            int t = ED_TABSTOP - (n % ED_TABSTOP);
            while (t-- > 0 && n < cap - 1) out[n++] = ' ';
        } else if (c != '\r' && c != '\n') {
            out[n++] = c;
        }
    }
    if (bmap) bmap[len] = n;
    out[n] = 0;
    return n;
}

static int DispAtByte(const ed::Doc* d, int row, int byte_col, int cap) {
    int len = ed::edx_line_len(d, row);
    int start = ed::edx_line_start(d, row);
    int col = clampi(byte_col, 0, len);
    int n = 0;
    for (int i = 0; i < col; i++) {
        char c = ed::edx_at(d, start + i);
        if (c == '\t') n += ED_TABSTOP - (n % ED_TABSTOP);
        else if (c != '\r' && c != '\n') n++;
        if (n > cap) return cap;
    }
    return n;
}

// next wrap boundary: prefers the last space inside the row (word wrap), otherwise breaks at
// the column. start >= len means the line is done. (satoru)
static int WrapNext(const char* disp, int len, int start, int max_cols) {
    if (start >= len) return len;
    int e = start + max_cols;
    if (e >= len) return len;
    int br = e;
    while (br > start && disp[br - 1] != ' ') br--;
    if (br > start + max_cols / 2) return br;
    return e;
}

static void sort_entries(kui::DirEntry* e, int n) {
    for (int i = 1; i < n; i++) {
        kui::DirEntry t = e[i];
        int j = i - 1;
        while (j >= 0) {
            if (e[j].dir && !t.dir) break;
            if (e[j].dir == t.dir && istr_cmp(e[j].name, t.name) <= 0) break;
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = t;
    }
}

// ---------------------------------------------------------------- menu tables (satoru)

enum {
    A_NONE = 0,
    A_NEW, A_OPEN, A_FOLDER, A_SAVE, A_SAVEAS, A_CLOSE, A_EXIT,
    A_UNDO, A_REDO, A_CUT, A_COPY, A_PASTE, A_SELECT, A_FIND, A_REPLACE, A_GOTO,
    A_SIDE, A_WRAP, A_LINES, A_BIGGER, A_SMALLER, A_KEYS, A_ABOUT
};

struct EdMenuItem { const char* label; const char* key; int act; };

static const EdMenuItem kFileMenu[] = {
    { "New Tab",        "Ctrl+N",       A_NEW },
    { "Open File...",   "Ctrl+O",       A_OPEN },
    { "Open Folder...", "",             A_FOLDER },
    { "-",              "",             A_NONE },
    { "Save",           "Ctrl+S",       A_SAVE },
    { "Save As...",     "Ctrl+Shift+S", A_SAVEAS },
    { "-",              "",             A_NONE },
    { "Close Tab",      "Ctrl+W",       A_CLOSE },
    { "Exit",           "",             A_EXIT },
};
static const EdMenuItem kEditMenu[] = {
    { "Undo",           "Ctrl+Z",       A_UNDO },
    { "Redo",           "Ctrl+Y",       A_REDO },
    { "-",              "",             A_NONE },
    { "Cut",            "Ctrl+X",       A_CUT },
    { "Copy",           "Ctrl+C",       A_COPY },
    { "Paste",          "Ctrl+V",       A_PASTE },
    { "Select All",     "Ctrl+A",       A_SELECT },
    { "-",              "",             A_NONE },
    { "Find...",        "Ctrl+F",       A_FIND },
    { "Replace...",     "Ctrl+H",       A_REPLACE },
    { "Go to Line...",  "Ctrl+G",       A_GOTO },
};
static const EdMenuItem kViewMenu[] = {
    { "Toggle Sidebar", "Ctrl+B",       A_SIDE },
    { "Word Wrap",      "",             A_WRAP },
    { "Line Numbers",   "",             A_LINES },
    { "-",              "",             A_NONE },
    { "Zoom In",        "Ctrl++",       A_BIGGER },
    { "Zoom Out",       "Ctrl+-",       A_SMALLER },
};
static const EdMenuItem kHelpMenu[] = {
    { "Keyboard Shortcuts", "",         A_KEYS },
    { "About",              "",         A_ABOUT },
};

static const EdMenuItem* menu_items(int m, int* n) {
    if (m == 0) { *n = (int)(sizeof(kFileMenu) / sizeof(kFileMenu[0])); return kFileMenu; }
    if (m == 1) { *n = (int)(sizeof(kEditMenu) / sizeof(kEditMenu[0])); return kEditMenu; }
    if (m == 2) { *n = (int)(sizeof(kViewMenu) / sizeof(kViewMenu[0])); return kViewMenu; }
    *n = (int)(sizeof(kHelpMenu) / sizeof(kHelpMenu[0])); return kHelpMenu;
}

static const char* const kShortcutHelp[] = {
    "Ctrl+S         save the current file",
    "Ctrl+Shift+S   save as",
    "Ctrl+O         open a file",
    "Ctrl+N         new tab",
    "Ctrl+W         close tab",
    "Ctrl+F         find",
    "Ctrl+H         find and replace",
    "Ctrl+G         go to line",
    "Ctrl+Z / Y     undo / redo",
    "Ctrl+C/X/V     copy / cut / paste",
    "Ctrl+A         select all",
    "Ctrl+Tab       next tab",
    "Ctrl+Shift+Tab previous tab",
    "Ctrl+B         toggle the file sidebar",
    "Ctrl++ / -     zoom the editor",
    "F3 / Shift+F3  next / previous match",
    nullptr
};

// end (satoru)
