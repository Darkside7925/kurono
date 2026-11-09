// kld test: parse a real HID report descriptor with the UNMODIFIED upstream
// hid-core (linux 6.12.99 drivers/hid/hid-core.c) and print it with upstream's
// own hid_dump_device (drivers/hid/hid-debug.c). The output is the first two
// sections of linux's /sys/kernel/debug/hid/<dev>/rdesc, so a byte diff against
// that file from the real machine is an exact A/B of the parser.
//
//   kld-hidparse.elf <report_descriptor.bin> <vendor> <product>
//
// Compiled in KERNEL context (like lx_emul/kimpl), so the hid structs are the
// real layouts. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../lx_emul/kimpl/lx_kshim.h"

long lx_os_read_file(const char *path, void *buf, unsigned long max);
void lx_os_write_out(const void *buf, unsigned long n);

// seq_file sink: everything hid-debug prints goes to stdout. (satoru)
static char kld_seqbuf[1 << 16];

void seq_printf(struct seq_file *m, const char *f, ...)
{
    va_list ap;
    int n;
    (void)m;
    va_start(ap, f);
    n = vsnprintf(kld_seqbuf, sizeof(kld_seqbuf), f, ap);
    va_end(ap);
    if (n > 0)
        lx_os_write_out(kld_seqbuf, (unsigned long)(n < (int)sizeof(kld_seqbuf) ? n : (int)sizeof(kld_seqbuf) - 1));
}

// seq_puts is a header inline (forwards to __seq_puts). (satoru)
void __seq_puts(struct seq_file *m, const char *s)
{
    (void)m;
    lx_os_write_out(s, strlen(s));
}

void seq_putc(struct seq_file *m, char c)
{
    (void)m;
    lx_os_write_out(&c, 1);
}

static struct hid_driver kld_hid_drv = { .name = "kld-hidparse" };
// the harness stands in for the transport (i2c-hid on the laptop). no
// max_buffer_size, i.e. upstream's default HID_MAX_BUFFER_SIZE. (satoru)
static const struct hid_ll_driver kld_hid_ll = { 0 };

int kld_test_main(int argc, char **argv)
{
    static u8 rdesc[HID_MAX_DESCRIPTOR_SIZE];
    struct seq_file sf;
    if (argc < 4) {
        lx_log("usage: kld-hidparse <rdesc.bin> <vendor hex> <product hex>\n");
        return 2;
    }
    long n = lx_os_read_file(argv[1], rdesc, sizeof(rdesc));
    if (n <= 0) {
        lx_log("[hidparse] cannot read descriptor\n");
        return 2;
    }
    struct hid_device *hid = hid_allocate_device();
    if (IS_ERR(hid)) {
        lx_log("[hidparse] hid_allocate_device failed\n");
        return 3;
    }
    hid->bus = BUS_I2C;
    hid->vendor = (u32)simple_strtoul(argv[2], NULL, 16);
    hid->product = (u32)simple_strtoul(argv[3], NULL, 16);
    hid->driver = &kld_hid_drv;
    hid->ll_driver = &kld_hid_ll;
    int r = hid_parse_report(hid, rdesc, (unsigned int)n);
    if (!r)
        r = hid_open_report(hid);
    if (r) {
        lx_log("[hidparse] parse failed\n");
        return 4;
    }
    memset(&sf, 0, sizeof(sf));
    // exactly hid_debug_rdesc_show's first two sections. (satoru)
    for (unsigned int i = 0; i < hid->rsize; i++)
        seq_printf(&sf, "%02x ", hid->rdesc[i]);
    seq_printf(&sf, "\n\n");
    hid_dump_device(hid, &sf);
    seq_printf(&sf, "\n");
    return 0;
}
// end (satoru)
