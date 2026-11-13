// lx_irqdomain: the small irq-domain/irq-desc core upstream irq_sim and
// gpiolib need. a container has no real interrupt controller: a sim virq is
// one of lx_irq.c's software slots, and a "fire" is one raise of that slot.
// the irq_sim chip, gpiolib and i2c-hid stay unmodified; this file only supplies
// the mapping, irq_data, and flow-handler plumbing they call. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/gpio/driver.h>
#include <linux/slab.h>

#include "lx_kshim.h"

// gpiolib is only linked into the families that carry a gpio chip; the others
// must still link this irq domain. (satoru)
int gpiochip_lock_as_irq(struct gpio_chip *gc, unsigned int offset)
    __attribute__((weak));
void gpiochip_unlock_as_irq(struct gpio_chip *gc, unsigned int offset)
    __attribute__((weak));

#define LX_SIM_MAX 16

struct lx_sim_desc {
    struct irq_data data;
    struct irq_common_data common;
};

static struct lx_sim_desc *lx_sim_descs[LX_SIM_MAX];
static unsigned int lx_sim_virq[LX_SIM_MAX];

static struct lx_sim_desc *lx_sim_get(unsigned int virq)
{
    for (int i = 0; i < LX_SIM_MAX; i++)
        if (lx_sim_virq[i] == virq)
            return lx_sim_descs[i];
    return NULL;
}

struct irq_domain *irq_domain_instantiate(const struct irq_domain_info *info)
{
    unsigned int size = info && info->size ? info->size : 1;
    struct irq_domain *d;

    d = kzalloc(sizeof(*d) + (size_t)size * sizeof(d->revmap[0]), GFP_KERNEL);
    if (!d)
        return ERR_PTR(-ENOMEM);
    INIT_LIST_HEAD(&d->link);
    mutex_init(&d->mutex);
    d->ops = info->ops;
    d->host_data = info->host_data;
    d->fwnode = info->fwnode;
    d->hwirq_max = info->hwirq_max ? info->hwirq_max : size;
    d->revmap_size = size;
    d->exit = info->exit;
    return d;
}

// gpiolib's generic irqchip creates its domain this way (gpiochip_irqchip_add). (satoru)
struct irq_domain *irq_domain_create_simple(struct fwnode_handle *fwnode,
                                            unsigned int size,
                                            unsigned int first_irq,
                                            const struct irq_domain_ops *ops,
                                            void *host_data)
{
    struct irq_domain_info info = {
        .fwnode = fwnode,
        .size = size,
        .hwirq_max = size,
        .ops = ops,
        .host_data = host_data,
    };

    (void)first_irq;
    return irq_domain_instantiate(&info);
}

void irq_domain_remove(struct irq_domain *domain)
{
    if (!domain)
        return;
    if (domain->exit)
        domain->exit(domain);
    kfree(domain);
}

struct irq_desc *irq_to_desc(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    return (struct irq_desc *)p;
}

struct irq_desc *__irq_resolve_mapping(struct irq_domain *domain,
                                       irq_hw_number_t hwirq,
                                       unsigned int *irq)
{
    if (!domain || hwirq >= domain->revmap_size || !domain->revmap[hwirq])
        return NULL;
    struct irq_data *data = domain->revmap[hwirq];
    if (irq)
        *irq = data->irq;
    return irq_to_desc(data->irq);
}

unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
                                         irq_hw_number_t hwirq,
                                         const struct irq_affinity_desc *affinity)
{
    (void)affinity;
    if (!domain || hwirq >= domain->revmap_size)
        return 0;
    if (domain->revmap[hwirq])
        return domain->revmap[hwirq]->irq;
    for (int i = 0; i < LX_SIM_MAX; i++) {
        if (lx_sim_descs[i])
            continue;
        unsigned int virq;
        if (lx_irq_alloc_soft(&virq))
            return 0;
        struct lx_sim_desc *p = kzalloc(sizeof(*p), GFP_KERNEL);
        if (!p) {
            lx_irq_free_soft(virq);
            return 0;
        }
        p->data.irq = virq;
        p->data.hwirq = hwirq;
        p->data.domain = domain;
        p->data.common = &p->common;
        lx_sim_descs[i] = p;
        lx_sim_virq[i] = virq;
        domain->revmap[hwirq] = &p->data;
        domain->mapcount++;
        if (domain->ops && domain->ops->map) {
            if (domain->ops->map(domain, virq, hwirq)) {
                domain->revmap[hwirq] = NULL;
                lx_sim_descs[i] = NULL;
                lx_sim_virq[i] = 0;
                lx_irq_free_soft(virq);
                kfree(p);
                return 0;
            }
        }
        // the chip's unmask/startup belongs to the request path (the irq core
        // calls it after irq_request_resources), not to mapping. (satoru)
        return virq;
    }
    return 0;
}

void irq_dispose_mapping(unsigned int virq)
{
    struct lx_sim_desc *p = lx_sim_get(virq);
    if (!p)
        return;
    struct irq_domain *d = p->data.domain;
    if (d && d->ops && d->ops->unmap)
        d->ops->unmap(d, virq);
    if (d && p->data.hwirq < d->revmap_size && d->revmap[p->data.hwirq] == &p->data) {
        d->revmap[p->data.hwirq] = NULL;
        d->mapcount--;
    }
    for (int i = 0; i < LX_SIM_MAX; i++)
        if (lx_sim_descs[i] == p) {
            lx_sim_descs[i] = NULL;
            lx_sim_virq[i] = 0;
            break;
        }
    lx_irq_free_soft(virq);
    kfree(p);
}

int irq_set_chip(unsigned int irq, const struct irq_chip *chip)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    if (!p)
        return -EINVAL;
    p->data.chip = (struct irq_chip *)chip;
    return 0;
}

int irq_set_chip_data(unsigned int irq, void *data)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    if (!p)
        return -EINVAL;
    p->data.chip_data = data;
    return 0;
}

void irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
                                   irq_flow_handler_t handle, const char *name)
{
    (void)handle; (void)name;
    irq_set_chip(irq, chip);
}

void __irq_set_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
                       const char *name)
{
    (void)irq; (void)handle; (void)is_chained; (void)name;
}

void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
    (void)irq; (void)clr; (void)set;
}

struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
                                         unsigned int virq)
{
    (void)domain;
    struct lx_sim_desc *p = lx_sim_get(virq);
    return p ? &p->data : NULL;
}

void irq_domain_reset_irq_data(struct irq_data *irq_data)
{
    if (irq_data)
        memset(irq_data, 0, sizeof(*irq_data));
}

struct irq_data *irq_get_irq_data(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    return p ? &p->data : NULL;
}

int irq_set_irq_type(unsigned int irq, unsigned int type)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    if (!p)
        return 0;
    p->common.state_use_accessors = type & IRQD_TRIGGER_MASK;
    return 0;
}

int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool state)
{
    struct lx_sim_desc *p = lx_sim_get(irq);
    if (!p || !p->data.chip || !p->data.chip->irq_set_irqchip_state)
        return -EINVAL;
    return p->data.chip->irq_set_irqchip_state(&p->data, which, state);
}

// the irq core calls the chip's request/release resource hooks around a
// request; gpiolib uses them to mark the line used as IRQ (gpiochip_lock_as_irq). (satoru)
int lx_irqdomain_request_resources(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);

    if (!p || !p->data.chip)
        return 0;
    if (p->data.chip->irq_request_resources)
        return p->data.chip->irq_request_resources(&p->data);
    // an IRQCHIP_IMMUTABLE gpiolib chip (pinctrl-intel) leaves this hook to
    // the core; the core's job is to lock the line as IRQ. (satoru)
    if (p->data.chip_data && gpiochip_lock_as_irq)
        return gpiochip_lock_as_irq(p->data.chip_data,
                                    irqd_to_hwirq(&p->data));
    return 0;
}

int lx_irqdomain_activate(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);

    if (!p || !p->data.chip)
        return 0;
    if (p->data.chip->irq_startup)
        return p->data.chip->irq_startup(&p->data);
    if (p->data.chip->irq_enable)
        p->data.chip->irq_enable(&p->data);
    else if (p->data.chip->irq_unmask)
        p->data.chip->irq_unmask(&p->data);
    return 0;
}

void lx_irqdomain_deactivate(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);

    if (!p || !p->data.chip)
        return;
    if (p->data.chip->irq_disable)
        p->data.chip->irq_disable(&p->data);
    else if (p->data.chip->irq_mask)
        p->data.chip->irq_mask(&p->data);
}

void lx_irqdomain_release_resources(unsigned int irq)
{
    struct lx_sim_desc *p = lx_sim_get(irq);

    if (!p || !p->data.chip)
        return;
    if (p->data.chip->irq_release_resources) {
        p->data.chip->irq_release_resources(&p->data);
        return;
    }
    if (p->data.chip_data && gpiochip_unlock_as_irq)
        gpiochip_unlock_as_irq(p->data.chip_data, irqd_to_hwirq(&p->data));
}

// pinctrl-intel's shared handler dispatches each pending pad to the child
// domain's irq; raising the container's software line runs that irq's action. (satoru)
int generic_handle_domain_irq(struct irq_domain *domain, unsigned int hwirq)
{
    unsigned int virq;

    if (!domain || !__irq_resolve_mapping(domain, hwirq, &virq))
        return -EINVAL;
    lx_irq_raise_soft(virq);
    return 0;
}

void handle_simple_irq(struct irq_desc *desc)
{
    struct lx_sim_desc *p = (struct lx_sim_desc *)desc;
    if (p)
        lx_irq_raise_soft(p->data.irq);
}

__attribute__((weak)) bool irq_work_queue(struct irq_work *work)
{
    if (work && work->func)
        work->func(work);
    return true;
}

void irq_work_sync(struct irq_work *work)
{
    (void)work;
}

// end (satoru)
