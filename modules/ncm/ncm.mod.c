#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x2c635209, "module_layout" },
	{ 0x30a93ed, "kmalloc_caches" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xe22b65d5, "ethtool_op_get_ts_info" },
	{ 0xfef817f6, "usb_altnum_to_altsetting" },
	{ 0xa38bc394, "usbnet_resume" },
	{ 0x54b1fac6, "__ubsan_handle_load_invalid_value" },
	{ 0x3c96fa2f, "hrtimer_active" },
	{ 0xa07d1b3c, "tasklet_setup" },
	{ 0xe98260e7, "usbnet_probe" },
	{ 0x585391ae, "usbnet_link_change" },
	{ 0x4a666d56, "hrtimer_cancel" },
	{ 0x5f204020, "usbnet_disconnect" },
	{ 0xc3690fc, "_raw_spin_lock_bh" },
	{ 0xb5c014d7, "usbnet_get_link_ksettings_internal" },
	{ 0x73aefea2, "__dev_kfree_skb_any" },
	{ 0x6e08d0dd, "usbnet_stop" },
	{ 0x4cc25947, "usbnet_update_max_qlen" },
	{ 0xe7a578c, "param_ops_bool" },
	{ 0x4629334c, "__preempt_count" },
	{ 0x7a2af7b4, "cpu_number" },
	{ 0x3c3ff9fd, "sprintf" },
	{ 0x71038ac7, "pv_ops" },
	{ 0x946ae2a4, "__dynamic_netdev_dbg" },
	{ 0xbcd5486f, "__netdev_alloc_skb" },
	{ 0xd0881559, "dev_get_tstats64" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x44ba780a, "netif_schedule_queue" },
	{ 0x5c3c7387, "kstrtoull" },
	{ 0xcc84d264, "usbnet_nway_reset" },
	{ 0xd955afa6, "hrtimer_start_range_ns" },
	{ 0x99231916, "_dev_warn" },
	{ 0xfb578fc5, "memset" },
	{ 0x124bad4d, "kstrtobool" },
	{ 0xe180ce57, "usb_deregister" },
	{ 0x3c3fce39, "__local_bh_enable_ip" },
	{ 0x3398f7af, "usb_set_interface" },
	{ 0x9d2ab8ac, "__tasklet_schedule" },
	{ 0x9cfbb611, "usb_driver_claim_interface" },
	{ 0xf361707d, "usbnet_get_drvinfo" },
	{ 0x6c229b8c, "usbnet_start_xmit" },
	{ 0x7af6e01c, "usbnet_suspend" },
	{ 0x850f2c91, "usbnet_get_link" },
	{ 0xf0311d7, "_dev_err" },
	{ 0xea3c74e, "tasklet_kill" },
	{ 0x87a21cb3, "__ubsan_handle_out_of_bounds" },
	{ 0x749a5a6e, "usbnet_read_cmd" },
	{ 0xec2dcbc3, "_dev_info" },
	{ 0x10473ee, "__alloc_skb" },
	{ 0xe46021ca, "_raw_spin_unlock_bh" },
	{ 0xc89dda51, "usbnet_tx_timeout" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0x6170cf94, "cdc_parse_cdc_header" },
	{ 0x4f008fa7, "usbnet_get_ethernet_addr" },
	{ 0x688215df, "usbnet_skb_return" },
	{ 0x92997ed8, "_printk" },
	{ 0xf621d045, "usb_driver_release_interface" },
	{ 0x5316c79f, "usbnet_open" },
	{ 0x4f17d836, "usbnet_get_msglevel" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xaf88e69b, "kmem_cache_alloc_trace" },
	{ 0xba8fbd64, "_raw_spin_lock" },
	{ 0xff5c4137, "__dynamic_dev_dbg" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0xaae7737f, "usbnet_unlink_rx_urbs" },
	{ 0xaf0f7de1, "eth_validate_addr" },
	{ 0x37a0cba, "kfree" },
	{ 0x69acdf38, "memcpy" },
	{ 0x48879f20, "hrtimer_init" },
	{ 0x942410fb, "usb_register_driver" },
	{ 0x2aff387f, "usb_ifnum_to_if" },
	{ 0x46daee22, "usbnet_cdc_update_filter" },
	{ 0x28d384c6, "skb_put" },
	{ 0x86fef34c, "eth_mac_addr" },
	{ 0x133c0dc8, "usbnet_manage_power" },
	{ 0x4a5635b1, "usbnet_write_cmd" },
	{ 0xd187a64f, "usbnet_set_msglevel" },
	{ 0x579fdb7a, "usbnet_set_rx_mode" },
};

MODULE_INFO(depends, "usbnet,cdc_ether");

MODULE_ALIAS("usb:v0BDBp*d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v1BC7p0036d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v413Cp81BBd*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v413Cp81BCd*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v413Cp*d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v0930p*d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v12D1p*d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v1519p0443d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v1546p1010d*dc*dsc*dp*ic02isc0Dip00in*");
MODULE_ALIAS("usb:v*p*d*dc*dsc*dp*ic02isc0Dip00in*");

MODULE_INFO(srcversion, "58B50A9C216C033FFA9A344");
