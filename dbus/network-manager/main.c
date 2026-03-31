#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <libcli.h>
#include "nm_cli.h"

#define NM_CLI_VERSION "1.0.0"
#define DEFAULT_PORT 8000

static int running = 1;

static int cmd_help(struct cli_def *cli, const char *command, char **argv, int argc);
static int cmd_exit(struct cli_def *cli, const char *command, char **argv, int argc);
static int cmd_quit(struct cli_def *cli, const char *command, char **argv, int argc);

static int create_listen_socket(int port) {
    int sockfd;
    struct sockaddr_in addr;
    int opt = 1;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 5) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

#ifndef SA_RESTART
#define SA_RESTART 0x000004
#endif
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x000001
#endif

static void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    
    sa.sa_handler = sigint_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int cmd_help(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    cli_print(cli, "NetworkManager CLI Commands:");
    cli_print(cli, "  show status                   - Show NetworkManager status");
    cli_print(cli, "  show connections              - Show saved connections");
    cli_print(cli, "  show devices                  - Show network devices");
    cli_print(cli, "  show device <interface>       - Show device details");
    cli_print(cli, "  show wireless                 - Show wireless devices");
    cli_print(cli, "  device wifi list              - List available Wi-Fi networks");
    cli_print(cli, "  device wifi connect <ssid>   - Connect to Wi-Fi network");
    cli_print(cli, "  connect <name>                - Activate a connection");
    cli_print(cli, "  disconnect                    - Disconnect all devices");
    cli_print(cli, "  reconnect                     - Reconnect");
    cli_print(cli, "  delete connection <name>     - Delete a connection");
    cli_print(cli, "  reload                        - Reload NetworkManager");
    cli_print(cli, "  version                       - Show version");
    cli_print(cli, "  help                          - Show this help");
    cli_print(cli, "  exit/quit                     - Exit");
    return CLI_OK;
}

static int cmd_exit(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)cli; (void)command; (void)argv; (void)argc;
    running = 0;
    return CLI_OK;
}

static int cmd_quit(struct cli_def *cli, const char *command, char **argv, int argc) {
    return cmd_exit(cli, command, argv, argc);
}

int main(int argc, char *argv[]) {
    struct cli_def *cli;
    struct cli_command *show_cmd;
    struct cli_command *device_cmd;
    struct cli_command *device_wifi_cmd;
    struct cli_command *c;
    int port = DEFAULT_PORT;
    int sockfd;
    int i;
    
    setup_signals();
    
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -p, --port <port>  Set telnet port (default: %d)\n", DEFAULT_PORT);
            printf("  -h, --help         Show this help\n");
            return 0;
        }
    }
    
    sockfd = create_listen_socket(port);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to create listen socket on port %d\n", port);
        return 1;
    }
    
    cli = cli_init();
    cli_set_banner(cli, "NetworkManager CLI v" NM_CLI_VERSION "\n");
    cli_set_hostname(cli, "nm-cli");
    
    cli_register_command(cli, NULL, "help", cmd_help, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show help");
    cli_register_command(cli, NULL, "exit", cmd_exit, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Exit");
    cli_register_command(cli, NULL, "quit", cmd_quit, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Exit");
    
    if (cli_init_nm(cli) != 0) {
        fprintf(stderr, "Warning: Could not connect to NetworkManager\n");
    }
    
    cli_register_command(cli, NULL, "version", cmd_version, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show version");
    
    show_cmd = cli_register_command(cli, NULL, "show", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show information");
    cli_register_command(cli, show_cmd, "status", cmd_show_status, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show status");
    cli_register_command(cli, show_cmd, "connections", cmd_show_connections, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show connections");
    cli_register_command(cli, show_cmd, "devices", cmd_show_devices, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show devices");
    cli_register_command(cli, show_cmd, "device", cmd_show_device, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show device details");
    cli_register_command(cli, show_cmd, "wireless", cmd_show_wireless, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Show wireless");
    
    cli_register_command(cli, NULL, "connect", cmd_connect, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Connect");
    cli_register_command(cli, NULL, "disconnect", cmd_disconnect, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Disconnect");
    cli_register_command(cli, NULL, "reconnect", cmd_reconnect, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Reconnect");
    cli_register_command(cli, NULL, "reload", cmd_reload, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Reload");
    
    device_cmd = cli_register_command(cli, NULL, "device", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Device commands");
    device_wifi_cmd = cli_register_command(cli, device_cmd, "wifi", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Wi-Fi commands");
    cli_register_command(cli, device_wifi_cmd, "list", cmd_device_wifi_list, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "List Wi-Fi networks");
    cli_register_command(cli, device_wifi_cmd, "connect", cmd_device_wifi_connect, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Connect to Wi-Fi");
    
    c = cli_register_command(cli, NULL, "connection", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Connection commands");
    cli_register_command(cli, c, "delete", cmd_delete_connection, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Delete connection");
    
    printf("NetworkManager CLI v%s\n", NM_CLI_VERSION);
    printf("Starting CLI server on port %d...\n", port);
    printf("Connect with: telnet localhost %d\n", port);
    printf("Press Ctrl+C to stop\n");
    fflush(stdout);
    
    while (running) {
        cli_loop(cli, sockfd);
    }
    
    close(sockfd);
    cleanup_nm();
    cli_done(cli);
    
    return 0;
}
