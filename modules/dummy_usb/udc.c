// SPDX-License-Identifier: GPL-2.0+
/*
 * udc.c -- USB over Ethernet Device Controller Driver
 *
 * This driver implements a USB gadget that communicates with a remote
 * USB host controller (hcd.c) over UDP.
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
#include <linux/kthread.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/freezer.h>

#include <asm/byteorder.h>
#include <linux/io.h>
#include <asm/irq.h>

#include "usb_over_net.h"

#define DRIVER_DESC    "USB over Ethernet Device Controller"
#define DRIVER_VERSION "1.0"

static const char gadget_name[] = "usb_over_ethernet_udc";

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("USB over Ethernet");
MODULE_LICENSE("GPL");

#define FIFO_SIZE       64

#define TYPE_BULK_OR_INT    (USB_EP_CAPS_TYPE_BULK | USB_EP_CAPS_TYPE_INT)

static const char ep0name[] = "ep0";

static const struct {
    const char *name;
    const struct usb_ep_caps caps;
} ep_info[] = {
    #define EP_INFO(_name, _caps) \
        { .name = _name, .caps = _caps, }

    EP_INFO(ep0name, USB_EP_CAPS(USB_EP_CAPS_TYPE_CONTROL, USB_EP_CAPS_DIR_ALL)),
    EP_INFO("ep1in-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep2out-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_OUT)),
    EP_INFO("ep5in-int", USB_EP_CAPS(USB_EP_CAPS_TYPE_INT, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep6in-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep7out-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_OUT)),
    EP_INFO("ep10in-int", USB_EP_CAPS(USB_EP_CAPS_TYPE_INT, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep11in-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep12out-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_OUT)),
    EP_INFO("ep15in-int", USB_EP_CAPS(USB_EP_CAPS_TYPE_INT, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep1out-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_OUT)),
    EP_INFO("ep2in-bulk", USB_EP_CAPS(USB_EP_CAPS_TYPE_BULK, USB_EP_CAPS_DIR_IN)),
    EP_INFO("ep-aout", USB_EP_CAPS(TYPE_BULK_OR_INT, USB_EP_CAPS_DIR_OUT)),
    EP_INFO("ep-bin", USB_EP_CAPS(TYPE_BULK_OR_INT, USB_EP_CAPS_DIR_IN)),

    #undef EP_INFO
};

#define DUMMY_ENDPOINTS  ARRAY_SIZE(ep_info)

struct uoe_ep {
    struct list_head        queue;
    unsigned long           last_io;
    struct usb_gadget       *gadget;
    const struct usb_endpoint_descriptor *desc;
    struct usb_ep           ep;
    unsigned                halted:1;
    unsigned                wedged:1;
    unsigned                already_seen:1;
    unsigned                setup_stage:1;
};

struct uoe_request {
    struct list_head        queue;
    struct usb_request      req;
};

static inline struct uoe_ep *usb_ep_to_uoe_ep(struct usb_ep *_ep)
{
    return container_of(_ep, struct uoe_ep, ep);
}

static inline struct uoe_request *usb_request_to_uoe_request(struct usb_request *_req)
{
    return container_of(_req, struct uoe_request, req);
}

struct uoe_udc {
    spinlock_t              lock;
    struct uoe_ep           ep[DUMMY_ENDPOINTS];
    int                     address;
    int                     callback_usage;
    struct usb_gadget       gadget;
    struct usb_gadget_driver *driver;
    struct uoe_request      fifo_req;
    u8                      fifo_buf[FIFO_SIZE];
    u16                     devstatus;
    unsigned                ints_enabled:1;
    unsigned                udc_suspended:1;
    unsigned                pullup:1;
    struct uoe_connection   conn;
    struct task_struct      *poll_thread;
    __u16                   seq;
};

static inline struct uoe_udc *ep_to_uoe(struct uoe_ep *ep)
{
    return container_of(ep->gadget, struct uoe_udc, gadget);
}

static inline struct device *udc_dev(struct uoe_udc *dum)
{
    return dum->gadget.dev.parent;
}

static void nuke(struct uoe_udc *dum, struct uoe_ep *ep)
{
    while (!list_empty(&ep->queue)) {
        struct uoe_request *req;

        req = list_entry(ep->queue.next, struct uoe_request, queue);
        list_del_init(&req->queue);
        req->req.status = -ESHUTDOWN;

        spin_unlock(&dum->lock);
        usb_gadget_giveback_request(&ep->ep, &req->req);
        spin_lock(&dum->lock);
    }
}

static void stop_activity(struct uoe_udc *dum)
{
    int i;

    dum->address = 0;
    for (i = 0; i < DUMMY_ENDPOINTS; ++i)
        nuke(dum, &dum->ep[i]);
}

static int dummy_enable(struct usb_ep *_ep, const struct usb_endpoint_descriptor *desc)
{
    struct uoe_ep       *ep;
    struct uoe_udc      *dum;
    unsigned             max;
    int                  retval = -EINVAL;

    ep = usb_ep_to_uoe_ep(_ep);
    if (!_ep || !desc || ep->desc || _ep->name == ep0name
            || desc->bDescriptorType != USB_DT_ENDPOINT)
        return -EINVAL;
    dum = ep_to_uoe(ep);
    if (!dum->driver)
        return -ESHUTDOWN;

    max = usb_endpoint_maxp(desc);

    switch (usb_endpoint_type(desc)) {
    case USB_ENDPOINT_XFER_BULK:
        if (strstr(ep->ep.name, "-iso") || strstr(ep->ep.name, "-int"))
            goto done;
        switch (dum->gadget.speed) {
        case USB_SPEED_HIGH:
            if (max == 512)
                break;
            goto done;
        case USB_SPEED_FULL:
            if (max == 8 || max == 16 || max == 32 || max == 64)
                break;
            goto done;
        default:
            goto done;
        }
        break;
    case USB_ENDPOINT_XFER_INT:
        if (strstr(ep->ep.name, "-iso"))
            goto done;
        switch (dum->gadget.speed) {
        case USB_SPEED_HIGH:
            if (max <= 1024)
                break;
            goto done;
        case USB_SPEED_FULL:
            if (max <= 64)
                break;
            goto done;
        default:
            if (max <= 8)
                break;
            goto done;
        }
        break;
    default:
        goto done;
    }

    _ep->maxpacket = max;
    ep->desc = desc;
    ep->halted = ep->wedged = 0;
    retval = 0;

done:
    return retval;
}

static int dummy_disable(struct usb_ep *_ep)
{
    struct uoe_ep       *ep;
    struct uoe_udc      *dum;
    unsigned long        flags;

    ep = usb_ep_to_uoe_ep(_ep);
    if (!_ep || !ep->desc || _ep->name == ep0name)
        return -EINVAL;
    dum = ep_to_uoe(ep);

    spin_lock_irqsave(&dum->lock, flags);
    ep->desc = NULL;
    nuke(dum, ep);
    spin_unlock_irqrestore(&dum->lock, flags);

    return 0;
}

static struct usb_request *dummy_alloc_request(struct usb_ep *_ep, gfp_t mem_flags)
{
    struct uoe_request *req;

    if (!_ep)
        return NULL;

    req = kzalloc(sizeof(*req), mem_flags);
    if (!req)
        return NULL;
    INIT_LIST_HEAD(&req->queue);
    return &req->req;
}

static void dummy_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
    struct uoe_request *req;

    if (!_ep || !_req) {
        WARN_ON(1);
        return;
    }

    req = usb_request_to_uoe_request(_req);
    kfree(req);
}

static void fifo_complete(struct usb_ep *ep, struct usb_request *req)
{
}

static int dummy_queue(struct usb_ep *_ep, struct usb_request *_req, gfp_t mem_flags)
{
    struct uoe_ep       *ep;
    struct uoe_request  *req;
    struct uoe_udc      *dum;
    unsigned long       flags;

    req = usb_request_to_uoe_request(_req);
    if (!_req || !list_empty(&req->queue) || !_req->complete)
        return -EINVAL;

    ep = usb_ep_to_uoe_ep(_ep);
    if (!_ep || (!ep->desc && _ep->name != ep0name))
        return -EINVAL;

    dum = ep_to_uoe(ep);
    if (!dum->driver)
        return -ESHUTDOWN;

    _req->status = -EINPROGRESS;
    _req->actual = 0;
    spin_lock_irqsave(&dum->lock, flags);

    if (ep->desc && (ep->desc->bEndpointAddress & USB_DIR_IN) &&
            list_empty(&dum->fifo_req.queue) &&
            list_empty(&ep->queue) &&
            _req->length <= FIFO_SIZE) {
        req = &dum->fifo_req;
        req->req = *_req;
        req->req.buf = dum->fifo_buf;
        memcpy(dum->fifo_buf, _req->buf, _req->length);
        req->req.context = dum;
        req->req.complete = fifo_complete;

        list_add_tail(&req->queue, &ep->queue);
        spin_unlock(&dum->lock);
        _req->actual = _req->length;
        _req->status = 0;
        usb_gadget_giveback_request(_ep, _req);
        spin_lock(&dum->lock);
    } else {
        list_add_tail(&req->queue, &ep->queue);
    }

    spin_unlock_irqrestore(&dum->lock, flags);
    return 0;
}

static int dummy_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
    struct uoe_ep       *ep;
    struct uoe_udc      *dum;
    int                  retval = -EINVAL;
    unsigned long       flags;
    struct uoe_request  *iter;

    if (!_ep || !_req)
        return retval;
    ep = usb_ep_to_uoe_ep(_ep);
    dum = ep_to_uoe(ep);

    if (!dum->driver)
        return -ESHUTDOWN;

    spin_lock_irqsave(&dum->lock, flags);
    list_for_each_entry(iter, &ep->queue, queue) {
        if (&iter->req != _req)
            continue;
        list_del_init(&iter->queue);
        _req->status = -ECONNRESET;
        spin_unlock(&dum->lock);
        usb_gadget_giveback_request(_ep, _req);
        return 0;
    }
    spin_unlock_irqrestore(&dum->lock, flags);
    return retval;
}

static int dummy_set_halt(struct usb_ep *_ep, int value)
{
    struct uoe_ep       *ep;
    struct uoe_udc      *dum;

    if (!_ep)
        return -EINVAL;
    ep = usb_ep_to_uoe_ep(_ep);
    dum = ep_to_uoe(ep);

    if (!dum->driver)
        return -ESHUTDOWN;
    if (!value)
        ep->halted = ep->wedged = 0;
    else if (ep->desc && (ep->desc->bEndpointAddress & USB_DIR_IN) &&
            !list_empty(&ep->queue))
        return -EAGAIN;
    else {
        ep->halted = 1;
        if (value == 2)
            ep->wedged = 1;
    }
    return 0;
}

static int dummy_set_wedge(struct usb_ep *_ep)
{
    if (!_ep || _ep->name == ep0name)
        return -EINVAL;
    return dummy_set_halt(_ep, 1);
}

static const struct usb_ep_ops dummy_ep_ops = {
    .enable       = dummy_enable,
    .disable      = dummy_disable,
    .alloc_request = dummy_alloc_request,
    .free_request  = dummy_free_request,
    .queue         = dummy_queue,
    .dequeue       = dummy_dequeue,
    .set_halt      = dummy_set_halt,
    .set_wedge     = dummy_set_wedge,
};

static int dummy_g_get_frame(struct usb_gadget *_gadget)
{
    struct timespec64 ts64;
    ktime_get_ts64(&ts64);
    return ts64.tv_nsec / NSEC_PER_MSEC;
}

static int dummy_wakeup(struct usb_gadget *_gadget)
{
    struct uoe_udc *dum = container_of(_gadget, struct uoe_udc, gadget);
    if (!(dum->devstatus & (1 << USB_DEVICE_REMOTE_WAKEUP)))
        return -EINVAL;
    return uoe_send_event(&dum->conn, UOE_EVENT_RESUME);
}

static int dummy_set_selfpowered(struct usb_gadget *_gadget, int value)
{
    struct uoe_udc *dum = container_of(_gadget, struct uoe_udc, gadget);
    _gadget->is_selfpowered = (value != 0);
    if (value)
        dum->devstatus |= (1 << USB_DEVICE_SELF_POWERED);
    else
        dum->devstatus &= ~(1 << USB_DEVICE_SELF_POWERED);
    return 0;
}

static void dummy_udc_update_ep0(struct uoe_udc *dum)
{
    if (dum->gadget.speed == USB_SPEED_SUPER)
        dum->ep[0].ep.maxpacket = 9;
    else
        dum->ep[0].ep.maxpacket = 64;
}

static int dummy_pullup(struct usb_gadget *_gadget, int value)
{
    struct uoe_udc *dum = container_of(_gadget, struct uoe_udc, gadget);
    unsigned long flags;

    spin_lock_irqsave(&dum->lock, flags);
    dum->pullup = (value != 0);
    if (value == 0) {
        while (dum->callback_usage > 0) {
            spin_unlock_irqrestore(&dum->lock, flags);
            usleep_range(1000, 2000);
            spin_lock_irqsave(&dum->lock, flags);
        }
    }
    spin_unlock_irqrestore(&dum->lock, flags);

    if (value)
        uoe_send_event(&dum->conn, UOE_EVENT_CONNECT);
    else
        uoe_send_event(&dum->conn, UOE_EVENT_DISCONNECT);

    return 0;
}

static void dummy_udc_set_speed(struct usb_gadget *_gadget, enum usb_device_speed speed)
{
    struct uoe_udc *dum = container_of(_gadget, struct uoe_udc, gadget);
    dum->gadget.speed = speed;
    dummy_udc_update_ep0(dum);
}

static void dummy_udc_async_callbacks(struct usb_gadget *_gadget, bool enable)
{
    struct uoe_udc *dum = container_of(_gadget, struct uoe_udc, gadget);
    spin_lock_irq(&dum->lock);
    dum->ints_enabled = enable;
    spin_unlock_irq(&dum->lock);
}

static int dummy_udc_start(struct usb_gadget *g, struct usb_gadget_driver *driver)
{
    struct uoe_udc *dum = container_of(g, struct uoe_udc, gadget);

    switch (g->speed) {
    case USB_SPEED_LOW:
    case USB_SPEED_FULL:
    case USB_SPEED_HIGH:
        break;
    default:
        return -EINVAL;
    }

    spin_lock_irq(&dum->lock);
    dum->devstatus = 0;
    dum->driver = driver;
    spin_unlock_irq(&dum->lock);

    return 0;
}

static int dummy_udc_stop(struct usb_gadget *g)
{
    struct uoe_udc *dum = container_of(g, struct uoe_udc, gadget);

    spin_lock_irq(&dum->lock);
    dum->ints_enabled = 0;
    stop_activity(dum);
    dum->driver = NULL;
    spin_unlock_irq(&dum->lock);

    return 0;
}

static const struct usb_gadget_ops dummy_ops = {
    .get_frame      = dummy_g_get_frame,
    .wakeup         = dummy_wakeup,
    .set_selfpowered = dummy_set_selfpowered,
    .pullup         = dummy_pullup,
    .udc_start      = dummy_udc_start,
    .udc_stop       = dummy_udc_stop,
    .udc_set_speed  = dummy_udc_set_speed,
    .udc_async_callbacks = dummy_udc_async_callbacks,
};

static ssize_t function_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct uoe_udc *dum = container_of(dev, struct uoe_udc, gadget.dev);
    if (!dum->driver || !dum->driver->function)
        return 0;
    return scnprintf(buf, PAGE_SIZE, "%s\n", dum->driver->function);
}
static DEVICE_ATTR_RO(function);

static void init_dummy_udc_hw(struct uoe_udc *dum)
{
    int i;

    INIT_LIST_HEAD(&dum->gadget.ep_list);
    for (i = 0; i < DUMMY_ENDPOINTS; i++) {
        struct uoe_ep *ep = &dum->ep[i];

        if (!ep_info[i].name)
            break;
        ep->ep.name = ep_info[i].name;
        ep->ep.caps = ep_info[i].caps;
        ep->ep.ops = &dummy_ep_ops;
        list_add_tail(&ep->ep.ep_list, &dum->gadget.ep_list);
        ep->halted = ep->wedged = ep->already_seen = ep->setup_stage = 0;
        usb_ep_set_maxpacket_limit(&ep->ep, ~0);
        ep->ep.max_streams = 16;
        ep->last_io = jiffies;
        ep->gadget = &dum->gadget;
        ep->desc = NULL;
        INIT_LIST_HEAD(&ep->queue);
    }

    dum->gadget.ep0 = &dum->ep[0].ep;
    list_del_init(&dum->ep[0].ep.ep_list);
    INIT_LIST_HEAD(&dum->fifo_req.queue);
}

static int handle_control_request(struct uoe_udc *dum, struct usb_ctrlrequest *setup)
{
    int ret = 1;
    u16 w_index = le16_to_cpu(setup->wIndex);
    u16 w_value = le16_to_cpu(setup->wValue);

    switch (setup->bRequest) {
    case USB_REQ_SET_ADDRESS:
        if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
            dum->address = w_value;
            ret = 0;
        }
        break;
    case USB_REQ_SET_FEATURE:
        if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
            switch (w_value) {
            case USB_DEVICE_REMOTE_WAKEUP:
                ret = 0;
                dum->devstatus |= (1 << USB_DEVICE_REMOTE_WAKEUP);
                break;
            }
        } else if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_ENDPOINT)) {
            int i;
            for (i = 1; i < DUMMY_ENDPOINTS; i++) {
                if (dum->ep[i].desc && dum->ep[i].desc->bEndpointAddress == w_index) {
                    dum->ep[i].halted = 1;
                    ret = 0;
                    break;
                }
            }
        }
        break;
    case USB_REQ_CLEAR_FEATURE:
        if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
            if (w_value == USB_DEVICE_REMOTE_WAKEUP) {
                ret = 0;
                dum->devstatus &= ~(1 << USB_DEVICE_REMOTE_WAKEUP);
            }
        } else if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_ENDPOINT)) {
            int i;
            for (i = 1; i < DUMMY_ENDPOINTS; i++) {
                if (dum->ep[i].desc && dum->ep[i].desc->bEndpointAddress == w_index) {
                    if (!dum->ep[i].wedged)
                        dum->ep[i].halted = 0;
                    ret = 0;
                    break;
                }
            }
        }
        break;
    case USB_REQ_GET_STATUS:
        if (setup->bRequestType == (USB_TYPE_STANDARD | USB_RECIP_DEVICE | USB_DIR_IN)) {
            char buf[2] = { (char)dum->devstatus, 0 };
            dum->fifo_req.req.buf = buf;
            dum->fifo_req.req.length = 2;
            ret = 0;
        }
        break;
    }
    return ret;
}

static int uoe_udc_poll_loop(void *data)
{
    struct uoe_udc *dum = data;
    struct uoe_packet *pkt = NULL;
    struct sockaddr_in src;
    int ret;

    allow_signal(SIGTERM);

    while (!kthread_should_stop()) {
        ret = uoe_recv_packet(&dum->conn, &pkt, &src, 1000);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EINTR)
                continue;
            msleep(5);
            continue;
        }

        if (!pkt)
            continue;

        if (pkt->hdr.type == UOE_POLL_IN || pkt->hdr.type == UOE_POLL_OUT) {
            __u8 ep_addr = pkt->hdr.ep_addr;
            __u16 seq = ntohs(pkt->hdr.seq);
            struct uoe_ep *ep = NULL;
            struct uoe_request *req = NULL;
            int i;

            spin_lock(&dum->lock);
            for (i = 0; i < DUMMY_ENDPOINTS; i++) {
                if (dum->ep[i].desc && dum->ep[i].desc->bEndpointAddress == ep_addr) {
                    ep = &dum->ep[i];
                    break;
                }
            }

            if (!ep && ep_addr == 0)
                ep = &dum->ep[0];

            if (ep) {
                if (!list_empty(&ep->queue)) {
                    req = list_entry(ep->queue.next, struct uoe_request, queue);
                    list_del_init(&req->queue);
                }
            }

            if (req) {
                uoe_send_data(&dum->conn, ep_addr, seq, req->req.buf, req->req.actual);
                spin_unlock(&dum->lock);
                usb_gadget_giveback_request(&ep->ep, &req->req);
            } else if (ep && ep->halted) {
                uoe_send_stall(&dum->conn, ep_addr, seq);
                spin_unlock(&dum->lock);
            } else {
                uoe_send_nak(&dum->conn, ep_addr, seq);
                spin_unlock(&dum->lock);
            }
        } else if (pkt->hdr.type == UOE_SETUP) {
            struct usb_ctrlrequest setup;
            int value;

            memcpy(&setup, pkt->data, sizeof(setup));
            spin_lock(&dum->lock);
            dum->ep[0].setup_stage = 1;

            value = handle_control_request(dum, &setup);
            if (value > 0 && dum->driver) {
                ++dum->callback_usage;
                spin_unlock(&dum->lock);
                value = dum->driver->setup(&dum->gadget, &setup);
                spin_lock(&dum->lock);
                --dum->callback_usage;
            }
            spin_unlock(&dum->lock);
        }

        uoe_free_packet(pkt);
        pkt = NULL;
    }

    return 0;
}

static struct platform_driver uoe_udc_driver;
static struct platform_device *uoe_udc_pdev;

static char *listen_addr = "0.0.0.0";
static int listen_port = UOE_DATA_PORT;

module_param(listen_addr, charp, S_IRUGO);
MODULE_PARM_DESC(listen_addr, "Listen address");
module_param(listen_port, int, S_IRUGO);
MODULE_PARM_DESC(listen_port, "Listen port");

static int uoe_udc_probe(struct platform_device *pdev)
{
    struct uoe_udc *dum;
    int rc;

    dum = devm_kzalloc(&pdev->dev, sizeof(*dum), GFP_KERNEL);
    if (!dum)
        return -ENOMEM;

    platform_set_drvdata(pdev, dum);
    spin_lock_init(&dum->lock);

    dum->gadget.name = gadget_name;
    dum->gadget.ops = &dummy_ops;
    dum->gadget.max_speed = USB_SPEED_HIGH;
    dum->gadget.dev.parent = &pdev->dev;

    init_dummy_udc_hw(dum);

    rc = uoe_create_server(&dum->conn, listen_addr, listen_port, UOE_EVENT_PORT);
    if (rc < 0) {
        dev_err(&pdev->dev, "failed to create server: %d\n", rc);
        return rc;
    }

    rc = usb_add_gadget_udc(&pdev->dev, &dum->gadget);
    if (rc < 0)
        goto err_udc;

    rc = device_create_file(&dum->gadget.dev, &dev_attr_function);
    if (rc < 0)
        goto err_dev;

    dum->poll_thread = kthread_run(uoe_udc_poll_loop, dum, "uoe_udc_poll");
    if (IS_ERR(dum->poll_thread)) {
        rc = PTR_ERR(dum->poll_thread);
        goto err_thread;
    }

    pr_info("UOE UDC: listening on %s:%d\n", listen_addr, listen_port);
    return 0;

err_thread:
    device_remove_file(&dum->gadget.dev, &dev_attr_function);
err_dev:
    usb_del_gadget_udc(&dum->gadget);
err_udc:
    uoe_close(&dum->conn);
    return rc;
}

static int uoe_udc_remove(struct platform_device *pdev)
{
    struct uoe_udc *dum = platform_get_drvdata(pdev);

    if (dum->poll_thread)
        kthread_stop(dum->poll_thread);

    device_remove_file(&dum->gadget.dev, &dev_attr_function);
    usb_del_gadget_udc(&dum->gadget);
    uoe_close(&dum->conn);

    return 0;
}

static int uoe_udc_suspend(struct platform_device *dev, pm_message_t state)
{
    struct uoe_udc *dum = platform_get_drvdata(dev);
    spin_lock_irq(&dum->lock);
    dum->udc_suspended = 1;
    spin_unlock_irq(&dum->lock);
    return 0;
}

static int uoe_udc_resume(struct platform_device *dev)
{
    struct uoe_udc *dum = platform_get_drvdata(dev);
    spin_lock_irq(&dum->lock);
    dum->udc_suspended = 0;
    spin_unlock_irq(&dum->lock);
    return 0;
}

static struct platform_driver uoe_udc_driver = {
    .probe    = uoe_udc_probe,
    .remove   = uoe_udc_remove,
    .suspend  = uoe_udc_suspend,
    .resume   = uoe_udc_resume,
    .driver   = {
        .name = gadget_name,
    },
};

static int __init uoe_udc_init(void)
{
    int retval;
    struct platform_device *udc_pdev;

    retval = platform_driver_register(&uoe_udc_driver);
    if (retval < 0)
        return retval;

    udc_pdev = platform_device_alloc(gadget_name, 0);
    if (!udc_pdev) {
        retval = -ENOMEM;
        goto err_alloc;
    }

    retval = platform_device_add(udc_pdev);
    if (retval < 0) {
        platform_device_put(udc_pdev);
        goto err_add;
    }

    uoe_udc_pdev = udc_pdev;
    return 0;

err_alloc:
    platform_driver_unregister(&uoe_udc_driver);
err_add:
    return retval;
}

static void __exit uoe_udc_cleanup(void)
{
    if (uoe_udc_pdev)
        platform_device_unregister(uoe_udc_pdev);
    platform_driver_unregister(&uoe_udc_driver);
}

module_init(uoe_udc_init);
module_exit(uoe_udc_cleanup);