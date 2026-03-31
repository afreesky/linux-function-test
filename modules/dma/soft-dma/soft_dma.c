// SPDX-License-Identifier: GPL-2.0
/*
 * Soft DMA Controller Driver - No Device Tree Version
 *
 * Copyright (C) 2024
 *
 * This is a software-emulated DMA controller that uses kernel threads
 * to perform DMA transfers. From Linux's perspective, it appears as
 * a real physical DMA controller.
 *
 * This version works without device tree - creates a virtual DMA device
 * that can be used immediately after loading the module.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/property.h>

#define DRV_NAME        "soft-dma"
#define DRV_VERSION     "1.0"
#define NUM_CHANNELS    4

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Soft DMA Driver");
MODULE_DESCRIPTION("Software Emulated DMA Controller (No Device Tree)");
MODULE_VERSION(DRV_VERSION);

struct soft_dma_desc;
struct soft_dma_chan;

struct soft_dma_dev {
    struct dma_device       dma;
    struct device         *dma_dev;
    struct dentry         *debugfs;
    struct soft_dma_chan  *channels;
    atomic_t              refcount;
};

struct soft_dma_desc {
    struct dma_async_tx_descriptor   tx;
    struct soft_dma_chan            *schan;
    dma_addr_t                      src;
    dma_addr_t                      dst;
    size_t                          len;
    enum dma_data_direction         direction;
    bool                            active;
    struct list_head                node;
};

struct soft_dma_chan {
    struct dma_chan                 chan;
    struct task_struct             *thread;
    wait_queue_head_t              wait_queue;
    atomic_t                       task_count;
    bool                           stop;
    spinlock_t                     lock;
    struct soft_dma_dev            *sdev;
    struct list_head               submitted;
    struct list_head               issued;
    struct list_head               completed;
    u64                            bytes_transferred;
    u64                            transactions_completed;
    spinlock_t                     desc_lock;
};

#define to_soft_dma_chan(c) container_of(c, struct soft_dma_chan, chan)
#define to_soft_dma_desc(d) container_of(d, struct soft_dma_desc, tx)

static struct soft_dma_dev *soft_dma_device;
static struct dentry *soft_dma_debugfs_root;

static void soft_dma_issue_pending(struct dma_chan *chan);
static enum dma_status soft_dma_tx_status(struct dma_chan *chan,
        dma_cookie_t cookie, struct dma_tx_state *txstate);
static int soft_dma_terminate_all(struct dma_chan *chan);

static int soft_dma_thread_fn(void *data)
{
    struct soft_dma_chan *schan = data;
    struct soft_dma_desc *sdesc;
    unsigned long flags;

    set_freezable();

    pr_info("%s: channel %d thread started\n", DRV_NAME, schan->chan.chan_id);

    while (!kthread_should_stop()) {
        wait_event_interruptible(schan->wait_queue,
                kthread_should_stop() || atomic_read(&schan->task_count) > 0);

        if (kthread_should_stop())
            break;

        try_to_freeze();

        spin_lock_irqsave(&schan->desc_lock, flags);
        if (list_empty(&schan->submitted)) {
            spin_unlock_irqrestore(&schan->desc_lock, flags);
            continue;
        }

        sdesc = list_first_entry(&schan->submitted,
                struct soft_dma_desc, node);
        list_del(&sdesc->node);
        list_add_tail(&sdesc->node, &schan->issued);
        sdesc->active = true;
        atomic_dec(&schan->task_count);
        spin_unlock_irqrestore(&schan->desc_lock, flags);

        if (sdesc->len > 0) {
            void *src_ptr, *dst_ptr;

            src_ptr = phys_to_virt(sdesc->src);
            dst_ptr = phys_to_virt(sdesc->dst);

            memcpy(dst_ptr, src_ptr, sdesc->len);

            schan->bytes_transferred += sdesc->len;
        }

        spin_lock_irqsave(&schan->desc_lock, flags);
        list_del(&sdesc->node);
        
        if (sdesc->tx.callback) {
            dma_async_tx_callback cb = sdesc->tx.callback;
            void *cb_param = sdesc->tx.callback_param;
            spin_unlock_irqrestore(&schan->desc_lock, flags);
            cb(cb_param);
        } else {
            spin_unlock_irqrestore(&schan->desc_lock, flags);
        }

        list_add_tail(&sdesc->node, &schan->completed);
        schan->transactions_completed++;
    }

    pr_info("%s: channel %d thread stopped\n", DRV_NAME, schan->chan.chan_id);

    return 0;
}

static void soft_dma_desc_free(struct soft_dma_desc *sdesc)
{
    kfree(sdesc);
}

static struct dma_async_tx_descriptor *
soft_dma_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
        size_t len, unsigned long flags)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    struct soft_dma_desc *sdesc;

    if (!len)
        return NULL;

    sdesc = kzalloc(sizeof(*sdesc), GFP_NOWAIT);
    if (!sdesc)
        return NULL;

    sdesc->src = src;
    sdesc->dst = dst;
    sdesc->len = len;
    sdesc->direction = DMA_BIDIRECTIONAL;
    sdesc->schan = schan;
    sdesc->active = false;

    dma_async_tx_descriptor_init(&sdesc->tx, chan);
    sdesc->tx.flags = flags;

    return &sdesc->tx;
}

static void soft_dma_issue_pending(struct dma_chan *chan)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    unsigned long flags;

    spin_lock_irqsave(&schan->desc_lock, flags);
    list_splice_tail_init(&schan->submitted, &schan->issued);
    spin_unlock_irqrestore(&schan->desc_lock, flags);

    atomic_inc(&schan->task_count);
    wake_up_interruptible(&schan->wait_queue);
}

static enum dma_status soft_dma_tx_status(struct dma_chan *chan,
        dma_cookie_t cookie, struct dma_tx_state *txstate)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    struct soft_dma_desc *sdesc;
    enum dma_status status = DMA_COMPLETE;
    unsigned long flags;
    u32 residue = 0;
    dma_cookie_t last_complete = 0;
    dma_cookie_t last_used = 0;

    if (!txstate)
        return DMA_COMPLETE;

    spin_lock_irqsave(&schan->desc_lock, flags);
    list_for_each_entry(sdesc, &schan->issued, node) {
        if (sdesc->tx.cookie == cookie) {
            residue = sdesc->len;
            status = DMA_IN_PROGRESS;
        }
        if (sdesc->tx.cookie > last_used)
            last_used = sdesc->tx.cookie;
    }
    list_for_each_entry(sdesc, &schan->completed, node) {
        if (sdesc->tx.cookie == cookie) {
            residue = 0;
            status = DMA_COMPLETE;
        }
        if (sdesc->tx.cookie > last_complete)
            last_complete = sdesc->tx.cookie;
    }
    spin_unlock_irqrestore(&schan->desc_lock, flags);

    dma_set_tx_state(txstate, last_complete, last_used, residue);

    return status;
}

static int soft_dma_terminate_all(struct dma_chan *chan)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    struct soft_dma_desc *sdesc;
    unsigned long flags;
    LIST_HEAD(head);

    spin_lock_irqsave(&schan->desc_lock, flags);
    list_splice_init(&schan->submitted, &head);
    list_splice_init(&schan->issued, &head);
    list_splice_init(&schan->completed, &head);
    spin_unlock_irqrestore(&schan->desc_lock, flags);

    list_for_each_entry(sdesc, &head, node) {
        soft_dma_desc_free(sdesc);
    }

    return 0;
}

static int soft_dma_device_config(struct dma_chan *chan,
        struct dma_slave_config *config)
{
    return 0;
}

static int soft_dma_alloc_chan_resources(struct dma_chan *chan)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    char thread_name[32];

    snprintf(thread_name, sizeof(thread_name), "soft_dma_%d", chan->chan_id);

    INIT_LIST_HEAD(&schan->submitted);
    INIT_LIST_HEAD(&schan->issued);
    INIT_LIST_HEAD(&schan->completed);

    schan->thread = kthread_run(soft_dma_thread_fn, schan, thread_name);
    if (IS_ERR(schan->thread)) {
        pr_err("%s: failed to create thread for channel %d\n",
                DRV_NAME, chan->chan_id);
        return PTR_ERR(schan->thread);
    }

    pr_info("%s: allocated channel %d\n", DRV_NAME, chan->chan_id);

    return 0;
}

static void soft_dma_free_chan_resources(struct dma_chan *chan)
{
    struct soft_dma_chan *schan = to_soft_dma_chan(chan);
    struct soft_dma_desc *sdesc;
    unsigned long flags;
    LIST_HEAD(head);

    schan->stop = true;
    wake_up_interruptible(&schan->wait_queue);

    if (schan->thread) {
        kthread_stop(schan->thread);
        schan->thread = NULL;
    }

    spin_lock_irqsave(&schan->desc_lock, flags);
    list_splice_init(&schan->submitted, &head);
    list_splice_init(&schan->issued, &head);
    list_splice_init(&schan->completed, &head);
    spin_unlock_irqrestore(&schan->desc_lock, flags);

    list_for_each_entry(sdesc, &head, node) {
        soft_dma_desc_free(sdesc);
    }

    pr_info("%s: freed channel %d\n", DRV_NAME, chan->chan_id);
}

static int soft_dma_debugfs_show(struct seq_file *s, void *data)
{
    struct soft_dma_dev *sdev = s->private;
    int i;

    seq_printf(s, "Soft DMA Controller Driver v%s\n", DRV_VERSION);
    seq_printf(s, "======================================\n\n");
    seq_printf(s, "Channels: %d\n\n", NUM_CHANNELS);

    for (i = 0; i < NUM_CHANNELS; i++) {
        struct soft_dma_chan *schan = &sdev->channels[i];

        seq_printf(s, "Channel %d:\n", i);
        seq_printf(s, "  Status:      %s\n",
                schan->thread ? "Running" : "Stopped");
        seq_printf(s, "  Bytes:       %llu\n",
                schan->bytes_transferred);
        seq_printf(s, "  Completed:   %llu\n",
                schan->transactions_completed);
        seq_printf(s, "  Pending:     %d\n",
                atomic_read(&schan->task_count));
        seq_printf(s, "\n");
    }

    return 0;
}

DEFINE_SHOW_ATTRIBUTE(soft_dma_debugfs);

static void soft_dma_dev_release(struct device *dev)
{
}

static int soft_dma_probe(struct platform_device *plat_dev)
{
    struct soft_dma_dev *sdev;
    struct dma_device *dma;
    int ret;
    int i;
    struct device *dev;

    pr_info("%s: probing...\n", DRV_NAME);

    sdev = devm_kzalloc(&plat_dev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    soft_dma_device = sdev;

    sdev->channels = devm_kcalloc(&plat_dev->dev, NUM_CHANNELS,
            sizeof(struct soft_dma_chan), GFP_KERNEL);
    if (!sdev->channels)
        return -ENOMEM;

    dev = &plat_dev->dev;
    sdev->dma_dev = dev;

    dma = &sdev->dma;
    dma->dev = dev;
    dma->dev->dma_mask = &dma->dev->coherent_dma_mask;
    if (!dma->dev->coherent_dma_mask)
        dma->dev->coherent_dma_mask = DMA_BIT_MASK(64);

    dma_cap_zero(dma->cap_mask);
    dma_cap_set(DMA_MEMCPY, dma->cap_mask);

    dma->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
            BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
            BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
            BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
    dma->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
            BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
            BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
            BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
    dma->directions = BIT(DMA_MEM_TO_MEM);
    dma->residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

    dma->device_alloc_chan_resources = soft_dma_alloc_chan_resources;
    dma->device_free_chan_resources = soft_dma_free_chan_resources;
    dma->device_prep_dma_memcpy = soft_dma_prep_dma_memcpy;
    dma->device_issue_pending = soft_dma_issue_pending;
    dma->device_tx_status = soft_dma_tx_status;
    dma->device_config = soft_dma_device_config;
    dma->device_terminate_all = soft_dma_terminate_all;

    INIT_LIST_HEAD(&dma->channels);

    for (i = 0; i < NUM_CHANNELS; i++) {
        struct soft_dma_chan *schan = &sdev->channels[i];

        schan->sdev = sdev;
        schan->chan.device = dma;
        schan->chan.chan_id = i;

        init_waitqueue_head(&schan->wait_queue);
        spin_lock_init(&schan->lock);
        spin_lock_init(&schan->desc_lock);
        atomic_set(&schan->task_count, 0);
        schan->stop = false;
        schan->bytes_transferred = 0;
        schan->transactions_completed = 0;

        list_add_tail(&schan->chan.device_node, &dma->channels);
    }

    ret = dma_async_device_register(dma);
    if (ret) {
        pr_err("%s: failed to register DMA device: %d\n", DRV_NAME, ret);
        return ret;
    }

    soft_dma_debugfs_root = debugfs_create_dir(DRV_NAME, NULL);
    if (!IS_ERR(soft_dma_debugfs_root)) {
        debugfs_create_file("stats", 0444, soft_dma_debugfs_root,
                sdev, &soft_dma_debugfs_fops);
    }

    pr_info("%s: registered %d channels\n", DRV_NAME, NUM_CHANNELS);

    return 0;
}

static void soft_dma_remove(struct platform_device *plat_dev)
{
    struct soft_dma_dev *sdev = soft_dma_device;
    struct dma_device *dma = &sdev->dma;
    int i;

    dma_async_device_unregister(dma);

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (sdev->channels[i].thread) {
            sdev->channels[i].stop = true;
            wake_up_interruptible(&sdev->channels[i].wait_queue);
            kthread_stop(sdev->channels[i].thread);
        }
    }

    debugfs_remove_recursive(soft_dma_debugfs_root);
    soft_dma_debugfs_root = NULL;

    pr_info("%s: removed\n", DRV_NAME);
}

static const struct of_device_id soft_dma_of_match[] = {
    { .compatible = "soft,dma", },
    { },
};

MODULE_DEVICE_TABLE(of, soft_dma_of_match);

static struct platform_driver soft_dma_platform_driver = {
    .driver = {
        .name = DRV_NAME,
        .of_match_table = soft_dma_of_match,
    },
    .probe = soft_dma_probe,
    .remove = soft_dma_remove,
};

static struct platform_device *soft_dma_platform_device;

static int __init soft_dma_init(void)
{
    int ret;

    ret = platform_driver_register(&soft_dma_platform_driver);
    if (ret) {
        pr_err("%s: failed to register platform driver: %d\n", DRV_NAME, ret);
        return ret;
    }

    soft_dma_platform_device = platform_device_alloc(DRV_NAME, PLATFORM_DEVID_NONE);
    if (!soft_dma_platform_device) {
        pr_err("%s: failed to allocate platform device\n", DRV_NAME);
        platform_driver_unregister(&soft_dma_platform_driver);
        return -ENOMEM;
    }

    soft_dma_platform_device->dev.release = soft_dma_dev_release;

    ret = platform_device_add(soft_dma_platform_device);
    if (ret) {
        pr_err("%s: failed to add platform device: %d\n", DRV_NAME, ret);
        platform_device_put(soft_dma_platform_device);
        platform_driver_unregister(&soft_dma_platform_driver);
        return ret;
    }

    pr_info("%s: driver loaded (v%s)\n", DRV_NAME, DRV_VERSION);
    pr_info("%s: DMA channels available at /sys/class/dma/\n", DRV_NAME);

    return 0;
}

static void __exit soft_dma_exit(void)
{
    if (soft_dma_platform_device) {
        platform_device_del(soft_dma_platform_device);
        platform_device_put(soft_dma_platform_device);
    }
    platform_driver_unregister(&soft_dma_platform_driver);

    pr_info("%s: driver unloaded\n", DRV_NAME);
}

module_init(soft_dma_init);
module_exit(soft_dma_exit);
