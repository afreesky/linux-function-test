#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/if_packet.h>

#define ETH_P_PAUSE 0x8808
#define ETH_ALEN 6
#define PFC_OPCODE 0x0101

struct pfc_frame {
    unsigned char dst_mac[ETH_ALEN];
    unsigned char src_mac[ETH_ALEN];
    unsigned short ether_type;
    unsigned short opcode;
    unsigned short priority_enable;
    unsigned short pause_time[8];
} __attribute__((packed));

void usage(const char *prog) {
    printf("Usage: %s <interface> [options]\n", prog);
    printf("Options:\n");
    printf("  -t <time>     Pause time value (0-65535, default 65535)\n");
    printf("  -e <vector>  Priority enable vector (0-255, default 255=all enabled)\n");
    printf("  -c <count>   Number of frames to send (default 1)\n");
    printf("  -i <ms>      Interval between frames in ms (default 1000)\n");
    printf("  -h           Show this help\n");
    printf("\nExample: %s eth0 -t 1000 -e 0xFF -c 10 -i 100\n", prog);
}

int get_interface_mac(const char *ifname, unsigned char *mac) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl SIOCGIFHWADDR");
        close(sock);
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    close(sock);
    return 0;
}

int create_pfc_frame(struct pfc_frame *frame, const unsigned char *src_mac, 
                     unsigned short priority_enable, unsigned short pause_time) {
    frame->dst_mac[0] = 0x01;
    frame->dst_mac[1] = 0x80;
    frame->dst_mac[2] = 0xC2;
    frame->dst_mac[3] = 0x00;
    frame->dst_mac[4] = 0x00;
    frame->dst_mac[5] = 0x01;

    memcpy(frame->src_mac, src_mac, ETH_ALEN);

    frame->ether_type = htons(ETH_P_PAUSE);
    frame->opcode = htons(PFC_OPCODE);
    frame->priority_enable = htons(priority_enable);

    for (int i = 0; i < 8; i++) {
        frame->pause_time[i] = htons(pause_time);
    }

    return sizeof(struct pfc_frame);
}

int main(int argc, char *argv[]) {
    char *ifname = NULL;
    unsigned short pause_time = 0xFFFF;
    unsigned char priority_enable = 0xFF;
    int count = 1;
    int interval_ms = 1000;
    int opt;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    ifname = argv[1];

    if (argc > 2) {
        optind = 1;
        while ((opt = getopt(argc - 1, &argv[1], "t:e:c:i:h")) != -1) {
            switch (opt) {
            case 't':
                pause_time = (unsigned short)strtoul(optarg, NULL, 0);
                break;
            case 'e':
                priority_enable = (unsigned char)strtoul(optarg, NULL, 0);
                break;
            case 'c':
                count = atoi(optarg);
                break;
            case 'i':
                interval_ms = atoi(optarg);
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
            }
        }
    }

    unsigned char src_mac[ETH_ALEN];
    if (get_interface_mac(ifname, src_mac) < 0) {
        fprintf(stderr, "Failed to get MAC address for interface %s\n", ifname);
        fprintf(stderr, "Make sure the interface exists and you have root privileges\n");
        return 1;
    }

    printf("Interface: %s\n", ifname);
    printf("Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
    printf("Priority Enable: 0x%02x\n", priority_enable);
    printf("Pause Time: %u\n", pause_time);
    printf("Sending %d frame(s) with %d ms interval\n", count, interval_ms);

    int sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("socket(PF_PACKET)");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl SIOCGIFINDEX");
        close(sock);
        return 1;
    }

    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex = ifr.ifr_ifindex;

    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    int one = 1;
    if (setsockopt(sock, SOL_PACKET, PACKET_TX_RING, &one, sizeof(one)) < 0) {
        perror("setsockopt PACKET_TX_RING");
    }

    struct pfc_frame frame;
    int frame_len = create_pfc_frame(&frame, src_mac, priority_enable, pause_time);

    int pad_len = 60 - frame_len;
    if (pad_len < 0) pad_len = 0;

    unsigned char send_buf[ETH_ALEN * 2 + 2 + 2 + 2 + 16 + 60];
    memcpy(send_buf, &frame, frame_len);
    memset(send_buf + frame_len, 0, pad_len);
    int total_len = frame_len + pad_len;

    printf("\nPFC Pause Frame:\n");
    printf("  Dest MAC:    %02x:%02x:%02x:%02x:%02x:%02x\n",
           frame.dst_mac[0], frame.dst_mac[1], frame.dst_mac[2],
           frame.dst_mac[3], frame.dst_mac[4], frame.dst_mac[5]);
    printf("  Src MAC:     %02x:%02x:%02x:%02x:%02x:%02x\n",
           frame.src_mac[0], frame.src_mac[1], frame.src_mac[2],
           frame.src_mac[3], frame.src_mac[4], frame.src_mac[5]);
    printf("  EtherType:   0x%04x\n", ntohs(frame.ether_type));
    printf("  Opcode:      0x%04x\n", ntohs(frame.opcode));
    printf("  Prio Enable: 0x%04x\n", ntohs(frame.priority_enable));
    printf("  Pause Times: ");
    for (int i = 0; i < 8; i++) {
        printf("%u ", ntohs(frame.pause_time[i]));
    }
    printf("\n");
    printf("  Frame Size:  %d bytes (with %d padding to minimum)\n", total_len, pad_len);

    for (int i = 0; i < count; i++) {
        ssize_t sent = sendto(sock, send_buf, total_len, 0, NULL, 0);
        if (sent < 0) {
            perror("sendto");
            close(sock);
            return 1;
        }
        printf("Sent frame %d/%d (%zd bytes)\n", i + 1, count, sent);

        if (i < count - 1 && interval_ms > 0) {
            usleep(interval_ms * 1000);
        }
    }

    close(sock);
    printf("Done\n");
    return 0;
}
