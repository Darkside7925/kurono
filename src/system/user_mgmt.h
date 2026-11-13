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
    // stored hashes are "p2$<iterations>$<64 hex>" (pbkdf2-hmac-sha256 over the
    // salt) since 2026-11; a plain 64 hex value is the old sha-256 form, still
    // accepted and upgraded at the next successful sign-in. (satoru)
    static bool VerifyPassword(const char* salt, const char* plaintext, const char* stored);
    static bool IsLegacyHash(const char* stored);
    // ken / the admin gate: true when `password` is the password of any
    // administrator account. (satoru)
    static bool VerifyAdminPassword(const char* password);
    // ksa hashes a typed credential inside its isolated region and hands back
    // only a 32 byte digest; these two let it compare that digest against an
    // account without the plaintext ever leaving the region. (satoru)
    static bool CredentialDigest(const char* username, const char* plaintext, uint8_t out[32]);
    static bool DigestMatches(const char* username, const uint8_t digest[32]);
    static const char* FirstAdminName();
    static bool CurrentIsAdmin();
    // account changes. SetAdmin refuses to demote the last administrator;
    // ChangePassword needs the old password. both persist. (satoru)
    static bool SetAdmin(const char* username, bool admin);
    static bool ChangePassword(const char* username, const char* old_pw, const char* new_pw);
    // failed sign-ins: after 5 in a row an account waits 30 s, doubling. (satoru)
    static uint32_t LockoutRemainingMs(const char* username);

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
