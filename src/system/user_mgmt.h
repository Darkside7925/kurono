#pragma once
#include "../kernel/types.h"

struct User {
    char username[32];
    char password_hash[32]; // Simple hash for demo
    bool has_profile_pic;
    // Profile pic data would be stored elsewhere or linked
};

class UserManager {
public:
    static const int MAX_USERS = 8;
    static User users[MAX_USERS];
    static int user_count;
    
    static void Init() {
        user_count = 0;
        // Mock default user
        // AddUser("admin", "password");
    }
    
    static bool AddUser(const char* username, const char* password) {
        if (user_count >= MAX_USERS) return false;
        // Basic copy
        int i = 0;
        while(username[i] && i < 31) { users[user_count].username[i] = username[i]; i++; }
        users[user_count].username[i] = 0;
        
        // Mock hash
        // In real OS, use proper hashing
        i = 0;
        while(password[i] && i < 31) { users[user_count].password_hash[i] = password[i]; i++; }
        users[user_count].password_hash[i] = 0;
        
        users[user_count].has_profile_pic = false;
        user_count++;
        return true;
    }
    
    static bool Validate(const char* username, const char* password) {
        for (int i = 0; i < user_count; i++) {
            bool match = true;
            // Check username
            const char* u1 = users[i].username;
            const char* u2 = username;
            while(*u1 && *u2) { if (*u1 != *u2) { match = false; break; } u1++; u2++; }
            if (match && *u1 == 0 && *u2 == 0) {
                // Check password
                const char* p1 = users[i].password_hash;
                const char* p2 = password;
                while(*p1 && *p2) { if (*p1 != *p2) return false; p1++; p2++; }
                return *p1 == 0 && *p2 == 0;
            }
        }
        return false;
    }
    
    static int GetUserCount() { return user_count; }
};
