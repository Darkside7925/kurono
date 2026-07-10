#pragma once

#include "../kernel/types.h"
#include "../virt/hypervisor.h"

enum ConduitEventType {
    CONDUIT_EVT_NONE = 0,
    CONDUIT_EVT_SYSTEM_BOOT,
    CONDUIT_EVT_BOOT_SEQUENCE,
    CONDUIT_EVT_SHUTDOWN,
    CONDUIT_EVT_PACKAGE_INSTALL,
    CONDUIT_EVT_PACKAGE_UPDATE,
    CONDUIT_EVT_PACKAGE_REMOVE,
    CONDUIT_EVT_GPU_RENDER,
    CONDUIT_EVT_WIFI_DRIVER,
    CONDUIT_EVT_AUDIO_DRIVER,
    CONDUIT_EVT_RAM_WARNING,
    CONDUIT_EVT_NVIDIA_FAULT,
    CONDUIT_EVT_GUEST_SWITCH,
    CONDUIT_EVT_GENERIC_COMMAND,
};

struct ConduitEvent {
    uint32_t seq;
    uint32_t timestamp_ms;
    ConduitEventType type;
    LinuxGuestProfile guest;
    char command[96];
    char detail[96];
    uint32_t metric_a;
    uint32_t metric_b;
};

class ConduitBridge {
public:
    static void Init();
    static void PollSystemState();
    static void RecordCommand(const char* cmdline);
    static int Consume(uint32_t after_seq, ConduitEvent* out, int max_events);
    static uint32_t GetLatestSeq();
};