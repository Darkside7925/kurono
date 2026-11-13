// i915 register model group 4 (d4.1): DP AUX channels with a sink model.
// a write to XELPDP_DP_AUX_CH_CTL with SEND_BUSY runs the request packed in
// the data registers against the sink on that channel and leaves the reply
// in the data registers, DONE and the reply size in the control register.
// sinks: an eDP panel on AUX A (variant edp) and a DP monitor on AUX USBC1
// (variants edp, tc); every other channel times out like an empty port.
// each sink has a DPCD (receiver caps, link status that reports a trained
// link, eDP backlight caps) and an EDID behind i2c address 0x50.
// what this does NOT prove: link training against a real receiver. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"

u32 i915m_peek(unsigned long off);
void i915m_poke(unsigned long off, u32 v);

#define AUX_BUSY     (1u << 31)
#define AUX_DONE     (1u << 30)
#define AUX_IRQ      (1u << 29)
#define AUX_TIMEOUT  (1u << 28)
#define AUX_RXERR    (1u << 25)
#define AUX_SIZE_SH  20
#define AUX_SIZE_MSK (0x1fu << AUX_SIZE_SH)

struct sink {
    int present;
    u8 dpcd[0x800];
    u8 edid[128];
    u8 i2c_off;
};

static struct sink g_edp, g_dp;
static unsigned int g_aux_logged;

// ── edid: one detailed timing, a name, checksum (edid 1.4, digital dp) ─────
struct dtd { u32 khz; u16 ha, hb, hf, hs, va, vb, vf, vs, wmm, hmm; };

static void edid_build(u8 *e, const struct dtd *t, const char *name, u16 product)
{
    static const u8 hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    u8 *d = e + 54;
    int i, sum = 0;
    memset(e, 0, 128);
    memcpy(e, hdr, 8);
    e[8] = 0x2e; e[9] = 0xb2;              // manufacturer "KUR" (satoru)
    e[10] = (u8)product; e[11] = (u8)(product >> 8);
    e[16] = 1; e[17] = 34;                 // week 1, 2024 (satoru)
    e[18] = 1; e[19] = 4;                  // edid 1.4
    e[20] = 0xa5;                          // digital, 8 bpc, displayport
    e[21] = (u8)(t->wmm / 10); e[22] = (u8)(t->hmm / 10);
    e[23] = 0x78;                          // gamma 2.2
    e[24] = 0x0a;                          // rgb, preferred timing is native
    for (i = 38; i < 54; i++) e[i] = 0x01; // no standard timings
    d[0] = (u8)(t->khz / 10); d[1] = (u8)((t->khz / 10) >> 8);
    d[2] = (u8)t->ha; d[3] = (u8)t->hb; d[4] = (u8)(((t->ha >> 8) << 4) | (t->hb >> 8));
    d[5] = (u8)t->va; d[6] = (u8)t->vb; d[7] = (u8)(((t->va >> 8) << 4) | (t->vb >> 8));
    d[8] = (u8)t->hf; d[9] = (u8)t->hs;
    d[10] = (u8)(((t->vf & 0xf) << 4) | (t->vs & 0xf));
    d[11] = (u8)(((t->hf >> 8) << 6) | ((t->hs >> 8) << 4) | ((t->vf >> 4) << 2) | (t->vs >> 4));
    d[12] = (u8)t->wmm; d[13] = (u8)t->hmm; d[14] = (u8)(((t->wmm >> 8) << 4) | (t->hmm >> 8));
    d[17] = 0x1a;                          // digital separate, +hsync -vsync
    d = e + 72;                            // display name descriptor
    d[3] = 0xfc;
    for (i = 0; i < 13; i++) d[5 + i] = (u8)(name[i] ? name[i] : '\n');
    for (i = 90; i < 126; i += 18) e[i + 3] = 0x10;   // dummy descriptors
    for (i = 0; i < 127; i++) sum += e[i];
    e[127] = (u8)(0x100 - (sum & 0xff));
}

static void dpcd_common(struct sink *s)
{
    memset(s->dpcd, 0, sizeof(s->dpcd));
    s->dpcd[0x000] = 0x14;   // dpcd 1.4 (satoru)
    s->dpcd[0x001] = 0x14;   // hbr2
    s->dpcd[0x002] = 0x84;   // 4 lanes, enhanced framing
    s->dpcd[0x003] = 0x01;   // downspread
    s->dpcd[0x004] = 0x01;
    s->dpcd[0x006] = 0x01;   // 8b/10b
    s->dpcd[0x200] = 0x01;   // sink count 1
    s->present = 1;
}

void i915_model_aux_init(const char *variant)
{
    static const struct dtd panel = { 268500, 2560, 160, 48, 32, 1600, 46, 3, 6, 344, 215 };
    static const struct dtd mon = { 148500, 1920, 280, 88, 44, 1080, 45, 4, 5, 527, 296 };
    int edp = variant && variant[0] == 'e';
    int tc = variant && (variant[0] == 'e' || (variant[0] == 't' && variant[1] == 'c' && !variant[2]));
    memset(&g_edp, 0, sizeof(g_edp));
    memset(&g_dp, 0, sizeof(g_dp));
    if (edp) {
        dpcd_common(&g_edp);
        g_edp.dpcd[0x00d] = 0x01;   // edp: alternate scrambler reset (satoru)
        g_edp.dpcd[0x700] = 0x03;   // edp 1.4
        g_edp.dpcd[0x701] = 0x01;   // tcon backlight adjustment
        g_edp.dpcd[0x702] = 0x02;   // brightness set over aux
        g_edp.dpcd[0x724] = 0x08;   // pwmgen bit count 8 (max 255) and its caps (satoru)
        g_edp.dpcd[0x725] = 0x08;
        g_edp.dpcd[0x726] = 0x08;
        edid_build(g_edp.edid, &panel, "KURONO PANEL", 0x0001);
    }
    if (tc) {
        dpcd_common(&g_dp);
        edid_build(g_dp.edid, &mon, "KURONO DP", 0x0002);
    }
}

// which sink answers on an aux control register. (satoru)
static struct sink *sink_of(unsigned long ctl, const char **name)
{
    if (ctl == 0x64010) { *name = "A"; return g_edp.present ? &g_edp : NULL; }
    if (ctl == 0x16f210) { *name = "USBC1"; return g_dp.present ? &g_dp : NULL; }
    *name = "?";
    return NULL;
}

static u8 dpcd_read(struct sink *s, u32 a)
{
    if (a >= 0x202 && a <= 0x203) return 0x77;   // cr, eq, symbol lock, 4 lanes
    if (a == 0x204) return 0x01;                 // interlane align done
    if (a == 0x205 || a == 0x206 || a == 0x207) return 0x00;
    return a < sizeof(s->dpcd) ? s->dpcd[a] : 0;
}

static void aux_log(const char *ch, u8 cmd, u32 a, int len, const char *res)
{
    char b[96];
    if (g_aux_logged++ >= 600)
        return;
    snprintf(b, sizeof(b), "[i915m] aux %s cmd=%x addr=%x len=%d %s\n", ch, cmd, a, len, res);
    lx_log(b);
}

// returns the control value to store. (satoru)
u32 i915_model_aux_xfer(unsigned long ctl, u32 v)
{
    u8 tx[20], rx[20];
    int tx_n = (int)((v & AUX_SIZE_MSK) >> AUX_SIZE_SH), rx_n = 0, i;
    const char *ch;
    struct sink *s = sink_of(ctl, &ch);
    u32 keep = v & ~(AUX_BUSY | AUX_DONE | AUX_IRQ | AUX_TIMEOUT | AUX_RXERR | AUX_SIZE_MSK);
    u8 cmd;
    u32 a;
    int len;

    if (tx_n > 20) tx_n = 20;
    for (i = 0; i < tx_n; i++)
        tx[i] = (u8)(i915m_peek(ctl + 4 + (i & ~3)) >> ((3 - (i & 3)) * 8));
    cmd = tx[0] >> 4;
    a = ((u32)(tx[0] & 0xf) << 16) | ((u32)tx[1] << 8) | tx[2];
    len = tx_n > 3 ? tx[3] + 1 : 0;
    if (!s) {
        aux_log(ch, cmd, a, len, "timeout (no sink)");
        return keep | AUX_DONE | AUX_TIMEOUT;
    }
    rx[0] = 0x00;   // ack (satoru)
    rx_n = 1;
    if (cmd == 0x8) {                         // native write
        for (i = 0; i < len && 4 + i < tx_n; i++)
            if (a + i < sizeof(s->dpcd)) s->dpcd[a + i] = tx[4 + i];
    } else if (cmd == 0x9) {                  // native read
        for (i = 0; i < len && rx_n < 20; i++)
            rx[rx_n++] = dpcd_read(s, a + i);
    } else if ((cmd & 0x8) == 0) {            // i2c over aux (mot = bit 2)
        int rd = cmd & 1, status = (cmd & 3) == 2;
        if (a != 0x50 && a != 0x30) {
            rx[0] = 0x40;                     // i2c nack (satoru)
        } else if (status || len == 0) {
            ;                                 // address only / status: ack
        } else if (rd && a == 0x50) {
            for (i = 0; i < len && rx_n < 20; i++)
                rx[rx_n++] = s->edid[(u8)(s->i2c_off + i) & 0x7f];
            s->i2c_off = (u8)(s->i2c_off + len);
        } else if (!rd && a == 0x50 && tx_n > 4) {
            s->i2c_off = tx[4];
        }
    }
    for (i = 0; i < 5; i++) {
        u32 w = 0;
        for (int k = 0; k < 4; k++)
            if (i * 4 + k < rx_n) w |= (u32)rx[i * 4 + k] << ((3 - k) * 8);
        i915m_poke(ctl + 4 + i * 4, w);
    }
    aux_log(ch, cmd, a, len, rx[0] ? "nack" : "ack");
    return keep | AUX_DONE | ((u32)rx_n << AUX_SIZE_SH);
}

// end (satoru)
