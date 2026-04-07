// SPDX-License-Identifier: GPL-2.0+
/*
 * hcd.c -- USB over Ethernet Host Controller Driver
 *
 * This driver implements a USB HCD that communicates with a remote
 * USB device controller (udc.c) over UDP.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/gadget.h>
#include <linux/usb/hcd.h>
#include <linux/scatterlist.h>
#include <linux/kthread.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/freezer.h>

#include <asm/byteorder.h>
#include <linux/io.h>
#include <asm/irq.h>

#include "usb_over_net.h"

#define DRIVER_DESC    "USB over Ethernet Host Controller"
#define DRIVER_VERSION "1.0"

#define POWER_BUDGET     500
#define POWER_BUDGET_3   900
#define POLL_INTERVAL_MS  5
#define POLL_TIMEOUT_MS  10

static const char driver_name[] = "usb_over_ethernet_hcd";
static const char driver_desc[] = "USB over Ethernet HCD";

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("USB over Ethernet");
MODULE_LICENSE("GPL");

struct uoe_hcd {
    struct usb_hcd       *hcd;
    struct uoe_connection conn;
    struct task_struct    *poll_thread;
    struct task_struct    *event_thread;
    spinlock_t            lock;
    struct list_head      urb_queue;
    u32                   port_status;
    u32                   old_status;
    int                   connected;
    int                   remote_connected;
    unsigned long         poll_jiffies;
};

static struct platform_driver uoe_hcd_driver;
static struct platform_device *uoe_hcd_pdev;

static inline struct uoe_hcd *hcd_to_uoe(struct usb_hcd *hcd)
{
    return (struct uoe_hcd *)(hcd->hcd_priv);
}

static inline struct usb_hcd *uoe_to_hcd(struct uoe_hcd *uoe)
{
    return container_of((void *)uoe, struct usb_hcd, hcd_priv);
}

static int uoe_hcd_poll_loop(void *data)
{
    struct uoe_hcd *uoe = data;
    struct uoe_packet *pkt = NULL;
    struct sockaddr_in src;
    int ret;

    allow_signal(SIGTERM);

    while (!kthread_should_stop()) {
        ret = uoe_recv_packet(&uoe->conn, &pkt, &src, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EINTR)
                continue;
            msleep(POLL_INTERVAL_MS);
            continue;
        }

        if (pkt) {
            uoe_free_packet(pkt);
            pkt = NULL;
        }
    }

    return 0;
}

static int uoe_hcd_event_loop(void *data)
{
    struct uoe_hcd *uoe = data;
    struct socket *sock;
    struct msghdr msg;
    struct kvec iov;
    struct uoe_event evt;
    struct sockaddr_in src;
    int ret;

    allow_signal(SIGTERM);

    sock = uoe->conn.sock_event;
    if (!sock)
        return -EINVAL;

    while (!kthread_should_stop()) {
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &src;
        msg.msg_namelen = sizeof(src);

        iov.iov_base = &evt;
        iov.iov_len = sizeof(evt);

        ret = kernel_recvmsg(sock, &msg, &iov, 1, sizeof(evt), 0);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EINTR)
                continue;
            msleep(10);
            continue;
        }

        if (evt.type != UOE_EVENT)
            continue;

        spin_lock(&uoe->lock);
        switch (evt.event) {
        case UOE_EVENT_CONNECT:
            uoe->port_status |= USB_PORT_STAT_CONNECTION;
            uoe->port_status |= USB_PORT_STAT_ENABLE;
            uoe->remote_connected = 1;
            break;
        case UOE_EVENT_DISCONNECT:
            uoe->port_status &= ~USB_PORT_STAT_CONNECTION;
            uoe->port_status &= ~USB_PORT_STAT_ENABLE;
            uoe->remote_connected = 0;
            break;
        case UOE_EVENT_RESET_DONE:
            uoe->port_status &= ~USB_PORT_STAT_RESET;
            uoe->port_status |= USB_PORT_STAT_ENABLE;
            uoe->port_status |= (USB_PORT_STAT_C_RESET << 16);
            break;
        case UOE_EVENT_SUSPEND:
            uoe->port_status |= USB_PORT_STAT_SUSPEND;
            break;
        case UOE_EVENT_RESUME:
            uoe->port_status &= ~USB_PORT_STAT_SUSPEND;
            break;
        }
        uoe->old_status = uoe->port_status;
        spin_unlock(&uoe->lock);

        usb_hcd_poll_rh_status(uoe->hcd);
    }

    return 0;
}

static int uoe_hcd_start(struct usb_hcd *hcd)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);

    hcd->power_budget = POWER_BUDGET;
    hcd->state = HC_STATE_RUNNING;
    hcd->uses_new_polling = 1;
    hcd->self.root_hub->speed = USB_SPEED_HIGH;

    spin_lock_init(&uoe->lock);
    INIT_LIST_HEAD(&uoe->urb_queue);
    uoe->port_status = USB_PORT_STAT_POWER | USB_PORT_STAT_CONNECTION;
    uoe->old_status = uoe->port_status;
    uoe->poll_jiffies = jiffies;

    return 0;
}

static void uoe_hcd_stop(struct usb_hcd *hcd)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);

    if (uoe->poll_thread) {
        kthread_stop(uoe->poll_thread);
        uoe->poll_thread = NULL;
    }
    if (uoe->event_thread) {
        kthread_stop(uoe->event_thread);
        uoe->event_thread = NULL;
    }

    uoe_close(&uoe->conn);
}

static int uoe_hcd_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    unsigned long flags;
    int ret = 0;

    spin_lock_irqsave(&uoe->lock, flags);

    if (!uoe->connected) {
        ret = -ENODEV;
        goto done;
    }

    ret = usb_hcd_link_urb_to_ep(hcd, urb);
    if (ret)
        goto done;

    list_add_tail(&urb->urb_list, &uoe->urb_queue);

done:
    spin_unlock_irqrestore(&uoe->lock, flags);
    return ret;
}

static int uoe_hcd_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&uoe->lock, flags);

    ret = usb_hcd_check_unlink_urb(hcd, urb, status);
    if (ret)
        goto done;

    list_del(&urb->urb_list);

    spin_unlock_irqrestore(&uoe->lock, flags);
    usb_hcd_giveback_urb(hcd, urb, status);

    return 0;

done:
    spin_unlock_irqrestore(&uoe->lock, flags);
    return ret;
}

static int uoe_hcd_get_frame(struct usb_hcd *hcd)
{
    struct timespec64 ts64;
    ktime_get_ts64(&ts64);
    return ts64.tv_nsec / NSEC_PER_MSEC;
}

static int uoe_hcd_hub_status(struct usb_hcd *hcd, char *buf)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    unsigned long flags;
    int ret = 0;

    spin_lock_irqsave(&uoe->lock, flags);

    if ((uoe->port_status & 0xffff0000) != 0) {
        buf[0] = (1 << 1);
        ret = 1;
        if (hcd->state == HC_STATE_SUSPENDED)
            usb_hcd_resume_root_hub(hcd);
    }

    spin_unlock_irqrestore(&uoe->lock, flags);
    return ret;
}

static int uoe_hcd_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
                                u16 wIndex, char *buf, u16 wLength)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    unsigned long flags;
    int retval = 0;

    spin_lock_irqsave(&uoe->lock, flags);

    switch (typeReq) {
    case ClearHubFeature:
        break;
    case ClearPortFeature:
        switch (wValue) {
        case USB_PORT_FEAT_SUSPEND:
            uoe->port_status &= ~USB_PORT_STAT_SUSPEND;
            uoe_send_event(&uoe->conn, UOE_EVENT_RESUME);
            break;
        case USB_PORT_FEAT_POWER:
            uoe->port_status &= ~USB_PORT_STAT_POWER;
            break;
        case USB_PORT_FEAT_ENABLE:
        case USB_PORT_FEAT_C_CONNECTION:
        case USB_PORT_FEAT_C_RESET:
            uoe->port_status &= ~(1 << wValue);
            break;
        default:
            retval = -EPIPE;
        }
        break;
    case GetHubDescriptor:
        {
            struct usb_hub_descriptor *desc = (struct usb_hub_descriptor *)buf;
            desc->bDescriptorType = USB_DT_HUB;
            desc->bDescLength = 9;
            desc->wHubCharacteristics = cpu_to_le16(HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_COMMON_OCPM);
            desc->bNbrPorts = 1;
            desc->u.hs.DeviceRemovable[0] = 0;
            desc->u.hs.DeviceRemovable[1] = 0xff;
        }
        break;
    case GetHubStatus:
        *(__le32 *)buf = cpu_to_le32(0);
        break;
    case GetPortStatus:
        if (wIndex != 1) {
            retval = -EPIPE;
            break;
        }
        ((__le16 *)buf)[0] = cpu_to_le16(uoe->port_status);
        ((__le16 *)buf)[1] = cpu_to_le16(uoe->port_status >> 16);
        break;
    case SetHubFeature:
        retval = -EPIPE;
        break;
    case SetPortFeature:
        switch (wValue) {
        case USB_PORT_FEAT_SUSPEND:
            if (uoe->port_status & USB_PORT_STAT_ENABLE) {
                uoe->port_status |= USB_PORT_STAT_SUSPEND;
                uoe_send_event(&uoe->conn, UOE_EVENT_SUSPEND);
            }
            break;
        case USB_PORT_FEAT_POWER:
            uoe->port_status |= USB_PORT_STAT_POWER;
            break;
        case USB_PORT_FEAT_RESET:
            if (uoe->port_status & USB_PORT_STAT_CONNECTION) {
                uoe->port_status |= USB_PORT_STAT_RESET;
                uoe->port_status &= ~(USB_PORT_STAT_ENABLE | USB_PORT_STAT_LOW_SPEED | USB_PORT_STAT_HIGH_SPEED);
            }
            break;
        default:
            retval = -EPIPE;
        }
        break;
    default:
        retval = -EPIPE;
    }

    spin_unlock_irqrestore(&uoe->lock, flags);

    if ((uoe->port_status & 0xffff0000) != 0)
        usb_hcd_poll_rh_status(hcd);

    return retval;
}

static int uoe_hcd_bus_suspend(struct usb_hcd *hcd)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    hcd->state = HC_STATE_SUSPENDED;
    uoe->port_status |= USB_PORT_STAT_SUSPEND;
    return 0;
}

static int uoe_hcd_bus_resume(struct usb_hcd *hcd)
{
    struct uoe_hcd *uoe = hcd_to_uoe(hcd);
    hcd->state = HC_STATE_RUNNING;
    uoe->port_status &= ~USB_PORT_STAT_SUSPEND;
    return 0;
}

static struct hc_driver uoe_hcd_ops = {
    .description = (char *)driver_name,
    .product_desc = "USB over Ethernet Host Controller",
    .hcd_priv_size = sizeof(struct uoe_hcd),
    .reset = NULL,
    .start = uoe_hcd_start,
    .stop = uoe_hcd_stop,
    .urb_enqueue = uoe_hcd_urb_enqueue,
    .urb_dequeue = uoe_hcd_urb_dequeue,
    .get_frame_number = uoe_hcd_get_frame,
    .hub_status_data = uoe_hcd_hub_status,
    .hub_control = uoe_hcd_hub_control,
    .bus_suspend = uoe_hcd_bus_suspend,
    .bus_resume = uoe_hcd_bus_resume,
};

static char *remote_ip = "";
static int data_port = UOE_DATA_PORT;
static int event_port = UOE_EVENT_PORT;

module_param(remote_ip, charp, S_IRUGO);
MODULE_PARM_DESC(remote_ip, "Remote device IP address");
module_param(data_port, int, S_IRUGO);
MODULE_PARM_DESC(data_port, "Data channel UDP port");
module_param(event_port, int, S_IRUGO);
MODULE_PARM_DESC(event_port, "Event channel UDP port");

static int uoe_hcd_probe(struct platform_device *dev)
{
    struct usb_hcd *hcd;
    struct uoe_hcd *uoe;
    int retval;

    if (usb_disabled())
        return -ENODEV;

    if (!remote_ip || strlen(remote_ip) == 0) {
        pr_err("UOE HCD: remote_ip parameter required\n");
        return -EINVAL;
    }

    hcd = usb_create_hcd(&uoe_hcd_ops, &dev->dev, dev_name(&dev->dev));
    if (!hcd)
        return -ENOMEM;

    uoe = hcd_to_uoe(hcd);
    hcd->has_tt = 1;

    retval = uoe_create_client(&uoe->conn, remote_ip, data_port, event_port);
    if (retval < 0) {
        pr_err("UOE HCD: failed to connect to %s:%d\n", remote_ip, data_port);
        goto err_conn;
    }
    uoe->connected = 1;
    uoe->hcd = hcd;

    retval = usb_add_hcd(hcd, 0, 0);
    if (retval < 0) {
        pr_err("UOE HCD: failed to add USB HCD\n");
        goto err_add;
    }

    uoe->poll_thread = kthread_run(uoe_hcd_poll_loop, uoe, "uoe_poll");
    if (IS_ERR(uoe->poll_thread)) {
        retval = PTR_ERR(uoe->poll_thread);
        goto err_poll;
    }

    uoe->event_thread = kthread_run(uoe_hcd_event_loop, uoe, "uoe_event");
    if (IS_ERR(uoe->event_thread)) {
        retval = PTR_ERR(uoe->event_thread);
        goto err_event;
    }

    platform_set_drvdata(dev, hcd);
    pr_info("UOE HCD: started polling to %s\n", remote_ip);
    return 0;

err_event:
    kthread_stop(uoe->poll_thread);
err_poll:
    usb_remove_hcd(hcd);
err_add:
    uoe_close(&uoe->conn);
err_conn:
    usb_put_hcd(hcd);
    return retval;
}

static int uoe_hcd_remove(struct platform_device *dev)
{
    struct usb_hcd *hcd = platform_get_drvdata(dev);

    if (hcd) {
        struct uoe_hcd *uoe = hcd_to_uoe(hcd);

        if (uoe->poll_thread)
            kthread_stop(uoe->poll_thread);
        if (uoe->event_thread)
            kthread_stop(uoe->event_thread);

        usb_remove_hcd(hcd);
        uoe_close(&uoe->conn);
        usb_put_hcd(hcd);
    }

    return 0;
}

static int uoe_hcd_suspend(struct platform_device *dev, pm_message_t state)
{
    struct usb_hcd *hcd = platform_get_drvdata(dev);
    if (hcd)
        set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
    return 0;
}

static int uoe_hcd_resume(struct platform_device *dev)
{
    struct usb_hcd *hcd = platform_get_drvdata(dev);
    if (hcd) {
        set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
        usb_hcd_poll_rh_status(hcd);
    }
    return 0;
}

static struct platform_driver uoe_hcd_driver = {
    .probe = uoe_hcd_probe,
    .remove = uoe_hcd_remove,
    .suspend = uoe_hcd_suspend,
    .resume = uoe_hcd_resume,
    .driver = {
        .name = driver_name,
    },
};

static int __init uoe_hcd_init(void)
{
    int retval;
    struct platform_device *hcd_pdev;

    if (usb_disabled())
        return -ENODEV;

    retval = platform_driver_register(&uoe_hcd_driver);
    if (retval < 0)
        return retval;

    hcd_pdev = platform_device_alloc(driver_name, 0);
    if (!hcd_pdev) {
        retval = -ENOMEM;
        goto err_alloc;
    }

    retval = platform_device_add(hcd_pdev);
    if (retval < 0) {
        platform_device_put(hcd_pdev);
        goto err_add;
    }

    uoe_hcd_pdev = hcd_pdev;
    return 0;

err_alloc:
    platform_driver_unregister(&uoe_hcd_driver);
err_add:
    return retval;
}

static void __exit uoe_hcd_cleanup(void)
{
    if (uoe_hcd_pdev)
        platform_device_unregister(uoe_hcd_pdev);
    platform_driver_unregister(&uoe_hcd_driver);
}

module_init(uoe_hcd_init);
module_exit(uoe_hcd_cleanup);