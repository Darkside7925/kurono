#include "package_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <time.h>

static const char* default_packages[] = {
    "coreutils", "bash", "vim", "nano", "git", "curl", "wget", "python3",
    "nodejs", "npm", "docker", "kubernetes", "ansible", "terraform",
    "kcl-core", "kcl-utils", "kcl-dev", "kcl-admin", "kcl-security",
    "powershell-core", "windows-utils", "pe-loader", "registry-tools",
    NULL
};

PackageManager* package_manager_create(const char* cache_dir) {
    if (!cache_dir) return NULL;
    
    PackageManager* pm = (PackageManager*)malloc(sizeof(PackageManager));
    if (!pm) return NULL;
    
    pm->packages = (Package**)malloc(sizeof(Package*) * 1000);
    pm->package_count = 0;
    pm->package_capacity = 1000;
    pm->repository_url = strdup("https://packages.kurono-os.org/repo");
    pm->cache_directory = strdup(cache_dir);
    pm->auto_update = true;
    
    // Create cache directory
    struct stat st;
    if (stat(cache_dir, &st) != 0) {
#ifdef _WIN32
        _mkdir(cache_dir);
#else
        mkdir(cache_dir, 0755);
#endif
    }
    
    return pm;
}

void package_manager_destroy(PackageManager* pm) {
    if (!pm) return;
    
    for (size_t i = 0; i < pm->package_count; i++) {
        Package* pkg = pm->packages[i];
        free(pkg->name);
        free(pkg->version);
        free(pkg->description);
        free(pkg->author);
        free(pkg->homepage);
        free(pkg->download_url);
        free(pkg->install_script);
        free(pkg->uninstall_script);
        free(pkg);
    }
    
    free(pm->packages);
    free(pm->repository_url);
    free(pm->cache_directory);
    free(pm);
}

bool package_manager_install(PackageManager* pm, const char* package_name) {
    if (!pm || !package_name) return false;
    
    Package* existing = package_manager_get_package(pm, package_name);
    if (existing && existing->status == PKG_STATUS_INSTALLED) {
        return true; // Already installed
    }
    
    // Simulate package installation
    Package* pkg = (Package*)malloc(sizeof(Package));
    if (!pkg) return false;
    
    pkg->name = strdup(package_name);
    pkg->version = strdup("1.0.0");
    pkg->description = (char*)malloc(256);
    snprintf(pkg->description, 256, "Kurono OS package: %s", package_name);
    pkg->author = strdup("Kurono OS Team");
    pkg->homepage = strdup("https://kurono-os.org");
    pkg->download_url = (char*)malloc(256);
    snprintf(pkg->download_url, 256, "%s/%s-%s.kpkg", pm->repository_url, package_name, pkg->version);
    pkg->install_script = strdup("#!/bin/bash\necho 'Installing package...'");
    pkg->uninstall_script = strdup("#!/bin/bash\necho 'Uninstalling package...'");
    pkg->type = PKG_TYPE_UNIVERSAL;
    pkg->status = PKG_STATUS_INSTALLED;
    pkg->size = 1024 * 1024; // 1MB default
    pkg->installed_at = time(NULL);
    pkg->updated_at = time(NULL);
    
    if (pm->package_count >= pm->package_capacity) {
        pm->package_capacity *= 2;
        pm->packages = (Package**)realloc(pm->packages, sizeof(Package*) * pm->package_capacity);
    }
    
    pm->packages[pm->package_count++] = pkg;
    
    return true;
}

bool package_manager_remove(PackageManager* pm, const char* package_name) {
    if (!pm || !package_name) return false;
    
    for (size_t i = 0; i < pm->package_count; i++) {
        if (strcmp(pm->packages[i]->name, package_name) == 0) {
            Package* pkg = pm->packages[i];
            pkg->status = PKG_STATUS_REMOVED;
            pkg->updated_at = time(NULL);
            
            // Remove from array
            for (size_t j = i; j < pm->package_count - 1; j++) {
                pm->packages[j] = pm->packages[j + 1];
            }
            pm->package_count--;
            
            // Clean up package data
            free(pkg->name);
            free(pkg->version);
            free(pkg->description);
            free(pkg->author);
            free(pkg->homepage);
            free(pkg->download_url);
            free(pkg->install_script);
            free(pkg->uninstall_script);
            free(pkg);
            
            return true;
        }
    }
    
    return false;
}

bool package_manager_update(PackageManager* pm, const char* package_name) {
    if (!pm || !package_name) return false;
    
    Package* pkg = package_manager_get_package(pm, package_name);
    if (!pkg) return false;
    
    // Simulate update by incrementing version
    char* old_version = pkg->version;
    int major, minor, patch;
    sscanf(old_version, "%d.%d.%d", &major, &minor, &patch);
    patch++;
    
    pkg->version = (char*)malloc(16);
    snprintf(pkg->version, 16, "%d.%d.%d", major, minor, patch);
    pkg->updated_at = time(NULL);
    
    free(old_version);
    return true;
}

bool package_manager_upgrade(PackageManager* pm) {
    if (!pm) return false;
    
    bool success = true;
    for (size_t i = 0; i < pm->package_count; i++) {
        if (pm->packages[i]->status == PKG_STATUS_INSTALLED) {
            if (!package_manager_update(pm, pm->packages[i]->name)) {
                success = false;
            }
        }
    }
    
    return success;
}

Package* package_manager_get_package(PackageManager* pm, const char* package_name) {
    if (!pm || !package_name) return NULL;
    
    for (size_t i = 0; i < pm->package_count; i++) {
        if (strcmp(pm->packages[i]->name, package_name) == 0) {
            return pm->packages[i];
        }
    }
    
    return NULL;
}

Package** package_manager_list_packages(PackageManager* pm, size_t* count) {
    if (!pm || !count) return NULL;
    
    *count = pm->package_count;
    return pm->packages;
}

Package** package_manager_search_packages(PackageManager* pm, const char* query, size_t* count) {
    if (!pm || !query || !count) return NULL;
    
    *count = 0;
    Package** results = (Package**)malloc(sizeof(Package*) * pm->package_count);
    
    for (size_t i = 0; i < pm->package_count; i++) {
        if (strstr(pm->packages[i]->name, query) != NULL ||
            strstr(pm->packages[i]->description, query) != NULL) {
            results[(*count)++] = pm->packages[i];
        }
    }
    
    return results;
}

bool package_manager_refresh_repositories(PackageManager* pm) {
    if (!pm) return false;
    
    // Simulate repository refresh
    printf("Refreshing package repositories from %s...\n", pm->repository_url);
    return true;
}

bool package_manager_add_repository(PackageManager* pm, const char* repo_url) {
    if (!pm || !repo_url) return false;
    
    free(pm->repository_url);
    pm->repository_url = strdup(repo_url);
    
    return true;
}

bool package_manager_remove_repository(PackageManager* pm, const char* repo_url) {
    if (!pm || !repo_url) return false;
    
    if (strcmp(pm->repository_url, repo_url) == 0) {
        free(pm->repository_url);
        pm->repository_url = strdup("https://packages.kurono-os.org/repo");
        return true;
    }
    
    return false;
}

bool package_manager_register_kurono_packages(PackageManager* pm, CommandRegistry* registry) {
    if (!pm || !registry) return false;
    
    for (int i = 0; default_packages[i] != NULL; i++) {
        if (!package_manager_get_package(pm, default_packages[i])) {
            package_manager_install(pm, default_packages[i]);
        }
    }
    
    return true;
}

char* package_manager_get_cache_path(PackageManager* pm, const char* package_name) {
    if (!pm || !package_name) return NULL;
    
    char* cache_path = (char*)malloc(strlen(pm->cache_directory) + strlen(package_name) + 16);
    sprintf(cache_path, "%s/%s.kpkg", pm->cache_directory, package_name);
    
    return cache_path;
}

bool package_manager_clear_cache(PackageManager* pm) {
    if (!pm) return false;
    
    // Simulate cache clearing
    printf("Clearing package cache in %s...\n", pm->cache_directory);
    return true;
}

bool package_manager_validate_package(Package* pkg) {
    if (!pkg) return false;
    
    if (!pkg->name || strlen(pkg->name) == 0) return false;
    if (!pkg->version || strlen(pkg->version) == 0) return false;
    if (!pkg->description || strlen(pkg->description) == 0) return false;
    if (!pkg->download_url || strlen(pkg->download_url) == 0) return false;
    
    return true;
}