#ifndef SECURITY_SUPR_ENGINE_H
#define SECURITY_SUPR_ENGINE_H

#include "kernel.h"
#include <stdbool.h>

typedef enum {
    PERM_READ = 1,
    PERM_WRITE = 2,
    PERM_EXECUTE = 4,
    PERM_DELETE = 8,
    PERM_ADMIN = 16
} PermissionFlags;

typedef struct {
    char* username;
    char* password_hash;
    bool is_admin;
    bool is_active;
    time_t created_at;
    time_t last_login;
} UserAccount;

typedef struct {
    char* resource_path;
    char* owner;
    PermissionFlags owner_perms;
    PermissionFlags group_perms;
    PermissionFlags other_perms;
    bool requires_root;
} SecurityDescriptor;

typedef struct {
    UserAccount* current_user;
    UserAccount** users;
    size_t user_count;
    size_t user_capacity;
    SecurityDescriptor** descriptors;
    size_t descriptor_count;
    size_t descriptor_capacity;
    bool supr_mode;
    time_t supr_expires;
} SecuritySuprEngine;

SecuritySuprEngine* security_supr_engine_create(void);
void security_supr_engine_destroy(SecuritySuprEngine* engine);

bool security_supr_engine_authenticate(SecuritySuprEngine* engine, const char* username, const char* password);
bool security_supr_engine_create_user(SecuritySuprEngine* engine, const char* username, const char* password, bool is_admin);
bool security_supr_engine_delete_user(SecuritySuprEngine* engine, const char* username);

bool security_supr_engine_enable_supr(SecuritySuprEngine* engine, const char* password);
bool security_supr_engine_disable_supr(SecuritySuprEngine* engine);
bool security_supr_engine_is_supr_active(SecuritySuprEngine* engine);

bool security_supr_engine_check_permission(SecuritySuprEngine* engine, const char* resource, PermissionFlags required_perms);
bool security_supr_engine_set_permissions(SecuritySuprEngine* engine, const char* resource, PermissionFlags owner, PermissionFlags group, PermissionFlags other);

SecurityDescriptor* security_supr_engine_get_descriptor(SecuritySuprEngine* engine, const char* resource);
bool security_supr_engine_add_descriptor(SecuritySuprEngine* engine, const char* resource, const char* owner, PermissionFlags perms);

UserAccount* security_supr_engine_get_user(SecuritySuprEngine* engine, const char* username);
bool security_supr_engine_change_password(SecuritySuprEngine* engine, const char* username, const char* old_password, const char* new_password);

char* security_supr_engine_hash_password(const char* password);
bool security_supr_engine_verify_password(const char* password, const char* hash);

bool security_supr_engine_save_users(SecuritySuprEngine* engine, const char* path);
bool security_supr_engine_load_users(SecuritySuprEngine* engine, const char* path);
bool security_supr_engine_import_linux_passwd(SecuritySuprEngine* engine, const char* path);

#endif