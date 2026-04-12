#include "user_mgmt.h"

User UserManager::users[UserManager::MAX_USERS];
int UserManager::user_count = 0;
int UserManager::current_user = -1;
