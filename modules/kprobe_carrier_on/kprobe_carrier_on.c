
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/sched.h>
#include <linux/netdevice.h>

static struct kprobe kp = {
    .symbol_name = "netif_carrier_on",
};

/* 前置探针处理函数 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct net_device *dev = (struct net_device *)regs->di;
    pr_info("netif_carrier_on called for dev: %s\n", dev->name);
    
    /* 打印调用栈 */
    dump_stack();
    
    return 0;
}

static int __init kprobe_init(void)
{
    int ret;
    kp.pre_handler = handler_pre;
    
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("register_kprobe failed, returned %d\n", ret);
        return ret;
    }
    pr_info("Planted kprobe at %p\n", kp.addr);
    return 0;
}

static void __exit kprobe_exit(void)
{
    unregister_kprobe(&kp);
    pr_info("kprobe at %p unregistered\n", kp.addr);
}

module_init(kprobe_init);
module_exit(kprobe_exit);
MODULE_LICENSE("GPL");

