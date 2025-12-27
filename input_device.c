#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "input_dev"
#define CLASS_NAME "input_class"
#define VENDOR_ID 0x1b36
#define DEVICE_ID 0x0005

// Register offsets (должны совпадать с host_app!)
#define MAX_BUFFER_SIZE_OFFSET    0x00
#define BUFFER_SIZE_OFFSET        0x04
#define BUFFER_START_OFFSET       0x08
#define READ_POS_OFFSET           0x0C  // Позиция чтения драйвера (для синхронизации)
#define BUFFER_POS_OFFSET         0x10  // Позиция записи хоста
#define ACK_REGISTER_OFFSET       0x14
#define STATUS_REGISTER_OFFSET    0x18
#define INPUT_BUFFER_OFFSET       0x1C

#define ACK_NEW_DATA_FROM_HOST    (1 << 0)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("PCI Input Device Driver");

struct input_device_private {
    void __iomem *bar2;
    struct cdev cdev;
    dev_t dev_num;
    struct class *class;
    struct device *device;
    u32 read_pos;
    u32 last_buffer_pos;
};

static int input_dev_open(struct inode *inode, struct file *file)
{
    struct input_device_private *priv;

    priv = container_of(inode->i_cdev, struct input_device_private, cdev);
    file->private_data = priv;

    priv->last_buffer_pos = ioread32(priv->bar2 + BUFFER_POS_OFFSET);

    printk(KERN_INFO "input_dev: Device opened, read_pos=%u, buffer_pos=%u\n",
           priv->read_pos, priv->last_buffer_pos);
    return 0;
}

static int input_dev_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "input_dev: Device closed\n");
    return 0;
}

static ssize_t input_dev_read(struct file *file, char __user *buf,
                             size_t count, loff_t *pos)
{
    struct input_device_private *priv = file->private_data;
    void __iomem *bar2 = priv->bar2;
    u32 ack, current_pos, max_size;
    u8 data;

    printk(KERN_INFO "input_dev: read() called\n");

    if (count == 0) return 0;

    ack = ioread32(bar2 + ACK_REGISTER_OFFSET);
    current_pos = ioread32(bar2 + BUFFER_POS_OFFSET);
    max_size = ioread32(bar2 + MAX_BUFFER_SIZE_OFFSET);

    printk(KERN_INFO "input_dev: ack=0x%08x, current_pos=%u, read_pos=%u\n",
           ack, current_pos, priv->read_pos);

    if (priv->read_pos == current_pos) {
        printk(KERN_INFO "input_dev: No new data to read\n");
        return 0;
    }

    data = ioread8(bar2 + INPUT_BUFFER_OFFSET + priv->read_pos);

    if (copy_to_user(buf, &data, 1)) {
        printk(KERN_ERR "input_dev: copy_to_user failed\n");
        return -EFAULT;
    }

    printk(KERN_INFO "input_dev: Read '%c' (0x%02x) from pos %u\n",
           (data >= 32 && data < 127) ? data : '.', data, priv->read_pos);

    priv->read_pos = (priv->read_pos + 1) % max_size;
    
    // Записываем read_pos в BAR2 для синхронизации с хостом!
    iowrite32(priv->read_pos, bar2 + READ_POS_OFFSET);

    if (priv->read_pos == current_pos) {
        iowrite32(0, bar2 + ACK_REGISTER_OFFSET);
        printk(KERN_INFO "input_dev: All data read, reset ACK flag\n");
    }

    return 1;
}

static ssize_t input_dev_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *pos)
{
    return count;
}

static long input_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct input_device_private *priv = file->private_data;
    void __iomem *bar2 = priv->bar2;
    int size;

    switch (cmd) {
        case _IOW('i', 1, int):
            if (copy_from_user(&size, (int __user *)arg, sizeof(size))) {
                return -EFAULT;
            }
            iowrite32(size, bar2 + BUFFER_SIZE_OFFSET);
            printk(KERN_INFO "input_dev: Buffer size set to %d\n", size);
            break;

        case _IOR('i', 2, int):
            size = ioread32(bar2 + BUFFER_SIZE_OFFSET);
            if (copy_to_user((int __user *)arg, &size, sizeof(size))) {
                return -EFAULT;
            }
            break;

        default:
            return -ENOTTY;
    }

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = input_dev_open,
    .release = input_dev_release,
    .read = input_dev_read,
    .write = input_dev_write,
    .unlocked_ioctl = input_dev_ioctl,
};

// Sysfs attributes
static ssize_t buffer_size_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct input_device_private *priv = dev_get_drvdata(dev);
    u32 size = ioread32(priv->bar2 + BUFFER_SIZE_OFFSET);
    return sprintf(buf, "%u\n", size);
}

static ssize_t buffer_size_store(struct device *dev,
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
    struct input_device_private *priv = dev_get_drvdata(dev);
    u32 size;

    if (kstrtou32(buf, 0, &size))
        return -EINVAL;

    iowrite32(size, priv->bar2 + BUFFER_SIZE_OFFSET);
    return count;
}

static DEVICE_ATTR_RW(buffer_size);

static ssize_t max_buffer_size_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct input_device_private *priv = dev_get_drvdata(dev);
    u32 max_size = ioread32(priv->bar2 + MAX_BUFFER_SIZE_OFFSET);
    return sprintf(buf, "%u\n", max_size);
}

static DEVICE_ATTR_RO(max_buffer_size);

static ssize_t status_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    struct input_device_private *priv = dev_get_drvdata(dev);
    u32 ack = ioread32(priv->bar2 + ACK_REGISTER_OFFSET);
    u32 buffer_pos = ioread32(priv->bar2 + BUFFER_POS_OFFSET);
    u32 max_size = ioread32(priv->bar2 + MAX_BUFFER_SIZE_OFFSET);

    return sprintf(buf, "ack: 0x%08x\nnew_data: %s\nbuffer_pos: %u\nread_pos: %u\nmax_size: %u\n",
                   ack,
                   (ack & ACK_NEW_DATA_FROM_HOST) ? "yes" : "no",
                   buffer_pos, priv->read_pos, max_size);
}

static DEVICE_ATTR_RO(status);

static struct attribute *input_dev_attrs[] = {
    &dev_attr_buffer_size.attr,
    &dev_attr_max_buffer_size.attr,
    &dev_attr_status.attr,
    NULL,
};

static struct attribute_group input_dev_attr_group = {
    .attrs = input_dev_attrs,
};

static int input_dev_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct input_device_private *priv;
    int ret;

    printk(KERN_INFO "input_dev: PCI device probe started\n");

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        ret = -ENOMEM;
        goto err;
    }

    ret = pci_enable_device(pdev);
    if (ret) {
        printk(KERN_ERR "input_dev: Failed to enable PCI device\n");
        goto err_free;
    }

    ret = pci_request_region(pdev, 2, "input_dev_bar2");
    if (ret) {
        printk(KERN_ERR "input_dev: Failed to request BAR2\n");
        goto err_disable;
    }

    priv->bar2 = pci_iomap(pdev, 2, 0);
    if (!priv->bar2) {
        printk(KERN_ERR "input_dev: Failed to map BAR2\n");
        ret = -ENOMEM;
        goto err_release;
    }

    pci_set_drvdata(pdev, priv);

    printk(KERN_INFO "input_dev: BAR2 mapped at %p\n", priv->bar2);

    iowrite32(30720, priv->bar2 + MAX_BUFFER_SIZE_OFFSET);  // 30 KB
    iowrite32(30720, priv->bar2 + BUFFER_SIZE_OFFSET);
    iowrite32(INPUT_BUFFER_OFFSET, priv->bar2 + BUFFER_START_OFFSET);
    iowrite32(0, priv->bar2 + READ_POS_OFFSET);   // Позиция чтения драйвера
    iowrite32(0, priv->bar2 + BUFFER_POS_OFFSET); // Позиция записи хоста
    iowrite32(0, priv->bar2 + ACK_REGISTER_OFFSET);
    iowrite32(0, priv->bar2 + STATUS_REGISTER_OFFSET);

    priv->read_pos = 0;

    ret = alloc_chrdev_region(&priv->dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "input_dev: Failed to allocate device number\n");
        goto err_iounmap;
    }

    cdev_init(&priv->cdev, &fops);
    priv->cdev.owner = THIS_MODULE;

    ret = cdev_add(&priv->cdev, priv->dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "input_dev: Failed to add cdev\n");
        goto err_unregister;
    }

    priv->class = class_create(CLASS_NAME);
    if (IS_ERR(priv->class)) {
        ret = PTR_ERR(priv->class);
        printk(KERN_ERR "input_dev: Failed to create device class\n");
        goto err_cdev;
    }

    priv->device = device_create(priv->class, NULL, priv->dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(priv->device)) {
        ret = PTR_ERR(priv->device);
        printk(KERN_ERR "input_dev: Failed to create device\n");
        goto err_class;
    }

    ret = sysfs_create_group(&priv->device->kobj, &input_dev_attr_group);
    if (ret) {
        printk(KERN_ERR "input_dev: Failed to create sysfs group\n");
        goto err_device;
    }

    dev_set_drvdata(priv->device, priv);

    printk(KERN_INFO "input_dev: Driver loaded successfully\n");
    return 0;

err_device:
    device_destroy(priv->class, priv->dev_num);
err_class:
    class_destroy(priv->class);
err_cdev:
    cdev_del(&priv->cdev);
err_unregister:
    unregister_chrdev_region(priv->dev_num, 1);
err_iounmap:
    pci_iounmap(pdev, priv->bar2);
err_release:
    pci_release_region(pdev, 2);
err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(priv);
err:
    return ret;
}

static void input_dev_remove(struct pci_dev *pdev)
{
    struct input_device_private *priv = pci_get_drvdata(pdev);

    printk(KERN_INFO "input_dev: Removing driver\n");

    if (priv) {
        sysfs_remove_group(&priv->device->kobj, &input_dev_attr_group);
        device_destroy(priv->class, priv->dev_num);
        class_destroy(priv->class);
        cdev_del(&priv->cdev);
        unregister_chrdev_region(priv->dev_num, 1);

        if (priv->bar2) {
            pci_iounmap(pdev, priv->bar2);
        }

        pci_release_region(pdev, 2);
        kfree(priv);
    }

    pci_disable_device(pdev);
    printk(KERN_INFO "input_dev: Driver unloaded\n");
}

static const struct pci_device_id input_dev_ids[] = {
    { PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, input_dev_ids);

static struct pci_driver input_dev_driver = {
    .name = "input_device",
    .id_table = input_dev_ids,
    .probe = input_dev_probe,
    .remove = input_dev_remove,
};

module_pci_driver(input_dev_driver);

