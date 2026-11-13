// kernel-core providers the i2chid driver stack references but no upstream
// object in the container defines (C5.7b).
//
// gen_stubs refuses to emit a function stub for a symbol the objects use as
// DATA: the code would read the stub's code bytes as a value. The rule here is
// "real or loud":
//   * functions whose ADDRESS the driver tables store (irq_chip /
//     file_operations entries) are real function stubs (LX_STUB): calling one
//     prints and aborts, it never returns garbage;
//   * data objects get their REAL kernel type and the fields callers rely on
//     filled in; fields left empty are named in a comment and are unreachable
//     in a container (no sysfs, no module store, no class core);
//   * nothing is a zero-filled char array: that hid NULL-pointer jumps behind
//     a plausible-looking provider.
//
// This file is shim code, built into every family: these are generic kernel
// symbols, not ACPICA state. A family that already provides one must drop it
// here.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/fwnode.h>
#include <linux/kobject_ns.h>
#include <linux/moduleparam.h>
#include <linux/static_call.h>
#include <net/flow_dissector.h>


// ----- data objects: real type, real initial value --------------------------
// drivers/acpi/utils.c walks its bus list only from the (uncompiled) linux
// acpi core. an empty, self-linked list is the correct "no devices" state.
struct list_head acpi_bus_id_list = { &acpi_bus_id_list, &acpi_bus_id_list };
struct list_head acpi_bus_type    = { &acpi_bus_type, &acpi_bus_type };

// the linux acpi core is not linked; the shim's ACPICA answers the namespace,
// so acpi_disabled stays 0 exactly as on a boot with firmware ACPI present.
int acpi_disabled;
DEFINE_MUTEX(acpi_device_lock);

// upstream kernel/irq/dummychip.c: fixed-vector controllers. the callbacks are
// its noop/noop_ret/ack_bad shapes; the ack path logs instead of printing the
// irq descriptor (the container has no desc printer).
static void aux_noop(struct irq_data *d) { (void)d; }
static unsigned int aux_noop_ret(struct irq_data *d) { (void)d; return 0; }
static void aux_ack_bad(struct irq_data *d)
{
    lx_log("[kld-stub-data] dummy_irq_chip ack on irq ");
    lx_log_u((unsigned long)d->irq);
    lx_log("\n");
}
struct irq_chip dummy_irq_chip = {
    .name        = "dummy",
    .irq_startup = aux_noop_ret,
    .irq_shutdown = aux_noop,
    .irq_ack     = aux_ack_bad,
    .irq_mask    = aux_noop,
    .irq_unmask  = aux_noop,
};

// eth.o only takes the address. the container's skb hash path uses the shim's
// own flow keys; a zeroed dissector is the "no keys programmed" state.
struct flow_dissector flow_keys_basic_dissector;

// gpiolib stores this as an irqchip fwnode identity; the shim's irqdomain
// never creates a fwnode of this type, so no callback is ever invoked. the
// real type keeps the pointer arithmetic and comparisons honest.
const struct fwnode_operations irqchip_fwnode_ops = { };

// kernel/params.c owns the real get/set helpers, but the container has no
// sysfs/module store: __param only stores these pointers, and nothing can
// read or write the parameter. set/get/free are therefore deliberately NULL
// (never dereferenced); the alternative was reading stub code bytes.
__attribute__((weak)) const struct kernel_param_ops param_ops_bool  = { };
const struct kernel_param_ops param_ops_charp = { };

// net/wireless/sysfs.c stores this pointer in class.ns_type (R_X86_64_64 in
// the ieee80211_class data). class_register is the shim's no-op and no sysfs
// class core exists, so the pointer is stored and never dereferenced. the real
// type (and the NET type tag) keeps any future comparison meaningful.
const struct kobj_ns_type_operations net_ns_type_operations = {
    .type = KOBJ_NS_TYPE_NET,
};

// the pinned config has CONFIG_HAVE_STATIC_CALL_INLINE=y: might_resched is a
// NULL static call (DEFINE_STATIC_CALL_NULL), so the key is func=NULL,type=1
// and a conditional call is skipped. a zero array would make the key look
// non-NULL and dereference garbage.
struct static_call_key __SCK__might_resched = { .func = NULL, .type = 1 };

// end (satoru)
