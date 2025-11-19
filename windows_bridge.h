#ifndef WINDOWS_BRIDGE_H
#define WINDOWS_BRIDGE_H

#include "kernel.h"
#include <stdbool.h>

typedef struct {
    char* system_root;
    char* system32_path;
    char* powershell_path;
    char* registry_path;
} WindowsBridge;

typedef struct {
    char* key;
    char* value;
    char* type;
} RegistryEntry;

WindowsBridge* windows_bridge_create(const char* windows_root);
void windows_bridge_destroy(WindowsBridge* bridge);

bool windows_bridge_register_commands(WindowsBridge* bridge, CommandRegistry* registry);
bool windows_bridge_execute_command(WindowsBridge* bridge, const char* command, char** output, char** error);

bool windows_bridge_execute_pe(WindowsBridge* bridge, const char* pe_path, char** output, char** error);
bool windows_bridge_execute_powershell(WindowsBridge* bridge, const char* script, char** output, char** error);

bool windows_bridge_is_pe_file(const char* file_path);
bool windows_bridge_load_pe(WindowsBridge* bridge, const char* pe_path);

bool windows_bridge_registry_set(WindowsBridge* bridge, const char* key, const char* value, const char* type);
char* windows_bridge_registry_get(WindowsBridge* bridge, const char* key);
bool windows_bridge_registry_delete(WindowsBridge* bridge, const char* key);

#endif