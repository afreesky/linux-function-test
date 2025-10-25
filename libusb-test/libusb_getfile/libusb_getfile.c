#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>

#include <unistd.h>

#include <fcntl.h> /* Definition of AT_* constants */
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#define DEVICE_VID 0x0123
#define DEVICE_PID 0x0456

#define ENDPOINT_ADDRESS 1

void dump_data(unsigned char *ptr, int len)
{
    int i;
    printf("data:\n");
    for (i = 0; i < len; i++)
    {
        printf("%02x ", ptr[i]);
    }

    printf("\n");
}

pid_t libusb_read_bulk(struct libusb_device_handle *handle)
{
    unsigned char data[1024];            // 数据缓冲区
    int transferred;                     // 传输的数据量
    int r;                               // 标准返回值

    memset(data, 0, 1024);

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return -1;
    }

    if (pid == 0)
    {
        printf("new child process!\n");

        // 从bulk endpoint读取数据（IN endpoint）

        // 清空缓冲区以接收数据
        memset(data, 0, sizeof(data));

        // 替换ENDPOINT_ADDRESS为你的endpoint地址，例如0x82表示地址2的IN endpoint
        r = libusb_bulk_transfer(handle, ENDPOINT_ADDRESS | LIBUSB_ENDPOINT_IN, data, sizeof(data), &transferred, 100000);
        if (r == 0 && transferred > 0)
        {
            printf("收到 %d 字节的数据\n", transferred);
            dump_data(data, transferred);
        }
        else
        {
            fprintf(stderr, "wait receive data timeout\n");
        }
    }
    else
    {
        printf("start child process :%d\n", pid);
        // return back;
    }

    return pid;
}

int main(int argc, char *argv[])
{
    libusb_device **devs;                // 指向设备列表的指针
    libusb_context *ctx = NULL;          // 创建libusb会话
    int r;                               // 标准返回值
    ssize_t cnt;                         // 字节计数器
    struct libusb_device_handle *handle; // 设备句柄
    unsigned char data[1024];            // 数据缓冲区
    int transferred;                     // 传输的数据量
    int len = 0;
    char *fpath = 0;

    memset(data, 0, 1024);

    if (argc < 2)
    {
        printf("usage: %s <file>   --- get  file from usb device\n", argv[0]);
        return 0;
    }
    else
    {
        fpath = argv[1];
    }

    // 初始化libusb库
    r = libusb_init(&ctx);
    if (r < 0)
    {
        fprintf(stderr, "初始化错误 %d\n", r);
        return 1;
    }
    libusb_set_option(ctx, 3); // 设置调试级别

    // 获取设备列表，此处省略具体设备查找和选择逻辑，实际应用中需根据VID和PID筛选特定设备
    cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0)
    {
        fprintf(stderr, "获取设备失败\n");
        return 1;
    }
    printf("设备数量: %ld\n", cnt);

    // 打开设备，实际应用中需要指定VID和PID或使用其他方式找到特定设备
    handle = libusb_open_device_with_vid_pid(ctx, DEVICE_VID, DEVICE_PID);
    if (handle == NULL)
    {
        fprintf(stderr, "无法打开设备\n");
        return 1;
    }

    // 声明bulk endpoint的传输，此处需要根据实际情况配置endpoint地址和类型（例如0x02为IN，0x81为OUT）
    if (libusb_claim_interface(handle, 0) < 0)
    { // 声明接口，通常为0
        fprintf(stderr, "无法声明接口\n");
        return 1;
    }

    int save_fd = open(fpath, O_CREAT|O_TRUNC|O_RDWR);
    if (save_fd == -1) {
        perror("create");
	    return -1;
    }

    len = strlen(fpath);
    // 向bulk endpoint写入数据（OUT endpoint）, 发送请求下载文件名
    r = libusb_bulk_transfer(handle, ENDPOINT_ADDRESS | LIBUSB_ENDPOINT_OUT, (unsigned char*)fpath, len, &transferred, 5000);
    if (r == 0 && transferred == len)
    {
        ; // printf("数据发送成功: r:%d, len:%d\n", r, len);
    }
    else
    {
        fprintf(stderr, "数据发送失败, r:%d len:%d\n", r, len);
        return -1;
    }

    // 等待接收请求文件的下载数据
    while (1)
    {
        memset(data, 0, sizeof(data)); // 清空缓冲区以接收数据
        
        r = libusb_bulk_transfer(handle, ENDPOINT_ADDRESS | LIBUSB_ENDPOINT_IN, data, sizeof(data), &transferred, 5000);
        if (r == 0 && transferred > 0) {
            printf("收到 %d 字节的数据\n", transferred);
            write(save_fd, data, transferred);
        } else {
            fprintf(stderr, "数据接收失败\n");
        }

        if (transferred < 512)
            break;
    }

    libusb_release_interface(handle, 0); // 释放接口资源
    libusb_close(handle);                // 关闭设备句柄
    libusb_free_device_list(devs, 1);    // 释放设备列表，带参数1表示释放内部的引用计数器（如果有的话）
}
