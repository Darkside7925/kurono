// laptop_keys: see laptop_keys.h. (satoru)
#include "laptop_keys.h"
#include "audio_devices.h"
#include "backlight.h"
#include "ui_config.h"
#include "../drivers/audio.h"
#include "../drivers/audio_mixer.h"
#include "../net/network.h"
#include "../ui/notification.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"

namespace LaptopKeys {

namespace {
void set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    AudioMixer::SetMasterVolume(v);
    if (Audio::IsAvailable()) Audio::SetMasterVolume(v);
    if (v > 0 && AudioMixer::GetMasterMute()) AudioMixer::SetMasterMute(false);
    UIConfig::SetInt("audio.master_volume", v, true);
    AudioDevices::RememberVolume(v);
    Graphics::MarkUIDirty();
}
}  // namespace

bool Handle(Key k, bool pressed) {
    switch (k) {
        case KEY_MUTE: case KEY_VOLUMEDOWN: case KEY_VOLUMEUP: case KEY_PLAYPAUSE:
        case KEY_BRIGHTNESSDOWN: case KEY_BRIGHTNESSUP: case KEY_WLAN:
            break;
        default:
            return false;
    }
    if (!pressed) return true;
    SerialLogger::Log("[keys] system key ");
    SerialLogger::LogDec((int)k - (int)KEY_MUTE);
    SerialLogger::Log("\r\n");
    switch (k) {
        case KEY_MUTE: {
            bool m = !AudioMixer::GetMasterMute();
            AudioMixer::SetMasterMute(m);
            if (Audio::IsAvailable()) Audio::SetMuted(m);
            UIConfig::SetInt("audio.muted", m ? 1 : 0, true);
            Graphics::MarkUIDirty();
            break;
        }
        case KEY_VOLUMEDOWN: set_volume(AudioMixer::GetMasterVolume() - 6); break;
        case KEY_VOLUMEUP:   set_volume(AudioMixer::GetMasterVolume() + 6); break;
        case KEY_BRIGHTNESSDOWN: Backlight::Step(-1); break;
        case KEY_BRIGHTNESSUP:   Backlight::Step(+1); break;
        case KEY_WLAN: {
            if (WiFi::DetectedLink() != WiFi::LINK_WIFI) break;
            bool on = WiFi::GetState() == WIFI_OFF;
            if (on) { WiFi::Enable(); WiFi::Scan(); } else WiFi::Disable();
            UIConfig::SetInt("net.wifi_enabled", on ? 1 : 0, true);
            NotificationManager::Post("Wi-Fi", on ? "Wi-Fi is on" : "Wi-Fi is off",
                                      NotificationManager::ICON_INFO, 1500);
            break;
        }
        default: break;   // play/pause: no media session to send it to yet (satoru)
    }
    return true;
}

}  // namespace LaptopKeys

// end (satoru)
