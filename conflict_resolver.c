#include "conflict_resolver.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

ConflictResolver* conflict_resolver_create(const char* command_name) {
    if (!command_name) return NULL;
    
    ConflictResolver* resolver = (ConflictResolver*)malloc(sizeof(ConflictResolver));
    if (!resolver) return NULL;
    
    resolver->command_name = strdup(command_name);
    resolver->conflicting_entries = NULL;
    resolver->conflict_count = 0;
    resolver->user_choice = -1;
    resolver->auto_resolve = false;
    resolver->preferred_env = ENV_KURONO;
    
    return resolver;
}

void conflict_resolver_destroy(ConflictResolver* resolver) {
    if (!resolver) return;
    
    free(resolver->command_name);
    free(resolver->conflicting_entries);
    free(resolver);
}

bool conflict_resolver_detect_conflicts(ConflictResolver* resolver, CommandRegistry* registry) {
    if (!resolver || !registry) return false;
    
    size_t count = 0;
    CommandEntry** matches = command_registry_find(registry, resolver->command_name, &count);
    
    if (count <= 1) {
        free(matches);
        return false;
    }
    
    resolver->conflicting_entries = matches;
    resolver->conflict_count = count;
    
    return true;
}

int conflict_resolver_prompt_user(ConflictResolver* resolver) {
    if (!resolver || resolver->conflict_count <= 1) return -1;
    
    printf("[System Alert] Command '%s' exists in multiple environments:\n", resolver->command_name);
    
    for (size_t i = 0; i < resolver->conflict_count; i++) {
        const char* env_name = (resolver->conflicting_entries[i]->env == ENV_LINUX) ? "Linux" :
                             (resolver->conflicting_entries[i]->env == ENV_WINDOWS) ? "Windows" : "Kurono";
        printf("%zu) %-30s (%s)\n", i + 1, resolver->conflicting_entries[i]->path, env_name);
    }
    
    printf("Enter selection (1-%zu): ", resolver->conflict_count);
    
    int choice;
    if (scanf("%d", &choice) != 1) {
        return -1;
    }
    
    if (choice < 1 || choice > (int)resolver->conflict_count) {
        printf("Invalid selection. Please choose between 1 and %zu.\n", resolver->conflict_count);
        return -1;
    }
    
    resolver->user_choice = choice - 1;
    return resolver->user_choice;
}

int conflict_resolver_auto_resolve(ConflictResolver* resolver, ResolutionStrategy strategy) {
    if (!resolver || resolver->conflict_count <= 1) return -1;
    
    switch (strategy) {
        case RESOLUTION_PREFER_LINUX:
            for (size_t i = 0; i < resolver->conflict_count; i++) {
                if (resolver->conflicting_entries[i]->env == ENV_LINUX) {
                    resolver->user_choice = i;
                    return i;
                }
            }
            break;
            
        case RESOLUTION_PREFER_WINDOWS:
            for (size_t i = 0; i < resolver->conflict_count; i++) {
                if (resolver->conflicting_entries[i]->env == ENV_WINDOWS) {
                    resolver->user_choice = i;
                    return i;
                }
            }
            break;
            
        case RESOLUTION_PREFER_KURONO:
            for (size_t i = 0; i < resolver->conflict_count; i++) {
                if (resolver->conflicting_entries[i]->env == ENV_KURONO) {
                    resolver->user_choice = i;
                    return i;
                }
            }
            break;
            
        case RESOLUTION_FIRST_FOUND:
            resolver->user_choice = 0;
            return 0;
            
        case RESOLUTION_ENV_BASED:
            for (size_t i = 0; i < resolver->conflict_count; i++) {
                if (resolver->conflicting_entries[i]->env == resolver->preferred_env) {
                    resolver->user_choice = i;
                    return i;
                }
            }
            break;
            
        default:
            return -1;
    }
    
    // Fallback to first available
    resolver->user_choice = 0;
    return 0;
}

CommandEntry* conflict_resolver_get_resolution(ConflictResolver* resolver) {
    if (!resolver || resolver->user_choice < 0 || resolver->user_choice >= (int)resolver->conflict_count) {
        return NULL;
    }
    
    return resolver->conflicting_entries[resolver->user_choice];
}

void conflict_resolver_set_preferred_environment(ConflictResolver* resolver, EnvironmentType env) {
    if (!resolver) return;
    resolver->preferred_env = env;
}

void conflict_resolver_set_auto_resolve(ConflictResolver* resolver, bool auto_resolve) {
    if (!resolver) return;
    resolver->auto_resolve = auto_resolve;
}

char* conflict_resolver_format_conflict_message(ConflictResolver* resolver) {
    if (!resolver || resolver->conflict_count <= 1) return NULL;
    
    size_t buffer_size = 1024;
    char* message = (char*)malloc(buffer_size);
    
    snprintf(message, buffer_size, "[System Alert] Command '%s' exists in multiple environments:\n", resolver->command_name);
    
    for (size_t i = 0; i < resolver->conflict_count; i++) {
        const char* env_name = (resolver->conflicting_entries[i]->env == ENV_LINUX) ? "Linux" :
                             (resolver->conflicting_entries[i]->env == ENV_WINDOWS) ? "Windows" : "Kurono";
        char line[256];
        snprintf(line, sizeof(line), "%zu) %-30s (%s)\n", i + 1, resolver->conflicting_entries[i]->path, env_name);
        
        if (strlen(message) + strlen(line) >= buffer_size - 1) {
            buffer_size *= 2;
            message = (char*)realloc(message, buffer_size);
        }
        
        strcat(message, line);
    }
    
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Enter selection (1-%zu): ", resolver->conflict_count);
    strcat(message, prompt);
    
    return message;
}

bool conflict_resolver_save_preference(ConflictResolver* resolver, const char* preference_file) {
    if (!resolver || !preference_file) return false;
    
    FILE* file = fopen(preference_file, "w");
    if (!file) return false;
    
    fprintf(file, "command=%s\n", resolver->command_name);
    fprintf(file, "choice=%d\n", resolver->user_choice);
    fprintf(file, "auto_resolve=%s\n", resolver->auto_resolve ? "true" : "false");
    fprintf(file, "preferred_env=%d\n", resolver->preferred_env);
    
    fclose(file);
    return true;
}

bool conflict_resolver_load_preference(ConflictResolver* resolver, const char* preference_file) {
    if (!resolver || !preference_file) return false;
    
    FILE* file = fopen(preference_file, "r");
    if (!file) return false;
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char* key = strtok(line, "=");
        char* value = strtok(NULL, "\n");
        
        if (!key || !value) continue;
        
        if (strcmp(key, "choice") == 0) {
            resolver->user_choice = atoi(value);
        } else if (strcmp(key, "auto_resolve") == 0) {
            resolver->auto_resolve = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "preferred_env") == 0) {
            resolver->preferred_env = (EnvironmentType)atoi(value);
        }
    }
    
    fclose(file);
    return true;
}