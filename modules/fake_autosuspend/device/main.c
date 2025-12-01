#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h> /* 如果你使用设备树 */


/* 设备资源定义 */
#define MY_DEVICE_BASE_ADDR    0x10000000
#define MY_DEVICE_REG_SIZE     0x1000
#define MY_DEVICE_IRQ_NUM      25

static struct resource my_device_resources[] = {
    [0] = {
        .start = MY_DEVICE_BASE_ADDR,
        .end   = MY_DEVICE_BASE_ADDR + MY_DEVICE_REG_SIZE - 1,
        .flags = IORESOURCE_MEM,
    },
    [1] = {
        .start = MY_DEVICE_IRQ_NUM,
        .end   = MY_DEVICE_IRQ_NUM,
        .flags = IORESOURCE_IRQ,
    }
};

/* 平台设备私有数据 */
struct my_device_platform_data {
    int custom_setting;
    char device_name[32];
};

static struct my_device_platform_data my_pdata = {
    .custom_setting = 100,
    .device_name = "my_static_device",
};

/* 平台设备定义 */
static struct platform_device my_platform_device = {
    .name = "my_platform_device",
    .id = -1,
    .num_resources = ARRAY_SIZE(my_device_resources),
    .resource = my_device_resources,
    .dev = {
        .platform_data = &my_pdata,
        .power = {
            .runtime_status = RPM_SUSPENDED,
            .disable_depth = 1,
            .runtime_error = 0,
            .autosuspend_delay = 2000,
            .use_autosuspend = 1,
        },
    },
};


/* 模块初始化函数 */
static int __init my_device_init(void)
{
    int ret;
    
    pr_info("Registering platform device: %s\n", my_platform_device.name);
    
    ret = platform_device_register(&my_platform_device);
    if (ret) {
        pr_err("Failed to register platform device: %d\n", ret);
        return ret;
    }
    
    pr_info("Platform device registered successfully\n");
    return 0;
}

/* 模块退出函数 */
static void __exit my_device_exit(void)
{
    pr_info("Unregistering platform device\n");
    platform_device_unregister(&my_platform_device);
    pr_info("Platform device unregistered\n");
}

module_init(my_device_init);
module_exit(my_device_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Static Platform Device Registration Example");
MODULE_VERSION("1.0");