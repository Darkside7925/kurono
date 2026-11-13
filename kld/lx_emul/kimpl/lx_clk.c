// lx_clk: the fixed-rate clock provider LPSS registers and the designware
// driver consumes. a container has no clock tree; every clock reports a fixed
// rate and enable/prepare succeed. the objects are real so the probe path never
// reaches a stub. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/slab.h>
#include <linux/err.h>

#include "lx_kshim.h"

struct lx_clk {
    char name[64];
    unsigned long rate;
};

static struct lx_clk g_fixed_default = { .name = "lx-fixed", .rate = 100000000ul };

static struct clk *lx_clk_new(const char *name, unsigned long rate)
{
    struct lx_clk *c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c)
        return ERR_PTR(-ENOMEM);
    if (name)
        strscpy(c->name, name, sizeof(c->name));
    c->rate = rate;
    return (struct clk *)c;
}

struct clk *clk_register_fixed_rate(struct device *dev, const char *name,
                                    const char *parent_name, unsigned long flags,
                                    unsigned long fixed_rate)
{
    (void)dev; (void)parent_name; (void)flags;
    return lx_clk_new(name, fixed_rate);
}

struct clk *clk_register_gate(struct device *dev, const char *name,
                              const char *parent_name, unsigned long flags,
                              void __iomem *reg, u8 bit_idx, u8 clk_gate_flags,
                              spinlock_t *lock)
{
    (void)dev; (void)parent_name; (void)flags; (void)reg; (void)bit_idx;
    (void)clk_gate_flags; (void)lock;
    return lx_clk_new(name, 0);
}

struct clk *clk_register_fractional_divider(struct device *dev,
        const char *name, const char *parent_name, unsigned long flags,
        void __iomem *reg, u8 mshift, u8 mwidth, u8 nshift, u8 nwidth,
        u8 clk_divider_flags, spinlock_t *lock)
{
    (void)dev; (void)parent_name; (void)flags; (void)reg; (void)mshift;
    (void)mwidth; (void)nshift; (void)nwidth; (void)clk_divider_flags; (void)lock;
    return lx_clk_new(name, 0);
}

void clk_unregister(struct clk *clk)
{
    if (clk && clk != (struct clk *)&g_fixed_default)
        kfree(clk);
}

const char *__clk_get_name(const struct clk *clk)
{
    const struct lx_clk *c = (const struct lx_clk *)clk;
    return c && c->name[0] ? c->name : "lx-clk";
}

unsigned long clk_get_rate(struct clk *clk)
{
    const struct lx_clk *c = (const struct lx_clk *)clk;
    return c ? c->rate : 0;
}

int clk_set_rate(struct clk *clk, unsigned long rate)
{
    struct lx_clk *c = (struct lx_clk *)clk;
    if (c)
        c->rate = rate;
    return 0;
}

struct clk *clk_get_parent(struct clk *clk) { (void)clk; return NULL; }
int clk_prepare(struct clk *clk) { (void)clk; return 0; }
void clk_unprepare(struct clk *clk) { (void)clk; }
int clk_enable(struct clk *clk) { (void)clk; return 0; }
void clk_disable(struct clk *clk) { (void)clk; }
void clk_put(struct clk *clk) { (void)clk; }

struct clk *clk_get(struct device *dev, const char *id)
{
    (void)dev; (void)id;
    return (struct clk *)&g_fixed_default;
}

struct clk *devm_clk_get(struct device *dev, const char *id)
{
    (void)dev; (void)id;
    return (struct clk *)&g_fixed_default;
}

struct clk *devm_clk_get_optional(struct device *dev, const char *id)
{
    (void)dev; (void)id;
    return (struct clk *)&g_fixed_default;
}

struct clk_lookup *clkdev_create(struct clk *clk, const char *con_id,
                                 const char *dev_fmt, ...)
{
    (void)clk; (void)con_id; (void)dev_fmt;
    return kzalloc(sizeof(struct clk_lookup), GFP_KERNEL);
}

struct clk_lookup *clkdev_hw_create(struct clk_hw *hw, const char *con_id,
                                    const char *dev_fmt, ...)
{
    (void)hw; (void)con_id; (void)dev_fmt;
    return kzalloc(sizeof(struct clk_lookup), GFP_KERNEL);
}

void clkdev_add(struct clk_lookup *cl) { (void)cl; }
void clkdev_drop(struct clk_lookup *cl) { kfree(cl); }

// end (satoru)
