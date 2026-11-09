#pragma once
#include "../kernel/types.h"

// system clipboard - single shared text buffer backed by a fixed RAM
// region. bounded copies, nul-terminated, no dynamic allocation. (satoru)
//
// mime-aware store on top of the original text path: one clipboard slot and
// one primary-selection slot, each holding a few mime blobs (text, html,
// png), plus a text history ring for the super+v popup. payloads are heap
// copies bounded by MAX_BLOB; the text view stays nul terminated. (satoru)

class ClipboardManager {
public:
    // capacity of the backing buffer including the terminating nul. the
    // largest string returned by GetText() is CAPACITY-1 bytes. (satoru)
    static const uint32_t CAPACITY = 64 * 1024;

    // mime store bounds. (satoru)
    static const uint32_t MAX_MIME       = 8;
    static const uint32_t MAX_MIME_NAME  = 64;
    static const uint32_t MAX_BLOB       = 8u * 1024u * 1024u;
    static const uint32_t MAX_HISTORY    = 16;
    static const uint32_t HISTORY_TEXT   = 256u * 1024u;

    // replace clipboard contents with s (copied, bounded to CAPACITY-1).
    // passing nullptr clears the clipboard. (satoru)
    static void SetText(const char* s);

    // current clipboard text; never null - empty string when nothing is
    // stored. valid until the next SetText/Clear. (satoru)
    static const char* GetText();

    // empty the clipboard. (satoru)
    static void Clear();

    // true when GetText() would return a non-empty string. (satoru)
    static bool HasText();

    // byte length of the stored text (excluding the nul). (satoru)
    static uint32_t Length();

    // mime-aware access. AddMime replaces one mime inside the current slot;
    // GetData aliases the three text mime spellings. CopyData returns a heap
    // copy the caller frees. (satoru)
    static bool SetData(const char* mime, const void* data, uint32_t len);
    static const void* GetData(const char* mime, uint32_t* out_len);
    static bool CopyData(const char* mime, uint8_t** out, uint32_t* out_len);

    // primary selection (middle click). text only. (satoru)
    static void SetPrimaryText(const char* s);
    static const char* GetPrimaryText();
    static bool HasPrimaryText();
    static uint32_t PrimaryLength();

    // batched replacement used by the wayland server while it pulls a new
    // selection: BeginReplace drops the old slot, AddMime fills it, and
    // CommitReplace publishes it (and optionally notifies the compositor). (satoru)
    static void BeginReplace(bool primary);
    static bool AddMime(const char* mime, const void* data, uint32_t len);
    static bool AddMimeFor(bool primary, const char* mime, const void* data, uint32_t len);
    static void CommitReplace(bool primary, bool notify);

    // history: newest first. HistoryTextAt copies at most max-1 bytes plus a
    // nul and reports the full length. HistoryActivate puts an entry back on
    // the clipboard without adding a duplicate history record. (satoru)
    static int  HistoryCount();
    static bool HistoryTextAt(int index, char* out, int max, uint32_t* out_len);
    static uint32_t HistorySerialAt(int index);
    static bool HistoryActivate(int index);

    // compositor hook: fired after a native copy changed the system selection.
    // never called with the clipboard lock held. (satoru)
    typedef void (*ChangeHook)(bool primary);
    static void SetChangeHook(ChangeHook hook);

private:
    static char     buffer[CAPACITY];
    static uint32_t length;
    static void     RefreshTextViewLocked();   // caller holds the store lock (satoru)
};

// end (satoru)
