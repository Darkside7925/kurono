// editor_highlight.h: a small per line syntax highlighter for the languages the editor knows.
// the multi line state (block comments, triple strings, markdown fences) is cached per line start
// and recomputed lazily from the first edited line, so only visible lines are tokenized per frame. (satoru)
#pragma once

#include "editor_text.h"

namespace ed {

enum TokKind {
    TK_TEXT = 0, TK_KEYWORD, TK_TYPE, TK_STRING, TK_COMMENT, TK_NUMBER,
    TK_PREPROC, TK_FUNC, TK_OP, TK_PUNCT, TK_HEADING, TK_LINK
};

struct Tok {
    int start;
    int len;
    unsigned char kind;
};

#define HL_MAX_SPANS 128
#define HL_SCRATCH 4096

static inline bool hl_id0(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline bool hl_id(char c) {
    return hl_id0(c) || (c >= '0' && c <= '9');
}
static inline bool hl_digit(char c) { return c >= '0' && c <= '9'; }
static inline char hl_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static inline void hl_add(Tok* out, int* n, int start, int len, int kind) {
    if (len <= 0 || *n >= HL_MAX_SPANS) return;
    if (*n > 0) {
        Tok* p = &out[*n - 1];
        if (p->kind == kind && p->start + p->len == start) { p->len += len; return; }
    }
    out[*n].start = start;
    out[*n].len = len;
    out[*n].kind = (unsigned char)kind;
    (*n)++;
}

static inline bool hl_word_is(const char* w, int n, const char* const* list, int count) {
    for (int i = 0; i < count; i++) {
        const char* a = list[i];
        int k = 0;
        while (k < n && a[k] && w[k] == a[k]) k++;
        if (k == n && !a[k]) return true;
    }
    return false;
}

static inline bool hl_prev_word_is(const char* s, int i, const char* w) {
    int b = i - 1;
    while (b >= 0 && (s[b] == ' ' || s[b] == '\t')) b--;
    int e = b;
    while (b >= 0 && hl_id(s[b])) b--;
    int n = e - b;
    if (n <= 0) return false;
    int k = 0;
    while (k < n && w[k] && s[b + 1 + k] == w[k]) k++;
    return k == n && !w[k];
}

static const char* const k_c_kw[] = {
    "if","else","for","while","do","switch","case","default","break","continue","return","goto",
    "sizeof","typedef","struct","union","enum","static","extern","const","volatile","register","inline",
    "signed","unsigned","void","char","short","int","long","float","double","_Bool","_Atomic",
    "restrict","alignof","alignas","static_assert","thread_local","typeof","auto"
};
static const char* const k_cpp_kw[] = {
    "if","else","for","while","do","switch","case","default","break","continue","return","goto",
    "sizeof","typedef","struct","union","enum","static","extern","const","volatile","register","inline",
    "signed","unsigned","void","char","short","int","long","float","double","bool","true","false","nullptr",
    "class","namespace","template","typename","using","public","private","protected","virtual","override",
    "final","new","delete","this","operator","friend","constexpr","consteval","constinit","decltype",
    "noexcept","throw","try","catch","static_cast","dynamic_cast","reinterpret_cast","const_cast",
    "explicit","mutable","auto","concept","requires","co_await","co_return","co_yield","import","module",
    "alignof","alignas","static_assert","thread_local","wchar_t","char8_t","char16_t","char32_t"
};
static const char* const k_c_type[] = {
    "void","char","short","int","long","float","double","signed","unsigned","bool","size_t","ssize_t",
    "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t","intptr_t","uintptr_t",
    "FILE","uint32_t","uint64_t","char16_t","char32_t","wchar_t"
};
static const char* const k_py_kw[] = {
    "if","elif","else","for","while","break","continue","return","def","class","import","from","as",
    "try","except","finally","raise","with","lambda","pass","global","nonlocal","assert","del","yield",
    "await","async","in","is","not","and","or","None","True","False","match","case","type"
};
static const char* const k_js_kw[] = {
    "if","else","for","while","do","switch","case","default","break","continue","return","function",
    "var","let","const","class","extends","super","new","delete","typeof","instanceof","in","of",
    "this","null","undefined","true","false","throw","try","catch","finally","async","await","yield",
    "import","export","from","as","static","get","set","void","interface","type","enum","namespace",
    "public","private","protected","readonly","abstract","implements","declare","keyof","infer"
};
static const char* const k_sh_kw[] = {
    "if","then","else","elif","fi","for","while","until","do","done","case","esac","function","in",
    "select","time","coproc","break","continue","return","exit","export","local","readonly","declare",
    "typeset","unset","shift","source","alias","unalias","eval","exec","trap","set","test","true","false"
};
static const char* const k_kcl_kw[] = {
    "set","let","print","println","if","then","else","elif","end","while","for","in","do","func",
    "function","return","import","break","continue","true","false","none","null","and","or","not","exec"
};

static inline void hl_scan_string(const char* s, int n, int* i, char q, int* state, Tok* out, int* no) {
    int st = *i;
    int j = *i + 1;
    while (j < n) {
        char c = s[j];
        if (c == '\\' && j + 1 < n) { j += 2; continue; }
        if (c == q) { j++; break; }
        if (c == '\n') break;
        j++;
    }
    hl_add(out, no, st, j - st, TK_STRING);
    *i = j;
    (void)state;
}

static inline int hl_line(int lang, const char* s, int n, int state, Tok* out, int max, int* end_state) {
    int no = 0;
    int i = 0;
    if (max > HL_MAX_SPANS) max = HL_MAX_SPANS;
    if (n > HL_SCRATCH - 1) n = HL_SCRATCH - 1;

    if (lang == LANG_C || lang == LANG_CPP) {
        if (state == 1) {
            int j = i;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { j += 2; state = 0; }
            else { j = n; }
            hl_add(out, &no, 0, j, TK_COMMENT);
            i = j;
        }
        while (i < n) {
            char c = s[i];
            if (c == '/' && i + 1 < n && s[i + 1] == '/') { hl_add(out, &no, i, n - i, TK_COMMENT); i = n; break; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') {
                int j = i + 2;
                while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
                if (j + 1 < n) j += 2; else { j = n; state = 1; }
                hl_add(out, &no, i, j - i, TK_COMMENT);
                i = j;
                continue;
            }
            if (c == '"' || c == '\'') { hl_scan_string(s, n, &i, c, &state, out, &no); continue; }
            if (c == '#') {
                int b = i - 1;
                while (b >= 0 && (s[b] == ' ' || s[b] == '\t')) b--;
                if (b < 0) { hl_add(out, &no, i, n - i, TK_PREPROC); i = n; break; }
            }
            if (hl_digit(c) || (c == '.' && i + 1 < n && hl_digit(s[i + 1]))) {
                int j = i;
                while (j < n && (hl_id(s[j]) || s[j] == '.' || s[j] == '\'' ||
                                 ((s[j] == '+' || s[j] == '-') && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E' || s[j - 1] == 'p' || s[j - 1] == 'P')))) j++;
                hl_add(out, &no, i, j - i, TK_NUMBER);
                i = j;
                continue;
            }
            if (hl_id0(c)) {
                int j = i + 1;
                while (j < n && hl_id(s[j])) j++;
                int wl = j - i;
                int kind = TK_TEXT;
                const char* const* kw = (lang == LANG_C) ? k_c_kw : k_cpp_kw;
                int kwc = (lang == LANG_C) ? (int)(sizeof(k_c_kw) / sizeof(k_c_kw[0])) : (int)(sizeof(k_cpp_kw) / sizeof(k_cpp_kw[0]));
                if (hl_word_is(s + i, wl, kw, kwc) || hl_word_is(s + i, wl, k_c_type, (int)(sizeof(k_c_type) / sizeof(k_c_type[0])))) {
                    bool is_type = hl_word_is(s + i, wl, k_c_type, (int)(sizeof(k_c_type) / sizeof(k_c_type[0])));
                    kind = is_type ? TK_TYPE : TK_KEYWORD;
                } else if (wl > 0 && s[i] >= 'A' && s[i] <= 'Z') {
                    kind = TK_TYPE;
                } else {
                    int k = j;
                    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                    if (k < n && s[k] == '(') kind = TK_FUNC;
                }
                if (kind != TK_TEXT) hl_add(out, &no, i, wl, kind);
                i = j;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '!' ||
                c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '~' || c == '?') {
                int j = i + 1;
                while (j < n && (s[j] == '=' || s[j] == '&' || s[j] == '|' || s[j] == '+' || s[j] == '-' ||
                                 s[j] == '<' || s[j] == '>' || s[j] == '*')) j++;
                hl_add(out, &no, i, j - i, TK_OP);
                i = j;
                continue;
            }
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ';' || c == ':') {
                hl_add(out, &no, i, 1, TK_PUNCT);
                i++;
                continue;
            }
            i++;
        }
        if (end_state) *end_state = state;
        return no;
    }

    if (lang == LANG_PY) {
        if (state == 2 || state == 3) {
            char q = (state == 2) ? '\'' : '"';
            int j = i;
            while (j + 2 < n && !(s[j] == q && s[j + 1] == q && s[j + 2] == q)) j++;
            if (j + 2 < n) { j += 3; state = 0; } else j = n;
            hl_add(out, &no, 0, j, TK_STRING);
            i = j;
        }
        while (i < n) {
            char c = s[i];
            if (c == '#') { hl_add(out, &no, i, n - i, TK_COMMENT); i = n; break; }
            if (c == '\'' || c == '"') {
                if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
                    int st = i;
                    int j = i + 3;
                    while (j + 2 < n && !(s[j] == c && s[j + 1] == c && s[j + 2] == c)) j++;
                    if (j + 2 < n) j += 3; else { j = n; state = (c == '\'') ? 2 : 3; }
                    hl_add(out, &no, st, j - st, TK_STRING);
                    i = j;
                    continue;
                }
                hl_scan_string(s, n, &i, c, &state, out, &no);
                continue;
            }
            if (hl_digit(c) || (c == '.' && i + 1 < n && hl_digit(s[i + 1]))) {
                int j = i;
                while (j < n && (hl_id(s[j]) || s[j] == '.' ||
                                 ((s[j] == '+' || s[j] == '-') && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E')))) j++;
                hl_add(out, &no, i, j - i, TK_NUMBER);
                i = j;
                continue;
            }
            if (hl_id0(c)) {
                int j = i + 1;
                while (j < n && hl_id(s[j])) j++;
                int wl = j - i;
                int kind = TK_TEXT;
                if (hl_word_is(s + i, wl, k_py_kw, (int)(sizeof(k_py_kw) / sizeof(k_py_kw[0])))) kind = TK_KEYWORD;
                else {
                    if (hl_prev_word_is(s, i, "def") || hl_prev_word_is(s, i, "class")) kind = TK_FUNC;
                    else if (j < n && s[j] == '(') kind = TK_FUNC;
                }
                if (kind != TK_TEXT) hl_add(out, &no, i, wl, kind);
                i = j;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '!' ||
                c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '~') {
                hl_add(out, &no, i, 1, TK_OP);
                i++;
                continue;
            }
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':') {
                hl_add(out, &no, i, 1, TK_PUNCT);
            }
            i++;
        }
        if (end_state) *end_state = state;
        return no;
    }

    if (lang == LANG_JS) {
        if (state == 1) {
            int j = i;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { j += 2; state = 0; }
            else j = n;
            hl_add(out, &no, 0, j, TK_COMMENT);
            i = j;
        }
        if (state == 6) {
            int j = i;
            while (j < n) {
                if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
                if (s[j] == '`') { j++; state = 0; break; }
                j++;
            }
            hl_add(out, &no, 0, j, TK_STRING);
            i = j;
        }
        while (i < n) {
            char c = s[i];
            if (c == '/' && i + 1 < n && s[i + 1] == '/') { hl_add(out, &no, i, n - i, TK_COMMENT); i = n; break; }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') {
                int j = i + 2;
                while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
                if (j + 1 < n) j += 2; else { j = n; state = 1; }
                hl_add(out, &no, i, j - i, TK_COMMENT);
                i = j;
                continue;
            }
            if (c == '\'' || c == '"') { hl_scan_string(s, n, &i, c, &state, out, &no); continue; }
            if (c == '`') {
                int st = i;
                int j = i + 1;
                while (j < n) {
                    if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
                    if (s[j] == '`') { j++; break; }
                    j++;
                }
                if (j >= n && (j == 0 || s[j - 1] != '`')) state = 6;
                hl_add(out, &no, st, j - st, TK_STRING);
                i = j;
                continue;
            }
            if (hl_digit(c)) {
                int j = i;
                while (j < n && (hl_id(s[j]) || s[j] == '.')) j++;
                hl_add(out, &no, i, j - i, TK_NUMBER);
                i = j;
                continue;
            }
            if (hl_id0(c)) {
                int j = i + 1;
                while (j < n && hl_id(s[j])) j++;
                int wl = j - i;
                int kind = TK_TEXT;
                if (hl_word_is(s + i, wl, k_js_kw, (int)(sizeof(k_js_kw) / sizeof(k_js_kw[0])))) kind = TK_KEYWORD;
                else if (j < n && s[j] == '(') kind = TK_FUNC;
                else if (s[i] >= 'A' && s[i] <= 'Z') kind = TK_TYPE;
                if (kind != TK_TEXT) hl_add(out, &no, i, wl, kind);
                i = j;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '!' ||
                c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '~' || c == '?') {
                hl_add(out, &no, i, 1, TK_OP);
                i++;
                continue;
            }
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ';' || c == ':') {
                hl_add(out, &no, i, 1, TK_PUNCT);
            }
            i++;
        }
        if (end_state) *end_state = state;
        return no;
    }

    if (lang == LANG_SH || lang == LANG_KCL) {
        while (i < n) {
            char c = s[i];
            if (c == '#') { hl_add(out, &no, i, n - i, TK_COMMENT); i = n; break; }
            if (c == '\'' || c == '"') { hl_scan_string(s, n, &i, c, &state, out, &no); continue; }
            if (c == '$' && i + 1 < n && (hl_id0(s[i + 1]) || s[i + 1] == '{' || s[i + 1] == '(')) {
                int j = i + 1;
                if (s[j] == '{') { while (j < n && s[j] != '}') j++; if (j < n) j++; }
                else if (s[j] == '(') { while (j < n && s[j] != ')') j++; if (j < n) j++; }
                else { while (j < n && hl_id(s[j])) j++; }
                hl_add(out, &no, i, j - i, TK_TYPE);
                i = j;
                continue;
            }
            if (hl_digit(c)) {
                int j = i;
                while (j < n && (hl_id(s[j]) || s[j] == '.')) j++;
                hl_add(out, &no, i, j - i, TK_NUMBER);
                i = j;
                continue;
            }
            if (hl_id0(c)) {
                int j = i + 1;
                while (j < n && hl_id(s[j])) j++;
                int wl = j - i;
                const char* const* kw = (lang == LANG_SH) ? k_sh_kw : k_kcl_kw;
                int kwc = (lang == LANG_SH) ? (int)(sizeof(k_sh_kw) / sizeof(k_sh_kw[0])) : (int)(sizeof(k_kcl_kw) / sizeof(k_kcl_kw[0]));
                int kind = hl_word_is(s + i, wl, kw, kwc) ? TK_KEYWORD : TK_TEXT;
                if (kind == TK_TEXT && j < n && s[j] == '(') kind = TK_FUNC;
                if (kind != TK_TEXT) hl_add(out, &no, i, wl, kind);
                i = j;
                continue;
            }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '!' ||
                c == '<' || c == '>' || c == '&' || c == '|') {
                hl_add(out, &no, i, 1, TK_OP);
                i++;
                continue;
            }
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ';' || c == ':') {
                hl_add(out, &no, i, 1, TK_PUNCT);
            }
            i++;
        }
        if (end_state) *end_state = state;
        return no;
    }

    if (lang == LANG_MD) {
        if (state == 4) {
            int p = 0;
            while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p + 2 < n && s[p] == '`' && s[p + 1] == '`' && s[p + 2] == '`') {
                hl_add(out, &no, 0, n, TK_COMMENT);
                if (end_state) *end_state = 0;
                return no;
            }
            hl_add(out, &no, 0, n, TK_STRING);
            if (end_state) *end_state = 4;
            return no;
        }
        int p = 0;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p + 2 < n && s[p] == '`' && s[p + 1] == '`' && s[p + 2] == '`') {
            hl_add(out, &no, 0, n, TK_COMMENT);
            if (end_state) *end_state = 4;
            return no;
        }
        if (p < n && s[p] == '#') {
            int j = p;
            while (j < n && s[j] == '#') j++;
            hl_add(out, &no, 0, n, TK_HEADING);
            if (end_state) *end_state = 0;
            return no;
        }
        if (p < n && s[p] == '>') { hl_add(out, &no, p, n - p, TK_COMMENT); if (end_state) *end_state = 0; return no; }
        if (p + 1 < n && (s[p] == '-' || s[p] == '*' || s[p] == '+') && s[p + 1] == ' ') {
            hl_add(out, &no, p, 1, TK_OP);
        }
        i = 0;
        while (i < n) {
            if (s[i] == '`') {
                int j = i + 1;
                while (j < n && s[j] != '`') j++;
                if (j < n) j++;
                hl_add(out, &no, i, j - i, TK_STRING);
                i = j;
                continue;
            }
            if (s[i] == '[') {
                int j = i + 1;
                while (j < n && s[j] != ']') j++;
                if (j < n && j + 1 < n && s[j + 1] == '(') {
                    int k = j + 2;
                    while (k < n && s[k] != ')') k++;
                    if (k < n) k++;
                    hl_add(out, &no, i, k - i, TK_LINK);
                    i = k;
                    continue;
                }
            }
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
                int j = i + 2;
                while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '*')) j++;
                if (j + 1 < n) j += 2;
                hl_add(out, &no, i, j - i, TK_KEYWORD);
                i = j;
                continue;
            }
            i++;
        }
        if (end_state) *end_state = 0;
        return no;
    }

    if (lang == LANG_JSON) {
        while (i < n) {
            char c = s[i];
            if (c == '"') {
                int st = i;
                int j = i + 1;
                while (j < n) {
                    if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
                    if (s[j] == '"') { j++; break; }
                    j++;
                }
                int k = j;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                hl_add(out, &no, st, j - st, (k < n && s[k] == ':') ? TK_FUNC : TK_STRING);
                i = j;
                continue;
            }
            if (c == '/' && i + 1 < n && s[i + 1] == '/') { hl_add(out, &no, i, n - i, TK_COMMENT); i = n; break; }
            if (hl_digit(c) || c == '-') {
                int j = i + 1;
                while (j < n && (hl_digit(s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                                 s[j] == '+' || s[j] == '-')) j++;
                hl_add(out, &no, i, j - i, TK_NUMBER);
                i = j;
                continue;
            }
            if (hl_id0(c)) {
                static const char* const jkw[] = { "true", "false", "null" };
                int j = i + 1;
                while (j < n && hl_id(s[j])) j++;
                int wl = j - i;
                if (hl_word_is(s + i, wl, jkw, 3)) hl_add(out, &no, i, wl, TK_KEYWORD);
                i = j;
                continue;
            }
            if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':')
                hl_add(out, &no, i, 1, TK_PUNCT);
            i++;
        }
        if (end_state) *end_state = 0;
        return no;
    }

    if (end_state) *end_state = 0;
    return no;
}

static inline void hl_ensure(Doc* d, int line) {
    if (line < 0) line = 0;
    if (line >= d->nlines) line = d->nlines - 1;
    if (d->hl_cap < d->nlines + 1) {
        int nc = d->hl_cap ? d->hl_cap : 256;
        while (nc < d->nlines + 1) nc *= 2;
        unsigned char* nh = (unsigned char*)edx_alloc((size_t)nc);
        if (!nh) return;
        if (d->hl && d->hl_valid > 0) memcpy(nh, d->hl, (size_t)(d->hl_valid < nc ? d->hl_valid : nc));
        edx_free(d->hl);
        d->hl = nh;
        d->hl_cap = nc;
    }
    if (d->hl_valid < 1) { d->hl[0] = 0; d->hl_valid = 1; }
    if (d->hl_valid > d->nlines) d->hl_valid = d->nlines;
    if (d->hl_valid > line + 1) return;
    static char tmp[HL_SCRATCH];
    static Tok toks[HL_MAX_SPANS];
    while (d->hl_valid <= line) {
        int row = d->hl_valid - 1;
        int len = edx_line_len(d, row);
        if (len > HL_SCRATCH - 1) len = HL_SCRATCH - 1;
        edx_copy(d, edx_line_start(d, row), len, tmp);
        int st = 0;
        hl_line(d->lang, tmp, len, d->hl[row], toks, HL_MAX_SPANS, &st);
        if (d->hl_valid + 1 >= d->hl_cap) {
            int nc = d->hl_cap ? d->hl_cap * 2 : 256;
            unsigned char* nh = (unsigned char*)edx_alloc((size_t)nc);
            if (!nh) break;
            memcpy(nh, d->hl, (size_t)d->hl_valid);
            edx_free(d->hl);
            d->hl = nh;
            d->hl_cap = nc;
        }
        d->hl[d->hl_valid] = (unsigned char)st;
        d->hl_valid++;
    }
}

static inline int lang_from_path(const char* path) {
    if (!path) return LANG_PLAIN;
    int n = 0;
    while (path[n]) n++;
    int dot = -1;
    for (int i = n - 1; i >= 0; i--) {
        if (path[i] == '.') { dot = i; break; }
        if (path[i] == '/') break;
    }
    if (dot < 0) return LANG_PLAIN;
    char e[12];
    int k = 0;
    for (int i = dot + 1; i < n && k < 11; i++) e[k++] = hl_lower(path[i]);
    e[k] = 0;
    const char* const cpp_ext[] = { "cpp","cc","cxx","hpp","hh","hxx","h","ipp","tcc","ino" };
    const char* const c_ext[] = { "c" };
    const char* const py_ext[] = { "py","pyw","pyi" };
    const char* const js_ext[] = { "js","mjs","cjs","jsx","ts","tsx" };
    const char* const sh_ext[] = { "sh","bash","zsh","ksh","profile" };
    const char* const md_ext[] = { "md","markdown","mdown","mkd" };
    const char* const json_ext[] = { "json","jsonc","geojson","lock" };
    const char* const kcl_ext[] = { "kcl" };
    if (hl_word_is(e, k, c_ext, 1)) return LANG_C;
    if (hl_word_is(e, k, cpp_ext, (int)(sizeof(cpp_ext) / sizeof(cpp_ext[0])))) return LANG_CPP;
    if (hl_word_is(e, k, py_ext, (int)(sizeof(py_ext) / sizeof(py_ext[0])))) return LANG_PY;
    if (hl_word_is(e, k, js_ext, (int)(sizeof(js_ext) / sizeof(js_ext[0])))) return LANG_JS;
    if (hl_word_is(e, k, sh_ext, (int)(sizeof(sh_ext) / sizeof(sh_ext[0])))) return LANG_SH;
    if (hl_word_is(e, k, md_ext, (int)(sizeof(md_ext) / sizeof(md_ext[0])))) return LANG_MD;
    if (hl_word_is(e, k, json_ext, (int)(sizeof(json_ext) / sizeof(json_ext[0])))) return LANG_JSON;
    if (hl_word_is(e, k, kcl_ext, 1)) return LANG_KCL;
    return LANG_PLAIN;
}

static inline const char* lang_name(int l) {
    switch (l) {
    case LANG_C: return "C";
    case LANG_CPP: return "C++";
    case LANG_PY: return "Python";
    case LANG_JS: return "JavaScript";
    case LANG_SH: return "Shell";
    case LANG_MD: return "Markdown";
    case LANG_JSON: return "JSON";
    case LANG_KCL: return "KCL";
    default: return "Plain Text";
    }
}

}  // namespace ed

// end (satoru)
