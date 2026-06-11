#ifndef LINUX_SYNC_H
#define LINUX_SYNC_H

#include <stdbool.h>

bool linux_sync_create_user(const char* username, bool is_admin);
bool linux_sync_delete_user(const char* username);
bool linux_sync_sync_from_linux(void);

#endif