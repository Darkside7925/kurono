#include "linux_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static const char* common_linux_commands[] = {
    "ls", "dir", "cat", "grep", "find", "chmod", "chown", "mkdir", "rm", "cp", "mv",
    "ps", "kill", "top", "df", "du", "tar", "gzip", "wget", "curl", "ssh", "scp",
    "apt", "yum", "dnf", "pacman", "systemctl", "service", "ifconfig", "ip", "netstat",
    "iptables", "ufw", "useradd", "userdel", "passwd", "su", "sudo", "bash", "sh",
    "vim", "nano", "emacs", "less", "more", "head", "tail", "sort", "uniq", "wc",
    "diff", "patch", "make", "gcc", "g++", "python", "python3", "perl", "ruby", "php",
    "git", "svn", "mercurial", "docker", "podman", "kubectl", "ansible", "terraform",
    NULL
};

LinuxBridge* linux_bridge_create(const char* linux_root) {
    if (!linux_root) return NULL;
    
    LinuxBridge* bridge = (LinuxBridge*)malloc(sizeof(LinuxBridge));
    if (!bridge) return NULL;
    
    bridge->root_path = strdup(linux_root);
    bridge->bin_path = (char*)malloc(strlen(linux_root) + 5);
    bridge->usr_bin_path = (char*)malloc(strlen(linux_root) + 9);
    bridge->lib_path = (char*)malloc(strlen(linux_root) + 5);
    bridge->etc_path = (char*)malloc(strlen(linux_root) + 5);
    
    sprintf(bridge->bin_path, "%s/bin", linux_root);
    sprintf(bridge->usr_bin_path, "%s/usr/bin", linux_root);
    sprintf(bridge->lib_path, "%s/lib", linux_root);
    sprintf(bridge->etc_path, "%s/etc", linux_root);
    
    return bridge;
}

void linux_bridge_destroy(LinuxBridge* bridge) {
    if (!bridge) return;
    
    free(bridge->root_path);
    free(bridge->bin_path);
    free(bridge->usr_bin_path);
    free(bridge->lib_path);
    free(bridge->etc_path);
    free(bridge);
}

bool linux_bridge_register_commands(LinuxBridge* bridge, CommandRegistry* registry) {
    if (!bridge || !registry) return false;
    
    for (int i = 0; common_linux_commands[i] != NULL; i++) {
        char* full_path = linux_bridge_resolve_path(bridge, common_linux_commands[i]);
        if (full_path) {
            char description[256];
            snprintf(description, sizeof(description), "Linux command: %s", common_linux_commands[i]);
            command_registry_add(registry, common_linux_commands[i], full_path, ENV_LINUX, description);
            free(full_path);
        }
    }
    
    return true;
}

bool linux_bridge_execute_command(LinuxBridge* bridge, const char* command_line, char** output, char** error) {
    if (!bridge || !command_line) return false;
    
    #ifdef _WIN32
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "wsl -d KuronoLinux %s", command_line);
    FILE* pipe = _popen(cmd, "r");
    if (!pipe) {
        if (error) *error = strdup("Failed to execute WSL command");
        return false;
    }
    char buffer[1024];
    char* result = (char*)malloc(1);
    result[0] = '\0';
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result = (char*)realloc(result, strlen(result) + strlen(buffer) + 1);
        strcat(result, buffer);
    }
    _pclose(pipe);
    if (output) *output = result; else free(result);
    return true;
    #else
    int rc = system(command_line);
    if (rc != 0) {
        if (error) *error = strdup("Linux command failed");
        return false;
    }
    if (output) *output = strdup("Linux command executed successfully");
    return true;
    #endif
}

bool linux_bridge_is_command_available(LinuxBridge* bridge, const char* command) {
    if (!bridge || !command) return false;
    
    char* path = linux_bridge_resolve_path(bridge, command);
    if (!path) return false;
    
    struct stat st;
    bool exists = false;
    if (stat(path, &st) == 0) {
        #ifdef _WIN32
        exists = true;
        #else
        exists = (st.st_mode & S_IXUSR);
        #endif
    }
    free(path);
    
    return exists;
}

char* linux_bridge_resolve_path(LinuxBridge* bridge, const char* command) {
    if (!bridge || !command) return NULL;
    
    char* paths[] = { bridge->bin_path, bridge->usr_bin_path };
    
    for (int i = 0; i < 2; i++) {
        char* full_path = (char*)malloc(strlen(paths[i]) + strlen(command) + 2);
        sprintf(full_path, "%s/%s", paths[i], command);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            return full_path;
        }
        free(full_path);
    }
    
    return NULL;
}

bool linux_bridge_mount_filesystem(LinuxBridge* bridge) {
    if (!bridge) return false;
    
    struct stat st;
    if (stat(bridge->root_path, &st) != 0) {
        #ifdef _WIN32
        if (_mkdir(bridge->root_path) != 0) {
            return false;
        }
        #else
        if (mkdir(bridge->root_path, 0755) != 0) {
            return false;
        }
        #endif
    }
    
    char* dirs[] = { bridge->bin_path, bridge->usr_bin_path, bridge->lib_path, bridge->etc_path };
    
    for (int i = 0; i < 4; i++) {
        if (stat(dirs[i], &st) != 0) {
            #ifdef _WIN32
            if (_mkdir(dirs[i]) != 0) {
                return false;
            }
            #else
            if (mkdir(dirs[i], 0755) != 0) {
                return false;
            }
            #endif
        }
    }
    
    return true;
}

bool linux_bridge_unmount_filesystem(LinuxBridge* bridge) {
    if (!bridge) return false;
    return true;
}