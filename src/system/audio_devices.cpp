// audio_devices: see audio_devices.h. (satoru)
#include "audio_devices.h"
#include "ui_config.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_backend.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/hda.h"

namespace {

uint32_t pci_rd(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    uint32_t a = (1u << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(a), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

// an hd audio controller (class 04, subclass 03) on the pci bus, looked up once:
// the hda backend is only initialised when it wins at boot, so a usb dac that
// won must not hide the built-in speakers from the picker. (satoru)
bool hda_present() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = HDAudio::IsDetected() ? 1 : 0;
    for (uint32_t bus = 0; bus < 256 && !cached; bus++)
        for (uint32_t dev = 0; dev < 32 && !cached; dev++) {
            uint32_t id0 = pci_rd(bus, dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            bool multi = (pci_rd(bus, dev, 0, 0x0C) >> 16) & 0x80;
            for (uint32_t fn = 0; fn < (multi ? 8u : 1u); fn++) {
                if ((pci_rd(bus, dev, fn, 0) & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = pci_rd(bus, dev, fn, 0x08) >> 16;
                if (cls == 0x0403) { cached = 1; break; }
            }
        }
    return cached != 0;
}

bool usable(AudioBackend* be) {
    if (!be) return false;
    const char* n = be->Name();
    // the null sink and the pc speaker are fallbacks, not devices to pick. (satoru)
    if (n[0] == 'n' && n[1] == 'u' && n[2] == 'l') return false;
    if (n[0] == 'p' && n[1] == 'c' && n[2] == 's') return false;
    if (be->IsReady()) return true;
    // only the first working backend is brought up at boot: hda counts as
    // present when its controller was found, the legacy cards never. (satoru)
    if (n[0] == 'h' && n[1] == 'd' && n[2] == 'a') return hda_present();
    return false;
}

void copy(char* d, const char* s, int cap) {
    int i = 0;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void label_for(AudioBackend* be, char* out, int cap) {
    const char* n = be->Name();
    if (n[0] == 'h' && n[1] == 'd' && n[2] == 'a') { copy(out, "Speakers", cap); return; }
    if (n[0] == 'a' && n[1] == 'c' && n[2] == '9') { copy(out, "AC'97 audio", cap); return; }
    if (n[0] == 's' && n[1] == 'b' && n[2] == '1') { copy(out, "Sound Blaster", cap); return; }
    // usb: the device's own product name, without the "usbaudio: " prefix and
    // the vid:pid tail the backend adds for logs. (satoru)
    char d[96];
    be->Describe(d, (int)sizeof(d));
    const char* p = d;
    for (int i = 0; d[i]; i++) if (d[i] == ':' && d[i + 1] == ' ') { p = d + i + 2; break; }
    int o = 0;
    for (int i = 0; p[i] && o < cap - 1; i++) {
        if (p[i] == ' ' && p[i + 1] == ' ') break;
        if (p[i] == ' ' && p[i + 1] == 'v' && p[i + 2] == 'i' && p[i + 3] == 'd') break;
        out[o++] = p[i];
    }
    out[o] = 0;
    if (!out[0]) copy(out, n, cap);
}

void key_for(const char* name, char* key, int cap) {
    copy(key, "audio.vol.", cap);
    int p = 0; while (key[p]) p++;
    for (int i = 0; name[i] && p < cap - 1 && i < 16; i++) {
        char c = name[i];
        key[p++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    key[p] = 0;
}

AudioBackend* nth_usable(int i) {
    int k = 0;
    for (int b = 0; b < AudioServer::BackendCount(); b++) {
        AudioBackend* be = AudioServer::BackendAt(b);
        if (!usable(be)) continue;
        if (k++ == i) return be;
    }
    return nullptr;
}

}  // namespace

namespace AudioDevices {

int OutputCount() {
    int k = 0;
    for (int b = 0; b < AudioServer::BackendCount(); b++) if (usable(AudioServer::BackendAt(b))) k++;
    return k;
}

bool OutputInfo(int i, char* name, int ncap, char* label, int lcap, bool* active, bool* ready) {
    AudioBackend* be = nth_usable(i);
    if (!be) return false;
    if (name) copy(name, be->Name(), ncap);
    if (label) label_for(be, label, lcap);
    if (active) *active = (be == AudioServer::ActiveBackend());
    if (ready) *ready = be->IsReady();
    return true;
}

bool SelectOutput(const char* name) {
    AudioBackend* cur = AudioServer::ActiveBackend();
    if (cur) {
        char k[40]; key_for(cur->Name(), k, (int)sizeof(k));
        UIConfig::SetInt(k, AudioMixer::GetMasterVolume(), true);
    }
    if (!AudioServer::UseBackend(name)) return false;
    char k[40]; key_for(name, k, (int)sizeof(k));
    int v = UIConfig::Int(k, -1);
    if (v >= 0 && v <= 100) {
        AudioMixer::SetMasterVolume(v);
        if (Audio::IsAvailable()) Audio::SetMasterVolume(v);
        UIConfig::SetInt("audio.master_volume", v, true);
    }
    UIConfig::Set("audio.output", name, true);
    UIConfig::Save();
    return true;
}

void RememberVolume(int v) {
    AudioBackend* cur = AudioServer::ActiveBackend();
    if (!cur || v < 0 || v > 100) return;
    char k[40]; key_for(cur->Name(), k, (int)sizeof(k));
    UIConfig::SetInt(k, v, true);
}

int InputCount() { return 0; }

}  // namespace AudioDevices

// end (satoru)
