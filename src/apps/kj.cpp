//  kurono os  -  kj (kurono javascript) interpreter implementation. (satoru)
#include "kj.h"
#include "../kernel/heap.h"
#include "../fs/kvfs.h"
#include "../ui/kss.h"
#include "../ui/notification.h"
#include "../shell/shell.h"   // shell command registration (satoru)

namespace {

// ── tiny freestanding helpers (no libc) ──────────────────────────────────────
static int   kjslen(const char* s) { int n=0; while (s && s[n]) n++; return n; }
static bool  kjseq(const char* a, const char* b) {
    if (!a||!b) return false; while (*a&&*b&&*a==*b){a++;b++;} return *a==0&&*b==0;
}
static void  kjcpy(char* d, const char* s, int cap) {
    int i=0; if (s) for (; s[i]&&i<cap-1; i++) d[i]=s[i]; d[i]=0;
}
static bool  is_digit(char c){ return c>='0'&&c<='9'; }
static bool  is_alpha(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static bool  is_alnum(char c){ return is_alpha(c)||is_digit(c); }

// ── number formatting (no libc, js-ish) ──────────────────────────────────────
// integers print without a decimal point; non-integers print with up to 6
// significant fractional digits, trailing zeros trimmed. (satoru)
static void fmt_int(char* buf, long long v) {
    char tmp[24]; int n=0; bool neg=false;
    unsigned long long u;
    if (v<0){ neg=true; u=(unsigned long long)(-(v+1))+1ull; } else u=(unsigned long long)v;
    if (u==0) tmp[n++]='0';
    while (u){ tmp[n++]=(char)('0'+(int)(u%10)); u/=10; }
    int o=0; if (neg) buf[o++]='-';
    while (n) buf[o++]=tmp[--n];
    buf[o]=0;
}
static void fmt_num(char* buf, double d) {
    // NaN / inf guards (we don't produce inf normally). (satoru)
    if (d!=d){ kjcpy(buf,"NaN",16); return; }
    // integer-valued doubles print as integers. (satoru)
    long long i=(long long)d;
    if ((double)i==d && d<9.0e15 && d>-9.0e15){ fmt_int(buf,i); return; }
    bool neg=false; if (d<0){ neg=true; d=-d; }
    long long ip=(long long)d;
    double frac=d-(double)ip;
    char ibuf[24]; fmt_int(ibuf, ip);
    int o=0; if (neg) buf[o++]='-';
    for (int k=0; ibuf[k]; k++) buf[o++]=ibuf[k];
    buf[o++]='.';
    // up to 6 fractional digits. (satoru)
    int digits=6;
    char fbuf[8]; int fn=0;
    for (int k=0;k<digits;k++){ frac*=10.0; int dg=(int)frac; if(dg<0)dg=0; if(dg>9)dg=9; fbuf[fn++]=(char)('0'+dg); frac-=dg; }
    // trim trailing zeros. (satoru)
    while (fn>1 && fbuf[fn-1]=='0') fn--;
    for (int k=0;k<fn;k++) buf[o++]=fbuf[k];
    buf[o]=0;
}

// ── self-contained math for Math.* (no libm) ─────────────────────────────────
static double kj_sqrt(double x){ if(x<=0) return 0; double g=x>1?x:1; for(int i=0;i<40;i++) g=0.5*(g+x/g); return g; }
static double kj_abs(double x){ return x<0?-x:x; }
static double kj_floor(double x){ long long i=(long long)x; if((double)i>x) i--; return (double)i; }
static double kj_ceil(double x){ long long i=(long long)x; if((double)i<x) i++; return (double)i; }
static const double KJ_PI=3.14159265358979323846;
static double kj_sin(double x){
    // range-reduce to [-pi,pi] then 7-term taylor. (satoru)
    while (x> KJ_PI) x-=2*KJ_PI; while (x< -KJ_PI) x+=2*KJ_PI;
    double x2=x*x, t=x, s=x; double sign=-1;
    for (int n=3;n<=15;n+=2){ t*=x2/((double)(n-1)*(double)n); s+=sign*t; sign=-sign; }
    return s;
}
static double kj_cos(double x){ return kj_sin(x+KJ_PI/2.0); }
// tiny xorshift prng for Math.random (deterministic per run; fine for ui). (satoru)
static unsigned long long g_rng=0x9e3779b97f4a7c15ull;
static double kj_random(){ g_rng^=g_rng<<13; g_rng^=g_rng>>7; g_rng^=g_rng<<17; return (double)((g_rng>>11)&0xFFFFFFFFFFFFFull)/(double)0xFFFFFFFFFFFFFull; }

// ════════════════════════════════════════════════════════════════════════════
//  values
// ════════════════════════════════════════════════════════════════════════════
struct Obj;          // heap object (array / object / function), refcounted (satoru)
struct Node;         // ast node
struct Scope;

enum VT { T_UNDEF=0, T_NULL, T_BOOL, T_NUM, T_STR, T_OBJ };
enum OT { O_ARRAY=0, O_OBJECT, O_FUNC };

struct Value {
    int    t;        // VT
    double n;        // T_NUM, T_BOOL (0/1)
    char*  s;        // T_STR, owned
    Obj*   o;        // T_OBJ, refcounted
};

// a key->value entry for objects (and the prng of array indices is implicit). (satoru)
struct Prop { char* key; Value val; };

struct Obj {
    int   kind;      // OT
    int   refs;
    // array storage (O_ARRAY) (satoru)
    Value* items; int n; int cap;
    // object storage (O_OBJECT) (satoru)
    Prop*  props; int np; int pcap;
    // function (O_FUNC) (satoru)
    Node*  fn_body;            // block node
    char** fn_params; int fn_nparams;
    Scope* fn_closure;         // captured defining scope (shared, not owned hard)
    int    fn_builtin;         // >0 -> builtin id, else user fn
};

static void v_init(Value& v){ v.t=T_UNDEF; v.n=0; v.s=nullptr; v.o=nullptr; }
static Value mk_undef(){ Value v; v_init(v); return v; }
static Value mk_null(){ Value v; v_init(v); v.t=T_NULL; return v; }
static Value mk_bool(bool b){ Value v; v_init(v); v.t=T_BOOL; v.n=b?1:0; return v; }
static Value mk_num(double d){ Value v; v_init(v); v.t=T_NUM; v.n=d; return v; }
static Value mk_str(const char* src, int len){
    Value v; v_init(v); v.t=T_STR;
    if (len<0) len=kjslen(src);
    v.s=(char*)KernelHeap::Alloc((unsigned)(len+1));
    if (!v.s){ v.t=T_UNDEF; return v; }
    for (int i=0;i<len;i++) v.s[i]=src[i]; v.s[len]=0;
    return v;
}

static void scope_unref(Scope* s);   // fwd: a function Obj holds a closure-scope ref (satoru)
static void obj_ref(Obj* o){ if (o) o->refs++; }
static void obj_unref(Obj* o);
static void v_free(Value& v){
    if (v.t==T_STR && v.s){ KernelHeap::Free(v.s); v.s=nullptr; }
    else if (v.t==T_OBJ && v.o){ obj_unref(v.o); v.o=nullptr; }
    v.t=T_UNDEF;
}
static void v_copy(Value& dst, const Value& src){
    if (&dst==&src) return;
    Value tmp; v_init(tmp);
    tmp.t=src.t; tmp.n=src.n;
    if (src.t==T_STR) { tmp=mk_str(src.s?src.s:"",-1); }
    else if (src.t==T_OBJ){ tmp.t=T_OBJ; tmp.o=src.o; obj_ref(tmp.o); }
    v_free(dst); dst=tmp;
}

static Obj* obj_new(int kind){
    Obj* o=(Obj*)KernelHeap::Alloc(sizeof(Obj));
    if (!o) return nullptr;
    o->kind=kind; o->refs=1;
    o->items=nullptr; o->n=0; o->cap=0;
    o->props=nullptr; o->np=0; o->pcap=0;
    o->fn_body=nullptr; o->fn_params=nullptr; o->fn_nparams=0; o->fn_closure=nullptr; o->fn_builtin=0;
    return o;
}
static void obj_unref(Obj* o){
    if (!o) return;
    if (--o->refs > 0) return;
    if (o->items){ for (int i=0;i<o->n;i++) v_free(o->items[i]); KernelHeap::Free(o->items); }
    if (o->props){ for (int i=0;i<o->np;i++){ if (o->props[i].key) KernelHeap::Free(o->props[i].key); v_free(o->props[i].val); } KernelHeap::Free(o->props); }
    // fn_params point into ast-owned strings; not freed here. release the captured
    // closure scope ref this function held (claimed in eval N_FUNC). (satoru)
    if (o->kind==O_FUNC && o->fn_closure) scope_unref(o->fn_closure);
    KernelHeap::Free(o);
}

static void arr_push(Obj* a, const Value& v){
    if (!a || a->kind!=O_ARRAY) return;
    if (a->n>=a->cap){
        int nc=a->cap? a->cap*2:4;
        Value* na=(Value*)KernelHeap::Alloc((unsigned)(sizeof(Value)*nc));
        if (!na) return;
        for (int i=0;i<a->n;i++){ na[i]=a->items[i]; }
        for (int i=a->n;i<nc;i++) v_init(na[i]);
        if (a->items) KernelHeap::Free(a->items);
        a->items=na; a->cap=nc;
    }
    v_init(a->items[a->n]); v_copy(a->items[a->n], v); a->n++;
}

static Value* obj_find(Obj* o, const char* key){
    if (!o||o->kind!=O_OBJECT) return nullptr;
    for (int i=0;i<o->np;i++) if (kjseq(o->props[i].key,key)) return &o->props[i].val;
    return nullptr;
}
static void obj_set(Obj* o, const char* key, const Value& val){
    if (!o||o->kind!=O_OBJECT) return;
    Value* ex=obj_find(o,key);
    if (ex){ v_copy(*ex,val); return; }
    if (o->np>=o->pcap){
        int nc=o->pcap? o->pcap*2:4;
        Prop* np=(Prop*)KernelHeap::Alloc((unsigned)(sizeof(Prop)*nc));
        if (!np) return;
        for (int i=0;i<o->np;i++) np[i]=o->props[i];
        for (int i=o->np;i<nc;i++){ np[i].key=nullptr; v_init(np[i].val); }
        if (o->props) KernelHeap::Free(o->props);
        o->props=np; o->pcap=nc;
    }
    int kl=kjslen(key);
    o->props[o->np].key=(char*)KernelHeap::Alloc((unsigned)(kl+1));
    if (!o->props[o->np].key) return;
    kjcpy(o->props[o->np].key,key,kl+1);
    v_init(o->props[o->np].val); v_copy(o->props[o->np].val,val);
    o->np++;
}

// ════════════════════════════════════════════════════════════════════════════
//  lexer
// ════════════════════════════════════════════════════════════════════════════
enum Tk {
    K_EOF=0, K_NUM, K_STR, K_IDENT,
    K_LPAREN,K_RPAREN,K_LBRACE,K_RBRACE,K_LBRACKET,K_RBRACKET,
    K_COMMA,K_SEMI,K_COLON,K_DOT,K_QUESTION,
    K_ASSIGN,K_EQ,K_SEQ,K_NEQ,K_SNEQ,K_LT,K_GT,K_LTE,K_GTE,
    K_PLUS,K_MINUS,K_STAR,K_SLASH,K_PCT,
    K_INC,K_DEC,K_PLUSEQ,K_MINUSEQ,
    K_AND,K_OR,K_NOT,
    // keywords
    K_VAR,K_LET,K_CONST,K_FUNCTION,K_RETURN,K_IF,K_ELSE,
    K_FOR,K_WHILE,K_BREAK,K_CONTINUE,K_TRUE,K_FALSE,K_NULL,K_UNDEFINED,
    K_TYPEOF,K_OF,K_NEW
};

struct Token { int t; double num; char* str; int line; char text[40]; };

static const int TOK_MAX = 8192;

struct Lexer {
    Token* toks; int count; bool overflow;
};

static int kw_of(const char* w){
    struct M{ const char* w; int t; };
    static const M m[]={
        {"var",K_VAR},{"let",K_LET},{"const",K_CONST},{"function",K_FUNCTION},
        {"return",K_RETURN},{"if",K_IF},{"else",K_ELSE},{"for",K_FOR},
        {"while",K_WHILE},{"break",K_BREAK},{"continue",K_CONTINUE},
        {"true",K_TRUE},{"false",K_FALSE},{"null",K_NULL},{"undefined",K_UNDEFINED},
        {"typeof",K_TYPEOF},{"of",K_OF},{"new",K_NEW},
    };
    for (unsigned i=0;i<sizeof(m)/sizeof(m[0]);i++) if (kjseq(m[i].w,w)) return m[i].t;
    return K_IDENT;
}

static void push_tok(Lexer& lx, int t, int line){
    if (lx.count>=TOK_MAX){ lx.overflow=true; return; }
    Token& k=lx.toks[lx.count++]; k.t=t; k.num=0; k.str=nullptr; k.line=line; k.text[0]=0;
}

static bool lex(const char* src, Lexer& lx){
    lx.toks=(Token*)KernelHeap::Alloc(sizeof(Token)*TOK_MAX);
    if (!lx.toks) return false;
    lx.count=0; lx.overflow=false;
    int i=0, line=1;
    while (src[i]){
        char c=src[i];
        if (c=='\n'){ line++; i++; continue; }
        if (c==' '||c=='\t'||c=='\r'){ i++; continue; }
        // comments (satoru)
        if (c=='/'&&src[i+1]=='/'){ while (src[i]&&src[i]!='\n') i++; continue; }
        if (c=='/'&&src[i+1]=='*'){ i+=2; while (src[i]&&!(src[i]=='*'&&src[i+1]=='/')){ if(src[i]=='\n')line++; i++; } if(src[i]) i+=2; continue; }
        // numbers (satoru)
        if (is_digit(c) || (c=='.'&&is_digit(src[i+1]))){
            double val=0; bool isf=false; double fpos=0.1;
            while (is_digit(src[i])){ val=val*10+(src[i]-'0'); i++; }
            if (src[i]=='.'){ isf=true; i++; while (is_digit(src[i])){ val+=(src[i]-'0')*fpos; fpos*=0.1; i++; } }
            (void)isf;
            push_tok(lx,K_NUM,line); lx.toks[lx.count-1].num=val; continue;
        }
        // strings (satoru)
        if (c=='"'||c=='\''){
            char q=c; i++;
            int start=i; int len=0;
            // first pass: measure with escapes (satoru)
            int j=i; while (src[j]&&src[j]!=q){ if (src[j]=='\\'&&src[j+1]) j++; j++; len++; }
            char* buf=(char*)KernelHeap::Alloc((unsigned)(len+1));
            int o=0; j=start;
            while (src[j]&&src[j]!=q){
                char ch=src[j];
                if (ch=='\\'&&src[j+1]){
                    j++; char e=src[j];
                    if (e=='n') ch='\n'; else if (e=='t') ch='\t'; else if (e=='r') ch='\r';
                    else if (e=='\\') ch='\\'; else if (e=='\'') ch='\''; else if (e=='"') ch='"';
                    else if (e=='0') ch=0; else ch=e;
                }
                if (buf) buf[o++]=ch; j++;
            }
            if (buf) buf[o]=0;
            i=j; if (src[i]==q) i++;
            push_tok(lx,K_STR,line); lx.toks[lx.count-1].str=buf; continue;
        }
        // identifiers / keywords (satoru)
        if (is_alpha(c)){
            char w[40]; int n=0;
            while (is_alnum(src[i]) && n<39){ w[n++]=src[i++]; } w[n]=0;
            int t=kw_of(w);
            push_tok(lx,t,line);
            kjcpy(lx.toks[lx.count-1].text,w,40);
            continue;
        }
        // operators / punctuation (satoru)
        int t=-1, adv=1;
        char d=src[i+1];
        switch (c){
            case '(' : t=K_LPAREN; break;
            case ')' : t=K_RPAREN; break;
            case '{' : t=K_LBRACE; break;
            case '}' : t=K_RBRACE; break;
            case '[' : t=K_LBRACKET; break;
            case ']' : t=K_RBRACKET; break;
            case ',' : t=K_COMMA; break;
            case ';' : t=K_SEMI; break;
            case ':' : t=K_COLON; break;
            case '.' : t=K_DOT; break;
            case '?' : t=K_QUESTION; break;
            case '%' : t=K_PCT; break;
            case '*' : t=K_STAR; break;
            case '/' : t=K_SLASH; break;
            case '+' : if (d=='+'){t=K_INC;adv=2;} else if (d=='='){t=K_PLUSEQ;adv=2;} else t=K_PLUS; break;
            case '-' : if (d=='-'){t=K_DEC;adv=2;} else if (d=='='){t=K_MINUSEQ;adv=2;} else t=K_MINUS; break;
            case '=' : if (d=='='&&src[i+2]=='='){t=K_SEQ;adv=3;} else if (d=='='){t=K_EQ;adv=2;} else t=K_ASSIGN; break;
            case '!' : if (d=='='&&src[i+2]=='='){t=K_SNEQ;adv=3;} else if (d=='='){t=K_NEQ;adv=2;} else t=K_NOT; break;
            case '<' : if (d=='='){t=K_LTE;adv=2;} else t=K_LT; break;
            case '>' : if (d=='='){t=K_GTE;adv=2;} else t=K_GT; break;
            case '&' : if (d=='&'){t=K_AND;adv=2;} else { i++; continue; } break;
            case '|' : if (d=='|'){t=K_OR;adv=2;} else { i++; continue; } break;
            default  : i++; continue;   // skip unknown char (satoru)
        }
        push_tok(lx,t,line); i+=adv;
    }
    push_tok(lx,K_EOF,line);
    return !lx.overflow;
}

static void lex_free(Lexer& lx){
    if (!lx.toks) return;
    for (int i=0;i<lx.count;i++) if (lx.toks[i].t==K_STR && lx.toks[i].str) KernelHeap::Free(lx.toks[i].str);
    KernelHeap::Free(lx.toks); lx.toks=nullptr; lx.count=0;
}

// ════════════════════════════════════════════════════════════════════════════
//  ast
// ════════════════════════════════════════════════════════════════════════════
enum NT {
    N_NUM=0, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT,
    N_ARRAY, N_OBJECT,
    N_BINARY, N_UNARY, N_LOGICAL, N_ASSIGN, N_UPDATE, N_TERNARY,
    N_MEMBER,     // a.b  or a[b]
    N_CALL,
    N_FUNC,       // function expr/decl
    N_BLOCK, N_VARDECL, N_IF, N_WHILE, N_FOR, N_FOROF,
    N_RETURN, N_BREAK, N_CONTINUE, N_EXPRSTMT, N_PROGRAM
};

struct Node {
    int t;
    // generic payload
    double num;
    char*  str;        // owned by ast arena
    int    op;         // operator token for binary/unary/assign/update
    bool   prefix;     // for update (++x vs x++)
    bool   computed;   // member: a[b] vs a.b
    int    decl_kind;  // vardecl: K_VAR/K_LET/K_CONST
    // children (variable count) (satoru)
    Node** kids; int nkids; int kcap;
    // function params
    char** params; int nparams;
    int    line;
};

static const int NODE_POOL = 8192;
struct Arena {
    Node*  pool; int used;
    char** strs; int nstr; int strcap;   // owned strings to free at the end (satoru)
};
static Node* node_new(Arena& a, int t){
    if (a.used>=NODE_POOL) return nullptr;
    Node* n=&a.pool[a.used++];
    n->t=t; n->num=0; n->str=nullptr; n->op=0; n->prefix=false; n->computed=false; n->decl_kind=0;
    n->kids=nullptr; n->nkids=0; n->kcap=0; n->params=nullptr; n->nparams=0; n->line=0;
    return n;
}
static void node_add(Node* p, Node* c){
    if (!p) return;
    if (p->nkids>=p->kcap){
        int nc=p->kcap? p->kcap*2:4;
        Node** nk=(Node**)KernelHeap::Alloc((unsigned)(sizeof(Node*)*nc));
        for (int i=0;i<p->nkids;i++) nk[i]=p->kids[i];
        if (p->kids) KernelHeap::Free(p->kids);
        p->kids=nk; p->kcap=nc;
    }
    p->kids[p->nkids++]=c;
}
static char* arena_str(Arena& a, const char* s, int len){
    if (len<0) len=kjslen(s);
    char* buf=(char*)KernelHeap::Alloc((unsigned)(len+1));
    if (!buf) return nullptr;
    for (int i=0;i<len;i++) buf[i]=s[i]; buf[len]=0;
    if (a.nstr>=a.strcap){
        int nc=a.strcap? a.strcap*2:64;
        char** ns=(char**)KernelHeap::Alloc((unsigned)(sizeof(char*)*nc));
        for (int i=0;i<a.nstr;i++) ns[i]=a.strs[i];
        if (a.strs) KernelHeap::Free(a.strs);
        a.strs=ns; a.strcap=nc;
    }
    a.strs[a.nstr++]=buf;
    return buf;
}

// ════════════════════════════════════════════════════════════════════════════
//  parser (recursive descent -> ast)
// ════════════════════════════════════════════════════════════════════════════
struct Parser {
    Token* t; int n; int p;
    Arena* a;
    char err[96]; bool failed;
};
static int  P_tk(Parser& ps){ return ps.p<ps.n? ps.t[ps.p].t : K_EOF; }
static int  P_tk2(Parser& ps){ return ps.p+1<ps.n? ps.t[ps.p+1].t : K_EOF; }
static void P_err(Parser& ps, const char* m){
    if (ps.failed) return;
    int o=0; const char* pre="kj: parse error: ";
    while (pre[o]&&o<90){ ps.err[o]=pre[o]; o++; }
    for (int k=0; m[k]&&o<94; k++) ps.err[o++]=m[k];
    ps.err[o]=0; ps.failed=true;
}
static bool P_eat(Parser& ps, int t){ if (P_tk(ps)==t){ ps.p++; return true; } return false; }
static void P_expect(Parser& ps, int t, const char* m){ if (!P_eat(ps,t)) P_err(ps,m); }

static Node* parse_expr(Parser& ps);
static Node* parse_assign(Parser& ps);
static Node* parse_stmt(Parser& ps);
static Node* parse_block(Parser& ps);

static Node* parse_primary(Parser& ps){
    if (ps.failed) return nullptr;
    int t=P_tk(ps);
    Arena& a=*ps.a;
    if (t==K_NUM){ Node* n=node_new(a,N_NUM); n->num=ps.t[ps.p].num; ps.p++; return n; }
    if (t==K_STR){ Node* n=node_new(a,N_STR); n->str=arena_str(a, ps.t[ps.p].str?ps.t[ps.p].str:"",-1); ps.p++; return n; }
    if (t==K_TRUE){ Node* n=node_new(a,N_BOOL); n->num=1; ps.p++; return n; }
    if (t==K_FALSE){ Node* n=node_new(a,N_BOOL); n->num=0; ps.p++; return n; }
    if (t==K_NULL){ ps.p++; return node_new(a,N_NULL); }
    if (t==K_UNDEFINED){ ps.p++; return node_new(a,N_UNDEF); }
    if (t==K_IDENT){ Node* n=node_new(a,N_IDENT); n->str=arena_str(a,ps.t[ps.p].text,-1); ps.p++; return n; }
    if (t==K_LPAREN){ ps.p++; Node* e=parse_expr(ps); P_expect(ps,K_RPAREN,"expected ')'"); return e; }
    if (t==K_LBRACKET){
        ps.p++; Node* n=node_new(a,N_ARRAY);
        while (P_tk(ps)!=K_RBRACKET && !ps.failed){
            node_add(n, parse_assign(ps));
            if (!P_eat(ps,K_COMMA)) break;
        }
        P_expect(ps,K_RBRACKET,"expected ']'");
        return n;
    }
    if (t==K_LBRACE){
        ps.p++; Node* n=node_new(a,N_OBJECT);
        while (P_tk(ps)!=K_RBRACE && !ps.failed){
            // key: ident or string. (satoru)
            Node* key=node_new(a,N_STR);
            if (P_tk(ps)==K_STR) key->str=arena_str(a,ps.t[ps.p].str?ps.t[ps.p].str:"",-1);
            else if (P_tk(ps)==K_IDENT) key->str=arena_str(a,ps.t[ps.p].text,-1);
            else { P_err(ps,"expected object key"); break; }
            ps.p++;
            P_expect(ps,K_COLON,"expected ':' in object");
            Node* val=parse_assign(ps);
            node_add(n,key); node_add(n,val);
            if (!P_eat(ps,K_COMMA)) break;
        }
        P_expect(ps,K_RBRACE,"expected '}'");
        return n;
    }
    if (t==K_FUNCTION){
        ps.p++;
        Node* n=node_new(a,N_FUNC);
        if (P_tk(ps)==K_IDENT){ n->str=arena_str(a,ps.t[ps.p].text,-1); ps.p++; }   // optional name
        P_expect(ps,K_LPAREN,"expected '(' after function");
        // params (satoru)
        char* tmp[16]; int np=0;
        while (P_tk(ps)!=K_RPAREN && !ps.failed){
            if (P_tk(ps)==K_IDENT && np<16){ tmp[np++]=arena_str(a,ps.t[ps.p].text,-1); ps.p++; }
            if (!P_eat(ps,K_COMMA)) break;
        }
        P_expect(ps,K_RPAREN,"expected ')'");
        n->nparams=np;
        if (np){ n->params=(char**)KernelHeap::Alloc((unsigned)(sizeof(char*)*np)); for (int i=0;i<np;i++) n->params[i]=tmp[i]; }
        n->kids=nullptr;
        Node* body=parse_block(ps);
        node_add(n,body);
        return n;
    }
    if (t==K_NEW){ ps.p++; return parse_primary(ps); }   // `new X(...)` ~ treat as call (satoru)
    P_err(ps,"unexpected token");
    ps.p++;
    return node_new(a,N_UNDEF);
}

// member access + calls (left-associative postfix). (satoru)
static Node* parse_postfix(Parser& ps){
    Node* e=parse_primary(ps);
    Arena& a=*ps.a;
    while (!ps.failed){
        int t=P_tk(ps);
        if (t==K_DOT){
            ps.p++;
            Node* m=node_new(a,N_MEMBER); m->computed=false;
            Node* key=node_new(a,N_STR);
            if (P_tk(ps)==K_IDENT){ key->str=arena_str(a,ps.t[ps.p].text,-1); ps.p++; }
            else P_err(ps,"expected property name");
            node_add(m,e); node_add(m,key); e=m;
        } else if (t==K_LBRACKET){
            ps.p++;
            Node* m=node_new(a,N_MEMBER); m->computed=true;
            Node* idx=parse_expr(ps);
            P_expect(ps,K_RBRACKET,"expected ']'");
            node_add(m,e); node_add(m,idx); e=m;
        } else if (t==K_LPAREN){
            ps.p++;
            Node* c=node_new(a,N_CALL);
            node_add(c,e);   // callee is kid 0 (satoru)
            while (P_tk(ps)!=K_RPAREN && !ps.failed){
                node_add(c, parse_assign(ps));
                if (!P_eat(ps,K_COMMA)) break;
            }
            P_expect(ps,K_RPAREN,"expected ')'");
            e=c;
        } else if (t==K_INC||t==K_DEC){
            Node* u=node_new(a,N_UPDATE); u->op=t; u->prefix=false;
            node_add(u,e); ps.p++; e=u;
        } else break;
    }
    return e;
}

static Node* parse_unary(Parser& ps){
    Arena& a=*ps.a;
    int t=P_tk(ps);
    if (t==K_NOT||t==K_MINUS||t==K_PLUS||t==K_TYPEOF){
        ps.p++; Node* u=node_new(a,N_UNARY); u->op=t;
        node_add(u, parse_unary(ps)); return u;
    }
    if (t==K_INC||t==K_DEC){
        ps.p++; Node* u=node_new(a,N_UPDATE); u->op=t; u->prefix=true;
        node_add(u, parse_unary(ps)); return u;
    }
    return parse_postfix(ps);
}

// precedence-climbing for binary ops. (satoru)
static int binprec(int t){
    switch (t){
        case K_STAR: case K_SLASH: case K_PCT: return 7;
        case K_PLUS: case K_MINUS: return 6;
        case K_LT: case K_GT: case K_LTE: case K_GTE: return 5;
        case K_EQ: case K_NEQ: case K_SEQ: case K_SNEQ: return 4;
        case K_AND: return 3;
        case K_OR: return 2;
        default: return -1;
    }
}
static Node* parse_bin(Parser& ps, int minp){
    Node* left=parse_unary(ps);
    Arena& a=*ps.a;
    while (!ps.failed){
        int t=P_tk(ps); int pr=binprec(t);
        if (pr<minp) break;
        ps.p++;
        Node* right=parse_bin(ps,pr+1);
        Node* b=node_new(a, (t==K_AND||t==K_OR)? N_LOGICAL : N_BINARY);
        b->op=t; node_add(b,left); node_add(b,right); left=b;
    }
    return left;
}

static Node* parse_ternary(Parser& ps){
    Node* c=parse_bin(ps,2);
    if (P_tk(ps)==K_QUESTION && !ps.failed){
        Arena& a=*ps.a; ps.p++;
        Node* tern=node_new(a,N_TERNARY);
        Node* th=parse_assign(ps);
        P_expect(ps,K_COLON,"expected ':' in ternary");
        Node* el=parse_assign(ps);
        node_add(tern,c); node_add(tern,th); node_add(tern,el);
        return tern;
    }
    return c;
}

static Node* parse_assign(Parser& ps){
    Node* left=parse_ternary(ps);
    int t=P_tk(ps);
    if ((t==K_ASSIGN||t==K_PLUSEQ||t==K_MINUSEQ) && !ps.failed){
        Arena& a=*ps.a; ps.p++;
        Node* as=node_new(a,N_ASSIGN); as->op=t;
        Node* right=parse_assign(ps);
        node_add(as,left); node_add(as,right);
        return as;
    }
    return left;
}

static Node* parse_expr(Parser& ps){ return parse_assign(ps); }

static Node* parse_block(Parser& ps){
    Arena& a=*ps.a;
    Node* b=node_new(a,N_BLOCK);
    P_expect(ps,K_LBRACE,"expected '{'");
    while (P_tk(ps)!=K_RBRACE && P_tk(ps)!=K_EOF && !ps.failed)
        node_add(b, parse_stmt(ps));
    P_expect(ps,K_RBRACE,"expected '}'");
    return b;
}

static Node* parse_vardecl(Parser& ps, int kind){
    Arena& a=*ps.a;
    Node* d=node_new(a,N_VARDECL); d->decl_kind=kind;
    // one or more name(=init), comma-separated. (satoru)
    while (!ps.failed){
        if (P_tk(ps)!=K_IDENT){ P_err(ps,"expected name in declaration"); break; }
        Node* name=node_new(a,N_IDENT); name->str=arena_str(a,ps.t[ps.p].text,-1); ps.p++;
        Node* init=nullptr;
        if (P_eat(ps,K_ASSIGN)) init=parse_assign(ps);
        node_add(d,name); node_add(d, init? init : node_new(a,N_UNDEF));
        if (!P_eat(ps,K_COMMA)) break;
    }
    P_eat(ps,K_SEMI);
    return d;
}

static Node* parse_stmt(Parser& ps){
    if (ps.failed) return nullptr;
    Arena& a=*ps.a;
    int t=P_tk(ps);
    if (t==K_LBRACE) return parse_block(ps);
    if (t==K_VAR||t==K_LET||t==K_CONST){ ps.p++; return parse_vardecl(ps,t); }
    if (t==K_FUNCTION){
        // function declaration: parse as func expr, wrap in a vardecl-like binding. (satoru)
        Node* fn=parse_primary(ps);
        Node* d=node_new(a,N_VARDECL); d->decl_kind=K_VAR;
        Node* name=node_new(a,N_IDENT); name->str=fn->str;   // share the function name (satoru)
        node_add(d,name); node_add(d,fn);
        return d;
    }
    if (t==K_IF){
        ps.p++; P_expect(ps,K_LPAREN,"expected '(' after if");
        Node* n=node_new(a,N_IF);
        Node* cond=parse_expr(ps); P_expect(ps,K_RPAREN,"expected ')'");
        Node* then=parse_stmt(ps);
        node_add(n,cond); node_add(n,then);
        if (P_tk(ps)==K_ELSE){ ps.p++; node_add(n, parse_stmt(ps)); }
        return n;
    }
    if (t==K_WHILE){
        ps.p++; P_expect(ps,K_LPAREN,"expected '(' after while");
        Node* n=node_new(a,N_WHILE);
        Node* cond=parse_expr(ps); P_expect(ps,K_RPAREN,"expected ')'");
        node_add(n,cond); node_add(n, parse_stmt(ps));
        return n;
    }
    if (t==K_FOR){
        ps.p++; P_expect(ps,K_LPAREN,"expected '(' after for");
        // detect for-of: `for (let x of expr)` (satoru)
        int save=ps.p;
        int declkind=0;
        if (P_tk(ps)==K_VAR||P_tk(ps)==K_LET||P_tk(ps)==K_CONST){ declkind=P_tk(ps); ps.p++; }
        if (P_tk(ps)==K_IDENT && P_tk2(ps)==K_OF){
            Node* fo=node_new(a,N_FOROF); fo->decl_kind=declkind?declkind:K_LET;
            Node* var=node_new(a,N_IDENT); var->str=arena_str(a,ps.t[ps.p].text,-1); ps.p++;
            ps.p++; // 'of'
            Node* iter=parse_expr(ps);
            P_expect(ps,K_RPAREN,"expected ')'");
            node_add(fo,var); node_add(fo,iter); node_add(fo, parse_stmt(ps));
            return fo;
        }
        // classic for(init;cond;step) (satoru)
        ps.p=save;
        Node* n=node_new(a,N_FOR);
        Node* init=nullptr;
        if (P_tk(ps)==K_SEMI){ ps.p++; init=node_new(a,N_UNDEF); }
        else if (P_tk(ps)==K_VAR||P_tk(ps)==K_LET||P_tk(ps)==K_CONST){ int k=P_tk(ps); ps.p++; init=parse_vardecl(ps,k); }
        else { init=node_new(a,N_EXPRSTMT); node_add(init, parse_expr(ps)); P_eat(ps,K_SEMI); }
        Node* cond = (P_tk(ps)==K_SEMI)? node_new(a,N_BOOL) : parse_expr(ps);
        if (cond->t==N_BOOL) cond->num=1;   // empty cond = true
        P_eat(ps,K_SEMI);
        Node* step = (P_tk(ps)==K_RPAREN)? node_new(a,N_UNDEF) : parse_expr(ps);
        P_expect(ps,K_RPAREN,"expected ')'");
        node_add(n,init); node_add(n,cond); node_add(n,step); node_add(n, parse_stmt(ps));
        return n;
    }
    if (t==K_RETURN){
        ps.p++; Node* n=node_new(a,N_RETURN);
        if (P_tk(ps)!=K_SEMI && P_tk(ps)!=K_RBRACE && P_tk(ps)!=K_EOF) node_add(n, parse_expr(ps));
        P_eat(ps,K_SEMI);
        return n;
    }
    if (t==K_BREAK){ ps.p++; P_eat(ps,K_SEMI); return node_new(a,N_BREAK); }
    if (t==K_CONTINUE){ ps.p++; P_eat(ps,K_SEMI); return node_new(a,N_CONTINUE); }
    // expression statement (satoru)
    Node* es=node_new(a,N_EXPRSTMT);
    node_add(es, parse_expr(ps));
    P_eat(ps,K_SEMI);
    return es;
}

// ════════════════════════════════════════════════════════════════════════════
//  scopes
// ════════════════════════════════════════════════════════════════════════════
struct Binding { char* name; Value val; bool is_const; bool used; };
struct Scope {
    Binding* slots; int n; int cap;
    Scope* parent;
    int refs;        // closures share scopes; refcount frees them safely (satoru)
};
static Scope* scope_new(Scope* parent){
    Scope* s=(Scope*)KernelHeap::Alloc(sizeof(Scope));
    if (!s) return nullptr;
    s->slots=nullptr; s->n=0; s->cap=0; s->parent=parent; s->refs=1;
    if (parent) parent->refs++;
    return s;
}
static void scope_unref(Scope* s){
    if (!s) return;
    if (--s->refs>0) return;
    Scope* p=s->parent;
    if (s->slots){ for (int i=0;i<s->n;i++){ if (s->slots[i].name) KernelHeap::Free(s->slots[i].name); v_free(s->slots[i].val); } KernelHeap::Free(s->slots); }
    KernelHeap::Free(s);
    scope_unref(p);
}
static Binding* scope_find_local(Scope* s, const char* name){
    for (int i=0;i<s->n;i++) if (s->slots[i].used && kjseq(s->slots[i].name,name)) return &s->slots[i];
    return nullptr;
}
static Binding* scope_find(Scope* s, const char* name){
    while (s){ Binding* b=scope_find_local(s,name); if (b) return b; s=s->parent; }
    return nullptr;
}
static void scope_define(Scope* s, const char* name, const Value& v, bool is_const){
    Binding* ex=scope_find_local(s,name);
    if (ex){ v_copy(ex->val,v); ex->is_const=is_const; return; }
    if (s->n>=s->cap){
        int nc=s->cap? s->cap*2:8;
        Binding* nb=(Binding*)KernelHeap::Alloc((unsigned)(sizeof(Binding)*nc));
        for (int i=0;i<s->n;i++) nb[i]=s->slots[i];
        for (int i=s->n;i<nc;i++){ nb[i].name=nullptr; v_init(nb[i].val); nb[i].is_const=false; nb[i].used=false; }
        if (s->slots) KernelHeap::Free(s->slots);
        s->slots=nb; s->cap=nc;
    }
    int kl=kjslen(name);
    s->slots[s->n].name=(char*)KernelHeap::Alloc((unsigned)(kl+1));
    kjcpy(s->slots[s->n].name,name,kl+1);
    v_init(s->slots[s->n].val); v_copy(s->slots[s->n].val,v);
    s->slots[s->n].is_const=is_const; s->slots[s->n].used=true; s->n++;
}

// ════════════════════════════════════════════════════════════════════════════
//  output sink + interpreter context
// ════════════════════════════════════════════════════════════════════════════
struct Sink { char* buf; int cap; int len; };
static void out_str(Sink& s, const char* str){
    if (!str) return;
    for (int i=0; str[i]; i++){ if (s.len<s.cap-1) s.buf[s.len++]=str[i]; }
    s.buf[s.len]=0;
}
static void out_ch(Sink& s, char c){ if (s.len<s.cap-1){ s.buf[s.len++]=c; s.buf[s.len]=0; } }

struct Interp {
    Sink* out;
    Scope* globals;
    bool failed;
    char errbuf[96];
    // control-flow flags (satoru)
    bool ret_flag; Value ret_val;
    bool break_flag; bool continue_flag;
    int  recursion;
    int  loop_iters;       // runaway guard across all loops (satoru)
};
static const int KJ_MAX_RECURSION = 64;
static const int KJ_MAX_ITERS     = 5000000;

static void rt_err(Interp& it, const char* m){
    if (it.failed) return;
    int o=0; const char* pre="kj: ";
    while (pre[o]){ it.errbuf[o]=pre[o]; o++; }
    for (int k=0; m[k]&&o<94; k++) it.errbuf[o++]=m[k];
    it.errbuf[o]=0; it.failed=true;
}
static bool unwind(Interp& it){ return it.failed||it.ret_flag||it.break_flag||it.continue_flag; }

// ── truthiness, equality, stringify ──────────────────────────────────────────
static bool truthy(const Value& v){
    switch (v.t){
        case T_UNDEF: case T_NULL: return false;
        case T_BOOL: return v.n!=0;
        case T_NUM:  return v.n!=0 && v.n==v.n;
        case T_STR:  return v.s && v.s[0]!=0;
        case T_OBJ:  return v.o!=nullptr;
        default: return false;
    }
}
static double to_num(const Value& v){
    switch (v.t){
        case T_NUM: return v.n;
        case T_BOOL: return v.n;
        case T_NULL: return 0;
        case T_STR: { // parse leading number; non-numeric -> 0 (kj keeps it loose) (satoru)
            const char* s=v.s; if (!s) return 0; double sign=1; int i=0;
            while (s[i]==' ') i++; if (s[i]=='-'){sign=-1;i++;} else if (s[i]=='+') i++;
            double val=0; bool any=false; while (is_digit(s[i])){ val=val*10+(s[i]-'0'); i++; any=true; }
            if (s[i]=='.'){ i++; double f=0.1; while (is_digit(s[i])){ val+=(s[i]-'0')*f; f*=0.1; i++; any=true; } }
            return any? sign*val : 0;
        }
        default: return 0;
    }
}
static void val_to_str(const Value& v, Sink& s);   // fwd
static char* val_cstr(const Value& v){
    // render a value to a freshly-heap-allocated c string (caller frees). (satoru)
    char* buf=(char*)KernelHeap::Alloc(256);
    if (!buf) return nullptr;
    Sink tmp; tmp.buf=buf; tmp.cap=256; tmp.len=0; buf[0]=0;
    val_to_str(v,tmp);
    return buf;
}
static void val_to_str(const Value& v, Sink& s){
    char nb[40];
    switch (v.t){
        case T_UNDEF: out_str(s,"undefined"); break;
        case T_NULL:  out_str(s,"null"); break;
        case T_BOOL:  out_str(s, v.n!=0? "true":"false"); break;
        case T_NUM:   fmt_num(nb,v.n); out_str(s,nb); break;
        case T_STR:   out_str(s, v.s?v.s:""); break;
        case T_OBJ:
            if (!v.o){ out_str(s,"null"); break; }
            if (v.o->kind==O_ARRAY){
                out_ch(s,'[');
                for (int i=0;i<v.o->n;i++){ if (i) out_str(s,", ");
                    if (v.o->items[i].t==T_STR){ out_ch(s,'"'); val_to_str(v.o->items[i],s); out_ch(s,'"'); }
                    else val_to_str(v.o->items[i],s);
                }
                out_ch(s,']');
            } else if (v.o->kind==O_OBJECT){
                out_ch(s,'{');
                for (int i=0;i<v.o->np;i++){ if (i) out_str(s,", ");
                    out_str(s, v.o->props[i].key); out_str(s,": ");
                    if (v.o->props[i].val.t==T_STR){ out_ch(s,'"'); val_to_str(v.o->props[i].val,s); out_ch(s,'"'); }
                    else val_to_str(v.o->props[i].val,s);
                }
                out_ch(s,'}');
            } else out_str(s,"[function]");
            break;
        default: out_str(s,"undefined"); break;
    }
}

static bool strict_eq(const Value& a, const Value& b){
    if (a.t!=b.t) return false;
    switch (a.t){
        case T_UNDEF: case T_NULL: return true;
        case T_BOOL: case T_NUM: return a.n==b.n;
        case T_STR: return kjseq(a.s?a.s:"", b.s?b.s:"");
        case T_OBJ: return a.o==b.o;
        default: return false;
    }
}
static bool loose_eq(const Value& a, const Value& b){
    if (a.t==b.t) return strict_eq(a,b);
    if ((a.t==T_NULL||a.t==T_UNDEF)&&(b.t==T_NULL||b.t==T_UNDEF)) return true;
    if (a.t==T_OBJ||b.t==T_OBJ) return false;
    return to_num(a)==to_num(b);
}

// ════════════════════════════════════════════════════════════════════════════
//  evaluator
// ════════════════════════════════════════════════════════════════════════════
static Value eval(Interp& it, Node* n, Scope* sc);
static void  exec(Interp& it, Node* n, Scope* sc);

// builtin call dispatch (console.log / Math.* / kss.* / ui.*). returns true if a
// builtin handled the (object,method,args) shape. (satoru)
static bool call_builtin_member(Interp& it, const char* objname, const char* method,
                                Value* args, int argc, Value& out);
static bool call_builtin_global(Interp& it, const char* name, Value* args, int argc, Value& out, bool& handled);

// resolve a member target into (container, key-string or array index) for r/w. (satoru)
static Value eval_member(Interp& it, Node* n, Scope* sc){
    // namespaced constants: Math.PI / Math.E read directly without a binding. (satoru)
    if (!n->computed && n->kids[0]->t==N_IDENT && kjseq(n->kids[0]->str,"Math")){
        const char* key=n->kids[1]->str;
        if (kjseq(key,"PI")) return mk_num(KJ_PI);
        if (kjseq(key,"E"))  return mk_num(2.718281828459045);
    }
    Value base=eval(it, n->kids[0], sc);
    Value result=mk_undef();
    if (n->computed){
        Value idx=eval(it, n->kids[1], sc);
        if (base.t==T_OBJ && base.o){
            if (base.o->kind==O_ARRAY){
                int i=(int)to_num(idx);
                if (i>=0 && i<base.o->n) v_copy(result, base.o->items[i]);
            } else if (base.o->kind==O_OBJECT){
                char* k=val_cstr(idx);
                Value* p=obj_find(base.o, k?k:"");
                if (p) v_copy(result,*p);
                if (k) KernelHeap::Free(k);
            }
        } else if (base.t==T_STR && base.s){
            int i=(int)to_num(idx);
            int len=kjslen(base.s);
            if (i>=0 && i<len){ char ch[2]={base.s[i],0}; v_free(result); result=mk_str(ch,1); }
        }
        v_free(idx);
    } else {
        const char* key=n->kids[1]->str;
        // builtin properties: .length, .push handled at call sites. (satoru)
        if (base.t==T_OBJ && base.o){
            if (base.o->kind==O_ARRAY && kjseq(key,"length")) result=mk_num((double)base.o->n);
            else if (base.o->kind==O_OBJECT){
                Value* p=obj_find(base.o,key); if (p) v_copy(result,*p);
            }
        } else if (base.t==T_STR && base.s && kjseq(key,"length")){
            result=mk_num((double)kjslen(base.s));
        }
    }
    v_free(base);
    return result;
}

// assign to an lvalue node (ident or member). (satoru)
static void assign_to(Interp& it, Node* lv, const Value& val, Scope* sc){
    if (lv->t==N_IDENT){
        Binding* b=scope_find(sc, lv->str);
        if (b){ if (b->is_const){ rt_err(it,"assignment to const"); return; } v_copy(b->val,val); }
        else scope_define(it.globals, lv->str, val, false);   // implicit global (satoru)
        return;
    }
    if (lv->t==N_MEMBER){
        Value base=eval(it, lv->kids[0], sc);
        if (base.t!=T_OBJ || !base.o){ v_free(base); rt_err(it,"cannot set property of non-object"); return; }
        if (lv->computed){
            Value idx=eval(it, lv->kids[1], sc);
            if (base.o->kind==O_ARRAY){
                int i=(int)to_num(idx);
                if (i>=0){
                    while (base.o->n<=i){ Value u=mk_undef(); arr_push(base.o,u); v_free(u); }
                    v_copy(base.o->items[i], val);
                }
            } else if (base.o->kind==O_OBJECT){
                char* k=val_cstr(idx); if (k){ obj_set(base.o,k,val); KernelHeap::Free(k); }
            }
            v_free(idx);
        } else {
            if (base.o->kind==O_OBJECT) obj_set(base.o, lv->kids[1]->str, val);
        }
        v_free(base);
        return;
    }
    rt_err(it,"invalid assignment target");
}

// numeric/concat binary op. (satoru)
static Value eval_binary(Interp& it, int op, const Value& l, const Value& r){
    (void)it;   // numeric/string ops can't fail here (div-by-zero -> 0) (satoru)
    if (op==K_PLUS){
        if (l.t==T_STR || r.t==T_STR){
            char* ls=val_cstr(l); char* rs=val_cstr(r);
            int ln=kjslen(ls), rn=kjslen(rs);
            char* buf=(char*)KernelHeap::Alloc((unsigned)(ln+rn+1));
            int o=0; for (int i=0;ls&&ls[i];i++) buf[o++]=ls[i]; for (int i=0;rs&&rs[i];i++) buf[o++]=rs[i]; buf[o]=0;
            Value v; v_init(v); v.t=T_STR; v.s=buf;
            if (ls) KernelHeap::Free(ls); if (rs) KernelHeap::Free(rs);
            return v;
        }
        return mk_num(to_num(l)+to_num(r));
    }
    double a=to_num(l), b=to_num(r);
    switch (op){
        case K_MINUS: return mk_num(a-b);
        case K_STAR:  return mk_num(a*b);
        case K_SLASH: return mk_num(b!=0? a/b : 0); // div by zero -> 0 (no inf in freestanding) (satoru)
        case K_PCT:   { long long bb=(long long)b; return mk_num(bb!=0? (double)((long long)a % bb) : 0); }
        case K_LT:  if (l.t==T_STR&&r.t==T_STR){ int c=0; const char*x=l.s,*y=r.s; while(*x&&*y&&*x==*y){x++;y++;} c=(unsigned char)*x-(unsigned char)*y; return mk_bool(c<0); } return mk_bool(a<b);
        case K_GT:  if (l.t==T_STR&&r.t==T_STR){ const char*x=l.s,*y=r.s; while(*x&&*y&&*x==*y){x++;y++;} return mk_bool(((int)(unsigned char)*x-(int)(unsigned char)*y)>0); } return mk_bool(a>b);
        case K_LTE: return mk_bool(a<=b);
        case K_GTE: return mk_bool(a>=b);
        case K_EQ:  return mk_bool(loose_eq(l,r));
        case K_NEQ: return mk_bool(!loose_eq(l,r));
        case K_SEQ: return mk_bool(strict_eq(l,r));
        case K_SNEQ:return mk_bool(!strict_eq(l,r));
        default: return mk_undef();
    }
}

// invoke a function value with already-evaluated args. (satoru)
static Value call_function(Interp& it, Obj* fn, Value* args, int argc){
    if (!fn || fn->kind!=O_FUNC){ rt_err(it,"value is not a function"); return mk_undef(); }
    if (it.recursion>=KJ_MAX_RECURSION){ rt_err(it,"recursion limit"); return mk_undef(); }
    it.recursion++;
    Scope* call_sc=scope_new(fn->fn_closure? fn->fn_closure : it.globals);
    for (int i=0;i<fn->fn_nparams;i++){
        Value a = (i<argc)? args[i] : mk_undef();
        scope_define(call_sc, fn->fn_params[i], a, false);
        if (i>=argc) v_free(a);
    }
    Value result=mk_undef();
    if (fn->fn_body) exec(it, fn->fn_body, call_sc);
    if (it.ret_flag){ v_copy(result, it.ret_val); v_free(it.ret_val); it.ret_flag=false; }
    scope_unref(call_sc);
    it.recursion--;
    return result;
}

static Value eval(Interp& it, Node* n, Scope* sc){
    if (!n || it.failed) return mk_undef();
    switch (n->t){
        case N_NUM:  return mk_num(n->num);
        case N_STR:  return mk_str(n->str?n->str:"",-1);
        case N_BOOL: return mk_bool(n->num!=0);
        case N_NULL: return mk_null();
        case N_UNDEF:return mk_undef();
        case N_IDENT: {
            Binding* b=scope_find(sc, n->str);
            if (b){ Value v; v_init(v); v_copy(v,b->val); return v; }
            // unknown identifier -> undefined (loose, like js global lookups). (satoru)
            return mk_undef();
        }
        case N_ARRAY: {
            Obj* a=obj_new(O_ARRAY);
            for (int i=0;i<n->nkids;i++){ Value e=eval(it,n->kids[i],sc); arr_push(a,e); v_free(e); }
            Value v; v_init(v); v.t=T_OBJ; v.o=a; return v;
        }
        case N_OBJECT: {
            Obj* o=obj_new(O_OBJECT);
            for (int i=0;i+1<n->nkids;i+=2){
                const char* key=n->kids[i]->str;
                Value val=eval(it,n->kids[i+1],sc);
                obj_set(o,key,val); v_free(val);
            }
            Value v; v_init(v); v.t=T_OBJ; v.o=o; return v;
        }
        case N_FUNC: {
            Obj* f=obj_new(O_FUNC);
            f->fn_body = n->nkids? n->kids[n->nkids-1] : nullptr;
            f->fn_params=n->params; f->fn_nparams=n->nparams;
            f->fn_closure=sc; if (sc) sc->refs++;   // capture defining scope (closure) (satoru)
            Value v; v_init(v); v.t=T_OBJ; v.o=f; return v;
        }
        case N_UNARY: {
            if (n->op==K_TYPEOF){
                Value x=eval(it,n->kids[0],sc); const char* ty="undefined";
                switch (x.t){ case T_UNDEF:ty="undefined";break; case T_NULL:ty="object";break;
                    case T_BOOL:ty="boolean";break; case T_NUM:ty="number";break; case T_STR:ty="string";break;
                    case T_OBJ: ty=(x.o&&x.o->kind==O_FUNC)?"function":"object"; break; }
                v_free(x); return mk_str(ty,-1);
            }
            Value x=eval(it,n->kids[0],sc); Value r;
            if (n->op==K_MINUS) r=mk_num(-to_num(x));
            else if (n->op==K_NOT) r=mk_bool(!truthy(x));
            else r=mk_num(to_num(x));   // unary +
            v_free(x); return r;
        }
        case N_UPDATE: {
            Node* tgt=n->kids[0];
            Value cur=eval(it,tgt,sc);
            double old=to_num(cur); v_free(cur);
            double nv = (n->op==K_INC)? old+1 : old-1;
            Value nval=mk_num(nv);
            assign_to(it,tgt,nval,sc);
            v_free(nval);
            return mk_num(n->prefix? nv : old);
        }
        case N_BINARY: {
            Value l=eval(it,n->kids[0],sc); Value r=eval(it,n->kids[1],sc);
            Value out=eval_binary(it,n->op,l,r); v_free(l); v_free(r); return out;
        }
        case N_LOGICAL: {
            Value l=eval(it,n->kids[0],sc);
            if (n->op==K_AND){ if (!truthy(l)) return l; v_free(l); return eval(it,n->kids[1],sc); }
            else { if (truthy(l)) return l; v_free(l); return eval(it,n->kids[1],sc); }
        }
        case N_TERNARY: {
            Value c=eval(it,n->kids[0],sc); bool b=truthy(c); v_free(c);
            return eval(it, b? n->kids[1] : n->kids[2], sc);
        }
        case N_ASSIGN: {
            Value rhs;
            if (n->op==K_ASSIGN) rhs=eval(it,n->kids[1],sc);
            else { // += / -=
                Value cur=eval(it,n->kids[0],sc); Value add=eval(it,n->kids[1],sc);
                rhs=eval_binary(it, n->op==K_PLUSEQ? K_PLUS:K_MINUS, cur, add);
                v_free(cur); v_free(add);
            }
            assign_to(it,n->kids[0],rhs,sc);
            return rhs;   // assignment yields the value (satoru)
        }
        case N_MEMBER: return eval_member(it,n,sc);
        case N_CALL: {
            Node* callee=n->kids[0];
            int argc=n->nkids-1;
            Value* args=nullptr;
            if (argc>0){ args=(Value*)KernelHeap::Alloc((unsigned)(sizeof(Value)*argc));
                for (int i=0;i<argc;i++) args[i]=eval(it,n->kids[i+1],sc); }
            Value out=mk_undef();
            bool done=false;
            // member call: obj.method(...)  -  route builtins + array .push, else user fn. (satoru)
            if (callee->t==N_MEMBER && !callee->computed){
                Node* objn=callee->kids[0];
                const char* method=callee->kids[1]->str;
                // namespaced builtins: console.* / Math.* / kss.* / ui.* (satoru)
                if (objn->t==N_IDENT){
                    const char* on=objn->str;
                    if (kjseq(on,"console")||kjseq(on,"Math")||kjseq(on,"kss")||kjseq(on,"ui")){
                        done=call_builtin_member(it,on,method,args,argc,out);
                    }
                }
                if (!done){
                    // evaluate the receiver; support array.push / .pop, string.* basics. (satoru)
                    Value recv=eval(it,objn,sc);
                    if (recv.t==T_OBJ && recv.o && recv.o->kind==O_ARRAY){
                        if (kjseq(method,"push")){ for (int i=0;i<argc;i++) arr_push(recv.o,args[i]); out=mk_num((double)recv.o->n); done=true; }
                        else if (kjseq(method,"pop")){ if (recv.o->n>0){ v_copy(out, recv.o->items[recv.o->n-1]); v_free(recv.o->items[recv.o->n-1]); recv.o->n--; } done=true; }
                    }
                    if (!done && recv.t==T_OBJ && recv.o && recv.o->kind==O_OBJECT){
                        Value* m=obj_find(recv.o,method);
                        if (m && m->t==T_OBJ && m->o && m->o->kind==O_FUNC){ out=call_function(it,m->o,args,argc); done=true; }
                    }
                    if (!done && recv.t==T_STR){
                        if (kjseq(method,"toUpperCase")||kjseq(method,"toLowerCase")){
                            int len=kjslen(recv.s); char* b=(char*)KernelHeap::Alloc((unsigned)(len+1));
                            bool up=kjseq(method,"toUpperCase");
                            for (int i=0;i<len;i++){ char c=recv.s[i]; if (up&&c>='a'&&c<='z') c-=32; else if (!up&&c>='A'&&c<='Z') c+=32; b[i]=c; } b[len]=0;
                            v_init(out); out.t=T_STR; out.s=b; done=true;
                        }
                    }
                    v_free(recv);
                }
            } else {
                // plain call: f(...)  -  global builtin or user function. (satoru)
                if (callee->t==N_IDENT){
                    bool handled=false;
                    done=call_builtin_global(it, callee->str, args, argc, out, handled);
                    if (handled) done=true;
                }
                if (!done){
                    Value fv=eval(it,callee,sc);
                    if (fv.t==T_OBJ && fv.o && fv.o->kind==O_FUNC){ out=call_function(it,fv.o,args,argc); done=true; }
                    v_free(fv);
                }
            }
            if (!done && !it.failed) rt_err(it,"call of non-function");
            if (args){ for (int i=0;i<argc;i++) v_free(args[i]); KernelHeap::Free(args); }
            return out;
        }
        default: return mk_undef();
    }
}

static void exec_block(Interp& it, Node* n, Scope* sc){
    for (int i=0;i<n->nkids && !unwind(it); i++) exec(it,n->kids[i],sc);
}

static void exec(Interp& it, Node* n, Scope* sc){
    if (!n || it.failed) return;
    switch (n->t){
        case N_PROGRAM:
        case N_BLOCK: {
            Scope* block=scope_new(sc);
            exec_block(it,n,block);
            scope_unref(block);
            break;
        }
        case N_VARDECL: {
            bool isc = (n->decl_kind==K_CONST);
            for (int i=0;i+1<n->nkids;i+=2){
                Value v=eval(it,n->kids[i+1],sc);
                scope_define(sc, n->kids[i]->str, v, isc);
                v_free(v);
            }
            break;
        }
        case N_EXPRSTMT: { Value v=eval(it,n->kids[0],sc); v_free(v); break; }
        case N_IF: {
            Value c=eval(it,n->kids[0],sc); bool b=truthy(c); v_free(c);
            if (b) exec(it,n->kids[1],sc);
            else if (n->nkids>2) exec(it,n->kids[2],sc);
            break;
        }
        case N_WHILE: {
            while (!unwind(it)){
                Value c=eval(it,n->kids[0],sc); bool b=truthy(c); v_free(c);
                if (!b) break;
                exec(it,n->kids[1],sc);
                if (it.break_flag){ it.break_flag=false; break; }
                if (it.continue_flag){ it.continue_flag=false; }
                if (++it.loop_iters>KJ_MAX_ITERS){ rt_err(it,"loop iteration limit"); break; }
            }
            break;
        }
        case N_FOR: {
            Scope* fs=scope_new(sc);
            exec(it,n->kids[0],fs);   // init
            while (!unwind(it)){
                Value c=eval(it,n->kids[1],fs); bool b=truthy(c); v_free(c);
                if (!b) break;
                exec(it,n->kids[3],fs);   // body
                if (it.break_flag){ it.break_flag=false; break; }
                if (it.continue_flag){ it.continue_flag=false; }
                if (it.failed) break;
                Value st=eval(it,n->kids[2],fs); v_free(st);   // step
                if (++it.loop_iters>KJ_MAX_ITERS){ rt_err(it,"loop iteration limit"); break; }
            }
            scope_unref(fs);
            break;
        }
        case N_FOROF: {
            Value iter=eval(it,n->kids[1],sc);
            if (iter.t==T_OBJ && iter.o && iter.o->kind==O_ARRAY){
                int len=iter.o->n;
                for (int i=0;i<len && !unwind(it);i++){
                    Scope* fs=scope_new(sc);
                    scope_define(fs, n->kids[0]->str, iter.o->items[i], n->decl_kind==K_CONST);
                    exec(it,n->kids[2],fs);
                    scope_unref(fs);
                    if (it.break_flag){ it.break_flag=false; break; }
                    if (it.continue_flag){ it.continue_flag=false; }
                    if (++it.loop_iters>KJ_MAX_ITERS){ rt_err(it,"loop iteration limit"); break; }
                }
            } else if (iter.t==T_STR && iter.s){
                int len=kjslen(iter.s);
                for (int i=0;i<len && !unwind(it);i++){
                    Scope* fs=scope_new(sc);
                    char ch[2]={iter.s[i],0}; Value cv=mk_str(ch,1);
                    scope_define(fs, n->kids[0]->str, cv, false); v_free(cv);
                    exec(it,n->kids[2],fs);
                    scope_unref(fs);
                    if (it.break_flag){ it.break_flag=false; break; }
                    if (it.continue_flag){ it.continue_flag=false; }
                }
            }
            v_free(iter);
            break;
        }
        case N_RETURN: {
            Value v = n->nkids? eval(it,n->kids[0],sc) : mk_undef();
            v_free(it.ret_val); it.ret_val=v; it.ret_flag=true;
            break;
        }
        case N_BREAK:    it.break_flag=true; break;
        case N_CONTINUE: it.continue_flag=true; break;
        default: { Value v=eval(it,n,sc); v_free(v); break; }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  host bindings
// ════════════════════════════════════════════════════════════════════════════
// map an ease name/number to KSS::Anim::Ease. (satoru)
static KSS::Anim::Ease ease_of(const Value& v){
    if (v.t==T_STR && v.s){
        if (kjseq(v.s,"linear")) return KSS::Anim::Linear;
        if (kjseq(v.s,"outcubic")||kjseq(v.s,"out-cubic")||kjseq(v.s,"ease-out")) return KSS::Anim::OutCubic;
        if (kjseq(v.s,"inoutquint")||kjseq(v.s,"ease-in-out")) return KSS::Anim::InOutQuint;
        if (kjseq(v.s,"spring")) return KSS::Anim::Spring;
    }
    int i=(int)to_num(v);
    if (i<0||i>3) i=1;
    return (KSS::Anim::Ease)i;
}

static bool call_builtin_global(Interp& it, const char* name, Value* args, int argc, Value& out, bool& handled){
    (void)it;
    handled=false;
    // String() / Number() / Boolean() coercion helpers + parseInt/parseFloat. (satoru)
    if (kjseq(name,"String")){ handled=true; if (argc){ char* c=val_cstr(args[0]); out=mk_str(c?c:"",-1); if (c) KernelHeap::Free(c); } else out=mk_str("",0); return true; }
    if (kjseq(name,"Number")||kjseq(name,"parseFloat")){ handled=true; out=mk_num(argc? to_num(args[0]):0); return true; }
    if (kjseq(name,"parseInt")){ handled=true; out=mk_num(argc? (double)(long long)to_num(args[0]):0); return true; }
    if (kjseq(name,"Boolean")){ handled=true; out=mk_bool(argc? truthy(args[0]):false); return true; }
    return false;
}

static bool call_builtin_member(Interp& it, const char* on, const char* method,
                                 Value* args, int argc, Value& out){
    // console.log / console.error / console.warn -> program output line. (satoru)
    if (kjseq(on,"console")){
        if (kjseq(method,"log")||kjseq(method,"error")||kjseq(method,"warn")||kjseq(method,"info")){
            for (int i=0;i<argc;i++){ if (i) out_ch(*it.out,' '); val_to_str(args[i], *it.out); }
            out_ch(*it.out,'\n');
            out=mk_undef(); return true;
        }
        return false;
    }
    if (kjseq(on,"Math")){
        double a = argc>0? to_num(args[0]) : 0;
        double b = argc>1? to_num(args[1]) : 0;
        if (kjseq(method,"floor")){ out=mk_num(kj_floor(a)); return true; }
        if (kjseq(method,"ceil")){  out=mk_num(kj_ceil(a));  return true; }
        if (kjseq(method,"round")){ out=mk_num(kj_floor(a+0.5)); return true; }
        if (kjseq(method,"abs")){   out=mk_num(kj_abs(a));   return true; }
        if (kjseq(method,"sqrt")){  out=mk_num(kj_sqrt(a));  return true; }
        if (kjseq(method,"sin")){   out=mk_num(kj_sin(a));   return true; }
        if (kjseq(method,"cos")){   out=mk_num(kj_cos(a));   return true; }
        if (kjseq(method,"min")){   double m=a; for (int i=1;i<argc;i++){ double x=to_num(args[i]); if (x<m) m=x; } out=mk_num(argc?m:0); return true; }
        if (kjseq(method,"max")){   double m=a; for (int i=1;i<argc;i++){ double x=to_num(args[i]); if (x>m) m=x; } out=mk_num(argc?m:0); return true; }
        if (kjseq(method,"pow")){   double r=1; int e=(int)b; bool neg=e<0; if (neg) e=-e; for (int i=0;i<e;i++) r*=a; out=mk_num(neg?1.0/r:r); return true; }
        if (kjseq(method,"random")){ out=mk_num(kj_random()); return true; }
        return false;
    }
    if (kjseq(on,"kss")){
        // kss.set(selector, prop, value) (satoru)
        if (kjseq(method,"set") && argc>=3){
            char* sel=val_cstr(args[0]); const char* pn = args[1].t==T_STR? args[1].s : "";
            int rule=KSS::Sheet::FindRule(sel?sel:"");
            if (rule<0) rule=KSS::Sheet::DefineRule(sel?sel:"");
            int p=KSS::Sheet::PropByName(pn);
            if (rule>=0 && p>=0){
                if (p>=KSS::Sheet::P_BG && p<=KSS::Sheet::P_SHADOW)
                    KSS::Sheet::SetColor(rule,(KSS::Sheet::Prop)p,(uint32_t)(long long)to_num(args[2]));
                else
                    KSS::Sheet::SetScalar(rule,(KSS::Sheet::Prop)p,(float)to_num(args[2]));
            }
            if (sel) KernelHeap::Free(sel);
            out=mk_undef(); return true;
        }
        // kss.get(selector, prop) (satoru)
        if (kjseq(method,"get") && argc>=2){
            char* sel=val_cstr(args[0]); const char* pn=args[1].t==T_STR? args[1].s : "";
            int rule=KSS::Sheet::FindRule(sel?sel:""); int p=KSS::Sheet::PropByName(pn);
            if (rule>=0 && p>=0){
                if (p>=KSS::Sheet::P_BG && p<=KSS::Sheet::P_SHADOW) out=mk_num((double)KSS::Sheet::GetColor(rule,(KSS::Sheet::Prop)p));
                else out=mk_num((double)KSS::Sheet::GetScalar(rule,(KSS::Sheet::Prop)p));
            } else out=mk_undef();
            if (sel) KernelHeap::Free(sel);
            return true;
        }
        // kss.transition(selector, prop, ms, ease) (satoru)
        if (kjseq(method,"transition") && argc>=3){
            char* sel=val_cstr(args[0]); const char* pn=args[1].t==T_STR? args[1].s : "";
            int rule=KSS::Sheet::FindRule(sel?sel:""); if (rule<0) rule=KSS::Sheet::DefineRule(sel?sel:"");
            int p=KSS::Sheet::PropByName(pn);
            if (rule>=0 && p>=0) KSS::Sheet::SetTransition(rule,(KSS::Sheet::Prop)p,(uint32_t)to_num(args[2]), argc>3? ease_of(args[3]):KSS::Anim::OutCubic);
            if (sel) KernelHeap::Free(sel);
            out=mk_undef(); return true;
        }
        // kss.keyframes(name, prop, [offsets], [values]) (satoru)
        if (kjseq(method,"keyframes") && argc>=4){
            char* nm=val_cstr(args[0]); const char* pn=args[1].t==T_STR? args[1].s : "";
            int p=KSS::Sheet::PropByName(pn);
            if (p>=0 && args[2].t==T_OBJ && args[2].o && args[2].o->kind==O_ARRAY
                     && args[3].t==T_OBJ && args[3].o && args[3].o->kind==O_ARRAY){
                Obj* offs=args[2].o; Obj* vals=args[3].o;
                int ns=offs->n<vals->n? offs->n:vals->n; if (ns>8) ns=8;
                float fo[8], fv[8];
                for (int i=0;i<ns;i++){ fo[i]=(float)to_num(offs->items[i]); fv[i]=(float)to_num(vals->items[i]); }
                int id=KSS::Sheet::DefineKeyframes(nm?nm:"", (KSS::Sheet::Prop)p, fo, fv, ns);
                out=mk_num((double)id);
            } else out=mk_num(-1);
            if (nm) KernelHeap::Free(nm);
            return true;
        }
        // kss.play(selector, name, ms, loop, ease) (satoru)
        if (kjseq(method,"play") && argc>=3){
            char* sel=val_cstr(args[0]); char* nm=val_cstr(args[1]);
            int rule=KSS::Sheet::FindRule(sel?sel:""); if (rule<0) rule=KSS::Sheet::DefineRule(sel?sel:"");
            bool loop = argc>3? truthy(args[3]) : false;
            bool ok=KSS::Sheet::PlayKeyframes(rule, nm?nm:"", (uint32_t)to_num(args[2]), loop, argc>4? ease_of(args[4]):KSS::Anim::Linear);
            if (sel) KernelHeap::Free(sel); if (nm) KernelHeap::Free(nm);
            out=mk_bool(ok); return true;
        }
        return false;
    }
    if (kjseq(on,"ui")){
        // ui.notify(title, body) (satoru)
        if (kjseq(method,"notify")){
            char* title = argc>0? val_cstr(args[0]) : nullptr;
            char* body  = argc>1? val_cstr(args[1]) : nullptr;
            NotificationManager::Post(title?title:"", body?body:"", 0, 3000);
            if (title) KernelHeap::Free(title); if (body) KernelHeap::Free(body);
            out=mk_undef(); return true;
        }
        return false;
    }
    return false;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════════════
//  public api
// ════════════════════════════════════════════════════════════════════════════
void KJ::Init() {
    // nothing persistent yet; the kss sheet layer is initialized at boot. (satoru)
}

int KJ::Execute(const char* source, char* out, int max_out){
    if (out && max_out>0) out[0]=0;
    if (!source || !out || max_out<=0) return 0;

    Sink sink; sink.buf=out; sink.cap=max_out; sink.len=0; out[0]=0;

    Lexer lx; lx.toks=nullptr; lx.count=0; lx.overflow=false;
    if (!lex(source, lx)){ out_str(sink,"kj: lexer overflow\n"); lex_free(lx); return sink.len; }

    Arena arena;
    arena.pool=(Node*)KernelHeap::Alloc(sizeof(Node)*NODE_POOL);
    arena.used=0; arena.strs=nullptr; arena.nstr=0; arena.strcap=0;
    if (!arena.pool){ out_str(sink,"kj: out of memory\n"); lex_free(lx); return sink.len; }

    Parser ps; ps.t=lx.toks; ps.n=lx.count; ps.p=0; ps.a=&arena; ps.failed=false; ps.err[0]=0;
    Node* prog=node_new(arena,N_PROGRAM);
    while (ps.p<ps.n && ps.t[ps.p].t!=K_EOF && !ps.failed)
        node_add(prog, parse_stmt(ps));

    if (ps.failed){
        out_str(sink, ps.err); out_ch(sink,'\n');
    } else {
        Interp it; it.out=&sink; it.failed=false; it.errbuf[0]=0;
        it.ret_flag=false; v_init(it.ret_val); it.break_flag=false; it.continue_flag=false;
        it.recursion=0; it.loop_iters=0;
        it.globals=scope_new(nullptr);
        exec_block(it, prog, it.globals);
        if (it.failed){ out_str(sink, it.errbuf); out_ch(sink,'\n'); }
        v_free(it.ret_val);
        scope_unref(it.globals);
    }

    // free ast-owned strings + child arrays + function param arrays. (satoru)
    for (int i=0;i<arena.used;i++){ if (arena.pool[i].kids) KernelHeap::Free(arena.pool[i].kids);
                                    if (arena.pool[i].params) KernelHeap::Free(arena.pool[i].params); }
    for (int i=0;i<arena.nstr;i++) if (arena.strs[i]) KernelHeap::Free(arena.strs[i]);
    if (arena.strs) KernelHeap::Free(arena.strs);
    KernelHeap::Free(arena.pool);
    lex_free(lx);
    return sink.len;
}

int KJ::ExecFile(const char* path, char* out, int max_out){
    if (out && max_out>0) out[0]=0;
    if (!path) return 0;
    // read the file from kvfs into a heap buffer, run it. (satoru)
    char* src=(char*)KernelHeap::Alloc(65536);
    if (!src) return 0;
    int n=KVFS::ReadFile(path, (uint8_t*)src, 65535);
    if (n<0) n=0; src[n]=0;
    int r=Execute(src, out, max_out);
    KernelHeap::Free(src);
    return r;
}

// ── shell glue ───────────────────────────────────────────────────────────────
int KJ::cmd_kj(void* sh, int argc, const char** argv, char* out, int mx){
    (void)sh;
    if (argc < 2){
        const char* banner =
            "KJ (Kurono JavaScript)  -  a freestanding JS-subset interpreter.\n"
            "Usage: kj <file.js>            run a script from the filesystem\n"
            "       kj -c \"<code>\"          run inline source\n"
            "Bindings: console.log, kss.set/get/transition/keyframes/play, ui.notify, Math.*\n";
        int o=0; while (banner[o] && o<mx-1){ out[o]=banner[o]; o++; } out[o]=0;
        return o;
    }
    // -c "<code>": join the remaining args with spaces. (satoru)
    if (argv[1][0]=='-' && argv[1][1]=='c' && argv[1][2]==0 && argc>=3){
        char* src=(char*)KernelHeap::Alloc(8192);
        if (!src){ out[0]=0; return 0; }
        int o=0;
        for (int i=2;i<argc;i++){ if (i>2 && o<8191) src[o++]=' ';
            const char* a=argv[i]; while (*a && o<8191) src[o++]=*a++; }
        src[o]=0;
        int r=Execute(src,out,mx);
        KernelHeap::Free(src);
        return r;
    }
    return ExecFile(argv[1], out, mx);
}

void KJ::RegisterShellCommands(void* shell_ptr){
    KuronoShell* sh=(KuronoShell*)shell_ptr;
    sh->RegisterCommand("kj",   "Kurono JavaScript interpreter", ENV_KURONO, "lang", reinterpret_cast<ShellCmdHandler>(cmd_kj));
    sh->RegisterCommand("node", "Kurono JavaScript interpreter", ENV_KURONO, "lang", reinterpret_cast<ShellCmdHandler>(cmd_kj));
}
// end (satoru)
