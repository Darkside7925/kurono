// pch_gpio_model: a behavioural Intel PCH GPIO community block for the MMIO
// seam. It models what pinctrl-intel and gpiolib's irqchip actually touch:
// REVID/CAPLIST/PADBAR, PAD_OWN, PADCFGLOCK, HOSTSW_OWN, GPI_IS/GPI_IE and the
// PADCFG0..3 of every pad the driver's own community tables declare. The pad
// numbering and group layout are the ones in pinctrl-meteorpoint.c; the reset
// values are measured where this host exposes them (pin 310's PADCFG0 is
// 0x80800102, pad state "locked tx") and assumed otherwise.
//
// the community line is one of the container's software irqs. the model
// raises it when (GPI_IS & GPI_IE) becomes non-zero and the test drives a pad
// input; a W1C on GPI_IS acks. a level pad's status follows the inverted RX
// level; an edge pad's status latches until acked. (satoru)
//
// end (satoru)

#ifndef KLD_PCH_GPIO_MODEL_H
#define KLD_PCH_GPIO_MODEL_H

#include <linux/types.h>

#define PCH_GPIO_MAX_COMMUNITIES 8

// one MTP community's memory range and pin window. groups are per community,
// copied from pinctrl-meteorpoint.c's mtps_communityN_gpps tables. (satoru)
void pch_gpio_model_init(int idx, u64 base, unsigned long len);
void pch_gpio_model_set_irq(int irq);
void pch_gpio_model_set_level(int pin, int level);
int pch_gpio_model_get_level(int pin);
void pch_gpio_model_set_ie(int pin, int on);
void pch_gpio_model_ack(int pin);
unsigned int pch_gpio_model_padcfg0(int pin);
int pch_gpio_model_is(int pin);
int pch_gpio_model_ie(int pin);
int pch_gpio_model_hostown(int pin);
int pch_gpio_model_locked(int pin);
unsigned long pch_gpio_model_irqs(void);

#endif

// end (satoru)
