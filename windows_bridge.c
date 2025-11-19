#include "windows_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <windows.h>

static const char* common_windows_commands[] = {
    "dir", "copy", "move", "del", "type", "cd", "md", "rd", "cls", "echo",
    "set", "path", "ver", "date", "time", "tasklist", "taskkill", "net",
    "ipconfig", "ping", "tracert", "netstat", "nslookup", "systeminfo",
    "reg", "sc", "schtasks", "powershell", "cmd", "wmic", "fsutil",
    "diskpart", "sfc", "chkdsk", "defrag", "format", "label", "vol",
    "assoc", "ftype", "attrib", "comp", "fc", "find", "findstr",
    "more", "sort", "tree", "xcopy", "robocopy", "takeown", "icacls",
    NULL
};

static const char* common_powershell_commands[] = {
    "Get-ChildItem", "Get-Content", "Set-Content", "Copy-Item", "Move-Item",
    "Remove-Item", "New-Item", "Get-Process", "Stop-Process", "Start-Process",
    "Get-Service", "Start-Service", "Stop-Service", "Restart-Service",
    "Get-EventLog", "Write-EventLog", "Get-WmiObject", "Invoke-WmiMethod",
    "Get-Command", "Get-Help", "Get-Member", "Where-Object", "Select-Object",
    "Sort-Object", "Group-Object", "Measure-Object", "ForEach-Object",
    "If", "Else", "For", "While", "Switch", "Function", "Filter",
    NULL
};

WindowsBridge* windows_bridge_create(const char* windows_root) {
    if (!windows_root) return NULL;
    
    WindowsBridge* bridge = (WindowsBridge*)malloc(sizeof(WindowsBridge));
    if (!bridge) return NULL;
    
    bridge->system_root = strdup(windows_root);
    bridge->system32_path = (char*)malloc(strlen(windows_root) + 12);
    bridge->powershell_path = (char*)malloc(strlen(windows_root) + 24);
    bridge->registry_path = (char*)malloc(strlen(windows_root) + 10);
    
    sprintf(bridge->system32_path, "%s\\System32", windows_root);
    sprintf(bridge->powershell_path, "%s\\System32\\WindowsPowerShell\\v1.0", windows_root);
    sprintf(bridge->registry_path, "%s\\registry", windows_root);
    
    return bridge;
}

void windows_bridge_destroy(WindowsBridge* bridge) {
    if (!bridge) return;
    
    free(bridge->system_root);
    free(bridge->system32_path);
    free(bridge->powershell_path);
    free(bridge->registry_path);
    free(bridge);
}

bool windows_bridge_register_commands(WindowsBridge* bridge, CommandRegistry* registry) {
    if (!bridge || !registry) return false;
    
    for (int i = 0; common_windows_commands[i] != NULL; i++) {
        char* full_path = (char*)malloc(strlen(bridge->system32_path) + strlen(common_windows_commands[i]) + 2);
        sprintf(full_path, "%s\\%s.exe", bridge->system32_path, common_windows_commands[i]);
        
        char description[256];
        snprintf(description, sizeof(description), "Windows command: %s", common_windows_commands[i]);
        command_registry_add(registry, common_windows_commands[i], full_path, ENV_WINDOWS, description);
        free(full_path);
    }
    
    for (int i = 0; common_powershell_commands[i] != NULL; i++) {
        char* full_path = strdup(common_powershell_commands[i]);
        char description[256];
        snprintf(description, sizeof(description), "PowerShell command: %s", common_powershell_commands[i]);
        command_registry_add(registry, common_powershell_commands[i], full_path, ENV_WINDOWS, description);
        free(full_path);
    }
    
    return true;
}

bool windows_bridge_execute_command(WindowsBridge* bridge, const char* command, char** output, char** error) {
    if (!bridge || !command) return false;
    
    if (strncmp(command, "Get-", 4) == 0 || strncmp(command, "Set-", 4) == 0 || 
        strncmp(command, "New-", 4) == 0 || strncmp(command, "Remove-", 7) == 0 ||
        strncmp(command, "If ", 3) == 0 || strncmp(command, "For ", 4) == 0 ||
        strncmp(command, "While ", 6) == 0 || strncmp(command, "Function ", 9) == 0) {
        return windows_bridge_execute_powershell(bridge, command, output, error);
    }
    
    char* exe_path = (char*)malloc(strlen(bridge->system32_path) + strlen(command) + 6);
    sprintf(exe_path, "%s\\%s.exe", bridge->system32_path, command);
    
    struct stat st;
    if (stat(exe_path, &st) == 0) {
        bool result = windows_bridge_execute_pe(bridge, exe_path, output, error);
        free(exe_path);
        return result;
    }
    
    free(exe_path);
    if (error) *error = strdup("Windows command not found");
    return false;
}

bool windows_bridge_execute_pe(WindowsBridge* bridge, const char* pe_path, char** output, char** error) {
    if (!bridge || !pe_path) return false;
    
    if (!windows_bridge_is_pe_file(pe_path)) {
        if (error) *error = strdup("Not a valid PE executable");
        return false;
    }
    
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    char* cmd_line = (char*)malloc(strlen(pe_path) + 3);
    sprintf(cmd_line, "\"%s\"", pe_path);
    
    if (!CreateProcess(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (error) *error = strdup("Failed to create Windows process");
        free(cmd_line);
        return false;
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    free(cmd_line);
    
    if (output) {
        char* msg = (char*)malloc(256);
        sprintf(msg, "Windows PE executed with exit code: %lu", exit_code);
        *output = msg;
    }
    
    return true;
}

bool windows_bridge_execute_powershell(WindowsBridge* bridge, const char* script, char** output, char** error) {
    if (!bridge || !script) return false;
    
    char* ps_cmd = (char*)malloc(strlen(script) + 50);
    sprintf(ps_cmd, "powershell -Command \"%s\"", script);
    
    FILE* pipe = _popen(ps_cmd, "r");
    if (!pipe) {
        if (error) *error = strdup("Failed to execute PowerShell");
        free(ps_cmd);
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
    free(ps_cmd);
    
    if (output) *output = result;
    else free(result);
    
    return true;
}

bool windows_bridge_is_pe_file(const char* file_path) {
    if (!file_path) return false;
    
    FILE* file = fopen(file_path, "rb");
    if (!file) return false;
    
    unsigned char dos_header[2];
    if (fread(dos_header, 1, 2, file) != 2) {
        fclose(file);
        return false;
    }
    
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') {
        fclose(file);
        return false;
    }
    
    fseek(file, 0x3C, SEEK_SET);
    unsigned int pe_offset;
    if (fread(&pe_offset, 4, 1, file) != 1) {
        fclose(file);
        return false;
    }
    
    fseek(file, pe_offset, SEEK_SET);
    unsigned char pe_header[4];
    if (fread(pe_header, 1, 4, file) != 4) {
        fclose(file);
        return false;
    }
    
    fclose(file);
    
    return (pe_header[0] == 'P' && pe_header[1] == 'E' && pe_header[2] == 0 && pe_header[3] == 0);
}

bool windows_bridge_load_pe(WindowsBridge* bridge, const char* pe_path) {
    return windows_bridge_is_pe_file(pe_path);
}

bool windows_bridge_registry_set(WindowsBridge* bridge, const char* key, const char* value, const char* type) {
    if (!bridge || !key || !value) return false;
    
    char* reg_cmd = (char*)malloc(strlen(key) + strlen(value) + strlen(type) + 100);
    sprintf(reg_cmd, "reg add \"%s\" /v \"%s\" /t \"%s\" /d \"%s\" /f", key, "Default", type, value);
    
    system(reg_cmd);
    free(reg_cmd);
    
    return true;
}

char* windows_bridge_registry_get(WindowsBridge* bridge, const char* key) {
    if (!bridge || !key) return NULL;
    
    char* reg_cmd = (char*)malloc(strlen(key) + 100);
    sprintf(reg_cmd, "reg query \"%s\" /v \"Default\"", key);
    
    FILE* pipe = _popen(reg_cmd, "r");
    free(reg_cmd);
    
    if (!pipe) return NULL;
    
    char buffer[1024];
    char* result = (char*)malloc(1);
    result[0] = '\0';
    
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result = (char*)realloc(result, strlen(result) + strlen(buffer) + 1);
        strcat(result, buffer);
    }
    
    _pclose(pipe);
    return result;
}

bool windows_bridge_registry_delete(WindowsBridge* bridge, const char* key) {
    if (!bridge || !key) return false;
    
    char* reg_cmd = (char*)malloc(strlen(key) + 50);
    sprintf(reg_cmd, "reg delete \"%s\" /f", key);
    
    int result = system(reg_cmd);
    free(reg_cmd);
    
    return result == 0;
}