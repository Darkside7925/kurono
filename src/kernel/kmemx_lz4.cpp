#include "kmemx_lz4.h"

//  freestanding lz4 block-format codec  -  see kmemx_lz4.h. (satoru)
//
//  block format recap (so the bounds logic below reads clearly): a stream is a
//  series of sequences. each sequence is one token byte split into two nibbles:
//    high nibble = literal length (0..15), low nibble = match length - 4 (0..15).
//  if a nibble == 15 the real length continues in extra bytes (each 0..255,
//  summed, terminated by a byte < 255). after the literals comes a 2-byte
//  little-endian match offset (1..65535) UNLESS the literal run is the final run
//  of the block (then there is no match). the match copies `matchlen` bytes from
//  `offset` behind the current output cursor (overlap-allowed). (satoru)
//
//  end-of-block constraints we honour on compress so any spec decoder (and ours)
//  decodes us correctly: the last 5 bytes are always literals, and no match may
//  begin within the last 12 bytes. (satoru)

namespace KMemXLZ4 {

// ── small freestanding helpers (no libc) ────────────────────────────────────
static inline uint32_t read_u32(const uint8_t* p) {
    // unaligned 32-bit read; x86_64 permits it. (satoru)
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// the canonical lz4 4-byte hash: multiply + shift down to HASH_LOG bits. (satoru)
static inline uint32_t hash4(uint32_t seq) {
    return (seq * 2654435761u) >> (32 - HASH_LOG);
}

// lz4 block constants. (satoru)
constexpr int MIN_MATCH   = 4;    // shortest encodable match (satoru)
constexpr int LAST_LIT    = 5;    // trailing bytes forced to be literals (satoru)
constexpr int MF_LIMIT    = 12;   // no match may start in the last 12 bytes (satoru)
constexpr int ML_BITS     = 4;
constexpr int ML_MASK     = (1 << ML_BITS) - 1;
constexpr int RUN_MASK    = (1 << (8 - ML_BITS)) - 1;  // 15 (satoru)

// ── sse2 copy helpers (hand-written asm; compiler-generated code is -mno-sse,
//    but sse2 is enabled in cr0/cr4 at boot so explicit intrinsics-via-asm are
//    fine  -  the framebuffer copy already relies on this). these accelerate the
//    decompressor's literal + long-distance match copies, the hot bytes of the
//    fault path. (satoru)
static inline void sse_copy16(uint8_t* d, const uint8_t* s) {
    __asm__ __volatile__("movdqu (%1), %%xmm0\n\t movdqu %%xmm0, (%0)"
                         :: "r"(d), "r"(s) : "xmm0", "memory");
}
// copy `n` bytes d<-s where the regions DO NOT overlap within 16 bytes (literals,
// or a match whose offset >= 16). writes in 16-byte chunks and is allowed to
// OVERWRITE up to 15 bytes past the logical end, so callers must guarantee >=16
// bytes of destination slack; the precise byte-exact tail is handled by the
// caller's slow path when slack is insufficient. (satoru)
static inline void sse_wildcopy(uint8_t* d, const uint8_t* s, int n) {
    int i = 0;
    do { sse_copy16(d + i, s + i); i += 16; } while (i < n);
}

// ── compression ──────────────────────────────────────────────────────────────
int Compress(const uint8_t* src, int src_len,
             uint8_t* dst, int dst_cap, void* scratch) {
    if (src_len < 0 || dst_cap <= 0) return 0;

    uint32_t* table = (uint32_t*)scratch;
    // 0 is a valid offset only for position 0; use a sentinel so an empty bucket
    // never matches. we store position+1 and treat 0 as "empty". (satoru)
    for (int i = 0; i < HASH_SIZE; i++) table[i] = 0;

    const uint8_t* ip = src;            // input cursor (satoru)
    const uint8_t* const iend = src + src_len;
    const uint8_t* anchor = src;        // start of the pending literal run (satoru)
    uint8_t* op = dst;                  // output cursor (satoru)
    uint8_t* const oend = dst + dst_cap;

    // inputs too small to ever hold a match are emitted as one literal run. the
    // mf_limit guard below also needs src_len > MF_LIMIT to look for matches. (satoru)
    if (src_len < MF_LIMIT + 1) {
        goto emit_last_literals;
    }

    // prime the table with the first position. (satoru)
    table[hash4(read_u32(ip))] = (uint32_t)(ip - src) + 1;
    ip++;

    for (;;) {
        const uint8_t* match = nullptr;
        const uint8_t* search = ip;
        uint32_t h;

        // find the next 4-byte match, advancing with an acceleration step so
        // incompressible data does not cost O(n) probes per byte. (satoru)
        int step = 1;
        int search_match_fails = 0;
        for (;;) {
            ip = search;
            if (ip > iend - MF_LIMIT) goto emit_last_literals;
            h = hash4(read_u32(ip));
            uint32_t cand = table[h];
            table[h] = (uint32_t)(ip - src) + 1;
            if (cand != 0) {
                match = src + (cand - 1);
                // verify the 4 bytes actually match (hash collisions happen) and
                // the offset is in range (1..65535). (satoru)
                if ((ip - match) <= 65535 && read_u32(match) == read_u32(ip)) {
                    break;
                }
            }
            search_match_fails++;
            step = 1 + (search_match_fails >> 6);   // accelerate after misses (satoru)
            search = ip + step;
        }

        // we have a match at `ip` referencing `match`. first walk backwards to
        // extend the match over already-emitted literals (cheap ratio win). (satoru)
        while (ip > anchor && match > src && ip[-1] == match[-1]) {
            ip--; match--;
        }

        // ── emit the literal run [anchor, ip) ──
        int lit_len = (int)(ip - anchor);
        // worst-case space for this sequence: token + lit-extra + literals +
        // offset. check conservatively so we never write past oend. (satoru)
        {
            int need = 1 + (lit_len / 255) + lit_len + 2;
            if (op + need > oend) return 0;   // does not fit -> store uncompressed (satoru)
        }
        uint8_t* token = op++;
        if (lit_len >= RUN_MASK) {
            *token = (uint8_t)(RUN_MASK << ML_BITS);
            int rem = lit_len - RUN_MASK;
            while (rem >= 255) { *op++ = 255; rem -= 255; }
            *op++ = (uint8_t)rem;
        } else {
            *token = (uint8_t)(lit_len << ML_BITS);
        }
        for (int i = 0; i < lit_len; i++) *op++ = anchor[i];

        // ── emit the match ──
        // offset (little-endian, always >= 1 because match < ip here). (satoru)
        uint32_t offset = (uint32_t)(ip - match);
        *op++ = (uint8_t)(offset & 0xFF);
        *op++ = (uint8_t)((offset >> 8) & 0xFF);

        // match length: count bytes equal beyond the MIN_MATCH already
        // confirmed, stopping LAST_LIT before the end. (satoru)
        {
            const uint8_t* m = match + MIN_MATCH;
            const uint8_t* s = ip + MIN_MATCH;
            const uint8_t* match_limit = iend - LAST_LIT;
            while (s < match_limit && *s == *m) { s++; m++; }
            int match_len = (int)(s - ip) - MIN_MATCH;  // value stored is len-MIN_MATCH (satoru)

            // worst case for the match-length extension bytes. (satoru)
            if (op + (match_len / 255) + 1 > oend) return 0;
            if (match_len >= ML_MASK) {
                *token |= ML_MASK;
                int rem = match_len - ML_MASK;
                while (rem >= 255) { *op++ = 255; rem -= 255; }
                *op++ = (uint8_t)rem;
            } else {
                *token |= (uint8_t)match_len;
            }

            ip = s;        // advance past the whole match (satoru)
            anchor = ip;   // the next literal run starts here (satoru)
        }

        if (ip > iend - MF_LIMIT) goto emit_last_literals;

        // insert a hash for the position just before ip so an overlapping match
        // can still be found, then resume the outer search from ip. (a re-entrant
        // "chain into the next match" shortcut was removed: it could re-read its
        // own freshly-inserted table entry and emit an illegal offset-0 match.)
        // the outer loop immediately re-searches from ip, so this is both correct
        // and still fast. (satoru)
        table[hash4(read_u32(ip - 2))] = (uint32_t)(ip - 2 - src) + 1;
    }

emit_last_literals:
    {
        int lit_len = (int)(iend - anchor);
        int need = 1 + (lit_len / 255) + lit_len;
        if (op + need > oend) return 0;
        uint8_t* token = op++;
        if (lit_len >= RUN_MASK) {
            *token = (uint8_t)(RUN_MASK << ML_BITS);
            int rem = lit_len - RUN_MASK;
            while (rem >= 255) { *op++ = 255; rem -= 255; }
            *op++ = (uint8_t)rem;
        } else {
            *token = (uint8_t)(lit_len << ML_BITS);
        }
        for (int i = 0; i < lit_len; i++) *op++ = anchor[i];
    }
    return (int)(op - dst);
}

// ── decompression ─────────────────────────────────────────────────────────────
//  every read from `src` and every write to `dst` is bounds-checked, so a
//  corrupted pool entry can only ever return -1 (the caller panics) and can never
//  scribble past the page buffer. correctness over raw speed: this is the fault
//  critical path but a 4 kb page decodes in well under a microsecond even with the
//  checks. (satoru)
int Decompress(const uint8_t* src, int src_len,
               uint8_t* dst, int dst_len) {
    if (src_len < 0 || dst_len < 0) return -1;
    const uint8_t* ip = src;
    const uint8_t* const iend = src + src_len;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dst_len;

    while (ip < iend) {
        uint8_t tok = *ip++;

        // ── literal length ──
        int lit_len = tok >> ML_BITS;
        if (lit_len == RUN_MASK) {
            uint8_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                lit_len += b;
            } while (b == 255);
        }

        // copy literals (bounds-checked). use the sse wildcopy when both the
        // source and destination have >= 16 bytes of slack past the run (so the
        // 16-byte overwrite stays in-bounds of both buffers); otherwise byte-wise.
        // (satoru)
        if (ip + lit_len > iend) return -1;
        if (op + lit_len > oend) return -1;
        if (lit_len > 0 && (iend - (ip + lit_len)) >= 16 && (oend - (op + lit_len)) >= 16) {
            sse_wildcopy(op, ip, lit_len);
            op += lit_len; ip += lit_len;
        } else {
            for (int i = 0; i < lit_len; i++) *op++ = *ip++;
        }

        // the final sequence is literals only  -  if we consumed the input, stop. (satoru)
        if (ip >= iend) break;

        // ── match offset ──
        if (ip + 2 > iend) return -1;
        uint32_t offset = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
        ip += 2;
        if (offset == 0) return -1;                     // offset 0 is illegal (satoru)
        uint8_t* mp = op - offset;
        if (mp < dst) return -1;                        // refers before the buffer (satoru)

        // ── match length ──
        int match_len = tok & ML_MASK;
        if (match_len == ML_MASK) {
            uint8_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                match_len += b;
            } while (b == 255);
        }
        match_len += MIN_MATCH;

        if (op + match_len > oend) return -1;
        // when the match distance is >= 16 the source and destination 16-byte
        // chunks never overlap, so the sse wildcopy is safe (given 16 bytes of
        // destination slack). otherwise fall back to the byte-wise copy, which is
        // correct even when offset < match_len (the rle/overlap case lz4 relies
        // on). (satoru)
        if (offset >= 16 && (oend - (op + match_len)) >= 16) {
            sse_wildcopy(op, mp, match_len);
            op += match_len;
        } else {
            for (int i = 0; i < match_len; i++) *op++ = *mp++;
        }
    }

    return (int)(op - dst);
}

// ── crc32 (reflected, poly 0xEDB88320), slice-by-8 ─────────────────────────────
// the crc runs over every full 4kb page on every compress AND every decompress,
// so it sits squarely on the fault latency path. a byte-at-a-time table loop is
// ~4096 dependent loads per page; slice-by-8 consumes 8 bytes per iteration from
// 8 precomputed tables (the classic intel slicing technique), cutting the loop
// count 8x while staying a pure-integer, sse-free computation. (satoru)
static uint32_t g_crc_table[8][256];
static bool g_crc_ready = false;

static void crc_build() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[0][i] = c;
    }
    // tables 1..7: each is the previous table's value run through one more byte
    // of the crc shift, so table[n][b] folds in a byte n positions back. (satoru)
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = g_crc_table[0][i];
        for (int t = 1; t < 8; t++) {
            c = g_crc_table[0][c & 0xFF] ^ (c >> 8);
            g_crc_table[t][i] = c;
        }
    }
    g_crc_ready = true;
}

uint32_t Crc32(const uint8_t* data, int len) {
    if (!g_crc_ready) crc_build();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = data;
    int n = len;

    // 8 bytes per iteration: read two little-endian 32-bit words, fold the
    // running crc into the low word, then index all 8 tables. (satoru)
    while (n >= 8) {
        uint32_t lo = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t hi = ((uint32_t)p[4]) | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        lo ^= crc;
        crc = g_crc_table[7][ lo        & 0xFF] ^
              g_crc_table[6][(lo >> 8)  & 0xFF] ^
              g_crc_table[5][(lo >> 16) & 0xFF] ^
              g_crc_table[4][(lo >> 24) & 0xFF] ^
              g_crc_table[3][ hi        & 0xFF] ^
              g_crc_table[2][(hi >> 8)  & 0xFF] ^
              g_crc_table[1][(hi >> 16) & 0xFF] ^
              g_crc_table[0][(hi >> 24) & 0xFF];
        p += 8; n -= 8;
    }
    // byte-wise tail. (satoru)
    while (n-- > 0) {
        crc = g_crc_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace KMemXLZ4

// end (satoru)
