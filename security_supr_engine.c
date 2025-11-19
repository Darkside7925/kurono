#include "security_supr_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <openssl/sha.h>
#include "linux_sync.h"

#define SUPR_TIMEOUT_SECONDS 900
#define MAX_USERS 1000
#define MAX_DESCRIPTORS 10000

static SecuritySuprEngine* g_security_engine = NULL;
static const char* get_base(void) {
    const char* b = getenv("KURONO_BASE");
    if (!b || strlen(b) == 0) return "D:\\OS\\Kurono OS";
    return b;
}
static void build_users_path(char* out, size_t sz) {
    snprintf(out, sz, "%s\\Users\\users.json", get_base());
}

SecuritySuprEngine* security_supr_engine_create(void) {
    if (g_security_engine) return g_security_engine;
    
    SecuritySuprEngine* engine = (SecuritySuprEngine*)malloc(sizeof(SecuritySuprEngine));
    if (!engine) return NULL;
    
    engine->current_user = NULL;
    engine->users = (UserAccount**)malloc(sizeof(UserAccount*) * MAX_USERS);
    engine->user_count = 0;
    engine->user_capacity = MAX_USERS;
    engine->descriptors = (SecurityDescriptor**)malloc(sizeof(SecurityDescriptor*) * MAX_DESCRIPTORS);
    engine->descriptor_count = 0;
    engine->descriptor_capacity = MAX_DESCRIPTORS;
    engine->supr_mode = false;
    engine->supr_expires = 0;
    
    // Load existing users if present
    char upath[512];
    build_users_path(upath, sizeof(upath));
    security_supr_engine_load_users(engine, upath);
    if (engine->user_count == 0) {
        security_supr_engine_create_user(engine, "root", "toor", true);
        security_supr_engine_create_user(engine, "admin", "admin123", true);
        build_users_path(upath, sizeof(upath));
        security_supr_engine_save_users(engine, upath);
    }
    
    g_security_engine = engine;
    return engine;
}

void security_supr_engine_destroy(SecuritySuprEngine* engine) {
    if (!engine) return;
    
    for (size_t i = 0; i < engine->user_count; i++) {
        free(engine->users[i]->username);
        free(engine->users[i]->password_hash);
        free(engine->users[i]);
    }
    
    for (size_t i = 0; i < engine->descriptor_count; i++) {
        free(engine->descriptors[i]->resource_path);
        free(engine->descriptors[i]->owner);
        free(engine->descriptors[i]);
    }
    
    free(engine->users);
    free(engine->descriptors);
    free(engine);
    
    if (engine == g_security_engine) {
        g_security_engine = NULL;
    }
}

bool security_supr_engine_authenticate(SecuritySuprEngine* engine, const char* username, const char* password) {
    if (!engine || !username || !password) return false;
    
    UserAccount* user = security_supr_engine_get_user(engine, username);
    if (!user || !user->is_active) return false;
    
    if (!security_supr_engine_verify_password(password, user->password_hash)) return false;
    
    engine->current_user = user;
    user->last_login = time(NULL);
    
    return true;
}

bool security_supr_engine_create_user(SecuritySuprEngine* engine, const char* username, const char* password, bool is_admin) {
    if (!engine || !username || !password) return false;
    
    if (engine->user_count >= engine->user_capacity) return false;
    
    if (security_supr_engine_get_user(engine, username) != NULL) return false;
    
    UserAccount* user = (UserAccount*)malloc(sizeof(UserAccount));
    if (!user) return false;
    
    user->username = strdup(username);
    user->password_hash = security_supr_engine_hash_password(password);
    user->is_admin = is_admin;
    user->is_active = true;
    user->created_at = time(NULL);
    user->last_login = 0;
    
    engine->users[engine->user_count++] = user;
    linux_sync_create_user(username, is_admin);
    char upath2[512];
    build_users_path(upath2, sizeof(upath2));
    security_supr_engine_save_users(engine, upath2);
    
    return true;
}

bool security_supr_engine_delete_user(SecuritySuprEngine* engine, const char* username) {
    if (!engine || !username) return false;
    
    if (strcmp(username, "root") == 0) return false;
    
    for (size_t i = 0; i < engine->user_count; i++) {
        if (strcmp(engine->users[i]->username, username) == 0) {
            free(engine->users[i]->username);
            free(engine->users[i]->password_hash);
            free(engine->users[i]);
            
            for (size_t j = i; j < engine->user_count - 1; j++) {
                engine->users[j] = engine->users[j + 1];
            }
            
            engine->user_count--;
            linux_sync_delete_user(username);
            char upath3[512];
            build_users_path(upath3, sizeof(upath3));
            security_supr_engine_save_users(engine, upath3);
            return true;
        }
    }
    
    return false;
}

bool security_supr_engine_enable_supr(SecuritySuprEngine* engine, const char* password) {
    if (!engine || !password) return false;
    
    if (!engine->current_user || !engine->current_user->is_admin) return false;
    
    if (!security_supr_engine_verify_password(password, engine->current_user->password_hash)) return false;
    
    engine->supr_mode = true;
    engine->supr_expires = time(NULL) + SUPR_TIMEOUT_SECONDS;
    
    return true;
}

bool security_supr_engine_disable_supr(SecuritySuprEngine* engine) {
    if (!engine) return false;
    
    engine->supr_mode = false;
    engine->supr_expires = 0;
    
    return true;
}

bool security_supr_engine_is_supr_active(SecuritySuprEngine* engine) {
    if (!engine) return false;
    
    if (!engine->supr_mode) return false;
    
    time_t current_time = time(NULL);
    if (current_time > engine->supr_expires) {
        engine->supr_mode = false;
        engine->supr_expires = 0;
        return false;
    }
    
    return true;
}

bool security_supr_engine_check_permission(SecuritySuprEngine* engine, const char* resource, PermissionFlags required_perms) {
    if (!engine || !resource) return false;
    
    if (security_supr_engine_is_supr_active(engine)) return true;
    
    if (!engine->current_user) return false;
    
    SecurityDescriptor* descriptor = security_supr_engine_get_descriptor(engine, resource);
    if (!descriptor) {
        // Default permissions if no descriptor exists
        return (required_perms & (PERM_READ | PERM_EXECUTE)) != 0;
    }
    
    PermissionFlags effective_perms = (PermissionFlags)0;
    
    if (strcmp(engine->current_user->username, descriptor->owner) == 0) {
        effective_perms = descriptor->owner_perms;
    } else if (engine->current_user->is_admin) {
        effective_perms = (PermissionFlags)(descriptor->group_perms | descriptor->owner_perms);
    } else {
        effective_perms = descriptor->other_perms;
    }
    
    return (effective_perms & required_perms) == required_perms;
}

bool security_supr_engine_set_permissions(SecuritySuprEngine* engine, const char* resource, PermissionFlags owner, PermissionFlags group, PermissionFlags other) {
    if (!engine || !resource) return false;
    
    if (!security_supr_engine_is_supr_active(engine) && 
        (!engine->current_user || !engine->current_user->is_admin)) {
        return false;
    }
    
    SecurityDescriptor* descriptor = security_supr_engine_get_descriptor(engine, resource);
    if (!descriptor) {
        return security_supr_engine_add_descriptor(engine, resource, engine->current_user ? engine->current_user->username : "root", (PermissionFlags)(owner | group | other));
    }
    
    descriptor->owner_perms = owner;
    descriptor->group_perms = group;
    descriptor->other_perms = other;
    
    return true;
}

SecurityDescriptor* security_supr_engine_get_descriptor(SecuritySuprEngine* engine, const char* resource) {
    if (!engine || !resource) return NULL;
    
    for (size_t i = 0; i < engine->descriptor_count; i++) {
        if (strcmp(engine->descriptors[i]->resource_path, resource) == 0) {
            return engine->descriptors[i];
        }
    }
    
    return NULL;
}

bool security_supr_engine_add_descriptor(SecuritySuprEngine* engine, const char* resource, const char* owner, PermissionFlags perms) {
    if (!engine || !resource || !owner) return false;
    
    if (engine->descriptor_count >= engine->descriptor_capacity) return false;
    
    if (security_supr_engine_get_descriptor(engine, resource) != NULL) return false;
    
    SecurityDescriptor* descriptor = (SecurityDescriptor*)malloc(sizeof(SecurityDescriptor));
    if (!descriptor) return false;
    
    descriptor->resource_path = strdup(resource);
    descriptor->owner = strdup(owner);
    descriptor->owner_perms = perms;
    descriptor->group_perms = (PermissionFlags)(perms & (PERM_READ | PERM_EXECUTE));
    descriptor->other_perms = (PermissionFlags)(perms & PERM_READ);
    descriptor->requires_root = (perms & PERM_ADMIN) != 0;
    
    engine->descriptors[engine->descriptor_count++] = descriptor;
    
    return true;
}

UserAccount* security_supr_engine_get_user(SecuritySuprEngine* engine, const char* username) {
    if (!engine || !username) return NULL;
    
    for (size_t i = 0; i < engine->user_count; i++) {
        if (strcmp(engine->users[i]->username, username) == 0) {
            return engine->users[i];
        }
    }
    
    return NULL;
}

bool security_supr_engine_change_password(SecuritySuprEngine* engine, const char* username, const char* old_password, const char* new_password) {
    if (!engine || !username || !old_password || !new_password) return false;
    
    UserAccount* user = security_supr_engine_get_user(engine, username);
    if (!user) return false;
    
    if (!security_supr_engine_verify_password(old_password, user->password_hash)) return false;
    
    free(user->password_hash);
    user->password_hash = security_supr_engine_hash_password(new_password);
    char upath4[512];
    build_users_path(upath4, sizeof(upath4));
    security_supr_engine_save_users(engine, upath4);
    
    return true;
}

char* security_supr_engine_hash_password(const char* password) {
    if (!password) return NULL;
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)password, strlen(password), hash);
    
    char* hash_str = (char*)malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash_str + (i * 2), "%02x", hash[i]);
    }
    hash_str[SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return hash_str;
}

bool security_supr_engine_verify_password(const char* password, const char* hash) {
    if (!password || !hash) return false;
    
    char* computed_hash = security_supr_engine_hash_password(password);
    bool result = (strcmp(computed_hash, hash) == 0);
    free(computed_hash);
    
    return result;
}

bool security_supr_engine_save_users(SecuritySuprEngine* engine, const char* path) {
    if (!engine || !path) return false;
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"users\": [\n");
    for (size_t i = 0; i < engine->user_count; i++) {
        UserAccount* u = engine->users[i];
        fprintf(f, "    {\"username\": \"%s\", \"hash\": \"%s\", \"admin\": %s, \"active\": %s}%s\n",
                u->username, u->password_hash, u->is_admin ? "true" : "false", u->is_active ? "true" : "false",
                (i + 1 < engine->user_count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return true;
}

bool security_supr_engine_load_users(SecuritySuprEngine* engine, const char* path) {
    if (!engine || !path) return false;
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* ukey = strstr(line, "\"username\": ");
        if (ukey) {
            char uname[128] = {0};
            char* start = strchr(ukey, '"');
            if (start) {
                start++;
                char* end = strchr(start, '"');
                if (end) {
                    size_t len = (size_t)(end - start);
                    memcpy(uname, start, len);
                    uname[len] = '\0';
                }
            }
            // simplistic: set default password "imported"
            if (strlen(uname) > 0 && !security_supr_engine_get_user(engine, uname)) {
                security_supr_engine_create_user(engine, uname, "imported", false);
            }
        }
    }
    fclose(f);
    return true;
}

bool security_supr_engine_import_linux_passwd(SecuritySuprEngine* engine, const char* path) {
    if (!engine || !path) return false;
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Format: name:passwd:uid:gid:gecos:home:shell
        char name[128] = {0};
        char* p = strchr(line, ':');
        if (!p) continue;
        size_t nlen = (size_t)(p - line);
        if (nlen == 0 || nlen >= sizeof(name)) continue;
        memcpy(name, line, nlen);
        name[nlen] = '\0';
        if (!security_supr_engine_get_user(engine, name)) {
            security_supr_engine_create_user(engine, name, "imported", false);
        }
    }
    fclose(f);
    security_supr_engine_save_users(engine, "D:\\Important\\Kurono\\Users\\users.json");
    return true;
}