// editor_regex.h: a compact backtracking regular expression engine for find and replace.
// supports ., ^, $, literals, escapes (\d \w \s \b and friends), classes, groups,
// alternation and the *, +, ?, {n}, {n,}, {n,m} quantifiers. matching is per line,
// which is what the editor's find bar needs, and every step is bounded. (satoru)
#pragma once

#include "editor_text.h"

namespace ed {

enum ReOp {
    RE_CHAR = 0, RE_ANY, RE_CLASS, RE_BOL, RE_EOL, RE_BOUND, RE_SPLIT, RE_JMP, RE_MATCH
};

struct ReInst {
    unsigned char op;
    unsigned char neg;
    char ch;
    int x;
    int y;
    unsigned char cls[32];
};

enum ReNodeType {
    RN_EMPTY = 0, RN_CHAR, RN_ANY, RN_CLASS, RN_BOL, RN_EOL, RN_BOUND,
    RN_SEQ, RN_ALT, RN_REPEAT
};

struct ReNode {
    unsigned char type;
    unsigned char neg;
    char ch;
    int a;
    int b;
    int lo;
    int hi;
    unsigned char cls[32];
};

#define RE_CODE_MAX 4096
#define RE_NODE_MAX 1024
#define RE_STACK_MAX 2048
#define RE_STEP_MAX 200000

struct Regex {
    ReInst* code;
    ReNode* nodes;
    int ncode;
    int nnodes;
    int root;
    bool ok;
    bool icase;
    char err[72];
    int err_at;
    const char* pat;
    int plen;
    int ppos;
};

static inline void re_setbit(unsigned char* cls, int c) {
    if (c < 0 || c > 255) return;
    cls[c >> 3] = (unsigned char)(cls[c >> 3] | (1u << (c & 7)));
}
static inline bool re_getbit(const unsigned char* cls, int c) {
    return (cls[c >> 3] >> (c & 7)) & 1;
}
static inline void re_addchar(Regex* re, ReNode* n, int c) {
    re_setbit(n->cls, c);
    if (re->icase) {
        if (c >= 'a' && c <= 'z') re_setbit(n->cls, c - 32);
        else if (c >= 'A' && c <= 'Z') re_setbit(n->cls, c + 32);
    }
}
static inline void re_addclass_escape(Regex* re, ReNode* n, char e) {
    if (e == 'd') { for (int c = '0'; c <= '9'; c++) re_setbit(n->cls, c); }
    else if (e == 'w') { for (int c = '0'; c <= '9'; c++) re_setbit(n->cls, c);
                         for (int c = 'a'; c <= 'z'; c++) re_addchar(re, n, c);
                         for (int c = 'A'; c <= 'Z'; c++) re_addchar(re, n, c);
                         re_addchar(re, n, '_'); }
    else if (e == 's') { re_setbit(n->cls, ' '); re_setbit(n->cls, '\t'); re_setbit(n->cls, '\r'); re_setbit(n->cls, '\n'); re_setbit(n->cls, '\f'); re_setbit(n->cls, '\v'); }
    else if (e == 'D') { for (int c = 0; c < 256; c++) if (!(c >= '0' && c <= '9')) re_setbit(n->cls, c); }
    else if (e == 'W') { for (int c = 0; c < 256; c++) { bool w = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; if (!w) re_setbit(n->cls, c); } }
    else if (e == 'S') { for (int c = 0; c < 256; c++) { bool s = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; if (!s) re_setbit(n->cls, c); } }
}

static inline int re_new(Regex* re, int type) {
    if (re->nnodes >= RE_NODE_MAX) { re->ok = false; return 0; }
    ReNode* n = &re->nodes[re->nnodes];
    for (size_t i = 0; i < sizeof(*n); i++) ((char*)n)[i] = 0;
    n->type = (unsigned char)type;
    n->hi = -1;
    return re->nnodes++;
}

static inline int re_parse_alt(Regex* re);

static inline int re_parse_class(Regex* re) {
    int ni = re_new(re, RN_CLASS);
    if (!re->ok) return ni;
    ReNode* n = &re->nodes[ni];
    if (re->ppos < re->plen && re->pat[re->ppos] == '^') { n->neg = 1; re->ppos++; }
    bool first = true;
    while (re->ppos < re->plen) {
        char c = re->pat[re->ppos++];
        if (c == ']' && !first) return ni;
        first = false;
        if (c == '\\' && re->ppos < re->plen) {
            char e = re->pat[re->ppos++];
            if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
                re_addclass_escape(re, n, e);
                continue;
            }
            c = e;
            if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
            else if (e == 'f') c = '\f'; else if (e == 'v') c = '\v';
        }
        if (re->ppos < re->plen && re->pat[re->ppos] == '-' &&
            re->ppos + 1 < re->plen && re->pat[re->ppos + 1] != ']') {
            re->ppos++;
            int hi = (unsigned char)re->pat[re->ppos++];
            if (hi == '\\' && re->ppos < re->plen) {
                char e = re->pat[re->ppos++];
                hi = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : (unsigned char)e;
            }
            int lo = (unsigned char)c;
            if (lo <= hi) for (int x = lo; x <= hi; x++) re_addchar(re, n, x);
            continue;
        }
        re_addchar(re, n, (unsigned char)c);
    }
    re->ok = false;
    if (re->err[0] == 0) {
        re->err_at = re->ppos;
        const char* e = "unterminated character class";
        int i = 0; while (e[i] && i < (int)sizeof(re->err) - 1) { re->err[i] = e[i]; i++; } re->err[i] = 0;
    }
    return ni;
}

static inline int re_parse_atom(Regex* re) {
    if (re->ppos >= re->plen) return re_new(re, RN_EMPTY);
    char c = re->pat[re->ppos];
    if (c == '(') {
        re->ppos++;
        int inner = re_parse_alt(re);
        if (re->ppos < re->plen && re->pat[re->ppos] == ')') re->ppos++;
        else if (re->ok) {
            re->ok = false;
            re->err_at = re->ppos;
            const char* e = "missing )";
            int i = 0; while (e[i] && i < (int)sizeof(re->err) - 1) { re->err[i] = e[i]; i++; } re->err[i] = 0;
        }
        return inner;
    }
    if (c == '[') { re->ppos++; return re_parse_class(re); }
    if (c == '.') { re->ppos++; return re_new(re, RN_ANY); }
    if (c == '^') { re->ppos++; return re_new(re, RN_BOL); }
    if (c == '$') { re->ppos++; return re_new(re, RN_EOL); }
    if (c == '\\') {
        re->ppos++;
        if (re->ppos >= re->plen) {
            int ni = re_new(re, RN_CHAR);
            re->nodes[ni].ch = '\\';
            return ni;
        }
        char e = re->pat[re->ppos++];
        if (e == 'b' || e == 'B') {
            int ni = re_new(re, RN_BOUND);
            re->nodes[ni].neg = (e == 'B');
            return ni;
        }
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            int ni = re_new(re, RN_CLASS);
            re_addclass_escape(re, &re->nodes[ni], e);
            return ni;
        }
        char lit = e;
        if (e == 'n') lit = '\n'; else if (e == 't') lit = '\t'; else if (e == 'r') lit = '\r';
        else if (e == 'f') lit = '\f'; else if (e == 'v') lit = '\v'; else if (e == '0') lit = 0;
        int ni = re_new(re, RN_CHAR);
        re->nodes[ni].ch = lit;
        return ni;
    }
    re->ppos++;
    int ni = re_new(re, RN_CHAR);
    re->nodes[ni].ch = c;
    return ni;
}

static inline int re_parse_repeat(Regex* re) {
    int a = re_parse_atom(re);
    if (!re->ok || re->ppos >= re->plen) return a;
    char c = re->pat[re->ppos];
    int lo = -1, hi = -1;
    if (c == '*') { lo = 0; hi = -1; re->ppos++; }
    else if (c == '+') { lo = 1; hi = -1; re->ppos++; }
    else if (c == '?') { lo = 0; hi = 1; re->ppos++; }
    else if (c == '{') {
        int save = re->ppos++;
        int n1 = 0, n2 = 0;
        bool got1 = false, got2 = false, comma = false;
        while (re->ppos < re->plen && re->pat[re->ppos] >= '0' && re->pat[re->ppos] <= '9') {
            n1 = n1 * 10 + (re->pat[re->ppos] - '0'); re->ppos++; got1 = true;
        }
        if (re->ppos < re->plen && re->pat[re->ppos] == ',') {
            comma = true; re->ppos++;
            while (re->ppos < re->plen && re->pat[re->ppos] >= '0' && re->pat[re->ppos] <= '9') {
                n2 = n2 * 10 + (re->pat[re->ppos] - '0'); re->ppos++; got2 = true;
            }
        }
        if (re->ppos < re->plen && re->pat[re->ppos] == '}' && got1) {
            re->ppos++;
            lo = n1;
            hi = comma ? (got2 ? n2 : -1) : n1;
        } else {
            re->ppos = save;
        }
    }
    if (lo < 0) return a;
    if (hi >= 0 && hi < lo) { re->ok = false; if (re->err[0] == 0) { const char* e = "bad repeat range"; int i = 0; while (e[i] && i < (int)sizeof(re->err) - 1) { re->err[i] = e[i]; i++; } re->err[i] = 0; } }
    int ni = re_new(re, RN_REPEAT);
    ReNode* n = &re->nodes[ni];
    n->a = a;
    n->lo = lo;
    n->hi = hi;
    // '?' after a quantifier makes it lazy; treated as greedy like most simple engines. (satoru)
    if (re->ppos < re->plen && re->pat[re->ppos] == '?') re->ppos++;
    return ni;
}

static inline int re_parse_seq(Regex* re) {
    int first = -1;
    while (re->ppos < re->plen && re->pat[re->ppos] != ')' && re->pat[re->ppos] != '|') {
        int part = re_parse_repeat(re);
        if (!re->ok) return first >= 0 ? first : 0;
        if (first < 0) first = part;
        else {
            int s = re_new(re, RN_SEQ);
            re->nodes[s].a = first;
            re->nodes[s].b = part;
            first = s;
        }
    }
    if (first < 0) first = re_new(re, RN_EMPTY);
    return first;
}

static inline int re_parse_alt(Regex* re) {
    int a = re_parse_seq(re);
    while (re->ok && re->ppos < re->plen && re->pat[re->ppos] == '|') {
        re->ppos++;
        int b = re_parse_seq(re);
        int s = re_new(re, RN_ALT);
        re->nodes[s].a = a;
        re->nodes[s].b = b;
        a = s;
    }
    return a;
}

static inline int re_emit(Regex* re, int op) {
    if (re->ncode >= RE_CODE_MAX) { re->ok = false; return RE_CODE_MAX - 1; }
    ReInst* in = &re->code[re->ncode];
    for (size_t i = 0; i < sizeof(*in); i++) ((char*)in)[i] = 0;
    in->op = (unsigned char)op;
    return re->ncode++;
}

static inline void re_gen(Regex* re, int ni) {
    if (!re->ok) return;
    if (ni < 0 || ni >= re->nnodes) return;
    ReNode* n = &re->nodes[ni];
    switch (n->type) {
    case RN_EMPTY: break;
    case RN_CHAR: { int c = re_emit(re, RE_CHAR); re->code[c].ch = n->ch; break; }
    case RN_ANY: re_emit(re, RE_ANY); break;
    case RN_CLASS: {
        int c = re_emit(re, RE_CLASS);
        memcpy(re->code[c].cls, n->cls, 32);
        re->code[c].neg = n->neg;
        break;
    }
    case RN_BOL: re_emit(re, RE_BOL); break;
    case RN_EOL: re_emit(re, RE_EOL); break;
    case RN_BOUND: { int c = re_emit(re, RE_BOUND); re->code[c].neg = n->neg; break; }
    case RN_SEQ: {
        int a = n->a, b = n->b;
        re_gen(re, a);
        re_gen(re, b);
        break;
    }
    case RN_ALT: {
        int a = n->a, b = n->b;
        int sp = re_emit(re, RE_SPLIT);
        int left = re->ncode;
        re_gen(re, a);
        int j = re_emit(re, RE_JMP);
        int right = re->ncode;
        re_gen(re, b);
        int end = re->ncode;
        re->code[sp].x = left;
        re->code[sp].y = right;
        re->code[j].x = end;
        break;
    }
    case RN_REPEAT: {
        int child = n->a;
        int lo = n->lo < 0 ? 0 : n->lo;
        int hi = n->hi;
        for (int k = 0; k < lo && re->ok; k++) re_gen(re, child);
        if (hi < 0) {
            int sp = re_emit(re, RE_SPLIT);
            int body = re->ncode;
            re_gen(re, child);
            int j = re_emit(re, RE_JMP);
            int end = re->ncode;
            re->code[sp].x = body;
            re->code[sp].y = end;
            re->code[j].x = sp;
        } else {
            for (int k = lo; k < hi && re->ok; k++) {
                int sp = re_emit(re, RE_SPLIT);
                int body = re->ncode;
                re_gen(re, child);
                int end = re->ncode;
                re->code[sp].x = body;
                re->code[sp].y = end;
            }
        }
        break;
    }
    default: break;
    }
}

static inline bool re_compile(Regex* re, const char* pat, bool icase) {
    if (!re->code || !re->nodes) return false;
    re->ncode = 0;
    re->nnodes = 0;
    re->root = 0;
    re->ok = true;
    re->icase = icase;
    re->err[0] = 0;
    re->err_at = 0;
    re->pat = pat ? pat : "";
    re->plen = 0;
    while (re->pat[re->plen]) re->plen++;
    re->ppos = 0;
    int root = re_parse_alt(re);
    if (re->ok && re->ppos < re->plen) {
        re->ok = false;
        re->err_at = re->ppos;
        const char* e = "unexpected )";
        int i = 0; while (e[i] && i < (int)sizeof(re->err) - 1) { re->err[i] = e[i]; i++; } re->err[i] = 0;
    }
    if (!re->ok) return false;
    re_gen(re, root);
    re_emit(re, RE_MATCH);
    return re->ok;
}

static inline char re_low(char c, bool icase) {
    if (icase && c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static inline bool re_word_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static inline int re_match_at(const Regex* re, const char* s, int len, int start) {
    if (!re->ok || start < 0 || start > len) return -1;
    int sp = 0;
    int stack_pc[RE_STACK_MAX];
    int stack_pos[RE_STACK_MAX];
    int pc = 0;
    int pos = start;
    int steps = 0;
    for (;;) {
        if (++steps > RE_STEP_MAX) return -1;
        const ReInst* in = &re->code[pc];
        bool fail = false;
        switch (in->op) {
        case RE_CHAR:
            if (pos < len && re_low(s[pos], re->icase) == re_low(in->ch, re->icase)) { pos++; pc++; }
            else fail = true;
            break;
        case RE_ANY:
            if (pos < len) { pos++; pc++; } else fail = true;
            break;
        case RE_CLASS: {
            if (pos < len) {
                int c = (unsigned char)s[pos];
                bool hit = re_getbit(in->cls, c);
                if (re->icase && c >= 'A' && c <= 'Z') hit = hit || re_getbit(in->cls, c + 32);
                if (in->neg) hit = !hit;
                if (hit) { pos++; pc++; } else fail = true;
            } else fail = true;
            break;
        }
        case RE_BOL:
            if (pos == 0) pc++; else fail = true;
            break;
        case RE_EOL:
            if (pos == len) pc++; else fail = true;
            break;
        case RE_BOUND: {
            bool a = pos > 0 && re_word_byte(s[pos - 1]);
            bool b = pos < len && re_word_byte(s[pos]);
            bool boundary = (a != b);
            if (in->neg) boundary = !boundary;
            if (boundary) pc++; else fail = true;
            break;
        }
        case RE_SPLIT:
            if (sp < RE_STACK_MAX) { stack_pc[sp] = in->y; stack_pos[sp] = pos; sp++; }
            pc = in->x;
            break;
        case RE_JMP:
            pc = in->x;
            break;
        case RE_MATCH:
            return pos;
        default:
            fail = true;
            break;
        }
        if (fail) {
            if (sp <= 0) return -1;
            sp--;
            pc = stack_pc[sp];
            pos = stack_pos[sp];
        }
    }
}

static inline bool re_find(const Regex* re, const char* s, int len, int from, int* a, int* b) {
    if (!re->ok) return false;
    if (from < 0) from = 0;
    for (int st = from; st <= len; st++) {
        int e = re_match_at(re, s, len, st);
        if (e >= 0) { *a = st; *b = e; return true; }
    }
    return false;
}

static inline bool re_init(Regex* re) {
    memset(re, 0, sizeof(*re));
    re->code = (ReInst*)edx_alloc(sizeof(ReInst) * RE_CODE_MAX);
    re->nodes = (ReNode*)edx_alloc(sizeof(ReNode) * RE_NODE_MAX);
    re->ok = false;
    return re->code && re->nodes;
}
static inline void re_free(Regex* re) {
    edx_free(re->code);
    edx_free(re->nodes);
    memset(re, 0, sizeof(*re));
}

static inline int lit_find(const char* hay, int n, const char* pat, int pn, bool icase, int from) {
    if (pn <= 0) return from <= n ? from : -1;
    if (from < 0) from = 0;
    for (int i = from; i + pn <= n; i++) {
        int k = 0;
        while (k < pn) {
            char a = hay[i + k];
            char b = pat[k];
            if (icase) { a = re_low(a, true); b = re_low(b, true); }
            if (a != b) break;
            k++;
        }
        if (k == pn) return i;
    }
    return -1;
}

}  // namespace ed

// end (satoru)
