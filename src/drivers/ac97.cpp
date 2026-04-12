#include "ac97.h"
#include "../drivers/serial.h"

//  kurono os  -  ac'97 audio codec driver implementation
//  real pci scan, mixer programming, bdl dma playback

AC97Info AC97::info;
bool     AC97::muted = false;
int      AC97::master_vol = 80;

uint8_t* AC97::dma_buffer = nullptr;
AC97BufferDescriptor* AC97::bdl = nullptr;

const uint8_t* AC97::pcm_source = nullptr;
int   AC97::pcm_length = 0;
int   AC97::pcm_offset = 0;
bool  AC97::looping = false;

static inline void _out8(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void _out16(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void _out32(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t _in8(uint16_t port) {
    uint8_t val; asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port)); return val;
}
static inline uint16_t _in16(uint16_t port) {
    uint16_t val; asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port)); return val;
}
static inline uint32_t _in32(uint16_t port) {
    uint32_t val; asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port)); return val;
}

static uint32_t pci_rd(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (1u<<31) | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)func<<8) | (off & 0xFC);
    _out32(0xCF8, addr);
    return _in32(0xCFC);
}
static void pci_wr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
    uint32_t addr = (1u<<31) | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)func<<8) | (off & 0xFC);
    _out32(0xCF8, addr);
    _out32(0xCFC, v);
}

//  pci scan  -  find ac'97 controller
bool AC97::ScanPCI() {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_rd(bus, dev, func, 0x00);
                if (id == 0xFFFFFFFF) continue;

                uint16_t vid = id & 0xFFFF;
                uint16_t did = (id >> 16) & 0xFFFF;

                // check for audio class (04:01 = multimedia audio)
                uint32_t class_reg = pci_rd(bus, dev, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;

                // ac97 controllers: class 04 subclass 01
                if (base_class != 0x04 || sub_class != 0x01) continue;

                // verify this is ac97 (not hda which is 04:03)
                // known intel ich controllers
                bool known = false;
                if (vid == 0x8086) {  // intel
                    if (did == AC97_INTEL_ICH || did == AC97_INTEL_ICH2 ||
                        did == AC97_INTEL_ICH3 || did == AC97_INTEL_ICH4 ||
                        did == AC97_INTEL_ICH5 || did == AC97_INTEL_ICH6 ||
                        did == AC97_INTEL_ICH7)
                        known = true;
                }
                if (vid == 0x1039 && did == AC97_SIS_7012) known = true;
                if (vid == 0x1106 && (did == AC97_VIA_686 || did == AC97_VIA_8233)) known = true;

                // accept any 04:01 device even if not in known list
                // (qemu exposes intel ich ac97 at 04:01)
                (void)known;

                info.controller_vendor = vid;
                info.controller_device = did;

                // read bar0  -  native audio mixer registers (i/o space)
                uint32_t bar0 = pci_rd(bus, dev, func, 0x10);
                info.mixer_base = bar0 & 0xFFFC;  // i/o base

                // read bar1  -  native audio bus master registers
                uint32_t bar1 = pci_rd(bus, dev, func, 0x14);
                info.bus_master_base = bar1 & 0xFFFC;

                // enable bus mastering + i/o space
                uint32_t cmd = pci_rd(bus, dev, func, 0x04);
                cmd |= 0x05;  // i/o space + bus master
                pci_wr(bus, dev, func, 0x04, cmd);

                return true;
            }
        }
    }
    return false;
}

//  mixer i/o
uint16_t AC97::MixerRead(uint8_t reg) {
    return _in16(info.mixer_base + reg);
}

void AC97::MixerWrite(uint8_t reg, uint16_t val) {
    _out16(info.mixer_base + reg, val);
}

//  bus master i/o
uint8_t  AC97::BMRead8(uint16_t offset) { return _in8(info.bus_master_base + offset); }
uint16_t AC97::BMRead16(uint16_t offset) { return _in16(info.bus_master_base + offset); }
uint32_t AC97::BMRead32(uint16_t offset) { return _in32(info.bus_master_base + offset); }
void AC97::BMWrite8(uint16_t offset, uint8_t val) { _out8(info.bus_master_base + offset, val); }
void AC97::BMWrite16(uint16_t offset, uint16_t val) { _out16(info.bus_master_base + offset, val); }
void AC97::BMWrite32(uint16_t offset, uint32_t val) { _out32(info.bus_master_base + offset, val); }

//  codec reset & init
void AC97::ResetCodec() {
    // write any value to reset register
    MixerWrite(AC97_RESET, 0x42);

    // wait for codec ready (poll powerdown register)
    for (int i = 0; i < 100000; i++) {
        uint16_t pd = MixerRead(AC97_POWERDOWN);
        if (pd & 0x0F) break;  // codec sections ready
        for (volatile int j = 0; j < 100; j++) {}
    }

    // read codec vendor id
    info.codec_vendor_id1 = MixerRead(AC97_VENDOR_ID1);
    info.codec_vendor_id2 = MixerRead(AC97_VENDOR_ID2);

    // check for variable rate audio (vra) support
    uint16_t ext_id = MixerRead(AC97_EXT_AUDIO_ID);
    info.variable_rate = (ext_id & 0x01) != 0;

    // enable vra if supported
    if (info.variable_rate) {
        uint16_t ext_ctrl = MixerRead(AC97_EXT_AUDIO_CTRL);
        ext_ctrl |= 0x01;  // enable vra
        MixerWrite(AC97_EXT_AUDIO_CTRL, ext_ctrl);
    }

    // set default volumes
    MixerWrite(AC97_MASTER_VOL, 0x0808);     // -12db both channels
    MixerWrite(AC97_PCM_OUT_VOL, 0x0808);    // -12db both channels
    MixerWrite(AC97_HEADPHONE_VOL, 0x0808);

    // power up all sections
    MixerWrite(AC97_POWERDOWN, 0x0000);
}

//  bdl (buffer descriptor list) setup
void AC97::SetupBDL() {
    // bdl must be in physical memory accessible to dma
    // we use the region at 0x70000 (same area as sb16, but ac97 and sb16
    // won't coexist on the same system)
    bdl = (AC97BufferDescriptor*)0x70000;
    dma_buffer = (uint8_t*)0x80000;  // 512kb above bdl

    // initialize bdl entries  -  each points to a dma buffer chunk
    for (int i = 0; i < AC97_MAX_BDL_ENTRIES; i++) {
        bdl[i].buffer_addr = (uint32_t)(uintptr_t)(dma_buffer + i * AC97_BUFFER_SIZE);
        bdl[i].length = AC97_BUFFER_SIZE / 2;  // in samples (16-bit = 2 bytes)
        bdl[i].flags = AC97_BD_IOC;            // interrupt on completion
    }

    // set pcm out bdl base address
    BMWrite32(AC97_BM_PCM_OUT + BM_BDBAR, (uint32_t)(uintptr_t)bdl);
}

//  init
bool AC97::Init() {
    info.available = false;
    info.state = AC97_STOPPED;
    info.sample_rate = AC97_DEFAULT_RATE;
    info.channels = 2;
    info.bits = 16;

    if (!ScanPCI()) return false;

    SerialLogger::Log("[AC97] Controller ");
    SerialLogger::LogHex(info.controller_vendor);
    SerialLogger::Log(":");
    SerialLogger::LogHex(info.controller_device);
    SerialLogger::Log(" @ Mixer=0x");
    SerialLogger::LogHex(info.mixer_base);
    SerialLogger::Log(" BusMaster=0x");
    SerialLogger::LogHex(info.bus_master_base);
    SerialLogger::Log("\r\n");

    // reset bus master
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, BM_CR_RESET);
    for (volatile int i = 0; i < 10000; i++) {}
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, 0);

    ResetCodec();
    SetupBDL();

    info.available = true;

    SerialLogger::Log("[AC97] Codec Vendor: ");
    SerialLogger::LogHex(info.codec_vendor_id1);
    SerialLogger::Log(":");
    SerialLogger::LogHex(info.codec_vendor_id2);
    SerialLogger::Log(" VRA=");
    SerialLogger::Log(info.variable_rate ? "yes" : "no");
    SerialLogger::Log("\r\n");

    return true;
}

bool AC97::IsAvailable() { return info.available; }

//  sample rate
bool AC97::SetSampleRate(int rate) {
    if (!info.available) return false;

    if (info.variable_rate) {
        // set front dac rate
        MixerWrite(AC97_PCM_FRONT_RATE, (uint16_t)rate);
        // verify it took effect
        uint16_t actual = MixerRead(AC97_PCM_FRONT_RATE);
        info.sample_rate = actual;
        return (actual == (uint16_t)rate);
    }

    // without vra, ac97 is locked to 48000hz
    info.sample_rate = 48000;
    return (rate == 48000);
}

int AC97::GetSampleRate() { return info.sample_rate; }

//  volume control
void AC97::SetMasterVolume(int volume) {
    if (!info.available) return;
    master_vol = volume;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    // ac97 volume: 0 = max, 63 = -94.5db (mute at bit 15)
    int attn = 63 - (volume * 63 / 100);
    uint16_t reg = ((uint16_t)attn << 8) | (uint16_t)attn;
    if (muted) reg |= 0x8000;

    MixerWrite(AC97_MASTER_VOL, reg);
}

int AC97::GetMasterVolume() { return master_vol; }

void AC97::SetPCMVolume(int volume) {
    if (!info.available) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    int attn = 31 - (volume * 31 / 100);
    uint16_t reg = ((uint16_t)attn << 8) | (uint16_t)attn;
    if (muted) reg |= 0x8000;

    MixerWrite(AC97_PCM_OUT_VOL, reg);
}

int AC97::GetPCMVolume() {
    if (!info.available) return 0;
    uint16_t reg = MixerRead(AC97_PCM_OUT_VOL);
    int attn = reg & 0x1F;
    return (31 - attn) * 100 / 31;
}

void AC97::SetMuted(bool m) {
    muted = m;
    SetMasterVolume(master_vol);  // reapply with mute flag
}

bool AC97::IsMuted() { return muted; }

//  buffer filling
void AC97::FillBuffer(int idx) {
    if (idx < 0 || idx >= AC97_MAX_BDL_ENTRIES) return;

    uint8_t* dest = dma_buffer + idx * AC97_BUFFER_SIZE;
    int to_copy = AC97_BUFFER_SIZE;

    if (pcm_source && pcm_offset < pcm_length) {
        int remaining = pcm_length - pcm_offset;
        if (to_copy > remaining) to_copy = remaining;

        for (int i = 0; i < to_copy; i++)
            dest[i] = pcm_source[pcm_offset + i];

        // zero-pad if less than full buffer
        for (int i = to_copy; i < AC97_BUFFER_SIZE; i++)
            dest[i] = 0;

        pcm_offset += to_copy;

        // loop handling
        if (pcm_offset >= pcm_length && looping)
            pcm_offset = 0;
    } else {
        // silence
        for (int i = 0; i < AC97_BUFFER_SIZE; i++)
            dest[i] = 0;
    }

    // update bdl entry
    bdl[idx].length = AC97_BUFFER_SIZE / 2;  // sample count
    bdl[idx].flags = AC97_BD_IOC;
}

//  dma control
void AC97::StartDMA() {
    // set bdl base
    BMWrite32(AC97_BM_PCM_OUT + BM_BDBAR, (uint32_t)(uintptr_t)bdl);

    // set last valid index
    BMWrite8(AC97_BM_PCM_OUT + BM_LVI, AC97_MAX_BDL_ENTRIES - 1);

    // clear status bits
    BMWrite16(AC97_BM_PCM_OUT + BM_STATUS,
              BM_STATUS_LVBCI | BM_STATUS_BCIS | BM_STATUS_FIFOE);

    // start: run + interrupt on completion
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, BM_CR_RUN | BM_CR_IOCE | BM_CR_LVBIE);
}

void AC97::StopDMA() {
    // clear run bit
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, 0);

    // wait for dma to halt
    for (int i = 0; i < 10000; i++) {
        if (BMRead16(AC97_BM_PCM_OUT + BM_STATUS) & BM_STATUS_DCH) break;
        for (volatile int j = 0; j < 100; j++) {}
    }
}

//  playback control
bool AC97::Play(const uint8_t* pcm_data, int length, int sample_rate, int bits, int channels) {
    if (!info.available || !pcm_data || length <= 0) return false;

    Stop();

    pcm_source = pcm_data;
    pcm_length = length;
    pcm_offset = 0;
    looping = false;

    info.bits = bits;
    info.channels = channels;

    // configure sample rate
    SetSampleRate(sample_rate);

    // fill initial buffers
    int buffers_to_fill = 4;  // pre-fill 4 buffers
    if (buffers_to_fill > AC97_MAX_BDL_ENTRIES) buffers_to_fill = AC97_MAX_BDL_ENTRIES;

    for (int i = 0; i < buffers_to_fill; i++)
        FillBuffer(i);

    info.state = AC97_PLAYING;
    StartDMA();

    return true;
}

void AC97::Stop() {
    StopDMA();
    pcm_source = nullptr;
    pcm_length = 0;
    pcm_offset = 0;
    info.state = AC97_STOPPED;
}

void AC97::Pause() {
    if (info.state != AC97_PLAYING) return;
    // clear run bit but keep configuration
    uint8_t cr = BMRead8(AC97_BM_PCM_OUT + BM_CR);
    cr &= ~BM_CR_RUN;
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, cr);
    info.state = AC97_PAUSED;
}

void AC97::Resume() {
    if (info.state != AC97_PAUSED) return;
    uint8_t cr = BMRead8(AC97_BM_PCM_OUT + BM_CR);
    cr |= BM_CR_RUN;
    BMWrite8(AC97_BM_PCM_OUT + BM_CR, cr);
    info.state = AC97_PLAYING;
}

//  irq handler
void AC97::HandleIRQ() {
    if (!info.available) return;

    uint16_t status = BMRead16(AC97_BM_PCM_OUT + BM_STATUS);

    if (status & BM_STATUS_BCIS) {
        // buffer completion  -  refill the completed buffer
        int civ = BMRead8(AC97_BM_PCM_OUT + BM_CIV);
        int next = (civ + 1) % AC97_MAX_BDL_ENTRIES;
        FillBuffer(next);

        // update lvi to keep dma running
        BMWrite8(AC97_BM_PCM_OUT + BM_LVI,
                 (uint8_t)((civ + AC97_MAX_BDL_ENTRIES - 1) % AC97_MAX_BDL_ENTRIES));
    }

    if (status & BM_STATUS_LVBCI) {
        // last valid buffer reached  -  playback may be ending
        if (pcm_offset >= pcm_length && !looping) {
            info.state = AC97_STOPPED;
        }
    }

    // acknowledge interrupt bits
    BMWrite16(AC97_BM_PCM_OUT + BM_STATUS, status & 0x1C);
}

//  tick  -  poll-based buffer refill
void AC97::Tick() {
    if (info.state != AC97_PLAYING) return;

    // check if dma has advanced
    uint16_t status = BMRead16(AC97_BM_PCM_OUT + BM_STATUS);
    if (status & BM_STATUS_BCIS) {
        HandleIRQ();
    }

    // check for dma halt (underrun)
    if (status & BM_STATUS_DCH) {
        if (pcm_offset < pcm_length) {
            // buffer underrun  -  restart dma
            int civ = BMRead8(AC97_BM_PCM_OUT + BM_CIV);
            for (int i = 0; i < 4; i++)
                FillBuffer((civ + i) % AC97_MAX_BDL_ENTRIES);
            StartDMA();
        } else {
            info.state = AC97_STOPPED;
        }
    }
}

AC97State AC97::GetState() { return info.state; }
AC97Info  AC97::GetInfo()  { return info; }
