// kernel_slots: see kernel_slots.h. (satoru)
#include "kernel_slots.h"
#include "bootcfg.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"
#include "../kernel/pmm.h"
#include "../hal/hal.h"
#include "recovery_restart.h"

namespace {

void sapp(char* d, int cap, const char* s) {
    int n = 0; while (d[n]) n++;
    while (s && *s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}
void sdec(char* d, int cap, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 23) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[24]; int k = 0;
    while (n) r[k++] = t[--n];
    r[k] = 0;
    sapp(d, cap, r);
}
bool seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

// "KSLOTA.EFI" -> "KSLOTA  EFI". false for a name that is not 8.3. (satoru)
bool to83(const char* n, char out[12]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    out[11] = 0;
    int i = 0, o = 0;
    while (n[i] && n[i] != '.' && o < 8) { char c = n[i++]; out[o++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
    if (n[i] && n[i] != '.') return false;
    if (n[i] == '.') i++;
    o = 8;
    while (n[i] && o < 11) { char c = n[i++]; out[o++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
    return n[i] == 0 && out[0] != ' ';
}

bool looks_like_pe(const uint8_t* p, uint64_t n) {
    if (n < 0x200 || p[0] != 'M' || p[1] != 'Z') return false;
    uint32_t pe = (uint32_t)p[0x3C] | ((uint32_t)p[0x3D] << 8) | ((uint32_t)p[0x3E] << 16) | ((uint32_t)p[0x3F] << 24);
    return pe + 4 < n && p[pe] == 'P' && p[pe + 1] == 'E' && p[pe + 2] == 0 && p[pe + 3] == 0;
}

}  // namespace

namespace KernelSlots {

void Describe(char* out, int cap) {
    out[0] = 0;
    BootCfg::State s;
    if (!BootCfg::Read(&s)) { sapp(out, cap, "no KBOOT.CFG on the KURONO partition"); return; }
    sapp(out, cap, "default "); sapp(out, cap, s.current[0] ? s.current : "built in");
    sapp(out, cap, ", previous "); sapp(out, cap, s.previous[0] ? s.previous : "none");
    if (s.trial[0]) { sapp(out, cap, ", trying "); sapp(out, cap, s.trial); }
    if (s.flags & BootCfg::F_TRIAL_FAILED) sapp(out, cap, ", the last trial did not start");
}

bool Stage(const char* source, char* msg, int cap) {
    msg[0] = 0;
    BootCfg::State s;
    if (!BootCfg::Read(&s)) { sapp(msg, cap, "KBOOT.CFG is missing on the KURONO partition"); return false; }
    // read the new image: a KVFS file, else an 8.3 name in the KURONO root. (satoru)
    uint8_t* img = nullptr;
    uint64_t len = 0, cap_bytes = 0;
    if (source[0] == '/') {
        int sz = KVFS::GetFileSize(source);
        if (sz <= 0) { sapp(msg, cap, "cannot read "); sapp(msg, cap, source); return false; }
        len = (uint64_t)sz;
        cap_bytes = (len + 0xFFFFFull) & ~0xFFFFFull;
        img = (uint8_t*)PMM::AllocBytes(cap_bytes);
        if (!img || KVFS::ReadFile(source, img, (uint32_t)len) != (int)len) {
            if (img) PMM::FreeBytes(img, cap_bytes);
            sapp(msg, cap, "cannot read "); sapp(msg, cap, source); return false;
        }
    } else {
        char n83[12];
        if (!to83(source, n83) || DiskLog::StatRootFile(n83, &len) != 0 || !len) {
            sapp(msg, cap, "no "); sapp(msg, cap, source); sapp(msg, cap, " in the KURONO root"); return false;
        }
        cap_bytes = (len + 0xFFFFFull) & ~0xFFFFFull;
        img = (uint8_t*)PMM::AllocBytes(cap_bytes);
        if (!img || DiskLog::ReadRootFile(n83, img, cap_bytes, nullptr) != 0) {
            if (img) PMM::FreeBytes(img, cap_bytes);
            sapp(msg, cap, "cannot read "); sapp(msg, cap, source); return false;
        }
    }
    if (!looks_like_pe(img, len)) {
        PMM::FreeBytes(img, cap_bytes);
        sapp(msg, cap, "that file is not an EFI image");
        return false;
    }
    // the slot that is not the default (and not the one being tried). (satoru)
    const char* slot = seq(s.current, "KSLOTA.EFI") ? "KSLOTB.EFI" : "KSLOTA.EFI";
    char s83[12];
    to83(slot, s83);
    uint64_t have = 0;
    int st = DiskLog::StatRootFile(s83, &have);
    bool ok = false;
    if (st == 0) {
        // in place: the file has to be one contiguous extent at least as big. (satoru)
        uint64_t lba = 0, span = 0; uint32_t size = 0;
        if (DiskLog::RootFileExtent(s83, &lba, &span, &size) == 0 && size >= len) {
            uint32_t lb = DiskLog::LbaSize() ? DiskLog::LbaSize() : 512;
            ok = true;
            for (uint64_t off = 0; off < size && ok; off += 65536) {
                uint32_t chunk = (uint32_t)((size - off) < 65536 ? (size - off) : 65536);
                static uint8_t zero_tail[65536];
                const uint8_t* src = img + off;
                if (off >= len) src = zero_tail;   // pad: never leave an old image's tail (satoru)
                else if (off + chunk > len) {
                    for (uint32_t k = 0; k < chunk; k++) zero_tail[k] = (off + k < len) ? img[off + k] : 0;
                    src = zero_tail;
                }
                uint32_t n = (chunk + lb - 1) / lb * lb;
                if (off + n > span) { ok = false; break; }
                ok = DiskLog::WriteRootExtent(lba + off / lb, src, n);
                if (src == zero_tail) for (uint32_t k = 0; k < 65536; k++) zero_tail[k] = 0;
            }
        } else {
            sapp(msg, cap, slot); sapp(msg, cap, " is too small or fragmented: delete it from the KURONO partition first");
        }
    } else {
        // a new slot file with headroom, so later updates fit in place. (satoru)
        uint64_t want = ((len + (16ull << 20) - 1) & ~((16ull << 20) - 1)) + (16ull << 20);
        uint8_t* pad = (uint8_t*)PMM::AllocBytes(want);
        if (pad) {
            for (uint64_t k = 0; k < want; k++) pad[k] = k < len ? img[k] : 0;
            int wr = DiskLog::WriteRootFile(s83, pad, (uint32_t)want);
            ok = wr == 0;
            if (!ok) { sapp(msg, cap, "writing "); sapp(msg, cap, slot); sapp(msg, cap, " failed: "); sapp(msg, cap, DiskLog::RootFileMsg(wr)); }
            PMM::FreeBytes(pad, want);
        } else sapp(msg, cap, "not enough memory");
    }
    PMM::FreeBytes(img, cap_bytes);
    if (!ok) return false;
    if (!BootCfg::SetTrial(slot, 1)) { sapp(msg, cap, "KBOOT.CFG could not be updated"); return false; }
    sapp(msg, cap, "staged as "); sapp(msg, cap, slot); sapp(msg, cap, " (");
    sdec(msg, cap, len / (1024 * 1024)); sapp(msg, cap, " MB). Restart to try it; Kurono goes back by itself if it does not start.");
    SerialLogger::Log("[kslot] "); SerialLogger::Log(msg); SerialLogger::Log("\r\n");
    return true;
}

int Command(int argc, const char** argv, char* out, int mx) {
    out[0] = 0;
    const char* sub = argc >= 2 ? argv[1] : "status";
    if (seq(sub, "stage") && argc >= 3) { Stage(argv[2], out, mx - 1); sapp(out, mx, "\n"); }
    else if (seq(sub, "previous")) sapp(out, mx, BootCfg::RequestPreviousOnce() ? "the previous version starts next time\n" : "no KBOOT.CFG\n");
    else if (seq(sub, "recovery")) sapp(out, mx, BootCfg::RequestRecoveryOnce() ? "Recovery starts next time\n" : "no KBOOT.CFG\n");
    else { Describe(out, mx - 1); sapp(out, mx, "\n"); }
    int n = 0; while (out[n]) n++;
    return n;
}

}  // namespace KernelSlots

void RecoveryRestart() {
    bool ok = BootCfg::RequestRecoveryOnce();
    SerialLogger::Log(ok ? "[kslot] restarting into Recovery\r\n" : "[kslot] no KBOOT.CFG: restarting (hold R for Recovery)\r\n");
    DiskLog::Flush();
    HAL::Reboot();
}

// end (satoru)
