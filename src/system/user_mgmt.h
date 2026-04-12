#pragma once
#include "../kernel/types.h"

struct User {
    char username[32];
    char password_hash[32]; // simple hash for demo
    bool has_profile_pic;
    // profile pic data would be stored elsewhere or linked
};

class UserManager {
public:
    static const int MAX_USERS = 8;
    static User users[MAX_USERS];
    static int user_count;
    static int current_user;
    
    static void Init() {
        user_count = 0;
        current_user = -1;
        // mock default user
        // adduser("admin", "password");
    }
    
    static bool AddUser(const char* username, const char* password) {
        if (user_count >= MAX_USERS) return false;
        // basic copy
        int i = 0;
        while(username[i] && i < 31) { users[user_count].username[i] = username[i]; i++; }
        users[user_count].username[i] = 0;
        
        // mock hash
        // in real os, use proper hashing
        i = 0;
        while(password[i] && i < 31) { users[user_count].password_hash[i] = password[i]; i++; }
        users[user_count].password_hash[i] = 0;
        
        users[user_count].has_profile_pic = false;
        user_count++;
        return true;
    }

    static bool Login(const char* username, const char* password) {
        for (int i = 0; i < user_count; i++) {
            bool user_match = true;
            const char* stored_user = users[i].username;
            const char* input_user = username;
            while (*stored_user && *input_user) {
                if (*stored_user != *input_user) { user_match = false; break; }
                stored_user++; input_user++;
            }
            if (!user_match || *stored_user != 0 || *input_user != 0) continue;

            bool pass_match = true;
            const char* stored_pass = users[i].password_hash;
            const char* input_pass = password;
            while (*stored_pass && *input_pass) {
                if (*stored_pass != *input_pass) { pass_match = false; break; }
                stored_pass++; input_pass++;
            }
            if (pass_match && *stored_pass == 0 && *input_pass == 0) {
                current_user = i;
                return true;
            }
            return false;
        }
        return false;
    }
    
    static bool Validate(const char* username, const char* password) {
        for (int i = 0; i < user_count; i++) {
            bool match = true;
            // check username
            const char* u1 = users[i].username;
            const char* u2 = username;
            while(*u1 && *u2) { if (*u1 != *u2) { match = false; break; } u1++; u2++; }
            if (match && *u1 == 0 && *u2 == 0) {
                // check password
                const char* p1 = users[i].password_hash;
                const char* p2 = password;
                while(*p1 && *p2) { if (*p1 != *p2) return false; p1++; p2++; }
                return *p1 == 0 && *p2 == 0;
            }
        }
        return false;
    }

    static const char* GetCurrentUsername() {
        if (current_user >= 0 && current_user < user_count)
            return users[current_user].username;
        return "user";
    }

    static int GetCurrentUserIndex() {
        return current_user;
    }

    static void Logout() {
        current_user = -1;
    }
    
    static int GetUserCount() { return user_count; }
};
