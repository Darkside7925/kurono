#ifndef KCL_INTERPRETER_H
#define KCL_INTERPRETER_H

#include "kernel.h"
#include <stdbool.h>

typedef enum {
    KCL_TOKEN_COMMAND,
    KCL_TOKEN_ARGUMENT,
    KCL_TOKEN_PIPE,
    KCL_TOKEN_REDIRECT,
    KCL_TOKEN_VARIABLE,
    KCL_TOKEN_STRING,
    KCL_TOKEN_NUMBER,
    KCL_TOKEN_OPERATOR,
    KCL_TOKEN_EOF,
    KCL_TOKEN_UNKNOWN
} KCLTokenType;

typedef struct {
    KCLTokenType type;
    char* value;
    int line;
    int column;
} KCLToken;

typedef struct {
    KCLToken* tokens;
    size_t count;
    size_t capacity;
    size_t current;
} KCLLexer;

typedef enum {
    KCL_NODE_COMMAND,
    KCL_NODE_PIPELINE,
    KCL_NODE_REDIRECTION,
    KCL_NODE_VARIABLE,
    KCL_NODE_LITERAL
} KCLNodeType;

typedef struct KCLNode {
    KCLNodeType type;
    char* value;
    struct KCLNode** children;
    size_t child_count;
    size_t child_capacity;
} KCLNode;

typedef struct {
    KCLNode* root;
    char* script_path;
    char* error_message;
    bool has_error;
} KCLScript;

typedef struct {
    char* variables;
    size_t var_count;
    size_t var_capacity;
    KernelContext* kernel_ctx;
} KCLContext;

KCLLexer* kcl_lexer_create(const char* input);
void kcl_lexer_destroy(KCLLexer* lexer);
KCLToken kcl_lexer_next_token(KCLLexer* lexer);

KCLScript* kcl_parse(const char* input);
void kcl_script_destroy(KCLScript* script);

KCLContext* kcl_context_create(KernelContext* kernel_ctx);
void kcl_context_destroy(KCLContext* ctx);

ExecutionResult* kcl_execute(KCLContext* ctx, KCLScript* script);
ExecutionResult* kcl_execute_file(KCLContext* ctx, const char* filename);
ExecutionResult* kcl_execute_string(KCLContext* ctx, const char* script_text);

bool kcl_register_commands(KCLContext* ctx, CommandRegistry* registry);

char* kcl_get_variable(KCLContext* ctx, const char* name);
bool kcl_set_variable(KCLContext* ctx, const char* name, const char* value);

#endif