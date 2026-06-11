#include "kernel.h"
#include "linux_bridge.h"
#include "windows_bridge.h"
#include "kcl_interpreter.h"
#include "conflict_resolver.h"
#include "security_supr_engine.h"
#include "package_manager.h"
#include "linux_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static KernelContext* g_kernel = NULL;
static LinuxBridge* g_linux_bridge = NULL;
static WindowsBridge* g_windows_bridge = NULL;
static KCLContext* g_kcl_ctx = NULL;
static SecuritySuprEngine* g_security_engine = NULL;
static PackageManager* g_package_manager = NULL;
static CommandRegistry* g_command_registry = NULL;

static int run_cmd(const char* cmd) {
    int rc = system(cmd);
    return rc;
}

void kurono_os_init(void) {
    printf("Initializing %s v%s...\n", KERNEL_NAME, KERNEL_VERSION);
    
    // Initialize core kernel
    g_kernel = kernel_init();
    if (!g_kernel) {
        fprintf(stderr, "Failed to initialize kernel\n");
        exit(1);
    }
    
    // Initialize command registry
    g_command_registry = command_registry_create();
    if (!g_command_registry) {
        fprintf(stderr, "Failed to create command registry\n");
        exit(1);
    }
    
    // Initialize Linux bridge
    #ifdef _WIN32
    g_linux_bridge = linux_bridge_create("D:\\OS\\Kurono OS\\LinuxRoot");
    #else
    g_linux_bridge = linux_bridge_create("/kurono/linux");
    #endif
    if (g_linux_bridge) {
        linux_bridge_mount_filesystem(g_linux_bridge);
        linux_bridge_register_commands(g_linux_bridge, g_command_registry);
    }
    
    // Initialize Windows bridge
    g_windows_bridge = windows_bridge_create("C:\\kurono\\windows");
    if (g_windows_bridge) {
        windows_bridge_register_commands(g_windows_bridge, g_command_registry);
    }
    
    // Initialize KCL interpreter
    g_kcl_ctx = kcl_context_create(g_kernel);
    if (g_kcl_ctx) {
        kcl_register_commands(g_kcl_ctx, g_command_registry);
    }
    
    // Initialize security engine
    g_security_engine = security_supr_engine_create();
    
    // Initialize package manager
    g_package_manager = package_manager_create("/kurono/packages/cache");
    if (g_package_manager) {
        package_manager_register_kurono_packages(g_package_manager, g_command_registry);
    }
    
    printf("Kurono OS initialized successfully\n");
}

void kurono_os_shutdown(void) {
    printf("Shutting down Kurono OS...\n");
    
    if (g_package_manager) {
        package_manager_destroy(g_package_manager);
        g_package_manager = NULL;
    }
    
    if (g_security_engine) {
        security_supr_engine_destroy(g_security_engine);
        g_security_engine = NULL;
    }
    
    if (g_kcl_ctx) {
        kcl_context_destroy(g_kcl_ctx);
        g_kcl_ctx = NULL;
    }
    
    if (g_windows_bridge) {
        windows_bridge_destroy(g_windows_bridge);
        g_windows_bridge = NULL;
    }
    
    if (g_linux_bridge) {
        linux_bridge_unmount_filesystem(g_linux_bridge);
        linux_bridge_destroy(g_linux_bridge);
        g_linux_bridge = NULL;
    }
    
    if (g_command_registry) {
        command_registry_destroy(g_command_registry);
        g_command_registry = NULL;
    }
    
    if (g_kernel) {
        kernel_shutdown(g_kernel);
        g_kernel = NULL;
    }
    
    printf("Kurono OS shutdown complete\n");
}

void kurono_os_print_banner(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                              KURONO OS                                       ║\n");
    printf("║                    Unified Hybrid Kernel System                             ║\n");
    printf("║                                                                              ║\n");
    printf("║  Linux • Windows • Kurono Command Language Integration                       ║\n");
    printf("║  Native PE Execution • Cross-Environment Commands                          ║\n");
    printf("║  Advanced Security • Package Management • Conflict Resolution               ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

void kurono_os_print_help(void) {
    printf("Kurono OS Commands:\n");
    printf("  help              - Show this help message\n");
    printf("  version           - Show version information\n");
    printf("  env               - Show current environment\n");
    printf("  switch <env>      - Switch to different environment (linux/windows/kurono)\n");
    printf("  supr              - Enable root mode (requires admin password)\n");
    printf("  exit              - Exit Kurono OS\n");
    printf("  install <pkg>     - Install a package\n");
    printf("  remove <pkg>      - Remove a package\n");
    printf("  list              - List installed packages\n");
    printf("  search <query>    - Search for packages\n");
    printf("  kcl <script>      - Execute KCL script\n");
    printf("  linux-start       - Start Kurono-controlled Linux VM\n");
    printf("  linux-start-gui   - Start Kurono-controlled Linux VM with GUI\n");
    printf("  linux-stop        - Stop Kurono-controlled Linux VM\n");
    printf("  linux-sync-create-user <name> [admin] - Create Linux user\n");
    printf("  linux-sync-delete-user <name>         - Delete Linux user\n");
    printf("  linux-sync-dump   - Dump Linux users to shared file\n");
    printf("  linux-sync-import - Import Linux users into Kurono\n");
    printf("  linux-shim-setup  - Create MSYS2 command shims in LinuxRoot\n");
    printf("  linux-de-install  - Install XFCE desktop in Linux VM\n");
    printf("  linux-de-start    - Start XFCE desktop (requires GUI VM)\n");
    printf("\n");
    printf("Available environments:\n");
    printf("  linux    - Linux subsystem with GNU utilities\n");
    printf("  windows  - Windows subsystem with PE loader and PowerShell\n");
    printf("  kurono   - Kurono native environment with KCL\n");
    printf("\n");
}

bool kurono_os_switch_environment(const char* env_name) {
    if (!env_name || !g_kernel) return false;
    
    EnvironmentType target_env;
    if (strcmp(env_name, "linux") == 0) {
        target_env = ENV_LINUX;
    } else if (strcmp(env_name, "windows") == 0) {
        target_env = ENV_WINDOWS;
    } else if (strcmp(env_name, "kurono") == 0) {
        target_env = ENV_KURONO;
    } else {
        printf("Unknown environment: %s\n", env_name);
        return false;
    }
    
    if (kernel_switch_environment(g_kernel, target_env)) {
        printf("Switched to %s environment\n", env_name);
        return true;
    }
    
    return false;
}

bool kurono_os_handle_supr(void) {
    if (!g_security_engine) return false;
    
    char password[256];
    printf("Enter admin password: ");
    fflush(stdout);
    
    // Simple password input (in real implementation, use secure input)
    if (scanf("%255s", password) != 1) {
        return false;
    }
    
    if (security_supr_engine_enable_supr(g_security_engine, password)) {
        printf("SUPR mode enabled. You now have root privileges.\n");
        printf("SUPR mode will expire in 15 minutes.\n");
        return true;
    } else {
        printf("Authentication failed. SUPR mode not enabled.\n");
        return false;
    }
}

void kurono_os_handle_command(const char* command_line) {
    if (!command_line || !g_kernel || !g_command_registry) return;
    
    // Handle built-in commands
    if (strcmp(command_line, "help") == 0) {
        kurono_os_print_help();
        return;
    } else if (strcmp(command_line, "version") == 0) {
        printf("%s v%s\n", KERNEL_NAME, KERNEL_VERSION);
        return;
    } else if (strcmp(command_line, "env") == 0) {
        const char* env_names[] = {"Linux", "Windows", "Kurono", "Unknown"};
        printf("Current environment: %s\n", env_names[g_kernel->current_env]);
        return;
    } else if (strncmp(command_line, "switch ", 7) == 0) {
        kurono_os_switch_environment(command_line + 7);
        return;
    } else if (strcmp(command_line, "supr") == 0) {
        kurono_os_handle_supr();
        return;
    } else if (strcmp(command_line, "linux-start") == 0) {
        run_cmd("powershell -ExecutionPolicy Bypass -File \"D:\\OS\\Kurono OS\\linux_vm_start.ps1\" -VmDir \"D:\\OS\\Kurono OS\\LinuxVM\"");
        return;
    } else if (strcmp(command_line, "linux-start-gui") == 0) {
        run_cmd("powershell -ExecutionPolicy Bypass -File \"D:\\OS\\Kurono OS\\linux_vm_start_gui.ps1\" -VmDir \"D:\\OS\\Kurono OS\\LinuxVM\"");
        return;
    } else if (strcmp(command_line, "linux-stop") == 0) {
        run_cmd("powershell -ExecutionPolicy Bypass -File \"D:\\OS\\Kurono OS\\linux_vm_stop.ps1\" ");
        return;
    } else if (strncmp(command_line, "linux-sync-create-user ", 24) == 0) {
        const char* args = command_line + 24;
        char name[128] = {0};
        char admin[16] = {0};
        sscanf(args, "%127s %15s", name, admin);
        bool is_admin = (strcmp(admin, "admin") == 0 || strcmp(admin, "true") == 0);
        if (linux_sync_create_user(name, is_admin)) printf("Linux user created: %s\n", name);
        else printf("Failed to create Linux user: %s\n", name);
        return;
    } else if (strncmp(command_line, "linux-sync-delete-user ", 24) == 0) {
        const char* args = command_line + 24;
        char name[128] = {0};
        sscanf(args, "%127s", name);
        if (linux_sync_delete_user(name)) printf("Linux user deleted: %s\n", name);
        else printf("Failed to delete Linux user: %s\n", name);
        return;
    } else if (strcmp(command_line, "linux-sync-dump") == 0) {
        if (linux_sync_sync_from_linux()) printf("Dumped Linux users to D:\\OS\\Kurono OS\\Users\\linux_passwd.txt\n");
        else printf("Failed to dump Linux users\n");
        return;
    } else if (strcmp(command_line, "linux-sync-import") == 0) {
        if (security_supr_engine_import_linux_passwd(g_security_engine, "D:\\OS\\Kurono OS\\Users\\linux_passwd.txt")) printf("Imported Linux users into Kurono\n");
        else printf("Failed to import Linux users\n");
        return;
    } else if (strcmp(command_line, "linux-shim-setup") == 0) {
        run_cmd("powershell -ExecutionPolicy Bypass -File \"D:\\OS\\Kurono OS\\setup_kurono_linux_root.ps1\" -Root \"D:\\OS\\Kurono OS\\LinuxRoot\" ");
        return;
    } else if (strcmp(command_line, "linux-de-install") == 0) {
        run_cmd("powershell -NoProfile -Command \"ssh -p 2222 root@localhost 'apk update && apk add xfce4 xfce4-terminal lightdm lightdm-gtk-greeter dbus dbus-x11 xorg-server mesa-dri-swrast && rc-update add lightdm && rc-update add dbus'\"");
        printf("Desktop packages installed (XFCE + LightDM).\n");
        return;
    } else if (strcmp(command_line, "linux-de-start") == 0) {
        run_cmd("powershell -NoProfile -Command \"ssh -p 2222 root@localhost 'service dbus start; service lightdm start || startxfce4'\"");
        return;
    }
    
    // Check for command conflicts
    char* command_copy = strdup(command_line);
    char* command = strtok(command_copy, " ");
    
    size_t conflict_count = 0;
    CommandEntry** conflicts = command_registry_find(g_command_registry, command, &conflict_count);
    
    if (conflict_count > 1) {
        ConflictResolver* resolver = conflict_resolver_create(command);
        if (resolver && conflict_resolver_detect_conflicts(resolver, g_command_registry)) {
            int choice = conflict_resolver_prompt_user(resolver);
            if (choice >= 0) {
                CommandEntry* selected = conflicts[choice];
                printf("Selected: %s from %s environment\n", selected->path, 
                       selected->env == ENV_LINUX ? "Linux" : 
                       selected->env == ENV_WINDOWS ? "Windows" : "Kurono");
            }
        }
        conflict_resolver_destroy(resolver);
    } else if (conflict_count == 1) {
        // Execute the command
        ExecutionResult* result = kernel_execute_command(g_kernel, command_line);
        if (result) {
            if (result->output) {
                printf("%s\n", result->output);
            }
            if (result->error) {
                fprintf(stderr, "Error: %s\n", result->error);
            }
            execution_result_destroy(result);
        }
    } else {
        printf("Command not found: %s\n", command);
    }
    
    free(conflicts);
    free(command_copy);
}

int main(int argc, char* argv[]) {
    kurono_os_init();
    kurono_os_print_banner();
    kurono_os_print_help();
    
    char command_line[1024];
    
    printf("Kurono OS> ");
    fflush(stdout);
    
    while (fgets(command_line, sizeof(command_line), stdin)) {
        // Remove newline
        command_line[strcspn(command_line, "\n")] = 0;
        
        // Skip empty lines
        if (strlen(command_line) == 0) {
            printf("Kurono OS> ");
            fflush(stdout);
            continue;
        }
        
        // Handle exit command
        if (strcmp(command_line, "exit") == 0) {
            break;
        }
        
        // Handle the command
        kurono_os_handle_command(command_line);
        
        printf("Kurono OS> ");
        fflush(stdout);
    }
    
    kurono_os_shutdown();
    return 0;
}