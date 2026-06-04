//  kurono os  -  Sound Blaster 16 backend (auto-init DMA, double-buffered)

#include "audio_backend.h"
#include "audio_dma.h"
#include "audio_mixer.h"
#include "serial.h"
#include "../kernel/types.h"

namespace {

static inline void out8(uint16_t port, uint8_t v) {
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

constexpr uint16_t kDspReset      = 0x6;
constexpr uint16_t kDspRead       = 0xA;
constexpr uint16_t kDspWrite      = 0xC;
constexpr uint16_t kDspReadStatus = 0xE;
constexpr uint16_t kDspIRQ16Ack   = 0xF;
constexpr uint16_t kMixerAddr     = 0x4;
constexpr uint16_t kMixerData     = 0x5;

constexpr uint16_t kDma2Mask     = 0xD4;
constexpr uint16_t kDma2Mode     = 0xD6;
constexpr uint16_t kDma2Clear    = 0xD8;
constexpr uint16_t kDma2Addr5    = 0xC4;
constexpr uint16_t kDma2Count5   = 0xC6;
constexpr uint16_t kDma2Page5    = 0x8B;

constexpr uint16_t kProbePorts[] = {0x220, 0x240, 0x260, 0x280};
constexpr uint8_t  kIrqLine      = 5;
constexpr uint8_t  kDmaCh16      = 5;

class SB16Backend final : public AudioBackend {
public:
    SB16Backend() = default;

    const char* Name() const override { return "sb16"; }

    bool Init() override {
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

        void* p = AudioDMA::Acquire(AudioDMA::REGION_SB16_PRIMARY, "sb16-backend");
        if (!p) return false;
        dma_buf_phys_ = (uint32_t)(uintptr_t)p;
        dma_buf_      = (uint8_t*)p;
        // 32 KB buffer at the start of a 128 KB DMA-16 page  -  verify.
        if (!AudioDMA::SplitForDMA16(dma_buf_phys_, kBufBytes).valid) {
            SerialLogger::Log("[SB16] DMA buffer crosses 128KB page; refusing\r\n");
            return false;
        }
        for (int i = 0; i < kBufBytes; i++) dma_buf_[i] = 0;

        MixerWrite(0x80, 1 << ((kIrqLine == 2) ? 0 :
                              (kIrqLine == 5) ? 1 :
                              (kIrqLine == 7) ? 2 : 3));
        MixerWrite(0x81, 1 << kDmaCh16);
        MixerWrite(0x22, 0xFF);
        MixerWrite(0x32, 0xFF);
        MixerWrite(0x33, 0xFF);
        MixerWrite(0x30, 0xFF);
        MixerWrite(0x31, 0xFF);

        WriteDSP(0x41);
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE >> 8));
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE & 0xFF));
        WriteDSP(0x42);
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE >> 8));
        WriteDSP((uint8_t)(AudioMixer::INTERNAL_RATE & 0xFF));

        ProgramDMA16(dma_buf_phys_, kBufBytes, /*auto_init=*/true);

        // 0xB6 = 16-bit programmed I/O, A/I, FIFO, DAC.
        // mode byte: bit 5 stereo | bit 4 signed = 0x30
        // 16-bit DSP sample count = (bytes / 2) / channels - 1 = frames - 1
        uint16_t frames = (kBufBytes / 4) - 1;
        WriteDSP(0xB6);
        WriteDSP(0x30);
        WriteDSP((uint8_t)(frames & 0xFF));
        WriteDSP((uint8_t)(frames >> 8));

        playing_ = true;
        active_half_ = 0;
        // Both halves contain silence; DSP starts playing half 0, so the
        // first Submit() should fill half 1.  Subsequent Submit()s alternate.
        next_fill_half_ = 1;
        ready_ = true;
        return true;
    }

    bool IsReady() const override { return ready_; }

    uint32_t Submit(const int16_t* pcm, uint32_t frames) override {
        if (!ready_ || !pcm) return 0;
        constexpr uint32_t kFramesPerHalf = (kBufBytes / 2) / 4;  // 4096
        constexpr uint32_t kBytesPerHalf  = kBufBytes / 2;        // 16384

        // Accumulate periods into the staging buffer for the half we'll
        // fill next.  When the half is full, wait for the DSP to swap
        // away from it and DMA-copy it into the SB16 buffer.
        const uint8_t* src = (const uint8_t*)pcm;
        uint32_t bytes_in  = frames * 4;
        uint32_t bytes_done = 0;
        while (bytes_done < bytes_in) {
            uint32_t space = kBytesPerHalf - stage_fill_;
            uint32_t to_copy = (bytes_in - bytes_done) < space
                                  ? (bytes_in - bytes_done) : space;
            for (uint32_t i = 0; i < to_copy; i++)
                stage_[stage_fill_ + i] = src[bytes_done + i];
            stage_fill_ += to_copy;
            bytes_done  += to_copy;
            if (stage_fill_ == kBytesPerHalf) FlushHalf();
        }

        // Update queue estimate from real DMA progress.
        UpdateQueueEstimate();
        return frames;
    }

    uint32_t QueuedFrames() const override { return queued_frames_; }
    uint32_t SampleRate()   const override { return AudioMixer::INTERNAL_RATE; }

    void Stop() override {
        if (!ready_) return;
        WriteDSP(0xD9);
        WriteDSP(0xD5);
        // Mask the channel so any in-flight DMA stops cleanly and the
        // buffer can be re-armed by a subsequent Init().
        out8(kDma2Mask, 0x04 | (kDmaCh16 & 0x03));
        AckIRQ();
        playing_ = false;
        ready_ = false;
        queued_frames_ = 0;
    }

    void SetMasterVolume(int v) override {
        if (!ready_) return;
        if (v < 0) v = 0; if (v > 100) v = 100;
        master_vol_ = v;
        uint8_t enc = (uint8_t)((v * 31) / 100) << 3;
        MixerWrite(0x30, enc);
        MixerWrite(0x31, enc);
    }
    int GetMasterVolume() const override { return master_vol_; }

    void Tick() override {
        UpdateQueueEstimate();
    }

private:
    static constexpr int kBufBytes = 32 * 1024;

    uint16_t base_port_     = 0;
    uint16_t dsp_version_   = 0;
    uint8_t* dma_buf_       = nullptr;
    uint32_t dma_buf_phys_  = 0;
    bool     ready_         = false;
    bool     playing_       = false;
    int      master_vol_    = 80;
    int      active_half_   = 0;
    int      next_fill_half_= 0;
    uint32_t queued_frames_ = 0;

    // Staging buffer for accumulating mixer periods (1024 frames each)
    // into a full half (4096 frames) before swapping into the DMA buffer.
    uint8_t  stage_[kBufBytes / 2] = {};
    uint32_t stage_fill_           = 0;

    void FlushHalf() {
        WaitForSwap();
        uint8_t* dst = dma_buf_ + (next_fill_half_ * (kBufBytes / 2));
        for (int i = 0; i < kBufBytes / 2; i++) dst[i] = stage_[i];
        AckIRQ();
        next_fill_half_ ^= 1;
        stage_fill_ = 0;
    }

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
    void AckIRQ() {
        // 16-bit DMA IRQ is acked by reading port 0x22F (offset 0xF from base).
        (void)in8(base_port_ + kDspIRQ16Ack);
    }

    void ProgramDMA16(uint32_t phys, uint32_t len, bool auto_init) {
        AudioDMA::Dma16Layout l = AudioDMA::SplitForDMA16(phys, len);
        if (!l.valid) {
            SerialLogger::Log("[SB16] DMA layout invalid for buffer\r\n");
            return;
        }
        out8(kDma2Mask, 0x04 | (kDmaCh16 & 0x03));
        out8(kDma2Clear, 0x00);
        // mode: single(0x40)|auto-init(0x10)|read(0x08)|ch  -> single+AI+read=0x58
        uint8_t mode = (auto_init ? 0x58 : 0x48) | (kDmaCh16 & 0x03);
        out8(kDma2Mode, mode);
        out8(kDma2Page5, l.page);
        out8(kDma2Addr5, (uint8_t)(l.word_offset & 0xFF));
        out8(kDma2Addr5, (uint8_t)(l.word_offset >> 8));
        out8(kDma2Count5, (uint8_t)(l.word_count & 0xFF));
        out8(kDma2Count5, (uint8_t)(l.word_count >> 8));
        out8(kDma2Mask, kDmaCh16 & 0x03);
    }

    // Read the channel-5 word counter atomically.  The DMA controller
    // latches the 16-bit value on the first byte read after a flip-flop
    // reset, then the second byte returns the latched high byte.
    uint16_t ReadDmaCount() {
        out8(kDma2Clear, 0x00);
        uint8_t lo = in8(kDma2Count5);
        uint8_t hi = in8(kDma2Count5);
        return (uint16_t)lo | ((uint16_t)hi << 8);
    }

    void WaitForSwap() {
        // total words in the cyclic buffer
        const uint16_t total_words = (kBufBytes / 2);
        const uint16_t half_words  = total_words / 2;

        for (int spin = 0; spin < 200000; spin++) {
            uint16_t remaining = ReadDmaCount();
            // Auto-init reload sets remaining to total_words at the start
            // and counts down.  When remaining > half_words the DSP is
            // still in the first half, so we should fill the second; and
            // vice-versa.
            int playing_half = (remaining > half_words) ? 0 : 1;
            if (playing_half != next_fill_half_) {
                active_half_ = playing_half;
                return;
            }
            for (volatile int d = 0; d < 32; d++) {}
        }
        // Fall through if we ran out of patience  -  the half is still
        // being played but we have no choice; mixer is starved.
        active_half_ = next_fill_half_ ^ 1;
    }

    void UpdateQueueEstimate() {
        // Remaining 16-bit words in the *whole* cyclic buffer; falls from
        // kBufBytes/2 - 1 toward 0 then auto-init reloads.  Convert to
        // frames-in-flight (stereo s16 -> 4 bytes/frame).
        uint16_t remaining = ReadDmaCount();
        // Words remaining -> stereo s16 frames remaining.
        uint32_t frames_remaining = (uint32_t)remaining / 2;
        // Plus whatever the next half we filled hasn't been played yet,
        // which is at most kFramesPerHalf and is implicit in the counter.
        queued_frames_ = frames_remaining;
    }
};

static SB16Backend g_sb16;

struct __register_sb16 {
    __register_sb16() { AudioServer::RegisterBackend(&g_sb16); }
};
static __register_sb16 _reg_sb16_;

} // namespace
