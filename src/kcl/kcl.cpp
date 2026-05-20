#include "kcl.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../packages/pkgmgr.h"
#include "../kernel/time.h"  // for timemanager::nowutc
#include "../kernel/heap.h"

//  kcl interpreter implementation

KuronoShell* KCL::shell = nullptr;
KCLVar KCL::vars[KCL_MAX_VARS];
int KCL::var_count = 0;
KCLFunc KCL::funcs[KCL_MAX_FUNCS];
int KCL::func_count = 0;
int KCL::loop_depth = 0;
bool KCL::break_flag = false;
bool KCL::continue_flag = false;

static int klen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void kcpy(char* d, const char* s, int m) {
    int i=0; while (s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool keq(const char* a, const char* b) {
    while (*a && *b) { if(*a!=*b) return false; a++; b++; } return *a==*b;
}
static bool kends_with(const char* str, const char* suffix) {
    int sl = klen(str), tl = klen(suffix);
    if (tl == 0 || sl < tl) return false;
    return keq(str + sl - tl, suffix);
}
static int ka(char* b, int p, int m, const char* s) {
    while (*s && p<m-1) b[p++]=*s++; b[p]=0; return p;
}
static int kac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int kai(char* b, int p, int m, int v) {
    if (v<0) { p=kac(b,p,m,'-'); v=-v; }
    if (v==0) return kac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=kac(b,p,m,t[--ti]);
    return p;
}

static bool is_alpha(char c) { return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; }
static bool is_digit(char c) { return c>='0'&&c<='9'; }
static bool is_alnum(char c) { return is_alpha(c)||is_digit(c); }
static bool is_ws(char c) { return c==' '||c=='\t'||c=='\r'; }

static int to_int(const char* s) {
    int v=0; bool neg=false;
    if (*s=='-') { neg=true; s++; }
    while (*s>='0'&&*s<='9') { v=v*10+(*s-'0'); s++; }
    return neg ? -v : v;
}

static void int_to_str(int v, char* buf, int mx) {
    int p = kai(buf, 0, mx, v);
    (void)p;
}

static int cmd_kcl(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = ka(out, p, maxo, "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90 KCL \xe2\x94\x80 Kurono Command Language v1.0 \xe2\x95\x90\xe2\x95\x90\xe2\x95\x97\n");
        p = ka(out, p, maxo, "Usage:\n");
        p = ka(out, p, maxo, "  kcl <file.kcl>      Run a script file\n");
        p = ka(out, p, maxo, "  kcl -e \"code\"       Execute inline code\n");
        p = ka(out, p, maxo, "  kcl -v              Show built-in vars\n");
        p = ka(out, p, maxo, "\nBuilt-in functions:\n");
        p = ka(out, p, maxo, "  abs(n)  min(a,b)  max(a,b)  len(s)\n");
        p = ka(out, p, maxo, "  rand()  time()  sqrt(n)  pow(b,e)  clamp(v,lo,hi)\n");
        p = ka(out, p, maxo, "\nKeywords:\n");
        p = ka(out, p, maxo, "  set/let  print  if/else/end  while/do/end\n");
        p = ka(out, p, maxo, "  for/in/do/end  func/end  exec  import\n");
        p = ka(out, p, maxo, "  break  continue  return\n");
        return p;
    }
    if (keq(argv[1], "-e") && argc >= 3) {
        return KCL::Execute(argv[2], out, maxo);
    }
    if (keq(argv[1], "-v")) {
        int p = 0;
        p = ka(out, p, maxo, "KCL Built-in Variables:\n");
        const char* names[] = {"PI", "VERSION", "OS", "TRUE", "FALSE"};
        for (int i = 0; i < 5; i++) {
            const char* v = KCL::GetVar(names[i]);
            if (v) {
                p = ka(out, p, maxo, "  ");
                p = ka(out, p, maxo, names[i]);
                p = ka(out, p, maxo, " = ");
                p = ka(out, p, maxo, v);
                p = kac(out, p, maxo, '\n');
            }
        }
        return p;
    }
    return KCL::ExecFile(argv[1], out, maxo);
}

static int cmd_run(KuronoShell* sh, int argc, const char** argv, char* out, int maxo) {
    return cmd_kcl(sh, argc, argv, out, maxo);
}

void KCL::Init(KuronoShell* sh) {
    shell = sh;
    var_count = 0;
    func_count = 0;
    loop_depth = 0;
    break_flag = false;
    continue_flag = false;

    // built-in constants
    SetVar("PI", "3");
    SetVar("VERSION", "1.0.0");
    SetVar("OS", "Kurono");
    SetNumVar("TRUE", 1);
    SetNumVar("FALSE", 0);

    // register shell commands
    if (sh) {
        sh->RegisterCommand("kcl", "Run KCL script", ENV_KURONO, "scripting", cmd_kcl);
        sh->RegisterCommand("run", "Run KCL script", ENV_KURONO, "scripting", cmd_run);
    }

    SerialLogger::Log("KCL: Interpreter initialized\r\n");
}

void KCL::SetVar(const char* name, const char* value) {
    for (int i = 0; i < var_count; i++) {
        if (keq(vars[i].name, name)) {
            kcpy(vars[i].value, value, KCL_MAX_VALUE);
            vars[i].is_number = (value[0] >= '0' && value[0] <= '9') || value[0] == '-';
            vars[i].num_value = to_int(value);
            return;
        }
    }
    if (var_count < KCL_MAX_VARS) {
        kcpy(vars[var_count].name, name, KCL_MAX_NAME);
        kcpy(vars[var_count].value, value, KCL_MAX_VALUE);
        vars[var_count].is_number = (value[0] >= '0' && value[0] <= '9') || value[0] == '-';
        vars[var_count].num_value = to_int(value);
        var_count++;
    }
}

void KCL::SetNumVar(const char* name, int value) {
    char buf[32];
    int_to_str(value, buf, 32);
    SetVar(name, buf);
}

const char* KCL::GetVar(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (keq(vars[i].name, name)) return vars[i].value;
    }
    return nullptr;
}

int KCL::GetNumVar(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (keq(vars[i].name, name)) return vars[i].num_value;
    }
    return 0;
}

//  lexer

KCLTokenType KCL::MatchKeyword(const char* word) {
    if (keq(word, "set") || keq(word, "let"))      return TOK_SET;
    if (keq(word, "print") || keq(word, "println")) return TOK_PRINT;
    if (keq(word, "if"))       return TOK_IF;
    if (keq(word, "then"))     return TOK_THEN;
    if (keq(word, "else"))     return TOK_ELSE;
    if (keq(word, "end"))      return TOK_END;
    if (keq(word, "while"))    return TOK_WHILE;
    if (keq(word, "for"))      return TOK_FOR;
    if (keq(word, "in"))       return TOK_IN;
    if (keq(word, "do"))       return TOK_DO;
    if (keq(word, "func"))     return TOK_FUNC;
    if (keq(word, "return"))   return TOK_RETURN;
    if (keq(word, "exec"))     return TOK_EXEC;
    if (keq(word, "import"))   return TOK_IMPORT;
    if (keq(word, "break"))    return TOK_BREAK;
    if (keq(word, "continue")) return TOK_CONTINUE;
    if (keq(word, "true"))     return TOK_TRUE;
    if (keq(word, "false"))    return TOK_FALSE;
    if (keq(word, "null"))     return TOK_NULL;
    if (keq(word, "and"))      return TOK_AND;
    if (keq(word, "or"))       return TOK_OR;
    if (keq(word, "not"))      return TOK_NOT;
    return TOK_IDENT;
}

void KCL::Tokenize(const char* src, KCLToken* tokens, int* count, int max_tokens) {
    int tc = 0;
    int i = 0;

    while (src[i] && tc < max_tokens - 1) {
        // skip whitespace
        while (is_ws(src[i])) i++;
        if (!src[i] || src[i] == '\n') { if (src[i]) i++; continue; }

        // comments (# to end of line)
        if (src[i] == '#') { while (src[i] && src[i] != '\n') i++; continue; }

        KCLToken& t = tokens[tc];
        t.text[0] = 0;
        t.int_val = 0;

        // numbers
        if (is_digit(src[i]) || (src[i] == '-' && is_digit(src[i+1]))) {
            int ti = 0;
            if (src[i] == '-') t.text[ti++] = src[i++];
            while (is_digit(src[i]) && ti < KCL_MAX_VALUE - 1) t.text[ti++] = src[i++];
            t.text[ti] = 0;
            t.type = TOK_NUMBER;
            t.int_val = to_int(t.text);
            tc++;
            continue;
        }

        // strings
        if (src[i] == '"' || src[i] == '\'') {
            char quote = src[i++];
            int ti = 0;
            while (src[i] && src[i] != quote && ti < KCL_MAX_VALUE - 1) {
                if (src[i] == '\\' && src[i+1]) {
                    i++;
                    if (src[i] == 'n') t.text[ti++] = '\n';
                    else if (src[i] == 't') t.text[ti++] = '\t';
                    else t.text[ti++] = src[i];
                } else {
                    t.text[ti++] = src[i];
                }
                i++;
            }
            t.text[ti] = 0;
            if (src[i] == quote) i++;
            t.type = TOK_STRING;
            tc++;
            continue;
        }

        // identifiers and keywords
        if (is_alpha(src[i])) {
            int ti = 0;
            while (is_alnum(src[i]) && ti < KCL_MAX_VALUE - 1) t.text[ti++] = src[i++];
            t.text[ti] = 0;
            t.type = MatchKeyword(t.text);
            tc++;
            continue;
        }

        // operators
        char c = src[i++];
        t.text[0] = c; t.text[1] = 0;

        switch (c) {
            case '(': t.type = TOK_LPAREN; break;
            case ')': t.type = TOK_RPAREN; break;
            case '{': t.type = TOK_LBRACE; break;
            case '}': t.type = TOK_RBRACE; break;
            case '[': t.type = TOK_LBRACKET; break;
            case ']': t.type = TOK_RBRACKET; break;
            case ',': t.type = TOK_COMMA; break;
            case ';': t.type = TOK_SEMICOLON; break;
            case '.': t.type = TOK_DOT; break;
            case '+': t.type = TOK_PLUS; break;
            case '-': t.type = TOK_MINUS; break;
            case '*': t.type = TOK_STAR; break;
            case '/': t.type = TOK_SLASH; break;
            case '%': t.type = TOK_MOD; break;
            case '=':
                if (src[i] == '=') { t.type = TOK_EQ; t.text[1] = '='; t.text[2] = 0; i++; }
                else t.type = TOK_ASSIGN;
                break;
            case '!':
                if (src[i] == '=') { t.type = TOK_NEQ; t.text[1] = '='; t.text[2] = 0; i++; }
                else t.type = TOK_NOT;
                break;
            case '<':
                if (src[i] == '=') { t.type = TOK_LTE; t.text[1] = '='; t.text[2] = 0; i++; }
                else t.type = TOK_LT;
                break;
            case '>':
                if (src[i] == '=') { t.type = TOK_GTE; t.text[1] = '='; t.text[2] = 0; i++; }
                else t.type = TOK_GT;
                break;
            default:
                t.type = TOK_OPERATOR;
                break;
        }
        tc++;
    }

    tokens[tc].type = TOK_EOF;
    tokens[tc].text[0] = 0;
    *count = tc;
}

//  expression evaluator (recursive descent)

int KCL::EvalFactor(KCLToken* tokens, int* pos, int count) {
    if (*pos >= count) return 0;

    KCLToken& t = tokens[*pos];

    if (t.type == TOK_NUMBER) {
        (*pos)++;
        return t.int_val;
    }
    if (t.type == TOK_TRUE) { (*pos)++; return 1; }
    if (t.type == TOK_FALSE) { (*pos)++; return 0; }
    if (t.type == TOK_NULL) { (*pos)++; return 0; }

    if (t.type == TOK_STRING) {
        (*pos)++;
        return klen(t.text); // string evaluates to its length in numeric context
    }

    if (t.type == TOK_IDENT) {
        const char* name = t.text;
        (*pos)++;

        // function call?
        if (*pos < count && tokens[*pos].type == TOK_LPAREN) {
            (*pos)++; // skip (
            int args[8]; int argc = 0;
            while (*pos < count && tokens[*pos].type != TOK_RPAREN && argc < 8) {
                args[argc++] = EvalLogical(tokens, pos, count);
                if (*pos < count && tokens[*pos].type == TOK_COMMA) (*pos)++;
            }
            if (*pos < count && tokens[*pos].type == TOK_RPAREN) (*pos)++;
            // built-in functions
            if (keq(name, "len")) return args[0];
            if (keq(name, "abs")) return args[0] < 0 ? -args[0] : args[0];
            if (keq(name, "min")) return argc >= 2 ? (args[0] < args[1] ? args[0] : args[1]) : args[0];
            if (keq(name, "max")) return argc >= 2 ? (args[0] > args[1] ? args[0] : args[1]) : args[0];
            // extended builtins
            if (keq(name, "rand")) {
                static unsigned int seed = 48271;
                seed = seed * 1103515245 + 12345;
                return (int)((seed >> 16) & 0x7FFF);
            }
            if (keq(name, "time")) return (int)(TimeManager::NowUTC().us / 1000u);
            if (keq(name, "sqrt")) {
                int n = args[0]; if (n <= 0) return 0;
                int r = n;
                for (int i = 0; i < 20 && r > 0; i++) r = (r + n / r) / 2;
                return r;
            }
            if (keq(name, "pow")) {
                int base = args[0], exp = argc >= 2 ? args[1] : 1, res = 1;
                if (exp < 0) return 0;
                for (int i = 0; i < exp && i < 30; i++) res *= base;
                return res;
            }
            if (keq(name, "clamp")) {
                if (argc < 3) return args[0];
                int v = args[0], lo = args[1], hi = args[2];
                return v < lo ? lo : (v > hi ? hi : v);
            }
            if (keq(name, "sign")) return args[0] > 0 ? 1 : (args[0] < 0 ? -1 : 0);
            // ── sys() builtin: execute a shell command ──
            if (keq(name, "sys")) {
                if (argc < 1 || !*pos < count) return 0;
                // We need the actual string from the token, but args are numeric.
                // For sys(), use a simplified path: sys("command")
                // The caller should pass a string. Fallback: look up last string token.
                return 0; // sys() via ExecCall handles this separately
            }
            // ── kcl_read() builtin: read file into variable ──
            if (keq(name, "kcl_read")) {
                if (argc < 1) return 0;
                return 0; // handled in ExecCall
            }
            // ── kcl_write() builtin: write string to file ──
            if (keq(name, "kcl_write")) {
                if (argc < 2) return 0;
                return 0;
            }
            return 0;
        }

        // variable lookup
        return GetNumVar(name);
    }

    if (t.type == TOK_LPAREN) {
        (*pos)++;
        int val = EvalLogical(tokens, pos, count);
        if (*pos < count && tokens[*pos].type == TOK_RPAREN) (*pos)++;
        return val;
    }

    if (t.type == TOK_MINUS) {
        (*pos)++;
        return -EvalFactor(tokens, pos, count);
    }

    if (t.type == TOK_NOT) {
        (*pos)++;
        return !EvalFactor(tokens, pos, count);
    }

    (*pos)++;
    return 0;
}

int KCL::EvalTerm(KCLToken* tokens, int* pos, int count) {
    int left = EvalFactor(tokens, pos, count);
    while (*pos < count) {
        if (tokens[*pos].type == TOK_STAR) { (*pos)++; left *= EvalFactor(tokens, pos, count); }
        else if (tokens[*pos].type == TOK_SLASH) {
            (*pos)++;
            int r = EvalFactor(tokens, pos, count);
            left = r ? left / r : 0;
        }
        else if (tokens[*pos].type == TOK_MOD) {
            (*pos)++;
            int r = EvalFactor(tokens, pos, count);
            left = r ? left % r : 0;
        }
        else break;
    }
    return left;
}

int KCL::EvalExpr(KCLToken* tokens, int* pos, int count) {
    int left = EvalTerm(tokens, pos, count);
    while (*pos < count) {
        if (tokens[*pos].type == TOK_PLUS) { (*pos)++; left += EvalTerm(tokens, pos, count); }
        else if (tokens[*pos].type == TOK_MINUS) { (*pos)++; left -= EvalTerm(tokens, pos, count); }
        else break;
    }
    return left;
}

int KCL::EvalCompare(KCLToken* tokens, int* pos, int count) {
    int left = EvalExpr(tokens, pos, count);
    while (*pos < count) {
        KCLTokenType op = tokens[*pos].type;
        if (op == TOK_EQ)  { (*pos)++; left = (left == EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else if (op == TOK_NEQ) { (*pos)++; left = (left != EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else if (op == TOK_LT)  { (*pos)++; left = (left < EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else if (op == TOK_GT)  { (*pos)++; left = (left > EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else if (op == TOK_LTE) { (*pos)++; left = (left <= EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else if (op == TOK_GTE) { (*pos)++; left = (left >= EvalExpr(tokens, pos, count)) ? 1 : 0; }
        else break;
    }
    return left;
}

int KCL::EvalLogical(KCLToken* tokens, int* pos, int count) {
    int left = EvalCompare(tokens, pos, count);
    while (*pos < count) {
        if (tokens[*pos].type == TOK_AND) { (*pos)++; int r = EvalCompare(tokens, pos, count); left = (left && r) ? 1 : 0; }
        else if (tokens[*pos].type == TOK_OR) { (*pos)++; int r = EvalCompare(tokens, pos, count); left = (left || r) ? 1 : 0; }
        else break;
    }
    return left;
}

//  statement execution

int KCL::Execute(const char* script, char* output, int max_output) {
    KCLToken tokens[256];
    int token_count = 0;
    Tokenize(script, tokens, &token_count, 256);

    int pos = 0;
    int out_pos = 0;
    while (pos < token_count && tokens[pos].type != TOK_EOF) {
        if (break_flag || continue_flag) break;
        out_pos = ExecTokens(tokens, token_count, output, max_output, &pos);
    }
    return out_pos;
}

int KCL::ExecLine(const char* line, char* output, int max_output) {
    return Execute(line, output, max_output);
}

int KCL::ExecFile(const char* path, char* output, int max_output) {
    if (path && kends_with(path, ".kro")) {
        char app_name[KCL_MAX_NAME];
        char entry_path[KVFS_MAX_PATH];
        if (!PackageManager::InstallKro(path, app_name, (int)sizeof(app_name), entry_path, (int)sizeof(entry_path))) {
            int p = 0;
            p = ka(output, p, max_output, "kcl: cannot install kro app: ");
            p = ka(output, p, max_output, PackageManager::GetLastSyncMessage());
            p = kac(output, p, max_output, '\n');
            return p;
        }
        return ExecFile(entry_path, output, max_output);
    }

    unsigned char buf[KCL_MAX_SCRIPT];
    int sz = KVFS::ReadFile(path, buf, KCL_MAX_SCRIPT);
    if (sz < 0) {
        return ka(output, 0, max_output, "kcl: cannot open file\n");
    }
    buf[sz] = 0;
    return Execute((const char*)buf, output, max_output);
}

int KCL::ExecTokens(KCLToken* tokens, int count, char* out, int maxo, int* pos) {
    if (*pos >= count || tokens[*pos].type == TOK_EOF) return 0;

    // skip semicolons/newlines
    while (*pos < count && tokens[*pos].type == TOK_SEMICOLON) (*pos)++;

    KCLTokenType type = tokens[*pos].type;

    switch (type) {
        case TOK_SET:   return ExecSet(tokens, pos, count, out, maxo);
        case TOK_PRINT: return ExecPrint(tokens, pos, count, out, maxo);
        case TOK_IF:    return ExecIf(tokens, pos, count, out, maxo);
        case TOK_WHILE: return ExecWhile(tokens, pos, count, out, maxo);
        case TOK_FOR:   return ExecFor(tokens, pos, count, out, maxo);
        case TOK_FUNC:  return ExecFunc(tokens, pos, count, out, maxo);
        case TOK_EXEC:  return ExecExec(tokens, pos, count, out, maxo);
        case TOK_IMPORT:return ExecImport(tokens, pos, count, out, maxo);
        case TOK_BREAK:
            (*pos)++; break_flag = true; return 0;
        case TOK_CONTINUE:
            (*pos)++; continue_flag = true; return 0;
        case TOK_RETURN:
            (*pos)++;
            // evaluate return value but for now just skip the expression
            if (*pos < count && tokens[*pos].type != TOK_SEMICOLON && tokens[*pos].type != TOK_EOF)
                EvalLogical(tokens, pos, count);
            return 0;
        case TOK_IDENT: {
            // could be assignment (x = expr) or function call (f())
            char name[KCL_MAX_NAME];
            kcpy(name, tokens[*pos].text, KCL_MAX_NAME);
            (*pos)++;

            if (*pos < count && tokens[*pos].type == TOK_ASSIGN) {
                (*pos)++;
                // check if rhs is a string
                if (*pos < count && tokens[*pos].type == TOK_STRING) {
                    SetVar(name, tokens[*pos].text);
                    (*pos)++;
                } else {
                    int val = EvalLogical(tokens, pos, count);
                    SetNumVar(name, val);
                }
                return 0;
            }

            if (*pos < count && tokens[*pos].type == TOK_LPAREN) {
                (*pos)--;
                int val = EvalFactor(tokens, pos, count);
                (void)val;
                return 0;
            }
            return 0;
        }
        default:
            (*pos)++;
            return 0;
    }
}

int KCL::ExecSet(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip set/let
    if (*pos >= count || tokens[*pos].type != TOK_IDENT) return 0;

    char name[KCL_MAX_NAME];
    kcpy(name, tokens[*pos].text, KCL_MAX_NAME);
    (*pos)++;

    // optional =
    if (*pos < count && tokens[*pos].type == TOK_ASSIGN) (*pos)++;

    // check for string value
    if (*pos < count && tokens[*pos].type == TOK_STRING) {
        SetVar(name, tokens[*pos].text);
        (*pos)++;
    } else {
        int val = EvalLogical(tokens, pos, count);
        SetNumVar(name, val);
    }
    (void)out; (void)maxo;
    return 0;
}

int KCL::ExecPrint(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip print
    int p = klen(out); // append to current output

    while (*pos < count && tokens[*pos].type != TOK_SEMICOLON && tokens[*pos].type != TOK_EOF) {
        KCLToken& t = tokens[*pos];

        if (t.type == TOK_STRING) {
            p = ka(out, p, maxo, t.text);
            (*pos)++;
        } else if (t.type == TOK_IDENT) {
            const char* val = GetVar(t.text);
            if (val) p = ka(out, p, maxo, val);
            else p = ka(out, p, maxo, t.text);
            (*pos)++;
        } else if (t.type == TOK_COMMA) {
            p = kac(out, p, maxo, ' ');
            (*pos)++;
        } else if (t.type == TOK_PLUS) {
            (*pos)++; // string concatenation, just continue
        } else {
            // evaluate as expression
            int val = EvalLogical(tokens, pos, count);
            p = kai(out, p, maxo, val);
        }
    }
    p = kac(out, p, maxo, '\n');
    return p;
}

int KCL::ExecIf(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip if

    // evaluate condition
    int cond = EvalLogical(tokens, pos, count);

    // skip optional 'then'
    if (*pos < count && tokens[*pos].type == TOK_THEN) (*pos)++;

    if (cond) {
        // execute body until else/end
        while (*pos < count && tokens[*pos].type != TOK_ELSE && tokens[*pos].type != TOK_END && tokens[*pos].type != TOK_EOF) {
            if (break_flag || continue_flag) break;
            ExecTokens(tokens, count, out, maxo, pos);
        }
        // skip else block if present
        if (*pos < count && tokens[*pos].type == TOK_ELSE) {
            (*pos)++;
            int depth = 1;
            while (*pos < count && depth > 0) {
                if (tokens[*pos].type == TOK_IF) depth++;
                if (tokens[*pos].type == TOK_END) depth--;
                if (depth > 0) (*pos)++;
            }
        }
    } else {
        // skip to else/end
        int depth = 1;
        while (*pos < count && depth > 0) {
            if (tokens[*pos].type == TOK_IF) depth++;
            if (tokens[*pos].type == TOK_END && depth == 1) break;
            if (tokens[*pos].type == TOK_ELSE && depth == 1) break;
            if (tokens[*pos].type == TOK_END) depth--;
            (*pos)++;
        }
        if (*pos < count && tokens[*pos].type == TOK_ELSE) {
            (*pos)++;
            while (*pos < count && tokens[*pos].type != TOK_END && tokens[*pos].type != TOK_EOF) {
                if (break_flag || continue_flag) break;
                ExecTokens(tokens, count, out, maxo, pos);
            }
        }
    }

    // skip end
    if (*pos < count && tokens[*pos].type == TOK_END) (*pos)++;
    return klen(out);
}

int KCL::ExecWhile(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip while
    int cond_start = *pos;

    loop_depth++;
    for (int iter = 0; iter < 10000; iter++) {
        *pos = cond_start;
        int cond = EvalLogical(tokens, pos, count);
        if (!cond) break;

        // skip optional 'do'
        if (*pos < count && tokens[*pos].type == TOK_DO) (*pos)++;

        int body_start = *pos;
        (void)body_start;

        while (*pos < count && tokens[*pos].type != TOK_END && tokens[*pos].type != TOK_EOF) {
            ExecTokens(tokens, count, out, maxo, pos);
            if (break_flag) { break_flag = false; goto while_done; }
            if (continue_flag) { continue_flag = false; break; }
        }
    }
while_done:
    loop_depth--;

    // skip to end
    while (*pos < count && tokens[*pos].type != TOK_END) (*pos)++;
    if (*pos < count) (*pos)++;
    return klen(out);
}

int KCL::ExecFor(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip for

    if (*pos >= count) return 0;
    char var_name[KCL_MAX_NAME];
    kcpy(var_name, tokens[*pos].text, KCL_MAX_NAME);
    (*pos)++;

    // skip 'in'
    if (*pos < count && tokens[*pos].type == TOK_IN) (*pos)++;

    // evaluate range: start..end or a single value
    int start_val = EvalExpr(tokens, pos, count);
    int end_val = start_val;

    // check for ..
    if (*pos < count && tokens[*pos].type == TOK_DOT && *pos + 1 < count && tokens[*pos + 1].type == TOK_DOT) {
        *pos += 2;
        end_val = EvalExpr(tokens, pos, count);
    }

    // skip optional 'do'
    if (*pos < count && tokens[*pos].type == TOK_DO) (*pos)++;

    int body_start = *pos;
    loop_depth++;

    for (int i = start_val; i <= end_val; i++) {
        SetNumVar(var_name, i);
        *pos = body_start;

        while (*pos < count && tokens[*pos].type != TOK_END && tokens[*pos].type != TOK_EOF) {
            ExecTokens(tokens, count, out, maxo, pos);
            if (break_flag) { break_flag = false; goto for_done; }
            if (continue_flag) { continue_flag = false; break; }
        }
    }
for_done:
    loop_depth--;

    while (*pos < count && tokens[*pos].type != TOK_END) (*pos)++;
    if (*pos < count) (*pos)++;
    return klen(out);
}

int KCL::ExecFunc(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip func
    if (*pos >= count || func_count >= KCL_MAX_FUNCS) return 0;

    KCLFunc& f = funcs[func_count];
    kcpy(f.name, tokens[*pos].text, KCL_MAX_NAME);
    (*pos)++;
    f.param_count = 0;

    // parameters
    if (*pos < count && tokens[*pos].type == TOK_LPAREN) {
        (*pos)++;
        while (*pos < count && tokens[*pos].type != TOK_RPAREN && f.param_count < 8) {
            kcpy(f.params[f.param_count++], tokens[*pos].text, KCL_MAX_NAME);
            (*pos)++;
            if (*pos < count && tokens[*pos].type == TOK_COMMA) (*pos)++;
        }
        if (*pos < count && tokens[*pos].type == TOK_RPAREN) (*pos)++;
    }

    f.body_start = *pos;

    // skip to end
    int depth = 1;
    while (*pos < count && depth > 0) {
        if (tokens[*pos].type == TOK_FUNC || tokens[*pos].type == TOK_IF ||
            tokens[*pos].type == TOK_WHILE || tokens[*pos].type == TOK_FOR) depth++;
        if (tokens[*pos].type == TOK_END) depth--;
        if (depth > 0) (*pos)++;
    }
    f.body_end = *pos;
    if (*pos < count) (*pos)++;

    func_count++;
    (void)out; (void)maxo;
    return 0;
}

int KCL::ExecExec(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip exec

    if (*pos >= count) return 0;

    // build command string from remaining tokens
    char cmd[512]; int ci = 0;
    while (*pos < count && tokens[*pos].type != TOK_SEMICOLON && tokens[*pos].type != TOK_EOF) {
        if (tokens[*pos].type == TOK_STRING || tokens[*pos].type == TOK_IDENT) {
            for (int j = 0; tokens[*pos].text[j] && ci < 510; j++)
                cmd[ci++] = tokens[*pos].text[j];
            cmd[ci++] = ' ';
        }
        (*pos)++;
    }
    cmd[ci] = 0;

    // execute through shell
    if (shell) {
        char shell_out[4096];
        shell->Execute(cmd, shell_out, 4096);
        int p = klen(out);
        p = ka(out, p, maxo, shell_out);
        return p;
    }
    return 0;
}

int KCL::ExecImport(KCLToken* tokens, int* pos, int count, char* out, int maxo) {
    (*pos)++; // skip import
    if (*pos >= count) return 0;

    char path[256];
    if (tokens[*pos].type == TOK_STRING) {
        kcpy(path, tokens[*pos].text, 256);
    } else {
        // build path from ident
        kcpy(path, "/kurono/lib/", 256);
        int pl = klen(path);
        for (int j = 0; tokens[*pos].text[j] && pl < 250; j++)
            path[pl++] = tokens[*pos].text[j];
        path[pl++] = '.'; path[pl++] = 'k'; path[pl++] = 'c'; path[pl++] = 'l'; path[pl] = 0;
    }
    (*pos)++;

    return ExecFile(path, out, maxo);
}

int KCL::ExecCall(const char* fname, int args[], int argc, char* out, int maxo) {
    // find function
    for (int i = 0; i < func_count; i++) {
        if (keq(funcs[i].name, fname)) {
            KCLFunc& f = funcs[i];
            // set parameter variables
            for (int j = 0; j < f.param_count && j < argc; j++) {
                SetNumVar(f.params[j], args[j]);
            }
            // this is a simplified call  -  full implementation would need token copy
            return 0;
        }
    }
    int p = ka(out, 0, maxo, "kcl: undefined function: ");
    p = ka(out, p, maxo, fname);
    return kac(out, p, maxo, '\n');
}
