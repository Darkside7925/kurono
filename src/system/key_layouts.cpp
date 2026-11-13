// key_layouts: see key_layouts.h. (satoru)
#include "key_layouts.h"
#include "ui_config.h"
#include "../drivers/keyboard.h"
#include "../kernel/pmm.h"

extern "C" {
extern const uint8_t _binary_keymaps_bin_start[] __attribute__((weak));
extern const uint8_t _binary_keymaps_bin_end[] __attribute__((weak));
}

namespace {

// the main key block, in the order the tables below use. (satoru)
const int kKeys[47] = {
    KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LBRACKET, KEY_RBRACKET, KEY_BACKSLASH,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_QUOTE,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_PERIOD, KEY_SLASH,
};

// \x01 = this key has no ascii character on the layout at that level. (satoru)
struct Table { const char* base; const char* shift; const char* altgr; };

const Table kTables[] = {
    // us: never consulted (MapChar returns -1 and the driver keeps its own map) (satoru)
    { nullptr, nullptr, nullptr },
    // gb (satoru)
    { "`1234567890-=" "qwertyuiop[]#" "asdfghjkl;'" "zxcvbnm,./",
      "\x01!\"\x01$%^&*()_+" "QWERTYUIOP{}~" "ASDFGHJKL:@" "ZXCVBNM<>?",
      nullptr },
    // de (satoru)
    { "^1234567890\x01\x01" "qwertzuiop\x01+#" "asdfghjkl\x01\x01" "yxcvbnm,.-",
      "\x01!\"\x01$%&/()=?`" "QWERTZUIOP\x01*'" "ASDFGHJKL\x01\x01" "YXCVBNM;:_",
      "\x01\x01\x01\x01\x01\x01\x01{[]}\\\x01" "@\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01~\x01"
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" },
    // fr (satoru)
    { "\x01&\x01\"'(-\x01_\x01\x01)=" "azertyuiop^$*" "qsdfghjklm\x01" "wxcvbn,;:!",
      "\x01" "1234567890\x01+" "AZERTYUIOP\x01\x01\x01" "QSDFGHJKLM%" "WXCVBN?./\x01",
      "\x01\x01~#{[|`\\^@]}" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" },
    // es (satoru)
    { "\x01" "1234567890'\x01" "qwertyuiop`+\x01" "asdfghjkl\x01\x01" "zxcvbnm,.-",
      "\x01!\"\x01$%&/()=?\x01" "QWERTYUIOP^*\x01" "ASDFGHJKL\x01\x01" "ZXCVBNM;:_",
      "\\|@#~\x01\x01\x01\x01\x01\x01\x01\x01" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01[]}"
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01{" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" },
    // it (satoru)
    { "\\1234567890'\x01" "qwertyuiop\x01+\x01" "asdfghjkl\x01\x01" "zxcvbnm,.-",
      "|!\"\x01$%&/()=?^" "QWERTYUIOP\x01*\x01" "ASDFGHJKL\x01\x01" "ZXCVBNM;:_",
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01[]\x01"
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01@#" "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01" },
};
static_assert(sizeof(kTables) / sizeof(kTables[0]) == KeyLayouts::kLayoutCount, "one table per layout");

int      g_cur = 0;
uint32_t g_ver = 0xFFFFFFFFu;

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

}  // namespace

namespace KeyLayouts {

int Current() {
    // ui.conf lookups are linear: re-read only when ui.conf changed (every Set
    // and Reload bumps the version). no timer here: this runs in the key path
    // from early boot on. (satoru)
    uint32_t v = UIConfig::Version();
    if (v != g_ver) {
        g_ver = v;
        const char* id = UIConfig::Str("kbd.layout", "us");
        int found = 0;
        for (int i = 0; i < kLayoutCount; i++) if (same(kLayouts[i].id, id)) { found = i; break; }
        g_cur = found;
    }
    return g_cur;
}

int MapChar(int key, bool shift, bool caps, bool altgr) {
    int li = Current();
    if (li <= 0 || li >= kLayoutCount) return -1;
    const Table& t = kTables[li];
    int idx = -1;
    for (int i = 0; i < 47; i++) if (kKeys[i] == key) { idx = i; break; }
    if (idx < 0) return -1;
    const char* row = altgr ? t.altgr : (shift ? t.shift : t.base);
    if (!row) return altgr ? 0 : -1;
    char c = row[idx];
    if (c == '\x01') return 0;
    if (!altgr && caps) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return (unsigned char)c;
}

const uint8_t* WaylandKeymap(uint32_t* len) {
    int li = Current();
    if (li <= 0 || !_binary_keymaps_bin_start || !_binary_keymaps_bin_end) return nullptr;
    static const uint8_t* cache[kLayoutCount];
    static uint32_t cache_len[kLayoutCount];
    if (cache[li]) { *len = cache_len[li]; return cache[li]; }
    const uint8_t* b = _binary_keymaps_bin_start;
    uint32_t total = (uint32_t)(_binary_keymaps_bin_end - _binary_keymaps_bin_start);
    auto rd = [&](uint32_t o) -> uint32_t {
        return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    };
    if (total < 8 || rd(0) != 0x31504D4Bu) return nullptr;
    uint32_t n = rd(4);
    for (uint32_t i = 0; i < n && 8 + (i + 1) * 16 <= total; i++) {
        const char* nm = (const char*)(b + 8 + i * 16);
        char name[9];
        for (int k = 0; k < 8; k++) name[k] = nm[k];
        name[8] = 0;
        if (!same(name, kLayouts[li].id)) continue;
        uint32_t off = rd(8 + i * 16 + 8), l = rd(8 + i * 16 + 12);
        if (off + l > total || l < 2) return nullptr;
        // clients mmap from the base of a memfd: the copy has to start on a page. (satoru)
        uint8_t* p = (uint8_t*)PMM::AllocBytes(l);
        if (!p) return nullptr;
        for (uint32_t k = 0; k < l; k++) p[k] = b[off + k];
        cache[li] = p; cache_len[li] = l;
        *len = l;
        return p;
    }
    return nullptr;
}

}  // namespace KeyLayouts

// end (satoru)
