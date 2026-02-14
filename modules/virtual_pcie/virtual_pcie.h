/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual PCIe over Ethernet - Core Header
 *
 * This module provides virtual PCIe transport over Ethernet using TCP sockets.
 * Supports both Root Complex (RC) and Endpoint (EP) modes.
 *
 * Copyright (C) 2024 Virtual PCIe Contributors
 */

#ifndef _VIRTUAL_PCIE_H
#define _VIRTUAL_PCIE_H

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/io.h>

/* Protocol constants */
#define VPCI_ETH_TYPE        0x88B5
#define VPCI_VERSION         0x0001
#define VPCI_MAX_PAYLOAD     1460
#define VPCI_MAX_BARS        6
#define VPCI_MAGIC           0x56494345
#define VPCI_SHMEM_SIZE      (4 * 1024 * 1024)
#define VPCI_MAX_DEVS        16
#define VPCI_NAME            "virtual_pcie"
#define VPCI_KEEPALIVE_INTERVAL (HZ * 5)
#define VPCI_RECONNECT_DELAY (HZ * 2)
#define VPCI_TIMEOUT         (HZ * 10)

/* Message types */
enum vpci_msg_type {
    VPCI_MSG_CONFIG_READ  = 0x01,
    VPCI_MSG_CONFIG_WRITE = 0x02,
    VPCI_MSG_MEM_READ     = 0x03,
    VPCI_MSG_MEM_WRITE    = 0x04,
    VPCI_MSG_IRQ          = 0x05,
    VPCI_MSG_DMA_READ     = 0x06,
    VPCI_MSG_DMA_WRITE    = 0x07,
    VPCI_MSG_ACK          = 0x10,
    VPCI_MSG_NACK         = 0x11,
    VPCI_MSG_HANDSHAKE    = 0x20,
    VPCI_MSG_KEEPALIVE    = 0x21,
};

/* Virtual PCIe device role */
enum vpci_role {
    VPCI_ROLE_NONE,
    VPCI_ROLE_RC,
    VPCI_ROLE_EP,
    VPCI_ROLE_LOOPBACK,
};

/* BAR information */
struct vpci_bar_info {
    phys_addr_t    phys_addr;
    resource_size_t size;
    void __iomem   *addr;
    int            flags;
};

/* Virtual PCIe device state */
struct vpci_device {
    int                 id;
    enum vpci_role      role;
    struct device       *dev;
    struct socket       *sock;
    struct sockaddr_in  remote_addr;
    struct sockaddr_in  local_addr;
    struct work_struct  reconnect_work;
    struct timer_list   keepalive_timer;
    atomic_t            connected;
    atomic_t            session_id;
    atomic_t            seq_num;
    struct completion   handshake_comp;
    int                 handshake_status;
    spinlock_t          lock;
    atomic_t            refcount;
    struct completion   shutdown_comp;
    struct sk_buff_head tx_queue;
    wait_queue_head_t   tx_wait;
    wait_queue_head_t   ack_wait;
    struct list_head    tx_list;
    spinlock_t          tx_lock;
    struct task_struct  *rx_thread;
    struct task_struct  *tx_thread;
    struct msghdr       msg;
    struct vpci_bar_info bars[VPCI_MAX_BARS];
    int                 bar_count;
    void                *config_space;
    size_t              config_size;
    void                *shmem;
    size_t              shmem_size;
    spinlock_t          shmem_lock;
    irq_handler_t       irq_handler;
    void                *irq_data;
    unsigned long       irq_mask;
    struct {
        atomic64_t rx_packets;
        atomic64_t tx_packets;
        atomic64_t rx_bytes;
        atomic64_t tx_bytes;
        atomic64_t errors;
        atomic64_t dropped;
        atomic64_t timeouts;
    } stats;
};

/* Packet structures */
struct vpci_pkt_header {
    __be32  magic;
    __be16  version;
    __u8    type;
    __u8    flags;
    __be32  session_id;
    __be32  seq_num;
    __be32  length;
    __be64  address;
} __attribute__((packed));

struct vpci_packet {
    struct vpci_pkt_header hdr;
    void                  *data;
    struct list_head      list;
};

/* Module parameters */
extern char *vpci_remote_ip;
extern int vpci_remote_port;
extern int vpci_local_port;
extern bool vpci_loopback;
extern int vpci_debug;

#define vpci_debug_level vpci_debug

#define vpci_err(fmt, ...) \
    printk(KERN_ERR VPCI_NAME "[%d] ERROR: " fmt, __LINE__, ##__VA_ARGS__)
#define vpci_warn(fmt, ...) \
    printk(KERN_WARNING VPCI_NAME "[%d] WARNING: " fmt, __LINE__, ##__VA_ARGS__)
#define vpci_info(fmt, ...) \
    printk(KERN_INFO VPCI_NAME "[%d] INFO: " fmt, __LINE__, ##__VA_ARGS__)
#define vpci_debug(fmt, ...) \
    do { \
        if (vpci_debug_level >= 3) \
            printk(KERN_DEBUG VPCI_NAME "[%d] DEBUG: " fmt, __LINE__, ##__VA_ARGS__); \
    } while (0)

#define vpci_trace() \
    vpci_debug("trace: %s:%d\n", __func__, __LINE__)

/* IOCTL commands */
#define VPCI_IOCTL_CONNECT     _IO('V', 0x01)
#define VPCI_IOCTL_DISCONNECT  _IO('V', 0x02)
#define VPCI_IOCTL_GET_STATUS  _IOR('V', 0x03, int)
#define VPCI_IOCTL_SET_ROLE   _IOW('V', 0x04, int)

/* Function prototypes */
int vpci_net_init(void);
void vpci_net_exit(void);
int vpci_packet_send(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_packet_process(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_rx_work(struct task_struct *task);
void vpci_tx_work(struct task_struct *task);
int vpci_rx_thread(void *data);
int vpci_tx_thread(void *data);
void vpci_keepalive_work(struct work_struct *work);
void vpci_reconnect_work(struct work_struct *work);
int vpci_net_connect(struct vpci_device *dev);
void vpci_net_disconnect(struct vpci_device *dev);
void vpci_keepalive_timer(struct timer_list *t);
int vpci_bar_map(struct vpci_device *dev, int bar_num,
                resource_size_t addr, resource_size_t size);
void vpci_bar_unmap(struct vpci_device *dev, int bar_num);
int vpci_config_read(struct vpci_device *dev, u64 addr, void *data, u32 len);
int vpci_config_write(struct vpci_device *dev, u64 addr, void *data, u32 len);
int vpci_mem_read(struct vpci_device *dev, u64 addr, void *data, u32 len);
int vpci_mem_write(struct vpci_device *dev, u64 addr, void *data, u32 len);
void vpci_handle_config_read(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_config_write(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_mem_read(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_mem_write(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_irq(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_ack(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_nack(struct vpci_device *dev, struct vpci_packet *pkt);
void vpci_handle_handshake(struct vpci_device *dev, struct vpci_packet *pkt);

#endif /* _VIRTUAL_PCIE_H */
