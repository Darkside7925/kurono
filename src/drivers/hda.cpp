//  kurono os  -  intel hd audio (hda) controller driver implementation
//  pci class 04:03:00 (multimedia audio controller)
#include "hda.h"
#include "timer.h"
#include "../kernel/pci.h"
#include "../kernel/heap.h"
#include "../kernel/io.h"

bool HDAudio::detected = false;
volatile uint8_t* HDAudio::bar0 = nullptr;

void*     HDAudio::corb_raw = nullptr;
void*     HDAudio::rirb_raw = nullptr;
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

void* HDAudio::bdl_raw = nullptr;
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

// clamp the link position (lpib)'s forward advance to at most one period (4096
// bytes) per call.  `raw` is the freshly-read SD_LPIB already taken mod buffer
// size by the caller (Read32/stream_base are private statics, so the read stays
// at the call site).  qemu's emulated lpib can lag or jump in big steps; an
// unbounded jump would manufacture huge "free space" and let WriteRing lap the
// read pointer (overwriting samples the dma is about to play = crackle), while a
// backward jump (a near-N advance mod N) would corrupt the free/queued math the
// same way.  clamping the *effective* read position to last+period is uniformly
// conservative: free space can only shrink, queued can only grow, so we never
// over-write on a bogus reading.  updates g_hda_last_lpib to the clamped value
// so successive ticks track smoothly.  period == one bdl entry == 4096 bytes;
// buffer == HDA_BUFFER_SIZE == 128 kb. (satoru)
static uint32_t HDA_ClampLpibAdvance(uint32_t raw) {
    uint32_t adv = (raw + HDA_BUFFER_SIZE - g_hda_last_lpib) % HDA_BUFFER_SIZE;
    const uint32_t kPeriod = 4096;
    uint32_t eff = (adv > kPeriod)
                 ? ((g_hda_last_lpib + kPeriod) % HDA_BUFFER_SIZE)
                 : raw;
    g_hda_last_lpib = eff;
    return eff;
}

uint8_t  HDAudio::Read8(uint32_t offset)  { return *(volatile uint8_t*)(bar0 + offset); }
uint16_t HDAudio::Read16(uint32_t offset) { return *(volatile uint16_t*)(bar0 + offset); }
uint32_t HDAudio::Read32(uint32_t offset) { return *(volatile uint32_t*)(bar0 + offset); }
void HDAudio::Write8(uint32_t offset, uint8_t val)   { *(volatile uint8_t*)(bar0 + offset) = val; }
void HDAudio::Write16(uint32_t offset, uint16_t val) { *(volatile uint16_t*)(bar0 + offset) = val; }
void HDAudio::Write32(uint32_t offset, uint32_t val) { *(volatile uint32_t*)(bar0 + offset) = val; }

// busy-wait `us` microseconds off the hda wall clock (HDA_WALCLK, offset 0x30,
// free-running at 24.0 mhz => 24 ticks per microsecond). a real, calibrated
// delay that survives -O2; the old for(volatile int) loops optimized away so
// codec power-up/settle never actually waited. if the link isn't up yet the
// wall clock reads 0 and never advances, so fall back to a coarse pit-derived
// spin (1us ~ a few thousand pause iterations is unreliable, so we lean on the
// timer for the fallback path). (satoru)
void HDAudio::DelayUs(uint32_t us) {
    if (!bar0) {
        // no mmio yet  -  approximate with a bounded volatile spin. (satoru)
        for (uint32_t i = 0; i < us; i++)
            for (volatile int d = 0; d < 30; d++) { __asm__ __volatile__("pause"); }
        return;
    }
    uint32_t start = Read32(HDA_WALCLK);
    // probe whether the wall clock is actually ticking. (satoru)
    bool ticking = false;
    for (int i = 0; i < 1000; i++) {
        if (Read32(HDA_WALCLK) != start) { ticking = true; break; }
    }
    if (!ticking) {
        // wall clock dead (link not up)  -  fall back to a coarse volatile spin
        // so we still wait *something* rather than nothing at all. (satoru)
        for (uint32_t i = 0; i < us; i++)
            for (volatile int d = 0; d < 30; d++) { __asm__ __volatile__("pause"); }
        return;
    }
    const uint32_t target = us * 24u;   // 24 ticks per microsecond. (satoru)
    while ((Read32(HDA_WALCLK) - start) < target) {
        __asm__ __volatile__("pause");
    }
}

// millisecond wait via the pit timer (accurate and irq/cadence independent
// through Timer::WaitMs). used for the longer analog/charge-pump settles. (satoru)
void HDAudio::DelayMs(uint32_t ms) {
    Timer::WaitMs(ms);
}

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

    // reset controller. note: at this point the link clock (and thus the wall
    // clock) is held in reset, so DelayUs falls back to its volatile spin; once
    // crst is asserted below the wall clock starts and DelayUs is exact. (satoru)
    Write32(HDA_GCTL, 0); // de-assert crst
    for (int i = 0; i < 100000; i++) {
        if (!(Read32(HDA_GCTL) & HDA_GCTL_CRST)) break;
        DelayUs(10);
    }

    // hold reset briefly so the codec link fully quiesces. (satoru)
    DelayUs(500);

    // assert crst to bring controller out of reset
    Write32(HDA_GCTL, HDA_GCTL_CRST);
    for (int i = 0; i < 100000; i++) {
        if (Read32(HDA_GCTL) & HDA_GCTL_CRST) break;
        DelayUs(10);
    }

    // the spec requires >=521us after crst before the codecs are addressable
    // (the bus needs 25 frames to come up); wait generously so STATESTS is
    // populated before we probe. (satoru)
    DelayUs(1000);

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

    // allocate bdl with 128-byte alignment (hda spec requirement for the buffer
    // descriptor list; KernelHeap only guarantees 16-byte so over-allocate and
    // align up). real controllers fault or mis-dma an unaligned bdl. (satoru)
    bdl_raw = KernelHeap::Alloc(4096 + 128);
    if (!bdl_raw) return false;
    bdl = (HDA_BDL_Entry*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)bdl_raw, 128);

    // allocate dma buffer (multiple pages). align the base to 128 bytes so every
    // bdl entry's sample-buffer address is 128-byte aligned as the spec requires
    // (entries sit at 4096-byte offsets, so a 128-aligned base keeps them all
    // aligned). over-allocate by 128 for the alignment slack. (satoru)
    int pages_needed = (HDA_BUFFER_SIZE + 4095) / 4096;
    void* dma_raw = (void*)KernelHeap::Alloc(pages_needed * 4096 + 128);
    if (!dma_raw) return false;
    dma_buffer = (void*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)dma_raw, 128);

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

    // allocate corb (array of uint32_t verbs), 128-byte aligned per spec.
    // KernelHeap only guarantees 16-byte alignment so over-allocate and align
    // the usable pointer up; keep corb_raw for completeness. real controllers
    // require 128-byte alignment for the corb/rirb dma rings. (satoru)
    corb_raw = KernelHeap::Alloc(4096 + 128);
    if (!corb_raw) return false;
    corb = (uint32_t*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)corb_raw, 128);
    for (int i = 0; i < corb_size; i++) corb[i] = 0;

    // allocate rirb (array of {uint32_t response, uint32_t response_ex}), also
    // 128-byte aligned. (satoru)
    rirb_raw = KernelHeap::Alloc(4096 + 128);
    if (!rirb_raw) return false;
    rirb = (uint64_t*)(uintptr_t)hda_align_up((uint64_t)(uintptr_t)rirb_raw, 128);
    for (int i = 0; i < rirb_size; i++) rirb[i] = 0;

    // stop corb/rirb
    Write8(HDA_CORBCTL, 0);
    Write8(HDA_RIRBCTL, 0);
    DelayUs(100);

    // set corb base address  -  full 64-bit phys (identity-mapped: phys==virt),
    // upper half must not be hardcoded to 0 or a heap address >4 gb would be
    // truncated and the controller would dma the wrong page. (satoru)
    uint64_t corb_phys = (uint64_t)(uintptr_t)corb;
    Write32(HDA_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    Write32(HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));

    // reset corb read pointer
    Write16(HDA_CORBRP, (1 << 15));
    for (int i = 0; i < 1000; i++) {
        if (Read16(HDA_CORBRP) & (1 << 15)) break;
        DelayUs(10);
    }
    Write16(HDA_CORBRP, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(Read16(HDA_CORBRP) & (1 << 15))) break;
        DelayUs(10);
    }

    // reset corb write pointer
    Write16(HDA_CORBWP, 0);

    // set rirb base address  -  full 64-bit phys, same upper-half fix as corb
    // above so a >4 gb heap allocation is addressed correctly. (satoru)
    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb;
    Write32(HDA_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    Write32(HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // reset rirb write pointer, then sync our software read pointer to whatever
    // the hardware write pointer reads back so the two start equal. otherwise a
    // stale hardware RIRBWP (it does not necessarily clear to 0 on the wp-reset
    // strobe) would make WaitRIRB think a response is already pending and read a
    // garbage slot for the very first verb. (satoru)
    Write16(HDA_RIRBWP, (1 << 15));
    DelayUs(10);
    rirb_rp = Read16(HDA_RIRBWP) & 0xFF;

    // clear any latched rirb status (response-int bit0, overrun bit2) so the
    // status register doesn't start out wedged. (satoru)
    Write8(HDA_RIRBSTS, (1 << 0) | (1 << 2));

    // set rintcnt  -  one response per interrupt. (satoru)
    Write16(HDA_RINTCNT, 1);

    // start corb and rirb
    Write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    Write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN | HDA_RIRBCTL_INT);
    DelayUs(50);

    return true;
}

bool HDAudio::SendVerb(uint32_t verb, uint32_t* response) {
    // drop any stale responses by resyncing our software read pointer to the
    // current hardware write pointer before issuing. this guarantees WaitRIRB
    // only returns the response to *this* verb, not a leftover from a previous
    // command whose response we never consumed. (satoru)
    rirb_rp = Read16(HDA_RIRBWP) & 0xFF;

    // advance corb write pointer from the hardware value (+1) and post the verb
    // into that slot, then publish the new write pointer. (satoru)
    uint16_t wp = Read16(HDA_CORBWP) & 0xFF;
    wp = (wp + 1) % corb_size;
    corb[wp] = verb;
    Write16(HDA_CORBWP, wp);

    return WaitRIRB(response, 5000);
}

bool HDAudio::WaitRIRB(uint32_t* response, int timeout) {
    for (int i = 0; i < timeout * 100; i++) {
        uint16_t wp = Read16(HDA_RIRBWP) & 0xFF;

        // the controller writes a response THEN bumps RIRBWP to point AT the
        // slot it just filled. so when our read pointer differs from the
        // hardware write pointer, the next unread entry is at (rirb_rp+1):
        // advance first, then read that slot. (the old code read rirb[rirb_rp]
        // after advancing too, but never resynced on send and never cleared
        // status, so it drifted and wedged.) (satoru)
        if (rirb_rp != wp) {
            int next = (rirb_rp + 1) % rirb_size;
            rirb_rp = next;
            uint64_t entry = rirb[next];
            // low 32 bits = response, high 32 = response_ex (codec addr etc.).
            if (response) *response = (uint32_t)(entry & 0xFFFFFFFFu);

            // clear the rirb status (response-int bit0 + overrun bit2) so the
            // status register never stays latched and wedges later reads. (satoru)
            Write8(HDA_RIRBSTS, (1 << 0) | (1 << 2));
            return true;
        }
        DelayUs(1);
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

// locate the audio function group node on codec `cad`. don't assume it's node
// 1: read the root node (0) node-count param to get the function-group range,
// then query each group's function-group-type and return the one whose type is
// 0x01 (audio fg). returns -1 if none. (satoru)
int HDAudio::FindAFG(int cad) {
    uint32_t resp = 0;
    SendVerb(HDA_VERB(cad, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT), &resp);
    int start = (resp >> 16) & 0xFF;
    int count = resp & 0xFF;
    for (int i = 0; i < count && i < HDA_MAX_NODES; i++) {
        int fg = start + i;
        uint32_t fgt = 0;
        SendVerb(HDA_VERB(cad, fg, HDA_VERB_GET_PARAM, HDA_PARAM_FN_GROUP), &fgt);
        if ((fgt & 0xFF) == 0x01) return fg;   // 0x01 = audio function group
    }
    return -1;
}

bool HDAudio::FindOutputPath(int cad) {
    uint32_t resp = 0;

    // discover the audio function group rather than hardcoding nid 1. (satoru)
    int afg = FindAFG(cad);
    if (afg < 0) return false;

    // (a) power the afg to D0 and let it settle before touching widgets. (satoru)
    SendVerb(HDA_VERB(cad, afg, HDA_VERB_SET_POWER, 0x00), &resp);
    DelayUs(150);

    // enumerate the widgets under the afg.
    SendVerb(HDA_VERB(cad, afg, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT), &resp);
    int start_nid = (resp >> 16) & 0xFF;
    int num_nodes = resp & 0xFF;

    // (b) pick the best output pin and the dac it routes to. score pins by their
    // configuration default so a connected line-out/speaker/headphone wins over
    // an unconnected internal pin. record the dac's CONNECTION INDEX on the pin
    // (SET_CONNSEL takes the list index, not the node id). (satoru)
    int best_pin = -1, best_dac = -1, best_index = 0;
    int best_score = -1;
    uint32_t best_pincap = 0;

    for (int n = 0; n < num_nodes && n < HDA_MAX_NODES; n++) {
        int nid = start_nid + n;

        uint32_t wcap = 0;
        SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET), &wcap);
        uint8_t wtype = (wcap >> 20) & 0xF;     // widget type [23:20]
        if (wtype != HDA_WIDGET_PIN) continue;

        // output-capable pin? (pin caps bit 4) (satoru)
        uint32_t pincap = 0;
        SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP), &pincap);
        if (!(pincap & (1 << 4))) continue;

        // resolve a dac via this pin's connection list. read the list length,
        // then walk the entries (short form: 4 nids per response) and find an
        // entry whose widget type is AUDIO_OUTPUT. (satoru)
        uint32_t connlen_param = 0;
        SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_PARAM, HDA_PARAM_CONN_LEN), &connlen_param);
        int list_len = connlen_param & 0x7F;        // long-form bit is [7]; assume short. (satoru)
        if (list_len <= 0) continue;
        if (list_len > 16) list_len = 16;

        int dac_nid = -1, dac_index = -1;
        for (int ci = 0; ci < list_len; ci++) {
            uint32_t cl = 0;
            // each GET_CONNECTION_LIST response holds 4 short-form entries; the
            // payload is the starting index (rounded to the group of 4). (satoru)
            SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_CONNLIST, ci & ~0x3), &cl);
            int entry = (cl >> ((ci & 0x3) * 8)) & 0xFF;
            if (entry == 0) continue;
            uint32_t ewcap = 0;
            SendVerb(HDA_VERB(cad, entry, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET), &ewcap);
            if (((ewcap >> 20) & 0xF) == HDA_WIDGET_AUDIO_OUT) {
                dac_nid = entry;
                dac_index = ci;
                break;
            }
        }
        if (dac_nid < 0) continue;

        // score by configuration default (verb 0xF1C): port-connectivity [31:30]
        // (2 == "no physical connection" => worst) and default-device [23:20]
        // (line-out / speaker / headphone are preferred outputs). (satoru)
        uint32_t cfg = 0;
        SendVerb(HDA_VERB(cad, nid, HDA_VERB_GET_CONFIG_DEF, 0), &cfg);
        int port_conn = (cfg >> 30) & 0x3;
        int def_dev   = (cfg >> 20) & 0xF;

        int score = 0;
        if (port_conn != 0x1) score += 4;       // 0x1 = no connection (jack absent) (satoru)
        // default-device: 0=line-out, 1=speaker, 2=hp-out are the real outputs.
        if (def_dev == 0x0 || def_dev == 0x1 || def_dev == 0x2) score += 8;

        if (score > best_score) {
            best_score   = score;
            best_pin     = nid;
            best_dac     = dac_nid;
            best_index   = dac_index;
            best_pincap  = pincap;
        }
    }

    if (best_pin < 0 || best_dac < 0) return false;

    output_nid = best_dac;
    pin_nid    = best_pin;

    // (c) power the dac and the pin to D0, each followed by a settle. (satoru)
    SendVerb(HDA_VERB(cad, best_dac, HDA_VERB_SET_POWER, 0x00), &resp);
    DelayUs(150);
    SendVerb(HDA_VERB(cad, best_pin, HDA_VERB_SET_POWER, 0x00), &resp);
    DelayUs(150);

    // (d) route the pin to the dac via the connection INDEX (not the node id).
    // without this SET_CONNSEL the codec graph has no path from converter to
    // jack and silently drops every sample. (satoru)
    SendVerb(HDA_VERB(cad, best_pin, HDA_VERB_SET_CONNSEL, best_index & 0xFF), &resp);

    // (e) read the dac's output-amp capabilities to size the gain. num steps is
    // bits [14:8] of the amp-out-cap param; full-scale gain == numSteps. (satoru)
    uint32_t ampcap = 0;
    SendVerb(HDA_VERB(cad, best_dac, HDA_VERB_GET_PARAM, HDA_PARAM_AMP_OUT_CAP), &ampcap);
    uint32_t num_steps = (ampcap >> 8) & 0x7F;
    uint32_t gain = num_steps ? num_steps : 0x7F;   // fall back to max if unreported (satoru)
    if (gain > 0x7F) gain = 0x7F;

    // (f) unmute + set gain. the amp payload is a 16-bit field; build the full
    // dword as a 4-bit verb (0x3): (cad<<28)|(nid<<20)|(0x3<<16)|payload. this
    // is bit-identical to HDA_VERB(cad,nid,0x300,payload), so either spelling
    // works; HDA_VERB4 keeps the spec verb id explicit. (satoru)
    //   dac output amp:  set-output(15) | left(13) | right(12) | unmute(¬11) | gain
    uint32_t dac_amp = (1u << 15) | (1u << 13) | (1u << 12) | (gain & 0x7F);
    SendVerb(HDA_VERB4(cad, best_dac, HDA_VERB_SET_AMP_GAIN4, dac_amp), &resp);
    //   pin input amp:   set-input(14) | left(13) | right(12) | unmute | max gain
    uint32_t pin_amp = (1u << 14) | (1u << 13) | (1u << 12) | 0x7F;
    SendVerb(HDA_VERB4(cad, best_pin, HDA_VERB_SET_AMP_GAIN4, pin_amp), &resp);

    // (g) enable pin output; also drive the headphone amp if the pin advertises
    // headphone-drive capability (pin caps bit 3). (satoru)
    uint32_t pinctl = (1u << 6);                 // OUT_EN
    if (best_pincap & (1 << 3)) pinctl |= (1u << 7);  // HP_EN
    SendVerb(HDA_VERB(cad, best_pin, HDA_VERB_SET_PINCTL, pinctl), &resp);

    // (h) if the pin supports EAPD (pin caps bit 16), enable it (bit1)  -  many
    // consumer codecs keep the external amp / charge pump powered down until
    // EAPD is asserted. then wait for the analog stage to settle. (satoru)
    if (best_pincap & (1 << 16)) {
        SendVerb(HDA_VERB(cad, best_pin, HDA_VERB_SET_EAPDBTL, 0x02), &resp);
    }
    DelayMs(10);

    return true;
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

    // reset stream  -  assert SRST and wait for the controller to acknowledge by
    // reading the bit back as 1 (calibrated waits so the settle survives -O2). (satoru)
    Write8(stream_base + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 1000; i++) {
        if (Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        DelayUs(10);
    }

    // clear reset and wait for the bit to read back as 0 before reprogramming. (satoru)
    Write8(stream_base + HDA_SD_CTL, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(Read8(stream_base + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        DelayUs(10);
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
    // dma_buffer is non-null whenever g_hda_stream_live (StartStream gates on it),
    // but guard the deref anyway  -  zero-risk, can only avert a fault. (satoru)
    if (!dma_buffer) return 0;
    // clamp lpib's per-tick advance so a qemu link-position jump can't fake free
    // space (see HDA_ClampLpibAdvance). (satoru)
    uint32_t raw_lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    uint32_t lpib = HDA_ClampLpibAdvance(raw_lpib);

    // queued = unplayed bytes between the read pointer (lpib) and our write
    // cursor, going forward mod N; free = N - queued.  both branches below
    // already reduce to (N - queued) but computing queued explicitly lets us add
    // a hard lap guard the old code lacked. (satoru)
    uint32_t queued;
    if (g_hda_write_ptr >= lpib) {
        queued = g_hda_write_ptr - lpib;
    } else {
        queued = HDA_BUFFER_SIZE - (lpib - g_hda_write_ptr);
    }

    // lap guard: never let the write cursor reach or pass lpib.  keep at least
    // one period + one frame of slack so even with the full/empty ambiguity at
    // write_ptr==lpib (which the old N-(write-lpib) math mis-read as a totally
    // FREE ring) we can't overwrite samples the dma is mid-flight on.  when the
    // ring is already that full, skip this period  -  the mixer back-pressure gate
    // will simply try again next tick. (satoru)
    const uint32_t kPeriod = 4096;
    const uint32_t kGuard  = kPeriod + 4;
    if (queued + kGuard >= HDA_BUFFER_SIZE) return 0;
    uint32_t free_bytes = HDA_BUFFER_SIZE - queued - kGuard;
    if (free_bytes == 0) return 0;

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

    // clean-silence underrun pad: zero exactly one period immediately ahead of
    // the new write cursor so that if the cooperative pump stalls (>~213ms) and
    // the dma read pointer (lpib) catches up to write_ptr, it reads silence
    // instead of replaying the stale samples still sitting in the free region
    // from the previous lap (the audible click/repeat). this is the underrun
    // counterpart to the kGuard overrun lap-guard above, and is provably safe to
    // touch: after this write the guard leaves at least
    //   N - queued_new >= kGuard == kPeriod + 4
    // free bytes forward between write_ptr and lpib (to_write was capped at
    // free_bytes = N - queued - kGuard), so a kPeriod pad ahead of write_ptr
    // stays >=4 bytes short of lpib and never overwrites the bytes the dma is
    // reading now. clamped lpib only lags the true read position, which makes the
    // real margin larger, never smaller. wrap by hand to match the ring math. (satoru)
    uint32_t pad = g_hda_write_ptr;
    for (uint32_t i = 0; i < kPeriod; i++) {
        dst_base[pad] = 0;
        pad++;
        if (pad == HDA_BUFFER_SIZE) pad = 0;
    }
    return to_write;
}

uint32_t HDAudio::RingQueuedBytes() {
    if (!g_hda_stream_live) return 0;
    // use the same clamped link position as WriteRing so the back-pressure gate
    // sees a stable queue depth instead of jittering on qemu lpib jumps. (satoru)
    uint32_t raw_lpib = Read32(stream_base + HDA_SD_LPIB) % HDA_BUFFER_SIZE;
    uint32_t lpib = HDA_ClampLpibAdvance(raw_lpib);
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
        uint32_t amp_val = (1u << 15) | (1u << 13) | (1u << 12) | gain; // output, L+R, unmute
        // proper SET_AMP_GAIN (4-bit verb 0x3 + 16-bit payload) on dac + pin.
        // the old form masked the payload to 8 bits and corrupted the verb. (satoru)
        SendVerb(HDA_VERB(codec_addr, output_nid, HDA_VERB_SET_AMP_GAIN, amp_val), &resp);
        if (pin_nid >= 0)
            SendVerb(HDA_VERB(codec_addr, pin_nid, HDA_VERB_SET_AMP_GAIN, amp_val), &resp);
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
