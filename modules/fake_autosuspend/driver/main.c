#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h> /* 如果你使用设备树 */
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

#include "common.h"


static int my_device_suspend(struct device *dev)
{
    struct my_driver_private *priv = dev_get_drvdata(dev);
    
    dev_info(dev, "Suspending device\n");
    
    // if (priv && priv->reg_base) {
    //     iowrite32(0x0, priv->reg_base + 0x8);
    //     mdelay(10);
    // }
    
    priv->is_suspended = true;
    dev_info(dev, "Device suspended\n");
    return 0;
}

static int my_device_resume(struct device *dev)
{
    struct my_driver_private *priv = dev_get_drvdata(dev);
    
    dev_info(dev, "Resuming device\n");
    
    // if (priv && priv->reg_base) {
    //     iowrite32(0x1, priv->reg_base + 0x8);
    //     mdelay(10);
    // }
    
    priv->is_suspended = false;
    dev_info(dev, "Device resumed\n");
    return 0;
}

static int my_device_runtime_suspend(struct device *dev)
{
    struct my_driver_private *priv = dev_get_drvdata(dev);
    
    dev_dbg(dev, "Runtime suspending device\n");
    
    if (priv && priv->reg_base) {
        iowrite32(0x0, priv->reg_base + 0x4);
    }
    
    priv->is_suspended = true;
    dev_dbg(dev, "Device runtime suspended\n");
    return 0;
}

static int my_device_runtime_resume(struct device *dev)
{
    struct my_driver_private *priv = dev_get_drvdata(dev);
    
    dev_dbg(dev, "Runtime resuming device\n");
    
    if (priv && priv->reg_base) {
        iowrite32(0x1, priv->reg_base + 0x4);
    }
    
    priv->is_suspended = false;
    dev_dbg(dev, "Device runtime resumed\n");
    return 0;
}

static const struct dev_pm_ops my_device_pm_ops = {
    .suspend = my_device_suspend,
    .resume = my_device_resume,
    .runtime_suspend = my_device_runtime_suspend,
    .runtime_resume = my_device_runtime_resume,
};


/* 设备驱动的结构体 */
static int my_device_probe(struct platform_device *pdev) {
    struct my_driver_private *priv;
    // struct resource *mem_res, *irq_res;
    // int ret;
    
    dev_info(&pdev->dev, "Probing PM platform driver\n");
    
    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    
    priv->dev = &pdev->dev;
    priv->is_suspended = true;

    platform_set_drvdata(pdev, priv);

    pm_runtime_set_autosuspend_delay(&pdev->dev, 2000);
    pm_runtime_use_autosuspend(&pdev->dev);
    pm_runtime_enable(&pdev->dev);
    
    pm_runtime_mark_last_busy(&pdev->dev);
    pm_runtime_put_autosuspend(&pdev->dev);


    create_debugfs_entries(priv);
    
    return 0; // 返回0表示成功
}

static int my_device_remove(struct platform_device *pdev) {
    printk(KERN_INFO "My Device Removed\n");
    return 0; // 返回0表示成功
}

static struct platform_driver my_device_driver = {
    .probe  = my_device_probe,
    .remove = my_device_remove,
    .driver = {
        .name   = "my_platform_device", // 设备名称，在设备树中引用时使用
        .owner  = THIS_MODULE, // 所有者，通常设置为THIS_MODULE
    },
};

/* 模块初始化函数 */
static int __init my_device_init(void) {
    int ret;
    ret = platform_driver_register(&my_device_driver);
    if (ret) {
        printk(KERN_ERR "Failed to register my device driver\n");
        return ret;
    }
    return 0;
}

/* 模块退出函数 */
static void __exit my_device_exit(void) {
    remove_debugfs_entries();
    platform_driver_unregister(&my_device_driver);
}

module_init(my_device_init);
module_exit(my_device_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A Simple Platform Device Driver Example");