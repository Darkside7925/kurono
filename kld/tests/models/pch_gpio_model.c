// pch_gpio_model: behavioural Intel PCH GPIO communities at the MMIO seam.
// (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"
#include "pch_gpio_model.h"

// register offsets inside one community (pinctrl-intel.c / MTP defines). (satoru)
#define REVID		0x000
#define CAPLIST		0x004
#define PADBAR		0x00C
#define PAD_OWN		0x0B0
#define PADCFGLOCK	0x110
#define HOSTSW_OWN	0x150
#define GPI_IS		0x200
#define GPI_IE		0x220

#define PADCFG0			0x000
#define PADCFG0_RXEVCFG_MASK	(3u << 25)
#define PADCFG0_RXEVCFG_LEVEL	(0u << 25)
#define PADCFG0_RXEVCFG_EDGE	(1u << 25)
#define PADCFG0_RXEVCFG_DIS	(2u << 25)
#define PADCFG0_RXEVCFG_BOTH	(3u << 25)
#define PADCFG0_RXINV		(1u << 23)
#define PADCFG0_GPIORXDIS	(1u << 9)
#define PADCFG0_GPIOTXDIS	(1u << 8)
#define PADCFG0_GPIORXSTATE	(1u << 1)

#define REVID_VALUE		0x00920000u
#define PADBAR_VALUE		0x00000800u
#define PAD_REGS		4
#define PAD_STRIDE		(PAD_REGS * 4)

// the driver's own community tables (pinctrl-meteorpoint.c, mtps_*). (satoru)
struct pch_grp {
    unsigned short base, size;
    unsigned char reg_num;
};

static const struct pch_grp pch_groups[5][6] = {
    { { 0, 24, 0 }, { 25, 38, 1 }, { 39, 56, 2 }, { 57, 87, 3 } },
    { { 88, 102, 0 }, { 103, 114, 1 }, { 115, 136, 2 } },
    { { 137, 145, 0 }, { 146, 169, 1 }, { 170, 189, 2 }, { 190, 193, 3 },
      { 194, 201, 4 }, { 202, 232, 5 } },
    { { 233, 240, 0 }, { 241, 263, 1 }, { 264, 277, 2 }, { 278, 301, 3 } },
    { { 302, 322, 0 }, { 323, 338, 1 } },
};
static const unsigned char pch_ngroups[5] = { 4, 3, 6, 4, 2 };
static const unsigned short pch_pin_base[5] = { 0, 88, 137, 233, 302 };
static const unsigned short pch_npins[5] = { 88, 49, 96, 69, 37 };

struct pch_pad {
    u32 cfg[PAD_REGS];
    int level;
    int last_internal;
};

struct pch_grp_state {
    u32 is, ie;
};

struct pch_comm {
    int used;
    u64 base;
    unsigned long len;
    int idx;
    const struct pch_grp *grps;
    int ngroups;
    int pin_base, npins;
    struct pch_pad *pads;
    struct pch_grp_state groups[8];
    u32 hostown[8];
    u32 padcfg_lock[8][2];
    u32 padown[32];
    int irq;
    int line;
    unsigned long raises;
};

static struct pch_comm pch_comms[PCH_GPIO_MAX_COMMUNITIES];

static struct pch_comm *pch_find_comm(int pin)
{
    for (int i = 0; i < PCH_GPIO_MAX_COMMUNITIES; i++) {
        struct pch_comm *c = &pch_comms[i];

        if (c->used && pin >= c->pin_base && pin < c->pin_base + c->npins)
            return c;
    }
    return NULL;
}

static int pch_group_of(const struct pch_comm *c, int pin)
{
    for (int i = 0; i < c->ngroups; i++)
        if (pin >= c->grps[i].base &&
            pin < c->grps[i].base + c->grps[i].size)
            return i;
    return -1;
}

static void pch_update_line(struct pch_comm *c)
{
    int any = 0;

    for (int i = 0; i < c->ngroups; i++)
        if (c->groups[i].is & c->groups[i].ie)
            any = 1;
    if (any && !c->line) {
        c->line = 1;
        if (c->irq >= 0) {
            lx_irq_raise_soft((unsigned int)c->irq);
            c->raises++;
        }
    } else if (!any) {
        c->line = 0;
    }
}

static void pch_eval_pad(struct pch_comm *c, int pad_idx)
{
    struct pch_pad *p = &c->pads[pad_idx];
    int pin = c->pin_base + pad_idx;
    int gi = pch_group_of(c, pin);
    u32 cfg = p->cfg[0];
    int internal, last, ev;

    if (gi < 0)
        return;
    internal = (p->level ^ (!!(cfg & PADCFG0_RXINV))) ? 1 : 0;
    last = p->last_internal;
    p->last_internal = internal;

    switch (cfg & PADCFG0_RXEVCFG_MASK) {
    case PADCFG0_RXEVCFG_LEVEL:
        if (internal)
            c->groups[gi].is |= 1u << (pin - c->grps[gi].base);
        else
            c->groups[gi].is &= ~(1u << (pin - c->grps[gi].base));
        break;
    case PADCFG0_RXEVCFG_EDGE:
    case PADCFG0_RXEVCFG_BOTH:
        ev = internal != last;
        if (ev)
            c->groups[gi].is |= 1u << (pin - c->grps[gi].base);
        break;
    default:
        break;
    }
    pch_update_line(c);
}

static u64 pch_read(void *ctx, unsigned long off, int size)
{
    struct pch_comm *c = ctx;

    (void)size;
    switch (off) {
    case REVID: return REVID_VALUE;
    case CAPLIST: return 0;
    case PADBAR: return PADBAR_VALUE;
    }
    if (off >= PAD_OWN && off < PAD_OWN + sizeof(c->padown))
        return c->padown[(off - PAD_OWN) / 4];
    if (off >= PADCFGLOCK && off - PADCFGLOCK < 8 * 2 * 4) {
        unsigned int r = (unsigned int)(off - PADCFGLOCK);

        return c->padcfg_lock[r / 8][(r % 8) / 4];
    }
    if (off >= HOSTSW_OWN && off - HOSTSW_OWN < 8 * 4)
        return c->hostown[(off - HOSTSW_OWN) / 4];
    if (off >= GPI_IS && off - GPI_IS < 8 * 4)
        return c->groups[(off - GPI_IS) / 4].is;
    if (off >= GPI_IE && off - GPI_IE < 8 * 4)
        return c->groups[(off - GPI_IE) / 4].ie;
    if (off >= PADBAR_VALUE &&
        off - PADBAR_VALUE < (unsigned long)c->npins * PAD_STRIDE) {
        unsigned long rel = off - PADBAR_VALUE;
        int pad = (int)(rel / PAD_STRIDE);
        int reg = (int)((rel % PAD_STRIDE) / 4);

        if (reg == 0)
            return (c->pads[pad].cfg[0] & ~PADCFG0_GPIORXSTATE) |
                   (c->pads[pad].level ? PADCFG0_GPIORXSTATE : 0);
        return c->pads[pad].cfg[reg];
    }
    return 0;
}

static void pch_write(void *ctx, unsigned long off, int size, u64 val)
{
    struct pch_comm *c = ctx;

    (void)size;
    if (off >= PAD_OWN && off < PAD_OWN + sizeof(c->padown)) {
        c->padown[(off - PAD_OWN) / 4] = (u32)val;
        return;
    }
    if (off >= PADCFGLOCK && off - PADCFGLOCK < 8 * 2 * 4) {
        unsigned int r = (unsigned int)(off - PADCFGLOCK);

        c->padcfg_lock[r / 8][(r % 8) / 4] = (u32)val;
        return;
    }
    if (off >= HOSTSW_OWN && off - HOSTSW_OWN < 8 * 4) {
        c->hostown[(off - HOSTSW_OWN) / 4] = (u32)val;
        return;
    }
    if (off >= GPI_IS && off - GPI_IS < 8 * 4) {
        // write 1 to clear
        c->groups[(off - GPI_IS) / 4].is &= ~(u32)val;
        pch_update_line(c);
        return;
    }
    if (off >= GPI_IE && off - GPI_IE < 8 * 4) {
        c->groups[(off - GPI_IE) / 4].ie = (u32)val;
        pch_update_line(c);
        return;
    }
    if (off >= PADBAR_VALUE &&
        off - PADBAR_VALUE < (unsigned long)c->npins * PAD_STRIDE) {
        unsigned long rel = off - PADBAR_VALUE;
        int pad = (int)(rel / PAD_STRIDE);
        int reg = (int)((rel % PAD_STRIDE) / 4);

        if (reg == 0) {
            // RX state is read-only; everything else is configuration. (satoru)
            c->pads[pad].cfg[0] = (u32)val & ~PADCFG0_GPIORXSTATE;
            pch_eval_pad(c, pad);
        } else {
            c->pads[pad].cfg[reg] = (u32)val;
        }
    }
}

void pch_gpio_model_init(int idx, u64 base, unsigned long len)
{
    struct pch_comm *c;

    if (idx < 0 || idx >= 5 || idx >= PCH_GPIO_MAX_COMMUNITIES)
        return;
    c = &pch_comms[idx];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->base = base;
    c->len = len;
    c->idx = idx;
    c->grps = pch_groups[idx];
    c->ngroups = pch_ngroups[idx];
    c->pin_base = pch_pin_base[idx];
    c->npins = pch_npins[idx];
    c->irq = -1;
    c->pads = kzalloc(sizeof(*c->pads) * (size_t)c->npins, GFP_KERNEL);
    if (!c->pads) {
        c->used = 0;
        return;
    }
    for (int i = 0; i < c->npins; i++) {
        c->pads[i].level = 1;           // idle high (the pad's pull, assumed)
        c->pads[i].last_internal = 0;   // first eval treats a change as an edge
    }
    for (int i = 0; i < c->ngroups; i++) {
        c->hostown[i] = 0xffffffffu;    // host owns every pad by default
        c->groups[i].is = c->groups[i].ie = 0;
    }
    // measured on this host: GPP_I_8 (pin 310) has PADCFG0 0x80800102 and
    // its TX lock set; everything else is the assumed reset value. (satoru)
    if (idx == 4) {
        int pad = 310 - c->pin_base;
        int gi = pch_group_of(c, 310), off = 310 - c->grps[gi].base;

        c->pads[pad].cfg[0] = 0x80800102u;
        c->pads[pad].level = !!(c->pads[pad].cfg[0] & PADCFG0_GPIORXSTATE);
        c->padcfg_lock[gi][1] = 1u << off;     // PADCFGLOCKTX
        // GPP_I_9 (pin 311) is left in ACPI mode on purpose: the driver must
        // refuse to use it as an IRQ. (satoru)
        c->hostown[gi] &= ~(1u << (311 - c->grps[gi].base));
    }
    if (lx_mmio_sim_claim((void *)(unsigned long)base, len,
                          pch_read, pch_write, c))
        c->used = 0;
}

void pch_gpio_model_set_irq(int irq)
{
    for (int i = 0; i < PCH_GPIO_MAX_COMMUNITIES; i++)
        if (pch_comms[i].used)
            pch_comms[i].irq = irq;
}

void pch_gpio_model_set_level(int pin, int level)
{
    struct pch_comm *c = pch_find_comm(pin);

    if (!c)
        return;
    c->pads[pin - c->pin_base].level = level ? 1 : 0;
    pch_eval_pad(c, pin - c->pin_base);
}

void pch_gpio_model_set_ie(int pin, int on)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return;
    gi = pch_group_of(c, pin);
    if (gi < 0)
        return;
    if (on)
        c->groups[gi].ie |= 1u << (pin - c->grps[gi].base);
    else
        c->groups[gi].ie &= ~(1u << (pin - c->grps[gi].base));
    pch_update_line(c);
}

void pch_gpio_model_ack(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return;
    gi = pch_group_of(c, pin);
    if (gi < 0)
        return;
    c->groups[gi].is &= ~(1u << (pin - c->grps[gi].base));
    pch_update_line(c);
}

int pch_gpio_model_get_level(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);

    return c ? c->pads[pin - c->pin_base].level : -1;
}

unsigned int pch_gpio_model_padcfg0(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);

    if (!c)
        return 0;
    return (c->pads[pin - c->pin_base].cfg[0] & ~PADCFG0_GPIORXSTATE) |
           (c->pads[pin - c->pin_base].level ? PADCFG0_GPIORXSTATE : 0);
}

int pch_gpio_model_is(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return 0;
    gi = pch_group_of(c, pin);
    return gi < 0 ? 0 : !!(c->groups[gi].is & (1u << (pin - c->grps[gi].base)));
}

int pch_gpio_model_ie(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return 0;
    gi = pch_group_of(c, pin);
    return gi < 0 ? 0 : !!(c->groups[gi].ie & (1u << (pin - c->grps[gi].base)));
}

int pch_gpio_model_hostown(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return 0;
    gi = pch_group_of(c, pin);
    return gi < 0 ? 0 : !!(c->hostown[gi] & (1u << (pin - c->grps[gi].base)));
}

int pch_gpio_model_locked(int pin)
{
    struct pch_comm *c = pch_find_comm(pin);
    int gi;

    if (!c)
        return 0;
    gi = pch_group_of(c, pin);
    if (gi < 0)
        return 0;
    return !!(c->padcfg_lock[gi][0] & (1u << (pin - c->grps[gi].base)));
}

unsigned long pch_gpio_model_irqs(void)
{
    unsigned long n = 0;

    for (int i = 0; i < PCH_GPIO_MAX_COMMUNITIES; i++)
        n += pch_comms[i].raises;
    return n;
}

// end (satoru)
