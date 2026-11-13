// lx_proc: procfs is absent in the container.
//
// input-core (and a few other cores) create /proc entries in their init and
// treat a NULL return as fatal. there is no procfs in a container, but the
// entries are only a debugging interface; return one static dummy so the init
// succeeds and the entries are never reachable. no proc callback is ever
// called because no file exists. (satoru)

#include <linux/kernel.h>
#include <linux/proc_fs.h>

static char lx_proc_dummy;

struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent)
{
    (void)name;
    (void)parent;
    return (struct proc_dir_entry *)&lx_proc_dummy;
}

struct proc_dir_entry *proc_create(const char *name, umode_t mode,
                                   struct proc_dir_entry *parent,
                                   const struct proc_ops *proc_ops)
{
    (void)name;
    (void)mode;
    (void)parent;
    (void)proc_ops;
    return (struct proc_dir_entry *)&lx_proc_dummy;
}

void remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
    (void)name;
    (void)parent;
}

// end (satoru)
