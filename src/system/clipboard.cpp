#include "clipboard.h"

// 64 kb backing store lives in bss; starts empty. (satoru)
char     ClipboardManager::buffer[ClipboardManager::CAPACITY];
uint32_t ClipboardManager::length = 0;

void ClipboardManager::SetText(const char* s) {
    if (!s) { Clear(); return; }

    // bounded copy: leave room for the terminating nul. (satoru)
    uint32_t i = 0;
    const uint32_t cap = CAPACITY - 1;
    while (s[i] && i < cap) {
        buffer[i] = s[i];
        i++;
    }
    buffer[i] = 0;
    length = i;
}

const char* ClipboardManager::GetText() {
    // guarantee a valid c-string even before the first SetText. (satoru)
    if (length == 0) buffer[0] = 0;
    return buffer;
}

void ClipboardManager::Clear() {
    buffer[0] = 0;
    length = 0;
}

bool ClipboardManager::HasText() {
    return length > 0;
}

uint32_t ClipboardManager::Length() {
    return length;
}

// end (satoru)
