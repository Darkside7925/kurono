//  kurono os  -  Sound Blaster 16 backend (auto-init DMA, double-buffered)
//
//  This is an AudioBackend implementation that talks to the SB16 ISA
//  device QEMU emulates (and real SB16/SBPro/AWE32 cards too).  Unlike
//  the legacy Audio:: driver it uses *auto-init* DMA on a single 32 KB
//  buffer split into two 16 KB halves  -  the mixer keeps the inactive
//  half full while the DSP plays the active half, then swaps.
//
//  Layout:
//    * DSP base port: configurable (0x220 default).  Probed via
//      successive DSP resets at 0x220, 0x240, 0x260, 0x280.
//    * IRQ:           polled from the bus  -  we never enable the IRQ.
//                     The mixer's Tick() drains via Tick() here.
//    * DMA channel 1 (8-bit) for sample rates the SB16 calls "low".
//      For our internal stereo 16-bit @ 48 kHz path we use 16-bit
//      DMA on channel 5.
//
//  This file does NOT replace src/drivers/audio.cpp; the legacy class
//  still exists for the Audio::Beep() / Audio::Play() callers that
//  haven't been migrated yet.  Eventually those will all forward to
//  AudioServer and this backend.

#include "audio_backend.h"
#include "audio_dma.h"
#include "audio_mixer.h"
#include "serial.h"
#include "../kernel/types.h"

namespace {

// ---- ISA port I/O ----
static inline void out8(uint16_t port, uint8_t v) {
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

// ---- SB16 register offsets relative to the base port ----
constexpr uint16_t kDspReset      = 0x6;
constexpr uint16_t kDspRead       = 0xA;
constexpr uint16_t kDspWrite      = 0xC;
constexpr uint16_t kDspReadStatus = 0xE;
constexpr uint16_t kDspIRQ16Ack   = 0xF;
constexpr uint16_t kMixerAddr     = 0x4;
constexpr uint16_t kMixerData     = 0x5;

// DMA controller (16-bit half: ch 5/6/7) -- master DMAC2 ports
constexpr uint16_t kDma2Mask     = 0xD4;
constexpr uint16_t kDma2Mode     = 0xD6;
constexpr uint16_t kDma2Clear    = 0xD8;
constexpr uint16_t kDma2Addr5    = 0xC4;   // ch5 base address (word port)
constexpr uint16_t kDma2Count5   = 0xC6;   // ch5 word count
constexpr uint16_t kDma2Page5    = 0x8B;   // ch5 page register

constexpr uint16_t kProbePorts[] = {0x220, 0x240, 0x260, 0x280};
constexpr uint8_t  kIrqLine      = 5;       // QEMU default
constexpr uint8_t  kDmaCh16      = 5;       // QEMU default 16-bit channel

class SB16Backend final : public AudioBackend {
public:
    SB16Backend() = default;

    const char* Name() const override { return "sb16"; }

    bool Init() override {
        // Probe each candidate base port by attempting a DSP reset.
        for (uint16_t base : kProbePorts) {
            if (ResetDSP(base)) {
                base_port_ = base;
                break;
            }
        }
        if (base_port_ == 0) {
            SerialLogger::Log("[SB16] No DSP found at any probe port\r\n");
            return false;
        }

        // Read DSP version.  SB16 returns 4.xx; SB Pro 3.xx; SB 2.0 2.xx.
        WriteDSP(0xE1);
        uint8_t maj = ReadDSP();
        uint8_t min = ReadDSP();
        dsp_version_ = (maj << 8) | min;
        SerialLogger::Log("[SB16] DSP detected at 0x");
        SerialLogger::LogHex(base_port_);
        SerialLogger::Log(" version ");
        SerialLogger::LogDec(maj);
        SerialLogger::Log(".");
        SerialLogger::LogDec(min);
        SerialLogger::Log("\r\n");

        if (maj < 4) {
            SerialLogger::Log("[SB16] DSP < 4.x, refusing (need SB16 for stereo 16-bit)\r\n");
            return false;
        }

        // Acquire DMA buffer.  Two 16 KB halves.
        void* p = AudioDMA::Acquire(AudioDMA::REGION_SB16_PRIMARY, "sb16-backend");
        if (!p) return false;
        dma_buf_phys_ = (uint32_t)(uintptr_t)p;
        dma_buf_      = (uint8_t*)p;
        for (int i = 0; i < kBufBytes; i++) dma_buf_[i] = 0;

        // Program mixer: IRQ select (reg 0x80), DMA select (reg 0x81),
        // master + voice volumes.
        MixerWrite(0x80, 1 << ((kIrqLine == 2) ? 0 :
                              (kIrqLine == 5) ? 1 :
                              (kIrqLine == 7) ? 2 : 3));
        MixerWrite(0x81, 1 << kDmaCh16);             // 16-bit DMA channel
        MixerWrite(0x22, 0xFF);                      // master L/R = max
        MixerWrite(0x32, 0xFF);                      // SB16 master L
        MixerWrite(0x33, 0xFF);                      // SB16 master R
        MixerWrite(0x30, 0xFF);                      // voice L
        MixerWrite(0x31, 0xFF);                      // voice R

        // Set sample rate (DSP 4.x command 0x41/0x42 take a big-endian
        // 16-bit rate following the command).
        WriteDSP(0x41);
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE >> 8));
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE & 0xFF));
        WriteDSP(0x42);
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE >> 8));
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE & 0xFF));

        // Program 16-bit DMA channel 5 for the full 32 KB auto-init buffer.
        ProgramDMA16(dma_buf_phys_, kBufBytes, /*auto_init=*/true);

        // Issue 16-bit auto-init programmed-transfer command.
        // 0xB6 = 16-bit, signed, A/I, output, FIFO on
        // mode byte: stereo (0x20) + signed (0x10)
        // count = (samples / channel) - 1 measured in *samples* (16-bit)
        uint16_t samples = (kBufBytes / 2) - 1;       // 16-bit sample count
        WriteDSP(0xB6);
        WriteDSP(0x30);                               // stereo signed
        WriteDSP((uint8_t)(samples & 0xFF));
        WriteDSP((uint8_t)(samples >> 8));

        playing_ = true;
        active_half_ = 0;
        ready_ = true;
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        // We store stereo 16-bit, so frames count = samples / 2.
        // Each half holds 4096 frames (8192 samples = 16 KB).
        constexpr uint32_t kFramesPerHalf = (kBufBytes / 2) / 2;  // 4096

        // Wait for the inactive half to actually be inactive  -  i.e. for
        // the DSP's current position to be in the *other* half.  We
        // poll the DMA controller's current count register to figure
        // out which half is being played.
        // Inactive half is the one *not* being played; we fill it.
        if (frames > kFramesPerHalf) frames = kFramesPerHalf;

        // Wait for the swap.  Each call to Submit fills exactly one half.
        WaitForSwap();

        uint8_t* dst = dma_buf_ + (next_fill_half_ * (kBufBytes / 2));
        const uint8_t* src = (const uint8_t*)pcm;
        const uint32_t bytes = frames * 4;            // stereo s16
        for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];
        // zero-pad the rest of the half if we got fewer than expected
        for (uint32_t i = bytes; i < (kBufBytes / 2); i++) dst[i] = 0;

        next_fill_half_ ^= 1;
        queued_frames_ += frames;
        return frames;
    }

    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override {
        if (!ready_) return;
        WriteDSP(0xD9);              // exit 16-bit auto-init
        WriteDSP(0xD5);              // pause output
        playing_ = false;
    }

    void SetMasterVolume(int v) override {
        if (!ready_) return;
        if (v < 0) v = 0; if (v > 100) v = 100;
        master_vol_ = v;
        // SB16 master vol is 5-bit per channel in reg 0x30/0x31:
        uint8_t enc = (uint8_t)((v * 31) / 100) << 3;
        MixerWrite(0x30, enc);
        MixerWrite(0x31, enc);
    }
    int GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        // No IRQ wired  -  the mixer pulls Submit() based on QueuedFrames.
        // Decay queued count based on elapsed time / DMA progress.  The
        // DMA controller's current-count register tells us how many
        // *bytes* remain in the current 32 KB transfer.
        UpdateQueueEstimate();
    }

private:
    static constexpr int kBufBytes = 32 * 1024;     // 32 KB total (two 16 KB halves)

    uint16_t base_port_     = 0;
    uint16_t dsp_version_   = 0;
    uint8_t* dma_buf_       = nullptr;
    uint32_t dma_buf_phys_  = 0;
    bool     ready_         = false;
    bool     playing_       = false;
    int      master_vol_    = 80;
    int      active_half_   = 0;          // which half DSP is currently playing
    int      next_fill_half_= 0;          // which half Submit() should fill next
    uint32_t queued_frames_ = 0;

    bool ResetDSP(uint16_t base) {
        out8(base + kDspReset, 1);
        for (volatile int i = 0; i < 1000; i++) {}
        out8(base + kDspReset, 0);
        for (int t = 0; t < 1000; t++) {
            if ((in8(base + kDspReadStatus) & 0x80)) {
                if (in8(base + kDspRead) == 0xAA) return true;
            }
            for (volatile int d = 0; d < 100; d++) {}
        }
        return false;
    }
    void WriteDSP(uint8_t v) {
        for (int t = 0; t < 10000; t++) {
            if ((in8(base_port_ + kDspWrite) & 0x80) == 0) {
                out8(base_port_ + kDspWrite, v);
                return;
            }
        }
    }
    uint8_t ReadDSP() {
        for (int t = 0; t < 10000; t++) {
            if (in8(base_port_ + kDspReadStatus) & 0x80) {
                return in8(base_port_ + kDspRead);
            }
        }
        return 0;
    }
    void MixerWrite(uint8_t reg, uint8_t v) {
        out8(base_port_ + kMixerAddr, reg);
        out8(base_port_ + kMixerData, v);
    }

    void ProgramDMA16(uint32_t phys, uint32_t len, bool auto_init) {
        AudioDMA::Dma16Layout l = AudioDMA::SplitForDMA16(phys, len);
        if (!l.valid) {
            SerialLogger::Log("[SB16] DMA layout invalid for buffer\r\n");
            return;
        }
        // Disable channel 5 (channel & 3 = 1, plus 0x04 mask bit)
        out8(kDma2Mask, 0x04 | (kDmaCh16 & 0x03));
        // Reset flip-flop
        out8(kDma2Clear, 0x00);
        // Mode: single (0x40) + auto-init (0x10) + read (0x08) + ch 1
        // For 16-bit master DMAC2, channels 5..7 map to (ch & 3) = 1..3.
        uint8_t mode = (auto_init ? 0x58 : 0x48) | (kDmaCh16 & 0x03);
        out8(kDma2Mode, mode);
        // Page register (high byte of 24-bit physical address >> 16)
        out8(kDma2Page5, l.page);
        // Word offset (low 16 bits of the 17-bit word address)
        out8(kDma2Addr5, (uint8_t)(l.word_offset & 0xFF));
        out8(kDma2Addr5, (uint8_t)(l.word_offset >> 8));
        // Word count (samples - 1)
        out8(kDma2Count5, (uint8_t)(l.word_count & 0xFF));
        out8(kDma2Count5, (uint8_t)(l.word_count >> 8));
        // Unmask
        out8(kDma2Mask, kDmaCh16 & 0x03);
    }

    void WaitForSwap() {
        // Poll the DMA channel 5 current word counter.  When the
        // counter falls below half the buffer, the DSP is playing the
        // second half, so we should fill the first.  When it rises back
        // above half (auto-init wraps), it's playing the first half so
        // we fill the second.
        for (int spin = 0; spin < 200000; spin++) {
            // Reset flip-flop before reading word count
            out8(kDma2Clear, 0x00);
            uint8_t lo = in8(kDma2Count5);
            uint8_t hi = in8(kDma2Count5);
            uint16_t remaining_words = (uint16_t)lo | ((uint16_t)hi << 8);
            // total buffer is kBufBytes/2 words
            uint16_t total_words = (kBufBytes / 2);
            int playing_half = (remaining_words >= total_words / 2) ? 0 : 1;
            if (playing_half != next_fill_half_) {
                active_half_ = playing_half;
                return;
            }
            for (volatile int d = 0; d < 50; d++) {}
        }
    }

    void UpdateQueueEstimate() {
        // The hardware has at most one full half pending = kFramesPerHalf
        // frames of latency.  Reflect that in queued_frames_ for the
        // mixer's pacing decisions.
        constexpr uint32_t kFramesPerHalf = (kBufBytes / 2) / 2;
        if (queued_frames_ > kFramesPerHalf) {
            queued_frames_ = kFramesPerHalf;
        }
    }
};

static SB16Backend g_sb16;

// Static registration: runs before AudioServer::Init() because the
// constructor of __register_sb16 fires during C++ static init.
struct __register_sb16 {
    __register_sb16() { AudioServer::RegisterBackend(&g_sb16); }
};
static __register_sb16 _reg_sb16_;

} // namespace
