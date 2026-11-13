// lx_regmap: the MMIO regmap the DesignWare I2C driver asks for.
//
// the driver supplies its own reg_read/reg_write callbacks (including the
// byte-swapped variants), so this file only stores the config and dispatches.
// no cache, no locking: the container talks to one modelled controller. (satoru)

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "lx_kshim.h"

struct lx_regmap {
    struct regmap_config cfg_copy;
    const struct regmap_config *cfg;
    void *context;
};

static struct lx_regmap g_maps[4];
static int g_nmaps;

struct regmap *__devm_regmap_init(struct device *dev, const struct regmap_bus *bus,
                                  void *bus_context,
                                  const struct regmap_config *config,
                                  struct lock_class_key *lock_key,
                                  const char *lock_name)
{
    (void)dev; (void)bus; (void)lock_key; (void)lock_name;
    if (!config || g_nmaps >= 4)
        return ERR_PTR(-ENOMEM);
    struct lx_regmap *m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return ERR_PTR(-ENOMEM);
    m->cfg_copy = *config;
    m->cfg = &m->cfg_copy;
    m->context = bus_context;
    g_maps[g_nmaps].cfg = &m->cfg_copy;
    g_maps[g_nmaps].context = bus_context;
    g_nmaps++;
    return (struct regmap *)m;
}

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
    (void)dev; (void)name;
    return g_nmaps ? (struct regmap *)&g_maps[0] : NULL;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
    struct lx_regmap *m = (struct lx_regmap *)map;
    if (!m || !m->cfg || !m->cfg->reg_read)
        return -EINVAL;
    return m->cfg->reg_read(m->context, reg, val);
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
    struct lx_regmap *m = (struct lx_regmap *)map;
    if (!m || !m->cfg || !m->cfg->reg_write)
        return -EINVAL;
    return m->cfg->reg_write(m->context, reg, val);
}

int regmap_update_bits_base(struct regmap *map, unsigned int reg,
                            unsigned int mask, unsigned int val,
                            bool *change, bool async, bool force)
{
    (void)async; (void)force;
    unsigned int old = 0, newv;
    int rc = regmap_read(map, reg, &old);
    if (rc)
        return rc;
    newv = (old & ~mask) | (val & mask);
    if (newv == old) {
        if (change) *change = false;
        return 0;
    }
    if (change) *change = true;
    return regmap_write(map, reg, newv);
}

// end (satoru)
