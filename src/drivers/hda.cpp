//  kurono os  -  intel hd audio (hda) controller driver implementation
//  pci class 04:03:00 (multimedia audio controller)
#include "hda.h"
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"

bool HDAudio::detected = false;
volatile uint8_t* HDAudio::bar0 = nullptr;

uint32_t* HDAudio::corb = nullptr;
uint64_t* HDAudio::rirb = nullptr;
int HDAudio::corb_size = 0;
int HDAudio::rirb_size = 0;
int HDAudio::rirb_rp = 0;

int HDAudio::codec_count = 0;
uint32_t HDAudio::codec_vendors[HDA_MAX_CODECS] = {};
int HDAudio::output_nid = -1;
int HDAudio::pin_nid = -1;
int HDAudio::codec_addr = 0;

HDA_BDL_Entry* HDAudio::bdl = nullptr;
void* HDAudio::dma_buffer = nullptr;
bool HDAudio::playing = false;
uint8_t HDAudio::volume = 200;
HDAStreamFormat HDAudio::current_format = {48000, 16, 2};
uint32_t HDAudio::stream_base = 0;

// streaming state  -  independent of the legacy one-shot Play() path.
static bool     g_hda_stream_live  = false;
static uint32_t g_hda_write_ptr    = 0;     // byte offset into cyclic buffer
static uint32_t g_hda_last_lpib    = 0;     // last observed read position

uint8_t  HDAudio::Read8(uint32_t offset)  { return *(volatile uint8_t*)(bar0 + offset); }
uint16_t HDAudio::Read16(uint32_t offset) { return *(volatile uint16_t*)(bar0 + offset); }
uint32_t HDAudio::Read32(uint32_t offset) { return *(volatile uint32_t*)(bar0 + offset); }
void HDAudio::Write8(uint32_t offset, uint8_t val)   { *(volatile uint8_t*)(bar0 + offset) = val; }
void HDAudio::Write16(uint32_t offset, uint16_t val) { *(volatile uint16_t*)(bar0 + offset) = val; }
void HDAudio::Write32(uint32_t offset, uint32_t val) { *(volatile uint32_t*)(bar0 + offset) = val; }

bool HDAudio::Init() {
    detected = false;
    codec_count = 0;
    output_nid = -1;
    pin_nid = -1;

    // scan pci for intel hda controller (class 04:03:xx)
    uint32_t found_bar0 = 0;
    uint8_t found_bus = 0, found_dev = 0, found_func = 0;
    bool found = false;

    for (int bus = 0; bus < 256 && !found; bus++) {
        for (int dev = 0; dev < 32 && !found; dev++) {
            for (int func = 0; func < 8 && !found; func++) {
                uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);

                outl(0xCF8, addr | 0x00);
                uint32_t vendor_device = inl(0xCFC);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                outl(0xCF8, addr | 0x08);
                uint32_t class_reg = inl(0xCFC);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class = (class_reg >> 16) & 0xFF;

                // hda: class=04, subclass=03
                if (base_class == 0x04 && sub_class == 0x03) {
                    outl(0xCF8, addr | 0x10);
                    found_bar0 = inl(0xCFC) & ~0xF;
                    found_bus = bus;
                    found_dev = dev;
                    found_func = func;
                    found = true;
                }
            }
        }
    }

    if (!found) return false;

    bar0 = (volatile uint8_t*)(uintptr_t)found_bar0;

    // enable bus mastering and memory space
    uint32_t cmd_addr = (1 << 31) | (found_bus << 16) | (found_dev << 11) | (found_func << 8) | 0x04;
    outl(0xCF8, cmd_addr);
    uint32_t pci_cmd = inl(0xCFC);
    pci_cmd |= (1 << 1) | (1 << 2); // memory space + bus master
    outl(0xCF8, cmd_addr);
    outl(0xCFC, pci_cmd);

    // reset controller
    Write32(HDA_GCTL, 0); // de-assert crst
    for (int i = 0; i < 100000; i++) {
        if (!(Read32(HDA_GCTL) & HDA_GCTL_CRST)) break;
        for (volatile int d = 0; d < 1000; d++);
    }

    // wait a bit for codec detection
    for (volatile int d = 0; d < 500000; d++);

    // assert crst to bring controller out of reset
    Write32(HDA_GCTL, HDA_GCTL_CRST);
    for (int i = 0; i < 100000; i++) {
        if (Read32(HDA_GCTL) & HDA_GCTL_CRST) break;
        for (volatile int d = 0; d < 1000; d++);
    }

    // wait for codecs to enumerate
    for (volatile int d = 0; d < 500000; d++);

    // read capabilities
    uint16_t gcap = Read16(HDA_GCAP);
    int num_oss = (gcap >> 12) & 0xF;  // number of output streams
    int num_iss = (gcap >> 8) & 0xF;   // number of input streams
    (void)num_iss;

    if (num_oss == 0) return false;

    // calculate first output stream descriptor base
    // input streams come first, then output streams
    // each stream descriptor is 0x20 bytes, starting at offset 0x80
    stream_base = 0x80 + num_iss * 0x20;

    // init corb/rirb
    if (!InitCorbRirb()) return false;

    // probe codecs
    if (!ProbeCodecs()) return false;

    // find output path in first codec
    for (int c = 0; c < codec_count; c++) {
        if (FindOutputPath(c)) {
            codec_addr = c;
            break;
        }
    }

    // allocate bdl and dma buffer
    bdl = (HDA_BDL_Entry*)KernelHeap::Alloc(4096);
    if (!bdl) return false;

    // allocate dma buffer (multiple pages)
    int pages_needed = (HDA_BUFFER_SIZE + 4095) / 4096;
    dma_buffer = (void*)KernelHeap::Alloc(pages_needed * 4096);
    if (!dma_buffer) return false;

    // set up bdl entries
    uint8_t* buf_ptr = (uint8_t*)dma_buffer;
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        bdl[i].address = (uint64_t)(uintptr_t)(buf_ptr + i * 4096);
        bdl[i].length = 4096;
        bdl[i].ioc = (i == HDA_BDL_ENTRIES - 1) ? 1 : 0;
    }

    // set up output stream
    SetupOutputStream();

    detected = true;
    return true;
}

bool HDAudio::InitCorbRirb() {
    // determine corb size
    uint8_t corbsz = Read8(HDA_CORBSIZE);
    if (corbsz & 0x40) { corb_size = 256; Write8(HDA_CORBSIZE, 0x02); }
    else if (corbsz & 0x20) { corb_size = 16; Write8(HDA_CORBSIZE, 0x01); }
    else { corb_size = 2; Write8(HDA_CORBSIZE, 0x00); }

    // determine rirb size
    uint8_t rirbsz = Read8(HDA_RIRBSIZE);
    if (rirbsz & 0x40) { rirb_size = 256; Write8(HDA_RIRBSIZE, 0x02); }
    else if (rirbsz & 0x20) { rirb_size = 16; Write8(HDA_RIRBSIZE, 0x01); }
    else { rirb_size = 2; Write8(HDA_RIRBSIZE, 0x00); }

    // allocate corb (array of uint32_t verbs)
    corb = (uint32_t*)KernelHeap::Alloc(4096);
    if (!corb) return false;
    for (int i = 0; i < corb_size; i++) corb[i] = 0;

    // allocate rirb (array of {uint32_t response, uint32_t response_ex})
    rirb = (uint64_t*)KernelHeap::Alloc(4096);
    if (!rirb) return false;
    for (int i = 0; i < rirb_size; i++) rirb[i] = 0;

    // stop corb/rirb
    Write8(HDA_CORBCTL, 0);
    Write8(HDA_RIRBCTL, 0);
    for (volatile int d = 0; d < 10000; d++);

    // set corb base address  -  full 64-bit phys (identity-mapped: phys==virt),
    // upper half must not be hardcoded to 0 or a heap address >4 gb would be
    // truncated and the controller would dma the wrong page. (satoru)
    uint64_t corb_phys = (uint64_t)(uintptr_t)corb;
    Write32(HDA_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    Write32(HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));

    // reset corb read pointer
    Write16(HDA_CORBRP, (1 << 15));
    for (int i = 0; i < 10000; i++) {
        if (Read16(HDA_CORBRP) & (1 << 15)) break;
        for (volatile int d = 0; d < 1000; d++);
    }
    Write16(HDA_CORBRP, 0);
    for (int i = 0; i < 10000; i++) {
        if (!(Read16(HDA_CORBRP) & (1 << 15))) break;
        for (volatile int d = 0; d < 1000; d++);
    }

    // reset corb write pointer
    Write16(HDA_CORBWP, 0);

    // set rirb base address  -  full 64-bit phys, same upper-half fix as corb
    // above so a >4 gb heap allocation is addressed correctly. (satoru)
    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb;
    Write32(HDA_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    Write32(HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // reset rirb write pointer
    Write16(HDA_RIRBWP, (1 << 15));
    rirb_rp = 0;

    // set rintcnt
    Write16(HDA_RINTCNT, 1);

    // start corb and rirb
    Write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    Write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN | HDA_RIRBCTL_INT);

    return true;
}

bool HDAudio::SendVerb(uint32_t verb, uint32_t* response) {
    // get current write pointer
    uint16_t wp = Read16(HDA_CORBWP) & 0xFF;
    wp = (wp + 1) % corb_size;

    corb[wp] = verb;
    Write16(HDA_CORBWP, wp);

    return WaitRIRB(response, 5000);
}

bool HDAudio::WaitRIRB(uint32_t* response, int timeout) {
    for (int i = 0; i < timeout * 100; i++) {
        uint16_t wp = Read16(HDA_RIRBWP) & 0xFF;

        if (rirb_rp != wp) {
            rirb_rp = (rirb_rp + 1) % rirb_size;
            uint64_t entry = rirb[rirb_rp];
            if (response) *response = (uint32_t)(entry & 0xFFFFFFFF);
            return true;
        }
        for (volatile int d = 0; d < 100; d++);
    }
    return false;
}

bool HDAudio::ProbeCodecs() {
    codec_count = 0;
    uint16_t statests = Read16(HDA_STATESTS);

    for (int cad = 0; cad < HDA_MAX_CODECS && codec_count < HDA_MAX_CODECS; cad++) {
        if (!(statests & (1 << cad))) continue;

        // get vendor/device id
        uint32_t verb = HDA_VERB(cad, 0, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
        uint32_t resp = 0;
        if (SendVerb(verb, &resp)) {
            codec_vendors[codec_count] = resp;
            codec_count++;
        }
    }

    return codec_count > 0;
}

bool HDAudio::FindOutputPath(int cad) {
    // get afg (audio function group) node count
    uint32_t resp = 0;
    SendVerb(HDA_VERB(cad, 1, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT), &resp);
    int start_nid = (resp >> 16) & 0xFF;
    int num_nodes = resp & 0xFF;

    int found_dac = -1;
    int found_pin = -1;

    // scan nodes for output dac and output pin
    for (int n = 0; n < num_nodes && n < HDA_MAX_NODES; n++) {
        int nid = start_nid + n;

        // get audio widget capabilities
        uint32_t wcap = 0;
        SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET), &wcap);

        uint8_t wtype = (wcap >> 20) & 0xF;

        if (wtype == HDA_WIDGET_AUDIO_OUT && found_dac < 0) {
            found_dac = nid;
        }
        if (wtype == HDA_WIDGET_PIN && found_pin < 0) {
            // check if it's an output pin (check pin capabilities)
            uint32_t pincap = 0;
            SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP), &pincap);
            if (pincap & (1 << 4)) { // output capable
                found_pin = nid;
            }
        }
    }

    if (found_dac >= 0 && found_pin >= 0) {
        output_nid = found_dac;
        pin_nid = found_pin;

        // power on the dac
        SendVerb(HDA_VERB(cad, found_dac, HDA_VERB_SET_POWER, 0x00), &resp);

        // enable pin output
        SendVerb(HDA_VERB(cad, found_pin, HDA_VERB_SET_PINCTL, 0x40), &resp); // out_en

        // set output amplifier gain
        uint32_t amp_verb = ((uint32_t)HDA_VERB_SET_AMP_GAIN << 8) |
                            (1 << 15) | (1 << 13) | (1 << 12) | volume; // set output, left+right
        SendVerb(HDA_VERB(cad, found_dac, 0x3, amp_verb & 0xFFFF), &resp);

        return true;
    }

    return false;
}

uint16_t HDAudio::EncodeFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    uint16_t fmt = 0;

    // sample rate base and multiplier
    switch (sample_rate) {
        case 8000:  fmt |= (0x05 << 8); break; // 48000 / 6
        case 11025: fmt |= (0x03 << 8) | (1 << 14); break; // 44100 / 4
        case 16000: fmt |= (0x02 << 8); break; // 48000 / 3
        case 22050: fmt |= (0x01 << 8) | (1 << 14); break; // 44100 / 2
        case 32000: fmt |= (0x03 << 11) | (0x02 << 8); break; // 48000 * 2/3
        case 44100: fmt |= (1 << 14); break; // 44100 base
        case 48000: fmt |= 0; break; // 48000 base
        case 88200: fmt |= (1 << 14) | (1 << 11); break; // 44100 * 2
        case 96000: fmt |= (1 << 11); break; // 48000 * 2
        case 176400: fmt |= (1 << 14) | (3 << 11); break; // 44100 * 4
        case 192000: fmt |= (3 << 11); break; // 48000 * 4
        default:    fmt |= 0; break; // default 48000
    }

    // bits per sample
    switch (bits) {
        case 8:  fmt |= (0 << 4); break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break; // default 16-bit
    }

    // channels (0 = 1ch, 1 = 2ch, ...)
    fmt |= (channels - 1) & 0xF;

    return fmt;
}

bool HDAudio::SetupOutputStream() {
    if (!bdl || !dma_buffer) return false;

    // reset stream
    Write8(stream_base + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 10000; i++) {
        if (Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        for (volatile int d = 0; d < 100; d++);
    }

    // clear reset
    Write8(stream_base + HDA_SD_CTL, 0);
    for (int i = 0; i < 10000; i++) {
        if (!(Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        for (volatile int d = 0; d < 100; d++);
    }

    // set stream format
    uint16_t fmt = EncodeFormat(current_format.sample_rate, current_format.bits, current_format.channels);
    Write16(stream_base + HDA_SD_FMT, fmt);

    // set bdl address  -  program the full 64-bit physical base, low half in
    // sdbdpl and high half in sdbdpu.  the kernel is identity-mapped (phys ==
    // virt, see vmm.cpp) so the bdl's physical address is just its pointer.
    // we cannot borrow audiodma::acquire here: those four low-memory regions
    // are statically owned by sb16/ac97 and are far too small for hda's 128 kb
    // cyclic buffer + bdl.  hardcoding sdbdpu = 0 (the old code) truncated any
    // heap address >4 gb and would corrupt dma on real 64-bit machines; the
    // bdl entry addresses themselves are already full 64-bit. (satoru)
    uint64_t bdl_phys = (uint64_t)(uintptr_t)bdl;
    Write32(stream_base + HDA_SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    Write32(stream_base + HDA_SD_BDPU, (uint32_t)(bdl_phys >> 32));

    // set cyclic buffer length
    Write32(stream_base + HDA_SD_CBL, HDA_BUFFER_SIZE);

    // set last valid index
    Write16(stream_base + HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

    // set stream number (use stream 1) in ctl bits [23:20]
    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl &= ~(0xF << 20);
    ctl |= (1 << 20); // stream number 1
    Write32(stream_base + HDA_SD_CTL, ctl);

    // configure dac to use stream 1, channel 0
    if (output_nid >= 0) {
        uint32_t resp;
        SendVerb(HDA_VERB(codec_addr, output_nid, HDA_VERB_SET_CONVCTRL, (1 << 4) | 0), &resp);
        SendVerb(HDA_VERB(codec_addr, output_nid, HDA_VERB_SET_FORMAT, fmt), &resp);
    }

    return true;
}

bool HDAudio::IsDetected() { return detected; }
int HDAudio::GetCodecCount() { return codec_count; }

uint32_t HDAudio::GetCodecVendor(int codec) {
    if (codec < 0 || codec >= codec_count) return 0;
    return codec_vendors[codec];
}

bool HDAudio::SetFormat(uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    if (!detected) return false;
    if (playing) Stop();

    current_format.sample_rate = sample_rate;
    current_format.bits = bits;
    current_format.channels = channels;

    return SetupOutputStream();
}

bool HDAudio::Play(const void* pcm_data, uint32_t size) {
    if (!detected || !dma_buffer) return false;
    if (playing) Stop();

    // copy pcm data into dma buffer
    uint32_t copy_size = size;
    if (copy_size > HDA_BUFFER_SIZE) copy_size = HDA_BUFFER_SIZE;

    const uint8_t* src = (const uint8_t*)pcm_data;
    uint8_t* dst = (uint8_t*)dma_buffer;
    for (uint32_t i = 0; i < copy_size; i++) dst[i] = src[i];

    // zero-fill remainder
    for (uint32_t i = copy_size; i < HDA_BUFFER_SIZE; i++) dst[i] = 0;

    // start stream
    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    Write32(stream_base + HDA_SD_CTL, ctl);

    playing = true;
    return true;
}

bool HDAudio::Stop() {
    if (!detected) return false;

    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    Write32(stream_base + HDA_SD_CTL, ctl);

    playing = false;
    g_hda_stream_live = false;
    g_hda_write_ptr = 0;
    g_hda_last_lpib = 0;
    return true;
}

bool HDAudio::StartStream() {
    if (!detected || !dma_buffer) return false;
    if (g_hda_stream_live) return true;

    // Zero the cyclic buffer so the engine plays silence until the mixer
    // catches up.
    uint8_t* b = (uint8_t*)dma_buffer;
    for (uint32_t i = 0; i < HDA_BUFFER_SIZE; i++) b[i] = 0;
    g_hda_write_ptr = 0;
    g_hda_last_lpib = 0;

    // Make sure every BDL entry interrupts on completion so LPIB updates
    // are reliable -- the controller pre-fetches and we need accurate
    // wraparound notification.
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) bdl[i].ioc = 1;

    uint32_t ctl = Read32(stream_base + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    Write32(stream_base + HDA_SD_CTL, ctl);

    playing = true;
    g_hda_stream_live = true;
    return true;
}

uint32_t HDAudio::WriteRing(const void* data, uint32_t bytes) {
    if (!g_hda_stream_live || !data || bytes == 0) return 0;
    uint32_t lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    g_hda_last_lpib = lpib;

    // Available bytes ahead of the read pointer (leave a 1-frame guard).
    uint32_t free_bytes;
    if (g_hda_write_ptr >= lpib) {
        free_bytes = HDA_BUFFER_SIZE - (g_hda_write_ptr - lpib);
    } else {
        free_bytes = lpib - g_hda_write_ptr;
    }
    if (free_bytes < 8) return 0;
    free_bytes -= 4;        // keep one frame headroom

    uint32_t to_write = bytes < free_bytes ? bytes : free_bytes;
    const uint8_t* src = (const uint8_t*)data;
    uint8_t* dst_base = (uint8_t*)dma_buffer;

    uint32_t first = HDA_BUFFER_SIZE - g_hda_write_ptr;
    if (first > to_write) first = to_write;
    for (uint32_t i = 0; i < first; i++)
        dst_base[g_hda_write_ptr + i] = src[i];
    uint32_t second = to_write - first;
    for (uint32_t i = 0; i < second; i++)
        dst_base[i] = src[first + i];
    g_hda_write_ptr = (g_hda_write_ptr + to_write) % HDA_BUFFER_SIZE;
    return to_write;
}

uint32_t HDAudio::RingQueuedBytes() {
    if (!g_hda_stream_live) return 0;
    uint32_t lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    if (g_hda_write_ptr >= lpib) return g_hda_write_ptr - lpib;
    return HDA_BUFFER_SIZE - (lpib - g_hda_write_ptr);
}

uint32_t HDAudio::RingChunkBytes() { return 4096; }

bool HDAudio::IsPlaying() { return playing; }

void HDAudio::SetVolume(uint8_t vol) {
    volume = vol;
    if (detected && output_nid >= 0) {
        // update amplifier gain
        uint32_t resp;
        // output amp, left+right, gain = vol/4 (max 63 steps typically)
        uint32_t gain = vol >> 2;
        if (gain > 63) gain = 63;
        uint32_t amp_val = (1 << 15) | (1 << 13) | (1 << 12) | gain;
        SendVerb(((uint32_t)codec_addr << 28) | ((uint32_t)output_nid << 20) | (0x3 << 8) | (amp_val & 0xFF), &resp);
    }
}

uint8_t HDAudio::GetVolume() { return volume; }

uint32_t HDAudio::GetPosition() {
    if (!detected) return 0;
    return Read32(stream_base + HDA_SD_LPIB);
}

uint32_t HDAudio::GetBufferSize() { return HDA_BUFFER_SIZE; }

void HDAudio::DumpInfo(char* out, int max_len) {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12]; int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else { char rev[12]; int ri = 0; uint32_t tmp = val;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri]; }
        buf[i] = 0; append(buf);
    };
    auto append_hex = [&](uint32_t val, int digits) {
        const char* hex = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; i--)
            if (pos < max_len - 1) out[pos++] = hex[(val >> (i * 4)) & 0xF];
    };

    if (!detected) {
        append("Intel HD Audio: Not detected\n");
        out[pos] = 0; return;
    }

    append("Intel HD Audio Controller\n");
    append("  Codecs: "); append_num(codec_count); append("\n");

    for (int i = 0; i < codec_count; i++) {
        append("  Codec "); append_num(i);
        append(": Vendor=0x"); append_hex(codec_vendors[i] >> 16, 4);
        append(":0x"); append_hex(codec_vendors[i] & 0xFFFF, 4);
        append("\n");
    }

    append("  Output DAC NID: "); append_num(output_nid >= 0 ? output_nid : 0); append("\n");
    append("  Output Pin NID: "); append_num(pin_nid >= 0 ? pin_nid : 0); append("\n");
    append("  Format: "); append_num(current_format.sample_rate);
    append(" Hz, "); append_num(current_format.bits);
    append("-bit, "); append_num(current_format.channels);
    append(" ch\n");
    append("  Volume: "); append_num(volume); append("/255\n");
    append("  Playing: "); append(playing ? "Yes" : "No"); append("\n");
    append("  Buffer: "); append_num(HDA_BUFFER_SIZE); append(" bytes\n");

    out[pos] = 0;
}
