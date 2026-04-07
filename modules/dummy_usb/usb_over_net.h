#ifndef USB_OVER_NET_H
#define USB_OVER_NET_H

#include <linux/types.h>
#include <linux/skbuff.h>

#define UOE_DATA_PORT    5555
#define UOE_EVENT_PORT   5556
#define UOE_MAX_PAYLOAD  65536
#define UOE_TIMEOUT_MS  5

#define UOE_POLL_IN      0x01
#define UOE_POLL_OUT     0x02
#define UOE_SETUP        0x03
#define UOE_DATA         0x04
#define UOE_NAK          0x05
#define UOE_STALL        0x06

#define UOE_EVENT        0x10
#define UOE_EVENT_CONNECT      0x01
#define UOE_EVENT_DISCONNECT   0x02
#define UOE_EVENT_RESET_DONE   0x03
#define UOE_EVENT_SUSPEND      0x04
#define UOE_EVENT_RESUME       0x05

#pragma pack(push, 1)
struct uoe_header {
    __u8  type;
    __u8  ep_addr;
    __u16 seq;
    __u32 length;
};

struct uoe_event {
    __u8  type;
    __u8  event;
    __u16 reserved;
};

struct uoe_packet {
    struct uoe_header hdr;
    __u8  data[0];
};
#pragma pack(pop)

struct uoe_connection {
    struct socket  *sock_data;
    struct socket  *sock_event;
    struct sockaddr_in remote_data;
    struct sockaddr_in remote_event;
    struct sockaddr_in local_event;
    spinlock_t     lock;
    int            connected;
};

int uoe_create_client(struct uoe_connection *conn, 
                      const char *remote_ip, 
                      int data_port, 
                      int event_port);

int uoe_create_server(struct uoe_connection *conn,
                     const char *listen_addr,
                     int listen_port,
                     int event_port);

void uoe_close(struct uoe_connection *conn);

int uoe_send_poll(struct uoe_connection *conn, __u8 type, __u8 ep_addr, __u16 seq);
int uoe_send_data(struct uoe_connection *conn, __u8 ep_addr, __u16 seq, 
                  const void *data, __u32 len);
int uoe_send_nak(struct uoe_connection *conn, __u8 ep_addr, __u16 seq);
int uoe_send_stall(struct uoe_connection *conn, __u8 ep_addr, __u16 seq);

int uoe_send_event(struct uoe_connection *conn, __u8 event);
int uoe_recv_packet(struct uoe_connection *conn, struct uoe_packet **pkt, 
                    struct sockaddr_in *src, int timeout_ms);

void uoe_free_packet(struct uoe_packet *pkt);

#endif