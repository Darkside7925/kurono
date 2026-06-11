#include "kernel.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static KernelContext* g_kernel_ctx = NULL;
static CommandRegistry* g_command_registry = NULL;

KernelContext* kernel_init(void) {
    if (g_kernel_ctx != NULL) {
        return g_kernel_ctx;
    }
    
    g_kernel_ctx = (KernelContext*)malloc(sizeof(KernelContext));
    if (!g_kernel_ctx) return NULL;
    
    g_kernel_ctx->is_root = false;
    g_kernel_ctx->current_env = ENV_KURONO;
    g_kernel_ctx->current_user = strdup("user");
    g_kernel_ctx->current_directory = strdup("/home/user");
    
    g_command_registry = command_registry_create();
    
    printf("%s v%s initialized\n", KERNEL_NAME, KERNEL_VERSION);
    return g_kernel_ctx;
}

void kernel_shutdown(KernelContext* ctx) {
    if (ctx) {
        free(ctx->current_user);
        free(ctx->current_directory);
        free(ctx);
    }
    
    if (g_command_registry) {
        command_registry_destroy(g_command_registry);
        g_command_registry = NULL;
    }
    
    g_kernel_ctx = NULL;
    printf("Kernel shutdown complete\n");
}

CommandRegistry* command_registry_create(void) {
    CommandRegistry* registry = (CommandRegistry*)malloc(sizeof(CommandRegistry));
    if (!registry) return NULL;
    
    registry->capacity = 100;
    registry->count = 0;
    registry->entries = (CommandEntry*)malloc(sizeof(CommandEntry) * registry->capacity);
    
    return registry;
}

void command_registry_destroy(CommandRegistry* registry) {
    if (!registry) return;
    
    for (size_t i = 0; i < registry->count; i++) {
        free(registry->entries[i].name);
        free(registry->entries[i].path);
        free(registry->entries[i].description);
    }
    
    free(registry->entries);
    free(registry);
}

void command_registry_add(CommandRegistry* registry, const char* name, const char* path, EnvironmentType env, const char* description) {
    if (!registry || !name || !path) return;
    
    if (registry->count >= registry->capacity) {
        registry->capacity *= 2;
        registry->entries = (CommandEntry*)realloc(registry->entries, sizeof(CommandEntry) * registry->capacity);
    }
    
    CommandEntry* entry = &registry->entries[registry->count++];
    entry->name = strdup(name);
    entry->path = strdup(path);
    entry->env = env;
    entry->description = description ? strdup(description) : strdup("");
}

CommandEntry** command_registry_find(CommandRegistry* registry, const char* name, size_t* count) {
    if (!registry || !name || !count) return NULL;
    
    *count = 0;
    CommandEntry** matches = (CommandEntry**)malloc(sizeof(CommandEntry*) * registry->count);
    
    for (size_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->entries[i].name, name) == 0) {
            matches[(*count)++] = &registry->entries[i];
        }
    }
    
    return (*count > 0) ? matches : NULL;
}

ExecutionResult* kernel_execute_command(KernelContext* ctx, const char* command_line) {
    if (!ctx || !command_line) return NULL;
    
    ExecutionResult* result = (ExecutionResult*)malloc(sizeof(ExecutionResult));
    if (!result) return NULL;
    
    result->result = CMD_NOT_FOUND;
    result->output = NULL;
    result->error = NULL;
    result->exit_code = -1;
    
    char* command_copy = strdup(command_line);
    char* command = strtok(command_copy, " ");
    
    if (!command) {
        result->error = strdup("Empty command");
        free(command_copy);
        return result;
    }
    
    size_t match_count = 0;
    CommandEntry** matches = command_registry_find(g_command_registry, command, &match_count);
    
    if (match_count == 0) {
        result->error = strdup("Command not found");
        free(command_copy);
        return result;
    }
    
    if (match_count > 1) {
        result->result = CMD_AMBIGUOUS;
        char* error_msg = (char*)malloc(1024);
        strcpy(error_msg, "[System Alert] Command exists in multiple environments:\n");
        
        for (size_t i = 0; i < match_count; i++) {
            char line[256];
            const char* env_name = (matches[i]->env == ENV_LINUX) ? "Linux" :
                                 (matches[i]->env == ENV_WINDOWS) ? "Windows" : "Kurono";
            snprintf(line, sizeof(line), "%zu) %s (%s)\n", i + 1, matches[i]->path, env_name);
            strcat(error_msg, line);
        }
        
        result->error = error_msg;
        free(matches);
        free(command_copy);
        return result;
    }
    
    CommandEntry* entry = matches[0];
    result->result = CMD_SUCCESS;
    result->output = strdup("Command executed successfully");
    result->exit_code = 0;
    
    free(matches);
    free(command_copy);
    return result;
}

void execution_result_destroy(ExecutionResult* result) {
    if (!result) return;
    
    free(result->output);
    free(result->error);
    free(result);
}

EnvironmentType kernel_detect_environment(const char* command) {
    if (!command) return ENV_UNKNOWN;
    
    size_t count = 0;
    CommandEntry** matches = command_registry_find(g_command_registry, command, &count);
    
    if (count == 1) {
        EnvironmentType env = matches[0]->env;
        free(matches);
        return env;
    }
    
    free(matches);
    return ENV_UNKNOWN;
}

bool kernel_switch_environment(KernelContext* ctx, EnvironmentType env) {
    if (!ctx) return false;
    
    ctx->current_env = env;
    return true;
}
