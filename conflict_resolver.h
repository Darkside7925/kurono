#ifndef CONFLICT_RESOLVER_H
#define CONFLICT_RESOLVER_H

#include "kernel.h"
#include <stdbool.h>

typedef struct {
    char* command_name;
    CommandEntry** conflicting_entries;
    size_t conflict_count;
    int user_choice;
    bool auto_resolve;
    EnvironmentType preferred_env;
} ConflictResolver;

typedef enum {
    RESOLUTION_MANUAL,
    RESOLUTION_PREFER_LINUX,
    RESOLUTION_PREFER_WINDOWS,
    RESOLUTION_PREFER_KURONO,
    RESOLUTION_FIRST_FOUND,
    RESOLUTION_ENV_BASED
} ResolutionStrategy;

ConflictResolver* conflict_resolver_create(const char* command_name);
void conflict_resolver_destroy(ConflictResolver* resolver);

bool conflict_resolver_detect_conflicts(ConflictResolver* resolver, CommandRegistry* registry);
int conflict_resolver_prompt_user(ConflictResolver* resolver);
int conflict_resolver_auto_resolve(ConflictResolver* resolver, ResolutionStrategy strategy);

CommandEntry* conflict_resolver_get_resolution(ConflictResolver* resolver);
void conflict_resolver_set_preferred_environment(ConflictResolver* resolver, EnvironmentType env);
void conflict_resolver_set_auto_resolve(ConflictResolver* resolver, bool auto_resolve);

char* conflict_resolver_format_conflict_message(ConflictResolver* resolver);
bool conflict_resolver_save_preference(ConflictResolver* resolver, const char* preference_file);
bool conflict_resolver_load_preference(ConflictResolver* resolver, const char* preference_file);

#endif