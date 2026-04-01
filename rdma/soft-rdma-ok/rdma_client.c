#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>

#define BUFFER_SIZE 4096

struct context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
};

static int resolve_and_connect(struct rdma_cm_id *id, struct context *ctx, const char *host, int port) {
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, host, &sin.sin_addr);

    if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&sin, 2000)) {
        perror("rdma_resolve_addr");
        return -1;
    }

    struct rdma_cm_event *event = NULL;
    if (rdma_get_cm_event(id->channel, &event)) {
        perror("rdma_get_cm_event");
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        printf("Unexpected event: %s\n", rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        return -1;
    }
    rdma_ack_cm_event(event);

    if (!id->verbs) {
        fprintf(stderr, "ERROR: No verbs after address resolve\n");
        return -1;
    }

    ctx->ctx = id->verbs;
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) {
        perror("ibv_alloc_pd");
        return -1;
    }

    ctx->cq = ibv_create_cq(ctx->ctx, 128, NULL, NULL, 0);
    if (!ctx->cq) {
        perror("ibv_create_cq");
        ibv_dealloc_pd(ctx->pd);
        return -1;
    }

    ctx->buf = calloc(1, BUFFER_SIZE);
    if (!ctx->buf) {
        ibv_destroy_cq(ctx->cq);
        ibv_dealloc_pd(ctx->pd);
        return -1;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE, 
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        perror("ibv_reg_mr");
        free(ctx->buf);
        ibv_destroy_cq(ctx->cq);
        ibv_dealloc_pd(ctx->pd);
        return -1;
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 32,
            .max_recv_wr = 32,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };

    if (rdma_create_qp(id, ctx->pd, &qp_attr)) {
        perror("rdma_create_qp");
        return -1;
    }
    ctx->qp = id->qp;

    if (rdma_resolve_route(id, 2000)) {
        perror("rdma_resolve_route");
        return -1;
    }

    if (rdma_get_cm_event(id->channel, &event)) {
        perror("rdma_get_cm_event");
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        printf("Unexpected event: %s\n", rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        return -1;
    }
    rdma_ack_cm_event(event);

    struct rdma_conn_param param = {0};
    if (rdma_connect(id, &param)) {
        perror("rdma_connect");
        return -1;
    }

    if (rdma_get_cm_event(id->channel, &event)) {
        perror("rdma_get_cm_event");
        return -1;
    }

    if (event->event == RDMA_CM_EVENT_REJECTED || event->event == RDMA_CM_EVENT_CONNECT_ERROR) {
        printf("Connection rejected or error\n");
        rdma_ack_cm_event(event);
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED && event->event != RDMA_CM_EVENT_CONNECT_RESPONSE) {
        printf("Unexpected event: %s\n", rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        return -1;
    }
    rdma_ack_cm_event(event);

    printf("Connected to server!\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *host = "192.168.30.1";
    int port = 5001;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    struct rdma_event_channel *echannel = rdma_create_event_channel();
    if (!echannel) {
        perror("rdma_create_event_channel");
        return 1;
    }

    struct rdma_cm_id *id = NULL;
    if (rdma_create_id(echannel, &id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        rdma_destroy_event_channel(echannel);
        return 1;
    }

    struct context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        rdma_destroy_id(id);
        rdma_destroy_event_channel(echannel);
        return 1;
    }

    if (resolve_and_connect(id, ctx, host, port)) {
        if (ctx->mr) ibv_dereg_mr(ctx->mr);
        if (ctx->buf) free(ctx->buf);
        if (ctx->qp) rdma_destroy_qp(id);
        if (ctx->cq) ibv_destroy_cq(ctx->cq);
        if (ctx->pd) ibv_dealloc_pd(ctx->pd);
        free(ctx);
        rdma_destroy_id(id);
        rdma_destroy_event_channel(echannel);
        return 1;
    }

    id->context = ctx;

    sleep(1);

    strcpy(ctx->buf, "Hello from client!");
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list = &(struct ibv_sge){
            .addr = (uint64_t)ctx->buf,
            .length = 64,
            .lkey = ctx->mr->lkey,
        },
        .num_sge = 1,
    };
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(id->qp, &wr, &bad_wr)) {
        perror("ibv_post_send");
    }

    struct ibv_recv_wr rwr = {
        .sg_list = &(struct ibv_sge){
            .addr = (uint64_t)ctx->buf,
            .length = BUFFER_SIZE,
            .lkey = ctx->mr->lkey,
        },
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_rwr;
    if (ibv_post_recv(id->qp, &rwr, &bad_rwr)) {
        perror("ibv_post_recv");
    }

    struct ibv_wc wc;
    while (1) {
        int ne = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ne < 0) {
            perror("ibv_poll_cq");
            break;
        }
        if (ne == 0) continue;

        if (wc.status != IBV_WC_SUCCESS) {
            printf("WC error: %s\n", ibv_wc_status_str(wc.status));
            break;
        }

        if (wc.opcode == IBV_WC_SEND) {
            printf("Send completed\n");
        } else if (wc.opcode == IBV_WC_RECV) {
            printf("Received: %s\n", ctx->buf);
            break;
        }
    }

    rdma_disconnect(id);
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->buf) free(ctx->buf);
    if (ctx->qp) rdma_destroy_qp(id);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    free(ctx);
    if (id) rdma_destroy_id(id);
    rdma_destroy_event_channel(echannel);
    printf("Client closed\n");
    return 0;
}
