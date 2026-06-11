#ifndef PACKAGE_MANAGER_H
#define PACKAGE_MANAGER_H

#include "kernel.h"
#include <stdbool.h>

typedef enum {
    PKG_STATUS_INSTALLED,
    PKG_STATUS_AVAILABLE,
    PKG_STATUS_UPGRADABLE,
    PKG_STATUS_BROKEN,
    PKG_STATUS_REMOVED
} PackageStatus;

typedef enum {
    PKG_TYPE_KCL,
    PKG_TYPE_LINUX,
    PKG_TYPE_WINDOWS,
    PKG_TYPE_UNIVERSAL
} PackageType;

typedef struct {
    char* name;
    char* version;
    char* description;
    char* author;
    char* homepage;
    char* download_url;
    char* install_script;
    char* uninstall_script;
    PackageType type;
    PackageStatus status;
    size_t size;
    time_t installed_at;
    time_t updated_at;
} Package;

typedef struct {
    Package** packages;
    size_t package_count;
    size_t package_capacity;
    char* repository_url;
    char* cache_directory;
    bool auto_update;
} PackageManager;

PackageManager* package_manager_create(const char* cache_dir);
void package_manager_destroy(PackageManager* pm);

bool package_manager_install(PackageManager* pm, const char* package_name);
bool package_manager_remove(PackageManager* pm, const char* package_name);
bool package_manager_update(PackageManager* pm, const char* package_name);
bool package_manager_upgrade(PackageManager* pm);

Package* package_manager_get_package(PackageManager* pm, const char* package_name);
Package** package_manager_list_packages(PackageManager* pm, size_t* count);
Package** package_manager_search_packages(PackageManager* pm, const char* query, size_t* count);

bool package_manager_refresh_repositories(PackageManager* pm);
bool package_manager_add_repository(PackageManager* pm, const char* repo_url);
bool package_manager_remove_repository(PackageManager* pm, const char* repo_url);

bool package_manager_register_kurono_packages(PackageManager* pm, CommandRegistry* registry);

char* package_manager_get_cache_path(PackageManager* pm, const char* package_name);
bool package_manager_clear_cache(PackageManager* pm);
bool package_manager_validate_package(Package* pkg);

#endif