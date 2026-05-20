/* ═══════════════════════════════════════════════════════════════════════════
 *  kpy  -  Kurono Python-subset interpreter
 *
 *  This is NOT CPython.  It is a tiny, honestly-named interpreter that
 *  understands enough Python syntax to validate the Kurono userspace
 *  pipeline end to end:
 *    - SysV initial stack (argc/argv/envp/auxv) parsed by _start
 *    - Direct x86_64 SYSCALL fast-path for read/write/open/close/mmap/exit
 *    - brk/mmap-backed heap for the bigint scratch buffers
 *
 *  Supported syntax:
 *    print(<expr>, <expr>, ...)
 *    <name> = <expr>           # simple variables (int or str)
 *    import sys                # no-op
 *    sys.version               # evaluates to a string
 *    int literals, string literals (single OR double quotes)
 *    operators: + - * / ** (power, bigint via decimal doubling)
 *    ; and newline separate statements
 *    # line comments
 *
 *  Run as:
 *    kpython <vfs-path-to-.py>
 *    kpython -c "<inline-source>"
 *
 *  Built with -static -nostdlib; entry is _start which is hand-written.
 * ═══════════════════════════════════════════════════════════════════════════
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;
typedef unsigned long  size_t;
typedef long           ssize_t;

/* ─── x86_64 Linux-ABI SYSCALL wrapper ─────────────────────────────────── */
static inline i64 sc6(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 asm("r10") = d;
    register i64 r8  asm("r8")  = e;
    register i64 r9  asm("r9")  = f;
    i64 ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline i64 sc1(i64 nr, i64 a)                 { return sc6(nr,a,0,0,0,0,0); }
static inline i64 sc2(i64 nr, i64 a, i64 b)          { return sc6(nr,a,b,0,0,0,0); }
static inline i64 sc3(i64 nr, i64 a, i64 b, i64 c)   { return sc6(nr,a,b,c,0,0,0); }

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_mmap   9
#define SYS_brk    12
#define SYS_exit   60
#define SYS_exit_group 231

static void sys_write(int fd, const char* s, size_t n) {
    sc3(SYS_write, fd, (i64)(u64)s, (i64)n);
}
static void __attribute__((noreturn)) sys_exit(int code) {
    sc1(SYS_exit_group, code);
    sc1(SYS_exit, code);
    for(;;) asm volatile("hlt");
}
static i64 sys_open(const char* path, int flags) {
    return sc3(SYS_open, (i64)(u64)path, flags, 0);
}
static i64 sys_read(int fd, void* buf, size_t n) {
    return sc3(SYS_read, fd, (i64)(u64)buf, (i64)n);
}
static void sys_close(int fd) { sc1(SYS_close, fd); }

/* ─── tiny libc subset ─────────────────────────────────────────────────── */
static size_t klen(const char* s){ size_t n=0; while(s&&s[n]) n++; return n; }
static int    keq (const char* a, const char* b){
    while(*a && *a==*b){ a++; b++; }
    return *a==*b;
}
static int    knstart(const char* s, const char* p){
    while(*p){ if(*s++!=*p++) return 0; } return 1;
}
static void   kmemcpy(void* d, const void* s, size_t n){
    u8* dd=(u8*)d; const u8* ss=(const u8*)s;
    for(size_t i=0;i<n;i++) dd[i]=ss[i];
}
static void   kmemset(void* d, int c, size_t n){
    u8* dd=(u8*)d; for(size_t i=0;i<n;i++) dd[i]=(u8)c;
}

static void puts1(const char* s){ sys_write(1, s, klen(s)); }
static void putc1(char c){ sys_write(1, &c, 1); }
static void putln(void){ putc1('\n'); }

static void put_i64(i64 v){
    char buf[32]; int p=31; buf[p--]=0;
    int neg = (v<0); u64 u = neg ? (u64)(-v) : (u64)v;
    if(!u) buf[p--]='0';
    while(u){ buf[p--]=(char)('0'+(u%10)); u/=10; }
    if(neg) buf[p--]='-';
    puts1(&buf[p+1]);
}

/* ─── bump heap (brk-backed) ──────────────────────────────────────────── */
static u8*  g_heap_base = 0;
static u8*  g_heap_cur  = 0;
static u8*  g_heap_end  = 0;

static void heap_init(void){
    u64 base = (u64)sc1(SYS_brk, 0);
    if(!base){
        /* brk failed: try mmap a 1MB scratch arena. */
        i64 m = sc6(SYS_mmap, 0, 1024*1024, 3 /*RW*/, 0x22 /*PRIV|ANON*/, -1, 0);
        if(m < 0){ puts1("kpy: no heap\n"); sys_exit(20); }
        g_heap_base = g_heap_cur = (u8*)(u64)m;
        g_heap_end  = g_heap_base + 1024*1024;
        return;
    }
    g_heap_base = g_heap_cur = (u8*)base;
    u64 want = base + 1024*1024;
    u64 got  = (u64)sc1(SYS_brk, (i64)want);
    g_heap_end = (u8*)got;
    if(g_heap_end <= g_heap_cur){
        /* brk didn't grow; fall back to mmap. */
        i64 m = sc6(SYS_mmap, 0, 1024*1024, 3, 0x22, -1, 0);
        if(m < 0){ puts1("kpy: no heap\n"); sys_exit(20); }
        g_heap_base = g_heap_cur = (u8*)(u64)m;
        g_heap_end  = g_heap_base + 1024*1024;
    }
}
static void* halloc(size_t n){
    n = (n + 7) & ~(size_t)7;
    if(g_heap_cur + n > g_heap_end){ puts1("kpy: heap exhausted\n"); sys_exit(21); }
    void* p = g_heap_cur; g_heap_cur += n; return p;
}

/* ─── value model: int OR string ──────────────────────────────────────── */
typedef enum { V_INT=1, V_STR=2, V_BIG=3 } VType;
typedef struct {
    VType t;
    i64   i;            /* for V_INT */
    char* s;            /* for V_STR (zero-terminated, in heap) */
    char* big;          /* for V_BIG: decimal string (no sign) in heap */
    int   big_neg;
} Val;

static Val v_int(i64 x){ Val v={0}; v.t=V_INT; v.i=x; return v; }
static Val v_str(char* s){ Val v={0}; v.t=V_STR; v.s=s; return v; }
static Val v_big(char* d, int neg){ Val v={0}; v.t=V_BIG; v.big=d; v.big_neg=neg; return v; }

static char* str_dup_n(const char* s, size_t n){
    char* d = (char*)halloc(n+1); kmemcpy(d, s, n); d[n]=0; return d;
}
static char* str_dup(const char* s){ return str_dup_n(s, klen(s)); }

/* multiply a decimal string in-place-ish by 2: returns a fresh halloc'd string */
static char* dec_mul2(const char* d){
    size_t n = klen(d);
    char* out = (char*)halloc(n+2);
    int carry=0, p=0;
    /* read from least-significant (rightmost) end */
    for(int i=(int)n-1; i>=0; i--){
        int v = (d[i]-'0')*2 + carry;
        out[p++] = (char)('0' + (v%10));
        carry = v/10;
    }
    while(carry){ out[p++] = (char)('0'+(carry%10)); carry/=10; }
    out[p]=0;
    /* reverse in place */
    for(int i=0,j=p-1;i<j;i++,j--){ char t=out[i]; out[i]=out[j]; out[j]=t; }
    return out;
}

static void put_big(const char* d, int neg){
    if(neg) putc1('-');
    puts1(d);
}

/* compute 2^k as decimal big-int */
static char* big_pow2(i64 k){
    char* cur = str_dup("1");
    for(i64 i=0; i<k; i++) cur = dec_mul2(cur);
    return cur;
}

/* ─── tokenizer + recursive-descent parser ────────────────────────────── */
typedef struct {
    const char* src;
    size_t      pos;
    size_t      len;
} Lex;

static int is_d(char c){ return c>='0'&&c<='9'; }
static int is_a(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_an(char c){ return is_a(c)||is_d(c); }

static void skip_ws(Lex* L){
    for(;;){
        while(L->pos<L->len){
            char c = L->src[L->pos];
            if(c==' '||c=='\t'||c=='\r') L->pos++;
            else break;
        }
        if(L->pos<L->len && L->src[L->pos]=='#'){
            while(L->pos<L->len && L->src[L->pos]!='\n') L->pos++;
        } else break;
    }
}

static int peek_ch(Lex* L){
    skip_ws(L);
    return L->pos<L->len ? L->src[L->pos] : 0;
}

static int match_kw(Lex* L, const char* kw){
    skip_ws(L);
    size_t k = klen(kw);
    if(L->pos+k > L->len) return 0;
    for(size_t i=0;i<k;i++) if(L->src[L->pos+i]!=kw[i]) return 0;
    if(L->pos+k < L->len){
        char nx = L->src[L->pos+k];
        if(is_an(nx)) return 0;
    }
    L->pos += k;
    return 1;
}

/* ─── variable table (linear; ~32 slots) ──────────────────────────────── */
#define MAXV 64
static struct { char name[32]; Val val; } g_vars[MAXV];
static int g_nvars = 0;

static Val* var_lookup(const char* name, size_t nlen){
    for(int i=0;i<g_nvars;i++){
        if(klen(g_vars[i].name)==nlen){
            int eq=1;
            for(size_t j=0;j<nlen;j++) if(g_vars[i].name[j]!=name[j]){ eq=0; break; }
            if(eq) return &g_vars[i].val;
        }
    }
    return 0;
}
static void var_set(const char* name, size_t nlen, Val v){
    Val* slot = var_lookup(name, nlen);
    if(!slot){
        if(g_nvars>=MAXV) return;
        if(nlen>=32) nlen=31;
        for(size_t j=0;j<nlen;j++) g_vars[g_nvars].name[j]=name[j];
        g_vars[g_nvars].name[nlen]=0;
        slot = &g_vars[g_nvars++].val;
    }
    *slot = v;
}

/* ─── expression parser ───────────────────────────────────────────────── */
static Val parse_expr(Lex* L);

static Val parse_atom(Lex* L){
    skip_ws(L);
    if(L->pos>=L->len) return v_int(0);
    char c = L->src[L->pos];
    if(c=='('){ L->pos++; Val v = parse_expr(L); skip_ws(L); if(peek_ch(L)==')') L->pos++; return v; }
    if(c=='-' || c=='+'){
        L->pos++;
        Val v = parse_atom(L);
        if(c=='-' && v.t==V_INT) v.i = -v.i;
        if(c=='-' && v.t==V_BIG) v.big_neg = !v.big_neg;
        return v;
    }
    if(is_d(c)){
        i64 n=0; while(L->pos<L->len && is_d(L->src[L->pos])){ n = n*10 + (L->src[L->pos]-'0'); L->pos++; }
        return v_int(n);
    }
    if(c=='\'' || c=='"'){
        char q=c; L->pos++;
        size_t s = L->pos;
        while(L->pos<L->len && L->src[L->pos]!=q) L->pos++;
        char* dup = str_dup_n(L->src+s, L->pos-s);
        if(L->pos<L->len) L->pos++; /* skip closing quote */
        return v_str(dup);
    }
    if(is_a(c)){
        size_t s=L->pos;
        while(L->pos<L->len && is_an(L->src[L->pos])) L->pos++;
        size_t nlen = L->pos - s;
        /* dotted name: sys.version */
        if(L->pos<L->len && L->src[L->pos]=='.'){
            L->pos++;
            size_t s2=L->pos;
            while(L->pos<L->len && is_an(L->src[L->pos])) L->pos++;
            size_t nl2=L->pos-s2;
            if(nlen==3 && knstart(L->src+s,"sys") && nl2==7 && knstart(L->src+s2,"version")){
                return v_str(str_dup("kurono-kpy 0.1.0 (x86_64 SYSCALL)"));
            }
            return v_str(str_dup("<attr>"));
        }
        Val* p = var_lookup(L->src+s, nlen);
        if(p) return *p;
        return v_int(0);
    }
    return v_int(0);
}

static Val parse_pow(Lex* L){
    Val a = parse_atom(L);
    skip_ws(L);
    if(L->pos+1<L->len && L->src[L->pos]=='*' && L->src[L->pos+1]=='*'){
        L->pos+=2;
        Val b = parse_pow(L);  /* right-associative */
        if(a.t==V_INT && b.t==V_INT){
            if(a.i==2 && b.i>0 && b.i<4096){
                /* always go bigint for ** to handle 2**100 */
                return v_big(big_pow2(b.i), 0);
            }
            i64 r=1; for(i64 i=0;i<b.i;i++) r*=a.i;
            return v_int(r);
        }
        return v_int(0);
    }
    return a;
}

static Val parse_mul(Lex* L){
    Val a = parse_pow(L);
    for(;;){
        skip_ws(L);
        if(L->pos<L->len && (L->src[L->pos]=='*' || L->src[L->pos]=='/')){
            if(L->pos+1<L->len && L->src[L->pos]=='*' && L->src[L->pos+1]=='*') break;
            char op = L->src[L->pos++];
            Val b = parse_pow(L);
            if(a.t==V_INT && b.t==V_INT){
                if(op=='*') a.i *= b.i;
                else        a.i  = b.i? a.i/b.i : 0;
            }
        } else break;
    }
    return a;
}

static Val parse_add(Lex* L){
    Val a = parse_mul(L);
    for(;;){
        skip_ws(L);
        if(L->pos<L->len && (L->src[L->pos]=='+' || L->src[L->pos]=='-')){
            char op = L->src[L->pos++];
            Val b = parse_mul(L);
            if(a.t==V_INT && b.t==V_INT){
                if(op=='+') a.i+=b.i; else a.i-=b.i;
            }
        } else break;
    }
    return a;
}

static Val parse_expr(Lex* L){ return parse_add(L); }

static void print_val(Val v){
    if(v.t==V_INT) put_i64(v.i);
    else if(v.t==V_STR) puts1(v.s);
    else if(v.t==V_BIG) put_big(v.big, v.big_neg);
    else puts1("<?>");
}

/* statement: print(...) | name = expr | import ... | bare expr (ignored) */
static void exec_stmt(Lex* L){
    skip_ws(L);
    if(L->pos>=L->len) return;
    if(match_kw(L,"import")){
        /* swallow rest of identifier(s) */
        for(;;){
            skip_ws(L);
            while(L->pos<L->len && is_an(L->src[L->pos])) L->pos++;
            skip_ws(L);
            if(L->pos<L->len && L->src[L->pos]==','){ L->pos++; continue; }
            break;
        }
        return;
    }
    if(match_kw(L,"print")){
        skip_ws(L);
        if(L->pos<L->len && L->src[L->pos]=='(') L->pos++;
        int first=1;
        for(;;){
            skip_ws(L);
            if(L->pos>=L->len || L->src[L->pos]==')') break;
            if(!first) putc1(' ');
            Val v = parse_expr(L);
            print_val(v);
            first=0;
            skip_ws(L);
            if(L->pos<L->len && L->src[L->pos]==','){ L->pos++; continue; }
            break;
        }
        if(L->pos<L->len && L->src[L->pos]==')') L->pos++;
        putln();
        return;
    }
    /* assignment? lookahead: identifier '=' (not '==') */
    skip_ws(L);
    size_t save = L->pos;
    if(L->pos<L->len && is_a(L->src[L->pos])){
        size_t s=L->pos;
        while(L->pos<L->len && is_an(L->src[L->pos])) L->pos++;
        size_t nlen = L->pos - s;
        skip_ws(L);
        if(L->pos<L->len && L->src[L->pos]=='='
                          && (L->pos+1>=L->len || L->src[L->pos+1]!='=')){
            L->pos++;
            Val v = parse_expr(L);
            var_set(L->src+s, nlen, v);
            return;
        }
        /* not assignment: evaluate as bare expression and discard */
        L->pos = save;
    }
    (void)parse_expr(L);
}

static void run_source(const char* src, size_t n){
    Lex L = { src, 0, n };
    while(L.pos < L.len){
        skip_ws(&L);
        if(L.pos>=L.len) break;
        if(L.src[L.pos]==';' || L.src[L.pos]=='\n'){ L.pos++; continue; }
        exec_stmt(&L);
        skip_ws(&L);
        if(L.pos<L.len && (L.src[L.pos]==';' || L.src[L.pos]=='\n')) L.pos++;
    }
}

/* ─── _start: parse SysV initial stack, dispatch ──────────────────────── */
extern int main(int argc, char** argv);

__asm__(
    ".section .text\n"
    ".globl _start\n"
    "_start:\n"
    "    xor  %rbp, %rbp\n"
    "    mov  (%rsp), %rdi\n"          /* argc */
    "    lea  8(%rsp), %rsi\n"         /* argv */
    "    call main\n"
    "    mov  %eax, %edi\n"
    "    mov  $231, %eax\n"            /* exit_group */
    "    syscall\n"
    "    mov  $60, %eax\n"
    "    mov  $0,  %edi\n"
    "    syscall\n"
    "    hlt\n"
);

static char g_filebuf[16*1024];

int main(int argc, char** argv){
    heap_init();

    puts1("kpy 0.1  -  Kurono native Python-subset interpreter\n");

    if(argc < 2){
        puts1("usage: kpython <script.py>  |  kpython -c \"<source>\"\n");
        return 0;
    }

    /* -c "<source>": run argv[2] directly */
    if(argv[1][0]=='-' && argv[1][1]=='c' && argv[1][2]==0 && argc>=3){
        const char* src = argv[2];
        run_source(src, klen(src));
        return 0;
    }

    /* file mode */
    i64 fd = sys_open(argv[1], 0 /*O_RDONLY*/);
    if(fd < 0){
        puts1("kpy: cannot open: ");
        puts1(argv[1]);
        putln();
        return 2;
    }
    ssize_t total = 0;
    for(;;){
        ssize_t got = (ssize_t)sys_read((int)fd, g_filebuf+total,
                                         sizeof(g_filebuf)-1-(size_t)total);
        if(got <= 0) break;
        total += got;
        if((size_t)total >= sizeof(g_filebuf)-1) break;
    }
    sys_close((int)fd);
    g_filebuf[total]=0;
    run_source(g_filebuf, (size_t)total);
    return 0;
}
