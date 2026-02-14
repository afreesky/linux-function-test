// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual PCIe over Ethernet - Packet Handling
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include "virtual_pcie.h"

static void vpci_send_ack(struct vpci_device *dev, u32 seq_num, int status)
{
    struct vpci_packet *pkt;

    pkt = kzalloc(sizeof(*pkt), GFP_ATOMIC);
    if (!pkt)
        return;

    pkt->data = kmalloc(sizeof(status), GFP_ATOMIC);
    if (!pkt->data) {
        kfree(pkt);
        return;
    }

    memcpy(pkt->data, &status, sizeof(status));

    pkt->hdr.magic = cpu_to_be32(VPCI_MAGIC);
    pkt->hdr.version = cpu_to_be16(VPCI_VERSION);
    pkt->hdr.type = VPCI_MSG_ACK;
    pkt->hdr.flags = 0;
    pkt->hdr.session_id = cpu_to_be32(atomic_read(&dev->session_id));
    pkt->hdr.seq_num = cpu_to_be32(seq_num);
    pkt->hdr.length = cpu_to_be32(sizeof(status));
    pkt->hdr.address = 0;

    vpci_packet_send(dev, pkt);
}

static void vpci_send_nack(struct vpci_device *dev, u32 seq_num, int error)
{
    struct vpci_packet *pkt;

    pkt = kzalloc(sizeof(*pkt), GFP_ATOMIC);
    if (!pkt)
        return;

    pkt->data = kmalloc(sizeof(error), GFP_ATOMIC);
    if (!pkt->data) {
        kfree(pkt);
        return;
    }

    memcpy(pkt->data, &error, sizeof(error));

    pkt->hdr.magic = cpu_to_be32(VPCI_MAGIC);
    pkt->hdr.version = cpu_to_be16(VPCI_VERSION);
    pkt->hdr.type = VPCI_MSG_NACK;
    pkt->hdr.flags = 0;
    pkt->hdr.session_id = cpu_to_be32(atomic_read(&dev->session_id));
    pkt->hdr.seq_num = cpu_to_be32(seq_num);
    pkt->hdr.length = cpu_to_be32(sizeof(error));
    pkt->hdr.address = 0;

    vpci_packet_send(dev, pkt);
}

int vpci_packet_send(struct vpci_device *dev, struct vpci_packet *pkt)
{
    unsigned long flags;

    if (!atomic_read(&dev->connected)) {
        vpci_debug("Device not connected, queuing failed\n");
        atomic64_inc(&dev->stats.dropped);
        return -ENOTCONN;
    }

    INIT_LIST_HEAD(&pkt->list);
    spin_lock_irqsave(&dev->tx_lock, flags);
    list_add_tail(&pkt->list, &dev->tx_list);
    spin_unlock_irqrestore(&dev->tx_lock, flags);

    wake_up_interruptible(&dev->tx_wait);

    return 0;
}

void vpci_packet_process(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u32 type = pkt->hdr.type;
    u32 seq_num = be32_to_cpu(pkt->hdr.seq_num);
    u32 length = be32_to_cpu(pkt->hdr.length);

    vpci_debug("Processing packet type=0x%02x seq=%u len=%u\n", type, seq_num, length);

    atomic64_inc(&dev->stats.rx_packets);
    atomic64_add(length, &dev->stats.rx_bytes);

    if (be32_to_cpu(pkt->hdr.magic) != VPCI_MAGIC) {
        vpci_warn("Invalid magic number\n");
        atomic64_inc(&dev->stats.errors);
        vpci_send_nack(dev, seq_num, -EBADMSG);
        return;
    }

    switch (type) {
    case VPCI_MSG_CONFIG_READ:
        vpci_handle_config_read(dev, pkt);
        break;

    case VPCI_MSG_CONFIG_WRITE:
        vpci_handle_config_write(dev, pkt);
        break;

    case VPCI_MSG_MEM_READ:
        vpci_handle_mem_read(dev, pkt);
        break;

    case VPCI_MSG_MEM_WRITE:
        vpci_handle_mem_write(dev, pkt);
        break;

    case VPCI_MSG_IRQ:
        vpci_handle_irq(dev, pkt);
        break;

    case VPCI_MSG_ACK:
        vpci_handle_ack(dev, pkt);
        break;

    case VPCI_MSG_NACK:
        vpci_handle_nack(dev, pkt);
        break;

    case VPCI_MSG_HANDSHAKE:
        vpci_handle_handshake(dev, pkt);
        break;

    case VPCI_MSG_KEEPALIVE:
        vpci_debug("Keepalive received\n");
        break;

    default:
        vpci_warn("Unknown packet type: 0x%02x\n", type);
        atomic64_inc(&dev->stats.errors);
        vpci_send_nack(dev, seq_num, -EINVAL);
        break;
    }
}

void vpci_handle_config_read(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u64 addr = be64_to_cpu(pkt->hdr.address);
    u32 len = be32_to_cpu(pkt->hdr.length);
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);
    struct vpci_packet *resp;
    void *data;

    vpci_debug("Config read addr=0x%llx len=%u\n", addr, len);

    if (addr + len > dev->config_size) {
        vpci_warn("Config read out of bounds: addr=0x%llx len=%u\n", addr, len);
        vpci_send_nack(dev, seq, -ERANGE);
        return;
    }

    data = kmalloc(len, GFP_KERNEL);
    if (!data) {
        vpci_send_nack(dev, seq, -ENOMEM);
        return;
    }

    spin_lock(&dev->lock);
    memcpy(data, dev->config_space + addr, len);
    spin_unlock(&dev->lock);

    resp = kzalloc(sizeof(*resp), GFP_KERNEL);
    if (!resp) {
        kfree(data);
        vpci_send_nack(dev, seq, -ENOMEM);
        return;
    }

    resp->data = data;
    resp->hdr.magic = cpu_to_be32(VPCI_MAGIC);
    resp->hdr.version = cpu_to_be16(VPCI_VERSION);
    resp->hdr.type = VPCI_MSG_ACK;
    resp->hdr.flags = 0;
    resp->hdr.session_id = cpu_to_be32(atomic_read(&dev->session_id));
    resp->hdr.seq_num = cpu_to_be32(seq);
    resp->hdr.length = cpu_to_be32(len);
    resp->hdr.address = 0;

    vpci_packet_send(dev, resp);
}

void vpci_handle_config_write(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u64 addr = be64_to_cpu(pkt->hdr.address);
    u32 len = be32_to_cpu(pkt->hdr.length);
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);

    vpci_debug("Config write addr=0x%llx len=%u\n", addr, len);

    if (addr + len > dev->config_size) {
        vpci_warn("Config write out of bounds: addr=0x%llx len=%u\n", addr, len);
        vpci_send_nack(dev, seq, -ERANGE);
        return;
    }

    spin_lock(&dev->lock);
    memcpy(dev->config_space + addr, pkt->data, len);
    spin_unlock(&dev->lock);

    vpci_send_ack(dev, seq, 0);
}

void vpci_handle_mem_read(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u64 addr = be64_to_cpu(pkt->hdr.address);
    u32 len = be32_to_cpu(pkt->hdr.length);
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);
    struct vpci_packet *resp;
    void *data;
    int ret;

    vpci_debug("Mem read addr=0x%llx len=%u\n", addr, len);

    data = kmalloc(len, GFP_KERNEL);
    if (!data) {
        vpci_send_nack(dev, seq, -ENOMEM);
        return;
    }

    ret = vpci_mem_read(dev, addr, data, len);
    if (ret < 0) {
        kfree(data);
        vpci_send_nack(dev, seq, ret);
        return;
    }

    resp = kzalloc(sizeof(*resp), GFP_KERNEL);
    if (!resp) {
        kfree(data);
        vpci_send_nack(dev, seq, -ENOMEM);
        return;
    }

    resp->data = data;
    resp->hdr.magic = cpu_to_be32(VPCI_MAGIC);
    resp->hdr.version = cpu_to_be16(VPCI_VERSION);
    resp->hdr.type = VPCI_MSG_ACK;
    resp->hdr.flags = 0;
    resp->hdr.session_id = cpu_to_be32(atomic_read(&dev->session_id));
    resp->hdr.seq_num = cpu_to_be32(seq);
    resp->hdr.length = cpu_to_be32(len);
    resp->hdr.address = 0;

    vpci_packet_send(dev, resp);
}

void vpci_handle_mem_write(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u64 addr = be64_to_cpu(pkt->hdr.address);
    u32 len = be32_to_cpu(pkt->hdr.length);
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);
    int ret;

    vpci_debug("Mem write addr=0x%llx len=%u\n", addr, len);

    ret = vpci_mem_write(dev, addr, pkt->data, len);
    if (ret < 0) {
        vpci_send_nack(dev, seq, ret);
        return;
    }

    vpci_send_ack(dev, seq, 0);
}

void vpci_handle_irq(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u32 irq_num = be32_to_cpu(pkt->hdr.address);
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);

    vpci_debug("IRQ received: %u\n", irq_num);

    if (dev->irq_handler) {
        dev->irq_handler(irq_num, dev->irq_data);
    }

    vpci_send_ack(dev, seq, 0);
}

void vpci_handle_ack(struct vpci_device *dev, struct vpci_packet *pkt)
{
    vpci_debug("ACK received for seq=%u\n", be32_to_cpu(pkt->hdr.seq_num));
    wake_up_interruptible(&dev->ack_wait);
}

void vpci_handle_nack(struct vpci_device *dev, struct vpci_packet *pkt)
{
    int error;
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);

    memcpy(&error, pkt->data, sizeof(error));
    vpci_warn("NACK received for seq=%u error=%d\n", seq, error);

    atomic64_inc(&dev->stats.errors);
}

void vpci_handle_handshake(struct vpci_device *dev, struct vpci_packet *pkt)
{
    u32 seq = be32_to_cpu(pkt->hdr.seq_num);
    u32 session = be32_to_cpu(pkt->hdr.session_id);

    vpci_info("Handshake received: session=%u\n", session);

    atomic_set(&dev->session_id, session);
    dev->handshake_status = 0;
    complete(&dev->handshake_comp);

    vpci_send_ack(dev, seq, 0);
}
