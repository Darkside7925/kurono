// kui_launch_proto.h: the tiny request protocol ring-3 kui apps use to ask the desktop (satoru)
// to launch a registered app, open a file or run a .kcl script. fixed-size messages on a unix (satoru)
// socket at /system/run/user/1000/kui-launch, validated by both ends. (satoru)
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KUI_LAUNCH_MAGIC   0x4B4C4E43u   // "KLNC" (satoru)
#define KUI_LAUNCH_REPLY   0x4B4C5250u   // "KLRP" (satoru)
#define KUI_LAUNCH_PATH    "/system/run/user/1000/kui-launch"
#define KUI_LAUNCH_NAME_MAX 32
#define KUI_LAUNCH_ARG_MAX  256

enum KuiLaunchKind : uint32_t {
    KUI_LAUNCH_APP     = 1,   // name = registered app, arg = optional path (satoru)
    KUI_LAUNCH_OPEN    = 2,   // arg = file path, opens the default app for its type (satoru)
    KUI_LAUNCH_KCL     = 3,   // arg = .kcl script path, runs it in a terminal (satoru)
    KUI_LAUNCH_REFRESH = 4,   // no arguments, refreshes the desktop icons (satoru)
};

struct KuiLaunchMsg {
    uint32_t magic;
    uint32_t kind;
    uint32_t name_len;
    uint32_t arg_len;
    char     name[KUI_LAUNCH_NAME_MAX];
    char     arg[KUI_LAUNCH_ARG_MAX];
};

struct KuiLaunchReply {
    uint32_t magic;
    uint32_t ok;
    char     why[64];
};

// a registered app name: lowercase letters, digits, underscore and dash, starts with a letter (satoru)
static inline bool kui_launch_app_ok(const char* s) {
    if (!s || !s[0]) return false;
    if (!((s[0] >= 'a' && s[0] <= 'z'))) return false;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (i >= KUI_LAUNCH_NAME_MAX - 1) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

// an absolute path with no control bytes and no ".." component; spaces are allowed (satoru)
static inline bool kui_launch_path_ok(const char* s) {
    if (!s || s[0] != '/') return false;
    int n = 0;
    while (s[n]) {
        unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c == 0x7f) return false;
        if (n >= KUI_LAUNCH_ARG_MAX - 1) return false;
        n++;
    }
    if (!n) return false;
    for (int i = 0; s[i]; ) {
        while (s[i] == '/') i++;
        int b = i;
        while (s[i] && s[i] != '/') i++;
        if (i - b == 2 && s[b] == '.' && s[b + 1] == '.') return false;
    }
    return true;
}

// a script path is stricter: it is handed to the shell as `kcl <path>`, so only (satoru)
// letters, digits, slash, dot, underscore and dash are accepted. (satoru)
static inline bool kui_launch_script_ok(const char* s) {
    if (!kui_launch_path_ok(s)) return false;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '_' || c == '-')) return false;
    }
    int n = 0;
    while (s[n]) n++;
    if (n < 4) return false;
    const char* e = s + n - 4;
    return e[0] == '.' && (e[1] == 'k' || e[1] == 'K') && (e[2] == 'c' || e[2] == 'C') && (e[3] == 'l' || e[3] == 'L');
}

// end (satoru)
