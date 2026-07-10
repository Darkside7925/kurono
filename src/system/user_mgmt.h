#pragma once
#include "../kernel/types.h"

// Real per-user record.  Hash is hex SHA-256 of (salt || password)
// (lower-case hex, 64 chars + nul).
struct User {
    char username[32];
    char display_name[48];
    char password_hash[80];   // hex sha256 of salt+pw
    char salt[24];            // 8 random bytes -> 16 hex chars
    int  avatar_id;           // 0..7 default avatar (or -1 for custom)
    char avatar_path[96];     // path inside KVFS to a custom image (optional)
    uint32_t accent_color;    // 0xAARRGGBB
    bool has_pin;
    char pin_hash[80];
    bool auto_login;
    char timezone[32];
    char language[16];
    bool is_admin;
};

class UserManager {
public:
    static const int MAX_USERS = 16;
    static User users[MAX_USERS];
    static int  user_count;
    static int  current_user;

    static void Init();                  // load from /etc/passwd if present
    static void PersistToDisk();         // write /etc/passwd, /etc/shadow, /etc/kurono.conf

    // Authentication
    static bool Login(const char* username, const char* password);
    static bool LoginByPin(const char* username, const char* pin);
    static void Logout();

    // Registration / lifecycle
    static bool AddUser(const char* username, const char* password); // legacy 2-arg
    static bool RegisterUser(const User& u, const char* plaintext_password);
    static bool RemoveUser(const char* username);
    static User* FindByName(const char* username);

    // Real password hashing (SHA-256 of salt||password, hex)
    static void HashPassword(const char* salt, const char* plaintext, char* out_hex_64);
    static void GenerateSalt(char* out_hex_16);

    // Convenience accessors
    static const char* GetCurrentUsername();
    static const char* GetCurrentDisplayName();
    static int         GetCurrentUserIndex();
    static int         GetUserCount();

    // Username/password validation rules (used by registration UI)
    enum PwdStrength { PWD_WEAK = 0, PWD_FAIR = 1, PWD_STRONG = 2, PWD_VERY_STRONG = 3 };
    static int MeasurePassword(const char* p);
    static bool IsUsernameValid(const char* u);   // 3-31 chars, [a-z0-9_-], starts with letter
    static bool IsUsernameTaken(const char* u);
};
