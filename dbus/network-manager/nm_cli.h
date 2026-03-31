#ifndef NM_CLI_H
#define NM_CLI_H

#include <glib.h>
#include <NetworkManager.h>

int cli_init_nm(struct cli_def *cli);
void cleanup_nm(void);
NMClient *get_nm_client(void);

int cmd_show_status(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_show_connections(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_show_devices(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_show_device(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_connect(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_disconnect(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_reconnect(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_show_wireless(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_device_wifi_list(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_device_wifi_connect(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_delete_connection(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_reload(struct cli_def *cli, const char *command, char **argv, int argc);
int cmd_version(struct cli_def *cli, const char *command, char **argv, int argc);

#endif
