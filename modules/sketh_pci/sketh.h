#ifndef _SKETH_H
#define _SKETH_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ingress.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/bpf.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/xdp.h>

#define SKETH_DRIVER_NAME        "sketh"
#define SKETH_DRIVER_VERSION     "1.0.0"
#define SKETH_MAX_NUM_QUEUES     16
#define SKETH_DEFAULT_QUEUES     4
#define SKETH_RX_BUF_SIZE        2048
#define SKETH_TX_MAX_DESC        512
#define SKETH_RX_MAX_DESC        512

#define SKETH_TX_FLAGS_TSO       0x01
#define SKETH_TX_FLAGS_CSUM      0x02

#define __SKETH_STATE_DOWN       0
#define __SKETH_STATE_IN_IRQ     1

union sketh_rx_desc {
    struct {
        __le64 pkt_addr;
        __le64 hdr_addr;
        __le16 length;
        __le16 reserved;
        __le16 status;
        __le16 errors;
    } read;
};

union sketh_tx_desc {
    struct {
        __le64 pkt_addr;
        __le16 length;
        __u8 cso;
        __u8 cmd;
        __le16 status;
        __le16 reserved;
    } read;
    struct {
        __le64 addr;
        __le32 sflags;
        __le32 flags;
    } wb;
};

static inline u16 next_to_use(u16 index, u16 count)
{
    return (index + 1) & (count - 1);
}

static inline u16 next_to_clean(u16 index, u16 count)
{
    return (index + count - 1) & (count - 1);
}

struct sketh_adapter;
struct sketh_ring;
struct sketh_rx_buffer;

struct sketh_rx_buffer {
    struct sk_buff *skb;
    dma_addr_t dma;
    unsigned long handle;
};

struct sketh_tx_buffer {
    struct sk_buff *skb;
    dma_addr_t dma;
    unsigned int bytes;
    unsigned int mapped;
};

struct sketh_ring {
    struct sketh_adapter *adapter;
    void *desc;
    dma_addr_t desc_dma;
    unsigned int count;
    unsigned int size;
    unsigned int next_to_use;
    unsigned int next_to_clean;
    union {
        struct sketh_rx_buffer *rx_buffer;
        struct sketh_tx_buffer *tx_buffer;
    };
    struct napi_struct napi;
    struct net_device *netdev;
    struct xdp_rxq_info xdp_rxq;
    u16 queue_index;
    bool xdp_enabled;
    cpumask_t affinity_mask;
};

struct sketh_xdp_info {
    struct bpf_prog *prog;
};

struct sketh_adapter {
    struct net_device *netdev;
    struct pci_dev *pci_dev;
    struct msix_entry *msix_entries;
    struct sketh_ring *rx_ring;
    struct sketh_ring *tx_ring;
    struct sketh_xdp_info xdp_info;
    struct work_struct reset_task;
    struct work_struct watchdog_task;
    struct delayed_work service_task;
    unsigned long state;
    u64 tx_timeout_count;
    u64 tx_linearize;
    u64 tx_restart;
    u64 rx_drops;
    u64 rx_polls;
    u64 xdp_tx;
    u64 xdp_drops;
    u64 xdp_redirect;
    u64 tx_dropped;
    spinlock_t stats_lock;
    atomic_t tx_irq;
    atomic_t rx_irq;
    int num_queues;
    int max_queues;
    u32 msg_enable;
    bool dev_registered;
    bool msix_enabled;
    bool netpoll_enabled;
    bool hw_accel;
};

struct sketh_offload_info {
    struct nf_conn *conn;
    enum ip_conntrack_info ctinfo;
    struct net_device *indev;
    struct net_device *outdev;
    int hw_accel;
};

int sketh_netfilter_offload(struct sketh_adapter *adapter,
                             struct sk_buff *skb,
                             struct sketh_offload_info *info);
int sketh_hw_offload_enable(struct sketh_adapter *adapter);
int sketh_hw_offload_disable(struct sketh_adapter *adapter);
int sketh_hw_flow_table_create(struct sketh_adapter *adapter, u32 size);
int sketh_hw_flow_table_destroy(struct sketh_adapter *adapter);

int sketh_xdp_setup_prog(struct sketh_adapter *adapter, struct bpf_prog *prog);
int sketh_xdp(struct sketh_ring *rx_ring, struct sk_buff *skb);
int sketh_xdp_tx(struct sketh_adapter *adapter, struct xdp_buff *xdp);

int sketh_register_netfilter(struct sketh_adapter *adapter);
void sketh_unregister_netfilter(struct sketh_adapter *adapter);

int sketh_open(struct net_device *netdev);
int sketh_stop(struct net_device *netdev);
netdev_tx_t sketh_start_xmit(struct sk_buff *skb, struct net_device *netdev);
void sketh_tx_timeout(struct net_device *netdev, unsigned int txqueue);

int sketh_request_irqs(struct sketh_adapter *adapter);
void sketh_free_irqs(struct sketh_adapter *adapter);
irqreturn_t sketh_msix_ring(int irq, void *data);
irqreturn_t sketh_msix_mbx(int irq, void *data);

int sketh_napi_poll(struct napi_struct *napi, int budget);
int sketh_clean_tx_ring(struct sketh_ring *tx_ring, int budget);
int sketh_clean_rx_ring(struct sketh_ring *rx_ring, int budget);
bool sketh_alloc_rx_buffers(struct sketh_ring *rx_ring, int cleaned_count);

void sketh_receive_skb(struct sketh_ring *rx_ring, struct sk_buff *skb, unsigned int length);

void sketh_update_stats(struct sketh_adapter *adapter);
int sketh_xmit_desc(struct sketh_ring *tx_ring, dma_addr_t dma, unsigned int len, unsigned int tx_flags);

#endif
