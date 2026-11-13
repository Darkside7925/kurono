// lx_chrdev: major/minor registration without a /dev tree.
//
// input-core reserves the input major in its init so it can hand out device
// numbers. a container has no devtmpfs and never opens /dev/input/event*; the
// reservation is bookkeeping, so it succeeds and the unregister is a no-op.
// the input core itself is upstream and unchanged. (satoru)

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>

int register_chrdev_region(dev_t from, unsigned int count, const char *name)
{
    (void)from;
    (void)count;
    (void)name;
    return 0;
}

void unregister_chrdev_region(dev_t from, unsigned int count)
{
    (void)from;
    (void)count;
}

int alloc_chrdev_region(dev_t *dev, unsigned int baseminor, unsigned int count,
                        const char *name)
{
    (void)count;
    (void)name;
    if (dev)
        *dev = MKDEV(240, baseminor);
    return 0;
}

void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
    memset(cdev, 0, sizeof(*cdev));
    cdev->ops = fops;
    INIT_LIST_HEAD(&cdev->list);
}

int cdev_add(struct cdev *cdev, dev_t dev, unsigned int count)
{
    (void)cdev; (void)dev; (void)count;
    return 0;
}

void cdev_del(struct cdev *cdev)
{
    (void)cdev;
}

// end (satoru)
