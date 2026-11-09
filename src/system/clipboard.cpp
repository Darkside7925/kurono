#include "clipboard.h"
#include "../kernel/heap.h"
#include "../proc/spinlock.h"

// 64 kb backing store lives in bss; starts empty. (satoru)
char     ClipboardManager::buffer[ClipboardManager::CAPACITY];
uint32_t ClipboardManager::length = 0;

namespace {

struct MimeBlob {
    char     name[ClipboardManager::MAX_MIME_NAME];
    uint8_t* data;
    uint32_t len;
};

struct Slot {
    MimeBlob mimes[ClipboardManager::MAX_MIME];
    int      count;
    bool     replacing;
    uint32_t serial;
};

struct HistEntry {
    bool     used;
    bool     primary;
    uint32_t serial;
    uint32_t len;
    char     preview[80];
    uint8_t* text;
};

Slot g_clip;
Slot g_primary;
HistEntry g_hist[ClipboardManager::MAX_HISTORY];
int g_hist_count = 0;
int g_hist_head = 0;                 // newest entry slot (satoru)
uint32_t g_serial_next = 1;
char g_empty_text[1] = { 0 };
ClipboardManager::ChangeHook g_hook = nullptr;
Spinlock g_lock;

bool str_eq_local(const char* a, const char* b);

bool is_text_mime(const char* mime) {
    if (!mime) return false;
    return str_eq_local(mime, "text/plain") ||
           str_eq_local(mime, "text/plain;charset=utf-8") ||
           str_eq_local(mime, "UTF8_STRING");
}

const char* canon_mime(const char* mime) {
    if (is_text_mime(mime)) return "text/plain;charset=utf-8";
    return mime;
}

uint32_t str_len_local(const char* s) { uint32_t n = 0; while (s && s[n]) n++; return n; }

bool str_eq_local(const char* a, const char* b) {
    if (!a || !b) return false;
    uint32_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; i++; }
    return a[i] == 0 && b[i] == 0;
}

void free_slot(Slot& s) {
    for (int i = 0; i < s.count; i++) {
        if (s.mimes[i].data) KernelHeap::Free(s.mimes[i].data);
        s.mimes[i].data = nullptr;
        s.mimes[i].len = 0;
        s.mimes[i].name[0] = 0;
    }
    s.count = 0;
}

MimeBlob* find_blob(Slot& s, const char* mime) {
    const char* m = canon_mime(mime);
    for (int i = 0; i < s.count; i++) {
        if (str_eq_local(s.mimes[i].name, m)) return &s.mimes[i];
    }
    return nullptr;
}

// caller holds g_lock. (satoru)
bool slot_add(Slot& s, const char* mime, const void* data, uint32_t len) {
    if (!mime || !mime[0] || len > ClipboardManager::MAX_BLOB) return false;
    const char* m = canon_mime(mime);
    uint32_t nlen = str_len_local(m);
    if (nlen == 0 || nlen >= ClipboardManager::MAX_MIME_NAME) return false;

    MimeBlob* b = find_blob(s, m);
    if (!b) {
        if (s.count >= (int)ClipboardManager::MAX_MIME) return false;
        b = &s.mimes[s.count++];
        b->data = nullptr;
        b->len = 0;
        for (uint32_t i = 0; i < nlen; i++) b->name[i] = m[i];
        b->name[nlen] = 0;
    }

    uint8_t* copy = nullptr;
    if (data && len) {
        copy = (uint8_t*)KernelHeap::Alloc(len);
        if (!copy) return false;
        const uint8_t* src = (const uint8_t*)data;
        for (uint32_t i = 0; i < len; i++) copy[i] = src[i];
    }
    if (b->data) KernelHeap::Free(b->data);
    b->data = copy;
    b->len = len;
    return true;
}

void slot_clear(Slot& s) {
    free_slot(s);
    s.replacing = false;
}

// caller holds g_lock. text history is clipboard-only (never primary). (satoru)
void history_push(const Slot& s) {
    const MimeBlob* b = nullptr;
    for (int i = 0; i < s.count; i++) {
        if (str_eq_local(s.mimes[i].name, "text/plain;charset=utf-8")) { b = &s.mimes[i]; break; }
    }
    if (!b || !b->data || !b->len) return;

    uint32_t keep = b->len;
    if (keep > ClipboardManager::HISTORY_TEXT) keep = ClipboardManager::HISTORY_TEXT;

    // skip an exact repeat of the newest entry so ctrl+c twice is one row. (satoru)
    if (g_hist_count > 0) {
        HistEntry& top = g_hist[g_hist_head];
        if (top.used && top.len == keep) {
            bool same = true;
            for (uint32_t i = 0; i < keep; i++) {
                if (top.text[i] != b->data[i]) { same = false; break; }
            }
            if (same) return;
        }
    }

    int slot;
    if (g_hist_count < (int)ClipboardManager::MAX_HISTORY) {
        slot = (g_hist_head + g_hist_count) % ClipboardManager::MAX_HISTORY;
        g_hist_count++;
    } else {
        slot = g_hist_head;
        g_hist_head = (g_hist_head + 1) % ClipboardManager::MAX_HISTORY;
        if (g_hist[slot].text) KernelHeap::Free(g_hist[slot].text);
        g_hist[slot].text = nullptr;
    }
    HistEntry& e = g_hist[slot];
    e.text = (uint8_t*)KernelHeap::Alloc(keep ? keep : 1);
    if (!e.text) { e.used = false; return; }
    for (uint32_t i = 0; i < keep; i++) e.text[i] = b->data[i];
    e.used = true;
    e.primary = false;
    e.serial = s.serial;
    e.len = keep;
    int p = 0;
    uint32_t preview = keep > 76 ? 76 : keep;
    for (uint32_t i = 0; i < preview; i++) {
        uint8_t c = b->data[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 32) c = ' ';
        e.preview[p++] = (char)c;
    }
    if (keep > preview) { e.preview[p++] = '.'; e.preview[p++] = '.'; e.preview[p++] = '.'; }
    e.preview[p] = 0;
    g_hist_head = slot;
}

}  // namespace

// caller holds g_lock. publish the slot's text into the legacy fixed view. (satoru)
void ClipboardManager::RefreshTextViewLocked() {
    MimeBlob* b = find_blob(g_clip, "text/plain;charset=utf-8");
    uint32_t n = (b && b->data) ? b->len : 0;
    if (n >= CAPACITY) n = CAPACITY - 1;
    for (uint32_t i = 0; i < n; i++) buffer[i] = (char)b->data[i];
    buffer[n] = 0;
    length = n;
}

namespace {

void fire_hook(bool primary) {
    ClipboardManager::ChangeHook h = g_hook;
    if (h) h(primary);
}

}  // namespace

void ClipboardManager::SetText(const char* s) {
    if (!s) { Clear(); return; }
    BeginReplace(false);
    uint32_t n = str_len_local(s);
    AddMime("text/plain;charset=utf-8", s, n);
    CommitReplace(false, true);
}

const char* ClipboardManager::GetText() {
    SpinLockGuard g(g_lock);
    if (length == 0) buffer[0] = 0;
    return buffer;
}

void ClipboardManager::Clear() {
    {
        SpinLockGuard g(g_lock);
        slot_clear(g_clip);
        RefreshTextViewLocked();
    }
    fire_hook(false);
}

bool ClipboardManager::HasText() {
    SpinLockGuard g(g_lock);
    return length > 0;
}

uint32_t ClipboardManager::Length() {
    SpinLockGuard g(g_lock);
    return length;
}

bool ClipboardManager::SetData(const char* mime, const void* data, uint32_t len) {
    bool ok;
    {
        SpinLockGuard g(g_lock);
        ok = slot_add(g_clip, mime, data, len);
        if (ok && is_text_mime(mime)) RefreshTextViewLocked();
    }
    if (ok && is_text_mime(mime)) fire_hook(false);
    return ok;
}

const void* ClipboardManager::GetData(const char* mime, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    SpinLockGuard g(g_lock);
    MimeBlob* b = find_blob(g_clip, mime);
    if (!b) return nullptr;
    if (out_len) *out_len = b->len;
    return b->data;
}

bool ClipboardManager::CopyData(const char* mime, uint8_t** out, uint32_t* out_len) {
    if (out) *out = nullptr;
    if (out_len) *out_len = 0;
    if (!out || !out_len) return false;
    SpinLockGuard g(g_lock);
    MimeBlob* b = find_blob(g_clip, mime);
    if (!b || !b->data || !b->len) return false;
    uint8_t* copy = (uint8_t*)KernelHeap::Alloc(b->len);
    if (!copy) return false;
    for (uint32_t i = 0; i < b->len; i++) copy[i] = b->data[i];
    *out = copy;
    *out_len = b->len;
    return true;
}

void ClipboardManager::SetPrimaryText(const char* s) {
    BeginReplace(true);
    if (s) AddMime("text/plain;charset=utf-8", s, str_len_local(s));
    CommitReplace(true, true);
}

const char* ClipboardManager::GetPrimaryText() {
    SpinLockGuard g(g_lock);
    MimeBlob* b = find_blob(g_primary, "text/plain;charset=utf-8");
    if (!b || !b->data || !b->len) return g_empty_text;
    return (const char*)b->data;
}

bool ClipboardManager::HasPrimaryText() {
    SpinLockGuard g(g_lock);
    MimeBlob* b = find_blob(g_primary, "text/plain;charset=utf-8");
    return b && b->data && b->len > 0;
}

uint32_t ClipboardManager::PrimaryLength() {
    SpinLockGuard g(g_lock);
    MimeBlob* b = find_blob(g_primary, "text/plain;charset=utf-8");
    return (b && b->data) ? b->len : 0;
}

void ClipboardManager::BeginReplace(bool primary) {
    SpinLockGuard g(g_lock);
    Slot& s = primary ? g_primary : g_clip;
    slot_clear(s);
    s.replacing = true;
    if (!primary) RefreshTextViewLocked();
}

bool ClipboardManager::AddMime(const char* mime, const void* data, uint32_t len) {
    return AddMimeFor(false, mime, data, len);
}

bool ClipboardManager::AddMimeFor(bool primary, const char* mime, const void* data, uint32_t len) {
    SpinLockGuard g(g_lock);
    Slot& s = primary ? g_primary : g_clip;
    bool ok = slot_add(s, mime, data, len);
    if (ok && !primary && is_text_mime(mime)) RefreshTextViewLocked();
    return ok;
}

void ClipboardManager::CommitReplace(bool primary, bool notify) {
    {
        SpinLockGuard g(g_lock);
        Slot& s = primary ? g_primary : g_clip;
        s.replacing = false;
        s.serial = g_serial_next++;
        if (!primary) {
            RefreshTextViewLocked();
            history_push(s);
        }
    }
    if (notify) fire_hook(primary);
}

int ClipboardManager::HistoryCount() {
    SpinLockGuard g(g_lock);
    return g_hist_count;
}

bool ClipboardManager::HistoryTextAt(int index, char* out, int max, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    if (!out || max <= 0) return false;
    out[0] = 0;
    SpinLockGuard g(g_lock);
    if (index < 0 || index >= g_hist_count) return false;
    int slot = (g_hist_head - index + MAX_HISTORY) % MAX_HISTORY;
    HistEntry& e = g_hist[slot];
    if (!e.used || !e.text) return false;
    uint32_t n = e.len;
    if (out_len) *out_len = n;
    int copy = (int)((n > (uint32_t)(max - 1)) ? (uint32_t)(max - 1) : n);
    for (int i = 0; i < copy; i++) out[i] = (char)e.text[i];
    out[copy] = 0;
    return true;
}

uint32_t ClipboardManager::HistorySerialAt(int index) {
    SpinLockGuard g(g_lock);
    if (index < 0 || index >= g_hist_count) return 0;
    int slot = (g_hist_head - index + MAX_HISTORY) % MAX_HISTORY;
    return g_hist[slot].used ? g_hist[slot].serial : 0;
}

bool ClipboardManager::HistoryActivate(int index) {
    uint8_t* text = nullptr;
    uint32_t n = 0;
    {
        SpinLockGuard g(g_lock);
        if (index < 0 || index >= g_hist_count) return false;
        int slot = (g_hist_head - index + MAX_HISTORY) % MAX_HISTORY;
        HistEntry& e = g_hist[slot];
        if (!e.used || !e.text) return false;
        text = (uint8_t*)KernelHeap::Alloc(e.len ? e.len : 1);
        if (!text) return false;
        for (uint32_t i = 0; i < e.len; i++) text[i] = e.text[i];
        n = e.len;
    }
    BeginReplace(false);
    AddMime("text/plain;charset=utf-8", text, n);
    KernelHeap::Free(text);
    CommitReplace(false, true);
    return true;
}

void ClipboardManager::SetChangeHook(ChangeHook hook) {
    g_hook = hook;
}

// end (satoru)
