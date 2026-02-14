// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual PCIe over Ethernet - Network Transport Layer
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/inet.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <net/sock.h>
#include "virtual_pcie.h"

int vpci_net_init(void)
{
    vpci_info("Initializing network layer\n");
    return 0;
}

void vpci_net_exit(void)
{
    vpci_info("Exiting network layer\n");
}

int vpci_net_connect(struct vpci_device *dev)
{
    struct sockaddr_in *addr = &dev->remote_addr;
    int ret;

    vpci_info("Connecting to %s:%d\n", vpci_remote_ip, vpci_remote_port);

    ret = sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &dev->sock);
    if (ret < 0) {
        vpci_err("Failed to create socket: %d\n", ret);
        return ret;
    }

    dev->sock->sk->sk_rcvtimeo = VPCI_TIMEOUT;
    dev->sock->sk->sk_sndtimeo = VPCI_TIMEOUT;

    addr->sin_family = AF_INET;
    addr->sin_port = htons(vpci_remote_port);
    addr->sin_addr.s_addr = in_aton(vpci_remote_ip);

    ret = kernel_connect(dev->sock, (struct sockaddr *)addr, sizeof(*addr), 0);
    if (ret < 0) {
        vpci_err("Failed to connect to remote: %d\n", ret);
        sock_release(dev->sock);
        dev->sock = NULL;
        return ret;
    }

    dev->rx_thread = kthread_run(vpci_rx_thread, dev, "vpci_rx_%d", dev->id);
    if (IS_ERR(dev->rx_thread)) {
        vpci_err("Failed to start RX thread\n");
        ret = PTR_ERR(dev->rx_thread);
        dev->rx_thread = NULL;
        goto err_sock;
    }

    dev->tx_thread = kthread_run(vpci_tx_thread, dev, "vpci_tx_%d", dev->id);
    if (IS_ERR(dev->tx_thread)) {
        vpci_err("Failed to start TX thread\n");
        ret = PTR_ERR(dev->tx_thread);
        dev->tx_thread = NULL;
        goto err_rx;
    }

    atomic_set(&dev->connected, 1);

    mod_timer(&dev->keepalive_timer, jiffies + VPCI_KEEPALIVE_INTERVAL);

    vpci_info("Connected successfully\n");
    return 0;

err_rx:
    kthread_stop(dev->rx_thread);
    dev->rx_thread = NULL;
err_sock:
    sock_release(dev->sock);
    dev->sock = NULL;
    return ret;
}

void vpci_net_disconnect(struct vpci_device *dev)
{
    vpci_info("Disconnecting\n");

    timer_delete_sync(&dev->keepalive_timer);

    if (dev->tx_thread) {
        kthread_stop(dev->tx_thread);
        dev->tx_thread = NULL;
    }

    if (dev->rx_thread) {
        kthread_stop(dev->rx_thread);
        dev->rx_thread = NULL;
    }

    if (dev->sock) {
        sock_release(dev->sock);
        dev->sock = NULL;
    }

    atomic_set(&dev->connected, 0);

    vpci_info("Disconnected\n");
}

void vpci_keepalive_timer(struct timer_list *t)
{
    struct vpci_device *dev = container_of(t, struct vpci_device, keepalive_timer);
    struct vpci_packet pkt;

    if (!atomic_read(&dev->connected))
        return;

    pkt.hdr.magic = cpu_to_be32(VPCI_MAGIC);
    pkt.hdr.version = cpu_to_be16(VPCI_VERSION);
    pkt.hdr.type = VPCI_MSG_KEEPALIVE;
    pkt.hdr.flags = 0;
    pkt.hdr.session_id = cpu_to_be32(atomic_read(&dev->session_id));
    pkt.hdr.seq_num = cpu_to_be32(atomic_inc_return(&dev->seq_num));
    pkt.hdr.length = cpu_to_be32(0);
    pkt.hdr.address = 0;
    pkt.data = NULL;

    vpci_packet_send(dev, &pkt);

    mod_timer(&dev->keepalive_timer, jiffies + VPCI_KEEPALIVE_INTERVAL);
}

void vpci_reconnect_work(struct work_struct *work)
{
    struct vpci_device *dev = container_of(work, struct vpci_device, reconnect_work);
    int ret, delay = VPCI_RECONNECT_DELAY;

    while (!atomic_read(&dev->connected)) {
        vpci_info("Attempting reconnection...\n");

        ret = vpci_net_connect(dev);
        if (ret == 0) {
            vpci_info("Reconnection successful\n");
            return;
        }

        vpci_warn("Reconnection failed, retrying in %d jiffies\n", delay);
        msleep(delay);

        delay = min(delay * 2, HZ * 30);
    }
}

int vpci_rx_thread(void *data)
{
    struct vpci_device *dev = data;

    vpci_info("RX thread started for device %d\n", dev->id);

    while (!kthread_should_stop()) {
        struct vpci_packet *pkt;
        int ret;

        pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
        if (!pkt) {
            msleep(1);
            continue;
        }

        pkt->data = kmalloc(VPCI_MAX_PAYLOAD, GFP_KERNEL);
        if (!pkt->data) {
            kfree(pkt);
            msleep(1);
            continue;
        }

        ret = kernel_recvmsg(dev->sock, &dev->msg, pkt->data, VPCI_MAX_PAYLOAD, 0, MSG_DONTWAIT);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
                kfree(pkt->data);
                kfree(pkt);
                msleep(1);
                continue;
            }

            vpci_err("RX error: %d\n", ret);
            atomic64_inc(&dev->stats.errors);
            kfree(pkt->data);
            kfree(pkt);

            if (ret == -ECONNRESET || ret == -ENOTCONN) {
                vpci_net_disconnect(dev);
                schedule_work(&dev->reconnect_work);
                break;
            }
            continue;
        }

        if (ret == 0) {
            vpci_info("Connection closed\n");
            kfree(pkt->data);
            kfree(pkt);
            vpci_net_disconnect(dev);
            schedule_work(&dev->reconnect_work);
            break;
        }

        pkt->hdr.length = ret;
        vpci_packet_process(dev, pkt);
        kfree(pkt->data);
        kfree(pkt);
    }

    vpci_info("RX thread stopped\n");
    return 0;
}

int vpci_tx_thread(void *data)
{
    struct vpci_device *dev = data;

    vpci_info("TX thread started for device %d\n", dev->id);

    while (!kthread_should_stop()) {
        struct vpci_packet *pkt;
        unsigned long flags;

        wait_event_interruptible(dev->tx_wait, 
                               !list_empty(&dev->tx_list) || kthread_should_stop());

        if (kthread_should_stop())
            break;

        spin_lock_irqsave(&dev->tx_lock, flags);
        if (list_empty(&dev->tx_list)) {
            spin_unlock_irqrestore(&dev->tx_lock, flags);
            continue;
        }
        pkt = list_first_entry(&dev->tx_list, struct vpci_packet, list);
        list_del(&pkt->list);
        spin_unlock_irqrestore(&dev->tx_lock, flags);

        if (atomic_read(&dev->connected) && dev->sock) {
            int ret = kernel_sendmsg(dev->sock, &dev->msg, pkt->data, pkt->hdr.length, 0);
            if (ret < 0) {
                vpci_err("TX error: %d\n", ret);
                atomic64_inc(&dev->stats.errors);
            } else {
                atomic64_inc(&dev->stats.tx_packets);
                atomic64_add(ret, &dev->stats.tx_bytes);
            }
        } else {
            atomic64_inc(&dev->stats.dropped);
        }

        kfree(pkt->data);
        kfree(pkt);
    }

    vpci_info("TX thread stopped\n");
    return 0;
}

void vpci_rx_work(struct task_struct *task)
{
    // Legacy function - now handled by vpci_rx_thread
}

void vpci_tx_work(struct task_struct *task)
{
    // Legacy function - now handled by vpci_tx_thread
}
