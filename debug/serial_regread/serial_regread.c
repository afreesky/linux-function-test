#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

#define MAX_LINE 256
#define TIMEOUT_SEC 10          // 登录总超时（秒）
#define PROMPT_TIMEOUT_MS 2000  // 等待提示符超时（毫秒）
#define READ_TIMEOUT_MS 2000    // 读取一行超时（毫秒）

/* 寄存器信息结构体 */
typedef struct {
    const char *name;   // 寄存器宏名
    unsigned long addr; // 物理地址
} RegInfo;

/* 寄存器列表 —— 在此添加新寄存器即可扩展 */
RegInfo regs[] = {
    {"MP_PHY_MODE",   0xd055010},
    {"PHY_LINK_CFG_LN_1", 0xd055014},
    {"MP_PHY_REFCLK_DETECTED",   0xd055018},
    {"MP_PHY_REFCLK_CFG_0",   0xd05501c},
    {"MP_PHY_REFCLK_CFG_1",   0xd055020},
    {"MP_PHY_CFG",   0xd055024},
    {"POWER_MODES0",   0xd055028},
    {"PMA_XCVR_PLLCLK_EN0",   0xd05502c},
    {"DATA_WIDTH_STANDARD_MODE0",   0xd055030},
    {"CLKS_OUTPUT_10",   0xd055034},
    {"CLKS_OUTPUT_20",   0xd055038},
    {"POWER_MODES1",   0xd05503c},
    {"PMA_XCVR_PLLCLK_EN1",   0xd055040},
    {"DATA_WIDTH_STANDARD_MODE1",   0xd055044},
    {"CLKS_OUTPUT_11",   0xd055048},
    {"CLKS_OUTPUT_21",   0xd05504c},
    {"ETH_0_LOOPBACKS",   0xd055050},
    {"ETH_0_SPEEDS",   0xd055054},
    {"PMA_RX_SIGNAL_DETECT_LN_1",   0xd055058},
    {"PMA_RX_RD_LN_1",   0xd05505c},
    {"HS_IF_PLL_CFG_COMMON",0xd055084},
    {"HS_IF_PLL_CFG_DSKEWCAL",0xd055088},
    {"HS_IF_PLL_CFG_DSKEWCAL_VALUE",0xd05508c},
    {"HS_IF_PLL_CFG_FRAC",0xd055090},
    {"SW_RST_N_PHYS",0xd0550a0},
    {"SW_RST_N_AMBA",0xd0550a4},
    {"SW_RST_N_ETH_RX",0xd0550ac},
    {"SW_RST_N_ETH_TX", 0xd0550b0},
    {"SW_RST_N_PCIE", 0xd0550b4},
    {"CLK_EN_ETH_RX", 0xd0550c4},
    {"CLK_EN_ETH_TX", 0xd0550c8},
    {"ETH_RGMII_TX_CLK_SOURCE_CFG", 0xd0550cc},
    {"HS_IF_CD_CLK_GATE_EN", 0xd0551ec},
    {"HS_IF_CD_CLK_GATE_STATUS", 0xd0551f0},
    {"MP_PHY_PHY_EN_REFCLK", 0xd0551f4},
    {"MP_PHY_STATUS_DEBUG0_REG", 0xd0551f8},
    {"MP_PHY_STATUS_DEBUG1_REG", 0xd0551fc},
    /* 可继续添加 */
};
int reg_count = sizeof(regs) / sizeof(regs[0]);

/* 将整数波特率转换为 termios 宏 */
int baudrate_to_flag(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B115200; // 默认
    }
}

/* 打开并配置串口 */
int serial_open(const char *device, int baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open serial port");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    speed_t speed = baudrate_to_flag(baudrate);
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8位数据位
    tty.c_iflag &= ~IGNBRK;                          // 禁用 break 处理
    tty.c_lflag = 0;                                 // 无回声、无规范模式
    tty.c_oflag = 0;                                 // 无输出处理
    tty.c_cc[VMIN]  = 0;                             // 非阻塞读
    tty.c_cc[VTIME] = 5;                             // 0.5 秒超时

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);          // 关闭软件流控
    tty.c_cflag |= (CLOCAL | CREAD);                  // 忽略调制解调器控制、使能接收
    tty.c_cflag &= ~(PARENB | PARODD);                 // 无校验
    tty.c_cflag &= ~CSTOPB;                            // 1位停止位
    tty.c_cflag &= ~CRTSCTS;                           // 关闭硬件流控

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

/* 向串口写入数据 */
int serial_write(int fd, const char *buf, int len) {
    return write(fd, buf, len);
}

/* 读取一行（以 \n 或 \r 结束），超时返回 -1，成功返回行长度（不含换行） */
int serial_read_line(int fd, char *buf, int maxlen, int timeout_ms) {
    fd_set set;
    struct timeval tv;
    int pos = 0;
    char c;

    while (pos < maxlen - 1) {
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int rv = select(fd + 1, &set, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else if (rv == 0) {
            break;  // 超时
        }

        if (read(fd, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (pos > 0) break;   // 行结束
                else continue;         // 跳过开头的空行
            }
            buf[pos++] = c;
        } else {
            break;
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* 清空串口输入缓冲区 */
void serial_flush_input(int fd) {
    tcflush(fd, TCIFLUSH);
}

/* 等待 shell 提示符出现（包含 '#' 或 '$'） */
int wait_for_prompt(int fd, int timeout_ms) {
    char line[MAX_LINE];
    int start_time = time(NULL);

    while (time(NULL) - start_time < timeout_ms / 1000) {
        if (serial_read_line(fd, line, sizeof(line), 1000) > 0) {
            if (strchr(line, '#') || strchr(line, '$')) {
                return 0;
            }
        }
    }
    return -1;
}

/* 登录过程：发送用户名，等待 shell 提示符 */
int login(int fd, const char *username, int timeout_sec) {
    char line[MAX_LINE];
    int login_prompt_found = 0;
    int start_time = time(NULL);

    serial_flush_input(fd);
    serial_write(fd, "\r\n", 2);   // 唤醒可能存在的 getty
    usleep(100000);

    while (time(NULL) - start_time < timeout_sec) {
        if (serial_read_line(fd, line, sizeof(line), 1000) > 0) {
            // 检查是否是 shell 提示符
            if (strchr(line, '#') || strchr(line, '$')) {
                return 0;  // 已经登录成功
            }
            // 检查是否是 login: 提示
            if (strstr(line, "login:") != NULL) {
                login_prompt_found = 1;
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "%s\r\n", username);
                serial_write(fd, cmd, strlen(cmd));
            }
        }

        // 如果长时间没看到 login 提示，主动发送用户名尝试一次
        if (!login_prompt_found && (time(NULL) - start_time > 2)) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "%s\r\n", username);
            serial_write(fd, cmd, strlen(cmd));
            login_prompt_found = 1;  // 避免重复发送
        }
    }

    return -1; // 登录失败
}

/* 解析 devmem 命令的输出，提取十六进制数值 */
unsigned long parse_devmem_output(const char *line) {
    unsigned long val;
    // 尝试直接解析十六进制数
    if (sscanf(line, "%lx", &val) == 1)
        return val;
    // 尝试从包含 "0x" 的位置解析
    const char *p = strstr(line, "0x");
    if (p && sscanf(p, "%lx", &val) == 1)
        return val;
    return 0; // 解析失败
}

/* 读取所有寄存器值 */
int read_registers(int fd, RegInfo *regs, int count, unsigned long *values) {
    char cmd[64];
    char line[MAX_LINE];

    for (int i = 0; i < count; i++) {
        // 发送 devmem 命令
        snprintf(cmd, sizeof(cmd), "devmem 0x%lx\r\n", regs[i].addr);
        serial_write(fd, cmd, strlen(cmd));

	serial_read_line(fd, line, sizeof(line), READ_TIMEOUT_MS);

        // 读取一行输出（寄存器的值）
        if (serial_read_line(fd, line, sizeof(line), READ_TIMEOUT_MS) > 0) {
	    // printf("Read line for %s: '%s'\n", regs[i].name, line); // 调试输出
            values[i] = parse_devmem_output(line);
        } else {
            values[i] = 0;  // 读取失败
        }

        // 等待 shell 提示符，确保下一条命令时 shell 已就绪
        if (wait_for_prompt(fd, PROMPT_TIMEOUT_MS) < 0) {
            fprintf(stderr, "Warning: prompt not found after reading register %s\n", regs[i].name);
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/ttyUSB1";
    int baudrate = 115200;
    const char *username = "root";

    if (argc >= 2) device = argv[1];
    if (argc >= 3) baudrate = atoi(argv[2]);
    if (argc >= 4) username = argv[3];

    printf("Opening %s at %d baud...\n", device, baudrate);
    int fd = serial_open(device, baudrate);
    if (fd < 0) return 1;

//     printf("Logging in as '%s'...\n", username);
//     if (login(fd, username, TIMEOUT_SEC) < 0) {
//         fprintf(stderr, "Login failed (timeout or no prompt)\n");
//         close(fd);
//         return 1;
//     }
//     printf("Login successful.\n");

    unsigned long values[reg_count];
    if (read_registers(fd, regs, reg_count, values) < 0) {
        fprintf(stderr, "Failed to read registers\n");
        close(fd);
        return 1;
    }

    // 显示结果
    printf("\nRegister values:\n");
    printf("%-40s %-12s %s\n", "Name", "Address", "Value");
    for (int i = 0; i < reg_count; i++) {
        printf("%-40s 0x%08lx   0x%08lx\n", regs[i].name, regs[i].addr, values[i]);
    }

    close(fd);
    return 0;
}