// kurono os  -  built-in mini Python 3 interpreter.
//
// Supported subset (sufficient for hello-world, fizzbuzz, fibonacci,
// list comprehensions are NOT supported  -  but plain for/while are):
//   - literals: int, float, "string" / 'string', True/False/None, [a,b,c]
//   - operators: + - * / // % **, == != < > <= >=, and, or, not, in
//   - parens, function calls, list/string indexing, len/range/str/int/abs/min/max/print/type/input
//   - assignment (single target, no tuple unpacking)
//   - if / elif / else, while, for <var> in <iterable> (list or range)
//   - def name(p1,p2,...): / return <expr>
//   - comments starting with #
//
// Parser is line-oriented and indentation-aware (1 indent = 1 step,
// any consistent whitespace prefix counts as one level deeper).

#include "python_interp.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

namespace {

// ---------------- output sink ----------------
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
    if (s.len < s.cap - 1) s.buf[s.len++] = c;
    else s.overflow = true;
    s.buf[s.len] = 0;
}
static void swi(Sink& s, long long v) {
    if (v < 0) { swc(s, '-'); v = -v; }
    char t[24]; int ti = 0;
    if (v == 0) t[ti++] = '0';
    while (v > 0) { t[ti++] = '0' + (char)(v % 10); v /= 10; }
    while (ti > 0) swc(s, t[--ti]);
}
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
        if (dig < 0) dig = 0; if (dig > 9) dig = 9;
        swc(s, '0' + (char)dig);
        frac -= (double)dig;
    }
}

// ---------------- helpers ----------------
static int sl(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static bool seq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void scp(char* d, const char* s, int m) {
    int i = 0;
    while (s && s[i] && i < m - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static bool isd(char c) { return c >= '0' && c <= '9'; }
static bool isa(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool ian(char c) { return isa(c) || isd(c); }

// ---------------- Value ----------------
enum VType { V_NONE = 0, V_INT, V_FLOAT, V_BOOL, V_STR, V_LIST, V_FUNC };

struct Value {
    int type;
    long long i;
    double f;
    char* s;          // owned for STR
    int slen;
    Value* items;     // owned for LIST
    int item_count;
    int item_cap;
    // FUNC fields
    int func_body_start;
    int func_body_end;
    int func_indent;
    char fparams[6][24];
    int  fparam_count;
};

static void vinit(Value& v) {
    v.type = V_NONE; v.i = 0; v.f = 0.0;
    v.s = nullptr; v.slen = 0;
    v.items = nullptr; v.item_count = 0; v.item_cap = 0;
    v.func_body_start = v.func_body_end = v.func_indent = 0;
    v.fparam_count = 0;
}

static void vfree(Value& v); // fwd

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

static void vset_str(Value& v, const char* src, int n) {
    vfree(v);
    if (n < 0) n = sl(src);
    char* buf = (char*)KernelHeap::Alloc((unsigned)(n + 1));
    if (!buf) { v.type = V_NONE; return; }
    for (int i = 0; i < n; i++) buf[i] = src[i];
    buf[n] = 0;
    v.type = V_STR; v.s = buf; v.slen = n;
}

static void vcopy(Value& dst, const Value& src) {
    vfree(dst);
    dst.type = src.type;
    dst.i = src.i; dst.f = src.f;
    dst.func_body_start = src.func_body_start;
    dst.func_body_end = src.func_body_end;
    dst.func_indent = src.func_indent;
    dst.fparam_count = src.fparam_count;
    for (int p = 0; p < src.fparam_count; p++)
        scp(dst.fparams[p], src.fparams[p], 24);
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
        list.type = V_LIST;
        list.item_cap = 4;
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
        list.items = na;
        list.item_cap = nc;
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
    if (v.type == V_NONE) return false;
    if (v.type == V_BOOL) return v.i != 0;
    if (v.type == V_INT)  return v.i != 0;
    if (v.type == V_FLOAT)return v.f != 0.0;
    if (v.type == V_STR)  return v.slen > 0;
    if (v.type == V_LIST) return v.item_count > 0;
    return true;
}

static void vprint(Sink& s, const Value& v) {
    switch (v.type) {
        case V_NONE:  sw(s, "None"); break;
        case V_INT:   swi(s, v.i); break;
        case V_FLOAT: swd(s, v.f); break;
        case V_BOOL:  sw(s, v.i ? "True" : "False"); break;
        case V_STR:   if (v.s) sw(s, v.s); break;
        case V_LIST: {
            swc(s, '[');
            for (int i = 0; i < v.item_count; i++) {
                if (i) sw(s, ", ");
                if (v.items[i].type == V_STR) { swc(s, '\''); if (v.items[i].s) sw(s, v.items[i].s); swc(s, '\''); }
                else vprint(s, v.items[i]);
            }
            swc(s, ']');
            break;
        }
        case V_FUNC:  sw(s, "<function>"); break;
    }
}

// ---------------- env (variables + functions) ----------------
struct Binding { char name[24]; Value val; bool used; };
static const int ENV_SLOTS = 64;

struct Env {
    Binding slots[ENV_SLOTS];
    Env* parent;
};

static void env_init(Env& e, Env* parent) {
    for (int i = 0; i < ENV_SLOTS; i++) {
        e.slots[i].used = false;
        e.slots[i].name[0] = 0;
        vinit(e.slots[i].val);
    }
    e.parent = parent;
}

static void env_clear(Env& e) {
    for (int i = 0; i < ENV_SLOTS; i++) {
        if (e.slots[i].used) {
            vfree(e.slots[i].val);
            e.slots[i].used = false;
            e.slots[i].name[0] = 0;
        }
    }
}

static Value* env_find_local(Env& e, const char* name) {
    for (int i = 0; i < ENV_SLOTS; i++)
        if (e.slots[i].used && seq(e.slots[i].name, name))
            return &e.slots[i].val;
    return nullptr;
}

static Value* env_find(Env* e, const char* name) {
    while (e) {
        Value* v = env_find_local(*e, name);
        if (v) return v;
        e = e->parent;
    }
    return nullptr;
}

static void env_set(Env& e, const char* name, const Value& v) {
    Value* existing = env_find_local(e, name);
    if (existing) { vcopy(*existing, v); return; }
    for (int i = 0; i < ENV_SLOTS; i++) {
        if (!e.slots[i].used) {
            e.slots[i].used = true;
            scp(e.slots[i].name, name, 24);
            vinit(e.slots[i].val);
            vcopy(e.slots[i].val, v);
            return;
        }
    }
}

// ---------------- line model ----------------
struct Line { int indent; const char* text; int len; };

static int count_indent(const char* s, int n) {
    int ind = 0;
    int i = 0;
    while (i < n) {
        if (s[i] == ' ') ind++;
        else if (s[i] == '\t') ind += 4;
        else break;
        i++;
    }
    return ind;
}

static bool line_blank(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] == '#') return true;
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return false;
    }
    return true;
}

// trim trailing comment + whitespace, return effective length (skipping leading indent)
static int trim_line(const char* s, int n, int& start_out) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    start_out = i;
    int end = n;
    bool in_str = false; char qc = 0;
    int hash = -1;
    for (int j = i; j < end; j++) {
        char c = s[j];
        if (in_str) {
            if (c == '\\' && j + 1 < end) { j++; continue; }
            if (c == qc) in_str = false;
        } else {
            if (c == '"' || c == '\'') { in_str = true; qc = c; }
            else if (c == '#') { hash = j; break; }
        }
    }
    if (hash >= 0) end = hash;
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) end--;
    return end;
}

// ---------------- expression parser/evaluator ----------------
struct Parser {
    const char* src;
    int pos;
    int len;
    Sink* err;
    bool failed;
};

static void perr(Parser& p, const char* msg) {
    if (!p.failed) {
        sw(*p.err, "Error: ");
        sw(*p.err, msg);
        swc(*p.err, '\n');
    }
    p.failed = true;
}

static void pskip(Parser& p) {
    while (p.pos < p.len && (p.src[p.pos] == ' ' || p.src[p.pos] == '\t')) p.pos++;
}

static bool ppeek(Parser& p, const char* lit) {
    pskip(p);
    int n = sl(lit);
    if (p.pos + n > p.len) return false;
    for (int i = 0; i < n; i++) if (p.src[p.pos + i] != lit[i]) return false;
    // word boundary for keywords
    if (isa(lit[0])) {
        if (p.pos + n < p.len && ian(p.src[p.pos + n])) return false;
    }
    return true;
}

static bool peat(Parser& p, const char* lit) {
    if (!ppeek(p, lit)) return false;
    p.pos += sl(lit);
    return true;
}

static Value eval_expr(Parser& p, Env& env);

static bool read_ident(Parser& p, char* out, int max) {
    pskip(p);
    if (p.pos >= p.len || !isa(p.src[p.pos])) return false;
    int o = 0;
    while (p.pos < p.len && ian(p.src[p.pos]) && o < max - 1) {
        out[o++] = p.src[p.pos++];
    }
    out[o] = 0;
    return true;
}

// forward
struct Program;
static Value eval_call_func(Program& prog, Env& outer, const Value& func, Value* args, int argc, Sink& err);

struct Program {
    Line* lines;
    int line_count;
    Sink* out;
};

static Program* g_program = nullptr;
static int g_recursion = 0;

static Value eval_atom(Parser& p, Env& env) {
    Value v; vinit(v);
    pskip(p);
    if (p.pos >= p.len) { perr(p, "expected expression"); return v; }

    char c = p.src[p.pos];

    if (c == '(') {
        p.pos++;
        Value inner = eval_expr(p, env);
        pskip(p);
        if (p.pos < p.len && p.src[p.pos] == ')') p.pos++;
        else perr(p, "missing ')'");
        return inner;
    }

    if (c == '[') {
        p.pos++;
        v.type = V_LIST;
        v.item_cap = 4;
        v.items = alloc_items(v.item_cap);
        v.item_count = 0;
        pskip(p);
        if (p.pos < p.len && p.src[p.pos] == ']') { p.pos++; return v; }
        while (true) {
            Value e = eval_expr(p, env);
            list_push(v, e);
            vfree(e);
            pskip(p);
            if (p.pos < p.len && p.src[p.pos] == ',') { p.pos++; continue; }
            if (p.pos < p.len && p.src[p.pos] == ']') { p.pos++; break; }
            perr(p, "missing ']'"); break;
        }
        return v;
    }

    if (c == '"' || c == '\'') {
        char qc = c;
        p.pos++;
        char buf[512]; int bi = 0;
        while (p.pos < p.len && p.src[p.pos] != qc && bi < 511) {
            char ch = p.src[p.pos++];
            if (ch == '\\' && p.pos < p.len) {
                char esc = p.src[p.pos++];
                if (esc == 'n') ch = '\n';
                else if (esc == 't') ch = '\t';
                else if (esc == 'r') ch = '\r';
                else if (esc == '\\') ch = '\\';
                else if (esc == '\'') ch = '\'';
                else if (esc == '"') ch = '"';
                else ch = esc;
            }
            buf[bi++] = ch;
        }
        if (p.pos < p.len && p.src[p.pos] == qc) p.pos++;
        else perr(p, "unterminated string");
        buf[bi] = 0;
        vset_str(v, buf, bi);
        return v;
    }

    if (isd(c) || (c == '.' && p.pos + 1 < p.len && isd(p.src[p.pos + 1]))) {
        long long iv = 0;
        bool is_float = false;
        double fv = 0;
        while (p.pos < p.len && isd(p.src[p.pos])) {
            iv = iv * 10 + (p.src[p.pos] - '0');
            p.pos++;
        }
        if (p.pos < p.len && p.src[p.pos] == '.') {
            is_float = true;
            fv = (double)iv;
            p.pos++;
            double frac = 0.1;
            while (p.pos < p.len && isd(p.src[p.pos])) {
                fv += (double)(p.src[p.pos] - '0') * frac;
                frac *= 0.1;
                p.pos++;
            }
        }
        if (is_float) { v.type = V_FLOAT; v.f = fv; }
        else          { v.type = V_INT; v.i = iv; }
        return v;
    }

    if (isa(c)) {
        char name[24];
        if (!read_ident(p, name, 24)) { perr(p, "bad identifier"); return v; }

        if (seq(name, "True"))  { v.type = V_BOOL; v.i = 1; return v; }
        if (seq(name, "False")) { v.type = V_BOOL; v.i = 0; return v; }
        if (seq(name, "None"))  { v.type = V_NONE; return v; }
        if (seq(name, "not")) {
            Value rhs = eval_atom(p, env);
            v.type = V_BOOL; v.i = vtruthy(rhs) ? 0 : 1;
            vfree(rhs);
            return v;
        }

        // call?
        pskip(p);
        if (p.pos < p.len && p.src[p.pos] == '(') {
            p.pos++;
            Value args[6];
            for (int i = 0; i < 6; i++) vinit(args[i]);
            int argc = 0;
            pskip(p);
            if (!(p.pos < p.len && p.src[p.pos] == ')')) {
                while (argc < 6) {
                    args[argc++] = eval_expr(p, env);
                    pskip(p);
                    if (p.pos < p.len && p.src[p.pos] == ',') { p.pos++; continue; }
                    break;
                }
            }
            pskip(p);
            if (p.pos < p.len && p.src[p.pos] == ')') p.pos++;
            else perr(p, "missing ')'");

            // builtins
            if (seq(name, "print")) {
                for (int i = 0; i < argc; i++) {
                    if (i) swc(*g_program->out, ' ');
                    vprint(*g_program->out, args[i]);
                }
                swc(*g_program->out, '\n');
                v.type = V_NONE;
            } else if (seq(name, "len")) {
                v.type = V_INT;
                if (argc >= 1) {
                    if (args[0].type == V_STR)  v.i = args[0].slen;
                    else if (args[0].type == V_LIST) v.i = args[0].item_count;
                    else v.i = 0;
                }
            } else if (seq(name, "range")) {
                long long start = 0, stop = 0, step = 1;
                if (argc == 1) stop = vnumi(args[0]);
                else if (argc == 2) { start = vnumi(args[0]); stop = vnumi(args[1]); }
                else if (argc >= 3) { start = vnumi(args[0]); stop = vnumi(args[1]); step = vnumi(args[2]); }
                v.type = V_LIST;
                v.item_cap = 16;
                v.items = alloc_items(v.item_cap);
                v.item_count = 0;
                if (step != 0) {
                    if (step > 0) for (long long x = start; x < stop; x += step) {
                        Value t; vinit(t); t.type = V_INT; t.i = x; list_push(v, t);
                    }
                    else for (long long x = start; x > stop; x += step) {
                        Value t; vinit(t); t.type = V_INT; t.i = x; list_push(v, t);
                    }
                }
            } else if (seq(name, "str")) {
                char buf[64]; int bi = 0;
                Sink local = { buf, 64, 0, false };
                if (argc >= 1) vprint(local, args[0]);
                vset_str(v, buf, local.len);
            } else if (seq(name, "int")) {
                v.type = V_INT;
                if (argc >= 1) {
                    if (args[0].type == V_STR) {
                        long long iv = 0; bool neg = false; int j = 0;
                        if (args[0].s && args[0].s[0] == '-') { neg = true; j = 1; }
                        for (; j < args[0].slen; j++) {
                            char ch = args[0].s[j];
                            if (ch < '0' || ch > '9') break;
                            iv = iv * 10 + (ch - '0');
                        }
                        v.i = neg ? -iv : iv;
                    } else v.i = vnumi(args[0]);
                }
            } else if (seq(name, "float")) {
                v.type = V_FLOAT; if (argc >= 1) v.f = vnumf(args[0]);
            } else if (seq(name, "abs")) {
                if (argc >= 1 && args[0].type == V_FLOAT) {
                    v.type = V_FLOAT; v.f = args[0].f < 0 ? -args[0].f : args[0].f;
                } else {
                    v.type = V_INT; long long x = argc >= 1 ? vnumi(args[0]) : 0;
                    v.i = x < 0 ? -x : x;
                }
            } else if (seq(name, "min") || seq(name, "max")) {
                bool is_min = seq(name, "min");
                if (argc == 1 && args[0].type == V_LIST && args[0].item_count > 0) {
                    int best = 0;
                    for (int i = 1; i < args[0].item_count; i++) {
                        double a = vnumf(args[0].items[i]);
                        double b = vnumf(args[0].items[best]);
                        if ((is_min && a < b) || (!is_min && a > b)) best = i;
                    }
                    vcopy(v, args[0].items[best]);
                } else if (argc >= 2) {
                    int best = 0;
                    for (int i = 1; i < argc; i++) {
                        double a = vnumf(args[i]);
                        double b = vnumf(args[best]);
                        if ((is_min && a < b) || (!is_min && a > b)) best = i;
                    }
                    vcopy(v, args[best]);
                } else v.type = V_NONE;
            } else if (seq(name, "type")) {
                const char* t = "NoneType";
                if (argc >= 1) {
                    switch (args[0].type) {
                        case V_INT: t = "int"; break;
                        case V_FLOAT: t = "float"; break;
                        case V_BOOL: t = "bool"; break;
                        case V_STR: t = "str"; break;
                        case V_LIST: t = "list"; break;
                        case V_FUNC: t = "function"; break;
                        default: t = "NoneType";
                    }
                }
                vset_str(v, t, sl(t));
            } else if (seq(name, "input")) {
                vset_str(v, "", 0);
            } else {
                Value* fn = env_find(&env, name);
                if (fn && fn->type == V_FUNC) {
                    v = eval_call_func(*g_program, env, *fn, args, argc, *p.err);
                } else {
                    perr(p, "name not defined");
                }
            }
            for (int i = 0; i < 6; i++) vfree(args[i]);
        } else {
            Value* found = env_find(&env, name);
            if (found) vcopy(v, *found);
            else perr(p, "name not defined");
        }
        return v;
    }

    if (c == '-') {
        p.pos++;
        Value rhs = eval_atom(p, env);
        if (rhs.type == V_FLOAT) { v.type = V_FLOAT; v.f = -rhs.f; }
        else { v.type = V_INT; v.i = -vnumi(rhs); }
        vfree(rhs);
        return v;
    }
    if (c == '+') { p.pos++; return eval_atom(p, env); }

    perr(p, "unexpected token");
    return v;
}

// postfix (subscript)
static Value eval_postfix(Parser& p, Env& env) {
    Value v = eval_atom(p, env);
    while (!p.failed) {
        pskip(p);
        if (p.pos < p.len && p.src[p.pos] == '[') {
            p.pos++;
            Value idx = eval_expr(p, env);
            pskip(p);
            // slice?
            if (p.pos < p.len && p.src[p.pos] == ':') {
                p.pos++;
                Value stopv; vinit(stopv);
                pskip(p);
                if (!(p.pos < p.len && p.src[p.pos] == ']')) stopv = eval_expr(p, env);
                pskip(p);
                if (p.pos < p.len && p.src[p.pos] == ']') p.pos++;
                long long start = vnumi(idx);
                long long stop;
                if (v.type == V_STR) stop = stopv.type == V_NONE ? v.slen : vnumi(stopv);
                else if (v.type == V_LIST) stop = stopv.type == V_NONE ? v.item_count : vnumi(stopv);
                else stop = 0;
                Value out; vinit(out);
                if (v.type == V_STR) {
                    if (start < 0) start = 0;
                    if (stop > v.slen) stop = v.slen;
                    if (stop < start) stop = start;
                    vset_str(out, v.s + start, (int)(stop - start));
                } else if (v.type == V_LIST) {
                    out.type = V_LIST;
                    out.item_cap = (int)(stop - start) > 0 ? (int)(stop - start) : 1;
                    out.items = alloc_items(out.item_cap);
                    if (start < 0) start = 0;
                    if (stop > v.item_count) stop = v.item_count;
                    for (long long k = start; k < stop; k++) list_push(out, v.items[k]);
                }
                vfree(idx); vfree(stopv); vfree(v);
                v = out;
                continue;
            }
            if (p.pos < p.len && p.src[p.pos] == ']') p.pos++;
            long long k = vnumi(idx);
            Value out; vinit(out);
            if (v.type == V_STR) {
                if (k < 0) k += v.slen;
                if (k >= 0 && k < v.slen) {
                    char tmp[2] = { v.s[k], 0 };
                    vset_str(out, tmp, 1);
                }
            } else if (v.type == V_LIST) {
                if (k < 0) k += v.item_count;
                if (k >= 0 && k < v.item_count) vcopy(out, v.items[k]);
            }
            vfree(idx); vfree(v); v = out;
            continue;
        }
        break;
    }
    return v;
}

static Value eval_pow(Parser& p, Env& env) {
    Value lhs = eval_postfix(p, env);
    while (!p.failed) {
        pskip(p);
        if (p.pos + 1 < p.len && p.src[p.pos] == '*' && p.src[p.pos + 1] == '*') {
            p.pos += 2;
            Value rhs = eval_postfix(p, env);
            double a = vnumf(lhs), b = vnumf(rhs);
            double r = 1; bool neg = b < 0; long long bi = neg ? -(long long)b : (long long)b;
            for (long long k = 0; k < bi; k++) r *= a;
            if (neg) r = 1.0 / r;
            vfree(lhs); vfree(rhs);
            vinit(lhs);
            if (lhs.type == V_FLOAT || rhs.type == V_FLOAT || neg) { lhs.type = V_FLOAT; lhs.f = r; }
            else { lhs.type = V_INT; lhs.i = (long long)r; }
        } else break;
    }
    return lhs;
}

static Value eval_mul(Parser& p, Env& env) {
    Value lhs = eval_pow(p, env);
    while (!p.failed) {
        pskip(p);
        if (p.pos >= p.len) break;
        char c = p.src[p.pos];
        bool floordiv = (p.pos + 1 < p.len && c == '/' && p.src[p.pos + 1] == '/');
        if (floordiv) p.pos += 2;
        else if (c == '*' || c == '/' || c == '%') p.pos++;
        else break;
        Value rhs = eval_pow(p, env);
        Value out; vinit(out);
        bool any_float = (lhs.type == V_FLOAT) || (rhs.type == V_FLOAT);
        if (c == '*') {
            if (lhs.type == V_STR && rhs.type == V_INT) {
                long long n = rhs.i; if (n < 0) n = 0;
                int total = (int)(lhs.slen * n);
                char* buf = (char*)KernelHeap::Alloc((unsigned)(total + 1));
                if (buf) {
                    int o = 0;
                    for (long long k = 0; k < n; k++) for (int j = 0; j < lhs.slen; j++) buf[o++] = lhs.s[j];
                    buf[o] = 0;
                    vset_str(out, buf, total);
                    KernelHeap::Free(buf);
                }
            } else if (any_float) { out.type = V_FLOAT; out.f = vnumf(lhs) * vnumf(rhs); }
            else { out.type = V_INT; out.i = vnumi(lhs) * vnumi(rhs); }
        } else if (c == '/' && !floordiv) {
            double r = vnumf(rhs); if (r == 0) { perr(p, "division by zero"); }
            else { out.type = V_FLOAT; out.f = vnumf(lhs) / r; }
        } else if (floordiv) {
            long long r = vnumi(rhs); if (r == 0) { perr(p, "division by zero"); }
            else { out.type = V_INT; out.i = vnumi(lhs) / r; }
        } else if (c == '%') {
            long long r = vnumi(rhs); if (r == 0) { perr(p, "modulo by zero"); }
            else { out.type = V_INT; out.i = vnumi(lhs) % r; }
        }
        vfree(lhs); vfree(rhs);
        lhs = out;
    }
    return lhs;
}

static Value eval_add(Parser& p, Env& env) {
    Value lhs = eval_mul(p, env);
    while (!p.failed) {
        pskip(p);
        if (p.pos >= p.len) break;
        char c = p.src[p.pos];
        if (c != '+' && c != '-') break;
        p.pos++;
        Value rhs = eval_mul(p, env);
        Value out; vinit(out);
        if (c == '+' && lhs.type == V_STR && rhs.type == V_STR) {
            int total = lhs.slen + rhs.slen;
            char* buf = (char*)KernelHeap::Alloc((unsigned)(total + 1));
            if (buf) {
                for (int i = 0; i < lhs.slen; i++) buf[i] = lhs.s[i];
                for (int i = 0; i < rhs.slen; i++) buf[lhs.slen + i] = rhs.s[i];
                buf[total] = 0;
                vset_str(out, buf, total);
                KernelHeap::Free(buf);
            }
        } else if (c == '+' && lhs.type == V_LIST && rhs.type == V_LIST) {
            out.type = V_LIST; out.item_cap = lhs.item_count + rhs.item_count + 1;
            out.items = alloc_items(out.item_cap);
            for (int i = 0; i < lhs.item_count; i++) list_push(out, lhs.items[i]);
            for (int i = 0; i < rhs.item_count; i++) list_push(out, rhs.items[i]);
        } else {
            bool any_float = (lhs.type == V_FLOAT) || (rhs.type == V_FLOAT);
            if (any_float) {
                out.type = V_FLOAT;
                out.f = c == '+' ? vnumf(lhs) + vnumf(rhs) : vnumf(lhs) - vnumf(rhs);
            } else {
                out.type = V_INT;
                out.i = c == '+' ? vnumi(lhs) + vnumi(rhs) : vnumi(lhs) - vnumi(rhs);
            }
        }
        vfree(lhs); vfree(rhs);
        lhs = out;
    }
    return lhs;
}

static int vcmp(const Value& a, const Value& b) {
    if (a.type == V_STR && b.type == V_STR) {
        int n = a.slen < b.slen ? a.slen : b.slen;
        for (int i = 0; i < n; i++) {
            if (a.s[i] != b.s[i]) return (unsigned char)a.s[i] - (unsigned char)b.s[i];
        }
        return a.slen - b.slen;
    }
    double da = vnumf(a), db = vnumf(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static bool vin(const Value& needle, const Value& hay) {
    if (hay.type == V_LIST) {
        for (int i = 0; i < hay.item_count; i++)
            if (vcmp(needle, hay.items[i]) == 0) return true;
        return false;
    }
    if (hay.type == V_STR && needle.type == V_STR && needle.slen <= hay.slen) {
        for (int i = 0; i + needle.slen <= hay.slen; i++) {
            bool m = true;
            for (int j = 0; j < needle.slen; j++)
                if (hay.s[i + j] != needle.s[j]) { m = false; break; }
            if (m) return true;
        }
    }
    return false;
}

static Value eval_cmp(Parser& p, Env& env) {
    Value lhs = eval_add(p, env);
    while (!p.failed) {
        pskip(p);
        bool not_in = false;
        if (peat(p, "==") || peat(p, "!=") || peat(p, "<=") ||
            peat(p, ">=") || peat(p, "<") || peat(p, ">") ||
            ppeek(p, "in") || (ppeek(p, "not") && (not_in = true))) {
            const char* op = nullptr;
            if (p.pos >= 2 && p.src[p.pos - 2] == '=' && p.src[p.pos - 1] == '=') op = "==";
            else if (p.pos >= 2 && p.src[p.pos - 2] == '!' && p.src[p.pos - 1] == '=') op = "!=";
            else if (p.pos >= 2 && p.src[p.pos - 2] == '<' && p.src[p.pos - 1] == '=') op = "<=";
            else if (p.pos >= 2 && p.src[p.pos - 2] == '>' && p.src[p.pos - 1] == '=') op = ">=";
            else if (p.pos >= 1 && p.src[p.pos - 1] == '<') op = "<";
            else if (p.pos >= 1 && p.src[p.pos - 1] == '>') op = ">";
            else {
                if (not_in) { peat(p, "not"); pskip(p); }
                if (peat(p, "in")) op = not_in ? "ni" : "in";
                else { p.failed = true; break; }
            }
            Value rhs = eval_add(p, env);
            Value out; vinit(out); out.type = V_BOOL;
            if (op[0] == 'i' && op[1] == 'n') out.i = vin(lhs, rhs) ? 1 : 0;
            else if (op[0] == 'n' && op[1] == 'i') out.i = vin(lhs, rhs) ? 0 : 1;
            else {
                int c = vcmp(lhs, rhs);
                if (seq(op, "==")) out.i = c == 0;
                else if (seq(op, "!=")) out.i = c != 0;
                else if (seq(op, "<"))  out.i = c < 0;
                else if (seq(op, ">"))  out.i = c > 0;
                else if (seq(op, "<=")) out.i = c <= 0;
                else if (seq(op, ">=")) out.i = c >= 0;
            }
            vfree(lhs); vfree(rhs);
            lhs = out;
        } else break;
    }
    return lhs;
}

static Value eval_andor(Parser& p, Env& env) {
    Value lhs = eval_cmp(p, env);
    while (!p.failed) {
        pskip(p);
        if (peat(p, "and")) {
            Value rhs = eval_cmp(p, env);
            bool r = vtruthy(lhs) && vtruthy(rhs);
            vfree(lhs); vfree(rhs);
            vinit(lhs); lhs.type = V_BOOL; lhs.i = r ? 1 : 0;
        } else if (peat(p, "or")) {
            Value rhs = eval_cmp(p, env);
            bool r = vtruthy(lhs) || vtruthy(rhs);
            vfree(lhs); vfree(rhs);
            vinit(lhs); lhs.type = V_BOOL; lhs.i = r ? 1 : 0;
        } else break;
    }
    return lhs;
}

static Value eval_expr(Parser& p, Env& env) { return eval_andor(p, env); }

// ---------------- statement execution ----------------
enum ExecResult { EXEC_OK = 0, EXEC_RETURN = 1, EXEC_BREAK = 2, EXEC_CONT = 3, EXEC_ERROR = 4 };

static int find_block_end(Program& prog, int start_idx, int header_indent) {
    int i = start_idx;
    while (i < prog.line_count && prog.lines[i].indent > header_indent) i++;
    return i;
}

static int exec_block(Program& prog, int start, int end, int block_indent, Env& env, Value& retval, Sink& err);

static int exec_line(Program& prog, int idx, int& next_idx, Env& env, Value& retval, Sink& err) {
    Line& ln = prog.lines[idx];
    next_idx = idx + 1;
    Parser p; p.src = ln.text; p.pos = 0; p.len = ln.len; p.err = &err; p.failed = false;
    pskip(p);
    if (p.pos >= p.len) return EXEC_OK;

    if (peat(p, "pass")) return EXEC_OK;
    if (peat(p, "break")) return EXEC_BREAK;
    if (peat(p, "continue")) return EXEC_CONT;
    if (peat(p, "return")) {
        pskip(p);
        if (p.pos < p.len) {
            Value v = eval_expr(p, env);
            vcopy(retval, v);
            vfree(v);
        }
        return EXEC_RETURN;
    }

    if (peat(p, "def")) {
        pskip(p);
        char name[24];
        if (!read_ident(p, name, 24)) { sw(err, "Error: bad def\n"); return EXEC_ERROR; }
        pskip(p);
        if (p.pos >= p.len || p.src[p.pos] != '(') { sw(err, "Error: def missing (\n"); return EXEC_ERROR; }
        p.pos++;
        Value fn; vinit(fn); fn.type = V_FUNC; fn.fparam_count = 0;
        pskip(p);
        if (!(p.pos < p.len && p.src[p.pos] == ')')) {
            while (fn.fparam_count < 6) {
                char pname[24];
                if (!read_ident(p, pname, 24)) break;
                scp(fn.fparams[fn.fparam_count++], pname, 24);
                pskip(p);
                if (p.pos < p.len && p.src[p.pos] == ',') { p.pos++; continue; }
                break;
            }
        }
        pskip(p);
        if (p.pos < p.len && p.src[p.pos] == ')') p.pos++;
        fn.func_indent = ln.indent;
        fn.func_body_start = idx + 1;
        fn.func_body_end = find_block_end(prog, idx + 1, ln.indent);
        env_set(env, name, fn);
        vfree(fn);
        next_idx = fn.func_body_end;
        return EXEC_OK;
    }

    if (peat(p, "if")) {
        Value cond = eval_expr(p, env);
        bool taken = vtruthy(cond);
        vfree(cond);
        int block_end = find_block_end(prog, idx + 1, ln.indent);
        if (taken) {
            int r = exec_block(prog, idx + 1, block_end, ln.indent, env, retval, err);
            // skip elif/else
            int j = block_end;
            while (j < prog.line_count && prog.lines[j].indent == ln.indent) {
                Parser pp; pp.src = prog.lines[j].text; pp.pos = 0; pp.len = prog.lines[j].len;
                pp.err = &err; pp.failed = false;
                pskip(pp);
                if (ppeek(pp, "elif") || ppeek(pp, "else")) {
                    j = find_block_end(prog, j + 1, ln.indent);
                } else break;
            }
            next_idx = j;
            return r;
        } else {
            int j = block_end;
            while (j < prog.line_count && prog.lines[j].indent == ln.indent) {
                Parser pp; pp.src = prog.lines[j].text; pp.pos = 0; pp.len = prog.lines[j].len;
                pp.err = &err; pp.failed = false;
                pskip(pp);
                if (peat(pp, "elif")) {
                    Value c2 = eval_expr(pp, env);
                    bool t2 = vtruthy(c2);
                    vfree(c2);
                    int e2 = find_block_end(prog, j + 1, ln.indent);
                    if (t2) {
                        int r = exec_block(prog, j + 1, e2, ln.indent, env, retval, err);
                        // skip remaining elif/else
                        int k = e2;
                        while (k < prog.line_count && prog.lines[k].indent == ln.indent) {
                            Parser p3; p3.src = prog.lines[k].text; p3.pos = 0; p3.len = prog.lines[k].len;
                            p3.err = &err; p3.failed = false; pskip(p3);
                            if (ppeek(p3, "elif") || ppeek(p3, "else")) k = find_block_end(prog, k + 1, ln.indent);
                            else break;
                        }
                        next_idx = k;
                        return r;
                    }
                    j = e2;
                } else if (peat(pp, "else")) {
                    int e2 = find_block_end(prog, j + 1, ln.indent);
                    int r = exec_block(prog, j + 1, e2, ln.indent, env, retval, err);
                    next_idx = e2;
                    return r;
                } else break;
            }
            next_idx = j;
            return EXEC_OK;
        }
    }

    if (peat(p, "while")) {
        int body_end = find_block_end(prog, idx + 1, ln.indent);
        for (int guard = 0; guard < 1000000; guard++) {
            Parser p2; p2.src = ln.text; p2.pos = 0; p2.len = ln.len;
            p2.err = &err; p2.failed = false; pskip(p2); peat(p2, "while");
            Value c = eval_expr(p2, env);
            bool t = vtruthy(c); vfree(c);
            if (!t) break;
            int r = exec_block(prog, idx + 1, body_end, ln.indent, env, retval, err);
            if (r == EXEC_RETURN || r == EXEC_ERROR) { next_idx = body_end; return r; }
            if (r == EXEC_BREAK) break;
        }
        next_idx = body_end;
        return EXEC_OK;
    }

    if (peat(p, "for")) {
        char var[24];
        if (!read_ident(p, var, 24)) { sw(err, "Error: bad for\n"); return EXEC_ERROR; }
        pskip(p);
        if (!peat(p, "in")) { sw(err, "Error: for missing 'in'\n"); return EXEC_ERROR; }
        Value seq_v = eval_expr(p, env);
        int body_end = find_block_end(prog, idx + 1, ln.indent);
        if (seq_v.type == V_LIST) {
            for (int k = 0; k < seq_v.item_count; k++) {
                env_set(env, var, seq_v.items[k]);
                int r = exec_block(prog, idx + 1, body_end, ln.indent, env, retval, err);
                if (r == EXEC_RETURN || r == EXEC_ERROR) { vfree(seq_v); next_idx = body_end; return r; }
                if (r == EXEC_BREAK) break;
            }
        } else if (seq_v.type == V_STR) {
            for (int k = 0; k < seq_v.slen; k++) {
                Value ch; vinit(ch); char tmp[2] = { seq_v.s[k], 0 };
                vset_str(ch, tmp, 1);
                env_set(env, var, ch);
                vfree(ch);
                int r = exec_block(prog, idx + 1, body_end, ln.indent, env, retval, err);
                if (r == EXEC_RETURN || r == EXEC_ERROR) { vfree(seq_v); next_idx = body_end; return r; }
                if (r == EXEC_BREAK) break;
            }
        }
        vfree(seq_v);
        next_idx = body_end;
        return EXEC_OK;
    }

    // assignment? scan for '=' that is not part of ==, <=, >=, !=
    int eq_pos = -1;
    {
        int depth = 0; bool in_str = false; char qc = 0;
        for (int i = p.pos; i < p.len; i++) {
            char c = ln.text[i];
            if (in_str) { if (c == '\\' && i + 1 < p.len) i++; else if (c == qc) in_str = false; continue; }
            if (c == '"' || c == '\'') { in_str = true; qc = c; continue; }
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
            else if (depth == 0 && c == '=') {
                char prev = i > 0 ? ln.text[i - 1] : 0;
                char next = i + 1 < p.len ? ln.text[i + 1] : 0;
                if (prev == '=' || prev == '<' || prev == '>' || prev == '!' || next == '=') continue;
                eq_pos = i; break;
            }
        }
    }
    if (eq_pos > p.pos) {
        // identifier on left
        int save = p.pos;
        char target[24];
        if (!read_ident(p, target, 24)) { sw(err, "Error: bad assignment target\n"); return EXEC_ERROR; }
        pskip(p);
        if (p.pos != eq_pos) { p.pos = save; }
        else {
            p.pos = eq_pos + 1;
            Value rhs = eval_expr(p, env);
            env_set(env, target, rhs);
            vfree(rhs);
            return EXEC_OK;
        }
    }

    // expression statement
    Value v = eval_expr(p, env);
    vfree(v);
    if (p.failed) return EXEC_ERROR;
    return EXEC_OK;
}

static int exec_block(Program& prog, int start, int end, int block_indent, Env& env, Value& retval, Sink& err) {
    if (start >= end) return EXEC_OK;
    int target_indent = prog.lines[start].indent;
    if (target_indent <= block_indent) target_indent = block_indent + 1;
    int i = start;
    while (i < end) {
        if (prog.lines[i].indent < target_indent) { i++; continue; }
        int next_idx = i + 1;
        int r = exec_line(prog, i, next_idx, env, retval, err);
        if (r != EXEC_OK) return r;
        i = next_idx;
    }
    return EXEC_OK;
}

static Value eval_call_func(Program& prog, Env& outer, const Value& func, Value* args, int argc, Sink& err) {
    Value rv; vinit(rv);
    if (g_recursion >= 32) { sw(err, "Error: recursion too deep\n"); return rv; }
    g_recursion++;
    Env local; env_init(local, &outer);
    int n = func.fparam_count;
    if (argc < n) n = argc;
    for (int i = 0; i < n; i++) env_set(local, func.fparams[i], args[i]);
    exec_block(prog, func.func_body_start, func.func_body_end, func.func_indent, local, rv, err);
    env_clear(local);
    g_recursion--;
    return rv;
}

// ---------------- top-level ----------------
static int run_lines(Line* lines, int line_count, Sink& out) {
    Program prog; prog.lines = lines; prog.line_count = line_count; prog.out = &out;
    g_program = &prog;
    g_recursion = 0;
    Env global; env_init(global, nullptr);
    Value retval; vinit(retval);
    Sink err = out;
    int r = exec_block(prog, 0, line_count, -1, global, retval, out);
    vfree(retval);
    env_clear(global);
    g_program = nullptr;
    (void)err; (void)r;
    return 0;
}

} // namespace

void PythonInterp::Init() { SerialLogger::Log("Python interp ready\r\n"); }

int PythonInterp::RunSource(const char* src, char* out, int max_out) {
    Sink s = { out, max_out, 0, false };
    if (!src) { sw(s, "(empty source)\n"); return s.len; }

    int total = sl(src);
    // count lines
    int line_count = 1;
    for (int i = 0; i < total; i++) if (src[i] == '\n') line_count++;

    Line* lines = (Line*)KernelHeap::Alloc((unsigned)(sizeof(Line) * (line_count + 1)));
    if (!lines) { sw(s, "(out of memory)\n"); return s.len; }

    int li = 0;
    int i = 0;
    while (i <= total) {
        int start = i;
        while (i < total && src[i] != '\n') i++;
        int n = i - start;
        if (!line_blank(src + start, n)) {
            int ind = count_indent(src + start, n);
            int t_start;
            int t_end = trim_line(src + start, n, t_start);
            lines[li].indent = ind;
            lines[li].text = src + start + t_start;
            lines[li].len = t_end - t_start;
            li++;
        }
        if (i >= total) break;
        i++;
    }
    run_lines(lines, li, s);
    KernelHeap::Free(lines);
    return s.len;
}

int PythonInterp::RunFile(const char* vfs_path, char* out, int max_out) {
    if (!KVFS::Exists(vfs_path)) {
        Sink s = { out, max_out, 0, false };
        sw(s, "python: file not found: ");
        sw(s, vfs_path);
        swc(s, '\n');
        return s.len;
    }
    char buf[8192];
    int r = KVFS::ReadFile(vfs_path, buf, sizeof(buf) - 1);
    if (r < 0) r = 0;
    buf[r] = 0;
    return RunSource(buf, out, max_out);
}

int PythonInterp::cmd_python(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        Sink s = { out, mx, 0, false };
        sw(s, "Python 3.12.0 (Kurono mini) on kurono\n");
        sw(s, "Usage: python <file.py>  |  python -c \"<code>\"\n");
        sw(s, "       python -e \"<expr>\"  evaluates and prints expression\n");
        return s.len;
    }
    if (seq(argv[1], "-c") && argc >= 3) {
        // join remaining args with spaces
        char src[4096]; int o = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && o < (int)sizeof(src) - 1) src[o++] = ' ';
            const char* a = argv[i];
            while (*a && o < (int)sizeof(src) - 1) src[o++] = *a++;
        }
        src[o] = 0;
        return PythonInterp::RunSource(src, out, mx);
    }
    if (seq(argv[1], "-e") && argc >= 3) {
        char src[1024]; int o = 0;
        const char* prefix = "print(";
        while (*prefix && o < (int)sizeof(src) - 1) src[o++] = *prefix++;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && o < (int)sizeof(src) - 1) src[o++] = ' ';
            const char* a = argv[i];
            while (*a && o < (int)sizeof(src) - 1) src[o++] = *a++;
        }
        if (o < (int)sizeof(src) - 1) src[o++] = ')';
        src[o] = 0;
        return PythonInterp::RunSource(src, out, mx);
    }
    return PythonInterp::RunFile(argv[1], out, mx);
}

void PythonInterp::RegisterShellCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("python",   "Python 3 interpreter", ENV_KURONO, "lang", reinterpret_cast<ShellCmdHandler>(cmd_python));
    sh->RegisterCommand("python3",  "Python 3 interpreter", ENV_KURONO, "lang", reinterpret_cast<ShellCmdHandler>(cmd_python));
    sh->RegisterCommand("py",       "Python 3 interpreter", ENV_KURONO, "lang", reinterpret_cast<ShellCmdHandler>(cmd_python));
}
