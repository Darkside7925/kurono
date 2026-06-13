//  kurono os  -  kcl (kurono command language) interpreter
//
//  a complete tree-walking scripting language. pipeline:
//    source text -> lexer (token stream w/ line numbers)
//                -> recursive-descent parser/evaluator over the token stream
//  values are a tagged union (int / float / string / bool / list / none),
//  modelled on the in-kernel python_interp value system (owned heap strings,
//  dynamic lists, real doubles). everything is freestanding: no libc, no stl,
//  no exceptions. script errors are captured into an output buffer and never
//  crash the os. (satoru)

#include "kcl.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"

// ────────────────────────────────────────────────────────────────────────
//  static state
// ────────────────────────────────────────────────────────────────────────
KuronoShell* KCL::shell = nullptr;

namespace {

// ── tiny string helpers (no libc) ───────────────────────────────────────
static int kstrlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static bool kstreq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void kstrcpy(char* d, const char* s, int max) {
    int i = 0; while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static bool kis_digit(char c) { return c >= '0' && c <= '9'; }
static bool kis_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool kis_alnum(char c) { return kis_alpha(c) || kis_digit(c); }
static bool kends_with(const char* str, const char* suffix) {
    int sl = kstrlen(str), tl = kstrlen(suffix);
    if (tl == 0 || sl < tl) return false;
    return kstreq(str + sl - tl, suffix);
}

// ── output sink (bounded, overflow-safe) ────────────────────────────────
struct Sink { char* buf; int cap; int len; bool overflow; };
static void sw(Sink& s, const char* str) {
    if (!str) return;
    while (*str) {
        if (s.len < s.cap - 1) s.buf[s.len++] = *str;
        else s.overflow = true;
        str++;
    }
    s.buf[s.len] = 0;
}
static void swc(Sink& s, char c) {
    if (s.len < s.cap - 1) s.buf[s.len++] = c; else s.overflow = true;
    s.buf[s.len] = 0;
}
static void swi(Sink& s, long long v) {
    if (v < 0) { swc(s, '-'); v = -v; }
    char t[24]; int ti = 0;
    if (v == 0) t[ti++] = '0';
    while (v > 0) { t[ti++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (ti > 0) swc(s, t[--ti]);
}
// float -> text with 6 fractional digits (matches python_interp swd). (satoru)
static void swd(Sink& s, double d) {
    if (d != d) { sw(s, "nan"); return; }
    if (d < 0) { swc(s, '-'); d = -d; }
    long long ip = (long long)d;
    swi(s, ip);
    swc(s, '.');
    double frac = d - (double)ip;
    for (int i = 0; i < 6; i++) {
        frac *= 10.0;
        int dig = (int)frac;
        if (dig < 0) dig = 0;
        if (dig > 9) dig = 9;
        swc(s, (char)('0' + dig));
        frac -= (double)dig;
    }
}

// ── value type ──────────────────────────────────────────────────────────
enum VType { V_NONE = 0, V_INT, V_FLOAT, V_BOOL, V_STR, V_LIST };

struct Value {
    int type;
    long long i;
    double f;
    char* s;        // owned for V_STR (satoru)
    int slen;
    Value* items;   // owned for V_LIST (satoru)
    int item_count;
    int item_cap;
};

static void vinit(Value& v) {
    v.type = V_NONE; v.i = 0; v.f = 0.0;
    v.s = nullptr; v.slen = 0;
    v.items = nullptr; v.item_count = 0; v.item_cap = 0;
}

static void vfree(Value& v);

static Value* alloc_items(int cap) {
    Value* a = (Value*)KernelHeap::Alloc((unsigned)(sizeof(Value) * cap));
    if (!a) return nullptr;
    for (int i = 0; i < cap; i++) vinit(a[i]);
    return a;
}

static void vfree(Value& v) {
    if (v.type == V_STR && v.s) { KernelHeap::Free(v.s); v.s = nullptr; v.slen = 0; }
    if (v.type == V_LIST && v.items) {
        for (int i = 0; i < v.item_count; i++) vfree(v.items[i]);
        KernelHeap::Free(v.items);
        v.items = nullptr; v.item_count = 0; v.item_cap = 0;
    }
    v.type = V_NONE;
}

static void vset_int(Value& v, long long n) { vfree(v); v.type = V_INT; v.i = n; }
static void vset_float(Value& v, double d)  { vfree(v); v.type = V_FLOAT; v.f = d; }
static void vset_bool(Value& v, bool b)     { vfree(v); v.type = V_BOOL; v.i = b ? 1 : 0; }

static void vset_str(Value& v, const char* src, int n) {
    vfree(v);
    if (n < 0) n = kstrlen(src);
    char* buf = (char*)KernelHeap::Alloc((unsigned)(n + 1));
    if (!buf) { v.type = V_NONE; return; }
    for (int i = 0; i < n; i++) buf[i] = src[i];
    buf[n] = 0;
    v.type = V_STR; v.s = buf; v.slen = n;
}

static void vcopy(Value& dst, const Value& src) {
    if (&dst == &src) return;
    vfree(dst);
    dst.type = src.type;
    dst.i = src.i; dst.f = src.f;
    if (src.type == V_STR) vset_str(dst, src.s ? src.s : "", src.slen);
    else if (src.type == V_LIST) {
        dst.item_cap = src.item_count > 0 ? src.item_count : 1;
        dst.items = alloc_items(dst.item_cap);
        if (!dst.items) { dst.type = V_NONE; return; }
        dst.item_count = src.item_count;
        for (int i = 0; i < src.item_count; i++) vcopy(dst.items[i], src.items[i]);
    }
}

static void list_push(Value& list, const Value& v) {
    if (list.type != V_LIST) {
        vfree(list);
        list.type = V_LIST; list.item_cap = 4;
        list.items = alloc_items(list.item_cap);
        list.item_count = 0;
    }
    if (!list.items) return;
    if (list.item_count >= list.item_cap) {
        int nc = list.item_cap * 2;
        Value* na = alloc_items(nc);
        if (!na) return;
        for (int i = 0; i < list.item_count; i++) {
            vcopy(na[i], list.items[i]);
            vfree(list.items[i]);
        }
        KernelHeap::Free(list.items);
        list.items = na; list.item_cap = nc;
    }
    vcopy(list.items[list.item_count++], v);
}

static double vnumf(const Value& v) {
    if (v.type == V_INT)   return (double)v.i;
    if (v.type == V_FLOAT) return v.f;
    if (v.type == V_BOOL)  return v.i ? 1.0 : 0.0;
    return 0.0;
}
static long long vnumi(const Value& v) {
    if (v.type == V_INT)   return v.i;
    if (v.type == V_FLOAT) return (long long)v.f;
    if (v.type == V_BOOL)  return v.i ? 1 : 0;
    return 0;
}
static bool vtruthy(const Value& v) {
    if (v.type == V_NONE)  return false;
    if (v.type == V_BOOL)  return v.i != 0;
    if (v.type == V_INT)   return v.i != 0;
    if (v.type == V_FLOAT) return v.f != 0.0;
    if (v.type == V_STR)   return v.slen > 0;
    if (v.type == V_LIST)  return v.item_count > 0;
    return true;
}

// render a value into a sink (program output / str() / list contents). (satoru)
static void vprint(Sink& s, const Value& v) {
    switch (v.type) {
        case V_NONE:  sw(s, "none"); break;
        case V_INT:   swi(s, v.i); break;
        case V_FLOAT: swd(s, v.f); break;
        case V_BOOL:  sw(s, v.i ? "true" : "false"); break;
        case V_STR:   if (v.s) sw(s, v.s); break;
        case V_LIST: {
            swc(s, '[');
            for (int i = 0; i < v.item_count; i++) {
                if (i) sw(s, ", ");
                if (v.items[i].type == V_STR) {
                    swc(s, '"'); if (v.items[i].s) sw(s, v.items[i].s); swc(s, '"');
                } else vprint(s, v.items[i]);
            }
            swc(s, ']');
            break;
        }
    }
}

// render a value to a freshly heap-allocated c string (caller frees). used for
// string concatenation / str(). (satoru)
static char* vto_cstr(const Value& v) {
    char tmp[1024];
    Sink s; s.buf = tmp; s.cap = (int)sizeof(tmp); s.len = 0; s.overflow = false;
    vprint(s, v);
    int n = s.len;
    char* out = (char*)KernelHeap::Alloc((unsigned)(n + 1));
    if (!out) return nullptr;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    out[n] = 0;
    return out;
}

// ── token stream (heap-owned, freed after Execute) ──────────────────────
struct Lexer {
    KCLToken* toks;
    int count;
    bool ok;
};

static KCLTok keyword_of(const char* w) {
    if (kstreq(w, "set") || kstreq(w, "let"))      return KT_SET;
    if (kstreq(w, "print") || kstreq(w, "println")) return KT_PRINT;
    if (kstreq(w, "if"))       return KT_IF;
    if (kstreq(w, "then"))     return KT_THEN;
    if (kstreq(w, "else"))     return KT_ELSE;
    if (kstreq(w, "elif"))     return KT_ELIF;
    if (kstreq(w, "end"))      return KT_END;
    if (kstreq(w, "while"))    return KT_WHILE;
    if (kstreq(w, "for"))      return KT_FOR;
    if (kstreq(w, "in"))       return KT_IN;
    if (kstreq(w, "do"))       return KT_DO;
    if (kstreq(w, "func") || kstreq(w, "function")) return KT_FUNC;
    if (kstreq(w, "return"))   return KT_RETURN;
    if (kstreq(w, "import"))   return KT_IMPORT;
    if (kstreq(w, "break"))    return KT_BREAK;
    if (kstreq(w, "continue")) return KT_CONTINUE;
    if (kstreq(w, "true"))     return KT_TRUE;
    if (kstreq(w, "false"))    return KT_FALSE;
    if (kstreq(w, "none") || kstreq(w, "null")) return KT_NONE;
    if (kstreq(w, "and"))      return KT_AND;
    if (kstreq(w, "or"))       return KT_OR;
    if (kstreq(w, "not"))      return KT_NOT;
    return KT_IDENT;
}

static void lex_free(Lexer& lx) {
    if (!lx.toks) return;
    for (int i = 0; i < lx.count; i++)
        if (lx.toks[i].type == KT_STRING && lx.toks[i].str) KernelHeap::Free(lx.toks[i].str);
    KernelHeap::Free(lx.toks);
    lx.toks = nullptr; lx.count = 0;
}

static void tok_simple(KCLToken& t, KCLTok type, int line) {
    t.type = type; t.line = line; t.ival = 0; t.fval = 0.0; t.text[0] = 0; t.str = nullptr;
}

// tokenise the whole source. comments (#) drop to end of line. newlines become
// explicit KT_NEWLINE tokens (statement separators). shebang `#!...` on line 1
// is skipped like any comment. (satoru)
static void lex(const char* src, Lexer& lx) {
    lx.toks = (KCLToken*)KernelHeap::Alloc(sizeof(KCLToken) * KCL_MAX_TOKENS);
    lx.count = 0; lx.ok = true;
    if (!lx.toks) { lx.ok = false; return; }

    int i = 0, line = 1;
    while (src[i] && lx.count < KCL_MAX_TOKENS - 1) {
        char c = src[i];

        if (c == '\n') {
            // collapse runs of blank lines but keep one separator. (satoru)
            if (lx.count > 0 && lx.toks[lx.count - 1].type != KT_NEWLINE)
                tok_simple(lx.toks[lx.count++], KT_NEWLINE, line);
            line++; i++; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#') { while (src[i] && src[i] != '\n') i++; continue; }

        // numbers (int or float) (satoru)
        if (kis_digit(c) || (c == '.' && kis_digit(src[i + 1]))) {
            long long iv = 0; bool isf = false; double fv = 0; int startline = line;
            while (kis_digit(src[i])) { iv = iv * 10 + (src[i] - '0'); i++; }
            // disambiguate `.` (decimal point) from `..` (range). (satoru)
            if (src[i] == '.' && src[i + 1] != '.') {
                isf = true; fv = (double)iv; i++;
                double scale = 0.1;
                while (kis_digit(src[i])) { fv += (double)(src[i] - '0') * scale; scale *= 0.1; i++; }
            }
            KCLToken& t = lx.toks[lx.count++];
            tok_simple(t, isf ? KT_FLOAT : KT_NUMBER, startline);
            t.ival = iv; t.fval = fv;
            continue;
        }

        // strings (heap-owned, supports escapes) (satoru)
        if (c == '"' || c == '\'') {
            char q = c; i++; int startline = line;
            // gather into a growable temp first to size the heap copy. (satoru)
            int cap = 32, n = 0;
            char* buf = (char*)KernelHeap::Alloc((unsigned)cap);
            if (!buf) { lx.ok = false; return; }
            while (src[i] && src[i] != q) {
                char ch = src[i++];
                if (ch == '\\' && src[i]) {
                    char e = src[i++];
                    if (e == 'n') ch = '\n';
                    else if (e == 't') ch = '\t';
                    else if (e == 'r') ch = '\r';
                    else if (e == '0') ch = 0;
                    else ch = e;
                } else if (ch == '\n') { line++; }
                if (n + 1 >= cap) {
                    int nc = cap * 2;
                    char* nb = (char*)KernelHeap::Alloc((unsigned)nc);
                    if (!nb) { KernelHeap::Free(buf); lx.ok = false; return; }
                    for (int k = 0; k < n; k++) nb[k] = buf[k];
                    KernelHeap::Free(buf); buf = nb; cap = nc;
                }
                buf[n++] = ch;
            }
            buf[n] = 0;
            if (src[i] == q) i++;
            KCLToken& t = lx.toks[lx.count++];
            tok_simple(t, KT_STRING, startline);
            t.str = buf; t.ival = n;
            continue;
        }

        // identifiers / keywords (satoru)
        if (kis_alpha(c)) {
            int startline = line; int o = 0;
            KCLToken& t = lx.toks[lx.count];
            tok_simple(t, KT_IDENT, startline);
            while (kis_alnum(src[i]) && o < KCL_MAX_NAME - 1) t.text[o++] = src[i++];
            t.text[o] = 0;
            t.type = keyword_of(t.text);
            lx.count++;
            continue;
        }

        // operators / punctuation (satoru)
        int startline = line; i++;
        KCLToken& t = lx.toks[lx.count];
        switch (c) {
            case '(': tok_simple(t, KT_LPAREN, startline); break;
            case ')': tok_simple(t, KT_RPAREN, startline); break;
            case '[': tok_simple(t, KT_LBRACKET, startline); break;
            case ']': tok_simple(t, KT_RBRACKET, startline); break;
            case ',': tok_simple(t, KT_COMMA, startline); break;
            case ';': tok_simple(t, KT_SEMI, startline); break;
            case '+': tok_simple(t, KT_PLUS, startline); break;
            case '-': tok_simple(t, KT_MINUS, startline); break;
            case '*': tok_simple(t, KT_STAR, startline); break;
            case '/': tok_simple(t, KT_SLASH, startline); break;
            case '%': tok_simple(t, KT_PERCENT, startline); break;
            case '.':
                if (src[i] == '.') { tok_simple(t, KT_DOTDOT, startline); i++; }
                else tok_simple(t, KT_DOT, startline);
                break;
            case '=':
                if (src[i] == '=') { tok_simple(t, KT_EQ, startline); i++; }
                else tok_simple(t, KT_ASSIGN, startline);
                break;
            case '!':
                if (src[i] == '=') { tok_simple(t, KT_NEQ, startline); i++; }
                else tok_simple(t, KT_NOT, startline);
                break;
            case '<':
                if (src[i] == '=') { tok_simple(t, KT_LTE, startline); i++; }
                else tok_simple(t, KT_LT, startline);
                break;
            case '>':
                if (src[i] == '=') { tok_simple(t, KT_GTE, startline); i++; }
                else tok_simple(t, KT_GT, startline);
                break;
            default:
                // unknown char: skip silently (treated as separator). (satoru)
                continue;
        }
        lx.count++;
    }
    tok_simple(lx.toks[lx.count], KT_EOF, line);
    // count excludes the eof sentinel (it's at index lx.count). (satoru)
}

// ── environment: variable + function bindings, lexically scoped ──────────
struct FuncDef {
    char params[KCL_MAX_PARAMS][KCL_MAX_NAME];
    int  param_count;
    int  body_start;   // token index of first body token (satoru)
    // a func body is addressed by index INTO ITS OWN token stream. imports use
    // separate streams, so we remember which stream this body lives in and swap
    // the interpreter onto it for the duration of the call. (satoru)
    KCLToken* toks;
    int       count;
};

struct Binding {
    char name[KCL_MAX_NAME];
    bool used;
    bool is_func;
    Value val;       // for variables (satoru)
    FuncDef fn;      // for functions (satoru)
};

struct Env {
    Binding slots[KCL_ENV_SLOTS];
    Env* parent;
    bool overflow;   // ran out of binding slots (satoru)
};

static void env_init(Env& e, Env* parent) {
    for (int i = 0; i < KCL_ENV_SLOTS; i++) {
        e.slots[i].used = false; e.slots[i].is_func = false;
        e.slots[i].name[0] = 0; vinit(e.slots[i].val);
    }
    e.parent = parent; e.overflow = false;
}
static void env_clear(Env& e) {
    for (int i = 0; i < KCL_ENV_SLOTS; i++)
        if (e.slots[i].used) { vfree(e.slots[i].val); e.slots[i].used = false; e.slots[i].name[0] = 0; }
}
static Binding* env_find_local(Env& e, const char* name) {
    for (int i = 0; i < KCL_ENV_SLOTS; i++)
        if (e.slots[i].used && kstreq(e.slots[i].name, name)) return &e.slots[i];
    return nullptr;
}
static Binding* env_find(Env* e, const char* name) {
    while (e) { Binding* b = env_find_local(*e, name); if (b) return b; e = e->parent; }
    return nullptr;
}
// assign a variable in the nearest scope that already has it, else define it
// in the current (top-most) scope. (satoru)
static void env_set(Env& e, const char* name, const Value& v) {
    for (Env* p = &e; p; p = p->parent) {
        Binding* b = env_find_local(*p, name);
        if (b && !b->is_func) { vcopy(b->val, v); return; }
    }
    for (int i = 0; i < KCL_ENV_SLOTS; i++) {
        if (!e.slots[i].used) {
            e.slots[i].used = true; e.slots[i].is_func = false;
            kstrcpy(e.slots[i].name, name, KCL_MAX_NAME);
            vinit(e.slots[i].val); vcopy(e.slots[i].val, v);
            return;
        }
    }
    e.overflow = true;
}
static void env_set_local(Env& e, const char* name, const Value& v) {
    Binding* b = env_find_local(e, name);
    if (b && !b->is_func) { vcopy(b->val, v); return; }
    for (int i = 0; i < KCL_ENV_SLOTS; i++) {
        if (!e.slots[i].used) {
            e.slots[i].used = true; e.slots[i].is_func = false;
            kstrcpy(e.slots[i].name, name, KCL_MAX_NAME);
            vinit(e.slots[i].val); vcopy(e.slots[i].val, v);
            return;
        }
    }
    e.overflow = true;
}

// retained import: an imported file's lexer + source buffer must outlive the
// import statement, because functions it defines reference its token stream by
// index. they're chained here and all freed together when the run ends. (satoru)
struct RetainedLex;

// ── interpreter context threaded through the whole walk ─────────────────
struct Interp {
    KCLToken* toks;
    int count;
    Sink* out;        // program output (print, etc.) (satoru)
    Sink* err;        // error messages (with line numbers) (satoru)
    Env* globals;     // funcs live here so they're visible everywhere (satoru)
    bool failed;      // hard error  -  unwind everything (satoru)
    bool ret_flag;    // a return fired (satoru)
    Value ret_val;
    bool break_flag;
    bool continue_flag;
    int recursion;
    int import_depth;
    RetainedLex** retained;  // head of the retained-import list (shared) (satoru)
};

struct RetainedLex {
    KCLToken* toks;
    int count;
    char* src;
    RetainedLex* next;
};

static void raise_err(Interp& it, int line, const char* msg) {
    if (it.failed) return;
    sw(*it.err, "kcl: line ");
    swi(*it.err, line);
    sw(*it.err, ": ");
    sw(*it.err, msg);
    swc(*it.err, '\n');
    it.failed = true;
}
static void raise_err2(Interp& it, int line, const char* msg, const char* extra) {
    if (it.failed) return;
    sw(*it.err, "kcl: line ");
    swi(*it.err, line);
    sw(*it.err, ": ");
    sw(*it.err, msg);
    sw(*it.err, extra);
    swc(*it.err, '\n');
    it.failed = true;
}

// should we stop walking statements right now? (satoru)
static bool unwinding(Interp& it) {
    return it.failed || it.ret_flag || it.break_flag || it.continue_flag;
}

// forward decls (satoru)
static Value eval_expr(Interp& it, Env& env, int& pos);
static Value eval_or(Interp& it, Env& env, int& pos);
static void exec_block(Interp& it, Env& env, int& pos);
static void skip_block(Interp& it, int& pos);
static Value call_func(Interp& it, const char* name, FuncDef& fn, Value* args, int argc, int line);
static Value call_builtin(Interp& it, Env& env, const char* name, Value* args, int argc, bool& handled, int line);

// ── expression parser/evaluator (precedence climbing) ───────────────────
// grammar (low -> high precedence):
//   or  := and  ( 'or'  and )*
//   and := cmp  ( 'and' cmp )*
//   cmp := add  ( (==|!=|<|>|<=|>=) add )*
//   add := mul  ( (+|-) mul )*
//   mul := unary ( (*|/|%) unary )*
//   unary := ('-'|'not'|'+')* postfix
//   postfix := atom ( '[' expr ']' )*
//   atom := number | float | string | true | false | none | list
//         | ident | ident '(' args ')' | '(' expr ')'

static KCLTok cur(Interp& it, int pos) {
    return (pos >= 0 && pos <= it.count) ? it.toks[pos].type : KT_EOF;
}
static int cur_line(Interp& it, int pos) {
    return (pos >= 0 && pos <= it.count) ? it.toks[pos].line : 0;
}

static Value eval_atom(Interp& it, Env& env, int& pos) {
    Value v; vinit(v);
    if (it.failed) return v;
    KCLToken& t = it.toks[pos];

    switch (t.type) {
        case KT_NUMBER: pos++; v.type = V_INT; v.i = t.ival; return v;
        case KT_FLOAT:  pos++; v.type = V_FLOAT; v.f = t.fval; return v;
        case KT_TRUE:   pos++; vset_bool(v, true); return v;
        case KT_FALSE:  pos++; vset_bool(v, false); return v;
        case KT_NONE:   pos++; v.type = V_NONE; return v;
        case KT_STRING: pos++; vset_str(v, t.str ? t.str : "", (int)t.ival); return v;
        case KT_LPAREN: {
            pos++;
            Value inner = eval_expr(it, env, pos);
            if (cur(it, pos) == KT_RPAREN) pos++;
            else raise_err(it, cur_line(it, pos), "expected ')'");
            return inner;
        }
        case KT_LBRACKET: {
            // list literal (satoru)
            pos++;
            v.type = V_LIST; v.item_cap = 4; v.items = alloc_items(v.item_cap); v.item_count = 0;
            if (cur(it, pos) == KT_RBRACKET) { pos++; return v; }
            while (!it.failed) {
                Value e = eval_expr(it, env, pos);
                list_push(v, e); vfree(e);
                if (cur(it, pos) == KT_COMMA) { pos++; continue; }
                if (cur(it, pos) == KT_RBRACKET) { pos++; break; }
                raise_err(it, cur_line(it, pos), "expected ',' or ']' in list"); break;
            }
            return v;
        }
        case KT_IDENT: {
            char name[KCL_MAX_NAME]; kstrcpy(name, t.text, KCL_MAX_NAME);
            int line = t.line;
            pos++;
            if (cur(it, pos) == KT_LPAREN) {
                // function/builtin call (satoru)
                pos++;
                Value args[KCL_MAX_PARAMS]; for (int k = 0; k < KCL_MAX_PARAMS; k++) vinit(args[k]);
                int argc = 0;
                if (cur(it, pos) != KT_RPAREN) {
                    while (!it.failed) {
                        Value a = eval_expr(it, env, pos);
                        if (argc < KCL_MAX_PARAMS) vcopy(args[argc], a);
                        vfree(a); argc++;
                        if (cur(it, pos) == KT_COMMA) { pos++; continue; }
                        break;
                    }
                }
                if (cur(it, pos) == KT_RPAREN) pos++;
                else raise_err(it, cur_line(it, pos), "expected ')' after arguments");
                int effc = argc < KCL_MAX_PARAMS ? argc : KCL_MAX_PARAMS;

                Value result; vinit(result);
                if (!it.failed) {
                    bool handled = false;
                    result = call_builtin(it, env, name, args, effc, handled, line);
                    if (!handled) {
                        Binding* b = env_find(&env, name);
                        if (!b) b = env_find(it.globals, name);
                        if (b && b->is_func) result = call_func(it, name, b->fn, args, effc, line);
                        else raise_err2(it, line, "undefined function: ", name);
                    }
                }
                for (int k = 0; k < KCL_MAX_PARAMS; k++) vfree(args[k]);
                return result;
            }
            // variable reference (satoru)
            Binding* b = env_find(&env, name);
            if (b && !b->is_func) { vcopy(v, b->val); return v; }
            raise_err2(it, line, "undefined variable: ", name);
            return v;
        }
        default:
            raise_err(it, cur_line(it, pos), "unexpected token in expression");
            pos++;
            return v;
    }
}

// index a string/list with [n] (negative wraps from end). (satoru)
static Value index_value(Interp& it, const Value& base, const Value& idx, int line) {
    Value out; vinit(out);
    long long n = vnumi(idx);
    if (base.type == V_LIST) {
        if (n < 0) n += base.item_count;
        if (n < 0 || n >= base.item_count) { raise_err(it, line, "list index out of range"); return out; }
        vcopy(out, base.items[(int)n]);
        return out;
    }
    if (base.type == V_STR) {
        if (n < 0) n += base.slen;
        if (n < 0 || n >= base.slen) { raise_err(it, line, "string index out of range"); return out; }
        char ch[2] = { base.s[(int)n], 0 };
        vset_str(out, ch, 1);
        return out;
    }
    raise_err(it, line, "value is not indexable");
    return out;
}

static Value eval_postfix(Interp& it, Env& env, int& pos) {
    Value v = eval_atom(it, env, pos);
    while (cur(it, pos) == KT_LBRACKET && !it.failed) {
        int line = cur_line(it, pos);
        pos++;
        Value idx = eval_expr(it, env, pos);
        if (cur(it, pos) == KT_RBRACKET) pos++;
        else raise_err(it, cur_line(it, pos), "expected ']'");
        Value out = index_value(it, v, idx, line);
        vfree(idx); vfree(v); v = out;
    }
    return v;
}

static Value eval_unary(Interp& it, Env& env, int& pos) {
    if (cur(it, pos) == KT_MINUS) {
        pos++;
        Value r = eval_unary(it, env, pos);
        Value out; vinit(out);
        if (r.type == V_FLOAT) vset_float(out, -r.f);
        else vset_int(out, -vnumi(r));
        vfree(r); return out;
    }
    if (cur(it, pos) == KT_NOT) {
        pos++;
        Value r = eval_unary(it, env, pos);
        Value out; vinit(out); vset_bool(out, !vtruthy(r));
        vfree(r); return out;
    }
    if (cur(it, pos) == KT_PLUS) { pos++; return eval_unary(it, env, pos); }
    return eval_postfix(it, env, pos);
}

static bool numeric_value(const Value& x) { return x.type == V_INT || x.type == V_FLOAT || x.type == V_BOOL; }
static bool both_numeric(const Value& a, const Value& b) { return numeric_value(a) && numeric_value(b); }

static Value eval_mul(Interp& it, Env& env, int& pos) {
    Value left = eval_unary(it, env, pos);
    while (!it.failed) {
        KCLTok op = cur(it, pos);
        if (op != KT_STAR && op != KT_SLASH && op != KT_PERCENT) break;
        int line = cur_line(it, pos); pos++;
        Value right = eval_unary(it, env, pos);
        Value out; vinit(out);
        bool isf = (left.type == V_FLOAT) || (right.type == V_FLOAT);
        if (op == KT_STAR) {
            // string * int repeats. (satoru)
            if (left.type == V_STR && right.type == V_INT) {
                int reps = (int)right.i; if (reps < 0) reps = 0;
                int per = left.slen; int total = per * reps;
                char* buf = (char*)KernelHeap::Alloc((unsigned)(total + 1));
                if (buf) { int o = 0; for (int r = 0; r < reps; r++) for (int k = 0; k < per; k++) buf[o++] = left.s[k]; buf[o] = 0; vfree(out); out.type = V_STR; out.s = buf; out.slen = total; }
            } else if (isf) vset_float(out, vnumf(left) * vnumf(right));
            else vset_int(out, vnumi(left) * vnumi(right));
        } else if (op == KT_SLASH) {
            double rd = vnumf(right);
            if (rd == 0.0) { raise_err(it, line, "division by zero"); }
            else if (isf) vset_float(out, vnumf(left) / rd);
            else vset_int(out, vnumi(left) / vnumi(right));
        } else { // mod
            long long rr = vnumi(right);
            if (rr == 0) raise_err(it, line, "modulo by zero");
            else vset_int(out, vnumi(left) % rr);
        }
        vfree(left); vfree(right); left = out;
    }
    return left;
}

static Value eval_add(Interp& it, Env& env, int& pos) {
    Value left = eval_mul(it, env, pos);
    while (!it.failed) {
        KCLTok op = cur(it, pos);
        if (op != KT_PLUS && op != KT_MINUS) break;
        int line = cur_line(it, pos); pos++;
        Value right = eval_mul(it, env, pos);
        Value out; vinit(out);
        if (op == KT_PLUS) {
            // string concat: either side a string. list concat: both lists. (satoru)
            if (left.type == V_STR || right.type == V_STR) {
                char* ls = vto_cstr(left); char* rs = vto_cstr(right);
                int ln = kstrlen(ls), rn = kstrlen(rs);
                char* buf = (char*)KernelHeap::Alloc((unsigned)(ln + rn + 1));
                if (buf) { int o = 0; for (int k = 0; ls && ls[k]; k++) buf[o++] = ls[k]; for (int k = 0; rs && rs[k]; k++) buf[o++] = rs[k]; buf[o] = 0; vfree(out); out.type = V_STR; out.s = buf; out.slen = ln + rn; }
                if (ls) KernelHeap::Free(ls);
                if (rs) KernelHeap::Free(rs);
            } else if (left.type == V_LIST && right.type == V_LIST) {
                vcopy(out, left);
                for (int k = 0; k < right.item_count; k++) list_push(out, right.items[k]);
            } else if (both_numeric(left, right)) {
                if (left.type == V_FLOAT || right.type == V_FLOAT) vset_float(out, vnumf(left) + vnumf(right));
                else vset_int(out, vnumi(left) + vnumi(right));
            } else raise_err(it, line, "unsupported operands for '+'");
        } else { // minus
            if (both_numeric(left, right)) {
                if (left.type == V_FLOAT || right.type == V_FLOAT) vset_float(out, vnumf(left) - vnumf(right));
                else vset_int(out, vnumi(left) - vnumi(right));
            } else raise_err(it, line, "unsupported operands for '-'");
        }
        vfree(left); vfree(right); left = out;
    }
    return left;
}

// value equality across types (satoru)
static bool value_eq(const Value& a, const Value& b) {
    if (a.type == V_STR && b.type == V_STR) return kstreq(a.s ? a.s : "", b.s ? b.s : "");
    if (a.type == V_STR || b.type == V_STR) return false;
    if (a.type == V_NONE || b.type == V_NONE) return a.type == b.type;
    if (a.type == V_LIST && b.type == V_LIST) {
        if (a.item_count != b.item_count) return false;
        for (int k = 0; k < a.item_count; k++) if (!value_eq(a.items[k], b.items[k])) return false;
        return true;
    }
    if (a.type == V_LIST || b.type == V_LIST) return false;
    return vnumf(a) == vnumf(b);
}
// ordered comparison; returns -1/0/1, sets ok=false if uncomparable. (satoru)
static int value_cmp(const Value& a, const Value& b, bool& ok) {
    ok = true;
    if (a.type == V_STR && b.type == V_STR) {
        const char* x = a.s ? a.s : ""; const char* y = b.s ? b.s : "";
        while (*x && *y && *x == *y) { x++; y++; }
        if ((unsigned char)*x < (unsigned char)*y) return -1;
        if ((unsigned char)*x > (unsigned char)*y) return 1;
        return 0;
    }
    if (both_numeric(a, b)) {
        double x = vnumf(a), y = vnumf(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    ok = false; return 0;
}

static Value eval_cmp(Interp& it, Env& env, int& pos) {
    Value left = eval_add(it, env, pos);
    while (!it.failed) {
        KCLTok op = cur(it, pos);
        if (op != KT_EQ && op != KT_NEQ && op != KT_LT && op != KT_GT && op != KT_LTE && op != KT_GTE) break;
        int line = cur_line(it, pos); pos++;
        Value right = eval_add(it, env, pos);
        Value out; vinit(out);
        bool res = false;
        if (op == KT_EQ) res = value_eq(left, right);
        else if (op == KT_NEQ) res = !value_eq(left, right);
        else {
            bool ok = true; int c = value_cmp(left, right, ok);
            if (!ok) raise_err(it, line, "values are not comparable");
            else if (op == KT_LT) res = c < 0;
            else if (op == KT_GT) res = c > 0;
            else if (op == KT_LTE) res = c <= 0;
            else if (op == KT_GTE) res = c >= 0;
        }
        vset_bool(out, res);
        vfree(left); vfree(right); left = out;
    }
    return left;
}

static Value eval_and(Interp& it, Env& env, int& pos) {
    Value left = eval_cmp(it, env, pos);
    while (cur(it, pos) == KT_AND && !it.failed) {
        pos++;
        bool lt = vtruthy(left);
        Value right = eval_cmp(it, env, pos);
        Value out; vinit(out); vset_bool(out, lt && vtruthy(right));
        vfree(left); vfree(right); left = out;
    }
    return left;
}

static Value eval_or(Interp& it, Env& env, int& pos) {
    Value left = eval_and(it, env, pos);
    while (cur(it, pos) == KT_OR && !it.failed) {
        pos++;
        bool lt = vtruthy(left);
        Value right = eval_and(it, env, pos);
        Value out; vinit(out); vset_bool(out, lt || vtruthy(right));
        vfree(left); vfree(right); left = out;
    }
    return left;
}

static Value eval_expr(Interp& it, Env& env, int& pos) {
    return eval_or(it, env, pos);
}

// ── block skipping (used for not-taken branches and func bodies) ─────────
// advances pos to the matching 'end' (and optionally 'else'/'elif' at depth 0).
// depth tracks nested block openers (if/while/for/func). stops *before* the
// terminator so the caller decides what to do with it. (satoru)
static void skip_to_terminators(Interp& it, int& pos, bool stop_on_else) {
    int depth = 0;
    while (cur(it, pos) != KT_EOF) {
        KCLTok t = cur(it, pos);
        if (t == KT_IF || t == KT_WHILE || t == KT_FOR || t == KT_FUNC) { depth++; pos++; continue; }
        if (t == KT_END) {
            if (depth == 0) return;
            depth--; pos++; continue;
        }
        if (depth == 0 && stop_on_else && (t == KT_ELSE || t == KT_ELIF)) return;
        pos++;
    }
}
// skip an entire block including its 'end'. (satoru)
static void skip_block(Interp& it, int& pos) {
    skip_to_terminators(it, pos, false);
    if (cur(it, pos) == KT_END) pos++;
}

// skip leading statement separators. (satoru)
static void skip_seps(Interp& it, int& pos) {
    while (cur(it, pos) == KT_NEWLINE || cur(it, pos) == KT_SEMI) pos++;
}

// ── statement executors ──────────────────────────────────────────────────
static void exec_stmt(Interp& it, Env& env, int& pos);

// run statements until a block terminator (end/else/elif/eof). (satoru)
static void exec_block(Interp& it, Env& env, int& pos) {
    while (!unwinding(it)) {
        skip_seps(it, pos);
        KCLTok t = cur(it, pos);
        if (t == KT_EOF || t == KT_END || t == KT_ELSE || t == KT_ELIF) break;
        exec_stmt(it, env, pos);
    }
}

static void exec_set(Interp& it, Env& env, int& pos) {
    int line = cur_line(it, pos);
    pos++; // 'set'
    if (cur(it, pos) != KT_IDENT) { raise_err(it, line, "expected variable name after 'set'"); return; }
    char name[KCL_MAX_NAME]; kstrcpy(name, it.toks[pos].text, KCL_MAX_NAME);
    pos++;
    if (cur(it, pos) == KT_ASSIGN) pos++;
    else { raise_err(it, line, "expected '=' in 'set'"); return; }
    Value v = eval_expr(it, env, pos);
    if (!it.failed) env_set(env, name, v);
    vfree(v);
}

static void exec_print(Interp& it, Env& env, int& pos) {
    pos++; // 'print'
    bool open_paren = (cur(it, pos) == KT_LPAREN);
    if (open_paren) pos++;
    bool first = true;
    while (!it.failed) {
        KCLTok t = cur(it, pos);
        if (t == KT_NEWLINE || t == KT_SEMI || t == KT_EOF) break;
        if (open_paren && t == KT_RPAREN) { pos++; break; }
        if (t == KT_COMMA) { pos++; if (!first) sw(*it.out, " "); continue; }
        Value v = eval_expr(it, env, pos);
        if (it.failed) { vfree(v); return; }
        vprint(*it.out, v);
        vfree(v);
        first = false;
    }
    sw(*it.out, "\n");
}

static void exec_if(Interp& it, Env& env, int& pos) {
    pos++; // 'if'
    bool done = false;          // a branch already ran (satoru)
    while (!unwinding(it)) {
        Value c = eval_expr(it, env, pos);
        bool take = vtruthy(c);
        vfree(c);
        if (it.failed) return;
        if (cur(it, pos) == KT_THEN) pos++;
        skip_seps(it, pos);
        if (take && !done) {
            done = true;
            exec_block(it, env, pos);
        } else {
            skip_to_terminators(it, pos, true);
        }
        // now at else / elif / end (satoru)
        if (cur(it, pos) == KT_ELIF) { pos++; continue; }
        if (cur(it, pos) == KT_ELSE) {
            pos++; skip_seps(it, pos);
            if (!done && !unwinding(it)) { done = true; exec_block(it, env, pos); }
            else skip_to_terminators(it, pos, false);
        }
        break;
    }
    if (cur(it, pos) == KT_END) pos++;
}

static void exec_while(Interp& it, Env& env, int& pos) {
    pos++; // 'while'
    int cond_pos = pos;
    int iters = 0;
    int end_pos = -1;
    while (!it.failed && !it.ret_flag) {
        pos = cond_pos;
        Value c = eval_expr(it, env, pos);
        bool take = vtruthy(c); vfree(c);
        if (it.failed) return;
        if (cur(it, pos) == KT_DO) pos++;
        skip_seps(it, pos);
        if (!take) { skip_block(it, pos); end_pos = pos; break; }
        if (++iters > KCL_MAX_LOOP_ITERS) { raise_err(it, cur_line(it, pos), "loop iteration limit exceeded"); return; }
        exec_block(it, env, pos);
        if (it.break_flag) { it.break_flag = false; skip_block(it, pos); end_pos = pos; break; }
        if (it.continue_flag) it.continue_flag = false;
        if (it.failed || it.ret_flag) { skip_block(it, pos); end_pos = pos; break; }
    }
    if (end_pos >= 0) pos = end_pos;
    else {
        // condition errored  -  leave pos just past the block. (satoru)
        pos = cond_pos; Value c = eval_expr(it, env, pos); vfree(c);
        if (cur(it, pos) == KT_DO) pos++;
        skip_block(it, pos);
    }
}

// for x in a..b ... end   OR   for x in <list/string> ... end (satoru)
static void exec_for(Interp& it, Env& env, int& pos) {
    int line = cur_line(it, pos);
    pos++; // 'for'
    if (cur(it, pos) != KT_IDENT) { raise_err(it, line, "expected loop variable after 'for'"); return; }
    char var[KCL_MAX_NAME]; kstrcpy(var, it.toks[pos].text, KCL_MAX_NAME);
    pos++;
    if (cur(it, pos) == KT_IN) pos++;
    else { raise_err(it, line, "expected 'in' in for-loop"); return; }

    Value start = eval_expr(it, env, pos);
    if (it.failed) { vfree(start); return; }

    bool is_range = (cur(it, pos) == KT_DOTDOT);
    Value endv; vinit(endv);
    if (is_range) {
        pos++;
        endv = eval_expr(it, env, pos);
        if (it.failed) { vfree(start); vfree(endv); return; }
    }
    if (cur(it, pos) == KT_DO) pos++;
    skip_seps(it, pos);
    int body_pos = pos;

    bool stop = false;
    if (is_range) {
        long long a = vnumi(start), b = vnumi(endv);
        long long step = (a <= b) ? 1 : -1;
        long long iters = 0;
        for (long long i = a; (step > 0 ? i <= b : i >= b) && !stop; i += step) {
            if (++iters > KCL_MAX_LOOP_ITERS) { raise_err(it, line, "loop iteration limit exceeded"); break; }
            Value iv; vinit(iv); vset_int(iv, i); env_set(env, var, iv); vfree(iv);
            pos = body_pos;
            exec_block(it, env, pos);
            if (it.break_flag) { it.break_flag = false; stop = true; }
            if (it.continue_flag) it.continue_flag = false;
            if (it.failed || it.ret_flag) stop = true;
        }
    } else if (start.type == V_LIST) {
        for (int k = 0; k < start.item_count && !stop; k++) {
            env_set(env, var, start.items[k]);
            pos = body_pos;
            exec_block(it, env, pos);
            if (it.break_flag) { it.break_flag = false; stop = true; }
            if (it.continue_flag) it.continue_flag = false;
            if (it.failed || it.ret_flag) stop = true;
        }
    } else if (start.type == V_STR) {
        for (int k = 0; k < start.slen && !stop; k++) {
            char ch[2] = { start.s[k], 0 };
            Value cv; vinit(cv); vset_str(cv, ch, 1); env_set(env, var, cv); vfree(cv);
            pos = body_pos;
            exec_block(it, env, pos);
            if (it.break_flag) { it.break_flag = false; stop = true; }
            if (it.continue_flag) it.continue_flag = false;
            if (it.failed || it.ret_flag) stop = true;
        }
    } else {
        raise_err(it, line, "for-loop expects a range (a..b), list, or string");
    }
    vfree(start); vfree(endv);
    // position after the body's matching end (satoru)
    pos = body_pos;
    skip_block(it, pos);
}

// func name(params) ... end  -  register the body; do not execute. (satoru)
static void exec_func_def(Interp& it, Env& env, int& pos) {
    int line = cur_line(it, pos);
    pos++; // 'func'
    if (cur(it, pos) != KT_IDENT) { raise_err(it, line, "expected function name"); return; }
    char name[KCL_MAX_NAME]; kstrcpy(name, it.toks[pos].text, KCL_MAX_NAME);
    pos++;
    FuncDef fn; fn.param_count = 0; fn.body_start = 0;
    fn.toks = it.toks; fn.count = it.count;  // body lives in the current stream (satoru)
    if (cur(it, pos) == KT_LPAREN) {
        pos++;
        while (cur(it, pos) != KT_RPAREN && cur(it, pos) != KT_EOF) {
            if (cur(it, pos) == KT_IDENT && fn.param_count < KCL_MAX_PARAMS)
                kstrcpy(fn.params[fn.param_count++], it.toks[pos].text, KCL_MAX_NAME);
            pos++;
            if (cur(it, pos) == KT_COMMA) pos++;
        }
        if (cur(it, pos) == KT_RPAREN) pos++;
    }
    if (cur(it, pos) == KT_DO) pos++;
    skip_seps(it, pos);
    fn.body_start = pos;
    // skip body to its end so top-level execution continues after the func (satoru)
    skip_block(it, pos);

    // register in globals so it's callable from anywhere. (satoru)
    Env* g = it.globals;
    Binding* b = env_find_local(*g, name);
    if (!b) {
        for (int i = 0; i < KCL_ENV_SLOTS; i++)
            if (!g->slots[i].used) { b = &g->slots[i]; b->used = true; kstrcpy(b->name, name, KCL_MAX_NAME); vinit(b->val); break; }
    }
    if (b) { b->is_func = true; b->fn = fn; }
    else raise_err(it, line, "too many functions defined");
    (void)env;
}

static void exec_return(Interp& it, Env& env, int& pos) {
    pos++; // 'return'
    KCLTok t = cur(it, pos);
    if (t == KT_NEWLINE || t == KT_SEMI || t == KT_EOF || t == KT_END) {
        vfree(it.ret_val); vinit(it.ret_val);
    } else {
        Value v = eval_expr(it, env, pos);
        vfree(it.ret_val); it.ret_val = v;  // move (satoru)
    }
    it.ret_flag = true;
}

static void exec_import(Interp& it, Env& env, int& pos); // fwd (satoru)

// assignment / bare call / indexed assignment starting with an ident. (satoru)
static void exec_ident_stmt(Interp& it, Env& env, int& pos) {
    int line = cur_line(it, pos);
    char name[KCL_MAX_NAME]; kstrcpy(name, it.toks[pos].text, KCL_MAX_NAME);
    int save = pos;
    pos++;

    // indexed assignment: x[i] = expr (satoru)
    if (cur(it, pos) == KT_LBRACKET) {
        pos++;
        Value idx = eval_expr(it, env, pos);
        if (cur(it, pos) == KT_RBRACKET) pos++;
        else { raise_err(it, line, "expected ']'"); vfree(idx); return; }
        if (cur(it, pos) == KT_ASSIGN) {
            pos++;
            Value rhs = eval_expr(it, env, pos);
            if (!it.failed) {
                Binding* b = env_find(&env, name);
                if (b && b->val.type == V_LIST) {
                    long long n = vnumi(idx);
                    if (n < 0) n += b->val.item_count;
                    if (n < 0 || n >= b->val.item_count) raise_err(it, line, "list index out of range");
                    else vcopy(b->val.items[(int)n], rhs);
                } else raise_err(it, line, "indexed assignment requires a list variable");
            }
            vfree(rhs); vfree(idx); return;
        }
        // not an assignment  -  it was an expression statement; rewind & eval. (satoru)
        vfree(idx);
        pos = save;
        Value v = eval_expr(it, env, pos);
        vfree(v);
        return;
    }

    // plain assignment: x = expr (satoru)
    if (cur(it, pos) == KT_ASSIGN) {
        pos++;
        Value v = eval_expr(it, env, pos);
        if (!it.failed) env_set(env, name, v);
        vfree(v);
        return;
    }

    // otherwise it's an expression statement (e.g. a function call). (satoru)
    pos = save;
    Value v = eval_expr(it, env, pos);
    vfree(v);
}

static void exec_stmt(Interp& it, Env& env, int& pos) {
    if (it.failed) return;
    skip_seps(it, pos);
    KCLTok t = cur(it, pos);
    switch (t) {
        case KT_EOF: return;
        case KT_SET:    exec_set(it, env, pos); break;
        case KT_PRINT:  exec_print(it, env, pos); break;
        case KT_IF:     exec_if(it, env, pos); break;
        case KT_WHILE:  exec_while(it, env, pos); break;
        case KT_FOR:    exec_for(it, env, pos); break;
        case KT_FUNC:   exec_func_def(it, env, pos); break;
        case KT_RETURN: exec_return(it, env, pos); break;
        case KT_IMPORT: exec_import(it, env, pos); break;
        case KT_BREAK:  pos++; it.break_flag = true; break;
        case KT_CONTINUE: pos++; it.continue_flag = true; break;
        case KT_IDENT:  exec_ident_stmt(it, env, pos); break;
        case KT_END: case KT_ELSE: case KT_ELIF:
            // stray terminator  -  let the enclosing block handle it. (satoru)
            return;
        default: {
            // bare expression statement (satoru)
            Value v = eval_expr(it, env, pos);
            vfree(v);
            break;
        }
    }
    // consume the statement separator if present (satoru)
    if (cur(it, pos) == KT_NEWLINE || cur(it, pos) == KT_SEMI) pos++;
}

// ── function calls ────────────────────────────────────────────────────────
static Value call_func(Interp& it, const char* name, FuncDef& fn, Value* args, int argc, int line) {
    Value rv; vinit(rv);
    if (it.recursion >= KCL_MAX_RECURSION) { raise_err2(it, line, "recursion limit exceeded in: ", name); return rv; }
    it.recursion++;

    // heap-allocate the call scope  -  an Env is ~47kb, which would overflow the
    // 64kb kernel stack instantly on any recursion. (satoru)
    Env* local = (Env*)KernelHeap::Alloc(sizeof(Env));
    if (!local) { raise_err(it, line, "out of memory (call scope)"); it.recursion--; return rv; }
    env_init(*local, it.globals);  // params/locals; funcs come from globals (satoru)
    for (int p = 0; p < fn.param_count; p++) {
        Value a; vinit(a);
        if (p < argc) vcopy(a, args[p]);
        env_set_local(*local, fn.params[p], a);
        vfree(a);
    }

    int pos = fn.body_start;
    bool saved_ret = it.ret_flag, saved_brk = it.break_flag, saved_cont = it.continue_flag;
    it.ret_flag = false; it.break_flag = false; it.continue_flag = false;
    Value saved_retval; vinit(saved_retval); vcopy(saved_retval, it.ret_val); vfree(it.ret_val); vinit(it.ret_val);

    // the body may live in a different token stream (an imported file), so swap
    // the interpreter onto the function's stream for the call and restore it
    // after  -  otherwise body_start indexes the wrong (or freed) array. (satoru)
    KCLToken* saved_toks = it.toks; int saved_count = it.count;
    if (fn.toks) { it.toks = fn.toks; it.count = fn.count; }

    exec_block(it, *local, pos);

    it.toks = saved_toks; it.count = saved_count;

    if (it.ret_flag) { vcopy(rv, it.ret_val); }
    // restore caller's control flags; consume the callee's return. (satoru)
    vfree(it.ret_val); it.ret_val = saved_retval;
    it.ret_flag = saved_ret; it.break_flag = saved_brk; it.continue_flag = saved_cont;

    if (local->overflow && !it.failed) raise_err(it, line, "too many local variables");
    env_clear(*local);
    KernelHeap::Free(local);
    it.recursion--;
    return rv;
}

// ── builtin functions (stdlib) ──────────────────────────────────────────
// software square root via newton's method on a double (no fpu intrinsic). (satoru)
static double dsqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) { if (g == 0.0) break; g = 0.5 * (g + x / g); }
    return g;
}

static unsigned long long g_rng = 0x2545F4914F6CDD1DULL;
static unsigned long long rng_next() {
    // xorshift64*  -  deterministic, no fpu. (satoru)
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

// run a shell command string; capture its output. (satoru)
static Value bi_exec(Interp& it, const char* cmd) {
    Value out; vinit(out);
    KuronoShell* sh = KCL::GetShell();
    if (!sh) { vset_str(out, "", 0); return out; }
    char* buf = (char*)KernelHeap::Alloc(SHELL_OUTPUT_BUF);
    if (!buf) { vset_str(out, "", 0); return out; }
    buf[0] = 0;
    sh->Execute(cmd, buf, SHELL_OUTPUT_BUF);
    vset_str(out, buf, kstrlen(buf));
    KernelHeap::Free(buf);
    (void)it;
    return out;
}

// returns a value; sets handled=true if 'name' is a recognised builtin. (satoru)
static Value call_builtin(Interp& it, Env& env, const char* name, Value* args, int argc, bool& handled, int line) {
    (void)env;
    Value r; vinit(r);
    handled = true;

    if (kstreq(name, "print")) {
        for (int k = 0; k < argc; k++) { if (k) sw(*it.out, " "); vprint(*it.out, args[k]); }
        sw(*it.out, "\n");
        return r; // none
    }
    if (kstreq(name, "input")) {
        // headless kernel: no interactive stdin. echo the prompt and return an
        // empty string so scripts stay deterministic under automation. (satoru)
        if (argc >= 1) vprint(*it.out, args[0]);
        vset_str(r, "", 0);
        return r;
    }
    if (kstreq(name, "len")) {
        if (argc < 1) { raise_err(it, line, "len() needs 1 argument"); return r; }
        if (args[0].type == V_STR) vset_int(r, args[0].slen);
        else if (args[0].type == V_LIST) vset_int(r, args[0].item_count);
        else raise_err(it, line, "len() needs a string or list");
        return r;
    }
    if (kstreq(name, "str")) {
        char* s = (argc >= 1) ? vto_cstr(args[0]) : nullptr;
        vset_str(r, s ? s : "", s ? kstrlen(s) : 0);
        if (s) KernelHeap::Free(s);
        return r;
    }
    if (kstreq(name, "int")) {
        if (argc < 1) { vset_int(r, 0); return r; }
        if (args[0].type == V_STR) {
            const char* p = args[0].s ? args[0].s : ""; long long v = 0; bool neg = false;
            while (*p == ' ') p++;
            if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            vset_int(r, neg ? -v : v);
        } else vset_int(r, vnumi(args[0]));
        return r;
    }
    if (kstreq(name, "float")) {
        if (argc < 1) { vset_float(r, 0.0); return r; }
        if (args[0].type == V_STR) {
            const char* p = args[0].s ? args[0].s : ""; double v = 0; bool neg = false;
            while (*p == ' ') p++;
            if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (*p == '.') { p++; double sc = 0.1; while (*p >= '0' && *p <= '9') { v += (*p - '0') * sc; sc *= 0.1; p++; } }
            vset_float(r, neg ? -v : v);
        } else vset_float(r, vnumf(args[0]));
        return r;
    }
    if (kstreq(name, "abs")) {
        if (argc < 1) { vset_int(r, 0); return r; }
        if (args[0].type == V_FLOAT) { double d = args[0].f; vset_float(r, d < 0 ? -d : d); }
        else { long long v = vnumi(args[0]); vset_int(r, v < 0 ? -v : v); }
        return r;
    }
    if (kstreq(name, "sqrt")) {
        if (argc < 1) { vset_float(r, 0.0); return r; }
        vset_float(r, dsqrt(vnumf(args[0])));
        return r;
    }
    if (kstreq(name, "rand")) {
        // rand() -> int [0,32767]; rand(n) -> [0,n); rand(a,b) -> [a,b]. (satoru)
        unsigned long long x = rng_next();
        if (argc >= 2) { long long a = vnumi(args[0]), b = vnumi(args[1]); if (b < a) { long long t = a; a = b; b = t; } long long span = b - a + 1; vset_int(r, a + (long long)(x % (span > 0 ? (unsigned long long)span : 1ULL))); }
        else if (argc >= 1) { long long n = vnumi(args[0]); if (n <= 0) vset_int(r, 0); else vset_int(r, (long long)(x % (unsigned long long)n)); }
        else vset_int(r, (long long)(x % 32768ULL));
        return r;
    }
    if (kstreq(name, "min")) {
        if (argc < 1) { raise_err(it, line, "min() needs arguments"); return r; }
        int best = 0; for (int k = 1; k < argc; k++) { bool ok; if (value_cmp(args[k], args[best], ok) < 0 && ok) best = k; }
        vcopy(r, args[best]); return r;
    }
    if (kstreq(name, "max")) {
        if (argc < 1) { raise_err(it, line, "max() needs arguments"); return r; }
        int best = 0; for (int k = 1; k < argc; k++) { bool ok; if (value_cmp(args[k], args[best], ok) > 0 && ok) best = k; }
        vcopy(r, args[best]); return r;
    }
    if (kstreq(name, "type")) {
        const char* tn = "none";
        if (argc >= 1) switch (args[0].type) {
            case V_INT: tn = "int"; break; case V_FLOAT: tn = "float"; break;
            case V_BOOL: tn = "bool"; break; case V_STR: tn = "string"; break;
            case V_LIST: tn = "list"; break; default: tn = "none"; break;
        }
        vset_str(r, tn, kstrlen(tn));
        return r;
    }
    if (kstreq(name, "append")) {
        // append(list, value) -> the grown list (lists are value types here, so
        // callers do `x = append(x, v)`). (satoru)
        if (argc < 2 || args[0].type != V_LIST) { raise_err(it, line, "append() needs (list, value)"); return r; }
        vcopy(r, args[0]); list_push(r, args[1]);
        return r;
    }
    if (kstreq(name, "remove")) {
        // remove(list, index) -> the list without that index. (satoru)
        if (argc < 2 || args[0].type != V_LIST) { raise_err(it, line, "remove() needs (list, index)"); return r; }
        long long idx = vnumi(args[1]);
        r.type = V_LIST; r.item_cap = 4; r.items = alloc_items(r.item_cap); r.item_count = 0;
        for (int k = 0; k < args[0].item_count; k++) if (k != idx) list_push(r, args[0].items[k]);
        return r;
    }
    if (kstreq(name, "exists")) {
        if (argc < 1 || args[0].type != V_STR) { vset_bool(r, false); return r; }
        vset_bool(r, KVFS::Exists(args[0].s ? args[0].s : ""));
        return r;
    }
    if (kstreq(name, "read")) {
        if (argc < 1 || args[0].type != V_STR) { raise_err(it, line, "read() needs a path string"); return r; }
        const char* path = args[0].s ? args[0].s : "";
        char* buf = (char*)KernelHeap::Alloc(KVFS_MAX_CONTENT);
        if (!buf) { vset_str(r, "", 0); return r; }
        int n = KVFS::ReadFile(path, buf, KVFS_MAX_CONTENT - 1);
        if (n < 0) { KernelHeap::Free(buf); raise_err2(it, line, "read() cannot open: ", path); return r; }
        buf[n] = 0;
        vset_str(r, buf, n);
        KernelHeap::Free(buf);
        return r;
    }
    if (kstreq(name, "write")) {
        if (argc < 2 || args[0].type != V_STR) { raise_err(it, line, "write() needs (path, content)"); return r; }
        const char* path = args[0].s ? args[0].s : "";
        char* content = vto_cstr(args[1]);
        int n = content ? kstrlen(content) : 0;
        int rc = KVFS::WriteFile(path, content ? content : "", (unsigned)n);
        if (content) KernelHeap::Free(content);
        vset_bool(r, rc >= 0);
        return r;
    }
    if (kstreq(name, "exec")) {
        if (argc < 1 || args[0].type != V_STR) { raise_err(it, line, "exec() needs a command string"); return r; }
        return bi_exec(it, args[0].s ? args[0].s : "");
    }
    if (kstreq(name, "sleep")) {
        if (argc >= 1) { long long ms = vnumi(args[0]); if (ms < 0) ms = 0; if (ms > 60000) ms = 60000; Timer::WaitMs((unsigned)ms); }
        return r; // none
    }
    if (kstreq(name, "upper") || kstreq(name, "lower")) {
        if (argc < 1 || args[0].type != V_STR) { raise_err(it, line, "upper/lower() needs a string"); return r; }
        bool up = kstreq(name, "upper");
        int n = args[0].slen; vset_str(r, args[0].s ? args[0].s : "", n);
        for (int k = 0; k < n && r.s; k++) {
            char c = r.s[k];
            if (up && c >= 'a' && c <= 'z') r.s[k] = c - 32;
            if (!up && c >= 'A' && c <= 'Z') r.s[k] = c + 32;
        }
        return r;
    }

    handled = false;
    return r;
}

// ── import: run another .kcl file's top level into the current globals ───
static void exec_import(Interp& it, Env& env, int& pos) {
    int line = cur_line(it, pos);
    pos++; // 'import'
    char path[KVFS_MAX_PATH];
    if (cur(it, pos) == KT_STRING) {
        kstrcpy(path, it.toks[pos].str ? it.toks[pos].str : "", KVFS_MAX_PATH);
        pos++;
    } else if (cur(it, pos) == KT_IDENT) {
        // import foo[.kcl] -> /kurono/lib/foo.kcl then ./foo.kcl. (satoru)
        char base[KCL_MAX_NAME]; kstrcpy(base, it.toks[pos].text, KCL_MAX_NAME);
        pos++;
        // allow dotted: foo.kcl (ident '.' ident) (satoru)
        if (cur(it, pos) == KT_DOT && cur(it, pos + 1) == KT_IDENT) {
            kstrcpy(path, base, KVFS_MAX_PATH);
            int pl = kstrlen(path); if (pl < KVFS_MAX_PATH - 1) path[pl++] = '.'; path[pl] = 0;
            int o = pl; const char* ext = it.toks[pos + 1].text;
            for (int k = 0; ext[k] && o < KVFS_MAX_PATH - 1; k++) path[o++] = ext[k];
            path[o] = 0;
            pos += 2;
        } else {
            kstrcpy(path, base, KVFS_MAX_PATH);
        }
    } else { raise_err(it, line, "import expects a filename"); return; }

    if (!kends_with(path, ".kcl")) {
        int pl = kstrlen(path);
        const char* ext = ".kcl";
        for (int k = 0; ext[k] && pl < KVFS_MAX_PATH - 1; k++) path[pl++] = ext[k];
        path[pl] = 0;
    }

    char resolved[KVFS_MAX_PATH];
    if (KVFS::Exists(path)) kstrcpy(resolved, path, KVFS_MAX_PATH);
    else {
        // try /kurono/lib/<name> (satoru)
        kstrcpy(resolved, "/kurono/lib/", KVFS_MAX_PATH);
        int rl = kstrlen(resolved);
        for (int k = 0; path[k] && rl < KVFS_MAX_PATH - 1; k++) resolved[rl++] = path[k];
        resolved[rl] = 0;
    }

    if (it.import_depth >= KCL_MAX_IMPORTS) { raise_err(it, line, "import recursion too deep"); return; }
    if (!KVFS::Exists(resolved)) { raise_err2(it, line, "import: file not found: ", path); return; }

    char* src = (char*)KernelHeap::Alloc(KCL_MAX_SCRIPT);
    if (!src) { raise_err(it, line, "import: out of memory"); return; }
    int n = KVFS::ReadFile(resolved, src, KCL_MAX_SCRIPT - 1);
    if (n < 0) { KernelHeap::Free(src); raise_err2(it, line, "import: cannot read: ", resolved); return; }
    src[n] = 0;

    // lex + execute the imported source into the SAME globals so its funcs and
    // top-level vars become visible to the importing script. (satoru)
    Lexer lx; lex(src, lx);
    if (!lx.ok) { lex_free(lx); KernelHeap::Free(src); raise_err(it, line, "import: lexer failure"); return; }

    // retain the imported token stream + source for the rest of the run  -  any
    // function it defines references this stream by index. (satoru)
    RetainedLex* rl = (RetainedLex*)KernelHeap::Alloc(sizeof(RetainedLex));
    if (!rl) { lex_free(lx); KernelHeap::Free(src); raise_err(it, line, "import: out of memory"); return; }
    rl->toks = lx.toks; rl->count = lx.count; rl->src = src;
    if (it.retained) { rl->next = *it.retained; *it.retained = rl; }
    else rl->next = nullptr;

    Interp sub = it;          // share globals/out/err/retained (satoru)
    sub.toks = lx.toks; sub.count = lx.count;
    sub.ret_flag = false; sub.break_flag = false; sub.continue_flag = false;
    vinit(sub.ret_val);
    sub.import_depth = it.import_depth + 1;

    int sp = 0;
    skip_seps(sub, sp);
    while (cur(sub, sp) != KT_EOF && !sub.failed && !sub.ret_flag) {
        exec_stmt(sub, *it.globals, sp);
        skip_seps(sub, sp);
    }
    vfree(sub.ret_val);
    it.failed = sub.failed;   // propagate hard errors (satoru)
    // do NOT free lx/src here  -  retained above and freed at end of run. (satoru)
    (void)env;
}

// ── top-level driver ──────────────────────────────────────────────────────
static int run_source(const char* source, char* output, int max_output) {
    Sink out; out.buf = output; out.cap = max_output; out.len = 0; out.overflow = false;
    output[0] = 0;

    Lexer lx; lex(source, lx);
    if (!lx.ok) { lex_free(lx); sw(out, "kcl: out of memory tokenising script\n"); return out.len; }

    // Env is ~47kb (96 binding slots), far too big for the 64kb kernel stack  - 
    // heap-allocate it (and every callee scope) so deep recursion stays cheap. (satoru)
    Env* globals = (Env*)KernelHeap::Alloc(sizeof(Env));
    if (!globals) { lex_free(lx); sw(out, "kcl: out of memory (env)\n"); return out.len; }
    env_init(*globals, nullptr);

    RetainedLex* retained = nullptr;  // imported streams, freed below (satoru)

    Interp it;
    it.toks = lx.toks; it.count = lx.count;
    it.out = &out; it.err = &out; it.globals = globals;
    it.failed = false; it.ret_flag = false; it.break_flag = false; it.continue_flag = false;
    vinit(it.ret_val);
    it.recursion = 0; it.import_depth = 0;
    it.retained = &retained;

    int pos = 0;
    skip_seps(it, pos);
    int guard = 0;
    while (cur(it, pos) != KT_EOF && !it.failed && !it.ret_flag) {
        int before = pos;
        exec_stmt(it, *globals, pos);
        skip_seps(it, pos);
        if (pos == before) { pos++; }     // never spin (satoru)
        if (++guard > KCL_MAX_TOKENS * 4) { sw(out, "kcl: aborted (statement limit)\n"); break; }
    }

    if (globals->overflow && !it.failed) sw(out, "kcl: too many global variables\n");

    vfree(it.ret_val);
    env_clear(*globals);
    KernelHeap::Free(globals);
    lex_free(lx);
    // free every retained import stream + its source. (satoru)
    while (retained) {
        RetainedLex* nx = retained->next;
        Lexer tmp; tmp.toks = retained->toks; tmp.count = retained->count; tmp.ok = true;
        lex_free(tmp);
        if (retained->src) KernelHeap::Free(retained->src);
        KernelHeap::Free(retained);
        retained = nx;
    }
    return out.len;
}

// ── shell command handlers ──────────────────────────────────────────────
static int cmd_kcl(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        Sink s; s.buf = out; s.cap = maxo; s.len = 0; s.overflow = false;
        sw(s, "KCL - Kurono Command Language\n");
        sw(s, "usage:\n");
        sw(s, "  kcl <file.kcl>        run a script file\n");
        sw(s, "  kcl -c \"code\"         run inline code\n");
        sw(s, "  kcl -e \"code\"         run inline code (alias)\n");
        sw(s, "language: set/let, if/elif/else/end, while, for x in a..b / list,\n");
        sw(s, "          func name(args) ... end, return, import, break, continue\n");
        sw(s, "builtins: print input len str int float sqrt rand abs min max type\n");
        sw(s, "          read write exists exec sleep append remove upper lower\n");
        return s.len;
    }
    if ((kstreq(argv[1], "-c") || kstreq(argv[1], "-e")) && argc >= 3) {
        return KCL::Execute(argv[2], out, maxo);
    }
    return KCL::ExecFile(argv[1], out, maxo);
}

} // anonymous namespace

// ────────────────────────────────────────────────────────────────────────
//  public api
// ────────────────────────────────────────────────────────────────────────
void KCL::Init(KuronoShell* sh) {
    shell = sh;
    // seed the rng from the timer so rand() differs across boots. (satoru)
    g_rng ^= ((unsigned long long)Timer::GetRealMs() << 16) | 0x9E3779B97F4A7C15ULL;
    if (g_rng == 0) g_rng = 0x2545F4914F6CDD1DULL;
    if (sh) {
        sh->RegisterCommand("kcl", "Run KCL script", ENV_KURONO, "scripting", cmd_kcl);
        sh->RegisterCommand("run", "Run KCL script", ENV_KURONO, "scripting", cmd_kcl);
    }
    SerialLogger::Log("KCL: interpreter initialized\r\n");
}

int KCL::Execute(const char* source, char* output, int max_output) {
    if (!source || !output || max_output <= 1) return 0;
    return run_source(source, output, max_output);
}

int KCL::ExecLine(const char* line, char* output, int max_output) {
    return Execute(line, output, max_output);
}

int KCL::ExecFile(const char* path, char* output, int max_output) {
    if (!path || !output || max_output <= 1) return 0;
    char* buf = (char*)KernelHeap::Alloc(KCL_MAX_SCRIPT);
    if (!buf) { Sink s; s.buf = output; s.cap = max_output; s.len = 0; s.overflow = false; sw(s, "kcl: out of memory\n"); return s.len; }
    int n = KVFS::ReadFile(path, buf, KCL_MAX_SCRIPT - 1);
    if (n < 0) {
        KernelHeap::Free(buf);
        Sink s; s.buf = output; s.cap = max_output; s.len = 0; s.overflow = false;
        sw(s, "kcl: cannot open file: "); sw(s, path); swc(s, '\n');
        return s.len;
    }
    buf[n] = 0;
    int r = run_source(buf, output, max_output);
    KernelHeap::Free(buf);
    return r;
}
// end (satoru)
