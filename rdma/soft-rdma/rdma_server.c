/*
 * RDMA Server - 使用 Socket + Verbs API（Soft-RoCE 完全兼容版）
 * 
 * 编译: gcc rdma_server.c -o server -libverbs
 * 运行: ./server [device_name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define PORT 8888
#define BUF_SIZE 1024

/* QP 状态转换函数 */
int qp_to_init(struct ibv_qp *qp, int port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = port_num;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(qp, &attr, flags);
}

int qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, union ibv_gid *remote_gid, int port_num) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = port_num;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = *remote_gid;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = 0;
    
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return ibv_modify_qp(qp, &attr, flags);
}

int qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.timeout = 14;  /* Soft-RoCE 需要设置 timeout */
    
    int flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC | 
                IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT;
    return ibv_modify_qp(qp, &attr, flags);
}

int main(int argc, char *argv[]) {
    const char *dev_name = argc > 1 ? argv[1] : "rxe0";
    
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp = NULL;
    struct ibv_mr *mr = NULL;
    char *buf = NULL;
    
    int listen_sock = -1, client_sock = -1;
    
    printf("[Server] 启动 (设备: %s)\n", dev_name);

    /* 获取设备列表 */
    int num_devices;
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[Server] 没有找到RDMA设备\n");
        return 1;
    }

    /* 查找指定设备 */
    struct ibv_device *target_dev = NULL;
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            target_dev = dev_list[i];
            break;
        }
    }
    
    if (!target_dev) {
        fprintf(stderr, "[Server] 未找到设备 %s\n", dev_name);
        ibv_free_device_list(dev_list);
        return 1;
    }

    printf("[Server] 打开设备: %s\n", ibv_get_device_name(target_dev));

    /* 打开设备 */
    ctx = ibv_open_device(target_dev);
    if (!ctx) {
        perror("ibv_open_device");
        goto cleanup;
    }

    /* 分配 PD */
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        goto cleanup;
    }

    /* 创建 CQ */
    cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq");
        goto cleanup;
    }

    /* 创建 QP */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.cap.max_send_wr = 8;
    qp_attr.cap.max_recv_wr = 8;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.qp_type = IBV_QPT_RC;
    
    qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) {
        perror("ibv_create_qp");
        goto cleanup;
    }
    printf("[Server] QP 创建成功, QPN: 0x%06x\n", qp->qp_num);

    /* QP 转换到 INIT 状态 */
    if (qp_to_init(qp, 1) != 0) {
        fprintf(stderr, "[Server] QP 状态转换到 INIT 失败\n");
        goto cleanup;
    }
    printf("[Server] QP 状态: INIT\n");

    /* 分配并注册内存 */
    buf = calloc(1, BUF_SIZE);
    if (!buf) {
        perror("calloc");
        goto cleanup;
    }

    mr = ibv_reg_mr(pd, buf, BUF_SIZE, 
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        perror("ibv_reg_mr");
        goto cleanup;
    }
    printf("[Server] MR 注册成功, rkey: 0x%08x\n", mr->rkey);

    /* 获取 GID */
    union ibv_gid gid;
    ibv_query_gid(ctx, 1, 0, &gid);

    /* 创建 TCP socket 监听 */
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        goto cleanup;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(listen_sock, 1) < 0) {
        perror("listen");
        goto cleanup;
    }

    printf("[Server] 监听端口 %d...\n", PORT);

    /* 等待客户端连接 */
    client_sock = accept(listen_sock, NULL, NULL);
    if (client_sock < 0) {
        perror("accept");
        goto cleanup;
    }
    printf("[Server] 客户端已连接\n");

    /* 交换信息 */
    struct {
        uint32_t qpn;
        uint32_t rkey;
        uint64_t vaddr;
        uint8_t gid[16];
    } local_info, remote_info;

    local_info.qpn = qp->qp_num;
    local_info.rkey = mr->rkey;
    local_info.vaddr = (uint64_t)(uintptr_t)buf;
    memcpy(local_info.gid, gid.raw, 16);

    /* 先发送，再接收 */
    send(client_sock, &local_info, sizeof(local_info), 0);
    recv(client_sock, &remote_info, sizeof(remote_info), 0);

    printf("[Server] 收到客户端信息: QPN=0x%06x, rkey=0x%08x\n", 
           remote_info.qpn, remote_info.rkey);

    /* QP 转换到 RTR 状态 */
    if (qp_to_rtr(qp, remote_info.qpn, (union ibv_gid *)remote_info.gid, 1) != 0) {
        fprintf(stderr, "[Server] QP 状态转换到 RTR 失败\n");
        goto cleanup;
    }
    printf("[Server] QP 状态: RTR\n");

    /* QP 转换到 RTS 状态 */
    if (qp_to_rts(qp) != 0) {
        fprintf(stderr, "[Server] QP 状态转换到 RTS 失败\n");
        goto cleanup;
    }
    printf("[Server] QP 状态: RTS\n");

    /* 发布接收请求 */
    struct ibv_sge sge;
    struct ibv_recv_wr recv_wr, *bad_wr = NULL;
    
    sge.addr = (uint64_t)(uintptr_t)buf;
    sge.length = BUF_SIZE;
    sge.lkey = mr->lkey;
    
    recv_wr.wr_id = 0;
    recv_wr.next = NULL;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    
    if (ibv_post_recv(qp, &recv_wr, &bad_wr) != 0) {
        perror("ibv_post_recv");
        goto cleanup;
    }

    printf("[Server] 等待接收消息...\n");

    /* 轮询 CQ */
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0) {
        usleep(1000);
    }

    if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
        printf("\n========================================\n");
        printf("[Server] 收到消息: %s\n", buf);
        printf("========================================\n");
    } else {
        printf("[Server] 接收失败: %s\n", ibv_wc_status_str(wc.status));
    }

    sleep(1);

cleanup:
    printf("[Server] 清理资源\n");
    
    if (client_sock >= 0) close(client_sock);
    if (listen_sock >= 0) close(listen_sock);
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    if (buf) free(buf);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    return 0;
}
