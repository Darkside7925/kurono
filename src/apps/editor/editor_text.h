// editor_text.h: the document model behind the kurono text editor. one gap buffer per document,
// an incrementally maintained line start index, selection by absolute offset and a multi level
// undo stack of inverse edits. header only so both the kernel and the ring 3 build use it. (satoru)
#pragma once

#include "../../kui/kui.h"

#if defined(KUI_BACKEND_WAYLAND)
#include <string.h>
#else
#include "../../kernel/types.h"
#endif

namespace ed {

#define EDX_PATH_MAX 256
#define EDX_UNDO_MAX 400
#define EDX_UNDO_MERGE_MS 900

enum Lang {
    LANG_PLAIN = 0, LANG_C, LANG_CPP, LANG_PY, LANG_JS, LANG_SH, LANG_MD, LANG_JSON, LANG_KCL
};

struct UndoOp {
    int pos;
    int rem_len;
    int ins_len;
    char* rem;
    char* ins;
};

struct Doc {
    char* b;
    int cap;
    int gs;
    int ge;

    int* ls;
    int ls_cap;
    int nlines;

    char path[EDX_PATH_MAX];
    bool named;
    bool modified;

    int cur;
    int anchor;

    int top;
    int xcol;

    UndoOp* undo;
    int undo_n;
    int undo_cap;
    int undo_pos;

    unsigned char* hl;
    int hl_cap;
    int hl_valid;

    int lang;
    uint64_t saved_hash;
};

static inline void* edx_alloc(size_t n) { return kui::Alloc(n ? n : 1); }
static inline void edx_free(void* p) { if (p) kui::Free(p); }

static inline int edx_len(const Doc* d) { return d->cap - (d->ge - d->gs); }

static inline char edx_at(const Doc* d, int i) {
    if (i < 0 || i >= edx_len(d)) return 0;
    return i < d->gs ? d->b[i] : d->b[i + (d->ge - d->gs)];
}

static inline void edx_copy(const Doc* d, int pos, int n, char* out) {
    if (pos < 0) { n += pos; pos = 0; }
    int len = edx_len(d);
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    if (n <= 0) { if (out) out[0] = 0; return; }
    int a = pos < d->gs ? (n < d->gs - pos ? n : d->gs - pos) : 0;
    if (a > 0) memcpy(out, d->b + pos, (size_t)a);
    if (n - a > 0) {
        int so = (pos < d->gs) ? d->ge : d->ge + (pos - d->gs);
        memcpy(out + a, d->b + so, (size_t)(n - a));
    }
    out[n] = 0;
}

static inline uint64_t edx_hash(const Doc* d) {
    uint64_t h = 1469598103934665603ULL;
    int n = edx_len(d);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)edx_at(d, i);
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

static inline void edx_move_gap(Doc* d, int pos) {
    if (pos < 0) pos = 0;
    int len = edx_len(d);
    if (pos > len) pos = len;
    if (pos < d->gs) {
        int n = d->gs - pos;
        memmove(d->b + d->ge - n, d->b + pos, (size_t)n);
        d->ge -= n;
        d->gs = pos;
    } else if (pos > d->gs) {
        int n = pos - d->gs;
        memmove(d->b + d->gs, d->b + d->ge, (size_t)n);
        d->gs += n;
        d->ge += n;
    }
}

static inline void edx_ensure(Doc* d, int need) {
    if (d->ge - d->gs >= need) return;
    int len = edx_len(d);
    int free_now = d->ge - d->gs;
    int newcap = d->cap ? d->cap : 1024;
    while (newcap - len < need + 64) newcap *= 2;
    int suf = d->cap - d->ge;
    char* nb = (char*)edx_alloc((size_t)newcap);
    if (!nb) return;
    if (d->gs > 0) memcpy(nb, d->b, (size_t)d->gs);
    if (suf > 0) memcpy(nb + newcap - suf, d->b + d->ge, (size_t)suf);
    edx_free(d->b);
    d->b = nb;
    d->ge = newcap - suf;
    d->cap = newcap;
    (void)free_now;
}

static inline int edx_line_of(const Doc* d, int pos) {
    if (!d->ls || d->nlines <= 0) return 0;
    if (pos < 0) pos = 0;
    int lo = 0;
    int hi = d->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (d->ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static inline int edx_line_start(const Doc* d, int row) {
    if (row < 0) row = 0;
    if (row >= d->nlines) row = d->nlines - 1;
    return d->ls[row];
}

static inline int edx_line_len(const Doc* d, int row) {
    if (row < 0 || row >= d->nlines) return 0;
    int a = d->ls[row];
    int e = (row + 1 < d->nlines) ? d->ls[row + 1] - 1 : edx_len(d);
    if (e < a) e = a;
    return e - a;
}

static inline int edx_col(const Doc* d, int pos) {
    int row = edx_line_of(d, pos);
    int c = pos - d->ls[row];
    return c < 0 ? 0 : c;
}

static inline void edx_lines_reserve(Doc* d, int need) {
    if (d->ls_cap >= need) return;
    int nc = d->ls_cap ? d->ls_cap : 256;
    while (nc < need) nc *= 2;
    int* nl = (int*)edx_alloc(sizeof(int) * (size_t)nc);
    if (!nl) return;
    if (d->ls && d->nlines > 0) memcpy(nl, d->ls, sizeof(int) * (size_t)d->nlines);
    edx_free(d->ls);
    d->ls = nl;
    d->ls_cap = nc;
}

static inline void edx_lines_insert(Doc* d, int pos, const char* s, int n) {
    int li = edx_line_of(d, pos);
    int nl = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\n') nl++;
    if (!nl) {
        for (int i = li + 1; i < d->nlines; i++) d->ls[i] += n;
        return;
    }
    edx_lines_reserve(d, d->nlines + nl);
    for (int i = d->nlines - 1; i > li; i--) d->ls[i + nl] = d->ls[i] + n;
    int k = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\n') d->ls[li + 1 + (k++)] = pos + i + 1;
    d->nlines += nl;
}

static inline void edx_lines_erase(Doc* d, int pos, int n) {
    int end = pos + n;
    int li = edx_line_of(d, pos);
    int ri = edx_line_of(d, end);
    int rem = ri - li;
    for (int i = ri + 1; i < d->nlines; i++) d->ls[i - rem] = d->ls[i] - n;
    d->nlines -= rem;
    if (d->nlines < 1) {
        d->nlines = 1;
        edx_lines_reserve(d, 1);
        d->ls[0] = 0;
    }
}

static inline void edx_raw_erase(Doc* d, int pos, int n) {
    int len = edx_len(d);
    if (pos < 0) { n += pos; pos = 0; }
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    if (n <= 0) return;
    edx_lines_erase(d, pos, n);
    edx_move_gap(d, pos);
    d->ge += n;
}

static inline void edx_raw_insert(Doc* d, int pos, const char* s, int n) {
    if (n <= 0) return;
    int len = edx_len(d);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    edx_move_gap(d, pos);
    edx_ensure(d, n);
    if (d->ge - d->gs < n) return;
    memcpy(d->b + d->gs, s, (size_t)n);
    d->gs += n;
    edx_lines_insert(d, pos, s, n);
}

static inline void edx_raw_replace(Doc* d, int pos, int del, const char* ins, int ins_len) {
    if (del > 0) edx_raw_erase(d, pos, del);
    if (ins_len > 0) edx_raw_insert(d, pos, ins, ins_len);
}

static inline void edx_undo_free(UndoOp* u) {
    edx_free(u->rem);
    edx_free(u->ins);
    u->rem = nullptr;
    u->ins = nullptr;
}

static inline void edx_undo_reserve(Doc* d, int need) {
    if (d->undo_cap >= need) return;
    int nc = d->undo_cap ? d->undo_cap : 64;
    while (nc < need) nc *= 2;
    UndoOp* nu = (UndoOp*)edx_alloc(sizeof(UndoOp) * (size_t)nc);
    if (!nu) return;
    if (d->undo && d->undo_n > 0) memcpy(nu, d->undo, sizeof(UndoOp) * (size_t)d->undo_n);
    edx_free(d->undo);
    d->undo = nu;
    d->undo_cap = nc;
}

static inline void edx_undo_drop_redo(Doc* d) {
    for (int i = d->undo_pos; i < d->undo_n; i++) edx_undo_free(&d->undo[i]);
    d->undo_n = d->undo_pos;
}

static inline void edx_record(Doc* d, int pos, const char* removed, int rem_len,
                              const char* ins, int ins_len, bool merge) {
    edx_undo_drop_redo(d);
    UndoOp* last = d->undo_n > 0 ? &d->undo[d->undo_n - 1] : nullptr;
    if (merge && last && rem_len == 0 && last->rem_len == 0 && ins_len > 0 &&
        pos == last->pos + last->ins_len) {
        char* t = (char*)edx_alloc((size_t)(last->ins_len + ins_len + 1));
        if (t) {
            if (last->ins_len > 0) memcpy(t, last->ins, (size_t)last->ins_len);
            memcpy(t + last->ins_len, ins, (size_t)ins_len);
            t[last->ins_len + ins_len] = 0;
            edx_free(last->ins);
            last->ins = t;
            last->ins_len += ins_len;
            return;
        }
    }
    if (merge && last && ins_len == 0 && last->ins_len == 0 && rem_len > 0 &&
        pos + rem_len == last->pos) {
        char* t = (char*)edx_alloc((size_t)(last->rem_len + rem_len + 1));
        if (t) {
            memcpy(t, removed, (size_t)rem_len);
            if (last->rem_len > 0) memcpy(t + rem_len, last->rem, (size_t)last->rem_len);
            t[rem_len + last->rem_len] = 0;
            edx_free(last->rem);
            last->rem = t;
            last->rem_len += rem_len;
            last->pos = pos;
            return;
        }
    }
    edx_undo_reserve(d, d->undo_n + 1);
    if (d->undo_n >= d->undo_cap) return;
    UndoOp* u = &d->undo[d->undo_n];
    u->pos = pos;
    u->rem_len = rem_len;
    u->ins_len = ins_len;
    u->rem = nullptr;
    u->ins = nullptr;
    if (rem_len > 0) {
        u->rem = (char*)edx_alloc((size_t)rem_len + 1);
        if (u->rem) { memcpy(u->rem, removed, (size_t)rem_len); u->rem[rem_len] = 0; }
        else rem_len = u->rem_len = 0;
    }
    if (ins_len > 0) {
        u->ins = (char*)edx_alloc((size_t)ins_len + 1);
        if (u->ins) { memcpy(u->ins, ins, (size_t)ins_len); u->ins[ins_len] = 0; }
        else ins_len = u->ins_len = 0;
    }
    d->undo_n++;
    d->undo_pos = d->undo_n;
    if (d->undo_n > EDX_UNDO_MAX) {
        edx_undo_free(&d->undo[0]);
        memmove(&d->undo[0], &d->undo[1], sizeof(UndoOp) * (size_t)(d->undo_n - 1));
        d->undo_n--;
        d->undo_pos = d->undo_n;
    }
}

static inline void edx_invalidate_hl(Doc* d, int pos) {
    int row = edx_line_of(d, pos);
    if (row < d->hl_valid) d->hl_valid = row;
}

static inline void edx_replace(Doc* d, int pos, int del, const char* ins, int ins_len, bool merge) {
    int len = edx_len(d);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    if (del > len - pos) del = len - pos;
    if (del <= 0 && ins_len <= 0) return;
    char* removed = nullptr;
    if (del > 0) {
        removed = (char*)edx_alloc((size_t)del + 1);
        if (removed) edx_copy(d, pos, del, removed);
        else del = 0;
    }
    if (del > 0) edx_raw_erase(d, pos, del);
    if (ins_len > 0) edx_raw_insert(d, pos, ins, ins_len);
    edx_invalidate_hl(d, pos);
    d->modified = true;
    edx_record(d, pos, removed, del, ins, ins_len, merge);
    edx_free(removed);
    d->cur = pos + ins_len;
    d->anchor = d->cur;
}

static inline bool edx_undo(Doc* d) {
    if (d->undo_pos <= 0) return false;
    UndoOp* u = &d->undo[--d->undo_pos];
    edx_raw_replace(d, u->pos, u->ins_len, u->rem, u->rem_len);
    edx_invalidate_hl(d, u->pos);
    d->cur = u->pos + u->rem_len;
    d->anchor = d->cur;
    d->modified = (edx_hash(d) != d->saved_hash);
    return true;
}

static inline bool edx_redo(Doc* d) {
    if (d->undo_pos >= d->undo_n) return false;
    UndoOp* u = &d->undo[d->undo_pos++];
    edx_raw_replace(d, u->pos, u->rem_len, u->ins, u->ins_len);
    edx_invalidate_hl(d, u->pos);
    d->cur = u->pos + u->ins_len;
    d->anchor = d->cur;
    d->modified = (edx_hash(d) != d->saved_hash);
    return true;
}

static inline void edx_select(Doc* d, int pos, bool extend) {
    int len = edx_len(d);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    d->cur = pos;
    if (!extend) d->anchor = pos;
}

static inline bool edx_has_sel(const Doc* d) { return d->cur != d->anchor; }

static inline void edx_sel(const Doc* d, int* a, int* b) {
    int x = d->cur < d->anchor ? d->cur : d->anchor;
    int y = d->cur < d->anchor ? d->anchor : d->cur;
    *a = x;
    *b = y;
}

static inline bool edx_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static inline int edx_prev_char(const Doc* d, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)edx_at(d, pos) & 0xC0) == 0x80) pos--;
    return pos;
}

static inline int edx_next_char(const Doc* d, int pos) {
    int len = edx_len(d);
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)edx_at(d, pos) & 0xC0) == 0x80) pos++;
    return pos;
}

static inline int edx_word_left(const Doc* d, int pos) {
    int len = edx_len(d);
    if (pos > len) pos = len;
    while (pos > 0 && !edx_word_char(edx_at(d, pos - 1))) pos--;
    while (pos > 0 && edx_word_char(edx_at(d, pos - 1))) pos--;
    return pos;
}

static inline int edx_word_right(const Doc* d, int pos) {
    int len = edx_len(d);
    if (pos < 0) pos = 0;
    while (pos < len && !edx_word_char(edx_at(d, pos))) pos++;
    while (pos < len && edx_word_char(edx_at(d, pos))) pos++;
    return pos;
}

static inline void edx_select_word(Doc* d, int pos) {
    int len = edx_len(d);
    if (pos > len) pos = len;
    int a = pos;
    int b = pos;
    while (a > 0 && edx_word_char(edx_at(d, a - 1))) a--;
    while (b < len && edx_word_char(edx_at(d, b))) b++;
    if (a == b) {
        b = edx_next_char(d, b);
    }
    d->anchor = a;
    d->cur = b;
}

static inline void edx_clear_hl(Doc* d) {
    d->hl_valid = 0;
}

static inline void edx_init(Doc* d) {
    memset(d, 0, sizeof(*d));
    d->b = (char*)edx_alloc(1024);
    d->cap = 1024;
    d->gs = 0;
    d->ge = 1024;
    edx_lines_reserve(d, 1);
    d->nlines = 1;
    d->ls[0] = 0;
    d->saved_hash = edx_hash(d);
}

// header only shorthand for the common case: replace the active selection or insert at the cursor. (satoru)
static inline void edx_insert_text(Doc* d, const char* s, int n, bool merge) {
    int a, b;
    if (edx_has_sel(d)) {
        edx_sel(d, &a, &b);
        edx_replace(d, a, b - a, s, n, false);
    } else {
        edx_replace(d, d->cur, 0, s, n, merge);
    }
}

static inline void edx_delete_sel(Doc* d) {
    int a, b;
    if (!edx_has_sel(d)) return;
    edx_sel(d, &a, &b);
    edx_replace(d, a, b - a, nullptr, 0, false);
}

static inline void edx_free(Doc* d) {
    for (int i = 0; i < d->undo_n; i++) edx_undo_free(&d->undo[i]);
    edx_free(d->undo);
    edx_free(d->hl);
    edx_free(d->ls);
    edx_free(d->b);
    memset(d, 0, sizeof(*d));
}

static inline void edx_clear(Doc* d) {
    for (int i = 0; i < d->undo_n; i++) edx_undo_free(&d->undo[i]);
    d->undo_n = 0;
    d->undo_pos = 0;
    d->gs = 0;
    d->ge = d->cap;
    d->nlines = 1;
    d->ls[0] = 0;
    d->cur = 0;
    d->anchor = 0;
    d->top = 0;
    d->xcol = 0;
    d->modified = false;
    d->hl_valid = 0;
    d->saved_hash = edx_hash(d);
}

}  // namespace ed

// end (satoru)
