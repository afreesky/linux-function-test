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

static void *run_server(void *arg) {
    int port = *(int *)arg;
    struct rdma_event_channel *echannel = rdma_create_event_channel();
    if (!echannel) {
        perror("rdma_create_event_channel");
        return NULL;
    }

    struct rdma_cm_id *listener = NULL;
    if (rdma_create_id(echannel, &listener, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        rdma_destroy_event_channel(echannel);
        return NULL;
    }

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {htonl(INADDR_ANY)},
    };

    if (rdma_bind_addr(listener, (struct sockaddr *)&sin)) {
        perror("rdma_bind_addr");
        goto cleanup;
    }

    if (rdma_listen(listener, 128)) {
        perror("rdma_listen");
        goto cleanup;
    }

    printf("Server listening on port %d\n", port);

    struct rdma_cm_id *client = NULL;
    struct rdma_cm_event *event = NULL;

    if (rdma_get_cm_event(echannel, &event)) {
        perror("rdma_get_cm_event");
        goto cleanup;
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        printf("Unexpected event: %s\n", rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        goto cleanup;
    }

    client = event->id;
    rdma_ack_cm_event(event);

    if (!client->verbs) {
        fprintf(stderr, "ERROR: No verbs on client connection\n");
        goto cleanup;
    }

    struct context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        rdma_reject(client, NULL, 0);
        goto cleanup;
    }

    ctx->ctx = client->verbs;
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) {
        perror("ibv_alloc_pd");
        free(ctx);
        rdma_reject(client, NULL, 0);
        goto cleanup;
    }

    ctx->cq = ibv_create_cq(ctx->ctx, 128, NULL, NULL, 0);
    if (!ctx->cq) {
        perror("ibv_create_cq");
        ibv_dealloc_pd(ctx->pd);
        free(ctx);
        rdma_reject(client, NULL, 0);
        goto cleanup;
    }

    ctx->buf = calloc(1, BUFFER_SIZE);
    if (!ctx->buf) {
        ibv_destroy_cq(ctx->cq);
        ibv_dealloc_pd(ctx->pd);
        free(ctx);
        rdma_reject(client, NULL, 0);
        goto cleanup;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE, 
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        perror("ibv_reg_mr");
        free(ctx->buf);
        ibv_destroy_cq(ctx->cq);
        ibv_dealloc_pd(ctx->pd);
        free(ctx);
        rdma_reject(client, NULL, 0);
        goto cleanup;
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

    if (rdma_create_qp(client, ctx->pd, &qp_attr)) {
        perror("rdma_create_qp");
        goto ctx_cleanup;
    }

    ctx->qp = client->qp;
    client->context = ctx;

    struct rdma_conn_param param = {0};
    if (rdma_accept(client, &param)) {
        perror("rdma_accept");
        goto qp_cleanup;
    }

    if (rdma_get_cm_event(echannel, &event)) {
        perror("rdma_get_cm_event");
        goto qp_cleanup;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        printf("Unexpected event: %s\n", rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        goto qp_cleanup;
    }
    rdma_ack_cm_event(event);

    printf("Client connected!\n");

    struct ibv_recv_wr rwr = {
        .sg_list = &(struct ibv_sge){
            .addr = (uint64_t)ctx->buf,
            .length = BUFFER_SIZE,
            .lkey = ctx->mr->lkey,
        },
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_rwr;
    if (ibv_post_recv(client->qp, &rwr, &bad_rwr)) {
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

        if (wc.opcode == IBV_WC_RECV) {
            printf("Received: %s\n", ctx->buf);

            strcpy(ctx->buf, "Hello from server!");
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
            if (ibv_post_send(client->qp, &wr, &bad_wr)) {
                perror("ibv_post_send");
            }

            rwr.wr_id = wc.wr_id;
            if (ibv_post_recv(client->qp, &rwr, &bad_rwr)) {
                perror("ibv_post_recv");
            }
        }
    }

qp_cleanup:
    rdma_disconnect(client);
    if (ctx) {
        if (ctx->mr) ibv_dereg_mr(ctx->mr);
        if (ctx->buf) free(ctx->buf);
        if (ctx->cq) ibv_destroy_cq(ctx->cq);
        if (ctx->pd) ibv_dealloc_pd(ctx->pd);
        free(ctx);
    }
    goto cleanup;

ctx_cleanup:
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->buf) free(ctx->buf);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    free(ctx);

cleanup:
    if (client) {
        rdma_disconnect(client);
    }
    if (listener) rdma_destroy_id(listener);
    rdma_destroy_event_channel(echannel);
    printf("Server closed\n");
    return NULL;
}

int main(int argc, char **argv) {
    int port = 5001;
    if (argc > 1) port = atoi(argv[1]);
    run_server(&port);
    return 0;
}
