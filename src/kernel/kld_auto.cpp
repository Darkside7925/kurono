// KLD auto-match implementation. see kld_auto.h for the contract.
//
// (satoru)

#include "kld_auto.h"
#include "kld_broker.h"
#include "kld_display.h"
#include "kld_iommu.h"
#include "kld_model.h"
#include "elf_loader.h"
#include "heap.h"   // imgtest staging buffer (satoru)
#include "userspace.h"
#include "../proc/scheduler.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../net/network.h"
#include "kld_supervisor.h"

namespace KldAuto {

// the generated table, through weak accessors: a default build has no table
// and the function pointers are null. (satoru)
extern "C" {
extern const MatchFamily* kld_auto_families_get(void) __attribute__((weak));
extern unsigned kld_auto_layout_get(void) __attribute__((weak));
extern unsigned kld_auto_family_count_get(void) __attribute__((weak));
extern const MatchId* kld_auto_ids_get(void) __attribute__((weak));
extern unsigned kld_auto_id_count_get(void) __attribute__((weak));
extern unsigned kld_auto_layout_get(void) __attribute__((weak));
}

// set only by the kernel self check. (satoru)
bool g_table_absent_for_test;

bool table_present()
{
    if (g_table_absent_for_test)
        return false;
    if (!(kld_auto_families_get && kld_auto_family_count_get &&
          kld_auto_ids_get && kld_auto_id_count_get &&
          kld_auto_family_count_get() != 0u && kld_auto_id_count_get() != 0u))
        return false;
    if (!kld_auto_layout_get || kld_auto_layout_get() != FLAG_LAYOUT_VERSION) {
        // a table built for another flag layout would grant wrong privileges. (satoru)
        static bool warned;
        if (!warned) {
            warned = true;
            SerialLogger::Log("[kld-warn] match table flag layout version differs from the kernel's: table refused\r\n");
        }
        return false;
    }
    return true;
}

const MatchFamily* families()
{
    return kld_auto_families_get ? kld_auto_families_get() : nullptr;
}

unsigned family_count()
{
    return kld_auto_family_count_get ? kld_auto_family_count_get() : 0u;
}

const MatchId* ids()
{
    return kld_auto_ids_get ? kld_auto_ids_get() : nullptr;
}

unsigned ids_count()
{
    return kld_auto_id_count_get ? kld_auto_id_count_get() : 0u;
}

namespace {

// the container ELFs the kernel may embed, linked by src/Makefile only when
// KURONO_KLD_EMBED=1 and the file exists. weak: absent in a default build.
// (satoru)
extern "C" {
extern const uint8_t _binary_kld_e1000e_elf_start[]  __attribute__((weak));
extern const uint8_t _binary_kld_e1000e_elf_end[]    __attribute__((weak));
extern const uint8_t _binary_kld_iwlwifi_elf_start[] __attribute__((weak));
extern const uint8_t _binary_kld_iwlwifi_elf_end[]   __attribute__((weak));
extern const uint8_t _binary_kld_drmcirrus_elf_start[] __attribute__((weak));
extern const uint8_t _binary_kld_drmcirrus_elf_end[]   __attribute__((weak));
extern const uint8_t _binary_kld_bochs_elf_start[] __attribute__((weak));
extern const uint8_t _binary_kld_bochs_elf_end[]   __attribute__((weak));
extern const uint8_t _binary_kld_i915_elf_start[] __attribute__((weak));
extern const uint8_t _binary_kld_i915_elf_end[]   __attribute__((weak));
}

// families this kernel knows how to launch. the match table may name more
// (built but not embedded here); those log "(not embedded)". (satoru)
struct EmbedFamily {
    const char* name;
    const char* path;
    bool        network;
};

const EmbedFamily kEmbed[] = {
    { "e1000e",  "/usr/bin/kld-e1000e",  true },
    { "iwlwifi", "/usr/bin/kld-iwlwifi", true },
    { "drmcirrus", "/usr/bin/kld-drmcirrus", false },
    { "bochs",   "/usr/bin/kld-bochs",   false },
    { "i915",    "/usr/bin/kld-i915",    false },
};
constexpr int EMBED_COUNT = (int)(sizeof(kEmbed) / sizeof(kEmbed[0]));
constexpr int MAX_JOBS = 8;

struct Job {
    bool used;
    const EmbedFamily* fam;
    uint32_t bdf;   // the device the auto matcher selected (satoru)
};
Job g_jobs[MAX_JOBS];
int g_job_count;
volatile int g_job_claim;

// kurono.kld.take=vvvv:dddd overrides the native-owner skip for one device.
// (satoru)
bool g_take_valid;
uint16_t g_take_vendor, g_take_device;
bool g_take_bdf_valid;
uint32_t g_take_bdf;

// kurono.kld.imgtest=1: stage a same-size stale container image before the
// runner writes, then prove the runner replaced it. rig only. (satoru)
bool g_imgtest;
bool g_aliastest;   // kurono.kld.aliastest=1: display families get "aliastest" (satoru)
bool g_restoretest; // kurono.kld.restoretest=1: display families get "restoretest" (satoru)
bool g_bartest;     // kurono.kld.bartest=1: display families get "bartest" (d4 policy rig) (satoru)
bool g_failsetup;   // kurono.kld.failsetup=1: display families get "failsetup" (rollback rig) (satoru)
bool g_gtbroker;    // kurono.kld.gtbroker=1: model gt writes go through the broker allowlist (satoru)
bool g_imgtest_staged, g_imgtest_checked;
uint32_t g_imgtest_off;
uint8_t g_imgtest_byte;

// the boot policy, parsed once before Network::Init. (satoru)
struct Policy {
    bool     take_valid;
    uint32_t take_vendor;
    uint32_t take_device;
    bool     take_bdf_valid;   // take=vvvv:dddd@bb:dd.f selects one adapter (satoru)
    uint32_t take_bdf;
    bool     wifi_native;   // kurono.kld.wifi=native: native keeps wifi
    bool     net_ok;        // kurono.kld.net=0: native wired stays active
};
Policy g_policy;
bool g_legacy_vga_forced;

// whether the native drivers were really told to stand down. this is per KIND,
// not per device: if one wired nic is KLD-driven, the native wired driver
// stands down and the other wired nic is not claimed. fixing that needs a
// per-device claim check inside the native drivers (E1000::ScanPCI,
// Igc::FindDevice, WifiDev::Probe, WiFi::ProbePCIWireless), which is another
// agent's code. (satoru)
bool g_native_wired_deferred;
bool g_native_wifi_deferred;

// ── tiny string helpers (the kernel has no libc) ────────────────────────────
bool streq(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

bool streq_n(const char* a, const char* b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

unsigned slen(const char* s)
{
    unsigned n = 0;
    while (s && s[n]) n++;
    return n;
}

void log_dec(uint32_t v)   { SerialLogger::LogDec((int)v); }

void log_hex4(uint32_t v)
{
    static const char dig[] = "0123456789abcdef";
    char b[5];
    for (int i = 3; i >= 0; i--) { b[i] = dig[v & 15u]; v >>= 4; }
    b[4] = 0;
    SerialLogger::Log(b);
}

void log_bdf(uint32_t bdf)
{
    log_dec((bdf >> 16) & 0xFFu);
    SerialLogger::Log(":");
    log_dec((bdf >> 8) & 0xFFu);
    SerialLogger::Log(".");
    log_dec(bdf & 0xFFu);
}

// ── the driver's own table and linux's matching rule ────────────────────────
// drivers/pci/pci.h pci_match_one_device: vendor, device, subsystem ids with
// PCI_ANY_ID wildcards, then the masked class comparison. (satoru)
bool id_matches(const MatchId& id, uint32_t vendor, uint32_t device,
                uint32_t subvendor, uint32_t subdevice, uint32_t class_code)
{
    if (id.vendor != 0xFFFFFFFFu && id.vendor != vendor) return false;
    if (id.device != 0xFFFFFFFFu && id.device != device) return false;
    if (id.subvendor != 0xFFFFFFFFu && id.subvendor != subvendor) return false;
    if (id.subdevice != 0xFFFFFFFFu && id.subdevice != subdevice) return false;
    if ((id.class_id ^ class_code) & id.class_mask) return false;
    return true;
}

const MatchFamily* family_by_name(const char* name)
{
    const MatchFamily* f = families();
    for (unsigned i = 0; i < family_count(); i++)
        if (streq(f[i].name, name))
            return &f[i];
    return nullptr;
}

const EmbedFamily* embed_by_name(const char* name)
{
    for (int i = 0; i < EMBED_COUNT; i++)
        if (streq(kEmbed[i].name, name))
            return &kEmbed[i];
    return nullptr;
}

bool embed_elf(const char* name, const uint8_t** start, const uint8_t** end)
{
    *start = nullptr;
    *end = nullptr;
    if (streq(name, "e1000e")) {
        *start = _binary_kld_e1000e_elf_start;
        *end   = _binary_kld_e1000e_elf_end;
    } else if (streq(name, "iwlwifi")) {
        *start = _binary_kld_iwlwifi_elf_start;
        *end   = _binary_kld_iwlwifi_elf_end;
    } else if (streq(name, "drmcirrus")) {
        *start = _binary_kld_drmcirrus_elf_start;
        *end   = _binary_kld_drmcirrus_elf_end;
    } else if (streq(name, "bochs")) {
        *start = _binary_kld_bochs_elf_start;
        *end   = _binary_kld_bochs_elf_end;
    } else if (streq(name, "i915")) {
        *start = _binary_kld_i915_elf_start;
        *end   = _binary_kld_i915_elf_end;
    }
    return *start && *end && *end > *start;
}

// ── device policy: native owners and native stand-down ──────────────────────
// classes the kernel drives itself. read from src/drivers: nvme (01:08),
// ahci (01:06), usb xhci (0c:03:30), hda (04:03), intel/amd/nvidia gpu and
// the display drivers (03). these are never taken. (satoru)
uint32_t class_base(uint32_t cc) { return (cc >> 16) & 0xFFu; }
uint32_t class_sub(uint32_t cc)  { return (cc >> 8) & 0xFFu; }

bool native_class(uint32_t class_code)
{
    uint32_t b = class_base(class_code);
    if (b == 0x01) return true;                       // mass storage
    if (b == 0x03) return true;                       // display
    if (b == 0x04) return true;                       // multimedia audio
    if (b == 0x0C && class_sub(class_code) == 0x03)   // usb host (xhci)
        return true;
    return false;
}

enum DevKind { DEV_OTHER, DEV_WIRED, DEV_WIFI };

DevKind dev_kind(uint32_t class_code)
{
    if (class_base(class_code) == 0x02)
        return class_sub(class_code) == 0x80 ? DEV_WIFI : DEV_WIRED;
    return DEV_OTHER;
}

// exact ids the native wired drivers bind: src/drivers/e1000.h
// (8086:100e/100f/10d3), src/drivers/igc.cpp IsIgcId (i225/i226 ...) and
// IGC_ID_82576. (satoru)
bool native_wired_id(uint32_t vendor, uint32_t device)
{
    if (vendor != 0x8086) return false;
    switch (device) {
    case 0x100E: case 0x100F: case 0x10D3:                            // e1000
    case 0x10C9:                                                      // 82576
    case 0x125C: case 0x125B: case 0x125D: case 0x125E:
    case 0x3102: case 0x5503:                                         // i226
    case 0x15F3: case 0x15F2: case 0x15F8: case 0x15F7:
    case 0x3100: case 0x3101:                                         // i225/i220
    case 0x5502: case 0x0D9F:
        return true;
    default:
        return false;
    }
}

struct DevView {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class_code;
    uint32_t bdf;   // last: the policy self check initialisers leave it 0 (satoru)
};

enum DevVerdict {
    VERDICT_NO_DRIVER,
    VERDICT_NATIVE,
    VERDICT_NOT_EMBEDDED,
    VERDICT_TAKE,
    VERDICT_START
};

struct Decision {
    DevVerdict verdict;
    DevKind    kind;
    const char* family;
    uint32_t   bdf;
};

typedef bool (*MatchFn)(const DevView& d, const char** family, bool* needs_table_page);
typedef bool (*EmbedFn)(const char* family);

bool real_match(const DevView& d, const char** family, bool* need)
{
    return MatchDevice(d.vendor, d.device, d.subvendor, d.subdevice,
                       d.class_code, family, need);
}

bool real_embedded(const char* family)
{
    const uint8_t* start;
    const uint8_t* end;
    return embed_elf(family, &start, &end);
}

// one device under the policy. pure over its inputs, so the self check can
// feed it synthetic device lists and has no second copy of the rules. (satoru)
Decision decide_device(const DevView& d, const Policy& p, MatchFn mf, EmbedFn ef)
{
    Decision r = { VERDICT_NO_DRIVER, dev_kind(d.class_code), nullptr, d.bdf };
    // a take by id may also name one adapter's bdf (take=vvvv:dddd@bb:dd.f):
    // two identical display adapters are the normal bochs/cirrus rig. (satoru)
    bool take_id = p.take_valid && d.vendor == p.take_vendor &&
                   d.device == p.take_device &&
                   (!p.take_bdf_valid || d.bdf == p.take_bdf);
    if (native_class(d.class_code)) {
        // a display controller is normally the native owner's, but an explicit
        // take of an id that a built family declares hands it to that family.
        // this is how the kld display containers get their gpu; nothing changes
        // without the take flag. (satoru)
        bool take_display = take_id;
        if (take_display && mf) {
            const char* dfam = nullptr;
            bool dneed = false;
            if (mf(d, &dfam, &dneed) && ef && ef(dfam)) {
                r.verdict = VERDICT_TAKE;
                r.family = dfam;
                return r;
            }
        }
        r.verdict = VERDICT_NATIVE;
        return r;
    }

    const char* fam = nullptr;
    bool need = false;
    bool matched = mf && mf(d, &fam, &need);
    bool embedded = matched && ef && ef(fam);
    bool take = take_id;

    // b: a wired id the native driver binds stays native, unless the take
    // override names it and the matched family is really embedded. (satoru)
    if (r.kind == DEV_WIRED && native_wired_id(d.vendor, d.device) &&
        !(take && embedded)) {
        r.verdict = VERDICT_NATIVE;
        return r;
    }
    // d reverse: kurono.kld.wifi=native keeps a wifi controller native.
    // (satoru)
    if (r.kind == DEV_WIFI && p.wifi_native && !(take && embedded)) {
        r.verdict = VERDICT_NATIVE;
        return r;
    }
    if (!matched) {
        r.verdict = VERDICT_NO_DRIVER;
        return r;
    }
    r.family = fam;
    if (!embedded) {
        r.verdict = VERDICT_NOT_EMBEDDED;
        return r;
    }
    r.verdict = take ? VERDICT_TAKE : VERDICT_START;
    return r;
}

struct StandDown {
    bool wired;
    bool wifi;
};

// native drivers stand down only for a kind with a device the kernel will
// really start. (satoru)
StandDown evaluate_devices(const DevView* devs, int n, const Policy& p,
                           MatchFn mf, EmbedFn ef)
{
    StandDown s = { false, false };
    for (int i = 0; i < n; i++) {
        Decision d = decide_device(devs[i], p, mf, ef);
        if (d.verdict != VERDICT_START && d.verdict != VERDICT_TAKE)
            continue;
        if (d.kind == DEV_WIRED) s.wired = true;
        if (d.kind == DEV_WIFI)  s.wifi = true;
    }
    if (!p.net_ok)
        s.wired = false;
    return s;
}

constexpr int MAX_SCAN_DEVS = 64;

int read_scan(DevView* out, int max)
{
    int n = KldBroker::ScanDeviceCount();
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint32_t bdf;
        if (!KldBroker::ScanDeviceAt(i, &bdf, &out[i].vendor, &out[i].device,
                                     &out[i].subvendor, &out[i].subdevice,
                                     &out[i].class_code))
            return i;
        out[i].bdf = bdf;
    }
    return n;
}

// ── boot cmdline ────────────────────────────────────────────────────────────
const char* token_find(const char* cmdline, const char* token)
{
    unsigned want = slen(token);
    const char* p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((unsigned)(p - t) == want && streq_n(t, token, want))
            return t;
    }
    return nullptr;
}

const char* token_prefix(const char* cmdline, const char* prefix)
{
    unsigned want = slen(prefix);
    const char* p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((unsigned)(p - t) > want && streq_n(t, prefix, want))
            return t;
    }
    return nullptr;
}

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_take(const char* cmdline)
{
    const char* t = token_prefix(cmdline, "kurono.kld.take=");
    if (!t) return false;
    const char* p = t + slen("kurono.kld.take=");
    uint32_t v = 0, d = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(*p++);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    if (*p++ != ':') return false;
    for (int i = 0; i < 4; i++) {
        int h = hexval(*p++);
        if (h < 0) return false;
        d = (d << 4) | (uint32_t)h;
    }
    // optional @bus:dev.fn (hex, 1-2 digits each): select one of two adapters
    // with the same vendor/device. (satoru)
    if (*p == '@') {
        p++;
        uint32_t bus = 0, dev = 0, fn = 0;
        int hn = 0;
        while (hn < 2 && hexval(*p) >= 0) { bus = (bus << 4) | (uint32_t)hexval(*p++); hn++; }
        if (!hn || *p++ != ':') return false;
        hn = 0;
        while (hn < 2 && hexval(*p) >= 0) { dev = (dev << 4) | (uint32_t)hexval(*p++); hn++; }
        if (!hn || *p++ != '.') return false;
        hn = 0;
        while (hn < 2 && hexval(*p) >= 0) { fn = (fn << 4) | (uint32_t)hexval(*p++); hn++; }
        if (!hn || bus > 255 || dev > 31 || fn > 7) return false;
        g_take_bdf = (bus << 16) | (dev << 8) | fn;
        g_take_bdf_valid = true;
    }
    if (*p != 0 && *p != ' ' && *p != '\t') return false;
    g_take_vendor = (uint16_t)v;
    g_take_device = (uint16_t)d;
    return true;
}

// ── job execution ───────────────────────────────────────────────────────────
void sleep_forever()
{
    for (;;) Scheduler::SleepMs(60000);
}

void job_entry()
{
    int idx = __atomic_fetch_add(&g_job_claim, 1, __ATOMIC_SEQ_CST);
    if (idx < 0 || idx >= g_job_count || !g_jobs[idx].used) {
        SerialLogger::Log("[kld-auto] spurious job process\r\n");
        sleep_forever();
    }
    const EmbedFamily* fam = g_jobs[idx].fam;
    KldDisplay::ResetAttachBudget();
    int restarts = 0;
    bool image_written = false;   // per boot, per family (satoru)
    for (;;) {
        const uint8_t* start;
        const uint8_t* end;

        if (!embed_elf(fam->name, &start, &end)) {
            SerialLogger::Log("[kld-auto] container vanished before launch\r\n");
            sleep_forever();
        }

        SerialLogger::Log("[kld-auto] launching ");
        SerialLogger::Log(fam->path);
        SerialLogger::Log("\r\n");
        uint32_t flen = (uint32_t)(end - start);
        // the restart loop must not rewrite the embedded image every time:
        // each WriteFile allocates new fs blocks and the old ones were never
        // reclaimed, so the kernel heap grew by the image size per restart
        // until the PMM ran out. the flag is per boot, not a size comparison:
        // a rebuilt image of the same length with different bytes must land.
        // (satoru)
        if (!image_written && g_imgtest) {
            // a stale image of the SAME length with different bytes: the old
            // size-compare path would have kept it. (satoru)
            uint8_t* junk = (uint8_t*)KernelHeap::Alloc(flen);
            if (junk) {
                memcpy(junk, start, flen);
                g_imgtest_off = flen / 2;
                g_imgtest_byte = start[g_imgtest_off];
                junk[g_imgtest_off] ^= 0xFF;
                KVFS::Mkdirs("/usr/bin");
                g_imgtest_staged = KVFS::WriteFile(fam->path, junk, flen) >= 0;
                KernelHeap::Free(junk);
            }
            SerialLogger::Log("[kld-auto] imgtest: staged same-size stale image=");
            SerialLogger::Log(g_imgtest_staged ? "PASS\r\n" : "FAIL\r\n");
        }
        if (!image_written) {
            KVFS::Mkdirs("/usr/bin");
            int wr = KVFS::WriteFile(fam->path, start, flen);
            if (wr == KVFS_OK) {
                KVFS::Chmod(fam->path, 0755);
                image_written = true;   // only a real write counts (satoru)
            } else {
                SerialLogger::Log("[kld-auto] image write FAILED rc=");
                SerialLogger::LogDec(wr);
                SerialLogger::Log("\r\n");
            }
            if (g_imgtest && g_imgtest_staged && !g_imgtest_checked) {
                g_imgtest_checked = true;
                KVFSNode* n = KVFS::Resolve(fam->path);
                bool ok = n && n->size == flen && n->content &&
                          n->content[g_imgtest_off] == g_imgtest_byte;
                SerialLogger::Log("[kld-auto] imgtest: same-size stale image replaced=");
                SerialLogger::Log(ok ? "PASS\r\n" : "FAIL\r\n");
            }
        }

        // kurono.kld.sup=1: the recovery supervisor owns load, run, release
        // and restart. our bounded loop below is the no-supervisor path. (satoru)
        if (KldSupervisor::Enabled()) {
            KldSupervisor::Run(fam->name, fam->path);
            sleep_forever();
        }

        Process* p = ElfLoader::LoadELF64FromVFS(fam->path, fam->name);
        if (!p) {
            SerialLogger::Log("[kld-auto] load FAILED\r\n");
            sleep_forever();
        }
        // kurono.kld.iommutest=1 appends the vt-d test argument to the
        // family's own argv; every other boot is unchanged. (satoru)
        const char* av_test[] = { fam->path, "iommutest", nullptr };
        const char* av[] = { fam->path, nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        // kurono.kld.aliastest=1: a display family keeps a scattered-page
        // vmap alias live for its lifetime (rig only). (satoru)
        // kurono.kld.restoretest=1: the same for the drm restore test. (satoru)
        // kurono.kld.model=: the container driving the model device is told
        // so ("model:<variant>"), its register model then answers the bar.
        // (satoru)
        const char* av_rig[7] = { fam->path, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        int nrig = 1;
        if (g_aliastest && !fam->network) av_rig[nrig++] = "aliastest";
        if (g_restoretest && !fam->network) av_rig[nrig++] = "restoretest";
        if (g_bartest && !fam->network && nrig < 4) av_rig[nrig++] = "bartest";
        if (g_failsetup && !fam->network && nrig < 4) av_rig[nrig++] = "failsetup";
        if (g_gtbroker && !fam->network && nrig < 6) av_rig[nrig++] = "gtbroker";
        char model_arg[24] = "model:";
        if (KldModel::IsModel(g_jobs[idx].bdf) && nrig < 6) {
            const char* v = KldModel::Variant();
            int k = 6;
            while (*v && k < 23) model_arg[k++] = *v++;
            model_arg[k] = 0;
            av_rig[nrig++] = model_arg;
        }
        const char** argv = KldIommu::TestMode() ? av_test
                          : (nrig > 1) ? av_rig : av;
        uint64_t as = p->address_space;
        KldBroker::SetDmaBudgetForName(as, fam->path, (int)p->pid);
        // the pin is guaranteed or the container does not start: without a
        // broker record PCI_FIND would fall back to the first device with
        // these ids. (satoru)
        if (!KldBroker::SetPreferredBdf(as, g_jobs[idx].bdf)) {
            SerialLogger::Log("[kld-auto] ");
            SerialLogger::Log(fam->name);
            SerialLogger::Log(" not started: no broker record to pin its device\r\n");
            KldBroker::ReleaseAddressSpace(as);
            Scheduler::DestroyProcess(p);
            sleep_forever();
        }
        if (KldModel::IsModel(g_jobs[idx].bdf))
            KldModel::ResetForContainer();
        int rc = Userspace::RunProcessWithArgs(p, argv, envp);
        SerialLogger::Log("[kld-auto] ");
        SerialLogger::Log(fam->name);
        SerialLogger::Log(" container exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
        KldBroker::ReleaseAddressSpace(as);
        // a display container is the desktop's only output in primary mode and
        // a live output otherwise: bring it back if it died. (satoru)
        if (!fam->network && KldDisplay::Wanted()) {
            restarts++;
            if (restarts > 2 && !KldDisplay::RestartUnlimitedForTest()) {
                SerialLogger::Log("[kld-auto] giving up restarts after 2 attempts\r\n");
                break;
            }
            SerialLogger::Log("[kld-auto] restarting ");
            SerialLogger::Log(fam->path);
            SerialLogger::Log(restarts == 1 ? " (1s)\r\n" : " (4s)\r\n");
            Scheduler::SleepMs(restarts == 1 ? 1000 : 4000);
            continue;
        }
        break;
    }
    sleep_forever();
}

bool family_job_started(const char* name)
{
    for (int i = 0; i < g_job_count; i++)
        if (g_jobs[i].used && streq(g_jobs[i].fam->name, name))
            return true;
    return false;
}

}  // namespace

// ── public ──────────────────────────────────────────────────────────────────
bool TablePresent()
{
    return table_present();
}

bool FamilyKnown(const char* family)
{
    return family && family[0] && table_present() && family_by_name(family) != nullptr;
}

const char* FamilyBarWindow(const char* family)
{
    if (!family || !table_present())
        return nullptr;
    const MatchFamily* f = family_by_name(family);
    return (f && f->bar_window && f->bar_window[0]) ? f->bar_window : nullptr;
}

uint32_t FamilyFlags(const char* family)
{
    if (!family || !table_present())
        return 0;
    const MatchFamily* f = family_by_name(family);
    return f ? f->flags : 0;
}

bool LegacyVgaForced()
{
    return g_legacy_vga_forced;
}

void SetTableAbsentForTest(bool on)
{
    g_table_absent_for_test = on;
}

void ParseIommuFlags(const char* boot_cmdline)
{
    if (!boot_cmdline)
        return;
    // kurono.kld.iommu=0 keeps today's dma behaviour, iommutest=1 is the
    // test-only canary path, iommuaw=39|48 exercises both table widths.
    // (satoru)
    if (token_find(boot_cmdline, "kurono.kld.iommu=0")) KldIommu::SetEnabled(false);
    if (token_find(boot_cmdline, "kurono.kld.iommutest=1")) KldIommu::SetTestMode(true);
    if (token_find(boot_cmdline, "kurono.kld.iommu.native=1")) KldIommu::SetNativeTest(true);
    const char* aw = token_prefix(boot_cmdline, "kurono.kld.iommuaw=");
    if (aw) {
        const char* p = aw + slen("kurono.kld.iommuaw=");
        int bits = 0;
        for (int i = 0; i < 4 && p[i] >= '0' && p[i] <= '9'; i++)
            bits = bits * 10 + (p[i] - '0');
        KldIommu::SetForcedAw(bits);
    }
}

void BootInit(const char* boot_cmdline)
{
    if (!boot_cmdline)
        return;
    KldSupervisor::BootInit(boot_cmdline);   // kurono.kld.sup / faultinject (satoru)
    KldModel::BootInit(boot_cmdline);        // before the first pci scan (satoru)
    if (token_find(boot_cmdline, "kurono.kld.bar=ro")) KldBroker::SetBarPolicyMode(1);
    else if (token_find(boot_cmdline, "kurono.kld.bar=window")) KldBroker::SetBarPolicyMode(2);
    ParseIommuFlags(boot_cmdline);
    g_take_valid = parse_take(boot_cmdline);
    // the model device carries the real igpu's ids (review f5): with
    // kurono.kld.model= on, a take of those ids means the model's bdf and
    // nothing else; a take naming another bdf for them is dropped. without the
    // model, a take without @bdf matches the first scanned device with those
    // ids (unchanged). (satoru)
    if (g_take_valid && KldModel::Enabled()) {
        uint16_t mv, md, sv, sd;
        uint32_t cc;
        KldModel::Identity(&mv, &md, &sv, &sd, &cc);
        if (g_take_vendor == mv && g_take_device == md) {
            if (g_take_bdf_valid && g_take_bdf != KldModel::Bdf()) {
                SerialLogger::Log("[kld-auto] take of the model's ids at a real bdf refused while kurono.kld.model is on\r\n");
                g_take_valid = false;
            } else {
                g_take_bdf = KldModel::Bdf();
                g_take_bdf_valid = true;
            }
        }
    }
    g_policy.take_valid = g_take_valid;
    g_policy.take_vendor = g_take_vendor;
    g_policy.take_device = g_take_device;
    g_policy.take_bdf_valid = g_take_bdf_valid;
    g_policy.take_bdf = g_take_bdf;
    g_policy.wifi_native =
        token_find(boot_cmdline, "kurono.kld.wifi=native") != nullptr;
    g_policy.net_ok =
        token_find(boot_cmdline, "kurono.kld.net=0") == nullptr;
    g_imgtest = token_find(boot_cmdline, "kurono.kld.imgtest=1") != nullptr;
    g_aliastest = token_find(boot_cmdline, "kurono.kld.aliastest=1") != nullptr;
    g_restoretest = token_find(boot_cmdline, "kurono.kld.restoretest=1") != nullptr;
    g_bartest = token_find(boot_cmdline, "kurono.kld.bartest=1") != nullptr;
    g_failsetup = token_find(boot_cmdline, "kurono.kld.failsetup=1") != nullptr;
    g_gtbroker = token_find(boot_cmdline, "kurono.kld.gtbroker=1") != nullptr;
    KldBroker::SetTlbSkipTest(token_find(boot_cmdline, "kurono.kld.tlbskip=1") != nullptr);
    g_legacy_vga_forced =
        token_find(boot_cmdline, "kurono.kld.display.legacyvga=1") != nullptr;
    KldBroker::SetLegacyVgaForced(g_legacy_vga_forced);

    // decide per DEVICE, here, before Network::Init: the broker's one lazy
    // scan is the only pci walk. (satoru)
    DevView devs[MAX_SCAN_DEVS];
    int n = table_present() ? read_scan(devs, MAX_SCAN_DEVS) : 0;
    StandDown s = evaluate_devices(devs, n, g_policy, real_match, real_embedded);

    if (s.wired) {
        KldNet::SetWanted(true);
        g_native_wired_deferred = true;
    }
    if (s.wifi) {
        WiFi::SetKldOwned(true);
        g_native_wifi_deferred = true;
    }
    SerialLogger::Log("[kld-auto] native wired: ");
    SerialLogger::Log(s.wired ? "deferred" : "kept");
    SerialLogger::Log(", native wifi: ");
    SerialLogger::Log(s.wifi ? "deferred" : "kept");
    SerialLogger::Log("\r\n");
}

bool MatchDevice(uint32_t vendor, uint32_t device, uint32_t subvendor,
                 uint32_t subdevice, uint32_t class_code,
                 const char** family_out, bool* needs_table_page_out)
{
    if (family_out) *family_out = nullptr;
    if (needs_table_page_out) *needs_table_page_out = false;
    if (!table_present())
        return false;
    const MatchFamily* fams = families();
    const MatchId* tab = ids();
    unsigned nids = ids_count();
    for (unsigned f = 0; f < family_count(); f++) {
        const MatchFamily& fam = fams[f];
        uint32_t first = fam.first_id;
        uint32_t stop = first + fam.id_count;
        if (first > nids) continue;
        if (stop > nids) stop = nids;
        for (uint32_t i = first; i < stop; i++) {
            if (!id_matches(tab[i], vendor, device, subvendor,
                            subdevice, class_code))
                continue;
            if (family_out) *family_out = fam.name;
            if (needs_table_page_out)
                *needs_table_page_out = (fam.flags & FAMILY_NEEDS_MSIX_TABLE_PAGE) != 0;
            return true;
        }
    }
    return false;
}

bool FamilyMatchDeviceByName(const char* family, uint32_t vendor,
                             uint32_t device, uint32_t subvendor,
                             uint32_t subdevice, uint32_t class_code,
                             bool* needs_table_page_out)
{
    if (needs_table_page_out) *needs_table_page_out = false;
    const MatchFamily* fam = family_by_name(family);
    if (!fam)
        return false;
    const MatchId* tab = ids();
    unsigned nids = ids_count();
    uint32_t first = fam->first_id;
    uint32_t stop = first + fam->id_count;
    if (first > nids) return false;
    if (stop > nids) stop = nids;
    for (uint32_t i = first; i < stop; i++) {
        if (!id_matches(tab[i], vendor, device, subvendor,
                        subdevice, class_code))
            continue;
        if (needs_table_page_out)
            *needs_table_page_out = (fam->flags & FAMILY_NEEDS_MSIX_TABLE_PAGE) != 0;
        return true;
    }
    return false;
}

void MsixPolicySelfCheck()
{
    // the same broker decision op_pci_find applies: the CALLER's family must
    // have the device in its own table and declare the flag. (satoru)
    bool t1 = KldBroker::MsixTableFlagForGrant("iwlwifi", 0x8086, 0x272b,
                                               0xFFFF, 0xFFFF, 0x028000);
    SerialLogger::Log("[kld-auto-check] flagged family + matching id -> table page allowed: ");
    SerialLogger::Log(t1 ? "PASS\r\n" : "FAIL\r\n");

    bool t2 = !KldBroker::MsixTableFlagForGrant("e1000e", 0x8086, 0x10d3,
                                                0xFFFF, 0xFFFF, 0x020000);
    SerialLogger::Log("[kld-auto-check] unflagged family -> table page refused: ");
    SerialLogger::Log(t2 ? "PASS\r\n" : "FAIL\r\n");

    bool t3 = !KldBroker::MsixTableFlagForGrant("iwlwifi", 0x8086, 0x10d3,
                                                0xFFFF, 0xFFFF, 0x020000);
    SerialLogger::Log("[kld-auto-check] flagged family + foreign id -> table page refused: ");
    SerialLogger::Log(t3 ? "PASS\r\n" : "FAIL\r\n");

    bool t4 = !KldBroker::MsixTableFlagForGrant("e1000e", 0x8086, 0x272b,
                                                0xFFFF, 0xFFFF, 0x028000);
    SerialLogger::Log("[kld-auto-check] unflagged caller asks for the flagged family's device -> refused: ");
    SerialLogger::Log(t4 ? "PASS\r\n" : "FAIL\r\n");

    SerialLogger::Log("[kld-auto-check] empty table -> refused: ");
    SetTableAbsentForTest(true);
    bool t5 = !KldBroker::MsixTableFlagForGrant("iwlwifi", 0x8086, 0x272b,
                                                0xFFFF, 0xFFFF, 0x028000);
    SetTableAbsentForTest(false);
    SerialLogger::Log(t5 ? "PASS\r\n" : "FAIL\r\n");

    SerialLogger::Log("[kld-auto-check] ");
    SerialLogger::LogDec((t1 ? 1 : 0) + (t2 ? 1 : 0) + (t3 ? 1 : 0) +
                         (t4 ? 1 : 0) + (t5 ? 1 : 0));
    SerialLogger::Log("/5 PASS\r\n");
}

namespace {

struct FakeId {
    uint32_t vendor;
    uint32_t device;
    const char* family;
};

const FakeId* g_fake_ids;
int g_fake_count;

bool fake_match(const DevView& d, const char** family, bool* need)
{
    if (need) *need = false;
    for (int i = 0; i < g_fake_count; i++) {
        if (g_fake_ids[i].vendor == d.vendor && g_fake_ids[i].device == d.device) {
            if (family) *family = g_fake_ids[i].family;
            return true;
        }
    }
    return false;
}

// the two families this kernel can launch; the synthetic mt7921e is a family
// that is in the table but not embedded here. (satoru)
bool fake_embedded(const char* family)
{
    return streq(family, "e1000e") || streq(family, "iwlwifi");
}

void report_case(const char* what, bool ok, int* pass)
{
    if (ok) (*pass)++;
    SerialLogger::Log("[kld-auto-check] policy ");
    SerialLogger::Log(what);
    SerialLogger::Log(": ");
    SerialLogger::Log(ok ? "PASS\r\n" : "FAIL\r\n");
}

Decision decide_fake(const DevView& d, const Policy& p, const FakeId* ids, int nids)
{
    g_fake_ids = ids;
    g_fake_count = nids;
    return decide_device(d, p, fake_match, fake_embedded);
}

StandDown standdown_fake(const DevView* devs, int n, const Policy& p,
                         const FakeId* ids, int nids)
{
    g_fake_ids = ids;
    g_fake_count = nids;
    return evaluate_devices(devs, n, p, fake_match, fake_embedded);
}

}  // namespace

void PolicySelfCheck()
{
    Policy p = {};
    p.net_ok = true;
    int pass = 0;

    // a machine with a native igc and a BE200: igc stays native, the BE200 is
    // matched by iwlwifi, so wifi stands down and wired does not. (satoru)
    {
        const DevView devs[] = {
            { 0x8086, 0x272B, 0xFFFF, 0xFFFF, 0x028000 },   // BE200
            { 0x8086, 0x125C, 0xFFFF, 0xFFFF, 0x020000 },   // native igc
        };
        const FakeId ids[] = { { 0x8086, 0x272B, "iwlwifi" } };
        StandDown s = standdown_fake(devs, 2, p, ids, 1);
        Decision be = decide_fake(devs[0], p, ids, 1);
        Decision ig = decide_fake(devs[1], p, ids, 1);
        bool ok = !s.wired && s.wifi &&
                  be.verdict == VERDICT_START && streq(be.family, "iwlwifi") &&
                  ig.verdict == VERDICT_NATIVE;
        report_case("{igc + BE200} -> wired kept, wifi deferred, iwlwifi chosen",
                    ok, &pass);
    }

    // an e1000e id is one the native wired driver binds: native keeps it and
    // nothing is deferred or started. (satoru)
    {
        const DevView devs[] = {
            { 0x8086, 0x10D3, 0xFFFF, 0xFFFF, 0x020000 },
        };
        const FakeId ids[] = { { 0x8086, 0x10D3, "e1000e" } };
        StandDown s = standdown_fake(devs, 1, p, ids, 1);
        Decision d = decide_fake(devs[0], p, ids, 1);
        bool ok = !s.wired && !s.wifi && d.verdict == VERDICT_NATIVE;
        report_case("{e1000e} -> native keeps it, nothing started", ok, &pass);
    }

    // same device with take: the native rule is overridden, wired stands down
    // and e1000e is chosen. (satoru)
    {
        const DevView devs[] = {
            { 0x8086, 0x10D3, 0xFFFF, 0xFFFF, 0x020000 },
        };
        const FakeId ids[] = { { 0x8086, 0x10D3, "e1000e" } };
        Policy pt = p;
        pt.take_valid = true;
        pt.take_vendor = 0x8086;
        pt.take_device = 0x10D3;
        StandDown s = standdown_fake(devs, 1, pt, ids, 1);
        Decision d = decide_fake(devs[0], pt, ids, 1);
        bool ok = s.wired && !s.wifi &&
                  d.verdict == VERDICT_TAKE && streq(d.family, "e1000e");
        report_case("{e1000e + take} -> wired deferred, e1000e chosen", ok, &pass);
    }

    // a NIC with no native id and no kld id: no driver, nothing stands down.
    // (satoru)
    {
        const DevView devs[] = {
            { 0x10EC, 0x8168, 0xFFFF, 0xFFFF, 0x020000 },
        };
        StandDown s = standdown_fake(devs, 1, p, nullptr, 0);
        Decision d = decide_fake(devs[0], p, nullptr, 0);
        bool ok = !s.wired && !s.wifi && d.verdict == VERDICT_NO_DRIVER;
        report_case("{unknown NIC 10ec:8168} -> no driver, nothing deferred",
                    ok, &pass);
    }

    // a matched family that is not embedded in this kernel: the device is not
    // started, so wifi must NOT stand down and the native fallback keeps it.
    // (satoru)
    {
        const DevView devs[] = {
            { 0x14C3, 0x7961, 0xFFFF, 0xFFFF, 0x028000 },
        };
        const FakeId ids[] = { { 0x14C3, 0x7961, "mt7921e" } };
        StandDown s = standdown_fake(devs, 1, p, ids, 1);
        Decision d = decide_fake(devs[0], p, ids, 1);
        bool ok = !s.wired && !s.wifi && d.verdict == VERDICT_NOT_EMBEDDED &&
                  streq(d.family, "mt7921e");
        report_case("{mt7921} -> (not embedded), wifi not deferred", ok, &pass);
    }

    SerialLogger::Log("[kld-auto-check] policy ");
    SerialLogger::LogDec((uint32_t)pass);
    SerialLogger::Log("/5 PASS\r\n");
}

void MsixCheckEntry()
{
    Scheduler::SleepMs(3000);
    MsixPolicySelfCheck();
    PolicySelfCheck();
    sleep_forever();
}

void RunnerEntry()
{
    Scheduler::SleepMs(3500);
    if (!table_present()) {
        SerialLogger::Log("[kld-auto] no match table embedded; nothing to do\r\n");
        sleep_forever();
    }

    int n = KldBroker::ScanDeviceCount();
    SerialLogger::Log("[kld-auto] pci devices=");
    SerialLogger::LogDec(n);
    SerialLogger::Log(" (broker scan table)\r\n");

    for (int i = 0; i < n; i++) {
        uint32_t bdf;
        DevView d;
        if (!KldBroker::ScanDeviceAt(i, &bdf, &d.vendor, &d.device,
                                     &d.subvendor, &d.subdevice, &d.class_code))
            continue;
        d.bdf = bdf;

        SerialLogger::Log("[kld-auto] ");
        log_hex4(d.vendor);
        SerialLogger::Log(":");
        log_hex4(d.device);
        SerialLogger::Log(" at ");
        log_bdf(bdf);
        SerialLogger::Log(" -> ");

        Decision dec = decide_device(d, g_policy, real_match, real_embedded);
        switch (dec.verdict) {
        case VERDICT_NATIVE:
            SerialLogger::Log("(native driver keeps it)\r\n");
            break;
        case VERDICT_NO_DRIVER:
            SerialLogger::Log("(no driver)\r\n");
            break;
        case VERDICT_NOT_EMBEDDED:
            SerialLogger::Log(dec.family);
            SerialLogger::Log(" (not embedded)\r\n");
            break;
        case VERDICT_TAKE:
        case VERDICT_START: {
            SerialLogger::Log(dec.family);
            if (family_job_started(dec.family)) {
                // one container per family drives exactly its pinned device;
                // the native driver of this kind already stood down (per kind),
                // so this device stays without a driver. (satoru)
                SerialLogger::Log(" (family already started for another device: left unclaimed)\r\n");
                break;
            }
            const EmbedFamily* ef = embed_by_name(dec.family);
            if (!ef) {
                SerialLogger::Log(" (not embedded)\r\n");
                break;
            }
            if (g_job_count >= MAX_JOBS) {
                SerialLogger::Log(" (job table full)\r\n");
                break;
            }
            g_jobs[g_job_count].used = true;
            g_jobs[g_job_count].fam = ef;
            g_jobs[g_job_count].bdf = dec.bdf;
            g_job_count++;
            SerialLogger::Log(dec.verdict == VERDICT_TAKE
                              ? " (taken: kurono.kld.take)\r\n"
                              : " (started)\r\n");
            break;
        }
        }
    }

    if (g_job_count == 0) {
        SerialLogger::Log("[kld-auto] no container started\r\n");
        sleep_forever();
    }

    for (int i = 0; i < g_job_count; i++)
        Scheduler::SpawnKernelProcess("kld-auto-run", job_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    if (KldIommu::TestMode())
        Scheduler::SpawnKernelProcess("kld-iommu-test", KldIommu::TestCheckEntry,
                                      PRIO_LOW, 64, 16 * 1024);
    SerialLogger::Log("[kld-auto] ");
    SerialLogger::LogDec(g_job_count);
    SerialLogger::Log(" container(s) starting\r\n");
    sleep_forever();
}

// is this pci function the one kurono.kld.take= hands to a container? the
// native intel display paths ask before they touch the device, so taking the
// igpu for a kld family is itself the switch that keeps them off it. only set
// under kurono.kld=auto (BootInit parses the take there). (satoru)
bool TakesDevice(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor, uint16_t device)
{
    if (!g_take_valid || vendor != g_take_vendor || device != g_take_device) return false;
    if (!g_take_bdf_valid) return true;
    return g_take_bdf == (((uint32_t)bus << 16) | ((uint32_t)dev << 8) | fn);
}

}  // namespace KldAuto

// end (satoru)
