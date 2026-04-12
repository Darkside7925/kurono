#include "conduit.h"
#include "../drivers/graphics.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/timer.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"

namespace {
    static constexpr int CONDUIT_MAX_EVENTS = 128;

    static ConduitEvent g_events[CONDUIT_MAX_EVENTS];
    static bool g_initialized = false;
    static uint32_t g_next_seq = 1;
    static LinuxGuestProfile g_last_guest = LINUX_GUEST_ALPINE;
    static bool g_last_guest_booted = false;
    static uint32_t g_last_frames = 0;
    static uint32_t g_last_gpu_emit_ms = 0;
    static uint32_t g_last_ram_emit_ms = 0;
    static uint32_t g_last_any_emit_ms = 0;
    static bool g_ram_low = false;
    static uint32_t g_last_nvidia_fault_code = 0;

    static int c_len(const char* s) {
        int n = 0;
        if (!s) return 0;
        while (s[n]) n++;
        return n;
    }

    static void c_copy(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int i = 0;
        if (src) {
            while (src[i] && i < max_len - 1) {
                dst[i] = src[i];
                i++;
            }
        }
        dst[i] = 0;
    }

    static char c_lower(char ch) {
        if (ch >= 'A' && ch <= 'Z') return (char)(ch + 32);
        return ch;
    }

    static bool c_eq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b) {
            if (*a != *b) return false;
            a++; b++;
        }
        return *a == 0 && *b == 0;
    }

    static bool c_contains_ci(const char* haystack, const char* needle) {
        if (!haystack || !needle || !needle[0]) return false;
        int nl = c_len(needle);
        for (int i = 0; haystack[i]; i++) {
            int j = 0;
            while (needle[j] && haystack[i + j] && c_lower(haystack[i + j]) == c_lower(needle[j])) {
                j++;
            }
            if (j == nl) return true;
        }
        return false;
    }

    static void c_first_token(const char* line, char* out, int max_len) {
        if (!out || max_len < 1) return;
        out[0] = 0;
        if (!line) return;
        while (*line == ' ' || *line == '\t') line++;
        int i = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && i < max_len - 1) {
            out[i] = line[i];
            i++;
        }
        out[i] = 0;
    }

    static void c_copy_word_after(const char* line, const char* key, char* out, int max_len) {
        if (!out || max_len < 1) return;
        out[0] = 0;
        if (!line || !key) return;

        int key_len = c_len(key);
        for (int i = 0; line[i]; i++) {
            int j = 0;
            while (key[j] && line[i + j] && c_lower(line[i + j]) == c_lower(key[j])) j++;
            if (j == key_len) {
                i += key_len;
                while (line[i] == ' ' || line[i] == '\t') i++;
                int o = 0;
                while (line[i] && line[i] != ' ' && line[i] != '\t' && o < max_len - 1) {
                    out[o++] = line[i++];
                }
                out[o] = 0;
                return;
            }
        }
    }

    static uint32_t c_parse_hex_after_0x(const char* line) {
        if (!line) return 0;
        for (int i = 0; line[i] && line[i + 1]; i++) {
            if (line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                i += 2;
                uint32_t value = 0;
                bool any = false;
                while (line[i]) {
                    char ch = line[i];
                    uint32_t digit;
                    if (ch >= '0' && ch <= '9') digit = (uint32_t)(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') digit = (uint32_t)(10 + ch - 'a');
                    else if (ch >= 'A' && ch <= 'F') digit = (uint32_t)(10 + ch - 'A');
                    else break;
                    value = (value << 4) | digit;
                    any = true;
                    i++;
                }
                return any ? value : 0;
            }
        }
        return 0;
    }

    static bool guest_booted(LinuxGuestProfile guest) {
        return guest == LINUX_GUEST_DEBIAN ? Hypervisor::IsDebianBooted() : Hypervisor::IsAlpineBooted();
    }

    static void push_event(ConduitEventType type, LinuxGuestProfile guest,
                           const char* command, const char* detail,
                           uint32_t metric_a, uint32_t metric_b) {
        ConduitEvent* ev = &g_events[(g_next_seq - 1) % CONDUIT_MAX_EVENTS];
        ev->seq = g_next_seq++;
        ev->timestamp_ms = Timer::GetRealMs();
        ev->type = type;
        ev->guest = guest;
        c_copy(ev->command, command, (int)sizeof(ev->command));
        c_copy(ev->detail, detail, (int)sizeof(ev->detail));
        ev->metric_a = metric_a;
        ev->metric_b = metric_b;
    }

    static ConduitEventType classify_command(const char* cmdline, char* detail_out, int detail_max) {
        if (!detail_out || detail_max < 1) return CONDUIT_EVT_GENERIC_COMMAND;
        detail_out[0] = 0;

        char token[48];
        c_first_token(cmdline, token, sizeof(token));

        if (c_contains_ci(cmdline, "shutdown") || c_contains_ci(cmdline, "poweroff") ||
            c_contains_ci(cmdline, "halt") || c_eq(token, "reboot")) {
            c_copy(detail_out, "orderly powerdown", detail_max);
            return CONDUIT_EVT_SHUTDOWN;
        }
        if (c_eq(token, "apt") || c_eq(token, "apk")) {
            if (c_contains_ci(cmdline, " install ") || c_contains_ci(cmdline, "install ")) {
                c_copy_word_after(cmdline, "install", detail_out, detail_max);
                if (!detail_out[0]) c_copy(detail_out, "package", detail_max);
                return CONDUIT_EVT_PACKAGE_INSTALL;
            }
            if (c_contains_ci(cmdline, " update") || c_contains_ci(cmdline, " upgrade")) {
                c_copy(detail_out, "repository sync", detail_max);
                return CONDUIT_EVT_PACKAGE_UPDATE;
            }
            if (c_contains_ci(cmdline, " remove") || c_contains_ci(cmdline, " purge")) {
                c_copy_word_after(cmdline, "remove", detail_out, detail_max);
                if (!detail_out[0]) c_copy_word_after(cmdline, "purge", detail_out, detail_max);
                if (!detail_out[0]) c_copy(detail_out, "package", detail_max);
                return CONDUIT_EVT_PACKAGE_REMOVE;
            }
        }
        if (c_contains_ci(cmdline, "wifi") || c_contains_ci(cmdline, "wpa") ||
            c_contains_ci(cmdline, "iwconfig") || c_contains_ci(cmdline, "ifconfig") ||
            c_contains_ci(cmdline, "ip link") || c_contains_ci(cmdline, "network")) {
            c_copy(detail_out, "radio handshake", detail_max);
            return CONDUIT_EVT_WIFI_DRIVER;
        }
        if (c_contains_ci(cmdline, "audio") || c_contains_ci(cmdline, "alsa") ||
            c_contains_ci(cmdline, "pipewire") || c_contains_ci(cmdline, "sound") ||
            c_contains_ci(cmdline, "hda") || c_contains_ci(cmdline, "ac97") ||
            c_contains_ci(cmdline, "volume") || c_contains_ci(cmdline, "speaker")) {
            c_copy(detail_out, "codec buffer", detail_max);
            return CONDUIT_EVT_AUDIO_DRIVER;
        }
        if (c_contains_ci(cmdline, "nvidia") &&
            (c_contains_ci(cmdline, "crash") || c_contains_ci(cmdline, "fault") ||
             c_contains_ci(cmdline, "reset") || c_contains_ci(cmdline, "panic"))) {
            c_copy(detail_out, "driver fault", detail_max);
            return CONDUIT_EVT_NVIDIA_FAULT;
        }
        if (c_contains_ci(cmdline, "gpu") || c_contains_ci(cmdline, "render") ||
            c_contains_ci(cmdline, "frame") || c_contains_ci(cmdline, "ffmpeg") ||
            c_contains_ci(cmdline, "ffprobe") || c_contains_ci(cmdline, "video") ||
            c_contains_ci(cmdline, "browser")) {
            c_copy(detail_out, "render queue", detail_max);
            return CONDUIT_EVT_GPU_RENDER;
        }
        if (c_contains_ci(cmdline, "debian") || c_contains_ci(cmdline, "alpine") ||
            c_contains_ci(cmdline, "vm boot") || c_contains_ci(cmdline, "switch linux")) {
            c_copy(detail_out, "guest handoff", detail_max);
            return CONDUIT_EVT_GUEST_SWITCH;
        }

        if (token[0]) c_copy(detail_out, token, detail_max);
        else c_copy(detail_out, "command", detail_max);
        return CONDUIT_EVT_GENERIC_COMMAND;
    }
}

void ConduitBridge::Init() {
    if (g_initialized) return;
    g_initialized = true;
    g_last_guest = Hypervisor::GetLinuxGuestProfile();
    g_last_guest_booted = guest_booted(g_last_guest);
    g_last_frames = Graphics::GetDrawStats().frames_rendered;
    g_last_nvidia_fault_code = 0;
    push_event(CONDUIT_EVT_BOOT_SEQUENCE, g_last_guest, "boot", "desktop link established", 0, 0);
}

void ConduitBridge::PollSystemState() {
    Init();

    LinuxGuestProfile guest = Hypervisor::GetLinuxGuestProfile();
    bool booted = guest_booted(guest);
    uint32_t now = Timer::GetRealMs();

    // global cooldown: no more than one event every 4 seconds to
    // avoid overwhelming the conduit ui with rapid-fire messages
    bool cooldown_ok = (now - g_last_any_emit_ms >= 4000);

    if (guest != g_last_guest) {
        push_event(CONDUIT_EVT_GUEST_SWITCH, guest, "guest-switch", Hypervisor::GetLinuxGuestProfileName(), 0, 0);
        g_last_any_emit_ms = now;
        g_last_guest = guest;
        g_last_guest_booted = booted;
    } else if (booted && !g_last_guest_booted) {
        push_event(CONDUIT_EVT_BOOT_SEQUENCE, guest, "guest-boot", Hypervisor::GetLinuxGuestProfileName(), 0, 0);
        g_last_any_emit_ms = now;
        g_last_guest_booted = true;
    }

    const Graphics::DrawStats& stats = Graphics::GetDrawStats();
    uint32_t frame_delta = stats.frames_rendered - g_last_frames;
    if (cooldown_ok && frame_delta >= 300 && now - g_last_gpu_emit_ms >= 10000) {
        push_event(CONDUIT_EVT_GPU_RENDER, guest, "frameburst", "desktop compositor", frame_delta, stats.current_fps);
        g_last_gpu_emit_ms = now;
        g_last_any_emit_ms = now;
        g_last_frames = stats.frames_rendered;
    }

    NvidiaGPU::PollTelemetry();
    if (NvidiaGPU::HasFault()) {
        uint32_t fault_code = NvidiaGPU::GetLastFaultCode();
        if (fault_code != 0 && fault_code != g_last_nvidia_fault_code) {
            push_event(CONDUIT_EVT_NVIDIA_FAULT, guest, "nvidia-fault", "live driver fault", fault_code, (uint32_t)NvidiaGPU::GetState());
            g_last_any_emit_ms = now;
            g_last_nvidia_fault_code = fault_code;
        }
        NvidiaGPU::ClearFault();
    }

    uint64_t free_mem = PMM::GetFreeMemory();
    uint64_t total_mem = PMM::GetTotalMemory();
    uint64_t heap_used = (uint64_t)KernelHeap::GetUsed();
    bool low_mem = total_mem > 0 && (free_mem < (total_mem / 5) || free_mem < (64ULL * 1024ULL * 1024ULL));
    if (low_mem && cooldown_ok && (!g_ram_low || now - g_last_ram_emit_ms >= 15000)) {
        push_event(CONDUIT_EVT_RAM_WARNING, guest, "memory", "pressure rising", (uint32_t)(free_mem / (1024ULL * 1024ULL)), (uint32_t)(heap_used / 1024ULL));
        g_last_ram_emit_ms = now;
        g_last_any_emit_ms = now;
    }
    g_ram_low = low_mem;
}

void ConduitBridge::RecordCommand(const char* cmdline) {
    if (!cmdline || !cmdline[0]) return;
    Init();

    char detail[96];
    ConduitEventType type = classify_command(cmdline, detail, sizeof(detail));
    LinuxGuestProfile guest = Hypervisor::GetLinuxGuestProfile();
    uint32_t metric_a = 0;
    uint32_t metric_b = 0;

    if (type == CONDUIT_EVT_NVIDIA_FAULT) {
        NvidiaGPU::PollTelemetry();
        metric_a = NvidiaGPU::GetLastFaultCode();
        if (metric_a == 0) metric_a = c_parse_hex_after_0x(cmdline);
        metric_b = (uint32_t)NvidiaGPU::GetState();
        if (metric_a != 0) g_last_nvidia_fault_code = metric_a;
    }

    push_event(type, guest, cmdline, detail, metric_a, metric_b);
}

int ConduitBridge::Consume(uint32_t after_seq, ConduitEvent* out, int max_events) {
    if (!out || max_events <= 0) return 0;
    Init();

    uint32_t latest = GetLatestSeq();
    if (latest == 0 || after_seq >= latest) return 0;

    uint32_t oldest = latest >= (uint32_t)CONDUIT_MAX_EVENTS ? latest - (uint32_t)CONDUIT_MAX_EVENTS + 1 : 1;
    uint32_t seq = after_seq + 1;
    if (seq < oldest) seq = oldest;

    int count = 0;
    for (; seq <= latest && count < max_events; seq++) {
        out[count++] = g_events[(seq - 1) % CONDUIT_MAX_EVENTS];
    }
    return count;
}

uint32_t ConduitBridge::GetLatestSeq() {
    return g_next_seq > 1 ? g_next_seq - 1 : 0;
}