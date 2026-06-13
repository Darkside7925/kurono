#pragma once
//  kurono os  -  kcl (kurono command language) interpreter
//  a complete tree-walking scripting language for bare-metal kurono:
//  lexer + recursive-descent parser/evaluator over a tagged value type
//  (int / float / string / bool / list / none). no libc, no stl. (satoru)

class KuronoShell;

// hard limits  -  fixed buffers keep us off the stl and bound recursion (satoru)
#define KCL_MAX_SCRIPT     65536   // largest script source we will load (satoru)
#define KCL_MAX_TOKENS     8192    // token stream cap per script (satoru)
#define KCL_MAX_OUTPUT     8192    // default output buffer for shell glue (satoru)
#define KCL_MAX_NAME       48      // identifier length (satoru)
#define KCL_MAX_PARAMS     8       // params per func (satoru)
#define KCL_ENV_SLOTS      96      // variables/funcs per scope (satoru)
#define KCL_MAX_RECURSION  20      // call-depth guard. each kcl call nests a
                                   // chain of ~2-3kb c++ eval/exec frames, and
                                   // the kernel boot stack is only 64kb, so keep
                                   // this conservative to avoid a stack overflow
                                   // (env scopes themselves are heap-allocated). (satoru)
#define KCL_MAX_LOOP_ITERS 1000000 // runaway-loop guard (satoru)
#define KCL_MAX_IMPORTS    16      // import-recursion guard (satoru)

// token kinds produced by the lexer (satoru)
enum KCLTok {
    KT_EOF = 0,
    KT_NUMBER, KT_FLOAT, KT_STRING, KT_IDENT,
    KT_LPAREN, KT_RPAREN, KT_LBRACKET, KT_RBRACKET,
    KT_COMMA, KT_SEMI, KT_NEWLINE,
    KT_ASSIGN, KT_EQ, KT_NEQ, KT_LT, KT_GT, KT_LTE, KT_GTE,
    KT_PLUS, KT_MINUS, KT_STAR, KT_SLASH, KT_PERCENT, KT_DOTDOT, KT_DOT,
    // keywords (satoru)
    KT_SET, KT_PRINT, KT_IF, KT_THEN, KT_ELSE, KT_ELIF, KT_END,
    KT_WHILE, KT_FOR, KT_IN, KT_DO, KT_FUNC, KT_RETURN, KT_IMPORT,
    KT_BREAK, KT_CONTINUE, KT_TRUE, KT_FALSE, KT_NONE,
    KT_AND, KT_OR, KT_NOT
};

struct KCLToken {
    KCLTok type;
    int    line;         // 1-based source line for error messages (satoru)
    long long ival;      // for KT_NUMBER (satoru)
    double    fval;      // for KT_FLOAT (satoru)
    char text[KCL_MAX_NAME]; // for KT_IDENT / short tokens (satoru)
    char* str;           // heap-owned for KT_STRING (may be long) (satoru)
};

class KCL {
public:
    static void Init(KuronoShell* shell);

    // run a script source string. returns bytes written to output. errors are
    // captured into output (never crash the os). (satoru)
    static int Execute(const char* source, char* output, int max_output);
    // run a script stored in the kvfs. (satoru)
    static int ExecFile(const char* path, char* output, int max_output);
    // run a single line (alias of execute). (satoru)
    static int ExecLine(const char* line, char* output, int max_output);

    static KuronoShell* GetShell() { return shell; }

private:
    static KuronoShell* shell;
};
// end (satoru)
