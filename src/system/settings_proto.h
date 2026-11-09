// settings_proto.h: the fixed wire format between a ring-3 settings app and the kernel
// settings service. both ends include this header; it is freestanding c. (satoru)
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KSS_MAGIC        0x4B535356u   // "KSSV" request (satoru)
#define KSS_REPLY_MAGIC  0x4B535652u   // "KSSR" reply header (satoru)
#define KSS_TABLE_MAGIC  0x4B535454u   // "KSST" key/value table (satoru)
#define KSS_PATH         "/system/run/user/1000/settings-svc"

#define KSS_KEY_MAX   48
#define KSS_VAL_MAX   128
#define KSS_ARG_MAX   160
#define KSS_ENTRY_MAX 128   // one wire chunk; GET_INFO is requested in chunks (satoru)
#define KSS_INFO_MAX  640   // the whole info table the service builds (satoru)

enum KssKind : uint32_t {
    KSS_GET_CFG  = 1,   // reply payload: KssTable of ui.conf keys (satoru)
    KSS_GET_INFO = 2,   // reply payload: KssTable of read-only system info (satoru)
    KSS_SET      = 3,   // key + val: validate, persist and apply one setting (satoru)
    KSS_ACTION   = 4,   // action + arg: one privileged or live side effect (satoru)
};

struct KssEntry {
    char key[KSS_KEY_MAX];
    char val[KSS_VAL_MAX];
};

struct KssTable {
    uint32_t magic;
    uint32_t count;
    KssEntry entries[KSS_ENTRY_MAX];
};

struct KssReq {
    uint32_t magic;
    uint32_t kind;
    char     key[KSS_KEY_MAX];
    char     val[KSS_VAL_MAX];
    char     action[KSS_KEY_MAX];
    char     arg[KSS_ARG_MAX];
    int32_t  ival;
    uint32_t uval;
    int32_t  a[4];
    int32_t  b[4];
    uint32_t onmask;
};

struct KssReply {
    uint32_t magic;
    uint32_t ok;
    char     why[64];
};

// end (satoru)
