//  kurono os - wayland bridge (see wlbridge.h)
#include "wlbridge.h"
#include "vcon.h"
#include "guest_mem.h"
#include "../net/unix_socket.h"
#include "../linux/linux_syscall.h"
#include "../ui/wayland_server.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"
#include "../kernel/types.h"
#include "../system/clipboard.h"

namespace {
    enum {
        T_HELLO = 1, T_OPEN = 2, T_CLOSE = 3, T_DATA = 4, T_POOL = 5, T_SYNC = 6,
        T_RESIZE = 7, T_FREE = 8, T_BLOB = 9, T_HELLO_ACK = 10,
        T_CLIP_PULL = 11, T_CLIP_DATA = 12, T_CLIP_CHUNK = 13, T_CLIP_END = 14,
        T_CLIP_GET = 15, T_CLIP_RESULT = 16,
    };
    const int CON = 2;                          // hvc2
    const int MAX_CLIENTS = 16;
    const int MAX_POOLS = 64;
    const uint32_t POOL_MAX = 128u << 20;       // one pool
    const uint32_t IN_MAX = 512 * 1024;         // largest guest frame + slack
    const uint32_t OUT_MAX = 1024 * 1024;       // queued host -> guest bytes
    const uint32_t BLOB_MAX = 4u << 20;
    const uint64_t POOL_BUDGET = 768ull << 20;  // all bridged pools together (satoru)
    const uint32_t PART_MAX = 65536;            // partial guest message held per client

    struct Pool { bool used; uint32_t id; uint8_t* base; uint32_t size, cap; };
    struct Client {
        bool used;
        uint16_t cid;
        int sd;
        Pool pools[MAX_POOLS];
        int pend[16]; int npend;                // pools waiting for their DATA (the fd order)
        uint64_t in_bytes, out_bytes;
        uint8_t* part; uint32_t part_len;       // guest bytes not yet a whole message
    };
    struct Dead { uint8_t* p; uint64_t when; };

    // a server wl_data_source.send was turned into a guest clipboard pull: the
    // fd is the host side of the pipe the compositor reads, the guest streams
    // its source data back in T_CLIP_CHUNK frames. (satoru)
    struct ClipOut {
        bool     used;
        uint16_t cid;
        uint32_t xid;
        int      fd;
        uint8_t* buf;
        uint32_t len, cap, off;
        bool     ending;
        uint64_t deadline;
    };
    // a guest data_offer.receive asks the compositor for its stored payload;
    // it is streamed to the guest in chunks to stay inside the out ring. (satoru)
    struct ClipResult {
        bool     used;
        uint16_t cid;
        uint32_t xid;
        uint8_t* data;
        uint32_t len, off;
    };
    const int MAX_CLIPOUT = 16;
    const int MAX_CLIPRES = 8;
    const uint32_t CLIP_CHUNK = 16384;
    ClipOut    g_cout[MAX_CLIPOUT];
    ClipResult g_cres[MAX_CLIPRES];
    uint32_t   g_clip_xid = 1;

    Client  g_cl[MAX_CLIENTS];
    // 512 kb + 1 mb of bridge rings, only while a guest windows session is
    // actually talking to this compositor. (satoru)
    uint8_t* g_in = nullptr; uint32_t g_in_len = 0;
    uint8_t* g_out = nullptr; uint32_t g_out_len = 0;

    bool EnsureBuffers() {
        if (!g_in) {
            g_in = (uint8_t*)KernelHeap::Alloc(IN_MAX);
            if (!g_in) return false;
        }
        if (!g_out) {
            g_out = (uint8_t*)KernelHeap::Alloc(OUT_MAX);
            if (!g_out) return false;
        }
        return true;
    }
    uint8_t g_rx[32768];
    Dead    g_dead[1024];
    uint64_t g_pool_bytes = 0;
    char    g_path[UnixSocket::UNIX_PATH_MAX];
    bool    g_hello = false;
    uint64_t st_commits = 0, st_sync_bytes = 0, st_bad = 0, st_opens = 0;

    inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    inline uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

    void bury(uint8_t* p) {
        if (!p) return;
        // the compositor may still be inside a blit of this pool on another cpu:
        // free it a few seconds later (satoru)
        uint64_t t = Timer::GetRealMs64();
        for (auto& d : g_dead) if (!d.p) { d.p = p; d.when = t; return; }
        // graveyard full: leak it rather than free memory the compositor may
        // still be reading (satoru)
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; SerialLogger::Log("[wlbridge] pool graveyard full, leaking\r\n"); }
    }

    void sweep_dead() {
        uint64_t t = Timer::GetRealMs64();
        for (auto& d : g_dead) if (d.p && t - d.when > 3000) { KernelHeap::Free(d.p); d.p = nullptr; }
    }

    // queue one frame for the guest; partial console writes are fine, the
    // channel is a byte stream (satoru)
    bool frame(uint8_t type, uint8_t nfd, uint16_t cid, const void* a, uint32_t alen, const void* b = nullptr, uint32_t blen = 0) {
        uint32_t len = alen + blen;
        if (!EnsureBuffers()) return false;
        if (g_out_len + 8 + len > OUT_MAX) return false;
        uint8_t* o = g_out + g_out_len;
        o[0] = type; o[1] = nfd; o[2] = (uint8_t)cid; o[3] = (uint8_t)(cid >> 8);
        o[4] = (uint8_t)len; o[5] = (uint8_t)(len >> 8); o[6] = (uint8_t)(len >> 16); o[7] = (uint8_t)(len >> 24);
        if (alen) memcpy(o + 8, a, alen);
        if (blen) memcpy(o + 8 + alen, b, blen);
        g_out_len += 8 + len;
        return true;
    }

    void flush_out() {
        if (!g_out_len || !g_out) return;
        int k = GuestConsole::Write(CON, g_out, (int)g_out_len);
        if (k <= 0) return;
        memmove(g_out, g_out + k, g_out_len - (uint32_t)k);
        g_out_len -= (uint32_t)k;
    }

    Client* find(uint16_t cid) {
        for (auto& c : g_cl) if (c.used && c.cid == cid) return &c;
        return nullptr;
    }

    Pool* find_pool(Client* c, uint32_t id) {
        for (auto& p : c->pools) if (p.used && p.id == id) return &p;
        return nullptr;
    }

    // ---- clipboard relay (satoru)

    ClipOut* cout_find(uint32_t xid) {
        for (auto& c : g_cout) if (c.used && c.xid == xid) return &c;
        return nullptr;
    }
    void cout_free(ClipOut& c) {
        if (c.fd >= 0) UnixSocket::Close(c.fd);
        if (c.buf) KernelHeap::Free(c.buf);
        c.buf = nullptr;
        c.len = c.cap = c.off = 0;
        c.ending = false;
        c.fd = -1;
        c.used = false;
    }
    void cout_queue(ClipOut& c, const uint8_t* d, uint32_t n) {
        if (!c.used || c.fd < 0 || !n) return;
        if (c.off >= c.len && !c.buf) {
            int r = UnixSocket::Send(c.fd, d, (int)n, 0);
            if (r > 0) {
                if ((uint32_t)r == n) return;
                d += r; n -= (uint32_t)r;
            }
        }
        if (c.len + n > ClipboardManager::MAX_BLOB) { c.ending = true; return; }
        if (c.len + n > c.cap) {
            uint32_t ncap = c.cap ? c.cap : 65536;
            while (ncap < c.len + n) ncap *= 2;
            if (ncap > ClipboardManager::MAX_BLOB) ncap = ClipboardManager::MAX_BLOB;
            uint8_t* nb = (uint8_t*)KernelHeap::Alloc(ncap);
            if (!nb) { c.ending = true; return; }
            if (c.buf) {
                for (uint32_t i = c.off; i < c.len; i++) nb[i - c.off] = c.buf[i];
                KernelHeap::Free(c.buf);
                c.len -= c.off;
                c.off = 0;
            }
            c.buf = nb;
            c.cap = ncap;
        }
        for (uint32_t i = 0; i < n; i++) c.buf[c.len + i] = d[i];
        c.len += n;
    }
    void cout_pump() {
        uint64_t now = Timer::GetRealMs64();
        for (auto& c : g_cout) {
            if (!c.used) continue;
            while (c.off < c.len) {
                int r = UnixSocket::Send(c.fd, c.buf + c.off, (int)(c.len - c.off), 0);
                if (r <= 0) break;
                c.off += (uint32_t)r;
            }
            if (c.off >= c.len) {
                if (c.buf) { KernelHeap::Free(c.buf); c.buf = nullptr; }
                c.len = c.cap = c.off = 0;
                if (c.ending) cout_free(c);
                continue;
            }
            if (now > c.deadline) cout_free(c);
        }
    }

    void clip_result_pump() {
        for (auto& r : g_cres) {
            if (!r.used) continue;
            while (r.off < r.len && g_out_len + 8 + 4 + CLIP_CHUNK <= OUT_MAX) {
                uint32_t n = r.len - r.off;
                if (n > CLIP_CHUNK) n = CLIP_CHUNK;
                uint32_t xid = r.xid;
                if (!frame(T_CLIP_CHUNK, 0, r.cid, &xid, 4, r.data + r.off, n)) break;
                r.off += n;
            }
            if (r.off >= r.len) {
                uint32_t xid = r.xid;
                frame(T_CLIP_END, 0, r.cid, &xid, 4);
                if (r.data) KernelHeap::Free(r.data);
                r.data = nullptr;
                r.used = false;
            }
        }
    }

    bool clip_parse_str(const uint8_t* p, uint32_t len, uint32_t off, char* out, int max) {
        if (!out || max <= 0 || off + 4 > len) return false;
        uint32_t sl = le32(p + off);
        if (sl == 0 || off + 4 + sl > len) return false;
        uint32_t n = sl - 1;
        if (n > (uint32_t)(max - 1)) n = (uint32_t)(max - 1);
        for (uint32_t i = 0; i < n; i++) out[i] = (char)p[off + 4 + i];
        out[n] = 0;
        return true;
    }

    // the compositor wants data from a guest source: replace its fd-carrying
    // wl_data_source.send with a T_CLIP_PULL and keep the pipe. (satoru)
    void clip_pull(Client* c, const uint8_t* msg, uint32_t r, int fd) {
        if (!c || r < 12) { if (fd >= 0) UnixSocket::Close(fd); return; }
        uint32_t obj = le32(msg);
        uint32_t w = le32(msg + 4);
        uint16_t op = (uint16_t)(w & 0xFFFF);
        char mime[64];
        if (!clip_parse_str(msg, r, 8, mime, sizeof(mime))) { UnixSocket::Close(fd); return; }
        uint32_t xid = g_clip_xid++;
        uint32_t mlen = (uint32_t)strlen(mime);
        ClipOut* slot = nullptr;
        for (auto& x : g_cout) if (!x.used) { slot = &x; break; }
        if (!slot) { UnixSocket::Close(fd); return; }
        uint8_t pl[80];
        pl[0] = (uint8_t)xid; pl[1] = (uint8_t)(xid >> 8); pl[2] = (uint8_t)(xid >> 16); pl[3] = (uint8_t)(xid >> 24);
        pl[4] = (uint8_t)obj; pl[5] = (uint8_t)(obj >> 8); pl[6] = (uint8_t)(obj >> 16); pl[7] = (uint8_t)(obj >> 24);
        pl[8] = (uint8_t)op; pl[9] = (uint8_t)(op >> 8); pl[10] = 0; pl[11] = 0;
        pl[12] = (uint8_t)mlen; pl[13] = (uint8_t)(mlen >> 8); pl[14] = (uint8_t)(mlen >> 16); pl[15] = (uint8_t)(mlen >> 24);
        for (uint32_t i = 0; i < mlen; i++) pl[16 + i] = (uint8_t)mime[i];
        if (!frame(T_CLIP_PULL, 0, c->cid, pl, 16 + mlen)) { UnixSocket::Close(fd); return; }
        slot->used = true;
        slot->cid = c->cid;
        slot->xid = xid;
        slot->fd = fd;
        slot->buf = nullptr;
        slot->len = slot->cap = slot->off = 0;
        slot->ending = false;
        slot->deadline = Timer::GetRealMs64() + 8000;
        SerialLogger::Log("[wlbridge] clip pull cid="); SerialLogger::LogDec((int)c->cid);
        SerialLogger::Log(" mime="); SerialLogger::Log(mime);
        SerialLogger::Log("\r\n");
    }

    void clip_get(Client* c, const uint8_t* p, uint32_t len) {
        if (!c || len < 12) return;
        uint32_t xid = le32(p);
        uint32_t offer = le32(p + 4);
        char mime[64];
        if (!clip_parse_str(p, len, 8, mime, sizeof(mime))) mime[0] = 0;
        uint32_t mlen = (uint32_t)strlen(mime);
        int ssd = UnixSocket::PeerSd(c->sd);
        uint8_t* data = nullptr;
        uint32_t n = 0;
        if (ssd >= 0 && mime[0])
            WaylandServer::ClipboardGetForOffer(ssd, offer, mime, &data, &n);
        uint8_t pl[80];
        pl[0] = (uint8_t)xid; pl[1] = (uint8_t)(xid >> 8); pl[2] = (uint8_t)(xid >> 16); pl[3] = (uint8_t)(xid >> 24);
        pl[4] = (uint8_t)mlen; pl[5] = (uint8_t)(mlen >> 8); pl[6] = (uint8_t)(mlen >> 16); pl[7] = (uint8_t)(mlen >> 24);
        for (uint32_t i = 0; i < mlen; i++) pl[8 + i] = (uint8_t)mime[i];
        ClipResult* slot = nullptr;
        for (auto& r : g_cres) if (!r.used) { slot = &r; break; }
        if (!slot) {
            frame(T_CLIP_RESULT, 0, c->cid, pl, 8 + mlen);
            frame(T_CLIP_END, 0, c->cid, &xid, 4);
            if (data) KernelHeap::Free(data);
            return;
        }
        slot->used = true;
        slot->cid = c->cid;
        slot->xid = xid;
        slot->data = data;
        slot->len = n;
        slot->off = 0;
        SerialLogger::Log("[wlbridge] clip get cid="); SerialLogger::LogDec((int)c->cid);
        SerialLogger::Log(" mime="); SerialLogger::Log(mime);
        SerialLogger::Log(" len="); SerialLogger::LogDec((int)n);
        SerialLogger::Log("\r\n");
        frame(T_CLIP_RESULT, 0, c->cid, pl, 8 + mlen);
        clip_result_pump();
    }

    // retire a pool: the compositor forgets its base first, then the memory goes
    // to the graveyard (satoru)
    void retire(Client* c, Pool* p) {
        if (!p->used) return;
        int ssd = (c->sd >= 0) ? UnixSocket::PeerSd(c->sd) : -1;
        if (ssd >= 0) WaylandServer::RegisterPoolMemory(ssd, p->id, nullptr, 0);
        bury(p->base);
        g_pool_bytes -= p->cap;
        p->used = false;
        p->base = nullptr;
    }

    void drop(Client* c, bool tell_guest, const char* why = nullptr) {
        if (!c || !c->used) return;
        if (why) {
            SerialLogger::Log("[wlbridge] client "); SerialLogger::LogDec(c->cid);
            SerialLogger::Log(" closed: "); SerialLogger::Log(why); SerialLogger::Log("\r\n");
        }
        // close the client's windows with it: the compositor alone frees the
        // slot but leaves the windows on screen (satoru)
        if (c->sd >= 0) WaylandServer::DropClientOfSd(UnixSocket::PeerSd(c->sd));
        for (auto& p : c->pools) if (p.used) { bury(p.base); g_pool_bytes -= p.cap; p.used = false; }
        if (c->sd >= 0) UnixSocket::Close(c->sd);
        if (c->part) { KernelHeap::Free(c->part); c->part = nullptr; }
        c->part_len = 0;
        for (auto& o : g_cout) if (o.used && o.cid == c->cid) cout_free(o);
        for (auto& r : g_cres) if (r.used && r.cid == c->cid) {
            if (r.data) KernelHeap::Free(r.data);
            r.data = nullptr;
            r.used = false;
        }
        if (tell_guest) frame(T_CLOSE, 0, c->cid, nullptr, 0);
        c->used = false;
        c->sd = -1;
    }

    void open_client(uint16_t cid) {
        if (Client* old = find(cid)) drop(old, false);
        Client* c = nullptr;
        for (auto& x : g_cl) if (!x.used) { c = &x; break; }
        if (!c) { frame(T_CLOSE, 0, cid, nullptr, 0); return; }
        if (!g_path[0]) {
            int ls = WaylandServer::ListenSd();
            if (ls < 0 || UnixSocket::GetSockName(ls, g_path, sizeof(g_path)) < 0 || !g_path[0]) {
                frame(T_CLOSE, 0, cid, nullptr, 0);
                return;
            }
        }
        int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
        if (sd < 0 || UnixSocket::Connect(sd, g_path) < 0) {
            if (sd >= 0) UnixSocket::Close(sd);
            SerialLogger::Log("[wlbridge] connect to the compositor failed\r\n");
            frame(T_CLOSE, 0, cid, nullptr, 0);
            return;
        }
        for (auto& p : c->pools) p.used = false;
        c->part = (uint8_t*)KernelHeap::Alloc(PART_MAX);
        if (!c->part) { UnixSocket::Close(sd); frame(T_CLOSE, 0, cid, nullptr, 0); return; }
        c->part_len = 0;
        c->used = true; c->cid = cid; c->sd = sd; c->npend = 0;
        c->in_bytes = c->out_bytes = 0;
        st_opens++;
    }

    void on_pool(Client* c, const uint8_t* p, uint32_t len) {
        if (len < 8) { st_bad++; return; }
        uint32_t id = le32(p), size = le32(p + 4);
        if (size == 0 || size > POOL_MAX || c->npend >= 16) { st_bad++; drop(c, true); return; }
        Pool* slot = nullptr;
        for (auto& x : c->pools) if (!x.used) { slot = &x; break; }
        if (Pool* old = find_pool(c, id)) { retire(c, old); slot = old; }
        if (!slot) { st_bad++; drop(c, true, "too many pools"); return; }
        uint32_t cap = (size + 0xFFFFu) & ~0xFFFFu;
        if (g_pool_bytes + cap > POOL_BUDGET) { drop(c, true, "pool memory budget exceeded"); return; }
        uint8_t* b = (uint8_t*)KernelHeap::Alloc(cap);
        if (!b) { SerialLogger::Log("[wlbridge] out of memory for a pool\r\n"); drop(c, true); return; }
        memset(b, 0, cap);
        slot->used = true; slot->id = id; slot->base = b; slot->size = size; slot->cap = cap;
        g_pool_bytes += cap;
        c->pend[c->npend++] = (int)(slot - c->pools);
    }

    // forward whole wayland messages only, and never a wl_shm_pool.resize on a
    // bridged pool: the compositor would grow pool_size past the host buffer
    // and read beyond it. a guest is not trusted with the compositor's
    // bookkeeping (satoru)
    void on_data(Client* c, uint8_t nfd, const uint8_t* p, uint32_t len) {
        c->in_bytes += len;
        if (c->part_len + len > PART_MAX) { drop(c, true, "message larger than 64 kb"); return; }
        memcpy(c->part + c->part_len, p, len);
        c->part_len += len;
        static uint8_t fw[PART_MAX];
        uint32_t fn = 0, pos = 0;
        while (c->part_len - pos >= 8) {
            const uint8_t* m = c->part + pos;
            uint32_t id = le32(m), w = le32(m + 4);
            uint32_t size = w >> 16, op = w & 0xFFFF;
            if (size < 8 || (size & 3)) { drop(c, true, "malformed message"); return; }
            if (c->part_len - pos < size) break;
            bool keep = !(op == 2 && find_pool(c, id));
            if (keep) { memcpy(fw + fn, m, size); fn += size; }
            pos += size;
        }
        if (pos) { memmove(c->part, c->part + pos, c->part_len - pos); c->part_len -= pos; }
        if (!fn && !nfd) return;
        int r;
        if (nfd) {
            // ControlMsg grew (pin fields): this file must be recompiled with unix_socket.h (satoru)
            UnixSocket::ControlMsg cm = {};
            int k = 0;
            while (k < nfd && k < c->npend && k < UnixSocket::UNIX_MAX_PASSED_FD) {
                Pool& pl = c->pools[c->pend[k]];
                cm.passed_fds[k] = -1;
                cm.passed_shm_base[k] = (uint64_t)(uintptr_t)pl.base;
                cm.passed_shm_size[k] = pl.size;
                k++;
            }
            cm.passed_fd_count = k;
            for (int i = k; i < c->npend; i++) c->pend[i - k] = c->pend[i];
            c->npend -= k;
            r = UnixSocket::SendMsg(c->sd, fw, (int)fn, 0, &cm);
            if (r == UnixSocket::UNIX_NEED_KLS) {   // the compositor's handler runs under the linux layer lock (satoru)
                LinuxSyscall::LayerLock();
                r = UnixSocket::SendMsg(c->sd, fw, (int)fn, 0, &cm);
                LinuxSyscall::LayerUnlock();
            }
        } else {
            r = UnixSocket::Send(c->sd, fw, (int)fn, 0);
            if (r == UnixSocket::UNIX_NEED_KLS) {   // same retry as the syscall layer's unix_send_k (satoru)
                LinuxSyscall::LayerLock();
                r = UnixSocket::Send(c->sd, fw, (int)fn, 0);
                LinuxSyscall::LayerUnlock();
            }
        }
        if (r < (int)fn) {
            SerialLogger::Log("[wlbridge] send rc="); SerialLogger::LogDec(r);
            SerialLogger::Log(" len="); SerialLogger::LogDec((int)fn); SerialLogger::Log("\r\n");
            drop(c, true, "compositor refused client bytes");
        }
    }

    // copy len bytes out of guest ram into pool `id` at doff. the source is the
    // listed guest pages in order, starting soff bytes into the first (satoru)
    void on_sync(Client* c, const uint8_t* p, uint32_t len) {
        if (len < 20) { st_bad++; return; }
        uint32_t id = le32(p), doff = le32(p + 4), n = le32(p + 8), soff = le32(p + 12), npfn = le32(p + 16);
        if (20 + (uint64_t)npfn * 8 > len || soff >= 4096) { st_bad++; return; }
        Pool* pl = find_pool(c, id);
        if (!pl || doff >= pl->size) return;
        if (n > pl->size - doff) n = pl->size - doff;
        uint32_t d = doff, inpg = soff;
        for (uint32_t k = 0; k < npfn && n; k++) {
            uint32_t chunk = 4096 - inpg;
            if (chunk > n) chunk = n;
            uint64_t gpa = le64(p + 20 + (uint64_t)k * 8) << 12;
            uint8_t* src = GuestMemoryManager::GuestPhysToHost(gpa);
            if (src && GuestMemoryManager::ContigBytes(gpa) >= 4096) memcpy(pl->base + d, src + inpg, chunk);
            else st_bad++;
            d += chunk; n -= chunk; inpg = 0;
            st_sync_bytes += chunk;
        }
        st_commits++;
    }

    void on_resize(Client* c, const uint8_t* p, uint32_t len) {
        if (len < 8) { st_bad++; return; }
        uint32_t id = le32(p), size = le32(p + 4);
        Pool* pl = find_pool(c, id);
        if (!pl || size <= pl->size) return;
        if (size > POOL_MAX) { drop(c, true); return; }
        if (size <= pl->cap) { pl->size = size; return; }
        uint32_t cap = (size + 0x3FFFFFu) & ~0x3FFFFFu;   // grow in 4 mb steps
        if (cap > POOL_MAX) cap = POOL_MAX;
        if (g_pool_bytes - pl->cap + cap > POOL_BUDGET) { drop(c, true, "pool memory budget exceeded"); return; }
        uint8_t* b = (uint8_t*)KernelHeap::Alloc(cap);
        if (!b) { drop(c, true); return; }
        g_pool_bytes = g_pool_bytes - pl->cap + cap;
        memcpy(b, pl->base, pl->size);
        memset(b + pl->size, 0, cap - pl->size);
        bury(pl->base);
        pl->base = b; pl->size = size; pl->cap = cap;
        // the server keeps the pool's base pointer: hand it the new one (satoru)
        int ssd = UnixSocket::PeerSd(c->sd);
        if (ssd >= 0) WaylandServer::RegisterPoolMemory(ssd, id, b, size);
    }

    void on_free(Client* c, const uint8_t* p, uint32_t len) {
        if (len < 4) return;
        Pool* pl = find_pool(c, le32(p));
        if (!pl) return;
        for (int i = 0; i < c->npend; i++) if (&c->pools[c->pend[i]] == pl) return;   // still owed to a DATA
        retire(c, pl);
    }

    void handle(uint8_t type, uint8_t nfd, uint16_t cid, const uint8_t* p, uint32_t len) {
        if (type == T_HELLO) {
            for (auto& c : g_cl) if (c.used) drop(&c, false);
            g_hello = true;
            uint32_t v = 1;
            frame(T_HELLO_ACK, 0, 0, &v, 4);
            SerialLogger::Log("[wlbridge] guest proxy connected\r\n");
            return;
        }
        if (type == T_OPEN) { open_client(cid); return; }
        Client* c = find(cid);
        if (!c) return;                          // a client we already dropped
        switch (type) {
            case T_CLOSE:  drop(c, false); break;
            case T_DATA:   on_data(c, nfd, p, len); break;
            case T_POOL:   on_pool(c, p, len); break;
            case T_SYNC:   on_sync(c, p, len); break;
            case T_RESIZE: on_resize(c, p, len); break;
            case T_FREE:   on_free(c, p, len); break;
            case T_CLIP_DATA: {
                if (len >= 8) {
                    uint32_t xid = le32(p);
                    ClipOut* o = cout_find(xid);
                    if (o) o->deadline = Timer::GetRealMs64() + 8000;
                }
                break;
            }
            case T_CLIP_CHUNK: {
                if (len >= 4) {
                    ClipOut* o = cout_find(le32(p));
                    if (o) cout_queue(*o, p + 4, len - 4);
                }
                break;
            }
            case T_CLIP_END: {
                if (len >= 4) {
                    ClipOut* o = cout_find(le32(p));
                    if (o) { o->ending = true; cout_pump(); }
                }
                break;
            }
            case T_CLIP_GET: clip_get(c, p, len); break;
            default:       st_bad++; break;
        }
    }
}

void WLBridge::Reset() {
    for (auto& c : g_cl) if (c.used) drop(&c, false);
    if (g_in) { KernelHeap::Free(g_in); g_in = nullptr; }
    if (g_out) { KernelHeap::Free(g_out); g_out = nullptr; }
    g_in_len = 0;
    g_out_len = 0;
    g_hello = false;
    g_path[0] = 0;
}

void WLBridge::OnGuestBytes(int idx, const uint8_t* data, int n) {
    (void)idx;
    if (!EnsureBuffers()) return;
    while (n > 0) {
        uint32_t k = (uint32_t)n;
        if (k > IN_MAX - g_in_len) k = IN_MAX - g_in_len;
        memcpy(g_in + g_in_len, data, k);
        g_in_len += k; data += k; n -= (int)k;
        uint32_t pos = 0;
        while (g_in_len - pos >= 8) {
            const uint8_t* h = g_in + pos;
            uint32_t len = le32(h + 4);
            if (len > IN_MAX - 8) {
                // out of sync: nothing sane can follow. start over (satoru)
                SerialLogger::Log("[wlbridge] bad frame from the guest, resetting the bridge\r\n");
                for (auto& c : g_cl) if (c.used) drop(&c, true);
                g_in_len = 0;
                return;
            }
            if (g_in_len - pos < 8 + len) break;
            handle(h[0], h[1], (uint16_t)(h[2] | (h[3] << 8)), h + 8, len);
            pos += 8 + len;
        }
        if (pos) { memmove(g_in, g_in + pos, g_in_len - pos); g_in_len -= pos; }
        if (g_in_len == IN_MAX && n > 0) { Reset(); return; }
    }
}

void WLBridge::Poll() {
    flush_out();
    cout_pump();
    clip_result_pump();
    for (auto& c : g_cl) {
        if (!c.used) continue;
        for (int guard = 0; guard < 8 && g_out_len < OUT_MAX / 2; guard++) {
            UnixSocket::ControlMsg cm = {};
            int r = UnixSocket::Recv(c.sd, g_rx, sizeof(g_rx), 0, &cm);
            if (r == -11) break;
            if (r <= 0) { drop(&c, true, r == 0 ? "compositor hung up" : "recv error"); break; }
            int blobs = 0;
            bool clip_ev = false;
            // the fd-carrying wl_data_source.send is the FIRST message of the
            // read (ring_read stops at its frame offset); later events may
            // share the buffer, so parse the first size instead of requiring
            // the whole read to be one message. (satoru)
            uint32_t first_size = (r >= 8) ? (((uint32_t)le32(g_rx + 4)) >> 16) : 0;
            uint32_t clip_len = 0;
            for (int i = 0; i < cm.passed_fd_count; i++) {
                if (cm.passed_is_socket[i] && first_size >= 12 && first_size <= (uint32_t)r) {
                    clip_pull(&c, g_rx, first_size, cm.passed_sd[i]);
                    clip_ev = true;
                    clip_len = first_size;
                    continue;
                }
                const uint8_t* b = (const uint8_t*)(uintptr_t)cm.passed_shm_base[i];
                uint64_t sz = cm.passed_shm_size[i];
                if (!b || !sz || sz > BLOB_MAX) { b = nullptr; sz = 0; }   // still one fd for the client (satoru)
                if (!frame(T_BLOB, 0, c.cid, b, (uint32_t)sz)) break;
                blobs++;
            }
            if (clip_ev) {
                if ((uint32_t)r > clip_len) {
                    if (!frame(T_DATA, 0, c.cid, g_rx + clip_len, (uint32_t)r - clip_len)) { drop(&c, true, "guest queue full"); break; }
                    c.out_bytes += (uint64_t)((uint32_t)r - clip_len);
                }
            } else {
                if (!frame(T_DATA, (uint8_t)blobs, c.cid, g_rx, (uint32_t)r)) { drop(&c, true, "guest queue full"); break; }
                c.out_bytes += (uint64_t)r;
            }
        }
        if (c.used && UnixSocket::PeerClosed(c.sd)) drop(&c, true, "compositor closed the connection");
    }
    flush_out();
    cout_pump();
    clip_result_pump();
    sweep_dead();
}

int WLBridge::ClientCount() {
    int n = 0;
    for (auto& c : g_cl) if (c.used) n++;
    return n;
}

static int wb_app(char* o, int p, int mx, const char* s) { while (*s && p < mx - 1) o[p++] = *s++; o[p] = 0; return p; }
static int wb_appu(char* o, int p, int mx, uint64_t v) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) { if (p < mx - 1) o[p++] = t[--k]; else --k; } o[p] = 0; return p; }

int WLBridge::Status(char* out, int mx) {
    int pools = 0;
    for (auto& c : g_cl) if (c.used) for (auto& p : c.pools) if (p.used) pools++;
    int p = 0;
    p = wb_app(out, p, mx, "  wayland: proxy "); p = wb_app(out, p, mx, g_hello ? "connected" : "not connected");
    p = wb_app(out, p, mx, ", clients "); p = wb_appu(out, p, mx, (uint64_t)ClientCount());
    p = wb_app(out, p, mx, " (opened "); p = wb_appu(out, p, mx, st_opens);
    p = wb_app(out, p, mx, "), pools "); p = wb_appu(out, p, mx, (uint64_t)pools);
    p = wb_app(out, p, mx, ", commits "); p = wb_appu(out, p, mx, st_commits);
    p = wb_app(out, p, mx, " ("); p = wb_appu(out, p, mx, st_sync_bytes >> 20);
    p = wb_app(out, p, mx, " MB), bad frames "); p = wb_appu(out, p, mx, st_bad);
    p = wb_app(out, p, mx, "\n");
    return p;
}

// end (satoru)
