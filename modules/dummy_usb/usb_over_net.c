#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/time64.h>
#include <net/sock.h>

#include "usb_over_net.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("USB over Ethernet");
MODULE_DESCRIPTION("USB over Ethernet transport layer");

static int uoe_create_udp_socket(struct socket **sock, struct sockaddr_in *addr,
                                 const char *ip, int port, int bind_mode)
{
    struct socket *tmp_sock = NULL;
    struct sockaddr_in tmp_addr;
    int ret;

    ret = sock_create(PF_INET, SOCK_DGRAM, IPPROTO_UDP, &tmp_sock);
    if (ret < 0) {
        pr_err("UOE: failed to create UDP socket: %d\n", ret);
        return ret;
    }

    memset(&tmp_addr, 0, sizeof(tmp_addr));
    tmp_addr.sin_family = AF_INET;
    tmp_addr.sin_port = htons(port);

    if (bind_mode) {
        if (ip)
            tmp_addr.sin_addr.s_addr = in_aton(ip);
        else
            tmp_addr.sin_addr.s_addr = INADDR_ANY;

        ret = tmp_sock->ops->bind(tmp_sock, (struct sockaddr *)&tmp_addr, sizeof(tmp_addr));
        if (ret < 0) {
            pr_err("UOE: failed to bind socket to port %d: %d\n", port, ret);
            sock_release(tmp_sock);
            return ret;
        }
    } else {
        tmp_addr.sin_addr.s_addr = in_aton(ip);
    }

    *sock = tmp_sock;
    if (addr)
        *addr = tmp_addr;

    return 0;
}

int uoe_create_client(struct uoe_connection *conn,
                      const char *remote_ip,
                      int data_port,
                      int event_port)
{
    int ret;

    memset(conn, 0, sizeof(*conn));
    spin_lock_init(&conn->lock);

    ret = uoe_create_udp_socket(&conn->sock_data, &conn->remote_data,
                                remote_ip, data_port, 0);
    if (ret < 0)
        goto err_data;

    ret = uoe_create_udp_socket(&conn->sock_event, &conn->remote_event,
                                 remote_ip, event_port, 0);
    if (ret < 0)
        goto err_event;

    conn->connected = 1;
    pr_info("UOE: client connected to %s data=%d event=%d\n",
            remote_ip, data_port, event_port);
    return 0;

err_event:
    sock_release(conn->sock_data);
err_data:
    return ret;
}
EXPORT_SYMBOL(uoe_create_client);

int uoe_create_server(struct uoe_connection *conn,
                      const char *listen_addr,
                      int listen_port,
                      int event_port)
{
    int ret;

    memset(conn, 0, sizeof(*conn));
    spin_lock_init(&conn->lock);

    ret = uoe_create_udp_socket(&conn->sock_data, NULL,
                                listen_addr, listen_port, 1);
    if (ret < 0)
        goto err_data;

    ret = uoe_create_udp_socket(&conn->sock_event, &conn->local_event,
                                listen_addr, event_port, 1);
    if (ret < 0)
        goto err_event;

    conn->connected = 1;
    pr_info("UOE: server listening on %s data=%d event=%d\n",
            listen_addr, listen_port, event_port);
    return 0;

err_event:
    sock_release(conn->sock_data);
err_data:
    return ret;
}
EXPORT_SYMBOL(uoe_create_server);

void uoe_close(struct uoe_connection *conn)
{
    if (conn->sock_data) {
        sock_release(conn->sock_data);
        conn->sock_data = NULL;
    }
    if (conn->sock_event) {
        sock_release(conn->sock_event);
        conn->sock_event = NULL;
    }
    conn->connected = 0;
    pr_info("UOE: connection closed\n");
}
EXPORT_SYMBOL(uoe_close);

static int uoe_send_udp(struct socket *sock, struct sockaddr_in *dest,
                         const void *data, size_t len)
{
    struct msghdr msg;
    struct kvec iov;
    int ret;

    if (!sock || !dest)
        return -EINVAL;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = dest;
    msg.msg_namelen = sizeof(*dest);

    iov.iov_base = (void *)data;
    iov.iov_len = len;

    ret = kernel_sendmsg(sock, &msg, &iov, 1, len);
    if (ret < 0)
        pr_err("UOE: send failed: %d\n", ret);

    return ret;
}

int uoe_send_poll(struct uoe_connection *conn, __u8 type, __u8 ep_addr, __u16 seq)
{
    struct uoe_header hdr;
    unsigned long flags;
    int ret;

    hdr.type = type;
    hdr.ep_addr = ep_addr;
    hdr.seq = htons(seq);
    hdr.length = 0;

    spin_lock_irqsave(&conn->lock, flags);
    ret = uoe_send_udp(conn->sock_data, &conn->remote_data, &hdr, sizeof(hdr));
    spin_unlock_irqrestore(&conn->lock, flags);

    return ret;
}
EXPORT_SYMBOL(uoe_send_poll);

int uoe_send_data(struct uoe_connection *conn, __u8 ep_addr, __u16 seq,
                  const void *data, __u32 len)
{
    struct uoe_packet *pkt;
    unsigned long flags;
    int ret;

    pkt = kzalloc(sizeof(struct uoe_header) + len, GFP_ATOMIC);
    if (!pkt)
        return -ENOMEM;

    pkt->hdr.type = UOE_DATA;
    pkt->hdr.ep_addr = ep_addr;
    pkt->hdr.seq = htons(seq);
    pkt->hdr.length = htonl(len);
    if (data && len > 0)
        memcpy(pkt->data, data, len);

    spin_lock_irqsave(&conn->lock, flags);
    ret = uoe_send_udp(conn->sock_data, &conn->remote_data, pkt, 
                       sizeof(struct uoe_header) + len);
    spin_unlock_irqrestore(&conn->lock, flags);

    kfree(pkt);
    return ret;
}
EXPORT_SYMBOL(uoe_send_data);

int uoe_send_nak(struct uoe_connection *conn, __u8 ep_addr, __u16 seq)
{
    struct uoe_header hdr;
    unsigned long flags;
    int ret;

    hdr.type = UOE_NAK;
    hdr.ep_addr = ep_addr;
    hdr.seq = htons(seq);
    hdr.length = 0;

    spin_lock_irqsave(&conn->lock, flags);
    ret = uoe_send_udp(conn->sock_data, &conn->remote_data, &hdr, sizeof(hdr));
    spin_unlock_irqrestore(&conn->lock, flags);

    return ret;
}
EXPORT_SYMBOL(uoe_send_nak);

int uoe_send_stall(struct uoe_connection *conn, __u8 ep_addr, __u16 seq)
{
    struct uoe_header hdr;
    unsigned long flags;
    int ret;

    hdr.type = UOE_STALL;
    hdr.ep_addr = ep_addr;
    hdr.seq = htons(seq);
    hdr.length = 0;

    spin_lock_irqsave(&conn->lock, flags);
    ret = uoe_send_udp(conn->sock_data, &conn->remote_data, &hdr, sizeof(hdr));
    spin_unlock_irqrestore(&conn->lock, flags);

    return ret;
}
EXPORT_SYMBOL(uoe_send_stall);

int uoe_send_event(struct uoe_connection *conn, __u8 event)
{
    struct uoe_event evt;
    unsigned long flags;
    int ret;

    evt.type = UOE_EVENT;
    evt.event = event;
    evt.reserved = 0;

    spin_lock_irqsave(&conn->lock, flags);
    ret = uoe_send_udp(conn->sock_event, &conn->remote_event, &evt, sizeof(evt));
    spin_unlock_irqrestore(&conn->lock, flags);

    if (ret < 0)
        pr_err("UOE: failed to send event %d: %d\n", event, ret);

    return ret;
}
EXPORT_SYMBOL(uoe_send_event);

int uoe_recv_packet(struct uoe_connection *conn, struct uoe_packet **pkt,
                    struct sockaddr_in *src, int timeout_ms)
{
    struct msghdr msg;
    struct kvec iov[2];
    struct uoe_header hdr;
    struct uoe_packet *tmp_pkt;
    int ret, data_len;

    if (!conn->sock_data || !pkt)
        return -EINVAL;

    memset(&msg, 0, sizeof(msg));
    if (src) {
        msg.msg_name = src;
        msg.msg_namelen = sizeof(*src);
    }

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);

    ret = kernel_recvmsg(conn->sock_data, &msg, iov, 1, sizeof(hdr), 0);
    if (ret < 0)
        return ret;

    if (ret < sizeof(hdr))
        return -EMSGSIZE;

    data_len = ntohl(hdr.length);
    if (data_len > UOE_MAX_PAYLOAD)
        return -EMSGSIZE;

    tmp_pkt = kzalloc(sizeof(struct uoe_header) + data_len, GFP_ATOMIC);
    if (!tmp_pkt)
        return -ENOMEM;

    memcpy(&tmp_pkt->hdr, &hdr, sizeof(hdr));

    if (data_len > 0) {
        iov[1].iov_base = tmp_pkt->data;
        iov[1].iov_len = data_len;

        ret = kernel_recvmsg(conn->sock_data, &msg, iov, 2, data_len, 0);
        if (ret < 0) {
            kfree(tmp_pkt);
            return ret;
        }
    }

    *pkt = tmp_pkt;
    return sizeof(struct uoe_header) + data_len;
}
EXPORT_SYMBOL(uoe_recv_packet);

void uoe_free_packet(struct uoe_packet *pkt)
{
    kfree(pkt);
}
EXPORT_SYMBOL(uoe_free_packet);