#include "ac97.h"
#include "../drivers/serial.h"
#include "audio_dma.h"

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

// Per-entry PCM ring chunk size.  Sized to match the mixer's period
// (1024 frames × 4 bytes/frame stereo s16 = 4 KB) so each Submit() fills
// exactly one BDL entry and the controller never plays half-empty
// chunks.  32 entries × 4 KB = 128 KB, fits in REGION_AC97_PCM (288 KB).
static constexpr int kAC97RingChunkBytes = 4096;
static constexpr int kAC97RingChunkSamples = kAC97RingChunkBytes / 2;

// Streaming-mode bookkeeping for the AudioMixer integration.  When the
// stream is "live" we never call Play()/Stop() per period -- we just keep
// the next entry filled and advance LVI.
static bool     g_stream_live = false;
static int      g_stream_next_fill = 0;   // BDL index we'll write next
static uint32_t g_stream_queued_bytes = 0;
// last civ we observed in WriteRingChunk.  used to detect that the dma engine
// advanced past one or more entries since the last refill so we can zero the
// stale period(s) it left behind  -  an underrun must produce clean silence, not
// a replay of old bdl data (which is an audible pop/click). (satoru)
static int      g_stream_last_civ = 0;

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

    // set default volumes to MAX gain (0 attenuation). 0x0808 was ~-12db on
    // each channel which, combined with later attenuation math, left output
    // near-inaudible. SetMasterVolume() attenuates from this full-gain base. (satoru)
    MixerWrite(AC97_MASTER_VOL, 0x0000);     // max gain, no attenuation
    MixerWrite(AC97_PCM_OUT_VOL, 0x0000);    // max gain, no attenuation
    MixerWrite(AC97_HEADPHONE_VOL, 0x0000);

    // power up all sections
    MixerWrite(AC97_POWERDOWN, 0x0000);
}

//  bdl (buffer descriptor list) setup
void AC97::SetupBDL() {
    // Take ownership of two dedicated DMA regions:
    //   AC97_BDL  region (32 KB)  -  first 256 bytes hold the BDL itself,
    //                              the rest is reserved scratch.
    //   AC97_PCM  region (288 KB)  -  the per-buffer 8 KB PCM chunks.
    void* bdl_region = AudioDMA::Acquire(AudioDMA::REGION_AC97_BDL, "ac97-bdl");
    void* pcm_region = AudioDMA::Acquire(AudioDMA::REGION_AC97_PCM, "ac97-pcm");
    if (!bdl_region || !pcm_region) {
        // Fall back to legacy hardcoded addresses if the allocator is
        // unavailable (e.g. AC97 init runs before AudioDMA::Init()).
        bdl        = (AC97BufferDescriptor*)0x70000;
        dma_buffer = (uint8_t*)0x80000;
    } else {
        bdl        = (AC97BufferDescriptor*)bdl_region;
        dma_buffer = (uint8_t*)pcm_region;
    }

    // initialize bdl entries  -  each points to an 8 KB chunk.  Total ring
    // is 32 * 8 KB = 256 KB which fits inside the dedicated AC97 PCM
    // region (288 KB) without overrunning into adjacent allocator areas.
    for (int i = 0; i < AC97_MAX_BDL_ENTRIES; i++) {
        bdl[i].buffer_addr = (uint32_t)(uintptr_t)(dma_buffer + i * kAC97RingChunkBytes);
        bdl[i].length = kAC97RingChunkSamples;  // sample (s16) count per entry
        bdl[i].flags = AC97_BD_IOC;
    }

    // Pre-zero the entire ring so the first round of playback (which
    // happens before the mixer has filled anything) doesn't blast garbage.
    for (int i = 0; i < AC97_MAX_BDL_ENTRIES * kAC97RingChunkBytes; i++) {
        dma_buffer[i] = 0;
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

    uint8_t* dest = dma_buffer + idx * kAC97RingChunkBytes;
    int to_copy = kAC97RingChunkBytes;

    if (pcm_source && pcm_offset < pcm_length) {
        int remaining = pcm_length - pcm_offset;
        if (to_copy > remaining) to_copy = remaining;

        for (int i = 0; i < to_copy; i++)
            dest[i] = pcm_source[pcm_offset + i];

        for (int i = to_copy; i < kAC97RingChunkBytes; i++)
            dest[i] = 0;

        pcm_offset += to_copy;

        if (pcm_offset >= pcm_length && looping)
            pcm_offset = 0;
    } else {
        for (int i = 0; i < kAC97RingChunkBytes; i++)
            dest[i] = 0;
    }

    bdl[idx].length = kAC97RingChunkSamples;
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
    g_stream_live = false;
    g_stream_next_fill = 0;
    g_stream_queued_bytes = 0;
    g_stream_last_civ = 0;
    info.state = AC97_STOPPED;
}

bool AC97::EnsureStreaming(int sample_rate, int bits, int channels) {
    if (!info.available) return false;
    if (g_stream_live && info.state == AC97_PLAYING) return true;

    // Drain any previous one-shot playback first.
    if (info.state != AC97_STOPPED) {
        StopDMA();
    }
    pcm_source = nullptr;
    pcm_length = 0;
    pcm_offset = 0;
    looping = false;

    info.bits     = bits;
    info.channels = channels;
    SetSampleRate(sample_rate);

    // Pre-fill the whole ring with silence and arm the DMA engine.
    for (int i = 0; i < AC97_MAX_BDL_ENTRIES; i++) {
        uint8_t* dest = dma_buffer + i * kAC97RingChunkBytes;
        for (int j = 0; j < kAC97RingChunkBytes; j++) dest[j] = 0;
        bdl[i].length = kAC97RingChunkSamples;
        bdl[i].flags  = AC97_BD_IOC;
    }
    g_stream_next_fill = 0;
    g_stream_queued_bytes = 0;
    g_stream_live = true;
    info.state = AC97_PLAYING;
    StartDMA();
    // StartDMA pre-arms LVI to the last entry; for streaming mode we
    // want LVI to track WriteRingChunk progress instead.  Park it one
    // entry behind the fill cursor so the controller waits for fresh
    // data before advancing past the silence ring.
    BMWrite8(AC97_BM_PCM_OUT + BM_LVI, (uint8_t)0);
    g_stream_next_fill = 1;     // first chunk we fill goes to entry 1
    g_stream_last_civ  = 0;     // dma starts at entry 0. (satoru)
    return true;
}

uint32_t AC97::WriteRingChunk(const void* data, uint32_t bytes) {
    if (!g_stream_live || !data || bytes == 0) return 0;
    const uint8_t* src = (const uint8_t*)data;
    uint32_t written = 0;

    int civ = BMRead8(AC97_BM_PCM_OUT + BM_CIV);

    // zero every entry the dma engine has *already played* since our last
    // refill (g_stream_last_civ .. civ-1).  the controller cycles the ring, so
    // any consumed-but-unrefilled entry would be replayed verbatim on the next
    // wrap  -  an audible pop/click of stale period data on every underrun. blank
    // them to silence so an underrun is clean, not a crackle.  walk forward mod
    // N from last_civ up to (not including) civ; bounded to N iterations so a
    // bogus civ can never spin. N = AC97_MAX_BDL_ENTRIES = 32. (satoru)
    for (int idx = g_stream_last_civ, guard = 0;
         idx != civ && guard < AC97_MAX_BDL_ENTRIES;
         idx = (idx + 1) % AC97_MAX_BDL_ENTRIES, guard++) {
        uint8_t* z = dma_buffer + idx * kAC97RingChunkBytes;
        for (int i = 0; i < kAC97RingChunkBytes; i++) z[i] = 0;
    }
    g_stream_last_civ = civ;

    // recovery after an underrun: if our write cursor sits on civ the ring
    // *looks* full, but if the engine has halted (DCH) it actually drained dry
    // and the indices merely collided because we stopped feeding it.  bailing
    // here forever is the old "plays but stays silent" lock-up.  re-sync the
    // fill cursor one slot ahead of civ so we resume writing fresh data instead
    // of giving up.  (when DCH is clear, next_fill==civ is a genuinely full
    // ring and the per-iteration guard below still backs off correctly.) (satoru)
    if (g_stream_next_fill == civ) {
        uint16_t st = BMRead16(AC97_BM_PCM_OUT + BM_STATUS);
        if (st & BM_STATUS_DCH) {
            g_stream_next_fill = (civ + 1) % AC97_MAX_BDL_ENTRIES;
        }
    }

    while (bytes > 0) {
        // Don't overwrite the entry the controller is currently playing.
        if (g_stream_next_fill == civ) {
            // Ring is full from the controller's perspective; bail and
            // let the caller back off until the next chunk frees up.
            break;
        }
        uint8_t* dst = dma_buffer + g_stream_next_fill * kAC97RingChunkBytes;
        uint32_t to_copy = bytes < (uint32_t)kAC97RingChunkBytes ? bytes
                                                                 : (uint32_t)kAC97RingChunkBytes;
        for (uint32_t i = 0; i < to_copy; i++) dst[i] = src[i];
        for (uint32_t i = to_copy; i < (uint32_t)kAC97RingChunkBytes; i++) dst[i] = 0;
        bdl[g_stream_next_fill].length = kAC97RingChunkSamples;
        bdl[g_stream_next_fill].flags  = AC97_BD_IOC;

        int filled_idx = g_stream_next_fill;
        g_stream_next_fill = (g_stream_next_fill + 1) % AC97_MAX_BDL_ENTRIES;
        g_stream_queued_bytes += to_copy;

        // Advance LVI to the entry we just filled so the controller will
        // play it.  LVI is the *last valid index* the controller may
        // dispatch  -  the entry one before next_fill works for a 32-entry
        // ring where civ != next_fill.
        BMWrite8(AC97_BM_PCM_OUT + BM_LVI, (uint8_t)filled_idx);

        src += to_copy;
        bytes -= to_copy;
        written += to_copy;
    }

    // Clear completion interrupt bits so the controller keeps emitting them.
    uint16_t status = BMRead16(AC97_BM_PCM_OUT + BM_STATUS);
    BMWrite16(AC97_BM_PCM_OUT + BM_STATUS,
              status & (BM_STATUS_LVBCI | BM_STATUS_BCIS | BM_STATUS_FIFOE));

    // If the engine stalled (DCH) but we just gave it data, resume it WITHOUT
    // calling StartDMA(). StartDMA() re-arms LVI to the last bdl entry
    // (AC97_MAX_BDL_ENTRIES-1); in streaming mode that makes RingQueuedBytes
    // report an almost-full ring forever, which wedges the mixer's
    // back-pressure gate (QueuedFrames > 3*PERIOD) permanently shut  -  so after
    // the very first underrun no further audio is ever mixed. that is the
    // "video/music plays but stays silent" bug. the bdl base is unchanged and
    // we already advanced LVI to the entry we just filled, so just re-arm the
    // run bit to resume from the current index. (satoru)
    if (status & BM_STATUS_DCH) {
        BMWrite8(AC97_BM_PCM_OUT + BM_CR,
                 BM_CR_RUN | BM_CR_IOCE | BM_CR_LVBIE);
    }

    return written;
}

uint32_t AC97::RingQueuedBytes() {
    if (!g_stream_live) return 0;
    int civ = BMRead8(AC97_BM_PCM_OUT + BM_CIV);
    int lvi = BMRead8(AC97_BM_PCM_OUT + BM_LVI);
    // in-flight entries = the entries the controller still has to play, i.e.
    // civ up to and including lvi.  that count is (lvi+1-civ) mod N, NOT
    // (lvi-civ) mod N.  the old (lvi-civ+N)%N was off by one and, worse, it
    // BLEW UP after an underrun: once civ wrapped one slot past a stale lvi
    // (e.g. lvi=8, civ=9) the old math returned (8-9+32)%32 = 31  -  a nearly
    // full ring (31*4096 = 124 kb)  -  so the mixer's back-pressure gate
    // (QueuedFrames > PERIOD*8) latched shut forever and never refilled again,
    // i.e. audio went silent/crackly and never recovered. with (lvi+1-civ+N)%N
    // the same case returns (8+1-9+32)%32 = 0 (empty) so the mixer resumes
    // refilling. N = AC97_MAX_BDL_ENTRIES = 32, chunk = 4096 bytes. (satoru)
    int dist = (lvi + 1 - civ + AC97_MAX_BDL_ENTRIES) % AC97_MAX_BDL_ENTRIES;
    return (uint32_t)dist * (uint32_t)kAC97RingChunkBytes;
}

uint32_t AC97::RingChunkBytes() { return kAC97RingChunkBytes; }

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

    // Streaming mode: the AudioMixer owns ring refills.  We just clear
    // the W1C bits so the controller keeps interrupting.
    if (!g_stream_live) {
        if (status & BM_STATUS_BCIS) {
            int civ = BMRead8(AC97_BM_PCM_OUT + BM_CIV);
            int next = (civ + 1) % AC97_MAX_BDL_ENTRIES;
            FillBuffer(next);
            BMWrite8(AC97_BM_PCM_OUT + BM_LVI,
                     (uint8_t)((civ + AC97_MAX_BDL_ENTRIES - 1) % AC97_MAX_BDL_ENTRIES));
        }
        if (status & BM_STATUS_LVBCI) {
            if (pcm_offset >= pcm_length && !looping) {
                info.state = AC97_STOPPED;
            }
        }
    }

    // W1C: writing 1 to bits 1..4 (CELV, LVBCI, BCIS, FIFOE) clears them.
    BMWrite16(AC97_BM_PCM_OUT + BM_STATUS,
              status & (BM_STATUS_CELV | BM_STATUS_LVBCI |
                        BM_STATUS_BCIS | BM_STATUS_FIFOE));
}

//  tick  -  poll-based buffer refill
void AC97::Tick() {
    if (info.state != AC97_PLAYING) return;

    uint16_t status = BMRead16(AC97_BM_PCM_OUT + BM_STATUS);
    if (status & BM_STATUS_BCIS) {
        HandleIRQ();
    }

    if (status & BM_STATUS_DCH) {
        if (g_stream_live) {
            // Streaming mode: the engine stalled because we under-fed.
            // Don't reset  -  re-kick once new chunks land.  Clear DCH ack.
            BMWrite16(AC97_BM_PCM_OUT + BM_STATUS,
                      status & (BM_STATUS_LVBCI | BM_STATUS_BCIS | BM_STATUS_FIFOE));
        } else if (pcm_offset < pcm_length) {
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
