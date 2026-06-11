//  kurono os  -  settings module: sound (satoru)
//  a detailed sound page: active output backend + device, detected hda codecs
//  and jack/config-default legend, a live master-volume slider, a mute toggle,
//  a balance slider, and a read-only format summary. all live writes go through
//  AudioMixer + AudioServer; all persistence goes through UIConfig. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_backend.h"
#include "../drivers/hda.h"
#include "../system/ui_config.h"

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static int  s_master  = 75;     // 0..100, persisted to audio.master_volume (satoru)
static bool s_muted   = false;  // persisted to audio.muted (satoru)
static int  s_balance = 50;     // 0..100 (50 = centre), persisted to audio.balance (satoru)
static int  s_pref_be = 0;      // index into kBackends, persisted to audio.preferred_backend (satoru)

// the registered backend candidates in AudioServer's probe-priority order. the
// server picks the first whose Init() succeeds, so the active one is detected
// at runtime; this list only drives the "preferred" browse dropdown since no
// public live-switch api exists. (matches audio_server.h / the backend Name()s) (satoru)
static const char* kBackends[]      = { "hda", "ac97", "sb16", "pcspk" };
static const char* kBackendLabels[] = { "Intel HD Audio", "AC'97", "Sound Blaster 16", "PC Speaker" };
static const int   kBackendCount    = (int)(sizeof(kBackends) / sizeof(kBackends[0]));

// ── helpers ─────────────────────────────────────────────────────────────────
// decode an hda codec vendor-id (high 16 bits of GetCodecVendor) to a name.
// values per the pci-sig vendor list; low 16 bits are the device id. (satoru)
static const char* hda_vendor_name(uint16_t vid){
    switch(vid){
        case 0x10EC: return "Realtek";
        case 0x8086: return "Intel";
        case 0x1AF4: return "Red Hat / virtio";
        case 0x1002: return "AMD/ATI";
        case 0x10DE: return "NVIDIA";
        case 0x14F1: return "Conexant";
        case 0x11D4: return "Analog Devices";
        case 0x1106: return "VIA";
        case 0x1013: return "Cirrus Logic";
        case 0x0000: return "Unknown";
        default:     return "HDA codec";
    }
}

// index of kBackends matching the live active backend name, else 0. (satoru)
static int active_backend_index(){
    const char* an = AudioServer::ActiveBackendName();
    if(an){
        for(int i = 0; i < kBackendCount; i++){
            const char* a = an; const char* b = kBackends[i];
            while(*a && *b && *a == *b){ a++; b++; }
            if(*a == 0 && *b == 0) return i;
        }
    }
    return 0;
}

// clamp helper (libc-free). (satoru)
static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static void apply_master(int v){
    s_master = clampi(v, 0, 100);
    AudioMixer::SetMasterVolume(s_master);          // live (audio_mixer.h) (satoru)
    UIConfig::SetInt("audio.master_volume", s_master, true);
    UIConfig::Save();
}

static void apply_mute(bool m){
    s_muted = m;
    AudioMixer::SetMasterMute(s_muted);             // live (audio_mixer.h) (satoru)
    UIConfig::SetInt("audio.muted", s_muted ? 1 : 0, true);
    UIConfig::Save();
}

static void persist_balance(int v){
    s_balance = clampi(v, 0, 100);
    UIConfig::SetInt("audio.balance", s_balance, true);
    UIConfig::Save();
}

static void persist_pref_backend(int idx){
    s_pref_be = clampi(idx, 0, kBackendCount - 1);
    UIConfig::Set("audio.preferred_backend", kBackends[s_pref_be], true);
    UIConfig::Save();
}

// ── on_show: (re)load config + (re)read live audio state ────────────────────
static void audio_on_show(){
    AudioServer::ServerStatus st = AudioServer::GetStatus();

    // seed from persisted config, else from the live mixer state. (satoru)
    s_master  = UIConfig::Int("audio.master_volume", st.master_volume);
    s_master  = clampi(s_master, 0, 100);
    s_muted   = UIConfig::Int("audio.muted", st.master_muted ? 1 : 0) != 0;
    s_balance = clampi(UIConfig::Int("audio.balance", 50), 0, 100);

    // preferred-backend selection: default to whatever is actually active. (satoru)
    s_pref_be = active_backend_index();
    const char* pref = UIConfig::Str("audio.preferred_backend", "");
    if(pref && pref[0]){
        for(int i = 0; i < kBackendCount; i++){
            const char* a = pref; const char* b = kBackends[i];
            while(*a && *b && *a == *b){ a++; b++; }
            if(*a == 0 && *b == 0){ s_pref_be = i; break; }
        }
    }
}

// ── layout constants shared by render + input so hit-testing matches the
//    drawing exactly (input() gets no pane width, so the control geometry is
//    fixed here just like the display reference module). (satoru)
static const int CTRL_X_OFF = 170;   // controls column, relative to pane left (satoru)
static const int CTRL_W     = 300;   // dropdown width in px (satoru)
static const int SLIDER_W   = 250;   // slider track width in px (satoru)

static int ctrl_w_for(int pane_w){
    int avail = pane_w - CTRL_X_OFF;
    int cw = CTRL_W;
    if(cw > avail) cw = avail;
    if(cw < 80) cw = 80;
    return cw;
}
static int slider_w_for(int ctrl_w){
    int sw = SLIDER_W;
    if(sw > ctrl_w - 50) sw = ctrl_w - 50;
    if(sw < 60) sw = 60;
    return sw;
}

static void audio_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ctrl_w = ctrl_w_for(w);
    int sl_w   = slider_w_for(ctrl_w);
    int ly = y - scroll + 8;
    char buf[64];

    AudioServer::ServerStatus st = AudioServer::GetStatus();

    // ── output device ────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Output Device");
    ly += 26;
    SettingsUI::Row(x, ly, "Backend:",
                    (st.backend_name && st.backend_name[0]) ? st.backend_name : "none");
    ly += 22;

    // a friendly device description: prefer the backend's own Describe(). (satoru)
    {
        AudioBackend* be = AudioServer::ActiveBackend();
        if(be){ be->Describe(buf, 64); }
        else  { SettingsUI::StrCpy(buf, "No audio device", 64); }
        SettingsUI::Row(x, ly, "Device:", buf);
    }
    ly += 22;
    SettingsUI::Row(x, ly, "State:", st.backend_ready ? "Ready" : "Not ready");
    ly += 22;
    {
        SettingsUI::IntToStr((int)st.active_streams, buf, 64);
        SettingsUI::StrApp(buf, " active", 64);
        SettingsUI::Row(x, ly, "Streams:", buf);
    }
    ly += 30;

    // preferred-backend browse dropdown. note: no live-switch api, so this is a
    // persisted preference applied on next boot (the active backend above is the
    // one the server actually selected this boot). (satoru)
    Graphics::DrawString(x, ly + 4, "Preferred:", SettingsUI::COL_TEXT, 0xFF000000);
    {
        SettingsUI::StrCpy(buf, kBackendLabels[s_pref_be], 64);
        if(s_pref_be == active_backend_index()) SettingsUI::StrApp(buf, " (active)", 64);
        SettingsUI::Dropdown(ctrl_x, ly, ctrl_w, buf);
    }
    ly += 30;

    // ── headphones & jacks ───────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Headphones & Jacks");
    ly += 26;

    if(HDAudio::IsDetected()){
        int cc = HDAudio::GetCodecCount();
        SettingsUI::IntToStr(cc, buf, 64);
        SettingsUI::StrApp(buf, (cc == 1) ? " codec" : " codecs", 64);
        SettingsUI::Row(x, ly, "HDA codecs:", buf);
        ly += 22;

        // per-codec vendor:device, decoded from GetCodecVendor (hi=vendor). (satoru)
        for(int i = 0; i < cc && i < 4; i++){
            uint32_t v   = HDAudio::GetCodecVendor(i);
            uint16_t vid = (uint16_t)(v >> 16);
            uint16_t did = (uint16_t)(v & 0xFFFF);
            char lab[24]; SettingsUI::StrCpy(lab, "Codec ", 24);
            char nb[8]; SettingsUI::IntToStr(i, nb, 8); SettingsUI::StrApp(lab, nb, 24);
            SettingsUI::StrApp(lab, ":", 24);

            SettingsUI::StrCpy(buf, hda_vendor_name(vid), 64);
            SettingsUI::StrApp(buf, "  [", 64);
            char hb[8];
            SettingsUI::IntToStr((int)vid, hb, 8); SettingsUI::StrApp(buf, hb, 64);
            SettingsUI::StrApp(buf, ":", 64);
            SettingsUI::IntToStr((int)did, hb, 8); SettingsUI::StrApp(buf, hb, 64);
            SettingsUI::StrApp(buf, "]", 64);
            SettingsUI::Row(x, ly, lab, buf);
            ly += 22;
        }

        // config-default legend: the pin "configuration default" dword encodes
        // each physical jack. these are the spec bitfields the driver parses to
        // tell a green headphone jack from an internal speaker. (satoru)
        Graphics::DrawString(x, ly + 4, "Pin config-default decode (HDA spec):",
                             SettingsUI::COL_DIM, 0xFF000000);
        ly += 22;
        SettingsUI::Row(x, ly, "Bits 31:30:", "port-connectivity (jack / fixed / both / none)");
        ly += 22;
        SettingsUI::Row(x, ly, "Bits 23:20:", "default-device (line-out / speaker / hp-out)");
        ly += 22;
        SettingsUI::Row(x, ly, "Bits 15:12:", "color (green=HP, pink=mic, black=line)");
        ly += 22;

        // worked examples so the page reads like a real jack map. (satoru)
        SettingsUI::Row(x, ly, "Headphone:", "green jack, 1/8\", external (port=jack)");
        ly += 22;
        SettingsUI::Row(x, ly, "Speaker:", "internal, fixed (port=fixed, no jack)");
        ly += 30;
    } else {
        SettingsUI::Row(x, ly, "HDA codec:", "not detected on this system");
        ly += 22;
        SettingsUI::Row(x, ly, "Jacks:", "managed by the active backend mixer");
        ly += 30;
    }

    // ── volume ───────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Volume");
    ly += 26;

    // master volume slider with a percentage readout. (satoru)
    Graphics::DrawString(x, ly + 2, "Master:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Slider(ctrl_x, ly, sl_w, s_master);
    SettingsUI::IntToStr(s_master, buf, 64); SettingsUI::StrApp(buf, "%", 64);
    Graphics::DrawString(ctrl_x + sl_w + 10, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
    ly += 30;

    // mute toggle. (satoru)
    Graphics::DrawString(x, ly + 2, "Mute:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_muted);
    ly += 32;

    // balance slider (0=left, 50=centre, 100=right) with an L/C/R readout. (satoru)
    Graphics::DrawString(x, ly + 2, "Balance:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Slider(ctrl_x, ly, sl_w, s_balance);
    {
        const char* bl;
        if(s_balance < 45)      bl = "Left";
        else if(s_balance > 55) bl = "Right";
        else                    bl = "Center";
        Graphics::DrawString(ctrl_x + sl_w + 10, ly, bl, SettingsUI::COL_DIM, 0xFF000000);
    }
    ly += 24;
    // honest note: the mixer exposes only per-stream pan, not a master balance,
    // so this is saved as a preference but not applied to the master bus. (satoru)
    Graphics::DrawString(x, ly, "Saved as a preference  -  the mixer has no master balance bus.",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 16;

    // ── format ───────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Format");
    ly += 26;
    {
        // backend rate if known, else the mixer's internal rate. (satoru)
        uint32_t rate = st.backend_rate ? st.backend_rate : AudioMixer::INTERNAL_RATE;
        SettingsUI::IntToStr((int)rate, buf, 64);
        SettingsUI::StrApp(buf, " Hz", 64);
        SettingsUI::Row(x, ly, "Sample rate:", buf);
    }
    ly += 22;
    SettingsUI::Row(x, ly, "Bit depth:", "16-bit signed (24-in-32 mixer)");
    ly += 22;
    {
        SettingsUI::IntToStr(AudioMixer::INTERNAL_CHANNELS, buf, 64);
        SettingsUI::StrApp(buf, " (stereo)", 64);
        SettingsUI::Row(x, ly, "Channels:", buf);
    }
    ly += 22;
    SettingsUI::Row(x, ly, "Mixer:", "software, 16 streams, soft limiter");
    ly += 22;
}

// ── input: pane-local mx,my. walk the SAME running-y layout as render (already
//    offset by -scroll) so the hit rects line up. controls sit at pane-local
//    x = CTRL_X_OFF with the fixed widths above. (satoru)
static bool audio_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ctrl_w = CTRL_W;
    int sl_w   = SLIDER_W;
    int ly = -scroll + 8;

    // ── output device section ────────────────────────────────────────────
    ly += 26;   // "Output Device" header (satoru)
    ly += 22;   // backend row (satoru)
    ly += 22;   // device row (satoru)
    ly += 22;   // state row (satoru)
    ly += 22;   // streams row (satoru)

    // preferred-backend dropdown. (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x, ly, ctrl_w, mx, my);
        if(hit >= 0){
            int idx = s_pref_be + ((hit == 0) ? -1 : 1);
            if(idx < 0) idx = kBackendCount - 1;
            if(idx >= kBackendCount) idx = 0;
            persist_pref_backend(idx);
            return true;
        }
    }
    ly += 30;   // preferred row (satoru)

    // ── headphones & jacks section: read-only, just advance past it exactly as
    //    render laid it out (depends on whether hda was detected). (satoru)
    ly += 26;   // "Headphones & Jacks" header (satoru)
    if(HDAudio::IsDetected()){
        int cc = HDAudio::GetCodecCount();
        int shown = cc; if(shown > 4) shown = 4;
        ly += 22;                 // codec-count row (satoru)
        ly += 22 * shown;         // one row per shown codec (satoru)
        ly += 22;                 // "Pin config-default decode" caption (satoru)
        ly += 22;                 // bits 31:30 row (satoru)
        ly += 22;                 // bits 23:20 row (satoru)
        ly += 22;                 // bits 15:12 row (satoru)
        ly += 22;                 // headphone example row (satoru)
        ly += 30;                 // speaker example row (satoru)
    } else {
        ly += 22;                 // "HDA codec: not detected" row (satoru)
        ly += 30;                 // "Jacks:" row (satoru)
    }

    // ── volume section ───────────────────────────────────────────────────
    ly += 26;   // "Volume" header (satoru)

    // master volume slider. (satoru)
    {
        int p = SettingsUI::SliderHit(ctrl_x, ly, sl_w, mx, my);
        if(p >= 0){ apply_master(p); return true; }
    }
    ly += 30;   // master row (satoru)

    // mute toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        apply_mute(!s_muted);
        return true;
    }
    ly += 32;   // mute row (satoru)

    // balance slider. (satoru)
    {
        int p = SettingsUI::SliderHit(ctrl_x, ly, sl_w, mx, my);
        if(p >= 0){ persist_balance(p); return true; }
    }
    ly += 24 + 16;   // balance slider + honest note row (satoru)

    // format section is read-only  -  nothing to hit below here. (satoru)
    return false;
}

// total content height for the scrollbar  -  must mirror the row advances in
// render(), including the hda-detected branch. (satoru)
static int audio_content_height(){
    int hh = 8;
    // output device. (satoru)
    hh += 26 + 22 + 22 + 22 + 22 + 30;
    // headphones & jacks. (satoru)
    hh += 26;
    if(HDAudio::IsDetected()){
        int cc = HDAudio::GetCodecCount();
        int shown = cc; if(shown > 4) shown = 4;
        hh += 22 + 22 * shown + 22 + 22 + 22 + 22 + 22 + 30;
    } else {
        hh += 22 + 30;
    }
    // volume: header + master(30) + mute(32) + balance(24) + honest note(16). (satoru)
    hh += 26 + 30 + 32 + 24 + 16;
    // format. (satoru)
    hh += 26 + 22 + 22 + 22 + 22;
    // tail padding. (satoru)
    hh += 16;
    return hh;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_audio_module;` resolves at link time. without
// it, a namespace-scope const has internal linkage and the link fails. (satoru)
extern const SettingsModule g_audio_module = {
    "audio", "Sound", "\x07",
    audio_on_show, audio_render, audio_input, audio_content_height
};
// end (satoru)
