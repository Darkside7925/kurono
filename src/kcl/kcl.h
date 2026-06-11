#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — KCL (Kurono Command Language) Interpreter
//  Lightweight scripting for bare-metal kernel
// ═══════════════════════════════════════════════════════════════════════════

class KuronoShell;

#define KCL_MAX_VARS     128
#define KCL_MAX_FUNCS     32
#define KCL_MAX_STACK     64
#define KCL_MAX_LINE     256
#define KCL_MAX_SCRIPT  8192
#define KCL_MAX_NAME      32
#define KCL_MAX_VALUE    256

// Token types
enum KCLTokenType {
    TOK_NONE = 0, TOK_NUMBER, TOK_STRING, TOK_IDENT, TOK_OPERATOR,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMICOLON, TOK_ASSIGN, TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT,
    TOK_LTE, TOK_GTE, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_MOD,
    TOK_AND, TOK_OR, TOK_NOT, TOK_DOT,
    // Keywords
    TOK_SET, TOK_PRINT, TOK_IF, TOK_THEN, TOK_ELSE, TOK_END, TOK_WHILE,
    TOK_FOR, TOK_IN, TOK_DO, TOK_FUNC, TOK_RETURN, TOK_EXEC, TOK_IMPORT,
    TOK_BREAK, TOK_CONTINUE, TOK_TRUE, TOK_FALSE, TOK_NULL, TOK_LET,
    TOK_EOF
};

struct KCLToken {
    KCLTokenType type;
    char text[KCL_MAX_VALUE];
    int int_val;
};

struct KCLVar {
    char name[KCL_MAX_NAME];
    char value[KCL_MAX_VALUE];
    bool is_number;
    int  num_value;
};

struct KCLFunc {
    char name[KCL_MAX_NAME];
    int  body_start;     // Script offset
    int  body_end;       // Script offset
    char params[8][KCL_MAX_NAME];
    int  param_count;
};

class KCL {
public:
    static void Init(KuronoShell* shell);

    // Execute a KCL script string
    static int Execute(const char* script, char* output, int max_output);

    // Execute a single line
    static int ExecLine(const char* line, char* output, int max_output);

    // Execute from file
    static int ExecFile(const char* path, char* output, int max_output);

    // Variable access
    static void SetVar(const char* name, const char* value);
    static void SetNumVar(const char* name, int value);
    static const char* GetVar(const char* name);
    static int  GetNumVar(const char* name);

private:
    static KuronoShell* shell;
    static KCLVar vars[KCL_MAX_VARS];
    static int var_count;
    static KCLFunc funcs[KCL_MAX_FUNCS];
    static int func_count;
    static int loop_depth;
    static bool break_flag;
    static bool continue_flag;

    // Lexer
    static void Tokenize(const char* src, KCLToken* tokens, int* count, int max_tokens);
    static KCLTokenType MatchKeyword(const char* word);

    // Evaluator
    static int EvalExpr(KCLToken* tokens, int* pos, int count);
    static int EvalTerm(KCLToken* tokens, int* pos, int count);
    static int EvalFactor(KCLToken* tokens, int* pos, int count);
    static int EvalCompare(KCLToken* tokens, int* pos, int count);
    static int EvalLogical(KCLToken* tokens, int* pos, int count);

    // Exec internals
    static int ExecTokens(KCLToken* tokens, int count, char* out, int maxo, int* pos);
    static int ExecSet(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecPrint(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecIf(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecWhile(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecFor(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecFunc(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecExec(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecImport(KCLToken* tokens, int* pos, int count, char* out, int maxo);
    static int ExecCall(const char* fname, int args[], int argc, char* out, int maxo);
};
