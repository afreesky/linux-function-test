#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_conntrack.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/version.h>

#include <uapi/linux/bpf_common.h>

#include "sketh.h"

#define SKETH_NAPI_WEIGHT 64
#define SKETH_MIN_MTU 68
#define SKETH_MAX_MTU 9000
#define SKETH_DEFAULT_MTU 1500
#define SKETH_TX_DESC_CMD_EOP 0x01
#define SKETH_TX_DESC_CMD_RS 0x02

static int debug = -1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level");

static int num_queues = SKETH_DEFAULT_QUEUES;
module_param(num_queues, int, 0);
MODULE_PARM_DESC(num_queues, "Number of queue pairs");

static int mtu = SKETH_DEFAULT_MTU;
module_param(mtu, int, 0);
MODULE_PARM_DESC(mtu, "Maximum Transfer Unit");

#define sketh_debug(d, level, fmt, args...)          \
    do {                                             \
        if ((d)->msg_enable & (level))               \
            netdev_info(d->netdev, fmt, ##args);    \
    } while (0)

#define sketh_err(d, fmt, args...)    netdev_err(d->netdev, fmt, ##args)
#define sketh_warn(d, fmt, args...)   netdev_warn(d->netdev, fmt, ##args)
#define sketh_info(d, fmt, args...)  netdev_info(d->netdev, fmt, ##args)

static int sketh_alloc_queues(struct sketh_adapter *adapter);
static void sketh_free_queues(struct sketh_adapter *adapter);
static int sketh_setup_rx_resources(struct sketh_ring *rx_ring);
static int sketh_setup_tx_resources(struct sketh_ring *tx_ring);
static void sketh_free_rx_resources(struct sketh_ring *rx_ring);
static void sketh_free_tx_resources(struct sketh_ring *tx_ring);
static void sketh_configure_tx(struct sketh_adapter *adapter);
static void sketh_configure_rx(struct sketh_adapter *adapter);
static void sketh_configure(struct sketh_adapter *adapter);
static void sketh_unmap_and_free_tx_buffer(struct sketh_ring *tx_ring,
                                           struct sketh_tx_buffer *tx_buffer);

static inline void sketh_enable_irq(struct sketh_ring *ring)
{
    if (ring->adapter->msix_entries)
        enable_irq(ring->adapter->msix_entries[ring->queue_index].vector);
}

static inline void sketh_disable_irq(struct sketh_ring *ring)
{
    if (ring->adapter->msix_entries)
        disable_irq(ring->adapter->msix_entries[ring->queue_index].vector);
}

int sketh_xmit_desc(struct sketh_ring *tx_ring, dma_addr_t dma, unsigned int len, unsigned int tx_flags)
{
    union sketh_tx_desc *desc;
    unsigned int i;

    i = tx_ring->next_to_use;
    desc = &((union sketh_tx_desc *)tx_ring->desc)[i];

    desc->read.pkt_addr = dma;
    desc->read.length = len;
    desc->read.cmd = SKETH_TX_DESC_CMD_EOP | SKETH_TX_DESC_CMD_RS;

    if (tx_flags & SKETH_TX_FLAGS_CSUM)
        desc->read.cso = 0;

    tx_ring->next_to_use = next_to_use(tx_ring->next_to_use, tx_ring->count);

    return 0;
}

bool
sketh_alloc_rx_buffers(struct sketh_ring *rx_ring, int cleaned_count)
{
    struct sketh_rx_buffer *bi;
    unsigned int i;
    unsigned int ntu;

    i = rx_ring->next_to_use;
    bi = &rx_ring->rx_buffer[i];

    while (cleaned_count--) {
        struct sk_buff *skb;

        skb = __netdev_alloc_skb(rx_ring->netdev,
                                  rx_ring->adapter->netdev->mtu + ETH_HLEN + VLAN_HLEN,
                                  GFP_ATOMIC);
        if (!skb) {
            rx_ring->adapter->rx_drops++;
            break;
        }

        bi->skb = skb;
        bi->dma = dma_map_single(rx_ring->adapter->pci_dev->dev.parent,
                                  skb->data,
                                  rx_ring->adapter->netdev->mtu + ETH_HLEN + VLAN_HLEN,
                                  DMA_FROM_DEVICE);

        if (dma_mapping_error(rx_ring->adapter->pci_dev->dev.parent, bi->dma)) {
            dev_kfree_skb(skb);
            bi->skb = NULL;
            rx_ring->adapter->rx_drops++;
            break;
        }

        ntu = next_to_use(i, rx_ring->count);
        bi = &rx_ring->rx_buffer[ntu];
        i = ntu;
    }

    rx_ring->next_to_use = i;
    return 0;
}

static bool
sketh_alloc_tx_buffers(struct sketh_ring *tx_ring)
{
    struct sketh_tx_buffer *bi;
    unsigned int i;

    i = tx_ring->next_to_use;
    bi = &tx_ring->tx_buffer[i];

    while (tx_ring->next_to_clean != tx_ring->next_to_use) {
        bi->skb = NULL;
        bi->dma = 0;
        bi->bytes = 0;
        bi->mapped = 0;

        tx_ring->next_to_use = next_to_use(tx_ring->next_to_use, tx_ring->count);
        bi = &tx_ring->tx_buffer[tx_ring->next_to_use];
    }

    return true;
}

static void
sketh_unmap_and_free_tx_buffer(struct sketh_ring *tx_ring,
                                struct sketh_tx_buffer *tx_buffer)
{
    if (tx_buffer->mapped) {
        dma_unmap_single(tx_ring->adapter->pci_dev->dev.parent,
                         tx_buffer->dma,
                         tx_buffer->bytes,
                         DMA_TO_DEVICE);
    }

    if (tx_buffer->skb) {
        dev_kfree_skb_any(tx_buffer->skb);
    }

    tx_buffer->skb = NULL;
    tx_buffer->dma = 0;
    tx_buffer->bytes = 0;
    tx_buffer->mapped = false;
}

int
sketh_clean_tx_ring(struct sketh_ring *tx_ring, int budget)
{
    struct sketh_tx_buffer *tx_buffer;
    unsigned int total_bytes = 0;
    unsigned int total_packets = 0;
    unsigned int i;

    i = tx_ring->next_to_clean;

    while (i != tx_ring->next_to_use) {
        tx_buffer = &tx_ring->tx_buffer[i];

        if (tx_buffer->skb) {
            total_bytes += tx_buffer->bytes;
            total_packets++;
            sketh_unmap_and_free_tx_buffer(tx_ring, tx_buffer);
        }

        i = next_to_use(i, tx_ring->count);
    }

    if (total_packets) {
        struct netdev_queue *txq = netdev_get_tx_queue(tx_ring->netdev,
                                                        tx_ring->queue_index);

        tx_ring->netdev->stats.tx_packets += total_packets;
        tx_ring->netdev->stats.tx_bytes += total_bytes;
        netif_tx_wake_queue(txq);
    }

    tx_ring->next_to_clean = i;

    return 0;
}

void
sketh_receive_skb(struct sketh_ring *rx_ring, struct sk_buff *skb, unsigned int length)
{
    struct sketh_adapter *adapter = rx_ring->adapter;
    struct net_device *netdev = rx_ring->netdev;
    int ret;

    if (adapter->xdp_info.prog) {
        struct xdp_buff xdp;
        u32 act;

        xdp_init_buff(&xdp, SKETH_RX_BUF_SIZE, &rx_ring->xdp_rxq);
        xdp_prepare_buff(&xdp, skb->data, 0, length, false);

        act = bpf_prog_run(adapter->xdp_info.prog, &xdp);

        switch (act) {
        case XDP_PASS:
            break;
        case XDP_TX:
            sketh_xdp_tx(adapter, &xdp);
            adapter->xdp_tx++;
            dev_kfree_skb(skb);
            return;
        case XDP_REDIRECT:
            if (xdp_do_redirect(adapter->netdev, &xdp, adapter->xdp_info.prog) == 0) {
                adapter->xdp_redirect++;
            } else {
                adapter->xdp_drops++;
            }
            dev_kfree_skb(skb);
            return;
        case XDP_DROP:
            adapter->xdp_drops++;
            dev_kfree_skb(skb);
            return;
        default:
            bpf_warn_invalid_xdp_action(act);
            fallthrough;
        case XDP_ABORTED:
            adapter->xdp_drops++;
            dev_kfree_skb(skb);
            return;
        }
    }

    skb_put(skb, length);
    skb->protocol = eth_type_trans(skb, netdev);
    skb->ip_summed = CHECKSUM_UNNECESSARY;

    ret = netif_receive_skb(skb);
    if (ret == NET_RX_DROP) {
        adapter->rx_drops++;
    }
}

int
sketh_clean_rx_ring(struct sketh_ring *rx_ring, int budget)
{
    struct sketh_adapter *adapter = rx_ring->adapter;
    struct sketh_rx_buffer *rx_buffer;
    struct sk_buff *skb;
    unsigned int total_packets = 0;
    unsigned int total_bytes = 0;
    int cleaned = 0;
    unsigned int length;

    while (total_packets < budget) {
        rx_buffer = &rx_ring->rx_buffer[rx_ring->next_to_clean];
        skb = rx_buffer->skb;

        if (!skb)
            break;

        dma_unmap_single(adapter->pci_dev->dev.parent,
                         rx_buffer->dma,
                         rx_ring->adapter->netdev->mtu + ETH_HLEN + VLAN_HLEN,
                         DMA_FROM_DEVICE);

        length = rx_ring->adapter->netdev->mtu + ETH_HLEN;

        rx_buffer->skb = NULL;
        rx_buffer->dma = 0;

        sketh_receive_skb(rx_ring, skb, length);

        total_packets++;
        total_bytes += length;
        cleaned++;

        rx_ring->next_to_clean = next_to_use(rx_ring->next_to_clean,
                                              rx_ring->count);
    }

    if (cleaned) {
        sketh_alloc_rx_buffers(rx_ring, cleaned);
        rx_ring->netdev->stats.rx_packets += total_packets;
        rx_ring->netdev->stats.rx_bytes += total_bytes;
    }

    return total_packets;
}

int
sketh_napi_poll(struct napi_struct *napi, int budget)
{
    struct sketh_ring *rx_ring = container_of(napi, struct sketh_ring, napi);
    struct sketh_adapter *adapter = rx_ring->adapter;
    int work_done = 0;

    adapter->rx_polls++;

    work_done += sketh_clean_rx_ring(rx_ring, budget);

    if (rx_ring->next_to_use != rx_ring->next_to_clean) {
        if (work_done < budget) {
            napi_complete_done(napi, work_done);
            sketh_enable_irq(rx_ring);
        }
    } else {
        if (work_done < budget) {
            napi_complete_done(napi, work_done);
        }
    }

    if (budget > 0) {
        sketh_clean_tx_ring(&adapter->tx_ring[rx_ring->queue_index], budget);
    }

    return work_done;
}

static void
sketh_set_rx_buffer_len(struct sketh_adapter *adapter)
{
    struct sketh_ring *rx_ring;
    int i;

    for (i = 0; i < adapter->num_queues; i++) {
        rx_ring = &adapter->rx_ring[i];
        if (rx_ring->xdp_enabled)
            rx_ring->rx_buffer[0].skb = NULL;
    }
}

static void
sketh_configure_rx(struct sketh_adapter *adapter)
{
    int i;

    sketh_set_rx_buffer_len(adapter);

    for (i = 0; i < adapter->num_queues; i++) {
        struct sketh_ring *rx_ring = &adapter->rx_ring[i];

        sketh_alloc_rx_buffers(rx_ring, rx_ring->count);
    }
}

static void
sketh_configure_tx(struct sketh_adapter *adapter)
{
    int i;

    for (i = 0; i < adapter->num_queues; i++) {
        struct sketh_ring *tx_ring = &adapter->tx_ring[i];
        sketh_alloc_tx_buffers(tx_ring);
    }
}

static void
sketh_configure(struct sketh_adapter *adapter)
{
    struct net_device *netdev = adapter->netdev;

    sketh_configure_tx(adapter);
    sketh_configure_rx(adapter);

    netdev->features = NETIF_F_SG | NETIF_F_HW_CSUM |
                       NETIF_F_NTUPLE | NETIF_F_RXCSUM |
                       NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX |
                       NETIF_F_TSO | NETIF_F_TSO6;

    netdev->hw_features = netdev->features;
    netdev->vlan_features = netdev->features & ~NETIF_F_HW_VLAN_CTAG_RX;
}

netdev_tx_t
sketh_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    struct sketh_ring *tx_ring;
    struct sketh_tx_buffer *tx_buffer;
    unsigned int tx_flags = 0;
    unsigned int len;
    dma_addr_t dma;
    int ret;

    if (skb->len == 0) {
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    tx_ring = &adapter->tx_ring[skb->queue_mapping];

    if (unlikely(test_bit(__SKETH_STATE_DOWN, &adapter->state))) {
        dev_kfree_skb_any(skb);
        netdev->stats.tx_dropped++;
        return NETDEV_TX_OK;
    }

    if (skb_is_gso(skb)) {
        tx_flags |= SKETH_TX_FLAGS_TSO;
    } else if (skb->ip_summed == CHECKSUM_PARTIAL) {
        tx_flags |= SKETH_TX_FLAGS_CSUM;
    }

    len = skb_headlen(skb);
    dma = dma_map_single(adapter->pci_dev->dev.parent,
                         skb->data, len, DMA_TO_DEVICE);

    if (dma_mapping_error(adapter->pci_dev->dev.parent, dma)) {
        adapter->tx_dropped++;
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    tx_buffer = &tx_ring->tx_buffer[tx_ring->next_to_use];
    tx_buffer->skb = skb;
    tx_buffer->dma = dma;
    tx_buffer->bytes = len;
    tx_buffer->mapped = 1;

    tx_ring->next_to_use = next_to_use(tx_ring->next_to_use, tx_ring->count);

    ret = sketh_xmit_desc(tx_ring, dma, len, tx_flags);
    if (ret) {
        adapter->tx_dropped++;
        sketh_unmap_and_free_tx_buffer(tx_ring, tx_buffer);
        return NETDEV_TX_OK;
    }

    netif_trans_update(netdev);

    if (tx_ring->next_to_use == tx_ring->next_to_clean)
        adapter->tx_restart++;

    return NETDEV_TX_OK;
}

static void
sketh_set_rx_mode(struct net_device *netdev)
{
    if (netdev->flags & IFF_PROMISC) {
    }

    if (netdev->flags & IFF_ALLMULTI) {
    }

    if (netdev_mc_count(netdev) > 0) {
    }

    if (netdev_uc_count(netdev) > 0) {
    }
}

static int
sketh_set_mac_address(struct net_device *netdev, void *addr)
{
    struct sockaddr *sa = addr;

    if (!is_valid_ether_addr(sa->sa_data))
        return -EADDRNOTAVAIL;

    memcpy(netdev->dev_addr, sa->sa_data, ETH_ALEN);

    return 0;
}

static int
sketh_change_mtu(struct net_device *netdev, int new_mtu)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    int max_frame = new_mtu + ETH_HLEN + ETH_FCS_LEN;

    if ((new_mtu < SKETH_MIN_MTU) || (max_frame > SKETH_MAX_MTU))
        return -EINVAL;

    netdev->mtu = new_mtu;

    return 0;
}

void
sketh_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);

    adapter->tx_timeout_count++;

    schedule_work(&adapter->reset_task);
}

int
sketh_open(struct net_device *netdev)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    int err;
    int i;

    if (test_bit(__SKETH_STATE_DOWN, &adapter->state))
        return -EBUSY;

    err = sketh_request_irqs(adapter);
    if (err) {
        sketh_err(adapter, "Unable to allocate interrupts\n");
        return err;
    }

    sketh_configure(adapter);

    for (i = 0; i < adapter->num_queues; i++) {
        napi_enable(&adapter->rx_ring[i].napi);
    }

    clear_bit(__SKETH_STATE_DOWN, &adapter->state);

    netif_tx_start_all_queues(netdev);

    return 0;
}

int
sketh_stop(struct net_device *netdev)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    int i;

    set_bit(__SKETH_STATE_DOWN, &adapter->state);
    netif_tx_stop_all_queues(netdev);

    for (i = 0; i < adapter->num_queues; i++) {
        napi_disable(&adapter->rx_ring[i].napi);
    }

    sketh_free_irqs(adapter);

    for (i = 0; i < adapter->num_queues; i++) {
        struct sketh_ring *tx_ring = &adapter->tx_ring[i];
        struct sketh_ring *rx_ring = &adapter->rx_ring[i];

        if (tx_ring->desc)
            sketh_free_tx_resources(tx_ring);

        if (rx_ring->desc)
            sketh_free_rx_resources(rx_ring);
    }

    return 0;
}

static int
sketh_setup_rx_resources(struct sketh_ring *rx_ring)
{
    struct sketh_adapter *adapter = rx_ring->adapter;
    int size;

    size = sizeof(struct sketh_rx_buffer) * rx_ring->count;
    rx_ring->rx_buffer = vzalloc(size);

    if (!rx_ring->rx_buffer)
        return -ENOMEM;

    rx_ring->desc = dma_alloc_coherent(adapter->pci_dev->dev.parent,
                                       rx_ring->size,
                                       &rx_ring->desc_dma,
                                       GFP_KERNEL);

    if (!rx_ring->desc) {
        vfree(rx_ring->rx_buffer);
        rx_ring->rx_buffer = NULL;
        return -ENOMEM;
    }

    rx_ring->next_to_clean = 0;
    rx_ring->next_to_use = 0;

    return 0;
}

static int
sketh_setup_tx_resources(struct sketh_ring *tx_ring)
{
    struct sketh_adapter *adapter = tx_ring->adapter;
    int size;

    size = sizeof(struct sketh_tx_buffer) * tx_ring->count;
    tx_ring->tx_buffer = vzalloc(size);

    if (!tx_ring->tx_buffer)
        return -ENOMEM;

    tx_ring->desc = dma_alloc_coherent(adapter->pci_dev->dev.parent,
                                       tx_ring->size,
                                       &tx_ring->desc_dma,
                                       GFP_KERNEL);

    if (!tx_ring->desc) {
        vfree(tx_ring->tx_buffer);
        tx_ring->tx_buffer = NULL;
        return -ENOMEM;
    }

    tx_ring->next_to_clean = 0;
    tx_ring->next_to_use = 0;

    return 0;
}

static void
sketh_free_rx_resources(struct sketh_ring *rx_ring)
{
    if (rx_ring->rx_buffer) {
        vfree(rx_ring->rx_buffer);
        rx_ring->rx_buffer = NULL;
    }

    if (rx_ring->desc && rx_ring->desc_dma) {
        dma_free_coherent(rx_ring->adapter->pci_dev->dev.parent,
                          rx_ring->size,
                          rx_ring->desc,
                          rx_ring->desc_dma);
        rx_ring->desc = NULL;
        rx_ring->desc_dma = 0;
    }
}

static void
sketh_free_tx_resources(struct sketh_ring *tx_ring)
{
    if (tx_ring->tx_buffer) {
        vfree(tx_ring->tx_buffer);
        tx_ring->tx_buffer = NULL;
    }

    if (tx_ring->desc && tx_ring->desc_dma) {
        dma_free_coherent(tx_ring->adapter->pci_dev->dev.parent,
                          tx_ring->size,
                          tx_ring->desc,
                          tx_ring->desc_dma);
        tx_ring->desc = NULL;
        tx_ring->desc_dma = 0;
    }
}

static int
sketh_alloc_queues(struct sketh_adapter *adapter)
{
    int i;

    adapter->rx_ring = vzalloc(sizeof(struct sketh_ring) * adapter->num_queues);
    adapter->tx_ring = vzalloc(sizeof(struct sketh_ring) * adapter->num_queues);

    if (!adapter->rx_ring || !adapter->tx_ring)
        return -ENOMEM;

    for (i = 0; i < adapter->num_queues; i++) {
        struct sketh_ring *rx_ring = &adapter->rx_ring[i];
        struct sketh_ring *tx_ring = &adapter->tx_ring[i];

        rx_ring->adapter = adapter;
        tx_ring->adapter = adapter;

        rx_ring->queue_index = i;
        tx_ring->queue_index = i;

        rx_ring->count = SKETH_RX_MAX_DESC;
        tx_ring->count = SKETH_TX_MAX_DESC;

        rx_ring->size = rx_ring->count * sizeof(union sketh_rx_desc);
        tx_ring->size = tx_ring->count * sizeof(union sketh_tx_desc);

        rx_ring->netdev = adapter->netdev;
        tx_ring->netdev = adapter->netdev;
    }

    return 0;
}

static void
sketh_free_queues(struct sketh_adapter *adapter)
{
    if (adapter->rx_ring) {
        vfree(adapter->rx_ring);
        adapter->rx_ring = NULL;
    }

    if (adapter->tx_ring) {
        vfree(adapter->tx_ring);
        adapter->tx_ring = NULL;
    }
}

int
sketh_xdp_setup_prog(struct sketh_adapter *adapter, struct bpf_prog *prog)
{
    struct bpf_prog *old_prog;

    if (!prog)
        return 0;

    old_prog = xchg(&adapter->xdp_info.prog, prog);

    if (old_prog)
        bpf_prog_put(old_prog);

    return 0;
}

int
sketh_xdp_tx(struct sketh_adapter *adapter, struct xdp_buff *xdp)
{
    struct sketh_ring *tx_ring;
    struct sketh_tx_buffer *tx_buffer;
    dma_addr_t dma;

    tx_ring = &adapter->tx_ring[0];

    dma = dma_map_single(adapter->pci_dev->dev.parent,
                        xdp->data, xdp->data_end - xdp->data,
                        DMA_TO_DEVICE);

    if (dma_mapping_error(adapter->pci_dev->dev.parent, dma))
        return -ENOMEM;

    tx_buffer = &tx_ring->tx_buffer[tx_ring->next_to_use];
    tx_buffer->dma = dma;
    tx_buffer->bytes = xdp->data_end - xdp->data;
    tx_buffer->mapped = 1;

    tx_ring->next_to_use = next_to_use(tx_ring->next_to_use, tx_ring->count);

    return 0;
}

static int
sketh_set_features(struct net_device *netdev, netdev_features_t features)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    netdev_features_t changed = netdev->features ^ features;

    if (changed & NETIF_F_NTUPLE) {
    }

    return 0;
}

int
sketh_netfilter_offload(struct sketh_adapter *adapter,
                        struct sk_buff *skb,
                        struct sketh_offload_info *info)
{
    if (!info->conn)
        return -EINVAL;

    if (!adapter->hw_accel)
        return -EOPNOTSUPP;

    return 0;
}

int
sketh_hw_offload_enable(struct sketh_adapter *adapter)
{
    adapter->hw_accel = 1;
    return 0;
}

int
sketh_hw_offload_disable(struct sketh_adapter *adapter)
{
    adapter->hw_accel = 0;
    return 0;
}

int
sketh_hw_flow_table_create(struct sketh_adapter *adapter, unsigned int size)
{
    return 0;
}

int
sketh_hw_flow_table_destroy(struct sketh_adapter *adapter)
{
    return 0;
}

static unsigned int
sketh_nf_in(void *priv,
            struct sk_buff *skb,
            const struct nf_hook_state *state)
{
    struct sketh_adapter *adapter;
    struct sketh_offload_info offload_info;
    struct nf_conn *ct;
    enum ip_conntrack_info ctinfo;
    int ret;

    if (!priv)
        return NF_ACCEPT;

    adapter = (struct sketh_adapter *)priv;

    ct = nf_ct_get(skb, &ctinfo);
    if (!ct)
        return NF_ACCEPT;

    if (!test_bit(IPS_OFFLOAD_BIT, &ct->status))
        return NF_ACCEPT;

    memset(&offload_info, 0, sizeof(offload_info));
    offload_info.conn = ct;
    offload_info.ctinfo = ctinfo;
    offload_info.indev = state->in;
    offload_info.outdev = state->out;

    ret = sketh_netfilter_offload(adapter, skb, &offload_info);

    return ret == 0 ? NF_STOLEN : NF_ACCEPT;
}

static struct nf_hook_ops sketh_nf_hook_ops = {
    .hook     = sketh_nf_in,
    .pf       = NFPROTO_INET,
    .hooknum  = NF_INET_FORWARD,
    .priority = NF_IP_PRI_FIRST,
};

int
sketh_register_netfilter(struct sketh_adapter *adapter)
{
    sketh_nf_hook_ops.priv = adapter;
    return nf_register_net_hook(&init_net, &sketh_nf_hook_ops);
}

void
sketh_unregister_netfilter(struct sketh_adapter *adapter)
{
    nf_unregister_net_hook(&init_net, &sketh_nf_hook_ops);
}

irqreturn_t
sketh_msix_ring(int irq, void *data)
{
    struct sketh_ring *ring = (struct sketh_ring *)data;
    struct sketh_adapter *adapter = ring->adapter;

    napi_schedule(&ring->napi);

    return IRQ_HANDLED;
}

irqreturn_t
sketh_msix_mbx(int irq, void *data)
{
    return IRQ_HANDLED;
}

int
sketh_request_irqs(struct sketh_adapter *adapter)
{
    int i, err;

    for (i = 0; i < adapter->num_queues; i++) {
        struct sketh_ring *rx_ring = &adapter->rx_ring[i];
        struct msix_entry *entry = &adapter->msix_entries[i];

        err = request_irq(entry->vector,
                         sketh_msix_ring,
                         0,
                         adapter->netdev->name,
                         rx_ring);

        if (err) {
            sketh_err(adapter, "Request_irq failed for vector %d\n", i);
            goto err_free_irq;
        }

        irq_set_affinity_hint(entry->vector, &rx_ring->affinity_mask);
    }

    return 0;

err_free_irq:
    while (i--) {
        struct msix_entry *entry = &adapter->msix_entries[i];
        struct sketh_ring *rx_ring = &adapter->rx_ring[i];
        free_irq(entry->vector, rx_ring);
    }

    return err;
}

void
sketh_free_irqs(struct sketh_adapter *adapter)
{
    int i;

    for (i = 0; i < adapter->num_queues; i++) {
        if (adapter->msix_entries && adapter->msix_entries[i].vector) {
            struct sketh_ring *rx_ring = &adapter->rx_ring[i];
            free_irq(adapter->msix_entries[i].vector, rx_ring);
        }
    }

    if (adapter->msix_entries) {
        vfree(adapter->msix_entries);
        adapter->msix_entries = NULL;
    }
}

void
sketh_update_stats(struct sketh_adapter *adapter)
{
    struct net_device *netdev = adapter->netdev;

    spin_lock(&adapter->stats_lock);

    netdev->stats.rx_errors = 0;
    netdev->stats.tx_errors = 0;
    netdev->stats.rx_dropped = adapter->rx_drops;
    netdev->stats.tx_dropped = adapter->tx_dropped;

    spin_unlock(&adapter->stats_lock);
}

static void
sketh_get_stats64(struct net_device *netdev,
                  struct rtnl_link_stats64 *stats)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);
    int i;

    sketh_update_stats(adapter);

    stats->rx_packets = netdev->stats.rx_packets;
    stats->rx_bytes = netdev->stats.rx_bytes;
    stats->tx_packets = netdev->stats.tx_packets;
    stats->tx_bytes = netdev->stats.tx_bytes;

    stats->rx_errors = netdev->stats.rx_errors;
    stats->tx_errors = netdev->stats.tx_errors;
    stats->rx_dropped = netdev->stats.rx_dropped;
    stats->tx_dropped = netdev->stats.tx_dropped;
}

static const struct net_device_ops sketh_netdev_ops = {
    .ndo_open            = sketh_open,
    .ndo_stop            = sketh_stop,
    .ndo_start_xmit      = sketh_start_xmit,
    .ndo_set_rx_mode     = sketh_set_rx_mode,
    .ndo_set_mac_address = sketh_set_mac_address,
    .ndo_change_mtu      = sketh_change_mtu,
    .ndo_tx_timeout      = sketh_tx_timeout,
    .ndo_get_stats64     = sketh_get_stats64,
    .ndo_validate_addr   = eth_validate_addr,
    .ndo_set_features    = sketh_set_features,
};

static void
sketh_get_drvinfo(struct net_device *netdev,
                  struct ethtool_drvinfo *drvinfo)
{
    struct sketh_adapter *adapter = netdev_priv(netdev);

    strlcpy(drvinfo->driver, SKETH_DRIVER_NAME, sizeof(drvinfo->driver));
    strlcpy(drvinfo->version, SKETH_DRIVER_VERSION, sizeof(drvinfo->version));
    strlcpy(drvinfo->bus_info, pci_name(adapter->pci_dev),
            sizeof(drvinfo->bus_info));
}

static u32
sketh_get_link(struct net_device *netdev)
{
    return 1;
}

static const struct ethtool_ops sketh_ethtool_ops = {
    .get_drvinfo    = sketh_get_drvinfo,
    .get_link       = sketh_get_link,
};

static void
sketh_reset_task(struct work_struct *work)
{
    struct sketh_adapter *adapter = container_of(work, struct sketh_adapter, reset_task);
    struct net_device *netdev = adapter->netdev;

    if (test_bit(__SKETH_STATE_DOWN, &adapter->state))
        return;

    sketh_info(adapter, "Reset task started\n");
}

static int
sketh_probe(struct pci_dev *pci_dev, const struct pci_device_id *ent)
{
    struct net_device *netdev;
    struct sketh_adapter *adapter;
    int bars, err;
    int i;

    bars = pci_select_bars(pci_dev, IORESOURCE_MEM | IORESOURCE_IO);

    err = pci_enable_device(pci_dev);
    if (err)
        return err;

    err = pci_request_selected_regions(pci_dev, bars, SKETH_DRIVER_NAME);
    if (err) {
        pci_disable_device(pci_dev);
        return err;
    }

    pci_set_master(pci_dev);

    netdev = alloc_etherdev_mqs(sizeof(struct sketh_adapter),
                                SKETH_MAX_NUM_QUEUES,
                                SKETH_MAX_NUM_QUEUES);
    if (!netdev) {
        err = -ENOMEM;
        goto err_free_netdev;
    }

    SET_NETDEV_DEV(netdev, &pci_dev->dev);

    adapter = netdev_priv(netdev);
    adapter->netdev = netdev;
    adapter->pci_dev = pci_dev;
    adapter->msg_enable = (1 << debug) - 1;
    adapter->hw_accel = 0;

    adapter->num_queues = num_queues;

    if (adapter->num_queues > SKETH_MAX_NUM_QUEUES)
        adapter->num_queues = SKETH_MAX_NUM_QUEUES;

    err = sketh_alloc_queues(adapter);
    if (err) {
        sketh_err(adapter, "Unable to allocate queues\n");
        goto err_alloc_queues;
    }

    INIT_WORK(&adapter->reset_task, sketh_reset_task);

    adapter->msix_entries = vzalloc(sizeof(struct msix_entry) * adapter->num_queues);
    if (!adapter->msix_entries) {
        err = -ENOMEM;
        goto err_msix;
    }

    for (i = 0; i < adapter->num_queues; i++) {
        adapter->msix_entries[i].entry = i;
    }

    netdev->netdev_ops = &sketh_netdev_ops;
    netdev->ethtool_ops = &sketh_ethtool_ops;
    netdev->watchdog_timeo = 5 * HZ;
    netdev->mtu = mtu;

    netdev->hw_features = NETIF_F_SG | NETIF_F_HW_CSUM |
                         NETIF_F_NTUPLE | NETIF_F_RXCSUM |
                         NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX |
                         NETIF_F_TSO | NETIF_F_TSO6;

    netdev->features = netdev->hw_features;
    netdev->vlan_features = netdev->hw_features & ~NETIF_F_HW_VLAN_CTAG_RX;

    sketh_configure(adapter);

    err = register_netdev(netdev);
    if (err) {
        sketh_err(adapter, "Cannot register net device\n");
        goto err_register;
    }

    adapter->dev_registered = true;

    err = sketh_register_netfilter(adapter);
    if (err) {
        sketh_warn(adapter, "Cannot register netfilter hooks\n");
    }

    pci_set_drvdata(pci_dev, adapter);

    return 0;

err_register:
    vfree(adapter->msix_entries);
err_msix:
    sketh_free_queues(adapter);
err_alloc_queues:
    free_netdev(netdev);
err_free_netdev:
    pci_release_selected_regions(pci_dev, bars);
    pci_disable_device(pci_dev);
    return err;
}

static void
sketh_remove(struct pci_dev *pci_dev)
{
    struct sketh_adapter *adapter = pci_get_drvdata(pci_dev);
    struct net_device *netdev = adapter->netdev;
    int bars = pci_select_bars(pci_dev, IORESOURCE_MEM | IORESOURCE_IO);

    if (adapter->dev_registered) {
        sketh_unregister_netfilter(adapter);
        unregister_netdev(netdev);
    }

    sketh_free_queues(adapter);

    if (adapter->xdp_info.prog) {
        bpf_prog_put(adapter->xdp_info.prog);
        adapter->xdp_info.prog = NULL;
    }

    if (adapter->msix_entries)
        vfree(adapter->msix_entries);

    free_netdev(netdev);

    pci_release_selected_regions(pci_dev, bars);
    pci_disable_device(pci_dev);
}

static const struct pci_device_id sketh_pci_tbl[] = {
    { PCI_DEVICE(0x1234, 0x5678) },
    { 0, }
};

MODULE_DEVICE_TABLE(pci, sketh_pci_tbl);

static struct pci_driver sketh_driver = {
    .name     = SKETH_DRIVER_NAME,
    .id_table = sketh_pci_tbl,
    .probe    = sketh_probe,
    .remove   = sketh_remove,
};

static int sketh_get_resources(struct sketh_adapter *adapter)
{
    struct platform_device *plat_dev = adapter->plat_dev;
    struct resource *res;
    int ret = 0;

    res = platform_get_resource(plat_dev, IORESOURCE_MEM, 0);
    if (!res) {
        sketh_err(adapter, "No memory resource\n");
        return -ENODEV;
    }

    adapter->hw_addr = devm_ioremap_resource(&plat_dev->dev, res);
    if (IS_ERR(adapter->hw_addr)) {
        sketh_err(adapter, "Failed to ioremap\n");
        return PTR_ERR(adapter->hw_addr);
    }

    spin_lock_init(&adapter->mmio_lock);

    return ret;
}

static void sketh_put_resources(struct sketh_adapter *adapter)
{
    if (adapter->hw_addr)
        devm_iounmap(&adapter->plat_dev->dev, adapter->hw_addr);
}

int sketh_platform_probe(struct platform_device *plat_dev)
{
    struct net_device *netdev;
    struct sketh_adapter *adapter;
    struct resource *res;
    int err;
    int i;

    netdev = alloc_etherdev_mqs(sizeof(struct sketh_adapter),
                                SKETH_MAX_NUM_QUEUES,
                                SKETH_MAX_NUM_QUEUES);
    if (!netdev) {
        return -ENOMEM;
    }

    SET_NETDEV_DEV(netdev, &plat_dev->dev);

    adapter = netdev_priv(netdev);
    adapter->netdev = netdev;
    adapter->plat_dev = plat_dev;
    adapter->msg_enable = (1 << debug) - 1;
    adapter->hw_accel = 0;
    adapter->is_pci = false;

    adapter->num_queues = num_queues;
    if (adapter->num_queues > SKETH_MAX_NUM_QUEUES)
        adapter->num_queues = SKETH_MAX_NUM_QUEUES;

    err = sketh_alloc_queues(adapter);
    if (err) {
        sketh_err(adapter, "Unable to allocate queues\n");
        goto err_alloc_queues;
    }

    err = sketh_get_resources(adapter);
    if (err) {
        sketh_err(adapter, "Failed to get resources\n");
        goto err_get_resources;
    }

    INIT_WORK(&adapter->reset_task, sketh_reset_task);

    netdev->netdev_ops = &sketh_netdev_ops;
    netdev->ethtool_ops = &sketh_ethtool_ops;
    netdev->watchdog_timeo = 5 * HZ;
    netdev->mtu = mtu;

    netdev->hw_features = NETIF_F_SG | NETIF_F_HW_CSUM |
                         NETIF_F_NTUPLE | NETIF_F_RXCSUM |
                         NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX |
                         NETIF_F_TSO | NETIF_F_TSO6;

    netdev->features = netdev->hw_features;
    netdev->vlan_features = netdev->hw_features & ~NETIF_F_HW_VLAN_CTAG_RX;

    sketh_configure(adapter);

    err = register_netdev(netdev);
    if (err) {
        sketh_err(adapter, "Cannot register net device\n");
        goto err_register;
    }

    adapter->dev_registered = true;

    err = sketh_register_netfilter(adapter);
    if (err) {
        sketh_warn(adapter, "Cannot register netfilter hooks\n");
    }

    platform_set_drvdata(plat_dev, adapter);

    return 0;

err_register:
    sketh_put_resources(adapter);
err_get_resources:
    sketh_free_queues(adapter);
err_alloc_queues:
    free_netdev(netdev);
    return err;
}

int sketh_platform_remove(struct platform_device *plat_dev)
{
    struct sketh_adapter *adapter = platform_get_drvdata(plat_dev);
    struct net_device *netdev = adapter->netdev;

    if (adapter->dev_registered) {
        sketh_unregister_netfilter(adapter);
        unregister_netdev(netdev);
    }

    sketh_put_resources(adapter);
    sketh_free_queues(adapter);

    if (adapter->xdp_info.prog) {
        bpf_prog_put(adapter->xdp_info.prog);
        adapter->xdp_info.prog = NULL;
    }

    free_netdev(netdev);

    return 0;
}

int sketh_platform_suspend(struct platform_device *plat_dev, pm_message_t state)
{
    struct sketh_adapter *adapter = platform_get_drvdata(plat_dev);
    struct net_device *netdev = adapter->netdev;

    netif_device_detach(netdev);

    if (netif_running(netdev))
        sketh_stop(netdev);

    return 0;
}

int sketh_platform_resume(struct platform_device *plat_dev)
{
    struct sketh_adapter *adapter = platform_get_drvdata(plat_dev);
    struct net_device *netdev = adapter->netdev;
    int err;

    if (netif_running(netdev)) {
        err = sketh_open(netdev);
        if (err)
            return err;
    }

    netif_device_attach(netdev);

    return 0;
}

static const struct of_device_id sketh_of_match[] = {
    { .compatible = "sketh,ethernet", },
    { }
};
MODULE_DEVICE_TABLE(of, sketh_of_match);

static struct platform_driver sketh_platform_driver = {
    .probe    = sketh_platform_probe,
    .remove   = sketh_platform_remove,
    .suspend  = sketh_platform_suspend,
    .resume   = sketh_platform_resume,
    .driver   = {
        .name  = SKETH_DRIVER_NAME,
        .of_match_table = sketh_of_match,
    },
};

static int __init
sketh_init_module(void)
{
    int ret;

    pr_info("sketh: %s version %s\n", SKETH_DRIVER_NAME, SKETH_DRIVER_VERSION);

    ret = pci_register_driver(&sketh_driver);
    if (ret < 0) {
        pr_err("sketh: Failed to register PCI driver\n");
        return ret;
    }

    ret = platform_driver_register(&sketh_platform_driver);
    if (ret < 0) {
        pr_err("sketh: Failed to register platform driver\n");
        pci_unregister_driver(&sketh_driver);
        return ret;
    }

    return 0;
}

static void __exit
sketh_cleanup_module(void)
{
    platform_driver_unregister(&sketh_platform_driver);
    pci_unregister_driver(&sketh_driver);
}

module_init(sketh_init_module);
module_exit(sketh_cleanup_module);

MODULE_AUTHOR("sketh Driver Author");
MODULE_DESCRIPTION("sketh Ethernet Driver with NAPI, XDP and Netfilter Offload Support");
MODULE_LICENSE("GPL");
MODULE_VERSION(SKETH_DRIVER_VERSION);
