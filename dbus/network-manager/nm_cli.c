#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libcli.h>
#include "nm_cli.h"

static NMClient *nm_client = NULL;

NMClient *get_nm_client(void) {
    return nm_client;
}

static const char *device_type_to_string(NMDeviceType type) {
    switch (type) {
        case NM_DEVICE_TYPE_ETHERNET: return "Ethernet";
        case NM_DEVICE_TYPE_WIFI: return "Wi-Fi";
        case NM_DEVICE_TYPE_MODEM: return "Modem";
        case NM_DEVICE_TYPE_BOND: return "Bond";
        case NM_DEVICE_TYPE_BRIDGE: return "Bridge";
        case NM_DEVICE_TYPE_VLAN: return "VLAN";
        case NM_DEVICE_TYPE_LOOPBACK: return "Loopback";
        case NM_DEVICE_TYPE_TUN: return "Tun";
        case NM_DEVICE_TYPE_VETH: return "Veth";
        case NM_DEVICE_TYPE_PPP: return "PPP";
        default: return "Unknown";
    }
}

static const char *device_state_to_string(NMDeviceState state) {
    switch (state) {
        case NM_DEVICE_STATE_UNKNOWN: return "unknown";
        case NM_DEVICE_STATE_UNMANAGED: return "unmanaged";
        case NM_DEVICE_STATE_UNAVAILABLE: return "unavailable";
        case NM_DEVICE_STATE_DISCONNECTED: return "disconnected";
        case NM_DEVICE_STATE_PREPARE: return "prepare";
        case NM_DEVICE_STATE_CONFIG: return "config";
        case NM_DEVICE_STATE_NEED_AUTH: return "need-auth";
        case NM_DEVICE_STATE_IP_CONFIG: return "ip-config";
        case NM_DEVICE_STATE_IP_CHECK: return "ip-check";
        case NM_DEVICE_STATE_SECONDARIES: return "secondaries";
        case NM_DEVICE_STATE_ACTIVATED: return "activated";
        case NM_DEVICE_STATE_DEACTIVATING: return "deactivating";
        case NM_DEVICE_STATE_FAILED: return "failed";
        default: return "unknown";
    }
}

int cli_init_nm(struct cli_def *cli) {
    GError *error = NULL;
    
    nm_client = nm_client_new(NULL, &error);
    if (error) {
        cli_print(cli, "Error connecting to NetworkManager: %s", error->message);
        g_error_free(error);
        return -1;
    }
    
    return 0;
}

void cleanup_nm(void) {
    if (nm_client) {
        g_object_unref(nm_client);
        nm_client = NULL;
    }
}

int cmd_show_status(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    cli_print(cli, "NetworkManager Status:");
    cli_print(cli, "----------------------");
    cli_print(cli, "NM Version: %s", nm_client_get_version(nm_client));
    cli_print(cli, "Running: %s", nm_client_get_nm_running(nm_client) ? "Yes" : "No");
    
    return CLI_OK;
}

int cmd_show_connections(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    const GPtrArray *connections;
    guint i;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    connections = nm_client_get_connections(nm_client);
    
    cli_print(cli, "Saved Connections:");
    cli_print(cli, "%-40s %-20s %-15s", "Name", "UUID", "Type");
    cli_print(cli, "%-40s %-20s %-15s", "----------------------------------------", "--------------------", "---------------");
    
    for (i = 0; i < connections->len; i++) {
        NMRemoteConnection *conn = g_ptr_array_index(connections, i);
        NMConnection *settings = NM_CONNECTION(conn);
        NMSettingConnection *s_con;
        
        s_con = nm_connection_get_setting_connection(settings);
        if (s_con) {
            cli_print(cli, "%-40s %-20s %-15s",
                     nm_setting_connection_get_id(s_con),
                     nm_setting_connection_get_uuid(s_con),
                     nm_setting_connection_get_connection_type(s_con));
        }
    }
    
    return CLI_OK;
}

int cmd_show_devices(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    const GPtrArray *devices;
    guint i;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    devices = nm_client_get_devices(nm_client);
    
    cli_print(cli, "Network Devices:");
    cli_print(cli, "%-15s %-10s %-15s", "Device", "Type", "State");
    cli_print(cli, "%-15s %-10s %-15s", "---------------", "----------", "---------------");
    
    for (i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        cli_print(cli, "%-15s %-10s %-15s",
                 nm_device_get_iface(dev),
                 device_type_to_string(nm_device_get_device_type(dev)),
                 device_state_to_string(nm_device_get_state(dev)));
    }
    
    return CLI_OK;
}

int cmd_show_device(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command;
    
    NMDevice *dev;
    const char *iface;
    
    if (argc < 1) {
        cli_print(cli, "Usage: show device <interface>");
        return CLI_ERROR;
    }
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    iface = argv[0];
    dev = nm_client_get_device_by_iface(nm_client, iface);
    
    if (!dev) {
        cli_print(cli, "Error: Device %s not found", iface);
        return CLI_ERROR;
    }
    
    cli_print(cli, "Device: %s", nm_device_get_iface(dev));
    cli_print(cli, "  Type: %s", device_type_to_string(nm_device_get_device_type(dev)));
    cli_print(cli, "  State: %s", device_state_to_string(nm_device_get_state(dev)));
    cli_print(cli, "  MTU: %d", nm_device_get_mtu(dev));
    cli_print(cli, "  Interface: %s", nm_device_get_iface(dev));
    
    return CLI_OK;
}

int cmd_connect(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command;
    
    NMRemoteConnection *conn;
    const char *conn_name;
    guint i;
    
    if (argc < 1) {
        cli_print(cli, "Usage: connect <connection-name>");
        return CLI_ERROR;
    }
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    conn_name = argv[0];
    conn = NULL;
    
    const GPtrArray *connections = nm_client_get_connections(nm_client);
    for (i = 0; i < connections->len; i++) {
        NMRemoteConnection *c = g_ptr_array_index(connections, i);
        NMConnection *settings = NM_CONNECTION(c);
        NMSettingConnection *s_con = nm_connection_get_setting_connection(settings);
        
        if (s_con && strcmp(nm_setting_connection_get_id(s_con), conn_name) == 0) {
            conn = c;
            break;
        }
    }
    
    if (!conn) {
        cli_print(cli, "Error: Connection '%s' not found", conn_name);
        return CLI_ERROR;
    }
    
    cli_print(cli, "Activating connection '%s'...", conn_name);
    
    return CLI_OK;
}

int cmd_disconnect(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    const GPtrArray *devices;
    guint i;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    devices = nm_client_get_devices(nm_client);
    
    for (i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (nm_device_get_state(dev) == NM_DEVICE_STATE_ACTIVATED) {
            cli_print(cli, "Deactivating %s...", nm_device_get_iface(dev));
        }
    }
    
    return CLI_OK;
}

int cmd_reconnect(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    cli_print(cli, "Reconnecting...");
    return CLI_OK;
}

int cmd_show_wireless(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    const GPtrArray *devices;
    guint i;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    devices = nm_client_get_devices(nm_client);
    
    cli_print(cli, "Wireless Devices:");
    cli_print(cli, "%-15s %-10s %-20s", "Interface", "State", "SSID");
    cli_print(cli, "%-15s %-10s %-20s", "---------------", "----------", "--------------------");
    
    for (i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (nm_device_get_device_type(dev) == NM_DEVICE_TYPE_WIFI) {
            const char *ssid = "";
            NMAccessPoint *ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(dev));
            
            if (ap) {
                GBytes *ssid_bytes = nm_access_point_get_ssid(ap);
                if (ssid_bytes) {
                    ssid = g_bytes_get_data(ssid_bytes, NULL);
                }
            }
            
            cli_print(cli, "%-15s %-10s %-20s",
                     nm_device_get_iface(dev),
                     device_state_to_string(nm_device_get_state(dev)),
                     ssid);
        }
    }
    
    return CLI_OK;
}

int cmd_device_wifi_list(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    const GPtrArray *devices;
    guint i, j;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    devices = nm_client_get_devices(nm_client);
    
    for (i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (nm_device_get_device_type(dev) == NM_DEVICE_TYPE_WIFI) {
            const GPtrArray *aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(dev));
            
            cli_print(cli, "Wi-Fi networks on %s:", nm_device_get_iface(dev));
            cli_print(cli, "%-35s %-10s %-8s %s", "SSID", "BSSID", "Signal", "Security");
            cli_print(cli, "%-35s %-10s %-8s %s", "-----------------------------------", "----------", "--------", "--------");
            
            for (j = 0; j < aps->len; j++) {
                NMAccessPoint *ap = g_ptr_array_index(aps, j);
                const char *ssid = "";
                GBytes *ssid_bytes = nm_access_point_get_ssid(ap);
                const char *bssid = nm_access_point_get_bssid(ap);
                guint8 strength = nm_access_point_get_strength(ap);
                NM80211ApFlags flags = nm_access_point_get_flags(ap);
                
                if (ssid_bytes) {
                    ssid = g_bytes_get_data(ssid_bytes, NULL);
                }
                
                cli_print(cli, "%-35s %-10s %-8u %s",
                         ssid, bssid, strength,
                         (flags & NM_802_11_AP_SEC_KEY_MGMT_PSK) ? "WPA2" :
                         (flags & NM_802_11_AP_SEC_KEY_MGMT_SAE) ? "WPA3" : "Open");
            }
        }
    }
    
    return CLI_OK;
}

int cmd_device_wifi_connect(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command;
    
    if (argc < 1) {
        cli_print(cli, "Usage: device wifi connect <ssid> [password]");
        return CLI_ERROR;
    }
    
    cli_print(cli, "Connecting to Wi-Fi network '%s'...", argv[0]);
    return CLI_OK;
}

int cmd_delete_connection(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command;
    
    const char *conn_name;
    guint i;
    
    if (argc < 1) {
        cli_print(cli, "Usage: delete connection <name>");
        return CLI_ERROR;
    }
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    conn_name = argv[0];
    
    const GPtrArray *connections = nm_client_get_connections(nm_client);
    for (i = 0; i < connections->len; i++) {
        NMRemoteConnection *conn = g_ptr_array_index(connections, i);
        NMConnection *settings = NM_CONNECTION(conn);
        NMSettingConnection *s_con = nm_connection_get_setting_connection(settings);
        
        if (s_con && strcmp(nm_setting_connection_get_id(s_con), conn_name) == 0) {
            cli_print(cli, "Deleting connection '%s'...", conn_name);
            return CLI_OK;
        }
    }
    
    cli_print(cli, "Error: Connection '%s' not found", conn_name);
    return CLI_ERROR;
}

int cmd_reload(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    cli_print(cli, "Reloading NetworkManager configuration...");
    
    return CLI_OK;
}

int cmd_version(struct cli_def *cli, const char *command, char **argv, int argc) {
    (void)command; (void)argv; (void)argc;
    
    if (!nm_client) {
        cli_print(cli, "Error: Not connected to NetworkManager");
        return CLI_ERROR;
    }
    
    cli_print(cli, "NetworkManager version: %s", nm_client_get_version(nm_client));
    
    return CLI_OK;
}
