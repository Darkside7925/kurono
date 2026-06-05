#pragma once
#include "../kernel/types.h"

// system clipboard  -  single shared text buffer backed by a fixed RAM
// region. bounded copies, nul-terminated, no dynamic allocation. (satoru)

class ClipboardManager {
public:
    // capacity of the backing buffer including the terminating nul. the
    // largest string returned by GetText() is CAPACITY-1 bytes. (satoru)
    static const uint32_t CAPACITY = 64 * 1024;

    // replace clipboard contents with s (copied, bounded to CAPACITY-1).
    // passing nullptr clears the clipboard. (satoru)
    static void SetText(const char* s);

    // current clipboard text; never null  -  empty string when nothing is
    // stored. valid until the next SetText/Clear. (satoru)
    static const char* GetText();

    // empty the clipboard. (satoru)
    static void Clear();

    // true when GetText() would return a non-empty string. (satoru)
    static bool HasText();

    // byte length of the stored text (excluding the nul). (satoru)
    static uint32_t Length();

private:
    static char     buffer[CAPACITY];
    static uint32_t length;
};

// end (satoru)
