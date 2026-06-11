#ifndef LINUX_BRIDGE_H
#define LINUX_BRIDGE_H

#include "kernel.h"
#include <stdbool.h>

typedef struct {
    char* root_path;
    char* bin_path;
    char* usr_bin_path;
    char* lib_path;
    char* etc_path;
} LinuxBridge;

LinuxBridge* linux_bridge_create(const char* linux_root);
void linux_bridge_destroy(LinuxBridge* bridge);

bool linux_bridge_register_commands(LinuxBridge* bridge, CommandRegistry* registry);
bool linux_bridge_execute_command(LinuxBridge* bridge, const char* command_line, char** output, char** error);

bool linux_bridge_is_command_available(LinuxBridge* bridge, const char* command);
char* linux_bridge_resolve_path(LinuxBridge* bridge, const char* command);

bool linux_bridge_mount_filesystem(LinuxBridge* bridge);
bool linux_bridge_unmount_filesystem(LinuxBridge* bridge);

#endif