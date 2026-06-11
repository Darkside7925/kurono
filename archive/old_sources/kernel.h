#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KERNEL_VERSION "1.0.0"
#define KERNEL_NAME "Kurono OS Hybrid Kernel"

typedef enum {
    ENV_LINUX,
    ENV_WINDOWS,
    ENV_KURONO,
    ENV_UNKNOWN
} EnvironmentType;

typedef enum {
    CMD_SUCCESS,
    CMD_NOT_FOUND,
    CMD_AMBIGUOUS,
    CMD_PERMISSION_DENIED,
    CMD_EXECUTION_FAILED
} CommandResult;

typedef struct {
    char* name;
    char* path;
    EnvironmentType env;
    char* description;
} CommandEntry;

typedef struct {
    CommandEntry* entries;
    size_t count;
    size_t capacity;
} CommandRegistry;

typedef struct {
    bool is_root;
    EnvironmentType current_env;
    char* current_user;
    char* current_directory;
} KernelContext;

typedef struct {
    CommandResult result;
    char* output;
    char* error;
    int exit_code;
} ExecutionResult;

KernelContext* kernel_init(void);
void kernel_shutdown(KernelContext* ctx);

CommandRegistry* command_registry_create(void);
void command_registry_destroy(CommandRegistry* registry);
void command_registry_add(CommandRegistry* registry, const char* name, const char* path, EnvironmentType env, const char* description);
CommandEntry** command_registry_find(CommandRegistry* registry, const char* name, size_t* count);

ExecutionResult* kernel_execute_command(KernelContext* ctx, const char* command_line);
void execution_result_destroy(ExecutionResult* result);

EnvironmentType kernel_detect_environment(const char* command);
bool kernel_switch_environment(KernelContext* ctx, EnvironmentType env);

#endif