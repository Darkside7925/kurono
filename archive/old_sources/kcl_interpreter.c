#include "kcl_interpreter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

KCLLexer* kcl_lexer_create(const char* input) {
    if (!input) return NULL;
    
    KCLLexer* lexer = (KCLLexer*)malloc(sizeof(KCLLexer));
    if (!lexer) return NULL;
    
    lexer->tokens = (KCLToken*)malloc(sizeof(KCLToken) * 100);
    lexer->count = 0;
    lexer->capacity = 100;
    lexer->current = 0;
    
    return lexer;
}

void kcl_lexer_destroy(KCLLexer* lexer) {
    if (!lexer) return;
    
    for (size_t i = 0; i < lexer->count; i++) {
        free(lexer->tokens[i].value);
    }
    
    free(lexer->tokens);
    free(lexer);
}

KCLToken kcl_lexer_next_token(KCLLexer* lexer) {
    KCLToken token;
    token.type = KCL_TOKEN_EOF;
    token.value = NULL;
    token.line = 1;
    token.column = 0;
    
    return token;
}

KCLScript* kcl_parse(const char* input) {
    if (!input) return NULL;
    
    KCLScript* script = (KCLScript*)malloc(sizeof(KCLScript));
    if (!script) return NULL;
    
    script->root = NULL;
    script->script_path = NULL;
    script->error_message = NULL;
    script->has_error = false;
    
    return script;
}

void kcl_script_destroy(KCLScript* script) {
    if (!script) return;
    
    if (script->root) {
        // TODO: Implement proper AST destruction
    }
    
    free(script->script_path);
    free(script->error_message);
    free(script);
}

KCLContext* kcl_context_create(KernelContext* kernel_ctx) {
    if (!kernel_ctx) return NULL;
    
    KCLContext* ctx = (KCLContext*)malloc(sizeof(KCLContext));
    if (!ctx) return NULL;
    
    ctx->variables = (char*)malloc(1024);
    ctx->var_count = 0;
    ctx->var_capacity = 1024;
    ctx->kernel_ctx = kernel_ctx;
    
    return ctx;
}

void kcl_context_destroy(KCLContext* ctx) {
    if (!ctx) return;
    
    free(ctx->variables);
    free(ctx);
}

ExecutionResult* kcl_execute(KCLContext* ctx, KCLScript* script) {
    if (!ctx || !script) return NULL;
    
    ExecutionResult* result = (ExecutionResult*)malloc(sizeof(ExecutionResult));
    if (!result) return NULL;
    
    result->result = CMD_SUCCESS;
    result->output = strdup("KCL script executed successfully");
    result->error = NULL;
    result->exit_code = 0;
    
    return result;
}

ExecutionResult* kcl_execute_file(KCLContext* ctx, const char* filename) {
    if (!ctx || !filename) return NULL;
    
    FILE* file = fopen(filename, "r");
    if (!file) {
        ExecutionResult* result = (ExecutionResult*)malloc(sizeof(ExecutionResult));
        result->result = CMD_EXECUTION_FAILED;
        result->output = NULL;
        result->error = strdup("Failed to open KCL script file");
        result->exit_code = -1;
        return result;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* script_text = (char*)malloc(file_size + 1);
    fread(script_text, 1, file_size, file);
    script_text[file_size] = '\0';
    
    fclose(file);
    
    ExecutionResult* result = kcl_execute_string(ctx, script_text);
    free(script_text);
    
    return result;
}

ExecutionResult* kcl_execute_string(KCLContext* ctx, const char* script_text) {
    if (!ctx || !script_text) return NULL;
    
    KCLScript* script = kcl_parse(script_text);
    if (!script) {
        ExecutionResult* result = (ExecutionResult*)malloc(sizeof(ExecutionResult));
        result->result = CMD_EXECUTION_FAILED;
        result->output = NULL;
        result->error = strdup("Failed to parse KCL script");
        result->exit_code = -1;
        return result;
    }
    
    ExecutionResult* result = kcl_execute(ctx, script);
    kcl_script_destroy(script);
    
    return result;
}

bool kcl_register_commands(KCLContext* ctx, CommandRegistry* registry) {
    if (!ctx || !registry) return false;
    
    const char* kcl_commands[] = {
        "kcl", "kurono", "supr", "kcl-run", "kcl-install", "kcl-remove",
        "kcl-help", "kcl-version", "kcl-list", "kcl-env", "kcl-set",
        "kcl-get", "kcl-if", "kcl-for", "kcl-while", "kcl-function",
        NULL
    };
    
    for (int i = 0; kcl_commands[i] != NULL; i++) {
        char description[256];
        snprintf(description, sizeof(description), "KCL command: %s", kcl_commands[i]);
        command_registry_add(registry, kcl_commands[i], kcl_commands[i], ENV_KURONO, description);
    }
    
    return true;
}

char* kcl_get_variable(KCLContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    // TODO: Implement variable storage and retrieval
    return NULL;
}

bool kcl_set_variable(KCLContext* ctx, const char* name, const char* value) {
    if (!ctx || !name || !value) return false;
    
    // TODO: Implement variable storage
    return true;
}