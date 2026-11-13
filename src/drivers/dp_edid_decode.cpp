// displayport dpcd and edid decoders, see the header. freestanding: no libc,
// no kernel headers, builds unchanged on the host for the unit test. (satoru)
#include "dp_edid_decode.h"

namespace DpEdid {

// ---- text helpers (satoru) ----

Txt& Txt::s(const char* t) {
    while (t && *t && n < max - 1) b[n++] = *t++;
    if (max > 0) b[n] = 0;
    return *this;
}

Txt& Txt::c(char ch) {
    if (n < max - 1) { b[n++] = ch; b[n] = 0; }
    return *this;
}

Txt& Txt::d(uint64_t v) {
    char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) c(t[--k]);
    return *this;
}

Txt& Txt::x(uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) c("0123456789abcdef"[(v >> (i * 4)) & 0xF]);
    return *this;
}

Txt& Txt::hz(uint32_t mhz) {
    d(mhz / 1000); c('.');
    uint32_t f = mhz % 1000;
    c((char)('0' + f / 100)); c((char)('0' + (f / 10) % 10)); c((char)('0' + f % 10));
    return *this;
}

// ---- dpcd (satoru) ----

const char* LinkRateName(uint8_t code) {
    // drm_dp.h: dp_link_bw_1_62 0x06, 2_7 0x0a, 5_4 0x14, 8_1 0x1e (satoru)
    switch (code) {
        case 0x06: return "rbr";
        case 0x0a: return "hbr";
        case 0x14: return "hbr2";
        case 0x1e: return "hbr3";
        default: return "?";
    }
}

int SinkCount(uint8_t v) {
    // drm_dp.h: dp_get_sink_count(x) ((((x) & 0x80) >> 1) | ((x) & 0x3f)) (satoru)
    return ((v & 0x80) >> 1) | (v & 0x3f);
}

void DecodeDpcd(const uint8_t* r, int n, Dpcd* d) {
    *d = Dpcd();
    d->n = n;
    if (n < 15) return;
    // drm_dp.h offsets: 0x000 dp_dpcd_rev, 0x001 dp_max_link_rate, 0x002
    // dp_max_lane_count (4:0, bit 5 post_lt_adj_req, bit 6 tps3, bit 7
    // enhanced frame), 0x003 dp_max_downspread (bit 0 0.5%, bit 6 no aux
    // handshake, bit 7 tps4), 0x005 dp_downstreamport_present (bit 0, type
    // 2:1), 0x006 dp_main_link_channel_coding (bit 0 8b10b, bit 1 128b132b),
    // 0x007 dp_down_stream_port_count (3:0, bit 6 msa ignored, bit 7 oui),
    // 0x00d dp_edp_configuration_cap, 0x00e dp_training_aux_rd_interval
    // (6:0, bit 7 extended receiver cap present). (satoru)
    d->rev_major = r[0] >> 4;
    d->rev_minor = r[0] & 0xF;
    d->link_rate_code = r[1];
    d->link_rate_mbps = r[1] == 0x06 ? 1620 : r[1] == 0x0a ? 2700 : r[1] == 0x14 ? 5400 : r[1] == 0x1e ? 8100 : 0;
    d->max_lanes = r[2] & 0x1F;
    d->post_lt_adj = (r[2] >> 5) & 1;
    d->tps3 = (r[2] >> 6) & 1;
    d->enhanced_frame = (r[2] >> 7) & 1;
    d->downspread = r[3] & 1;
    d->no_aux_handshake = (r[3] >> 6) & 1;
    d->tps4 = (r[3] >> 7) & 1;
    d->dfp_present = r[5] & 1;
    d->dfp_type = (r[5] >> 1) & 3;
    d->coding_8b10b = r[6] & 1;
    d->coding_128b132b = (r[6] >> 1) & 1;
    d->dfp_count = r[7] & 0xF;
    d->msa_ignored = (r[7] >> 6) & 1;
    d->oui = (r[7] >> 7) & 1;
    d->edp_cfg = r[0xd];
    d->aux_rd_interval = r[0xe] & 0x7F;
    d->ext_caps = (r[0xe] >> 7) & 1;
}

void FormatDpcd(const Dpcd& d, LineFn fn, void* ctx) {
    char b[200];
    if (d.n < 15) { Txt t(b, sizeof(b)); t.s("dpcd: short read, not decoded"); fn(ctx, b); return; }
    {
        Txt t(b, sizeof(b));
        t.s("dpcd rev "); t.d(d.rev_major); t.c('.'); t.d(d.rev_minor);
        t.s(" max_link_rate 0x"); t.x(d.link_rate_code, 2); t.s(" ("); t.s(LinkRateName(d.link_rate_code));
        if (d.link_rate_mbps) { t.c(' '); t.d(d.link_rate_mbps); t.s(" mbps/lane"); }
        t.s(") max_lanes "); t.d(d.max_lanes);
        t.s(" tps3="); t.d(d.tps3); t.s(" tps4="); t.d(d.tps4);
        t.s(" enh_frame="); t.d(d.enhanced_frame); t.s(" post_lt_adj="); t.d(d.post_lt_adj);
        fn(ctx, b);
    }
    {
        Txt t(b, sizeof(b));
        t.s("dpcd downspread="); t.d(d.downspread); t.s(" no_aux_handshake="); t.d(d.no_aux_handshake);
        t.s(" coding 8b10b="); t.d(d.coding_8b10b); t.s(" 128b132b="); t.d(d.coding_128b132b);
        t.s(" dfp_present="); t.d(d.dfp_present);
        if (d.dfp_present) {
            static const char* ty[4] = { "dp", "vga", "dvi/hdmi/dp++", "other" };
            t.s(" dfp_type="); t.s(ty[d.dfp_type]); t.s(" dfp_count="); t.d(d.dfp_count);
        }
        t.s(" msa_ignored="); t.d(d.msa_ignored); t.s(" oui="); t.d(d.oui);
        t.s(" edp_cfg 0x"); t.x(d.edp_cfg, 2);
        t.s(" aux_rd_interval "); t.d(d.aux_rd_interval); t.s(" ext_caps="); t.d(d.ext_caps);
        fn(ctx, b);
    }
}

// ---- edid (satoru) ----

namespace {

void copy_text(const uint8_t* p, char* out) {
    // monitor descriptor text: 13 bytes, ends at 0x0a, padded with 0x20 (satoru)
    int k = 0;
    for (int i = 0; i < 13; i++) {
        uint8_t ch = p[i];
        if (ch == 0x0a || ch == 0) break;
        out[k++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

uint32_t refresh_of(uint32_t pixclk_khz, uint32_t htot, uint32_t vtot) {
    uint64_t frame = (uint64_t)htot * vtot;
    if (!frame) return 0;
    return (uint32_t)((uint64_t)pixclk_khz * 1000000ull / frame);
}

void add_timing(Edid* e, const Timing& t) {
    if (e->nt >= MAX_TIMINGS) return;
    e->t[e->nt++] = t;
}

// drm_edid.c drm_mode_detailed / struct detailed_pixel_timing: 18 byte
// detailed timing descriptor, pixel clock in 10 khz units. (satoru)
bool decode_dtd(const uint8_t* b, Timing* t) {
    uint32_t pc = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    if (pc == 0) return false;
    *t = Timing();
    t->pixclk_khz = pc * 10;
    t->hact   = (uint16_t)(b[2] | ((b[4] & 0xF0) << 4));
    t->hblank = (uint16_t)(b[3] | ((b[4] & 0x0F) << 8));
    t->vact   = (uint16_t)(b[5] | ((b[7] & 0xF0) << 4));
    t->vblank = (uint16_t)(b[6] | ((b[7] & 0x0F) << 8));
    t->hfp    = (uint16_t)(b[8] | ((b[11] & 0xC0) << 2));
    t->hsync  = (uint16_t)(b[9] | ((b[11] & 0x30) << 4));
    t->vfp    = (uint16_t)((b[10] >> 4) | ((b[11] & 0x0C) << 2));
    t->vsync  = (uint16_t)((b[10] & 0x0F) | ((b[11] & 0x03) << 4));
    t->hmm    = (uint16_t)(b[12] | ((b[14] & 0xF0) << 4));
    t->vmm    = (uint16_t)(b[13] | ((b[14] & 0x0F) << 8));
    t->interlaced = (b[17] >> 7) & 1;
    // flags bits 4:3 = 3 is digital separate sync: bit 2 vsync +, bit 1
    // hsync + (drm_mode_detailed) (satoru)
    if (((b[17] >> 3) & 3) == 3) { t->vpos = (b[17] >> 2) & 1; t->hpos = (b[17] >> 1) & 1; }
    t->refresh_mhz = refresh_of(t->pixclk_khz, (uint32_t)t->hact + t->hblank, (uint32_t)t->vact + t->vblank);
    return true;
}

void decode_descriptor(const uint8_t* b, Edid* e) {
    // display descriptor: bytes 0-1 zero, byte 3 tag (satoru)
    uint8_t tag = b[3];
    if (tag == 0xFC) copy_text(b + 5, e->name);
    else if (tag == 0xFF) copy_text(b + 5, e->serial_str);
    else if (tag == 0xFD) {
        // range limits (drm_edid.c edid_detail_monitor_range): byte 4
        // offset flags, bit 0 vmax +255, bit 1 vmin +255, bit 2 hmax +255,
        // bit 3 hmin +255 (edid 1.4) (satoru)
        uint8_t f = b[4];
        e->has_range = true;
        e->vmin_hz = (uint16_t)(b[5] + ((f & 2) ? 255 : 0));
        e->vmax_hz = (uint16_t)(b[6] + ((f & 1) ? 255 : 0));
        e->hmin_khz = (uint16_t)(b[7] + ((f & 8) ? 255 : 0));
        e->hmax_khz = (uint16_t)(b[8] + ((f & 4) ? 255 : 0));
        e->max_pclk_mhz = (uint16_t)(b[9] * 10);
    }
}

void decode_cta(const uint8_t* b, int blk, Edid* e) {
    // cta-861 extension: byte 1 rev, byte 2 dtd offset, byte 3 flags, data
    // block collection from byte 4 up to the dtd offset (drm_edid.c
    // cea_db_iter, cea_db_tag / cea_db_payload_len) (satoru)
    bool first = !e->cta;
    e->cta = true;
    uint8_t d = b[2];
    if (first) { e->cta_rev = b[1]; e->cta_flags = b[3]; }
    if (d >= 4 && d <= 127 && b[1] >= 3) {
        int o = 4;
        while (o < d) {
            uint8_t tag = b[o] >> 5, len = b[o] & 0x1F;
            if (o + 1 + len > d) break;
            const uint8_t* p = b + o + 1;
            if (tag == 2) {
                for (int i = 0; i < len && e->nvic < 32; i++) e->vic[e->nvic++] = p[i];
            } else if (tag == 3 && len >= 3) {
                uint32_t oui = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                if (oui == 0x000C03) e->hdmi_vsdb = true;   // hdmi_ieee_oui (satoru)
                if (oui == 0xC45DD8) e->hf_vsdb = true;     // hdmi_forum_ieee_oui (satoru)
            }
            o += 1 + len;
        }
    }
    if (d >= 4 && d < 127) {
        for (int o = d; o + 18 <= 127; o += 18) {
            Timing t;
            if (!decode_dtd(b + o, &t)) break;
            t.source = SRC_CTA_DTD;
            t.block = (uint8_t)blk;
            add_timing(e, t);
        }
    }
}

// drm_edid.c drm_mode_displayid_detailed: 20 byte type i (0x03) / type vii
// (0x22) descriptor. pixel clock 24 bit little endian + 1, in 10 khz units
// for type i and 1 khz for type vii. byte 3 bit 7 preferred, bit 4
// interlaced. each 16 bit field is value + 1; bit 15 of hsync offset and
// vsync offset is the positive polarity flag. (satoru)
void decode_did_timing(const uint8_t* p, bool type7, int blk, Edid* e) {
    Timing t = Timing();
    uint32_t pc = ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) + 1;
    t.pixclk_khz = type7 ? pc : pc * 10;
    t.preferred = (p[3] >> 7) & 1;
    t.interlaced = (p[3] >> 4) & 1;
    uint16_t hact = (uint16_t)((p[4] | (p[5] << 8)) + 1);
    uint16_t hblank = (uint16_t)((p[6] | (p[7] << 8)) + 1);
    uint16_t hfp = (uint16_t)((p[8] | ((p[9] & 0x7F) << 8)) + 1);
    uint16_t hsw = (uint16_t)((p[10] | (p[11] << 8)) + 1);
    uint16_t vact = (uint16_t)((p[12] | (p[13] << 8)) + 1);
    uint16_t vblank = (uint16_t)((p[14] | (p[15] << 8)) + 1);
    uint16_t vfp = (uint16_t)((p[16] | ((p[17] & 0x7F) << 8)) + 1);
    uint16_t vsw = (uint16_t)((p[18] | (p[19] << 8)) + 1);
    t.hact = hact; t.hblank = hblank; t.hfp = hfp; t.hsync = hsw;
    t.vact = vact; t.vblank = vblank; t.vfp = vfp; t.vsync = vsw;
    t.hpos = (p[9] >> 7) & 1;
    t.vpos = (p[17] >> 7) & 1;
    t.source = type7 ? SRC_DID_T7 : SRC_DID_T1;
    t.block = (uint8_t)blk;
    t.refresh_mhz = refresh_of(t.pixclk_khz, (uint32_t)hact + hblank, (uint32_t)vact + vblank);
    add_timing(e, t);
}

void decode_displayid(const uint8_t* b, int blk, Edid* e) {
    // edid extension tag 0x70: byte 1 version, byte 2 bytes in section,
    // byte 3 product type, byte 4 extension count, data blocks from byte 5
    // (drm_displayid.c displayid_iter_block: tag, rev, num_bytes, payload)
    // (satoru)
    e->did = true;
    e->did_ver = b[1];
    int len = b[2];
    int end = 5 + len;
    if (end > 127) end = 127;
    int o = 5;
    while (o + 3 <= end) {
        uint8_t tag = b[o], rev = b[o + 1], nb = b[o + 2];
        if (tag == 0 && nb == 0) break;    // padding (satoru)
        if (o + 3 + nb > end) break;
        const uint8_t* p = b + o + 3;
        if (tag == 0x03 || tag == 0x22) {
            bool t7 = tag == 0x22;
            // type vii descriptor size is 20 plus rev bits 6:4 (displayid
            // 2.0); type i is always 20 (satoru)
            int ds = 20 + (t7 ? ((rev >> 4) & 7) : 0);
            for (int k = 0; k + ds <= nb; k += ds) decode_did_timing(p + k, t7, blk, e);
        }
        o += 3 + nb;
    }
}

} // namespace

bool DecodeEdid(const uint8_t* data, int len, Edid* e) {
    *e = Edid();
    e->preferred = -1;
    int blocks = len / 128;
    if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;
    e->blocks = blocks;
    if (blocks < 1) return false;
    for (int k = 0; k < blocks; k++) {
        uint8_t sum = 0;
        for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + data[k * 128 + i]);
        e->sum_ok[k] = sum == 0;
        e->ext_tag[k] = data[k * 128];
    }
    const uint8_t* b = data;
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    e->header_ok = true;
    for (int i = 0; i < 8; i++) if (b[i] != hdr[i]) e->header_ok = false;
    if (!e->header_ok) return false;

    // struct edid in drm_edid.h: mfg_id 8-9 (big endian, 3 x 5 bit letters),
    // prod_code 10-11 and serial 12-15 little endian, mfg_week 16, mfg_year
    // 17 (+1990), version 18, revision 19, input 20, width_cm 21, height_cm
    // 22, established 35-37, standard timings 38-53, detailed 54-125,
    // extensions 126, checksum 127. (satoru)
    uint16_t m = (uint16_t)((b[8] << 8) | b[9]);
    e->mfg[0] = (char)('@' + ((m >> 10) & 31));
    e->mfg[1] = (char)('@' + ((m >> 5) & 31));
    e->mfg[2] = (char)('@' + (m & 31));
    e->mfg[3] = 0;
    e->product = (uint16_t)(b[10] | (b[11] << 8));
    e->serial = (uint32_t)b[12] | ((uint32_t)b[13] << 8) | ((uint32_t)b[14] << 16) | ((uint32_t)b[15] << 24);
    e->week = b[16];
    e->year = (uint16_t)(1990 + b[17]);
    e->ver = b[18];
    e->rev = b[19];
    e->digital = (b[20] >> 7) & 1;
    if (e->digital && e->rev >= 4) {
        // drm_edid.h drm_edid_digital_depth_mask 6:4 (1=6 .. 6=16 bpc) and
        // drm_edid_digital_type_mask 3:0 (satoru)
        static const uint8_t bpc[8] = { 0, 6, 8, 10, 12, 14, 16, 0 };
        e->bpc = bpc[(b[20] >> 4) & 7];
        e->iface = b[20] & 0xF;
    }
    e->hcm = b[21];
    e->vcm = b[22];
    e->ext_count = b[126];

    for (int i = 0; i < 8; i++) {
        uint8_t s1 = b[38 + i * 2], s2 = b[39 + i * 2];
        if ((s1 == 0x01 && s2 == 0x01) || s1 == 0) continue;
        StdTiming st;
        st.w = (uint16_t)((s1 + 31) * 8);
        // aspect bits 7:6: 0 16:10 (edid 1.3+), 1 4:3, 2 5:4, 3 16:9 (satoru)
        uint8_t a = s2 >> 6;
        uint32_t h = a == 0 ? st.w * 10u / 16u : a == 1 ? st.w * 3u / 4u : a == 2 ? st.w * 4u / 5u : st.w * 9u / 16u;
        st.h = (uint16_t)h;
        st.hz = (uint8_t)((s2 & 0x3F) + 60);
        e->std[e->nstd++] = st;
    }

    for (int i = 0; i < 4; i++) {
        const uint8_t* p = b + 54 + i * 18;
        Timing t;
        if (decode_dtd(p, &t)) {
            t.source = SRC_BASE_DTD;
            t.block = 0;
            // edid 1.4: the first detailed timing is always the preferred
            // mode (drm_edid.c, "preferred" for the first detailed) (satoru)
            if (i == 0) t.preferred = true;
            add_timing(e, t);
        } else {
            decode_descriptor(p, e);
        }
    }

    for (int k = 1; k < blocks; k++) {
        const uint8_t* x = data + k * 128;
        if (x[0] == 0x02) decode_cta(x, k, e);
        else if (x[0] == 0x70) decode_displayid(x, k, e);
    }

    for (int i = 0; i < e->nt; i++) if (e->t[i].preferred && e->t[i].source == SRC_BASE_DTD) { e->preferred = i; break; }
    if (e->preferred < 0) for (int i = 0; i < e->nt; i++) if (e->t[i].preferred) { e->preferred = i; break; }
    if (e->preferred < 0 && e->nt > 0) e->preferred = 0;
    return true;
}

int MaxRefreshAtPreferred(const Edid& e) {
    if (e.preferred < 0) return -1;
    const Timing& p = e.t[e.preferred];
    int best = e.preferred;
    for (int i = 0; i < e.nt; i++)
        if (e.t[i].hact == p.hact && e.t[i].vact == p.vact && !e.t[i].interlaced &&
            e.t[i].refresh_mhz > e.t[best].refresh_mhz) best = i;
    return best;
}

int PickTimingAtResolution(const Edid& e, uint32_t target_mhz) {
    if (e.preferred < 0) return -1;
    const Timing& pt = e.t[e.preferred];
    int best = -1;
    uint32_t bestd = 0xFFFFFFFFu;
    for (int i = 0; i < e.nt; i++) {
        const Timing& t = e.t[i];
        if (t.hact != pt.hact || t.vact != pt.vact || t.interlaced) continue;
        uint32_t d = t.refresh_mhz > target_mhz ? t.refresh_mhz - target_mhz : target_mhz - t.refresh_mhz;
        if (d < bestd || (d == bestd && best >= 0 && t.pixclk_khz < e.t[best].pixclk_khz)) { bestd = d; best = i; }
    }
    return best < 0 ? e.preferred : best;
}

int RatesAtPreferred(const Edid& e, uint32_t* hz, int max) {
    if (e.preferred < 0 || !hz || max <= 0) return 0;
    const Timing& pt = e.t[e.preferred];
    int n = 0;
    for (int i = 0; i < e.nt && n < max; i++) {
        const Timing& t = e.t[i];
        if (t.hact != pt.hact || t.vact != pt.vact || t.interlaced) continue;
        uint32_t r = (t.refresh_mhz + 500) / 1000;
        if (r < 1) continue;
        int at = 0;
        while (at < n && hz[at] < r) at++;
        if (at < n && hz[at] == r) continue;
        if (n == max) continue;
        for (int k = n; k > at; k--) hz[k] = hz[k - 1];
        hz[at] = r;
        n++;
    }
    return n;
}

int PickFittingTiming(const Edid& e, uint32_t target_mhz, bool (*fits)(const Timing& t, void* ctx), void* ctx) {
    int best = -1;
    uint64_t best_area = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    for (int i = 0; i < e.nt; i++) {
        const Timing& t = e.t[i];
        if (t.interlaced || !t.hact || !t.vact || !t.pixclk_khz) continue;
        if (fits && !fits(t, ctx)) continue;
        uint64_t area = (uint64_t)t.hact * t.vact;
        uint32_t d = t.refresh_mhz > target_mhz ? t.refresh_mhz - target_mhz : target_mhz - t.refresh_mhz;
        bool better = best < 0 || area > best_area ||
                      (area == best_area && (d < best_d || (d == best_d && t.pixclk_khz < e.t[best].pixclk_khz)));
        if (better) { best = i; best_area = area; best_d = d; }
    }
    return best;
}

void FormatTiming(const Timing& t, char* buf, int max) {
    Txt s(buf, max);
    s.d(t.hact); s.c('x'); s.d(t.vact); if (t.interlaced) s.c('i');
    s.c('@'); s.hz(t.refresh_mhz);
    s.s(" pixclk "); s.d(t.pixclk_khz); s.s("khz");
    s.s(" h "); s.d(t.hact); s.c(' '); s.d(t.hfp); s.c(' '); s.d(t.hsync); s.c(' ');
    s.d(t.hblank >= t.hfp + t.hsync ? (uint32_t)(t.hblank - t.hfp - t.hsync) : 0);
    s.s(" ("); s.d((uint32_t)t.hact + t.hblank); s.c(')');
    s.s(" v "); s.d(t.vact); s.c(' '); s.d(t.vfp); s.c(' '); s.d(t.vsync); s.c(' ');
    s.d(t.vblank >= t.vfp + t.vsync ? (uint32_t)(t.vblank - t.vfp - t.vsync) : 0);
    s.s(" ("); s.d((uint32_t)t.vact + t.vblank); s.c(')');
    s.s(t.hpos ? " +h" : " -h"); s.s(t.vpos ? " +v" : " -v");
}

void FormatEdid(const Edid& e, LineFn fn, void* ctx) {
    char b[220];
    if (!e.header_ok) { Txt t(b, sizeof(b)); t.s("edid: bad header, not decoded"); fn(ctx, b); return; }
    {
        Txt t(b, sizeof(b));
        t.s("edid "); t.d(e.ver); t.c('.'); t.d(e.rev); t.s(" mfg "); t.s(e.mfg);
        t.s(" product 0x"); t.x(e.product, 4); t.s(" serial 0x"); t.x(e.serial, 8);
        t.s(" week "); t.d(e.week); t.s(" year "); t.d(e.year);
        t.s(" name \""); t.s(e.name); t.s("\" serial_str \""); t.s(e.serial_str); t.c('"');
        fn(ctx, b);
    }
    {
        static const char* ifn[6] = { "undef", "dvi", "hdmi-a", "hdmi-b", "mddi", "dp" };
        Txt t(b, sizeof(b));
        t.s("edid input "); t.s(e.digital ? "digital" : "analog");
        if (e.digital && e.rev >= 4) {
            t.s(" bpc "); t.d(e.bpc); t.s(" iface "); t.s(e.iface < 6 ? ifn[e.iface] : "?");
        }
        t.s(" size "); t.d(e.hcm); t.c('x'); t.d(e.vcm); t.s("cm");
        t.s(" extensions "); t.d(e.ext_count); t.s(" (read "); t.d(e.blocks > 0 ? e.blocks - 1 : 0); t.c(')');
        t.s(" checksums");
        for (int k = 0; k < e.blocks; k++) {
            t.c(' '); t.d(k); t.c(':'); t.s(e.sum_ok[k] ? "ok" : "BAD");
            if (k) { t.s("(tag 0x"); t.x(e.ext_tag[k], 2); t.c(')'); }
        }
        fn(ctx, b);
    }
    if (e.has_range) {
        Txt t(b, sizeof(b));
        t.s("edid range v "); t.d(e.vmin_hz); t.c('-'); t.d(e.vmax_hz); t.s("hz h ");
        t.d(e.hmin_khz); t.c('-'); t.d(e.hmax_khz); t.s("khz max pixclk "); t.d(e.max_pclk_mhz); t.s("mhz");
        fn(ctx, b);
    }
    if (e.nstd) {
        Txt t(b, sizeof(b));
        t.s("edid standard timings:");
        for (int i = 0; i < e.nstd; i++) { t.c(' '); t.d(e.std[i].w); t.c('x'); t.d(e.std[i].h); t.c('@'); t.d(e.std[i].hz); }
        fn(ctx, b);
    }
    if (e.cta) {
        Txt t(b, sizeof(b));
        t.s("edid cta-861 rev "); t.d(e.cta_rev);
        t.s(" underscan="); t.d((e.cta_flags >> 7) & 1); t.s(" audio="); t.d((e.cta_flags >> 6) & 1);
        t.s(" ycc444="); t.d((e.cta_flags >> 5) & 1); t.s(" ycc422="); t.d((e.cta_flags >> 4) & 1);
        t.s(" hdmi_vsdb="); t.d(e.hdmi_vsdb); t.s(" hf_vsdb="); t.d(e.hf_vsdb);
        t.s(" vics");
        for (int i = 0; i < e.nvic; i++) {
            t.c(' ');
            // vic 1-64 may carry the native flag in bit 7 (satoru)
            uint8_t v = e.vic[i];
            if (v >= 129 && v <= 192) { t.d(v & 0x7F); t.c('*'); } else t.d(v);
        }
        fn(ctx, b);
    }
    if (e.did) {
        Txt t(b, sizeof(b));
        t.s("edid displayid ver 0x"); t.x(e.did_ver, 2);
        fn(ctx, b);
    }
    static const char* srcn[4] = { "dtd", "cta-dtd", "did-t1", "did-t7" };
    for (int i = 0; i < e.nt; i++) {
        Txt t(b, sizeof(b));
        t.s("edid timing "); t.d((uint64_t)i); t.s(i == e.preferred ? " [preferred] " : " ");
        t.s(srcn[e.t[i].source & 3]); t.s(" blk "); t.d(e.t[i].block); t.c(' ');
        FormatTiming(e.t[i], b + t.n, (int)sizeof(b) - t.n);
        fn(ctx, b);
    }
}

}

// end (satoru)
