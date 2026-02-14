// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual PCIe over Ethernet - Core Module
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/rculist.h>
#include "virtual_pcie.h"

MODULE_AUTHOR("Virtual PCIe Driver");
MODULE_DESCRIPTION("Virtual PCIe over Ethernet Transport");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

static int vpci_major;
static struct class *vpci_class;
static dev_t vpci_devt;
static struct cdev vpci_cdev;
static int vpci_device_count;

DEFINE_IDR(vpci_idr);
DEFINE_MUTEX(vpci_idr_lock);

char *vpci_remote_ip = "192.168.1.100";
module_param(vpci_remote_ip, charp, 0644);
MODULE_PARM_DESC(vpci_remote_ip, "Remote IP address for virtual PCIe connection");

int vpci_remote_port = 8888;
module_param(vpci_remote_port, int, 0644);
MODULE_PARM_DESC(vpci_remote_port, "Remote port for virtual PCIe connection");

int vpci_local_port = 8888;
module_param(vpci_local_port, int, 0644);
MODULE_PARM_DESC(vpci_local_port, "Local port for virtual PCIe connection");

bool vpci_loopback;
module_param(vpci_loopback, bool, 0644);
MODULE_PARM_DESC(vpci_loopback, "Enable loopback mode for self-testing");

int vpci_debug;
module_param(vpci_debug, int, 0644);
MODULE_PARM_DESC(vpci_debug, "Debug level (0-3)");

static int vpci_get_free_id(void)
{
    int id;

    mutex_lock(&vpci_idr_lock);
    id = idr_alloc(&vpci_idr, NULL, 0, VPCI_MAX_DEVS, GFP_KERNEL);
    mutex_unlock(&vpci_idr_lock);

    return id;
}

static void vpci_put_id(int id)
{
    mutex_lock(&vpci_idr_lock);
    idr_remove(&vpci_idr, id);
    mutex_unlock(&vpci_idr_lock);
}

static int vpci_device_init(struct vpci_device *dev)
{
    int i, ret = 0;

    vpci_info("Initializing virtual PCIe device %d\n", dev->id);

    atomic_set(&dev->connected, 0);
    atomic_set(&dev->session_id, 0);
    atomic_set(&dev->seq_num, 0);
    spin_lock_init(&dev->lock);
    atomic_set(&dev->refcount, 1);
    init_completion(&dev->shutdown_comp);
    init_completion(&dev->handshake_comp);
    skb_queue_head_init(&dev->tx_queue);
    init_waitqueue_head(&dev->tx_wait);
    init_waitqueue_head(&dev->ack_wait);
    INIT_LIST_HEAD(&dev->tx_list);
    spin_lock_init(&dev->tx_lock);
    spin_lock_init(&dev->shmem_lock);

    dev->sock = NULL;
    dev->rx_thread = NULL;
    dev->tx_thread = NULL;

    for (i = 0; i < VPCI_MAX_BARS; i++) {
        dev->bars[i].addr = NULL;
        dev->bars[i].phys_addr = 0;
        dev->bars[i].size = 0;
    }
    dev->bar_count = 0;

    dev->config_size = 4096;
    dev->config_space = kzalloc(dev->config_size, GFP_KERNEL);
    if (!dev->config_space) {
        vpci_err("Failed to allocate config space\n");
        return -ENOMEM;
    }

    dev->shmem_size = VPCI_SHMEM_SIZE;
    dev->shmem = kzalloc(dev->shmem_size, GFP_KERNEL);
    if (!dev->shmem) {
        vpci_err("Failed to allocate shared memory\n");
        ret = -ENOMEM;
        goto err_config;
    }

    INIT_WORK(&dev->reconnect_work, vpci_reconnect_work);
    timer_setup(&dev->keepalive_timer, vpci_keepalive_timer, 0);

    vpci_info("Virtual PCIe device %d initialized successfully\n", dev->id);
    return 0;

err_config:
    kfree(dev->config_space);
    return ret;
}

static void vpci_device_exit(struct vpci_device *dev)
{
    vpci_info("Cleaning up virtual PCIe device %d\n", dev->id);

    if (atomic_read(&dev->connected)) {
        vpci_net_disconnect(dev);
    }

    cancel_work_sync(&dev->reconnect_work);
    timer_delete_sync(&dev->keepalive_timer);

    kfree(dev->shmem);
    kfree(dev->config_space);

    vpci_bar_unmap(dev, 0);
    vpci_bar_unmap(dev, 1);

    complete(&dev->shutdown_comp);
}

static int vpci_open(struct inode *inode, struct file *file)
{
    struct vpci_device *dev;
    int id;

    id = iminor(inode);
    mutex_lock(&vpci_idr_lock);
    dev = idr_find(&vpci_idr, id);
    if (dev) {
        get_device(dev->dev);
        atomic_inc(&dev->refcount);
    }
    mutex_unlock(&vpci_idr_lock);

    if (!dev) {
        return -ENODEV;
    }

    file->private_data = dev;
    return 0;
}

static int vpci_release(struct inode *inode, struct file *file)
{
    struct vpci_device *dev = file->private_data;

    if (dev) {
        atomic_dec(&dev->refcount);
        put_device(dev->dev);
    }

    return 0;
}

static long vpci_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vpci_device *dev = file->private_data;
    int ret = 0;

    switch (cmd) {
    case VPCI_IOCTL_CONNECT:
        if (vpci_loopback) {
            dev->role = VPCI_ROLE_LOOPBACK;
            atomic_set(&dev->connected, 1);
        } else {
            dev->role = VPCI_ROLE_RC;
            ret = vpci_net_connect(dev);
        }
        break;

    case VPCI_IOCTL_DISCONNECT:
        vpci_net_disconnect(dev);
        dev->role = VPCI_ROLE_NONE;
        break;

    case VPCI_IOCTL_GET_STATUS:
        ret = atomic_read(&dev->connected);
        break;

    case VPCI_IOCTL_SET_ROLE:
        if (arg == VPCI_ROLE_EP) {
            dev->role = VPCI_ROLE_EP;
        }
        break;

    default:
        ret = -EINVAL;
    }

    return ret;
}

static const struct file_operations vpci_fops = {
    .owner          = THIS_MODULE,
    .open           = vpci_open,
    .release        = vpci_release,
    .unlocked_ioctl = vpci_ioctl,
};

static int vpci_probe(struct platform_device *pdev)
{
    struct vpci_device *dev;
    int id, ret;

    vpci_info("Probing virtual PCIe device\n");

    id = vpci_get_free_id();
    if (id < 0) {
        vpci_err("No free device IDs available\n");
        return -ENODEV;
    }

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        vpci_put_id(id);
        return -ENOMEM;
    }

    dev->id = id;
    dev->dev = &pdev->dev;
    platform_set_drvdata(pdev, dev);

    ret = vpci_device_init(dev);
    if (ret < 0) {
        vpci_err("Failed to initialize device: %d\n", ret);
        goto err_idr;
    }

    mutex_lock(&vpci_idr_lock);
    idr_replace(&vpci_idr, dev, id);
    mutex_unlock(&vpci_idr_lock);

    vpci_device_count++;
    vpci_info("Virtual PCIe device %d probed successfully\n", id);

    return 0;

err_idr:
    vpci_put_id(id);
    kfree(dev);
    return ret;
}

static void vpci_remove(struct platform_device *pdev)
{
    struct vpci_device *dev = platform_get_drvdata(pdev);

    if (dev) {
        vpci_device_exit(dev);
        vpci_put_id(dev->id);
        kfree(dev);
    }

    vpci_device_count--;
}

static struct platform_driver vpci_driver = {
    .probe  = vpci_probe,
    .remove = vpci_remove,
    .driver = {
        .name = VPCI_NAME,
    },
};

static int __init vpci_core_init(void)
{
    int ret;

    vpci_info("Initializing virtual PCIe core\n");

    ret = alloc_chrdev_region(&vpci_devt, 0, VPCI_MAX_DEVS, VPCI_NAME);
    if (ret < 0) {
        vpci_err("Failed to allocate chrdev region: %d\n", ret);
        return ret;
    }

    vpci_major = MAJOR(vpci_devt);

    vpci_class = class_create(VPCI_NAME);
    if (IS_ERR(vpci_class)) {
        vpci_err("Failed to create class\n");
        ret = PTR_ERR(vpci_class);
        goto err_chrdev;
    }

    cdev_init(&vpci_cdev, &vpci_fops);
    vpci_cdev.owner = THIS_MODULE;
    ret = cdev_add(&vpci_cdev, vpci_devt, VPCI_MAX_DEVS);
    if (ret < 0) {
        vpci_err("Failed to add cdev\n");
        goto err_class;
    }

    ret = platform_driver_register(&vpci_driver);
    if (ret < 0) {
        vpci_err("Failed to register platform driver\n");
        goto err_cdev;
    }

    vpci_net_init();

    vpci_info("Virtual PCIe core initialized successfully (major: %d)\n", vpci_major);
    return 0;

err_cdev:
    cdev_del(&vpci_cdev);
err_class:
    class_destroy(vpci_class);
err_chrdev:
    unregister_chrdev_region(vpci_devt, VPCI_MAX_DEVS);
    return ret;
}

static void __exit vpci_core_exit(void)
{
    vpci_info("Unloading virtual PCIe core\n");

    vpci_net_exit();
    platform_driver_unregister(&vpci_driver);
    cdev_del(&vpci_cdev);
    class_destroy(vpci_class);
    unregister_chrdev_region(vpci_devt, VPCI_MAX_DEVS);

    idr_destroy(&vpci_idr);

    vpci_info("Virtual PCIe core unloaded\n");
}

module_init(vpci_core_init);
module_exit(vpci_core_exit);
