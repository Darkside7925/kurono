//  kurono os - settings module: sound (satoru)
//  master volume (live through the mixer and the legacy audio path the menu
//  bar reads), mute, a test tone, and what the audio server actually drives.
//  volume + mute persist and are restored at boot (SystemSettings::RestoreAtBoot). (satoru)
#include "system_settings.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_backend.h"
#include "../drivers/hda.h"
#include "../system/ui_config.h"

using namespace SettingsUI;

static int  s_master = 75;
static bool s_muted  = false;

static const char* hda_vendor_name(uint16_t vid){
    switch(vid){
        case 0x10EC: return "Realtek";
        case 0x8086: return "Intel";
        case 0x1AF4: return "Red Hat";
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x14F1: return "Conexant";
        case 0x11D4: return "Analog Devices";
        case 0x1106: return "VIA";
        case 0x1013: return "Cirrus Logic";
        default:     return "HD Audio codec";
    }
}

static void audio_on_show(){
    AudioServer::ServerStatus st = AudioServer::GetStatus();
    s_master = AudioMixer::GetMasterVolume();
    if(s_master < 0 || s_master > 100) s_master = st.master_volume;
    s_muted  = st.master_muted;
}

static void apply_volume(bool persist){
    AudioMixer::SetMasterVolume(s_master);
    if(Audio::IsAvailable()) Audio::SetMasterVolume(s_master);
    if(persist){ UIConfig::SetInt("audio.master_volume", s_master, true); UIConfig::Save(); }
}
static void apply_mute(){
    AudioMixer::SetMasterMute(s_muted);
    if(Audio::IsAvailable()) Audio::SetMuted(s_muted);
    UIConfig::SetInt("audio.muted", s_muted ? 1 : 0, true);
    UIConfig::Save();
}

static void audio_build(){
    AudioServer::ServerStatus st = AudioServer::GetStatus();
    char buf[64];

    Group("Output");
    IntToStr(s_master, buf, 64); StrApp(buf, "%", 64);
    if(Slider("Volume", &s_master, 0, 100, 1, s_muted ? "Muted" : buf)){
        apply_volume(Committed());
    }
    if(Toggle("Mute", &s_muted)) apply_mute();
    if(Button("Test sound", "Play", BTN_SECONDARY, "Plays a short tone at the current volume")){
        AudioServer::PlayTone(660, 140, 70);
    }
    EndGroup();

    Group("Device");
    {
        AudioBackend* be = AudioServer::ActiveBackend();
        if(be) be->Describe(buf, 64); else StrCpy(buf, "No audio device", 64);
        Info("Output device", buf);
    }
    Info("Driver", (st.backend_name && st.backend_name[0]) ? st.backend_name : "None");
    Info("Status", st.backend_ready ? "Ready" : "Not ready", nullptr, st.backend_ready ? T().success : T().text3);
    {
        uint32_t rate = st.backend_rate ? st.backend_rate : AudioMixer::INTERNAL_RATE;
        IntToStr((int)rate, buf, 64); StrApp(buf, " Hz, 16-bit, stereo", 64);
        Info("Format", buf);
    }
    if(HDAudio::IsDetected() && HDAudio::GetCodecCount() > 0){
        int cc = HDAudio::GetCodecCount();
        StrCpy(buf, "", 64);
        for(int i = 0; i < cc && i < 3; i++){
            if(i) StrApp(buf, ", ", 64);
            StrApp(buf, hda_vendor_name((uint16_t)(HDAudio::GetCodecVendor(i) >> 16)), 64);
        }
        Info(cc == 1 ? "Codec" : "Codecs", buf);
    }
    {
        IntToStr(st.active_streams, buf, 64);
        StrApp(buf, st.active_streams == 1 ? " stream" : " streams", 64);
        Info("Playing", buf);
    }
    EndGroup();
}

extern const SettingsModule g_audio_module = {
    "audio", "Sound", "", audio_on_show, nullptr, nullptr, nullptr, audio_build
};
// end (satoru)
