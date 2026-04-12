#pragma once

#include "../ui/window_manager.h"
#include "../system/conduit.h"

#define CONDUIT_MAX_MESSAGES 64

enum ConduitMode {
    CONDUIT_MODE_ENGLISH = 0,
    CONDUIT_MODE_CMD = 1,
};

enum ConduitSpeaker {
    CONDUIT_SPEAKER_KURONO = 0,
    CONDUIT_SPEAKER_GUEST = 1,
};

struct ConduitMessage {
    ConduitSpeaker speaker;
    char speaker_name[16];
    char text[256];
    int total_words;
    int revealed_words;
};

class ConduitApp {
public:
    static void Open();
    static void Close();
    static bool IsOpen();
    static void OnRender(Window* w);
    static void OnInput(Window* w, int event, int a, int b);

    static int win_id;

private:
    static ConduitMode mode;
    static bool secret_enabled;
    static bool cold_start_blocking;
    static uint32_t last_seq;
    static uint32_t last_word_tick_ms;
    static ConduitMessage messages[CONDUIT_MAX_MESSAGES];
    static int message_count;

    static void ResetState();
    static void LoadSecret();
    static void ApplyTitle();
    static void Tick();
    static void SeedGreeting();
    static void ConsumeEvents();
    static void AppendDialogue(const ConduitEvent& ev);
    static void PushMessage(ConduitSpeaker speaker, const char* speaker_name, const char* text);
    static void RenderChrome(Window* w);
    static void RenderMessages(Window* w);
};