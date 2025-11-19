#include "kernel.h"
#include "linux_bridge.h"
#include "windows_bridge.h"
#include "kcl_interpreter.h"
#include "conflict_resolver.h"
#include "security_supr_engine.h"
#include "package_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) printf("Testing %s... ", name)
#define TEST_PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define TEST_FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define TEST_ASSERT(condition, msg) do { if (!(condition)) { TEST_FAIL(msg); return; } } while(0)

void test_kernel_core(void) {
    TEST_START("Kernel Core");
    
    KernelContext* ctx = kernel_init();
    TEST_ASSERT(ctx != NULL, "Kernel context should not be NULL");
    TEST_ASSERT(strcmp(ctx->current_user, "user") == 0, "Default user should be 'user'");
    TEST_ASSERT(ctx->current_env == ENV_KURONO, "Default environment should be KURONO");
    
    CommandRegistry* registry = command_registry_create();
    TEST_ASSERT(registry != NULL, "Command registry should not be NULL");
    
    command_registry_add(registry, "test", "/bin/test", ENV_LINUX, "Test command");
    size_t count = 0;
    CommandEntry** entries = command_registry_find(registry, "test", &count);
    TEST_ASSERT(count == 1, "Should find exactly one test command");
    TEST_ASSERT(entries != NULL, "Should return valid entries array");
    
    free(entries);
    command_registry_destroy(registry);
    kernel_shutdown(ctx);
    
    TEST_PASS();
}

void test_linux_bridge(void) {
#ifndef KURONO_DISABLE_LINUX
    TEST_START("Linux Bridge");
    
    LinuxBridge* bridge = linux_bridge_create("/tmp/test_linux");
    TEST_ASSERT(bridge != NULL, "Linux bridge should not be NULL");
    
    bool mounted = linux_bridge_mount_filesystem(bridge);
    TEST_ASSERT(mounted, "Linux filesystem should mount successfully");
    
    CommandRegistry* registry = command_registry_create();
    bool registered = linux_bridge_register_commands(bridge, registry);
    TEST_ASSERT(registered, "Linux commands should register successfully");
    
    size_t count = 0;
    CommandEntry** entries = command_registry_find(registry, "ls", &count);
    TEST_ASSERT(count > 0, "Should find Linux 'ls' command");
    
    free(entries);
    command_registry_destroy(registry);
    linux_bridge_destroy(bridge);
    
    TEST_PASS();
#else
    TEST_START("Linux Bridge (disabled)");
    TEST_PASS();
#endif
}

void test_windows_bridge(void) {
    TEST_START("Windows Bridge");
    
    WindowsBridge* bridge = windows_bridge_create("C:\\tmp\\test_windows");
    TEST_ASSERT(bridge != NULL, "Windows bridge should not be NULL");
    
    CommandRegistry* registry = command_registry_create();
    bool registered = windows_bridge_register_commands(bridge, registry);
    TEST_ASSERT(registered, "Windows commands should register successfully");
    
    size_t count = 0;
    CommandEntry** entries = command_registry_find(registry, "dir", &count);
    TEST_ASSERT(count > 0, "Should find Windows 'dir' command");
    
    free(entries);
    command_registry_destroy(registry);
    windows_bridge_destroy(bridge);
    
    TEST_PASS();
}

void test_kcl_interpreter(void) {
    TEST_START("KCL Interpreter");
    
    KernelContext* kernel_ctx = kernel_init();
    KCLContext* ctx = kcl_context_create(kernel_ctx);
    TEST_ASSERT(ctx != NULL, "KCL context should not be NULL");
    
    CommandRegistry* registry = command_registry_create();
    bool registered = kcl_register_commands(ctx, registry);
    TEST_ASSERT(registered, "KCL commands should register successfully");
    
    ExecutionResult* result = kcl_execute_string(ctx, "echo 'Hello KCL'");
    TEST_ASSERT(result != NULL, "KCL execution should return result");
    TEST_ASSERT(result->result == CMD_SUCCESS, "KCL execution should succeed");
    
    execution_result_destroy(result);
    command_registry_destroy(registry);
    kcl_context_destroy(ctx);
    kernel_shutdown(kernel_ctx);
    
    TEST_PASS();
}

void test_conflict_resolver(void) {
    TEST_START("Conflict Resolver");
    
    ConflictResolver* resolver = conflict_resolver_create("test");
    TEST_ASSERT(resolver != NULL, "Conflict resolver should not be NULL");
    
    CommandRegistry* registry = command_registry_create();
    command_registry_add(registry, "test", "/bin/test", ENV_LINUX, "Linux test");
    command_registry_add(registry, "test", "test.exe", ENV_WINDOWS, "Windows test");
    command_registry_add(registry, "test", "test.kc", ENV_KURONO, "Kurono test");
    
    bool conflicts = conflict_resolver_detect_conflicts(resolver, registry);
    TEST_ASSERT(conflicts, "Should detect command conflicts");
    TEST_ASSERT(resolver->conflict_count == 3, "Should find 3 conflicting commands");
    
    conflict_resolver_set_auto_resolve(resolver, true);
    int choice = conflict_resolver_auto_resolve(resolver, RESOLUTION_FIRST_FOUND);
    TEST_ASSERT(choice == 0, "Should auto-resolve to first option");
    
    CommandEntry* resolution = conflict_resolver_get_resolution(resolver);
    TEST_ASSERT(resolution != NULL, "Should return valid resolution");
    
    conflict_resolver_destroy(resolver);
    command_registry_destroy(registry);
    
    TEST_PASS();
}

void test_security_engine(void) {
    TEST_START("Security Engine");
    
    SecuritySuprEngine* engine = security_supr_engine_create();
    TEST_ASSERT(engine != NULL, "Security engine should not be NULL");
    
    bool auth = security_supr_engine_authenticate(engine, "root", "toor");
    TEST_ASSERT(auth, "Should authenticate root user with correct password");
    
    bool bad_auth = security_supr_engine_authenticate(engine, "root", "wrong");
    TEST_ASSERT(!bad_auth, "Should not authenticate with wrong password");
    
    bool supr_enabled = security_supr_engine_enable_supr(engine, "toor");
    TEST_ASSERT(supr_enabled, "Should enable SUPR mode with correct password");
    
    bool supr_active = security_supr_engine_is_supr_active(engine);
    TEST_ASSERT(supr_active, "SUPR mode should be active");
    
    security_supr_engine_disable_supr(engine);
    supr_active = security_supr_engine_is_supr_active(engine);
    TEST_ASSERT(!supr_active, "SUPR mode should be disabled");
    
    security_supr_engine_destroy(engine);
    
    TEST_PASS();
}

void test_package_manager(void) {
    TEST_START("Package Manager");
    
    PackageManager* pm = package_manager_create("/tmp/test_packages");
    TEST_ASSERT(pm != NULL, "Package manager should not be NULL");
    
    bool installed = package_manager_install(pm, "test-package");
    TEST_ASSERT(installed, "Should install package successfully");
    
    Package* pkg = package_manager_get_package(pm, "test-package");
    TEST_ASSERT(pkg != NULL, "Should retrieve installed package");
    TEST_ASSERT(strcmp(pkg->name, "test-package") == 0, "Package name should match");
    TEST_ASSERT(pkg->status == PKG_STATUS_INSTALLED, "Package should be marked as installed");
    
    size_t count = 0;
    Package** packages = package_manager_list_packages(pm, &count);
    TEST_ASSERT(count > 0, "Should list at least one package");
    TEST_ASSERT(packages != NULL, "Should return valid package list");
    
    bool removed = package_manager_remove(pm, "test-package");
    TEST_ASSERT(removed, "Should remove package successfully");
    
    pkg = package_manager_get_package(pm, "test-package");
    TEST_ASSERT(pkg == NULL, "Package should no longer exist");
    
    package_manager_destroy(pm);
    
    TEST_PASS();
}

void test_integration(void) {
    TEST_START("Integration Test");
    
    // Initialize all components
    KernelContext* kernel = kernel_init();
    TEST_ASSERT(kernel != NULL, "Kernel should initialize");
    
    CommandRegistry* registry = command_registry_create();
    TEST_ASSERT(registry != NULL, "Registry should create");
    
#ifndef KURONO_DISABLE_LINUX
    LinuxBridge* linux = linux_bridge_create("/tmp/test_integration");
    TEST_ASSERT(linux != NULL, "Linux bridge should create");
    linux_bridge_register_commands(linux, registry);
#endif
    
    WindowsBridge* windows = windows_bridge_create("C:\\tmp\\test_integration");
    TEST_ASSERT(windows != NULL, "Windows bridge should create");
    windows_bridge_register_commands(windows, registry);
    
    KCLContext* kcl = kcl_context_create(kernel);
    TEST_ASSERT(kcl != NULL, "KCL context should create");
    kcl_register_commands(kcl, registry);
    
    SecuritySuprEngine* security = security_supr_engine_create();
    TEST_ASSERT(security != NULL, "Security engine should create");
    
    PackageManager* pm = package_manager_create("/tmp/test_integration");
    TEST_ASSERT(pm != NULL, "Package manager should create");
    package_manager_register_kurono_packages(pm, registry);
    
    // Test cross-environment command execution
    size_t count = 0;
    CommandEntry** ls_entries = command_registry_find(registry, "ls", &count);
    TEST_ASSERT(count > 0, "Should find 'ls' command");
    
    count = 0;
    CommandEntry** dir_entries = command_registry_find(registry, "dir", &count);
    TEST_ASSERT(count > 0, "Should find 'dir' command");
    
    count = 0;
    CommandEntry** kcl_entries = command_registry_find(registry, "kcl", &count);
    TEST_ASSERT(count > 0, "Should find 'kcl' command");
    
    // Cleanup
    free(ls_entries);
    free(dir_entries);
    free(kcl_entries);
    
    package_manager_destroy(pm);
    security_supr_engine_destroy(security);
    kcl_context_destroy(kcl);
    windows_bridge_destroy(windows);
    
#ifndef KURONO_DISABLE_LINUX
    linux_bridge_destroy(linux);
#endif
    command_registry_destroy(registry);
    kernel_shutdown(kernel);
    
    TEST_PASS();
}

void run_all_tests(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        KURONO OS TEST SUITE                                  ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    test_kernel_core();
    test_linux_bridge();
    test_windows_bridge();
    test_kcl_interpreter();
    test_conflict_resolver();
    test_security_engine();
    test_package_manager();
    test_integration();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        TEST RESULTS                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("Total tests:  %d\n", tests_passed + tests_failed);
    printf("Success rate: %.1f%%\n", (float)tests_passed / (tests_passed + tests_failed) * 100);
    printf("\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        run_all_tests();
        return tests_failed > 0 ? 1 : 0;
    }
    
    if (argc > 1) {
        // Handle specific test flags
        if (strcmp(argv[1], "--test-kernel") == 0) {
            test_kernel_core();
        } else if (strcmp(argv[1], "--test-linux") == 0) {
            test_linux_bridge();
        } else if (strcmp(argv[1], "--test-windows") == 0) {
            test_windows_bridge();
        } else if (strcmp(argv[1], "--test-kcl") == 0) {
            test_kcl_interpreter();
        } else if (strcmp(argv[1], "--test-conflicts") == 0) {
            test_conflict_resolver();
        } else if (strcmp(argv[1], "--test-security") == 0) {
            test_security_engine();
        } else if (strcmp(argv[1], "--test-packages") == 0) {
            test_package_manager();
        } else if (strcmp(argv[1], "--test-integration") == 0) {
            test_integration();
        } else {
            printf("Unknown test flag: %s\n", argv[1]);
            return 1;
        }
        return 0;
    }
    
    printf("Kurono OS Test Suite\n");
    printf("Usage: %s --test [specific-test]\n", argv[0]);
    printf("Available tests:\n");
    printf("  --test              Run all tests\n");
    printf("  --test-kernel       Test kernel core\n");
    printf("  --test-linux        Test Linux bridge\n");
    printf("  --test-windows      Test Windows bridge\n");
    printf("  --test-kcl          Test KCL interpreter\n");
    printf("  --test-conflicts    Test conflict resolver\n");
    printf("  --test-security     Test security engine\n");
    printf("  --test-packages     Test package manager\n");
    printf("  --test-integration  Test full integration\n");
    
    return 0;
}