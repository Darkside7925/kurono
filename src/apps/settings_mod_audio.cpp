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
#include "../system/audio_devices.h"

using namespace SettingsUI;

static int  s_master = 75;
static bool s_muted  = false;
static bool s_restart_done = false, s_restart_ok = false;
static bool s_stop_done = false;
static int  s_stopped = 0;

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
    if(persist){
        AudioDevices::RememberVolume(s_master);   // each output keeps its own level (satoru)
        UIConfig::SetInt("audio.master_volume", s_master, true); UIConfig::Save();
    }
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

    // output picker: one row per device, a check on the active one. (satoru)
    {
        int n = AudioDevices::OutputCount();
        if(n > 1){
            Group("Output device");
            for(int i = 0; i < n && i < 6; i++){
                char nm[24], lb[48]; bool act = false, rdy = false;
                if(!AudioDevices::OutputInfo(i, nm, (int)sizeof(nm), lb, (int)sizeof(lb), &act, &rdy)) continue;
                PushId(i);
                bool on = act;
                if(Toggle(lb, &on, act ? "In use" : nullptr)){
                    if(!act && AudioDevices::SelectOutput(nm)){
                        s_master = AudioMixer::GetMasterVolume();
                    }
                }
                PopId();
            }
            EndGroup();
        }
    }

    Group("Output");
    IntToStr(s_master, buf, 64); StrApp(buf, "%", 64);
    if(Slider("Volume", &s_master, 0, 100, 1, s_muted ? "Muted" : buf)){
        apply_volume(Committed());
    }
    if(Toggle("Mute", &s_muted)) apply_mute();
    if(Button("Test sound", "Play", BTN_SECONDARY, "Plays a short tone at the current volume")){
        AudioServer::PlayTone(660, 140, 70);
    }
    if(Button("Restart audio", "Restart", BTN_SECONDARY,
              "Reopens the active output, including USB audio, without a reboot")){
        s_restart_ok = AudioServer::RestartActive();
        s_restart_done = true;
    }
    if(Button("Stop all audio", "Stop", BTN_SECONDARY,
              "Ends every playing stream (fixes a stuck or looping playback)")){
        s_stopped = AudioServer::StopAllStreams();
        s_stop_done = true;
    }
    EndGroup();

    Group("Input");
    if(AudioDevices::InputCount() == 0) Info("Microphone", "None", "No input device is supported yet");
    EndGroup();

    Group("Device");
    {
        AudioBackend* be = AudioServer::ActiveBackend();
        if(be) be->Describe(buf, 64); else StrCpy(buf, "No audio device", 64);
        Info("Output device", buf);
    }
    Info("Driver", (st.backend_name && st.backend_name[0]) ? st.backend_name : "None");
    Info("Status", st.backend_ready ? "Ready" : "Not ready", nullptr, st.backend_ready ? T().success : T().text3);
    if(s_restart_done)
        Info("Last restart", s_restart_ok ? "OK" : "FAILED (see serial log)", nullptr,
             s_restart_ok ? T().success : T().danger);
    if(s_stop_done){
        char sbuf[32];
        IntToStr(s_stopped, sbuf, 32);
        StrApp(sbuf, s_stopped == 1 ? " stream stopped" : " streams stopped", 32);
        Info("Last stop", sbuf);
    }
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
