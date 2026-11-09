// intel display stage 2: dp 8b/10b link training, see intel_dp_lt.h. the
// structure mirrors intel_dp_link_training.c function by function; dpcd
// layouts from include/drm/display/drm_dp.h. (satoru)
#include "intel_dp_lt.h"
#include "intel_modeset_calc.h"

namespace DpLt {

namespace {

// drm_dp.h (satoru)
constexpr uint32_t DP_LINK_BW_SET = 0x100;
constexpr uint32_t DP_DOWNSPREAD_CTRL = 0x107;      // + 0x108 main link channel coding set (satoru)
constexpr uint32_t DP_TRAINING_PATTERN_SET = 0x102;
constexpr uint32_t DP_TRAINING_LANE0_SET = 0x103;
constexpr uint32_t DP_LANE0_1_STATUS = 0x202;
constexpr uint32_t LTTPR1_BASE = 0xf0010, LTTPR_STRIDE = 0x50;   // __dp_lttpr1_base, __dp_lttpr2_base - 1 (satoru)
constexpr uint8_t DP_SET_ANSI_8B10B = 1;
constexpr uint8_t DP_LANE_COUNT_ENHANCED_FRAME_EN = 1u << 7;
constexpr uint8_t VSWING_MASK = 0x3, PREEMPH_MASK = 0x3 << 3, PREEMPH_SHIFT = 3;
constexpr uint8_t MAX_SWING_REACHED = 1u << 2, MAX_PREEMPH_REACHED = 1u << 5;
constexpr uint8_t LANE_CR_DONE = 1, CHANNEL_EQ_BITS = 0x7, INTERLANE_ALIGN_DONE = 1;

uint32_t tp_set_reg(int phy) { return phy == 0 ? DP_TRAINING_PATTERN_SET : LTTPR1_BASE + LTTPR_STRIDE * (uint32_t)(phy - 1); }
uint32_t lane_set_reg(int phy) { return phy == 0 ? DP_TRAINING_LANE0_SET : LTTPR1_BASE + 1 + LTTPR_STRIDE * (uint32_t)(phy - 1); }
uint32_t status_reg(int phy) { return phy == 0 ? DP_LANE0_1_STATUS : 0xf0030 + LTTPR_STRIDE * (uint32_t)(phy - 1); }

// tiny line builder, freestanding (satoru)
struct Ln {
    char b[200];
    int n = 0;
    Ln() { b[0] = 0; }
    Ln& s(const char* t) { while (t && *t && n < 198) b[n++] = *t++; b[n] = 0; return *this; }
    Ln& d(int v) {
        char t[12]; int k = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
        if (v < 0) s("-");
        do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (k && n < 198) b[n++] = t[--k];
        b[n] = 0; return *this;
    }
    Ln& x(uint32_t v, int digits) {
        for (int i = digits - 1; i >= 0 && n < 198; i--) b[n++] = "0123456789abcdef"[(v >> (i * 4)) & 0xF];
        b[n] = 0; return *this;
    }
};

struct St {
    const Ops* o;
    const Params* p;
    Result* r;
    uint8_t train_set[4];
};

void logl(St& t, Ln& l) { if (t.o->log) t.o->log(t.o->ctx, l.b); }

uint8_t lane_status(const uint8_t st[6], int lane) { return (st[lane >> 1] >> ((lane & 1) * 4)) & 0xF; }
// drm_dp_get_adjust_request_voltage / _pre_emphasis: lane0 bits 1:0 / 3:2,
// lane1 bits 5:4 / 7:6 of 0x206/0x207, returned shifted to the train_set
// positions (satoru)
uint8_t adj_v(const uint8_t st[6], int lane) { return (uint8_t)((st[4 + (lane >> 1)] >> ((lane & 1) ? 4 : 0)) & 0x3); }
uint8_t adj_p(const uint8_t st[6], int lane) { return (uint8_t)(((st[4 + (lane >> 1)] >> ((lane & 1) ? 6 : 2)) & 0x3) << PREEMPH_SHIFT); }

bool downstream_of_source(const St& t, int phy) {
    // intel_dp_phy_is_downstream_of_source (satoru)
    return t.p->lttpr_count <= 0 || phy == t.p->lttpr_count;
}

uint8_t phy_voltage_max(const St& t, int phy) {
    // intel_dp_phy_voltage_max / intel_dp_lttpr_voltage_max(dp_phy + 1):
    // dp_transmitter_capability_phy_repeater bit 0 = vswing level 3 (satoru)
    if (downstream_of_source(t, phy)) return t.p->source_vswing_max;
    return (t.p->lttpr_phy_caps[phy][1] & 1) ? 3 : 2;
}

uint8_t phy_preemph_max(const St& t, int phy) {
    if (downstream_of_source(t, phy)) return t.p->source_preemph_max;
    return (uint8_t)(((t.p->lttpr_phy_caps[phy][1] & 2) ? 3 : 2) << PREEMPH_SHIFT);
}

uint8_t dp_voltage_max(uint8_t preemph) {
    switch ((preemph & PREEMPH_MASK) >> PREEMPH_SHIFT) {
        case 0: return 3;
        case 1: return 2;
        case 2: return 1;
        default: return 0;
    }
}

// intel_dp_get_lane_adjust_vswing_preemph (per lane signal levels: display
// ver >= 10) (satoru)
uint8_t lane_adjust(const St& t, int phy, const uint8_t st[6], int lane) {
    if (lane > t.p->lanes - 1) lane = t.p->lanes - 1;
    uint8_t v = adj_v(st, lane), p = adj_p(st, lane);
    uint8_t pmax = phy_preemph_max(t, phy);
    if (p >= pmax) p = pmax | MAX_PREEMPH_REACHED;
    uint8_t vm = dp_voltage_max(p);
    if (v > vm) v = vm;
    uint8_t vmax = phy_voltage_max(t, phy);
    if (v >= vmax) v = vmax | MAX_SWING_REACHED;
    return (uint8_t)(v | p);
}

void get_adjust_train(St& t, int phy, const uint8_t st[6]) {
    for (int lane = 0; lane < 4; lane++) t.train_set[lane] = lane_adjust(t, phy, st, lane);
}

bool adjust_changed(const St& t, const uint8_t a[6], const uint8_t b[6]) {
    for (int lane = 0; lane < t.p->lanes; lane++)
        if ((adj_v(a, lane) | adj_p(a, lane)) != (adj_v(b, lane) | adj_p(b, lane))) return true;
    return false;
}

bool max_vswing_reached(const St& t) {
    // intel_dp_lane_max_vswing_reached: max swing flag and v + p == 3 (satoru)
    for (int lane = 0; lane < t.p->lanes; lane++) {
        uint8_t s = t.train_set[lane];
        if (!(s & MAX_SWING_REACHED)) return false;
        if ((s & VSWING_MASK) + ((s & PREEMPH_MASK) >> PREEMPH_SHIFT) != 3) return false;
    }
    return true;
}

void set_levels(St& t, int phy) {
    // intel_dp_set_signal_levels: only the source drives the first hop (satoru)
    if (downstream_of_source(t, phy)) t.o->set_source_levels(t.o->ctx, t.train_set, t.p->lanes);
}

bool set_link_train(St& t, int phy, uint8_t pat) {
    // intel_dp_set_link_train: source pattern, then dpcd pattern + lane sets (satoru)
    t.o->set_source_pattern(t.o->ctx, pat);
    uint8_t buf[5];
    buf[0] = pat;
    for (int i = 0; i < t.p->lanes; i++) buf[1 + i] = t.train_set[i];
    return t.o->dpcd_write(t.o->ctx, tp_set_reg(phy), buf, t.p->lanes + 1) == t.p->lanes + 1;
}

bool reset_link_train(St& t, int phy, uint8_t pat) {
    for (int i = 0; i < 4; i++) t.train_set[i] = 0;
    set_levels(t, phy);
    return set_link_train(t, phy, pat);
}

bool update_link_train(St& t, int phy) {
    set_levels(t, phy);
    return t.o->dpcd_write(t.o->ctx, lane_set_reg(phy), t.train_set, t.p->lanes) == t.p->lanes;
}

bool read_status(St& t, int phy, uint8_t st[6]) {
    // drm_dp_dpcd_read_phy_link_status: lttpr layout has no sink_status, so
    // insert a zero at index 3 (satoru)
    if (phy == 0) return t.o->dpcd_read(t.o->ctx, status_reg(0), st, 6) == 6;
    uint8_t tmp[5];
    if (t.o->dpcd_read(t.o->ctx, status_reg(phy), tmp, 5) != 5) return false;
    st[0] = tmp[0]; st[1] = tmp[1]; st[2] = tmp[2]; st[3] = 0; st[4] = tmp[3]; st[5] = tmp[4];
    return true;
}

void log_iter(St& t, const char* what, int phy, int iter, const uint8_t st[6]) {
    Ln l; l.s(what); l.s(" phy "); l.s(phy == 0 ? "dprx" : "lttpr"); if (phy) l.d(phy);
    l.s(" iter "); l.d(iter); l.s(" status");
    for (int i = 0; i < 6; i++) { l.s(" "); l.x(st[i], 2); }
    l.s(" | lanes cr/eq/lock:");
    for (int lane = 0; lane < t.p->lanes; lane++) {
        uint8_t s = lane_status(st, lane);
        l.s(" "); l.d(s & 1); l.s("/"); l.d((s >> 1) & 1); l.s("/"); l.d((s >> 2) & 1);
    }
    l.s(" align="); l.d(st[2] & 1);
    l.s(" | train_set");
    for (int lane = 0; lane < t.p->lanes; lane++) { l.s(" "); l.x(t.train_set[lane], 2); }
    logl(t, l);
}

// intel_dp_training_pattern for 8b/10b: tps4 when source (display >= 10)
// and sink support it (lttprs always), else tps3, else tps2 (satoru)
uint8_t eq_pattern(const St& t, int phy) {
    const uint8_t* d = t.p->dpcd;
    bool sink_tps4 = phy != 0 || (d[0] >= 0x14 && (d[3] & 0x80));
    if (sink_tps4) return TP4;
    bool sink_tps3 = phy != 0 || (d[0] >= 0x12 && (d[2] & 0x40));
    if (sink_tps3) return TP3;
    return TP2;
}

bool clock_recovery(St& t, int phy) {
    // intel_dp_link_training_clock_recovery (satoru)
    uint8_t old_st[6] = {}, st[6] = {};
    uint32_t delay = phy == 0 ? IgpuCalc::CrDelayUs(t.p->dpcd[0], t.p->dpcd[0x0e], false)
                              : IgpuCalc::CrDelayUs(0, 0, true);
    if (!reset_link_train(t, phy, TP1 | SCRAMBLING_DISABLE)) { t.r->reason = "cr: enable training failed"; return false; }
    int max_cr = t.p->dpcd[0] >= 0x14 ? 10 : 80;
    int voltage_tries = 1;
    bool max_vswing = false;
    for (int tries = 0; tries < max_cr; tries++) {
        t.o->delay_us(t.o->ctx, delay);
        if (!read_status(t, phy, st)) { t.r->reason = "cr: link status read failed"; return false; }
        t.r->cr_loops++;
        for (int i = 0; i < 6; i++) t.r->status[i] = st[i];
        log_iter(t, "cr", phy, tries, st);
        if (ClockRecoveryOk(st, t.p->lanes)) return true;
        if (voltage_tries == 5) { t.r->reason = "cr: same voltage tried 5 times"; return false; }
        if (max_vswing) { t.r->reason = "cr: max voltage swing reached"; return false; }
        get_adjust_train(t, phy, st);
        if (!update_link_train(t, phy)) { t.r->reason = "cr: update link training failed"; return false; }
        if (!adjust_changed(t, old_st, st)) ++voltage_tries; else voltage_tries = 1;
        for (int i = 0; i < 6; i++) old_st[i] = st[i];
        if (max_vswing_reached(t)) max_vswing = true;
    }
    t.r->reason = "cr: retries exhausted";
    return false;
}

bool channel_eq(St& t, int phy) {
    // intel_dp_link_training_channel_equalization (satoru)
    uint8_t st[6] = {};
    uint32_t delay = phy == 0 ? IgpuCalc::EqDelayUs(t.p->dpcd[0x0e]) : IgpuCalc::EqDelayUs(t.p->lttpr_phy_caps[phy - 1][0]);
    uint8_t pat = eq_pattern(t, phy);
    t.r->pattern = pat;
    if (pat != TP4) pat |= SCRAMBLING_DISABLE;     // tps4 is scrambled (satoru)
    if (!set_link_train(t, phy, pat)) { t.r->reason = "eq: start failed"; return false; }
    for (int tries = 0; tries < 5; tries++) {
        t.o->delay_us(t.o->ctx, delay);
        if (!read_status(t, phy, st)) { t.r->reason = "eq: link status read failed"; return false; }
        t.r->eq_loops++;
        for (int i = 0; i < 6; i++) t.r->status[i] = st[i];
        log_iter(t, "eq", phy, tries, st);
        if (!ClockRecoveryOk(st, t.p->lanes)) { t.r->reason = "eq: clock recovery lost"; return false; }
        if (ChannelEqOk(st, t.p->lanes)) return true;
        get_adjust_train(t, phy, st);
        if (!update_link_train(t, phy)) { t.r->reason = "eq: update link training failed"; return false; }
    }
    t.r->reason = "eq: failed 5 times";
    return false;
}

bool train_phy(St& t, int phy) {
    // intel_dp_link_train_phy (satoru)
    if (!clock_recovery(t, phy)) return false;
    return channel_eq(t, phy);
}

void disable_tp(St& t, int phy) {
    // intel_dp_disable_dpcd_training_pattern (satoru)
    uint8_t v = TP_DISABLE;
    (void)t.o->dpcd_write(t.o->ctx, tp_set_reg(phy), &v, 1);
}

} // namespace

bool ClockRecoveryOk(const uint8_t st[6], int lanes) {
    for (int lane = 0; lane < lanes; lane++) if (!(lane_status(st, lane) & LANE_CR_DONE)) return false;
    return true;
}

bool ChannelEqOk(const uint8_t st[6], int lanes) {
    if (!(st[2] & INTERLANE_ALIGN_DONE)) return false;
    for (int lane = 0; lane < lanes; lane++)
        if ((lane_status(st, lane) & CHANNEL_EQ_BITS) != CHANNEL_EQ_BITS) return false;
    return true;
}

bool PrepareSink(const Ops& o, const Params& p) {
    // intel_dp_update_downspread_ctrl -> intel_dp_link_training_set_mode:
    // 0x107 = 0 (no vrr msa ignore, no pr as sdp), 0x108 = set_ansi_8b10b.
    // then intel_dp_link_training_set_bw: 0x100 link bw code, 0x101 lane
    // count | enhanced frame. post_lt_adj_req_granted stays clear: with tps4
    // linux does not use it either, and with tps2/3 we skip the post lt
    // phase, so we must not grant it. (satoru)
    uint8_t mode[2] = { 0x00, DP_SET_ANSI_8B10B };
    if (o.dpcd_write(o.ctx, DP_DOWNSPREAD_CTRL, mode, 2) != 2) return false;
    uint8_t bw[2] = { IgpuCalc::RateToBwCode(p.rate_khz),
                      (uint8_t)(p.lanes | (p.enhanced_framing ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0)) };
    return o.dpcd_write(o.ctx, DP_LINK_BW_SET, bw, 2) == 2;
}

bool TrainAllPhys(const Ops& o, const Params& p, Result* r) {
    St t;
    t.o = &o;
    t.p = &p;
    t.r = r;
    for (int i = 0; i < 4; i++) t.train_set[i] = 0;
    *r = Result();
    r->phy_failed = -1;
    r->reason = "ok";
    bool ret = true;
    // intel_dp_link_train_all_phys: lttprs from the one next to the source
    // (dp_phy_lttpr(count - 1)) down to lttpr1, then the dprx (satoru)
    for (int i = p.lttpr_count - 1; i >= 0; i--) {
        int phy = i + 1;
        ret = train_phy(t, phy);
        disable_tp(t, phy);
        if (!ret) { r->phy_failed = phy; break; }
    }
    if (ret) {
        ret = train_phy(t, 0);
        if (!ret) r->phy_failed = 0;
    }
    disable_tp(t, 0);
    o.set_source_idle(o.ctx);
    for (int i = 0; i < 4; i++) r->train_set[i] = t.train_set[i];
    r->ok = ret;
    {
        Ln l; l.s("link training "); l.s(ret ? "passed" : "failed"); l.s(" at ");
        l.d((int)(p.rate_khz / 1000)); l.s(" mhz x"); l.d(p.lanes); l.s(" (cr loops "); l.d(r->cr_loops);
        l.s(", eq loops "); l.d(r->eq_loops); l.s(", eq pattern tps"); l.d(r->pattern == TP4 ? 4 : r->pattern);
        l.s(", lttprs "); l.d(p.lttpr_count); l.s(")"); if (!ret) { l.s(": "); l.s(r->reason); }
        logl(t, l);
    }
    return ret;
}

}

// end (satoru)
